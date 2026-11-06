#include "trait_runtime_native.h"
#include "trait_text_native.h"
#include "../platform/windows/code_memory.h"
#include "../platform/windows/process_memory.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <intrin.h>
#include "../../third_party/minhook/include/MinHook.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <cstdio>
#include <cstdarg>
#include <cwchar>
#include <limits>
#include <new>
#include <mutex>
#include <span>
#include <string>
#include <utility>

namespace s8::game {

// One immutable publication contains every text view and compatibility lineage.
// Game callers retain returned pointers; published storage has process lifetime.
// Failed unpublished allocations are released before returning.
struct NativeTraitTexts {
    struct Entry {
        const wchar_t* name = nullptr;
        const wchar_t* description = nullptr;
        const wchar_t* format_description = nullptr;
        int clone_source = -1;
        int background_trait = -1;
        bool present = false;
    };

    std::array<Entry, domain::TRAIT_RUNTIME_COUNT> entries{};
    const NativeTraitTexts* previous = nullptr;
    std::size_t allocated_bytes = 0;
};

namespace {

constexpr std::uint64_t CALLBACK_CLOSED = std::uint64_t{1} << 63;
constexpr std::uint64_t CALLBACK_COUNT = CALLBACK_CLOSED - 1;
constexpr std::size_t TRAIT_ID_OFFSET = 8;
constexpr std::size_t EFFECT_OFFSET = 10;
constexpr std::size_t METADATA_OFFSET = 0x2e;
constexpr std::size_t METADATA_SIZE = 0x12;
constexpr std::uintptr_t MAX_MODULE_OFFSET = 0x2e98bd0;
constexpr std::uint32_t NORMAL_DESCRIPTION_INDEX = 0x2e73;
constexpr std::uint32_t FORMAT_DESCRIPTION_INDEX = 0x3005;
constexpr std::size_t FIRST_CUSTOM_DESCRIPTION = 70;
constexpr std::size_t LAST_TABLE_DESCRIPTION = 199;
constexpr std::size_t MAX_TEXT_UNITS = 512;
constexpr std::size_t DESCRIPTION_STORAGE_HEADER = sizeof(std::uintptr_t) * 2;

using NameGetter = const wchar_t* (__fastcall*)(void*);
using DescriptionGetter = const wchar_t* (__fastcall*)(void*, int, std::uint8_t);
using EffectQuery = int (__fastcall*)(void*, std::uint16_t);
using OfficerQuery = int (__fastcall*)(void*, std::uint32_t);
using MessageSetter = void (__fastcall*)(void*, std::uint32_t);
using DataGetter = void* (__fastcall*)(void*, std::uint32_t);
using MonthlyUpdate = int (__fastcall*)(void*);
using OfficerSerializer = bool (__fastcall*)(void*, void*);
using RegisteredTraitGetter = void* (__fastcall*)(void*, int);
using SavedTraitAvailability = int (__fastcall*)(int);
using ScenarioTraitAvailability = int (__fastcall*)(int, int);
using TransferEvent = int (__fastcall*)(void*, void*, void*, void*, void*);

enum class HookIndex : std::size_t { Name, Description, Effect, Officer, Message, Data, Monthly, Transfer, OfficerSerialization, RegisteredTrait, RecordDescription, Selectable };

bool prepare_assignment_table(TraitRuntimeNative& state);

std::atomic<std::uint64_t> callback_gate{CALLBACK_CLOSED};
std::atomic<TraitRuntimeNative*> control_owner{nullptr};
std::atomic<const NativeTraitTexts*> published_texts{nullptr};
std::atomic<std::uintptr_t> published_root{0};
std::atomic<std::uintptr_t> published_module{0};
std::atomic<std::uint32_t> published_pointer_offset{0x57a4d0};
std::array<std::atomic<void*>, TraitRuntimeNative::HOOK_COUNT> originals{};
std::atomic<void*> saved_trait_original{nullptr};
std::atomic<void*> scenario_trait_original{nullptr};
std::atomic<const NativeTraitTexts*> saved_trait_texts{nullptr};
std::atomic<std::uintptr_t> saved_trait_module{0};

// Temporary read-only trace for the added-officer 2008 reproduction.
HANDLE assignment_trace_file = INVALID_HANDLE_VALUE;
std::mutex assignment_trace_mutex;
using RegisteredSerializer = void (__fastcall*)(void*, void*);
RegisteredSerializer registered_serializer_original = nullptr;
thread_local bool tracing_registered_officer = false;

void assignment_trace(const char* format, ...) {
    if (assignment_trace_file == INVALID_HANDLE_VALUE) {
        return;
    }

    char line[768]{};
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    int prefix = sprintf_s(line, "qpc=%lld tid=%lu ", counter.QuadPart, GetCurrentThreadId());
    va_list args;
    va_start(args, format);
    vsnprintf_s(line + prefix, sizeof(line) - prefix, _TRUNCATE, format, args);
    va_end(args);

    const std::lock_guard<std::mutex> lock(assignment_trace_mutex);
    DWORD written = 0;
    WriteFile(assignment_trace_file, line, static_cast<DWORD>(strlen(line)), &written, nullptr);
}

void __fastcall trace_registered_serializer(void* record, void* stream) {
    const std::byte* bytes = static_cast<const std::byte*>(record);
    const std::uint16_t before_id = *reinterpret_cast<const std::uint16_t*>(bytes + 8);
    const bool previous = tracing_registered_officer;
    tracing_registered_officer = before_id == 2008;
    std::array<std::uint16_t, 3> before{};
    memcpy(before.data(), bytes + 0x12e, sizeof(before));

    if (before_id == 2008) {
        assignment_trace("registered_enter record=%p slots=%u/%u/%u catalog=%d\n",
            record, before[0], before[1], before[2], saved_trait_texts.load() != nullptr);
    }

    registered_serializer_original(record, stream);

    const std::uint16_t after_id = *reinterpret_cast<const std::uint16_t*>(bytes + 8);
    if (before_id == 2008 || after_id == 2008) {
        std::array<std::uint16_t, 3> after{};
        memcpy(after.data(), bytes + 0x12e, sizeof(after));
        assignment_trace("registered record=%p id=%u->%u slots=%u/%u/%u->%u/%u/%u catalog=%d\n",
            record, before_id, after_id, before[0], before[1], before[2],
            after[0], after[1], after[2], saved_trait_texts.load() != nullptr);
    }
    tracing_registered_officer = previous;
}


class CallbackAdmission {
public:
    CallbackAdmission() noexcept {
        std::uint64_t observed = callback_gate.load(std::memory_order_acquire);

        while ((observed & CALLBACK_CLOSED) == 0 && (observed & CALLBACK_COUNT) != CALLBACK_COUNT) {
            if (callback_gate.compare_exchange_weak(observed, observed + 1,
                    std::memory_order_acquire, std::memory_order_relaxed)) {
                admitted_ = true;
                break;
            }
        }
    }

    ~CallbackAdmission() {
        if (admitted_) {
            callback_gate.fetch_sub(1, std::memory_order_release);
        }
    }

