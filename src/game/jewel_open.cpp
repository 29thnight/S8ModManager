#include "jewel_open.h"
#include "secondary_jewel_hook.h"
#include "../domain/secondary_jewel_definitions.h"
#include <algorithm>
#include "../platform/windows/process_memory.h"

#include <array>
#include <span>

namespace s8::game {
namespace {

constexpr std::array<std::uint8_t, 34> MASK{
    0,0,0,0,0,0,0,0, 0xfc,0xff,0xff,0xbf,0xff,0xff,0xf7,0xff,
    0xff,0xfe,0xff,0xdf,0xff,0xff,0xfb,0xff, 0x7f,0xff,0xff,0xef,0xff,0xff,0xff,0xff, 0x03,0};

bool apply(std::uintptr_t game_base, bool enabled, bool& changed) {
    changed = false;
    if (!game_base) {
        return false;
    }
    std::array<std::uint8_t, 34> bitmap{};
    const auto address = game_base + 0x71e0;
    if (!platform::windows::read_memory(address, std::as_writable_bytes(std::span(bitmap)))) {
        return false;
    }
    for (std::size_t i = 0; i < MASK.size(); ++i) {
        const auto value = static_cast<std::uint8_t>(enabled ? bitmap[i] | MASK[i] : bitmap[i] & ~MASK[i]);
        if (value != bitmap[i]) {
            if (!platform::windows::write_memory(address + i, std::as_bytes(std::span(&value, 1)))) {
                return false;
            }
            changed = true;
        }
    }
    return true;
}

} // namespace

bool set_jewel_open(JewelOpen& state, std::uintptr_t game_base, std::uint16_t raw_id, bool open) {
    const auto found = std::find_if(domain::SECONDARY_JEWEL_DEFINITIONS.begin(), domain::SECONDARY_JEWEL_DEFINITIONS.end(),
        [raw_id](const domain::SecondaryJewelDefinition& definition) { return definition.raw_id == raw_id; });
    if (!game_base || found == domain::SECONDARY_JEWEL_DEFINITIONS.end()) return false;
    const std::size_t byte = raw_id >> 3;
    const std::uint8_t bit = static_cast<std::uint8_t>(1u << (raw_id & 7));
    std::uint8_t value = 0;
    const std::uintptr_t address = game_base + 0x71e0 + byte;
    if (!platform::windows::read_memory(address, std::as_writable_bytes(std::span(&value, 1)))) return false;
    value = open ? value | bit : value & ~bit;
    if (!platform::windows::write_memory(address, std::as_bytes(std::span(&value, 1)))) return false;

    // An individual edit takes precedence over the repeating full-unlock setting.
    // Keep every other jewel bit as it is instead of clearing the whole bitmap.
    state.preferred = false;
    if (state.bitmap) (*state.bitmap)[byte] = value;
    reset_secondary_jewel_cache();
    return true;
}

bool set_all_jewels_open(JewelOpen& state, std::uintptr_t game_base, bool enabled) {
    state.preferred = enabled;
    bool changed = false;
    const bool applied = apply(game_base, enabled, changed);
    if (applied && enabled) {
        reset_secondary_jewel_cache();
    }
    return applied;
}

void update_jewel_open(JewelOpen& state, std::uintptr_t game_base, std::uint64_t now_ms) {
    if (now_ms - state.last_check_ms < 250) {
        return;
    }
    state.last_check_ms = now_ms;
    if (state.preferred) {
        bool changed = false;
        if (apply(game_base, true, changed) && changed) {
            reset_secondary_jewel_cache();
        }
    }
    std::array<std::uint8_t, 34> bitmap{};
    if (game_base && platform::windows::read_memory(game_base + 0x71e0, std::as_writable_bytes(std::span(bitmap)))) {
        state.bitmap = bitmap;
    } else {
        state.bitmap.reset();
    }
}

} // namespace s8::game
