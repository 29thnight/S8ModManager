#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace s8::platform::windows {

inline constexpr std::size_t CODE_PATCH_CAPACITY = 64;
inline constexpr std::size_t CODE_PATCH_BYTE_CAPACITY = 32;

struct CodePatch {
    std::uintptr_t address = 0;
    std::span<const std::byte> expected{};
    std::span<const std::byte> replacement{};
};

enum class CodePatchResult {
    Applied,
    AlreadyApplied,
    InvalidInput,
    ExpectedBytesMismatch,
    Busy,
    ThreadEnumerationFailed,
    ThreadCapacityExceeded,
    ThreadAccessFailed,
    ThreadSuspendFailed,
    ThreadContextFailed,
    WriteFailed,
    RollbackFailed,
    ResumeFailed
};

constexpr bool code_patch_succeeded(CodePatchResult result) {
    return result == CodePatchResult::Applied || result == CodePatchResult::AlreadyApplied;
}

// Only call from an admitted game commit boundary, never from a recurring tick.
// All handles and bounded storage are prepared before suspending peer threads.
// A peer executing an edited instruction causes Busy; callers retain the request.
// The snapshot cannot prevent a new thread from starting after enumeration, so
// this does not replace the owner's game-thread/lifecycle admission contract.
CodePatchResult commit_code_patches(std::span<const CodePatch> patches);

} // namespace s8::platform::windows