    explicit operator bool() const noexcept { return admitted_; }
    CallbackAdmission(const CallbackAdmission&) = delete;
    CallbackAdmission& operator=(const CallbackAdmission&) = delete;

private:
    bool admitted_ = false;
};

struct MatchCache {
    const NativeTraitTexts* generation = nullptr;
    std::uintptr_t root = 0;
    std::uintptr_t query_return = 0;
    std::uint16_t requested = 0;
    std::uint16_t matched = 0;
};

thread_local MatchCache match_cache{};

struct BridgeSite {
    std::uintptr_t query_return;
    std::uintptr_t consumer_return;
    std::uint16_t requested;
};

constexpr std::array<BridgeSite, 8> MESSAGE_SITES{{
    {0x1854d85, 0x1854e73, 28}, {0x1859de5, 0x1859ecf, 1},
    {0x1902e70, 0x1902ef5, 10}, {0x1bcef8c, 0x1bcf20f, 11},
    {0x1bcff6e, 0x1bd0702, 202}, {0x1bd0954, 0x1bd0a16, 30},
    {0x1e4c093, 0x1e4c208, 19}, {0x18a5e30, 0x18a5f81, 26}
}};
constexpr std::array<BridgeSite, 1> DATA_SITES{{{0x19253e8, 0x192542f, 55}}};

template<class T>
bool read_value(std::uintptr_t address, T& value) {
    if (address < 0x10000 || address > std::numeric_limits<std::uintptr_t>::max() - sizeof(T)) {
        return false;
    }
    return platform::windows::read_memory(address, std::as_writable_bytes(std::span(&value, 1)));
}

template<class T>
bool write_value(std::uintptr_t address, const T& value) {
    return platform::windows::write_data_memory(address, std::as_bytes(std::span(&value, 1)));
}

template<class T>
T original(HookIndex index) {
    return reinterpret_cast<T>(originals[static_cast<std::size_t>(index)].load(std::memory_order_acquire));
}

bool executable_address(std::uintptr_t address) {
    MEMORY_BASIC_INFORMATION region{};

    if (!address || VirtualQuery(reinterpret_cast<const void*>(address), &region, sizeof(region)) != sizeof(region) ||
        region.State != MEM_COMMIT || (region.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
        return false;
    }

    const DWORD protection = region.Protect & 0xff;

    return protection == PAGE_EXECUTE || protection == PAGE_EXECUTE_READ ||
        protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE_WRITECOPY;
}

bool valid_game_object(void* object) {
    std::uintptr_t vtable = 0;
    std::uintptr_t validate = 0;

    if (!read_value(reinterpret_cast<std::uintptr_t>(object), vtable) ||
        !read_value(vtable + 0x48, validate) || !executable_address(validate)) {
        return false;
    }
    __try {
        return reinterpret_cast<bool (__fastcall*)(void*)>(validate)(object);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

struct TraitRecord {
    std::array<std::byte, TraitRuntimeNative::RECORD_SIZE> bytes{};

    std::uint16_t id() const {
        std::uint16_t value = 0;

        std::memcpy(&value, bytes.data() + TRAIT_ID_OFFSET, sizeof(value));
        return value;
    }

    domain::TraitEffect effect(std::size_t index) const {
        domain::TraitEffect value{};

        std::memcpy(&value, bytes.data() + EFFECT_OFFSET + index * sizeof(value), sizeof(value));
        return value;
    }
};

static_assert(sizeof(domain::TraitEffect) == 6);

bool matching_effect(const TraitRecord& held, const TraitRecord& requested) {
    for (std::size_t left = 0; left < 6; ++left) {
        const std::uint16_t held_type = held.effect(left).effect_id;

        if (held_type == 0) {
            continue;
        }
        for (std::size_t right = 0; right < 6; ++right) {
            if (held_type == requested.effect(right).effect_id) {
                return true;
            }
        }
    }
    return false;
}

std::uint16_t find_matching_trait(void* officer, std::uint16_t requested_id,
    const NativeTraitTexts* texts, std::uintptr_t root) {
    if (!texts || !officer || root < 0x10000 || requested_id == 0 || requested_id > domain::TRAIT_RUNTIME_COUNT) {
        return 0;
    }

    const std::uint32_t offset = published_pointer_offset.load(std::memory_order_relaxed);
    std::uintptr_t requested_address = 0;
    TraitRecord requested{};
    std::array<std::uintptr_t, 3> held_addresses{};

    if (!read_value(root + offset + requested_id * sizeof(std::uintptr_t), requested_address) ||
        !read_value(requested_address, requested) || requested.id() != requested_id ||
        !read_value(reinterpret_cast<std::uintptr_t>(officer) + 0x88, held_addresses)) {
        return 0;
    }
    for (const std::uintptr_t address : held_addresses) {
        TraitRecord held{};

        if (!read_value(address, held) || held.id() < 71 || held.id() > domain::TRAIT_RUNTIME_COUNT) {
            continue;
        }

        const NativeTraitTexts::Entry& metadata = texts->entries[held.id() - 1];
        const int requested_index = requested_id - 1;

        if (metadata.present &&
            (metadata.clone_source == requested_index || metadata.background_trait == requested_index) &&
            matching_effect(held, requested)) {
            return held.id();
        }
    }
    return 0;
}

std::uintptr_t return_offset(std::uintptr_t address) {
    const std::uintptr_t module = published_module.load(std::memory_order_relaxed);

    return address >= module && module != 0 ? address - module : 0;
}

std::uint16_t consume_match(std::uint32_t requested, std::uintptr_t caller, std::span<const BridgeSite> sites) {
    const NativeTraitTexts* texts = published_texts.load(std::memory_order_acquire);
    const std::uintptr_t root = published_root.load(std::memory_order_acquire);
    const std::uintptr_t offset = return_offset(caller);

    if (!texts || match_cache.generation != texts || match_cache.root != root ||
        match_cache.requested != requested || match_cache.matched < 71 ||
        match_cache.matched > domain::TRAIT_RUNTIME_COUNT) {
        return 0;
    }
    for (const BridgeSite& site : sites) {
        if (site.query_return != match_cache.query_return || site.consumer_return != offset ||
            site.requested != requested) {
            continue;
        }

        const std::uint32_t pointer_offset = published_pointer_offset.load(std::memory_order_relaxed);
        std::uintptr_t record = 0;
        std::uint16_t id = 0;

        if (!read_value(root + pointer_offset + match_cache.matched * sizeof(std::uintptr_t), record) ||
            !read_value(record + TRAIT_ID_OFFSET, id) || id != match_cache.matched) {
            return 0;
        }
        match_cache = {};
        return id;
    }
    return 0;
}

__declspec(noinline) const wchar_t* __fastcall name_getter(void* object) noexcept {
    const CallbackAdmission admitted{};
    const NameGetter native = original<NameGetter>(HookIndex::Name);
    const NativeTraitTexts* texts = published_texts.load(std::memory_order_acquire);
    std::uint16_t id = 0;
    const wchar_t* result = nullptr;

    if (texts && read_value(reinterpret_cast<std::uintptr_t>(object) + TRAIT_ID_OFFSET, id) &&
        id >= 1 && id <= domain::TRAIT_RUNTIME_COUNT) {
        result = texts->entries[id - 1].name;
    }
    if (!result || result[0] == L'\0') {
        result = native ? native(object) : L"";
    }
    return trait_text_name_override(result);
}

// Extended IDs use the string manager; kind 0 is a name, 1/2 are descriptions.
__declspec(noinline) const wchar_t* __fastcall description_getter(void* context, int id, std::uint8_t kind) noexcept {
    const CallbackAdmission admitted{};
    const DescriptionGetter native = original<DescriptionGetter>(HookIndex::Description);
    const NativeTraitTexts* texts = published_texts.load(std::memory_order_acquire);

    if (admitted && texts && id >= 201 && id <= static_cast<int>(domain::TRAIT_RUNTIME_COUNT) && kind <= 2) {
        const auto& entry = texts->entries[static_cast<std::size_t>(id - 1)];
        const wchar_t* text = kind == 0 ? entry.name : kind == 2 ? entry.format_description : entry.description;

        if (text && text[0] != L'\0') {
            return text;
        }
    }

    const wchar_t* result = native ? native(context, id, kind) : nullptr;
    return result ? result : L"";
}

// Officer selection renders through PersonalityData's formatted description
// getter. IDs 71..200 do not pass through the extended-ID string manager.
__declspec(noinline) const wchar_t* __fastcall record_description_getter(void* record) noexcept {
    const CallbackAdmission admitted{};
    const NativeTraitTexts* texts = published_texts.load(std::memory_order_acquire);
    const auto id = *reinterpret_cast<const std::uint16_t*>(static_cast<const std::byte*>(record) + TRAIT_ID_OFFSET);

    if (admitted && texts && id >= 1 && id <= domain::TRAIT_RUNTIME_COUNT) {
        const wchar_t* text = texts->entries[id - 1].description;

        if (text && text[0] != L'\0') {
            return text;
        }
    }

    return original<NameGetter>(HookIndex::RecordDescription)(record);
}

// Native IsValid rejects an empty first numeric effect. Named custom records
// may instead carry a managed effect, so they must remain selectable.
__declspec(noinline) bool __fastcall personality_selectable(void* record) noexcept {
    const CallbackAdmission admitted{};
    const NativeTraitTexts* texts = published_texts.load(std::memory_order_acquire);
    const auto id = *reinterpret_cast<const std::uint16_t*>(static_cast<const std::byte*>(record) + TRAIT_ID_OFFSET);

    if (admitted && texts && id >= 71 && id <= domain::TRAIT_RUNTIME_COUNT) {
        const auto& entry = texts->entries[id - 1];

        if (entry.present && entry.name && entry.name[0] != L'\0') {
            return true;
        }
    }

    return original<bool (__fastcall*)(void*)>(HookIndex::Selectable)(record);
}

__declspec(noinline) int __fastcall effect_query(void* officer, std::uint16_t requested) noexcept {
    const CallbackAdmission admitted{};
    const EffectQuery native = original<EffectQuery>(HookIndex::Effect);

    match_cache = {};
    if (native && native(officer, requested)) {
        return 1;
    }
    if (!admitted) {
        return 0;
    }

    const std::uintptr_t caller = return_offset(reinterpret_cast<std::uintptr_t>(_ReturnAddress()));
    const NativeTraitTexts* texts = published_texts.load(std::memory_order_acquire);
    const std::uintptr_t root = published_root.load(std::memory_order_acquire);

    if (caller == 0x1c7a135) {
        return 0;
    }

    const std::uint16_t matched = find_matching_trait(officer, requested, texts, root);

    if (matched != 0) {
        match_cache = {texts, root, caller, requested, matched};
        return 1;
    }
    return 0;
}

__declspec(noinline) int __fastcall officer_query(void* officer, std::uint32_t requested) noexcept {
    const CallbackAdmission admitted{};
    const OfficerQuery native = original<OfficerQuery>(HookIndex::Officer);
    const int result = native ? native(officer, requested) : 0;

    if (result || !admitted || requested != 11) {
        return result;
    }

    const NativeTraitTexts* texts = published_texts.load(std::memory_order_acquire);
    const std::uintptr_t root = published_root.load(std::memory_order_acquire);
    const std::uint16_t matched = find_matching_trait(officer, 11, texts, root);

    match_cache = {};
    if (matched != 0) {
        match_cache = {texts, root, return_offset(reinterpret_cast<std::uintptr_t>(_ReturnAddress())), 11, matched};
        return 1;
    }
    return 0;
}

__declspec(noinline) void __fastcall message_setter(void* message, std::uint32_t requested) noexcept {
    const CallbackAdmission admitted{};
    const MessageSetter native = original<MessageSetter>(HookIndex::Message);
    const std::uint16_t matched = admitted ? consume_match(requested,
        reinterpret_cast<std::uintptr_t>(_ReturnAddress()), MESSAGE_SITES) : 0;

    if (native) {
        native(message, matched != 0 ? matched : requested);
    }
}

__declspec(noinline) void* __fastcall data_getter(void* center, std::uint32_t requested) noexcept {
    const CallbackAdmission admitted{};
    const DataGetter native = original<DataGetter>(HookIndex::Data);
    void* result = native ? native(center, requested) : nullptr;
    const std::uint16_t matched = admitted ? consume_match(requested,
        reinterpret_cast<std::uintptr_t>(_ReturnAddress()), DATA_SITES) : 0;

    if (!native || matched == 0) {
        return result;
    }

    void* custom = native(center, matched);
    std::uint16_t actual = 0;

    return read_value(reinterpret_cast<std::uintptr_t>(custom) + TRAIT_ID_OFFSET, actual) && actual == matched ? custom : result;
}

__declspec(noinline) int __fastcall monthly_update(void* turn) noexcept {
    const CallbackAdmission admitted{};
    const MonthlyUpdate native = original<MonthlyUpdate>(HookIndex::Monthly);
    const int result = native ? native(turn) : 0;
    const std::uintptr_t root = published_root.load(std::memory_order_acquire);

    if (!admitted || root < 0x10000 || !published_texts.load(std::memory_order_acquire)) {
        return result;
    }
    // Signed effect 166, positive aggregate only, 1801 historical officer slots.
    for (std::uintptr_t slot = root + 0x576c88; slot < root + 0x57a4d0; slot += sizeof(std::uintptr_t)) {
        std::uintptr_t officer = 0;
        std::array<std::uintptr_t, 3> held{};
        int amount = 0;

        if (!read_value(slot, officer) || !valid_game_object(reinterpret_cast<void*>(officer)) ||
            !read_value(officer + 0x88, held)) {
            continue;
        }
        for (const std::uintptr_t address : held) {
            TraitRecord record{};

            if (!read_value(address, record) || record.id() < 71 || record.id() > domain::TRAIT_RUNTIME_COUNT) {
                continue;
            }
            for (std::size_t index = 0; index < 6; ++index) {
                const domain::TraitEffect effect = record.effect(index);
                std::int16_t signed_amount = 0;

                if (effect.effect_id == 166) {
                    std::memcpy(&signed_amount, &effect.value1, sizeof(signed_amount));
                    amount += signed_amount;
                }
            }
        }
        if (amount <= 0) {
            continue;
        }

        std::uint32_t current = 0;

        if (read_value(officer + 0x300, current)) {
            const std::uint32_t next = static_cast<std::uint32_t>(std::min<std::uint64_t>(99999,
                static_cast<std::uint64_t>(current) + static_cast<std::uint32_t>(amount)));

            if (next != current) {
                write_value(officer + 0x300, next);
            }
        }
    }
    return result;
}

__declspec(noinline) int __fastcall transfer_event(void* first, void* second, void* list, void* first_city, void* second_city) noexcept {
    const CallbackAdmission admitted{};
    const TransferEvent native = original<TransferEvent>(HookIndex::Transfer);
    const MatchCache previous = match_cache;

    if (!native) {
        return 0;
    }
    if (!admitted) {
        return native(first, second, list, first_city, second_city);
    }

    const NativeTraitTexts* texts = published_texts.load(std::memory_order_acquire);
    const std::uintptr_t root = published_root.load(std::memory_order_acquire);
    const std::uint16_t matched = find_matching_trait(second, 26, texts, root);

    match_cache = matched != 0 ? MatchCache{texts, root, 0x18a5e30, 26, matched} : MatchCache{};

    const int result = native(first, second, list, first_city, second_city);

    match_cache = previous;
    return result;
}

// Native serialization stores three uint16 IDs and resolves them against the
// current table on load. Prepare definitions before either operation; never skip
// native stream consumption when the definition table is still being initialized.
__declspec(noinline) bool __fastcall officer_serialization(void* officer, void* stream) {
    const OfficerSerializer native = original<OfficerSerializer>(HookIndex::OfficerSerialization);
    CallbackAdmission admission;
    TraitRuntimeNative* state = admission ? control_owner.load(std::memory_order_acquire) : nullptr;

    if (state) {
        const std::lock_guard<std::recursive_mutex> lock(state->table_mutex);

        if (state->catalog) {
            prepare_assignment_table(*state);
        }

        return native(officer, stream);
    }

    return native(officer, stream);
}

__declspec(noinline) void* __fastcall registered_trait_getter(void* officer, int slot) {
    const RegisteredTraitGetter native = original<RegisteredTraitGetter>(HookIndex::RegisteredTrait);
    CallbackAdmission admission;
    TraitRuntimeNative* state = admission ? control_owner.load(std::memory_order_acquire) : nullptr;

    if (state) {
        const std::lock_guard<std::recursive_mutex> lock(state->table_mutex);

        if (state->catalog) {
            prepare_assignment_table(*state);
        }

        return native(officer, slot);
    }

    return native(officer, slot);
}

struct HookSpec {
    std::uintptr_t offset;
    std::span<const std::byte> prologue;
    void* detour;
};

// Registered-person deserialization validates IDs 201..300 after reading the
// three uint16 slots. Custom definitions have no native unlock-state bits.
__declspec(noinline) int __fastcall saved_trait_available(int id) noexcept {
    const SavedTraitAvailability native = reinterpret_cast<SavedTraitAvailability>(
        saved_trait_original.load(std::memory_order_acquire));
    const std::uintptr_t caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    const NativeTraitTexts* texts = saved_trait_texts.load(std::memory_order_acquire);

    if (caller == saved_trait_module.load(std::memory_order_relaxed) + 0x1f05178 &&
        texts && id >= 201 && id <= static_cast<int>(domain::TRAIT_RUNTIME_COUNT)) {
        const NativeTraitTexts::Entry& entry = texts->entries[static_cast<std::size_t>(id - 1)];

        if (entry.present && entry.name && entry.name[0] != L'\0') {
            assignment_trace("validation id=%d result=1 custom=1 officer2008=%d\n", id, tracing_registered_officer);
            return 1;
        }
    }

    const int result = native(id);
    if (caller == saved_trait_module.load() + 0x1f05178) {
        assignment_trace("validation id=%d result=%d custom=0 catalog=%d officer2008=%d\n",
            id, result, texts != nullptr, tracing_registered_officer);
    }
    return result;
}

// Scenario initialization independently checks categories 8 and 9 for IDs 201..300.
// Only these two call sites admit named custom definitions. Native state bits and
// the subsequent officer-status exclusion remain owned by the game.
__declspec(noinline) int __fastcall scenario_trait_available(int category, int id) noexcept {
    const std::uintptr_t caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    const std::uintptr_t module = saved_trait_module.load(std::memory_order_relaxed);
    const NativeTraitTexts* texts = saved_trait_texts.load(std::memory_order_acquire);
    const bool scenario_check = (category == 8 && caller == module + 0x1c405b3) ||
        (category == 9 && caller == module + 0x1c405c3);

    if (scenario_check && texts && id >= 201 && id <= static_cast<int>(domain::TRAIT_RUNTIME_COUNT)) {
        const NativeTraitTexts::Entry& entry = texts->entries[static_cast<std::size_t>(id - 1)];

        if (entry.present && entry.name && entry.name[0] != L'\0') {
            assignment_trace("scenario_validation category=%d id=%d result=1 custom=1\n", category, id);
            return 1;
        }
    }

    const ScenarioTraitAvailability native = reinterpret_cast<ScenarioTraitAvailability>(
        scenario_trait_original.load(std::memory_order_acquire));
    return native(category, id);
}

bool install_saved_trait_filter(TraitRuntimeNative& state, std::uintptr_t module_base) {
    if (!state.saved_trait_filter.target) {
        constexpr std::array<std::byte, 6> prologue = native_hex("40534883EC20");
        const std::uintptr_t target = module_base + 0x685c60;

        if (!native_bytes_match(target, prologue)) {
            state.status = TraitRuntimeStatus::UnsupportedVersion;
            return false;
        }
        if (!platform::windows::retain_callback_module(reinterpret_cast<std::uintptr_t>(&saved_trait_available))) {
            state.status = TraitRuntimeStatus::HookFailed;
            return false;
        }

        const std::expected<platform::windows::Hook, int> hook =
            platform::windows::create_hook(reinterpret_cast<void*>(target), reinterpret_cast<void*>(&saved_trait_available));

        if (!hook) {
            state.status = TraitRuntimeStatus::HookFailed;
            return false;
        }
        state.saved_trait_filter = *hook;
        saved_trait_original.store(hook->original, std::memory_order_release);
        saved_trait_module.store(module_base, std::memory_order_release);
    }

    if (!state.scenario_trait_filter.target) {
        constexpr std::array<std::byte, 6> prologue = native_hex("48895C241057");
        constexpr std::array<std::byte, 5> category8_call = native_hex("e89d8849fe");
        constexpr std::array<std::byte, 5> category9_call = native_hex("e88d8849fe");
        const std::uintptr_t target = module_base + 0xd8e50;

        if (!native_bytes_match(target, prologue) ||
            !native_bytes_match(module_base + 0x1c405ae, category8_call) ||
            !native_bytes_match(module_base + 0x1c405be, category9_call)) {
            state.status = TraitRuntimeStatus::UnsupportedVersion;
            return false;
        }

        const std::expected<platform::windows::Hook, int> hook = platform::windows::create_hook(
            reinterpret_cast<void*>(target), reinterpret_cast<void*>(&scenario_trait_available));

        if (!hook) {
            state.status = TraitRuntimeStatus::HookFailed;
            return false;
        }
        state.scenario_trait_filter = *hook;
        scenario_trait_original.store(hook->original, std::memory_order_release);
    }

    assignment_trace("filter_enable catalog=%d\n", saved_trait_texts.load() != nullptr);
    for (void* target : {state.saved_trait_filter.target, state.scenario_trait_filter.target}) {
        const MH_STATUS result = MH_EnableHook(target);

        if (result != MH_OK && result != MH_ERROR_ENABLED) {
            state.status = TraitRuntimeStatus::HookFailed;
            return false;
        }
    }
    return true;
}

constexpr std::array<std::byte, 5> COMMON_PROLOGUE = native_hex("48895C2408");
constexpr std::array<std::byte, 6> MESSAGE_PROLOGUE = native_hex("40534883EC20");
constexpr std::array<std::byte, 7> DATA_PROLOGUE = native_hex("4883EC288D42FF");
constexpr std::array<std::byte, 8> MONTHLY_PROLOGUE = native_hex("4C8BDC5341544157");
constexpr std::array<std::byte, 7> TRANSFER_PROLOGUE = native_hex("40535556574156");
constexpr std::array<std::byte, 7> SERIALIZER_PROLOGUE = native_hex("40555356574157");
constexpr std::array<std::byte, 7> REGISTERED_TRAIT_PROLOGUE = native_hex("4883EC284863C2");
constexpr auto RECORD_DESCRIPTION_PROLOGUE = native_hex("48895C2410488974241857");
constexpr auto SELECTABLE_PROLOGUE = native_hex("0FB74108BA2B01000066FFC8");
constexpr std::array<std::byte, 6> ELIGIBILITY_ORIGINAL = native_hex("81FE95000000");
constexpr std::array<std::byte, 6> ELIGIBILITY_ENABLED = native_hex("81FE01000000");

const std::array<HookSpec, TraitRuntimeNative::HOOK_COUNT> HOOK_SPECS{{
    // Upstream validates executability, not a guessed prologue, for the name getter.
    {0x170e2d0, {}, reinterpret_cast<void*>(&name_getter)},
    {0x171e070, COMMON_PROLOGUE, reinterpret_cast<void*>(&description_getter)},
    {0x17aad60, COMMON_PROLOGUE, reinterpret_cast<void*>(&effect_query)},
    {0x170c080, COMMON_PROLOGUE, reinterpret_cast<void*>(&officer_query)},
    {0x13f91e0, MESSAGE_PROLOGUE, reinterpret_cast<void*>(&message_setter)},
    {0x16e63f0, DATA_PROLOGUE, reinterpret_cast<void*>(&data_getter)},
    {0x1c79c90, MONTHLY_PROLOGUE, reinterpret_cast<void*>(&monthly_update)},
    {0x18a5e30, TRANSFER_PROLOGUE, reinterpret_cast<void*>(&transfer_event)},
    {0x16f6970, SERIALIZER_PROLOGUE, reinterpret_cast<void*>(&officer_serialization)},
    {0x1f032e0, REGISTERED_TRAIT_PROLOGUE, reinterpret_cast<void*>(&registered_trait_getter)},
    {0x170abb0, RECORD_DESCRIPTION_PROLOGUE, reinterpret_cast<void*>(&record_description_getter)},
    {0x16f9240, SELECTABLE_PROLOGUE, reinterpret_cast<void*>(&personality_selectable)}
}};

bool disable_hooks(TraitRuntimeNative& state) {
    bool disabled = true;

    callback_gate.fetch_or(CALLBACK_CLOSED, std::memory_order_acq_rel);
    for (std::size_t index = 0; index < state.created_hooks; ++index) {
        const MH_STATUS result = MH_DisableHook(state.hooks[index].target);
        const MH_STATUS queued = MH_QueueDisableHook(state.hooks[index].target);

        if ((result != MH_OK && result != MH_ERROR_DISABLED) ||
            (queued != MH_OK && queued != MH_ERROR_DISABLED)) {
            disabled = false;
        }
    }
    if (disabled) {
        state.hooks_enabled = false;
    }
    return disabled;
}

bool install_hooks(TraitRuntimeNative& state, std::uintptr_t module_base) {
    TraitRuntimeNative* owner = control_owner.load(std::memory_order_acquire);

    if (owner && owner != &state) {
        state.status = TraitRuntimeStatus::Conflict;
        return false;
    }
    if (!platform::windows::retain_callback_module(reinterpret_cast<std::uintptr_t>(&name_getter))) {
        state.status = TraitRuntimeStatus::HookFailed;
        return false;
    }
    if (state.module_base && state.module_base != module_base) {
        state.status = TraitRuntimeStatus::UnsupportedVersion;
        return false;
    }
    state.module_base = module_base;
    control_owner.store(&state, std::memory_order_release);
    for (std::size_t index = state.created_hooks; index < HOOK_SPECS.size(); ++index) {
        const HookSpec& spec = HOOK_SPECS[index];
        const std::uintptr_t target = module_base + spec.offset;

        if (!executable_address(target) || (!spec.prologue.empty() && !native_bytes_match(target, spec.prologue))) {
            state.status = TraitRuntimeStatus::UnsupportedVersion;
            return false;
        }
    }
    for (std::size_t index = state.created_hooks; index < HOOK_SPECS.size(); ++index) {
        const HookSpec& spec = HOOK_SPECS[index];
        const std::expected<platform::windows::Hook, int> created =
            platform::windows::create_hook(reinterpret_cast<void*>(module_base + spec.offset), spec.detour);

        if (!created) {
            state.status = TraitRuntimeStatus::HookFailed;
            return false;
        }
        state.hooks[index] = *created;
        ++state.created_hooks;
        originals[index].store(created->original, std::memory_order_release);
    }
    if (state.hooks_enabled) {
        return true;
    }
    published_module.store(module_base, std::memory_order_release);
    published_pointer_offset.store(state.trait_pointer_offset, std::memory_order_release);
    callback_gate.fetch_and(CALLBACK_COUNT, std::memory_order_release);
    for (const platform::windows::Hook& hook : state.hooks) {
        const MH_STATUS queued = MH_QueueEnableHook(hook.target);

        if (queued != MH_OK && queued != MH_ERROR_ENABLED) {
            disable_hooks(state);
            state.status = TraitRuntimeStatus::HookFailed;
            return false;
        }
    }
    if (MH_ApplyQueued() != MH_OK) {
        disable_hooks(state);
        state.status = TraitRuntimeStatus::HookFailed;
        return false;
    }
    state.hooks_enabled = true;
    return true;
}

int utf16_count(const std::string& value) {
    if (value.empty()) {
        return 0;
    }
    if (value.find('\0') != std::string::npos || value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return -1;
    }

    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0);

    return count > 0 && count <= static_cast<int>(MAX_TEXT_UNITS) ? count : -1;
}

bool put_utf16(const std::string& value, int count, wchar_t*& cursor, const wchar_t*& output) {
    if (count == 0) {
        output = nullptr;
        return true;
    }
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
            static_cast<int>(value.size()), cursor, count) != count) {
        return false;
    }
    cursor[count] = L'\0';
    output = cursor;
    cursor += count + 1;
    return true;
}

NativeTraitTexts* prepare_texts(const domain::TraitRuntimeCatalog& catalog, TraitRuntimeStatus& status) {
    std::array<int, domain::TRAIT_RUNTIME_COUNT> names{};
    std::array<int, domain::TRAIT_RUNTIME_COUNT> descriptions{};
    std::size_t units = 0;

    for (std::size_t index = 0; index < catalog.metadata.size(); ++index) {
        const domain::TraitRuntimeMetadata& row = catalog.metadata[index];

        names[index] = row.present ? utf16_count(row.name) : 0;
        descriptions[index] = row.present ? utf16_count(row.description) : 0;
        if (names[index] < 0 || descriptions[index] < 0) {
            status = TraitRuntimeStatus::InvalidText;
            return nullptr;
        }
        if (names[index] != 0) {
            units += static_cast<std::size_t>(names[index]) + 1;
        }
        if (descriptions[index] != 0) {
            units += static_cast<std::size_t>(descriptions[index]) * 3 + 2;
        }
    }

    const std::size_t bytes = sizeof(NativeTraitTexts) + units * sizeof(wchar_t);
    void* allocation = VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);

