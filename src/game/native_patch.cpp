#include "native_patch.h"
#include "../platform/windows/process_memory.h"

#include <algorithm>
#include <cstring>
#include <limits>

namespace s8::game {

bool native_bytes_match(std::uintptr_t address, std::span<const std::byte> expected) {
    std::array<std::byte, platform::windows::CODE_PATCH_BYTE_CAPACITY> current{};

    if (expected.empty() || expected.size() > current.size() ||
        !platform::windows::read_memory(address, std::span(current.data(), expected.size()))) {
        return false;
    }
    return std::equal(expected.begin(), expected.end(), current.begin());
}

bool append_native_patch(NativePatchGroup& group, std::uintptr_t address,
    std::span<const std::byte> original, std::span<const std::byte> replacement) {
    if (group.applied || group.uncertain || group.count == group.entries.size() || !address ||
        original.empty() || original.size() != replacement.size() ||
        original.size() > platform::windows::CODE_PATCH_BYTE_CAPACITY) {
        return false;
    }

    NativePatch& entry = group.entries[group.count];

    entry.address = address;
    entry.size = original.size();
    std::copy(original.begin(), original.end(), entry.original.begin());
    std::copy(replacement.begin(), replacement.end(), entry.replacement.begin());
    ++group.count;
    return true;
}

bool put_native_rel32(std::span<std::byte> code, std::size_t offset,
    std::uintptr_t field_address, std::uintptr_t destination) {
    if (offset > code.size() || sizeof(std::int32_t) > code.size() - offset ||
        field_address > std::numeric_limits<std::uintptr_t>::max() - sizeof(std::int32_t)) {
        return false;
    }

    const std::uintptr_t next = field_address + sizeof(std::int32_t);
    std::int32_t displacement = 0;

    if (destination >= next) {
        const std::uintptr_t distance = destination - next;

        if (distance > static_cast<std::uintptr_t>(std::numeric_limits<std::int32_t>::max())) {
            return false;
        }
        displacement = static_cast<std::int32_t>(distance);
    } else {
        const std::uintptr_t distance = next - destination;
        constexpr std::uintptr_t NEGATIVE_LIMIT =
            static_cast<std::uintptr_t>(std::numeric_limits<std::int32_t>::max()) + 1;

        if (distance > NEGATIVE_LIMIT) {
            return false;
        }
        displacement = static_cast<std::int32_t>(-static_cast<std::int64_t>(distance));
    }
    std::memcpy(code.data() + offset, &displacement, sizeof(displacement));
    return true;
}

bool put_native_pointer(std::span<std::byte> code, std::size_t offset, std::uintptr_t value) {
    if (offset > code.size() || sizeof(value) > code.size() - offset) {
        return false;
    }
    std::memcpy(code.data() + offset, &value, sizeof(value));
    return true;
}

bool append_native_branch(NativePatchGroup& group, std::uintptr_t address,
    std::uintptr_t destination, std::span<const std::byte> original, std::byte opcode) {
    std::array<std::byte, platform::windows::CODE_PATCH_BYTE_CAPACITY> branch{};

    if (original.size() < 5 || original.size() > branch.size()) {
        return false;
    }
    std::fill(branch.begin(), branch.end(), std::byte{0x90});
    branch[0] = opcode;
    if (!put_native_rel32(branch, 1, address + 1, destination)) {
        return false;
    }
    return append_native_patch(group, address, original,
        std::span<const std::byte>(branch.data(), original.size()));
}

bool append_native_absolute_branch(NativePatchGroup& group, std::uintptr_t address,
    std::uintptr_t destination, std::span<const std::byte> original) {
    std::array<std::byte, platform::windows::CODE_PATCH_BYTE_CAPACITY> branch{};

    if (original.size() < 14 || original.size() > branch.size()) {
        return false;
    }
    std::fill(branch.begin(), branch.end(), std::byte{0x90});
    branch[0] = std::byte{0xff};
    branch[1] = std::byte{0x25};
    std::fill(branch.begin() + 2, branch.begin() + 6, std::byte{0});
    if (!put_native_pointer(branch, 6, destination)) {
        return false;
    }
    return append_native_patch(group, address, original,
        std::span<const std::byte>(branch.data(), original.size()));
}

std::size_t replace_native_marker(std::span<std::byte> code, std::uint64_t marker, std::uintptr_t value) {
    std::size_t count = 0;

    for (std::size_t offset = 0; offset <= code.size() && sizeof(marker) <= code.size() - offset;) {
        std::uint64_t current = 0;

        std::memcpy(&current, code.data() + offset, sizeof(current));
        if (current == marker) {
            put_native_pointer(code, offset, value);
            ++count;
            offset += sizeof(marker);
        } else {
            ++offset;
        }
    }
    return count;
}

bool set_native_patch_group(NativePatchGroup& group, bool enabled) {
    if (group.count == 0) {
        return !enabled;
    }

    std::array<platform::windows::CodePatch, platform::windows::CODE_PATCH_CAPACITY> patches{};

    for (std::size_t index = 0; index < group.count; ++index) {
        const NativePatch& entry = group.entries[index];
        const std::span<const std::byte> original(entry.original.data(), entry.size);
        const std::span<const std::byte> replacement(entry.replacement.data(), entry.size);

        patches[index] = {entry.address, enabled ? original : replacement,
            enabled ? replacement : original};
    }
    group.last_result = platform::windows::commit_code_patches(
        std::span<const platform::windows::CodePatch>(patches.data(), group.count));
    if (platform::windows::code_patch_succeeded(group.last_result)) {
        group.applied = enabled;
        group.uncertain = false;
        return true;
    }
    if (group.last_result == platform::windows::CodePatchResult::RollbackFailed ||
        group.last_result == platform::windows::CodePatchResult::ResumeFailed) {
        group.uncertain = true;
    }
    return false;
}

} // namespace s8::game
