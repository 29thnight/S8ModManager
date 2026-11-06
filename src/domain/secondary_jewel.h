#pragma once

#include "secondary_jewel_ids.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace s8::domain {

struct SecondaryJewelMask {
    std::array<std::uint64_t, 8> words{};
};

SecondaryJewelMask all_secondary_jewels();
bool secondary_jewel_selected(const SecondaryJewelMask& mask, std::uint16_t id);
std::string secondary_jewel_mask_hex(const SecondaryJewelMask& mask);
std::optional<SecondaryJewelMask> parse_secondary_jewel_mask(std::string_view text);

} // namespace s8::domain