    if (!allocation) {
        status = TraitRuntimeStatus::AllocationFailed;
        return nullptr;
    }

    NativeTraitTexts* texts = new (allocation) NativeTraitTexts{};
    wchar_t* cursor = reinterpret_cast<wchar_t*>(texts + 1);

    texts->allocated_bytes = bytes;
    for (std::size_t index = 0; index < catalog.metadata.size(); ++index) {
        const domain::TraitRuntimeMetadata& row = catalog.metadata[index];
        NativeTraitTexts::Entry& entry = texts->entries[index];

        entry.present = row.present;
        entry.clone_source = row.clone_source;
        entry.background_trait = row.background_trait;
        if (!put_utf16(row.name, names[index], cursor, entry.name) ||
            !put_utf16(row.description, descriptions[index], cursor, entry.description)) {
            VirtualFree(allocation, 0, MEM_RELEASE);
            status = TraitRuntimeStatus::InvalidText;
            return nullptr;
        }
        if (entry.description) {
            entry.format_description = cursor;
            for (const wchar_t* character = entry.description; *character != L'\0'; ++character) {
                *cursor++ = *character;
                if (*character == L'%') {
                    *cursor++ = L'%';
                }
            }
            *cursor++ = L'\0';
        }
    }
    return texts;
}

bool records_ready(const TraitRuntimeNative::Records& records) {
    std::uint16_t first = 0;
    std::size_t nonzero = 0;

    std::memcpy(&first, records[0].data() + TRAIT_ID_OFFSET, sizeof(first));
    if (first != 1) {
        return false;
    }
    for (std::size_t index = 0; index < 30; ++index) {
        for (std::size_t slot = 0; slot < 6; ++slot) {
            std::uint16_t type = 0;

            std::memcpy(&type, records[index].data() + EFFECT_OFFSET + slot * sizeof(domain::TraitEffect), sizeof(type));
            if (type != 0) {
                if (type < 102 || type > 194) {
                    return false;
                }
                ++nonzero;
            }
        }
    }
    return nonzero >= 10;
}

bool owned_record_bytes(const TraitRuntimeNative::Records& current,
    const TraitRuntimeNative::Records& original_records, const TraitRuntimeNative::Records& replacement,
    const TraitRuntimeNative::Records* previous) {
    for (std::size_t row = 0; row < current.size(); ++row) {
        for (std::size_t byte = 0; byte < current[row].size(); ++byte) {
            if (current[row][byte] != original_records[row][byte] && current[row][byte] != replacement[row][byte] &&
                (!previous || current[row][byte] != (*previous)[row][byte])) {
                return false;
            }
        }
    }
    return true;
}

bool apply_records(TraitRuntimeNative& state, const TraitRuntimeNative::Records& current) {
    TraitRuntimeNative::Records desired = state.table_applied ? state.original : current;

    for (std::size_t index = 0; index < desired.size(); ++index) {
        const domain::TraitRuntimeMetadata& metadata = state.catalog->metadata[index];
        const domain::TraitRuntimeEntry& entry = state.catalog->traits[index];
        const std::uint16_t id = static_cast<std::uint16_t>(index + 1);

        // Preserve the upstream sequential donor dependency: preceding records
        // already contain this pass's edits, later records have not been visited.
        if (metadata.present && (!metadata.name.empty() || !metadata.description.empty())) {
            int source = metadata.clone_source;

            if (source < 0 || source >= static_cast<int>(desired.size()) || source == static_cast<int>(index)) {
                source = 30;
            }
            if (source != static_cast<int>(index)) {
                // The donor supplies the runtime body, not the trait's badge: an
                // unset grade keeps the trait's own valid grade (1..3). Mods cannot
                // read it themselves; the table is overwritten before it is readable.
                std::uint16_t own_grade = 0;
                std::memcpy(&own_grade, desired[index].data() + 0x2e, sizeof(own_grade));
                desired[index] = desired[static_cast<std::size_t>(source)];
                if (metadata.grade < 0 && own_grade >= 1 && own_grade <= 3) {
                    std::memcpy(desired[index].data() + 0x2e, &own_grade, sizeof(own_grade));
                }
            }
        }
        if (metadata.present && metadata.background_trait >= 0 &&
            metadata.background_trait < static_cast<int>(desired.size()) && metadata.background_trait != static_cast<int>(index)) {
            std::memcpy(desired[index].data() + METADATA_OFFSET,
                desired[static_cast<std::size_t>(metadata.background_trait)].data() + METADATA_OFFSET, METADATA_SIZE);
        }
        if (metadata.grade >= 0) {
            const auto grade = static_cast<std::uint16_t>(metadata.grade);
            std::memcpy(desired[index].data() + 0x2e, &grade, sizeof(grade));
        }
        if (metadata.voice >= 0) {
            const auto voice = static_cast<std::uint16_t>(metadata.voice);
            std::memcpy(desired[index].data() + 0x30, &voice, sizeof(voice));
        }
        std::memcpy(desired[index].data() + TRAIT_ID_OFFSET, &id, sizeof(id));
        if (entry.present) {
            std::memcpy(desired[index].data() + EFFECT_OFFSET, entry.trait.effects.data(),
                entry.trait.effects.size() * sizeof(domain::TraitEffect));
        }
    }

    const std::uintptr_t address = state.table + TraitRuntimeNative::RECORD_SIZE;

    if (!state.table_applied) {
        state.original = current;
    }
    if (!platform::windows::write_data_memory(address, std::as_bytes(std::span(desired)))) {
        if (!platform::windows::write_data_memory(address, std::as_bytes(std::span(current)))) {
            state.uncertain_previous = current;
            state.replacement = desired;
            state.table_applied = true;
            state.table_uncertain = true;
            state.status = TraitRuntimeStatus::Conflict;
        } else {
            state.status = TraitRuntimeStatus::MemoryFailed;
        }
        return false;
    }
    state.table_applied = true;
    state.replacement = desired;
    state.table_uncertain = false;
    state.pending = false;
    return true;
}

bool prepare_records(TraitRuntimeNative& state, std::uintptr_t game_root,
    std::uint64_t epoch, std::uint64_t real_milliseconds, bool immediate) {
    if (state.game_root != game_root || state.epoch != epoch) {
        published_root.store(0, std::memory_order_release);
        state.game_root = game_root;
        state.epoch = epoch;
        state.table = 0;
        state.table_applied = false;
        state.pending = true;
        state.last_tick_ms = 0;
    }
    if (!immediate && state.last_tick_ms != 0 && real_milliseconds - state.last_tick_ms < 500) {
        return state.table_applied;
    }
    if (!immediate) {
        state.last_tick_ms = real_milliseconds;
    }

    std::uintptr_t table = 0;

    if (game_root < 0x10000 || !read_value(game_root + state.trait_pointer_offset, table) || table < 0x10000) {
        published_root.store(0, std::memory_order_release);
        state.status = TraitRuntimeStatus::WaitingForTable;
        return false;
    }
    if (table != state.table) {
        state.table = table;
        state.table_applied = false;
        state.pending = true;
    }
    if (state.table_applied && !state.pending && state.sentinel_index >= 0) {
        TraitRecord sentinel{};
        const std::size_t index = static_cast<std::size_t>(state.sentinel_index);
        const domain::Trait& desired = state.catalog->traits[index].trait;

        if (!read_value(table + (index + 1) * TraitRuntimeNative::RECORD_SIZE, sentinel)) {
            published_root.store(0, std::memory_order_release);
            state.status = TraitRuntimeStatus::WaitingForTable;
            return false;
        }
        if (sentinel.id() == desired.id && std::memcmp(sentinel.bytes.data() + EFFECT_OFFSET,
                desired.effects.data(), sizeof(desired.effects)) == 0) {
            published_root.store(game_root, std::memory_order_release);
            return true;
        }
        state.table_applied = false;
        state.pending = true;
    }

    TraitRuntimeNative::Records current{};

    if (!read_value(table + TraitRuntimeNative::RECORD_SIZE, current) ||
        (!state.table_applied && !records_ready(current))) {
        published_root.store(0, std::memory_order_release);
        state.status = TraitRuntimeStatus::WaitingForTable;
        return false;
    }
    if (state.table_applied && current != state.replacement) {
        // Configuration replacement must not overwrite an unrecognized edit.
        state.status = TraitRuntimeStatus::Conflict;
        return false;
    }
    if (state.pending && !apply_records(state, current)) {
        return false;
    }
    return state.table_applied;
}

bool prepare_assignment_table(TraitRuntimeNative& state) {
    if (state.stopping || state.table_uncertain) {
        return false;
    }

    std::uintptr_t root = 0;

    // Native save/load and registered-person lookup use this same owner.
    if (!read_value(state.module_base + 0x2e98bc8, root) || root == 0) {
        state.status = TraitRuntimeStatus::WaitingForTable;
        return false;
    }

    return prepare_records(state, root, state.epoch, GetTickCount64(), true);
}

bool restore_descriptions(TraitRuntimeNative& state) {
    if (state.description_count != 0) {
        std::uintptr_t resource = 0;
        std::uint32_t offset = 0;

        if (!read_value(state.module_base + 0x2c34ec0, resource) ||
            !read_value(resource + 0x10, offset)) {
            state.status = TraitRuntimeStatus::MemoryFailed;
            return false;
        }
        if (resource + offset != state.description_table) {
            state.description_count = 0;
            state.description_table = 0;
            state.description_texts = nullptr;
            state.description_uncertain = false;
            return true;
        }
    }
    for (std::size_t index = 0; index < state.description_count; ++index) {
        const TraitDescriptionPatch& patch = state.descriptions[index];
        std::uint32_t current = 0;

        if (!read_value(patch.address, current)) {
            state.status = TraitRuntimeStatus::MemoryFailed;
            return false;
        }
        const std::span<const std::byte> bytes = std::as_bytes(std::span(&current, 1));
        const std::span<const std::byte> original_bytes = std::as_bytes(std::span(&patch.original, 1));
        const std::span<const std::byte> replacement_bytes = std::as_bytes(std::span(&patch.replacement, 1));
        const std::span<const std::byte> previous_bytes = std::as_bytes(std::span(&patch.previous, 1));
        bool owned = true;

        for (std::size_t byte = 0; byte < bytes.size(); ++byte) {
            owned = owned && (bytes[byte] == original_bytes[byte] || bytes[byte] == replacement_bytes[byte] ||
                (state.description_uncertain && bytes[byte] == previous_bytes[byte]));
        }
        if (!owned) {
            state.status = TraitRuntimeStatus::Conflict;
            return false;
        }
    }
    for (std::size_t count = state.description_count; count != 0; --count) {
        const TraitDescriptionPatch& patch = state.descriptions[count - 1];

        if (!write_value(patch.address, patch.original)) {
            state.status = TraitRuntimeStatus::MemoryFailed;
            return false;
        }
    }
    state.description_count = 0;
    state.description_table = 0;
    state.description_texts = nullptr;
    state.description_uncertain = false;
    return true;
}

std::uintptr_t allocate_description_storage(std::uintptr_t table, std::size_t size) {
    constexpr std::uintptr_t GRANULARITY = 0x10000;
    constexpr std::uintptr_t MAX_DISTANCE = 0xffff0000;
    std::uintptr_t candidate = (table + GRANULARITY - 1) & ~(GRANULARITY - 1);
    const std::uintptr_t limit = table <= std::numeric_limits<std::uintptr_t>::max() - MAX_DISTANCE ?
        table + MAX_DISTANCE : std::numeric_limits<std::uintptr_t>::max();

    while (candidate >= table && candidate < limit && size <= limit - candidate) {
        MEMORY_BASIC_INFORMATION region{};

        if (VirtualQuery(reinterpret_cast<void*>(candidate), &region, sizeof(region)) != sizeof(region) || region.RegionSize == 0) {
            return 0;
        }
        if (region.State == MEM_FREE && size <= region.RegionSize) {
            void* allocation = VirtualAlloc(reinterpret_cast<void*>(candidate), size,
                MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);

            if (allocation) {
                return reinterpret_cast<std::uintptr_t>(allocation);
            }
        }

        const std::uintptr_t start = reinterpret_cast<std::uintptr_t>(region.BaseAddress);

        if (region.RegionSize > std::numeric_limits<std::uintptr_t>::max() - GRANULARITY ||
            start > std::numeric_limits<std::uintptr_t>::max() - region.RegionSize - GRANULARITY) {
            return 0;
        }
        candidate = (start + region.RegionSize + GRANULARITY - 1) & ~(GRANULARITY - 1);
    }
    return 0;
}

bool sync_descriptions(TraitRuntimeNative& state) {
    if (state.description_uncertain && !restore_descriptions(state)) {
        return false;
    }
    std::uintptr_t resource = 0;
    std::uint16_t header = 0;
    std::uint32_t offset = 0;

    if (!state.texts || !read_value(state.module_base + 0x2c34ec0, resource) ||
        !read_value(resource + 0x0a, header) || !read_value(resource + 0x10, offset) || header != 4 || offset == 0) {
        return false;
    }

    const std::uintptr_t table = resource + offset;

    if (state.description_table == table && state.description_texts == state.texts) {
        return true;
    }
    if (state.description_table != table) {
        // Resource replacement invalidates the old address generation. Never
        // restore old offsets into an allocation that the game may have reused.
        state.description_count = 0;
        state.description_table = 0;
        state.description_texts = nullptr;
    }

    std::size_t bytes = 0;

    for (std::size_t index = FIRST_CUSTOM_DESCRIPTION; index <= LAST_TABLE_DESCRIPTION; ++index) {
        const NativeTraitTexts::Entry& entry = state.texts->entries[index];

        if (entry.description) {
            bytes += (std::wcslen(entry.description) + 1) * sizeof(wchar_t);
            bytes += (std::wcslen(entry.format_description) + 1) * sizeof(wchar_t);
        }
    }
    if (bytes == 0) {
        if (!restore_descriptions(state)) {
            return false;
        }
        state.description_table = table;
        state.description_texts = state.texts;
        return true;
    }

    if (state.description_candidate_table != table || state.description_candidate_texts != state.texts) {
        if (state.description_candidate && !state.description_candidate_exposed) {
            VirtualFree(reinterpret_cast<void*>(state.description_candidate), 0, MEM_RELEASE);
        }
        state.description_candidate = 0;
        state.description_candidate_exposed = false;
        state.description_candidate_table = table;
        state.description_candidate_texts = state.texts;
    }
    if (!state.description_candidate) {
        state.description_candidate = allocate_description_storage(table, bytes + DESCRIPTION_STORAGE_HEADER);
    }

    const std::uintptr_t allocation = state.description_candidate;
    std::array<TraitDescriptionPatch, TraitRuntimeNative::DESCRIPTION_COUNT> patches{};
    std::array<std::uint32_t, TraitRuntimeNative::DESCRIPTION_COUNT> previous{};
    std::size_t count = 0;
    std::size_t cursor = 0;

    if (!allocation) {
        state.status = TraitRuntimeStatus::AllocationFailed;
        return false;
    }
    // Build a replacement transaction without first removing the current edit.
    // Old-only entries restore their saved value; new entries capture their own.
    for (std::size_t index = FIRST_CUSTOM_DESCRIPTION; index <= LAST_TABLE_DESCRIPTION; ++index) {
        const NativeTraitTexts::Entry& entry = state.texts->entries[index];

        for (std::size_t kind = 0; kind < 2; ++kind) {
            const std::uintptr_t address = table +
                ((kind == 0 ? NORMAL_DESCRIPTION_INDEX : FORMAT_DESCRIPTION_INDEX) + index) * sizeof(std::uint32_t);
            const TraitDescriptionPatch* owned = nullptr;

            for (std::size_t prior = 0; prior < state.description_count; ++prior) {
                if (state.descriptions[prior].address == address) {
                    owned = &state.descriptions[prior];
                    break;
                }
            }
            if (!entry.description && !owned) {
                continue;
            }

            TraitDescriptionPatch& patch = patches[count];
            std::uint32_t current = 0;

            if (!read_value(address, current) || (owned && current != owned->replacement)) {
                if (!state.description_candidate_exposed) {
                    VirtualFree(reinterpret_cast<void*>(allocation), 0, MEM_RELEASE);
                    state.description_candidate = 0;
                }
                state.status = TraitRuntimeStatus::Conflict;
                return false;
            }
            patch.address = address;
            patch.original = owned ? owned->original : current;
            patch.previous = current;
            patch.replacement = patch.original;
            previous[count++] = current;
            if (entry.description) {
                const wchar_t* text = kind == 0 ? entry.description : entry.format_description;
                const std::size_t size = (std::wcslen(text) + 1) * sizeof(wchar_t);
                const std::uintptr_t destination = allocation + DESCRIPTION_STORAGE_HEADER + cursor;

                patch.replacement = static_cast<std::uint32_t>(destination - table);
                if (!state.description_candidate_exposed) {
                    std::memcpy(reinterpret_cast<void*>(destination), text, size);
                }
                cursor += size;
            }
        }
    }
    if (!state.description_candidate_exposed) {
        const std::array<std::uintptr_t, 2> storage_header{{state.retained_description_storage,
            bytes + DESCRIPTION_STORAGE_HEADER}};

        std::memcpy(reinterpret_cast<void*>(allocation), storage_header.data(), sizeof(storage_header));
        state.retained_description_storage = allocation;
        state.description_candidate_exposed = true;
    }
    for (std::size_t index = 0; index < count; ++index) {
        if (write_value(patches[index].address, patches[index].replacement)) {
            continue;
        }

        bool restored = true;

        for (std::size_t rollback = index + 1; rollback != 0; --rollback) {
            if (!write_value(patches[rollback - 1].address, previous[rollback - 1])) {
                restored = false;
            }
        }
        // Any attempted pointer publication retains storage, even after a
        // successful rollback, because the game can have observed the pointer.
        if (!restored) {
            state.descriptions = patches;
            state.description_count = count;
            state.description_table = table;
            state.description_texts = nullptr;
            state.description_uncertain = true;
        }
        state.status = restored ? TraitRuntimeStatus::MemoryFailed : TraitRuntimeStatus::Conflict;
        return false;
    }
    state.descriptions = patches;
    state.description_count = count;
    state.description_table = table;
    state.description_texts = state.texts;
    state.description_uncertain = false;
    return true;
}

bool prepare_eligibility(TraitRuntimeNative& state) {
    if (state.eligibility.count != 0) {
        return true;
    }
    for (const std::uintptr_t offset : {std::uintptr_t{0x17c03e7}, std::uintptr_t{0x17c042f}}) {
        if (!native_bytes_match(state.module_base + offset, ELIGIBILITY_ORIGINAL)) {
            state.status = TraitRuntimeStatus::Conflict;
            return false;
        }
    }
    return append_native_patch(state.eligibility, state.module_base + 0x17c03e7,
            ELIGIBILITY_ORIGINAL, ELIGIBILITY_ENABLED) &&
        append_native_patch(state.eligibility, state.module_base + 0x17c042f,
            ELIGIBILITY_ORIGINAL, ELIGIBILITY_ENABLED);
}

} // namespace


