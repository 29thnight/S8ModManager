#include "secondary_jewel.h"

namespace s8::domain {

SecondaryJewelMask all_secondary_jewels() {
    SecondaryJewelMask result;
    for (const auto id : SECONDARY_JEWEL_IDS) {
        result.words[id >> 6] |= std::uint64_t{1} << (id & 63);
    }
    return result;
}

bool secondary_jewel_selected(const SecondaryJewelMask& mask, std::uint16_t id) {
    return id < 512 && (mask.words[id >> 6] & (std::uint64_t{1} << (id & 63))) != 0;
}

std::string secondary_jewel_mask_hex(const SecondaryJewelMask& mask) {
    constexpr char DIGITS[] = "0123456789ABCDEF";
    std::string result;
    result.reserve(128);
    for (const auto word : mask.words) {
        for (int shift = 60; shift >= 0; shift -= 4) {
            result.push_back(DIGITS[(word >> shift) & 0x0f]);
        }
    }
    return result;
}

std::optional<SecondaryJewelMask> parse_secondary_jewel_mask(std::string_view text) {
    if (text.size() != 128) {
        return std::nullopt;
    }
    SecondaryJewelMask parsed;
    for (std::size_t word = 0; word < parsed.words.size(); ++word) {
        for (std::size_t digit = 0; digit < 16; ++digit) {
            const char c = text[word * 16 + digit];
            int value = -1;
            if (c >= '0' && c <= '9') {
                value = c - '0';
            } else if (c >= 'A' && c <= 'F') {
                value = c - 'A' + 10;
            } else if (c >= 'a' && c <= 'f') {
                value = c - 'a' + 10;
            }
            if (value < 0) {
                return std::nullopt;
            }
            parsed.words[word] = (parsed.words[word] << 4) | static_cast<std::uint64_t>(value);
        }
    }
    const auto allowed = all_secondary_jewels();
    for (std::size_t i = 0; i < parsed.words.size(); ++i) {
        parsed.words[i] &= allowed.words[i];
    }
    return parsed;
}

} // namespace s8::domain
