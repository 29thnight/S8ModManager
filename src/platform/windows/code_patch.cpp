#include "code_patch.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <TlHelp32.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <limits>

namespace s8::platform::windows {
namespace {

constexpr std::size_t THREAD_CAPACITY = 1024;
std::atomic_flag commit_active = ATOMIC_FLAG_INIT;

struct CommitAdmission {
    ~CommitAdmission() {
        commit_active.clear(std::memory_order_release);
    }
};

struct PreparedPatch {
    std::uintptr_t address = 0;
    std::size_t size = 0;
    std::array<std::byte, CODE_PATCH_BYTE_CAPACITY> original{};
    std::array<std::byte, CODE_PATCH_BYTE_CAPACITY> replacement{};
    DWORD protection = 0;
    bool needs_write = false;
};

struct PeerThread {
    HANDLE handle = nullptr;
    bool suspended = false;
};

struct PeerThreads {
    std::array<PeerThread, THREAD_CAPACITY> entries{};
    std::size_t count = 0;

    bool resume() {
        bool succeeded = true;

        for (std::size_t index = count; index != 0; --index) {
            PeerThread& peer = entries[index - 1];

            if (!peer.suspended) {
                continue;
            }
            if (ResumeThread(peer.handle) != static_cast<DWORD>(-1)) {
                peer.suspended = false;
                continue;
            }

            DWORD exit_code = STILL_ACTIVE;

            if (GetExitCodeThread(peer.handle, &exit_code) && exit_code != STILL_ACTIVE) {
                peer.suspended = false;
            } else {
                succeeded = false;
            }
        }
        return succeeded;
    }

    ~PeerThreads() {
        resume();
        for (std::size_t index = 0; index < count; ++index) {
            CloseHandle(entries[index].handle);
        }
    }
};

bool read_bytes(std::uintptr_t address, std::byte* destination, std::size_t size) {
    SIZE_T copied = 0;

    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address),
        destination, size, &copied) && copied == size;
}

bool write_bytes(const PreparedPatch& patch, const std::byte* source) {
    DWORD previous_protection = 0;

    if (!VirtualProtect(reinterpret_cast<void*>(patch.address), patch.size,
        PAGE_EXECUTE_READWRITE, &previous_protection)) {
        return false;
    }

    SIZE_T copied = 0;
    const bool written = WriteProcessMemory(GetCurrentProcess(),
        reinterpret_cast<void*>(patch.address), source, patch.size, &copied) && copied == patch.size;
    DWORD ignored = 0;
    const bool protected_again = VirtualProtect(reinterpret_cast<void*>(patch.address),
        patch.size, patch.protection, &ignored) != FALSE;
    const bool flushed = FlushInstructionCache(GetCurrentProcess(),
        reinterpret_cast<const void*>(patch.address), patch.size) != FALSE;

    return written && protected_again && flushed;
}

CodePatchResult prepare_threads(PeerThreads& peers) {
    const DWORD process_id = GetCurrentProcessId();
    const DWORD caller_id = GetCurrentThreadId();
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);

    if (snapshot == INVALID_HANDLE_VALUE) {
        return CodePatchResult::ThreadEnumerationFailed;
    }

    THREADENTRY32 entry{};

    entry.dwSize = sizeof(entry);
    if (!Thread32First(snapshot, &entry)) {
        CloseHandle(snapshot);
        return CodePatchResult::ThreadEnumerationFailed;
    }

    CodePatchResult result = CodePatchResult::Applied;

    do {
        if (entry.th32OwnerProcessID != process_id || entry.th32ThreadID == caller_id) {
            continue;
        }
        if (peers.count == peers.entries.size()) {
            result = CodePatchResult::ThreadCapacityExceeded;
            break;
        }

        const HANDLE handle = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
            THREAD_QUERY_INFORMATION, FALSE, entry.th32ThreadID);

        if (!handle) {
            result = CodePatchResult::ThreadAccessFailed;
            break;
        }
        peers.entries[peers.count].handle = handle;
        ++peers.count;
    } while (Thread32Next(snapshot, &entry));

    if (result == CodePatchResult::Applied && GetLastError() != ERROR_NO_MORE_FILES) {
        result = CodePatchResult::ThreadEnumerationFailed;
    }
    CloseHandle(snapshot);
    return result;
}