void start_trait_assignment_trace(std::uintptr_t module_base) {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    wchar_t* separator = wcsrchr(path, L'\\');
    if (!separator) {
        return;
    }
    swprintf_s(separator + 1, MAX_PATH - (separator + 1 - path),
        L"S8_trait_assignment_%lu.log", GetCurrentProcessId());
    assignment_trace_file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ,
        nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    assignment_trace("start module=%p frequency=%lld\n", reinterpret_cast<void*>(module_base), frequency.QuadPart);

    constexpr std::array<std::byte, 8> prologue = native_hex("4053555657415448");
    if (!native_bytes_match(module_base + 0x1f04b30, prologue)) {
        assignment_trace("registered_hook unsupported_prologue\n");
        return;
    }
    if (!platform::windows::retain_callback_module(reinterpret_cast<std::uintptr_t>(&trace_registered_serializer))) {
        assignment_trace("registered_hook retain_failed\n");
        return;
    }
    const std::expected<platform::windows::Hook, int> hook = platform::windows::create_hook(
        reinterpret_cast<void*>(module_base + 0x1f04b30), reinterpret_cast<void*>(&trace_registered_serializer));
    if (!hook) {
        assignment_trace("registered_hook create_failed=%d\n", hook.error());
        return;
    }
    registered_serializer_original = reinterpret_cast<RegisteredSerializer>(hook->original);
    assignment_trace("registered_hook enable=%d\n", platform::windows::enable_hook(*hook));
}

