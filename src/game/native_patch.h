#pragma once

#include "../platform/windows/code_patch.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace s8::game {

template<std::size_t N>
consteval std::array<std::byte, (N - 1) / 2> native_hex(const char (&text)[N]) {
    static_assert((N - 1) % 2 == 0);
    std::array<std::byte, (N - 1) / 2> bytes{};

    for (std::size_t index = 0; index < N - 1; ++index) {
        const char digit = text[index];
        unsigned int value = 0;

        if (digit >= '0' && digit <= '9') {
            value = static_cast<unsigned int>(digit - '0');
        } else if (digit >= 'A' && digit <= 'F') {
            value = static_cast<unsigned int>(digit - 'A' + 10);
        } else if (digit >= 'a' && digit <= 'f') {
            value = static_cast<unsigned int>(digit - 'a' + 10);
        } else {
            throw "Invalid native code hex digit";
        }
        if (index % 2 == 0) {
            bytes[index / 2] = static_cast<std::byte>(value << 4);
        } else {
            bytes[index / 2] |= static_cast<std::byte>(value);
        }
    }
    return bytes;
}

struct NativePatch {
    std::uintptr_t address = 0;
    std::size_t size = 0;
    std::array<std::byte, platform::windows::CODE_PATCH_BYTE_CAPACITY> original{};
    std::array<std::byte, platform::windows::CODE_PATCH_BYTE_CAPACITY> replacement{};
};

struct NativePatchGroup {
    std::array<NativePatch, platform::windows::CODE_PATCH_CAPACITY> entries{};
    std::size_t count = 0;
    bool applied = false;
    bool uncertain = false;
    platform::windows::CodePatchResult last_result = platform::windows::CodePatchResult::AlreadyApplied;
};

bool native_bytes_match(std::uintptr_t address, std::span<const std::byte> expected);
bool append_native_patch(NativePatchGroup& group, std::uintptr_t address,
    std::span<const std::byte> original, std::span<const std::byte> replacement);
bool append_native_branch(NativePatchGroup& group, std::uintptr_t address,
    std::uintptr_t destination, std::span<const std::byte> original, std::byte opcode = std::byte{0xe9});
bool append_native_absolute_branch(NativePatchGroup& group, std::uintptr_t address,
    std::uintptr_t destination, std::span<const std::byte> original);
bool set_native_patch_group(NativePatchGroup& group, bool enabled);
bool put_native_rel32(std::span<std::byte> code, std::size_t offset,
    std::uintptr_t field_address, std::uintptr_t destination);
bool put_native_pointer(std::span<std::byte> code, std::size_t offset, std::uintptr_t value);
std::size_t replace_native_marker(std::span<std::byte> code, std::uint64_t marker, std::uintptr_t value);

} // namespace s8::game