CodePatchResult suspend_threads(PeerThreads& peers, std::span<const PreparedPatch> patches) {
    for (std::size_t index = 0; index < peers.count; ++index) {
        PeerThread& peer = peers.entries[index];

        if (SuspendThread(peer.handle) == static_cast<DWORD>(-1)) {
            return CodePatchResult::ThreadSuspendFailed;
        }
        peer.suspended = true;

        CONTEXT context{};

        context.ContextFlags = CONTEXT_CONTROL;
        if (!GetThreadContext(peer.handle, &context)) {
            return CodePatchResult::ThreadContextFailed;
        }
        for (const PreparedPatch& patch : patches) {
            if (patch.needs_write && context.Rip >= patch.address &&
                context.Rip - patch.address < patch.size) {
                return CodePatchResult::Busy;
            }
        }
    }
    return CodePatchResult::Applied;
}

CodePatchResult write_transaction(std::span<const PreparedPatch> patches) {
    std::array<std::byte, CODE_PATCH_BYTE_CAPACITY> current{};

    for (const PreparedPatch& patch : patches) {
        if (!read_bytes(patch.address, current.data(), patch.size) ||
            !std::equal(current.begin(), current.begin() + patch.size, patch.original.begin())) {
            return CodePatchResult::ExpectedBytesMismatch;
        }
    }
    for (std::size_t index = 0; index < patches.size(); ++index) {
        const PreparedPatch& patch = patches[index];

        if (!patch.needs_write || write_bytes(patch, patch.replacement.data())) {
            continue;
        }

        bool restored = true;

        // Include the failed write: WriteProcessMemory can have copied a prefix.
        for (std::size_t rollback = index + 1; rollback != 0; --rollback) {
            const PreparedPatch& original = patches[rollback - 1];

            if (original.needs_write && !write_bytes(original, original.original.data())) {
                restored = false;
            }
        }
        return restored ? CodePatchResult::WriteFailed : CodePatchResult::RollbackFailed;
    }
    return CodePatchResult::Applied;
}

} // namespace

CodePatchResult commit_code_patches(std::span<const CodePatch> patches) {
    if (patches.empty() || patches.size() > CODE_PATCH_CAPACITY) {
        return CodePatchResult::InvalidInput;
    }
    if (commit_active.test_and_set(std::memory_order_acquire)) {
        return CodePatchResult::Busy;
    }

    const CommitAdmission admission{};
    std::array<PreparedPatch, CODE_PATCH_CAPACITY> prepared{};
    bool needs_write = false;

    for (std::size_t index = 0; index < patches.size(); ++index) {
        const CodePatch& input = patches[index];
        PreparedPatch& output = prepared[index];

        if (!input.address || input.expected.empty() ||
            input.expected.size() != input.replacement.size() ||
            input.expected.size() > CODE_PATCH_BYTE_CAPACITY ||
            input.address > std::numeric_limits<std::uintptr_t>::max() - input.expected.size()) {
            return CodePatchResult::InvalidInput;
        }
        output.address = input.address;
        output.size = input.expected.size();
        for (std::size_t previous = 0; previous < index; ++previous) {
            const PreparedPatch& other = prepared[previous];

            if (output.address < other.address + other.size &&
                other.address < output.address + output.size) {
                return CodePatchResult::InvalidInput;
            }
        }

        MEMORY_BASIC_INFORMATION region{};

        if (!VirtualQuery(reinterpret_cast<const void*>(output.address), &region, sizeof(region)) ||
            region.State != MEM_COMMIT || (region.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0 ||
            output.address - reinterpret_cast<std::uintptr_t>(region.BaseAddress) > region.RegionSize ||
            output.size > region.RegionSize -
                (output.address - reinterpret_cast<std::uintptr_t>(region.BaseAddress))) {
            return CodePatchResult::InvalidInput;
        }
        output.protection = region.Protect;
        if (!read_bytes(output.address, output.original.data(), output.size)) {
            return CodePatchResult::ExpectedBytesMismatch;
        }
        std::copy(input.replacement.begin(), input.replacement.end(), output.replacement.begin());
        if (std::equal(input.replacement.begin(), input.replacement.end(), output.original.begin())) {
            continue;
        }
        if (!std::equal(input.expected.begin(), input.expected.end(), output.original.begin())) {
            return CodePatchResult::ExpectedBytesMismatch;
        }
        output.needs_write = true;
        needs_write = true;
    }
    if (!needs_write) {
        return CodePatchResult::AlreadyApplied;
    }

    PeerThreads peers{};
    CodePatchResult result = prepare_threads(peers);

    if (result != CodePatchResult::Applied) {
        return result;
    }
    const std::span<const PreparedPatch> transaction(prepared.data(), patches.size());

    result = suspend_threads(peers, transaction);
    if (result == CodePatchResult::Applied) {
        result = write_transaction(transaction);
    }
    if (!peers.resume()) {
        return CodePatchResult::ResumeFailed;
    }
    return result;
}

} // namespace s8::platform::windows