void sample_trait_assignment_trace(std::uintptr_t module_base) {
    if (assignment_trace_file == INVALID_HANDLE_VALUE) {
        return;
    }
    std::uintptr_t owner = 0;
    std::uintptr_t person = 0;
    std::array<std::uintptr_t, 3> pointers{};
    std::array<std::uint16_t, 3> ids{};
    if (!platform::windows::read_memory(module_base + 0x2e98bc8, std::as_writable_bytes(std::span(&owner, 1))) || !owner ||
        !platform::windows::read_memory(owner + 0x21bdf0 + 2008 * 8, std::as_writable_bytes(std::span(&person, 1))) || !person ||
        !platform::windows::read_memory(person + 0x88, std::as_writable_bytes(std::span(pointers)))) {
        return;
    }
    for (std::size_t index = 0; index < pointers.size(); ++index) {
        if (pointers[index] && !platform::windows::read_memory(pointers[index] + 8,
            std::as_writable_bytes(std::span(&ids[index], 1)))) {
            return;
        }
    }
    static std::uintptr_t previous_person = 0;
    static std::array<std::uint16_t, 3> previous_ids{};
    if (previous_person != person || previous_ids != ids) {
        assignment_trace("scenario_sample person=%p slots=%u/%u/%u catalog=%d\n",
            reinterpret_cast<void*>(person), ids[0], ids[1], ids[2], saved_trait_texts.load() != nullptr);
        previous_person = person;
        previous_ids = ids;
    }
}

