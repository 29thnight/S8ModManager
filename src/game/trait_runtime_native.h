#pragma once

#include "native_patch.h"
#include "../domain/ownership.h"
#include "../domain/trait_runtime.h"
#include "../platform/windows/hook.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>

namespace s8::game {

struct NativeTraitTexts;

enum class TraitRuntimeStatus {
    Disabled,
    WaitingForTable,
    Applied,
    UnsupportedVersion,
    InvalidText,
    AllocationFailed,
    HookFailed,
    MemoryFailed,
    Conflict,
    CallbackBusy
};

struct TraitDescriptionPatch {
    std::uintptr_t address = 0;
    std::uint32_t original = 0;
    std::uint32_t replacement = 0;
    std::uint32_t previous = 0;
};

struct TraitRuntimeNative {
    static constexpr std::size_t HOOK_COUNT = 12;
    static constexpr std::size_t RECORD_SIZE = 0x40;
    static constexpr std::size_t DESCRIPTION_COUNT = 260;
    using Records = std::array<std::array<std::byte, RECORD_SIZE>, domain::TRAIT_RUNTIME_COUNT>;

    // Shared by control updates and native save/load callbacks.
    std::recursive_mutex table_mutex;
    own::shared_owner<const domain::TraitRuntimeCatalog> catalog{};
    std::array<platform::windows::Hook, HOOK_COUNT> hooks{};
    platform::windows::Hook saved_trait_filter{};
    platform::windows::Hook scenario_trait_filter{};
    std::size_t created_hooks = 0;
    NativePatchGroup eligibility{};
    NativeTraitTexts* texts = nullptr;
    std::uintptr_t module_base = 0;
    std::uintptr_t game_root = 0;
    std::uintptr_t table = 0;
    std::uint64_t epoch = 0;
    std::uint64_t last_tick_ms = 0;
    std::uint32_t trait_pointer_offset = 0x57A4D0;
    int sentinel_index = -1;
    Records original{};
    Records replacement{};
    Records uncertain_previous{};
    std::array<TraitDescriptionPatch, DESCRIPTION_COUNT> descriptions{};
    std::size_t description_count = 0;
    std::uintptr_t description_table = 0;
    std::uintptr_t retained_description_storage = 0;
    std::uintptr_t description_candidate = 0;
    std::uintptr_t description_candidate_table = 0;
    const NativeTraitTexts* description_candidate_texts = nullptr;
    bool description_candidate_exposed = false;
    bool description_uncertain = false;
    const NativeTraitTexts* description_texts = nullptr;
    bool hooks_enabled = false;
    bool table_applied = false;
    bool table_uncertain = false;
    bool pending = false;
    bool stopping = false;
    TraitRuntimeStatus status = TraitRuntimeStatus::Disabled;

    TraitRuntimeNative() = default;
    TraitRuntimeNative(const TraitRuntimeNative&) = delete;
    TraitRuntimeNative& operator=(const TraitRuntimeNative&) = delete;
};

// Configuration publication and hook installation are control operations, never
// Present work. MinHook must be initialized by the lifecycle owner first.
// The caller closes its command admission for final shutdown; catalog removal
// retries finish a previous stop and may subsequently activate a new catalog.
bool set_trait_runtime_catalog(TraitRuntimeNative& state, std::uintptr_t module_base,
    own::shared_owner<const domain::TraitRuntimeCatalog> catalog);

// Legacy name editing also works when no extended-trait catalog is installed.
// This uses the same first hook and publication gateway as the full runtime.
bool ensure_trait_runtime_name_hook(TraitRuntimeNative& state, std::uintptr_t module_base);

// Call at the admitted game boundary. The caller supplies real elapsed time;
// the 500 ms check is unaffected by SpeedHack's game-facing clock hooks.
// game_root is read_game_base's result (the GetGameBase signature), not DataCenter.
bool tick_trait_runtime_native(TraitRuntimeNative& state, std::uintptr_t game_root,
    std::uint64_t epoch, std::uint64_t real_milliseconds);

// Stop closes admission before disabling detours. A false/CallbackBusy result
// requires a later control-boundary retry; disabled trampolines stay retained.
bool stop_trait_runtime_native(TraitRuntimeNative& state, std::uintptr_t game_root, std::uint64_t epoch);
bool trait_runtime_callbacks_idle();

} // namespace s8::game
