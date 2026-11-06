#pragma once

#include <cstdint>
#include <array>
#include <optional>

namespace s8::game {

struct JewelOpen {
    bool preferred = false;
    std::uint64_t last_check_ms = 0;
    std::optional<std::array<std::uint8_t, 34>> bitmap;
};

bool set_jewel_open(JewelOpen& state, std::uintptr_t game_base, std::uint16_t raw_id, bool open);
bool set_all_jewels_open(JewelOpen& state, std::uintptr_t game_base, bool enabled);
void update_jewel_open(JewelOpen& state, std::uintptr_t game_base, std::uint64_t now_ms);

} // namespace s8::game