bool set_trait_runtime_catalog(TraitRuntimeNative& state, std::uintptr_t module_base,
    own::shared_owner<const domain::TraitRuntimeCatalog> catalog) {
    const std::lock_guard<std::recursive_mutex> lock(state.table_mutex);

    if (!catalog || !module_base || module_base > std::numeric_limits<std::uintptr_t>::max() - MAX_MODULE_OFFSET) {
        state.status = TraitRuntimeStatus::UnsupportedVersion;
        return false;
    }
    if (state.stopping) {
        // A prior catalog removal can have paused while an admitted callback
        // drains. Complete that removal before accepting its retry or replacement.
        if (!stop_trait_runtime_native(state, state.game_root, state.epoch)) {
            return false;
        }
        state.catalog = {};
        state.pending = false;
        state.stopping = false;
    }
    bool populated = false;

    for (std::size_t index = 0; index < catalog->traits.size(); ++index) {
        if (catalog->traits[index].present && catalog->traits[index].trait.id != index + 1) {
            state.status = TraitRuntimeStatus::UnsupportedVersion;
            return false;
        }
        populated = populated || catalog->traits[index].present || catalog->metadata[index].present;
    }
    if (!populated) {
        if (!stop_trait_runtime_native(state, state.game_root, state.epoch)) {
            return false;
        }
        state.catalog = {};
        state.stopping = false;
        state.pending = false;
        return true;
    }

    NativeTraitTexts* next = prepare_texts(*catalog, state.status);

    if (!next) {
        return false;
    }
    if (!install_hooks(state, module_base) || !prepare_eligibility(state) ||
        !install_saved_trait_filter(state, module_base)) {
        VirtualFree(next, 0, MEM_RELEASE);
        return false;
    }
    next->previous = state.texts;
    state.texts = next;
    state.catalog = std::move(catalog);
    state.pending = true;
    state.last_tick_ms = 0;
    state.sentinel_index = -1;
    for (std::size_t index = 0; index < state.catalog->traits.size(); ++index) {
        const domain::TraitRuntimeMetadata& metadata = state.catalog->metadata[index];

        if (state.catalog->traits[index].present && metadata.present &&
            (!metadata.name.empty() || !metadata.description.empty())) {
            state.sentinel_index = static_cast<int>(index);
            break;
        }
    }
    if (state.sentinel_index < 0) {
        for (std::size_t index = 0; index < state.catalog->traits.size(); ++index) {
            if (state.catalog->traits[index].present) {
                state.sentinel_index = static_cast<int>(index);
                break;
            }
        }
    }
    published_texts.store(next, std::memory_order_release);
    saved_trait_texts.store(next, std::memory_order_release);
    assignment_trace("catalog_published entry204=%d\n", next->entries[203].present);
    state.status = TraitRuntimeStatus::WaitingForTable;
    return true;
}

