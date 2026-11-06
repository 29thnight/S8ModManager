#include "secondary_jewel_hook.h"

#include "../platform/windows/hook.h"
#include "../platform/windows/process_memory.h"
#include "../../third_party/minhook/include/MinHook.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <array>
#include <atomic>
#include <span>

namespace s8::game {
namespace {

using Judge = bool (__fastcall*)(std::uint16_t);
std::array<std::atomic<std::uint64_t>, 8> selected{};
std::atomic<std::uintptr_t> game_base_address{0};
std::atomic<std::uintptr_t> cache_base{0};
std::array<std::atomic<std::uint8_t>, 501> cache{};
std::array<std::atomic<std::uint64_t>, 501> last_probe{};
platform::windows::Hook hook{};
bool installed = false;

bool __fastcall secondary_jewel_detour(std::uint16_t id) {
    if (id <= 500 && (selected[id >> 6].load(std::memory_order_relaxed) & (std::uint64_t{1} << (id & 63)))) {
        std::uintptr_t game_base = 0;
        const auto address = game_base_address.load(std::memory_order_acquire);
        if (address && platform::windows::read_memory(address, std::as_writable_bytes(std::span(&game_base, 1))) && game_base) {
            const auto raw_id = static_cast<std::uint16_t>(id + 0x40);
            const auto byte_index = raw_id >> 3;
            std::uint8_t bitmap = 0;
            if (byte_index < 34 && platform::windows::read_memory(game_base + 0x71e0 + byte_index,
                    std::as_writable_bytes(std::span(&bitmap, 1))) && (bitmap & (1u << (raw_id & 7)))) {
                if (cache_base.exchange(game_base, std::memory_order_acq_rel) != game_base) {
                    reset_secondary_jewel_cache();
                    cache_base.store(game_base, std::memory_order_release);
                }
                auto status = cache[id].load(std::memory_order_relaxed);
                const auto now = GetTickCount64();
                if (status == 0 || (status == 1 && now - last_probe[id].load(std::memory_order_relaxed) >= 250)) {
                    std::uintptr_t first = 0, second = 0;
                    const auto slot = game_base + sizeof(std::uintptr_t) * (static_cast<std::uintptr_t>(id) + 0xb11de);
                    const bool available = platform::windows::read_memory(slot, std::as_writable_bytes(std::span(&first, 1))) &&
                        first && platform::windows::read_memory(first, std::as_writable_bytes(std::span(&second, 1))) && second;
                    last_probe[id].store(now, std::memory_order_relaxed);
                    status = available ? 2 : 1;
                    cache[id].store(status, std::memory_order_relaxed);
                }
                if (status == 2) {
                    return true;
                }
            }
        }
    }
    return reinterpret_cast<Judge>(hook.original)(id);
}

} // namespace

void reset_secondary_jewel_cache() {
    for (auto& value : cache) {
        value.store(0, std::memory_order_relaxed);
    }
    for (auto& value : last_probe) {
        value.store(0, std::memory_order_relaxed);
    }
}

bool set_secondary_jewels(const domain::SecondaryJewelMask& mask, std::uintptr_t module_base,
                          std::uintptr_t game_base_pointer) {
    bool any = false;
    for (const auto word : mask.words) {
        any = any || word != 0;
    }
    if (!installed) {
        if (!module_base) {
            return false;
        }
        const auto target = module_base + 0x17abbf0;
        constexpr std::array EXPECTED{std::byte{0x48}, std::byte{0x89}, std::byte{0x5c}, std::byte{0x24}, std::byte{0x08}};
        std::array<std::byte, 5> current{};
        if (!platform::windows::read_memory(target, current)) {
            return false;
        }
        if (current != EXPECTED) {
            return false;
        }
        if (!any) {
            return true;
        }
        const auto initialized = platform::windows::initialize_hooks();
        if (initialized != MH_OK && initialized != MH_ERROR_ALREADY_INITIALIZED) {
            return false;
        }
        const auto created = platform::windows::create_hook(reinterpret_cast<void*>(target),
                                                             reinterpret_cast<void*>(&secondary_jewel_detour));
        if (!created) {
            return false;
        }
        hook = *created;
        const auto status = platform::windows::enable_hook(hook);
        if (status != MH_OK && status != MH_ERROR_ENABLED) {
            platform::windows::remove_hook(hook);
            hook = {};
            return false;
        }
        installed = true;
    }
    game_base_address.store(game_base_pointer, std::memory_order_release);
    reset_secondary_jewel_cache();
    cache_base.store(0, std::memory_order_release);
    for (std::size_t i = 0; i < selected.size(); ++i) {
        selected[i].store(mask.words[i], std::memory_order_release);
    }
    return true;
}

} // namespace s8::game