bool ensure_trait_runtime_name_hook(TraitRuntimeNative& state, std::uintptr_t module_base) {
    const std::lock_guard<std::recursive_mutex> lock(state.table_mutex);

    TraitRuntimeNative* owner = control_owner.load(std::memory_order_acquire);

    if (!module_base || module_base > std::numeric_limits<std::uintptr_t>::max() - MAX_MODULE_OFFSET ||
        (state.module_base && state.module_base != module_base)) {
        state.status = TraitRuntimeStatus::UnsupportedVersion;
        return false;
    }
    if (owner && owner != &state) {
        state.status = TraitRuntimeStatus::Conflict;
        return false;
    }
    if (state.stopping) {
        state.status = TraitRuntimeStatus::CallbackBusy;
        return false;
    }
    if (!platform::windows::retain_callback_module(reinterpret_cast<std::uintptr_t>(&name_getter))) {
        state.status = TraitRuntimeStatus::HookFailed;
        return false;
    }
    if (state.created_hooks == 0) {
        const std::uintptr_t target = module_base + HOOK_SPECS[0].offset;

        if (!executable_address(target)) {
            state.status = TraitRuntimeStatus::UnsupportedVersion;
            return false;
        }

        const std::expected<platform::windows::Hook, int> hook =
            platform::windows::create_hook(reinterpret_cast<void*>(target), HOOK_SPECS[0].detour);

        if (!hook) {
            state.status = TraitRuntimeStatus::HookFailed;
            return false;
        }
        state.hooks[0] = *hook;
        state.created_hooks = 1;
        originals[0].store(hook->original, std::memory_order_release);
    }
    state.module_base = module_base;
    control_owner.store(&state, std::memory_order_release);
    published_module.store(module_base, std::memory_order_release);
    callback_gate.fetch_and(CALLBACK_COUNT, std::memory_order_release);

    const MH_STATUS result = MH_EnableHook(state.hooks[0].target);

    if (result != MH_OK && result != MH_ERROR_ENABLED) {
        state.status = TraitRuntimeStatus::HookFailed;
        return false;
    }
    return true;
}

bool tick_trait_runtime_native(TraitRuntimeNative& state, std::uintptr_t game_root,
    std::uint64_t epoch, std::uint64_t real_milliseconds) {
    const std::lock_guard<std::recursive_mutex> lock(state.table_mutex);

    if (!state.catalog || !state.hooks_enabled || state.stopping || state.table_uncertain) {
        return false;
    }
    if (!prepare_records(state, game_root, epoch, real_milliseconds, false)) {
        return false;
    }
    if (!state.eligibility.applied && !set_native_patch_group(state.eligibility, true)) {
        state.status = TraitRuntimeStatus::MemoryFailed;
        state.pending = true;
        return false;
    }
    published_root.store(game_root, std::memory_order_release);
    sync_descriptions(state);
    state.status = TraitRuntimeStatus::Applied;
    return true;
}

bool trait_runtime_callbacks_idle() {
    return (callback_gate.load(std::memory_order_acquire) & CALLBACK_COUNT) == 0;
}

bool stop_trait_runtime_native(TraitRuntimeNative& state, std::uintptr_t game_root, std::uint64_t epoch) {
    const std::lock_guard<std::recursive_mutex> lock(state.table_mutex);

    saved_trait_texts.store(nullptr, std::memory_order_release);
    for (void* target : {state.saved_trait_filter.target, state.scenario_trait_filter.target}) {
        if (!target) {
            continue;
        }
        const MH_STATUS result = MH_DisableHook(target);

        if (result != MH_OK && result != MH_ERROR_DISABLED) {
            state.status = TraitRuntimeStatus::HookFailed;
            return false;
        }
    }
    state.stopping = true;
    published_root.store(0, std::memory_order_release);
    callback_gate.fetch_or(CALLBACK_CLOSED, std::memory_order_acq_rel);
    if (!trait_runtime_callbacks_idle()) {
        state.status = TraitRuntimeStatus::CallbackBusy;
        return false;
    }
    if ((state.eligibility.applied || state.eligibility.uncertain) && !set_native_patch_group(state.eligibility, false)) {
        state.status = TraitRuntimeStatus::MemoryFailed;
        return false;
    }
    if ((state.table_applied || state.table_uncertain) && state.game_root == game_root && state.epoch == epoch) {
        std::uintptr_t current_table = 0;
        TraitRuntimeNative::Records current{};

        if (!read_value(game_root + state.trait_pointer_offset, current_table) || current_table != state.table ||
            !read_value(state.table + TraitRuntimeNative::RECORD_SIZE, current) ||
            !owned_record_bytes(current, state.original, state.replacement,
                state.table_uncertain ? &state.uncertain_previous : nullptr)) {
            state.status = TraitRuntimeStatus::Conflict;
            return false;
        }
        if (!platform::windows::write_data_memory(state.table + TraitRuntimeNative::RECORD_SIZE,
                std::as_bytes(std::span(state.original)))) {
            state.status = TraitRuntimeStatus::MemoryFailed;
            return false;
        }
    }
    state.table_applied = false;
    state.table_uncertain = false;
    if (!restore_descriptions(state)) {
        return false;
    }
    published_texts.store(nullptr, std::memory_order_release);
    if (!disable_hooks(state)) {
        state.status = TraitRuntimeStatus::HookFailed;
        return false;
    }
    state.status = TraitRuntimeStatus::Disabled;
    return true;
}

} // namespace s8::game
