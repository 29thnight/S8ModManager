#include "process_memory.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <cstring>

namespace s8::platform::windows {

bool is_committed_memory(std::uintptr_t address, std::size_t size) {
    if (!address || !size || address + size - 1 < address) {
        return false;
    }

    const std::uintptr_t end = address + size - 1;
    std::uintptr_t cursor = address;

    for (;;) {
        MEMORY_BASIC_INFORMATION region{};

        if (VirtualQuery(reinterpret_cast<const void*>(cursor), &region, sizeof(region)) != sizeof(region) ||
            region.State != MEM_COMMIT || (region.Protect & (PAGE_NOACCESS | PAGE_GUARD)) || !region.RegionSize) {
            return false;
        }

        const std::uintptr_t region_start = reinterpret_cast<std::uintptr_t>(region.BaseAddress);
        const std::uintptr_t region_end = region_start + region.RegionSize - 1;

        if (region_end < region_start || region_end < cursor) {
            return false;
        }
        if (end <= region_end) {
            return true;
        }

        cursor = region_end + 1;
    }
}

bool read_memory(std::uintptr_t address, std::span<std::byte> bytes) {
    __try {
        std::memcpy(bytes.data(), reinterpret_cast<const void*>(address), bytes.size());
        return true;
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ||
                GetExceptionCode() == EXCEPTION_IN_PAGE_ERROR
                    ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        return false;
    }
}

bool write_memory(std::uintptr_t address, std::span<const std::byte> bytes) {
    __try {
        std::memcpy(reinterpret_cast<void*>(address), bytes.data(), bytes.size());
        return true;
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ||
                GetExceptionCode() == EXCEPTION_IN_PAGE_ERROR
                    ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        return false;
    }
}

bool write_data_memory(std::uintptr_t address, std::span<const std::byte> bytes) {
    void* target = reinterpret_cast<void*>(address);
    DWORD old_protection = 0;
    if (!VirtualProtect(target, bytes.size(), PAGE_READWRITE, &old_protection))
        return false;

    const bool written = write_memory(address, bytes);
    DWORD ignored = 0;
    const bool protection_restored = VirtualProtect(target, bytes.size(), old_protection, &ignored) != FALSE;

    return written && protection_restored;
}

bool write_code_memory(std::uintptr_t address, std::span<const std::byte> bytes) {
    void* target = reinterpret_cast<void*>(address);
    DWORD old_protection = 0;
    if (!VirtualProtect(target, bytes.size(), PAGE_EXECUTE_READWRITE, &old_protection))
        return false;
    const bool written = write_memory(address, bytes);
    DWORD ignored = 0;
    const bool protection_restored = VirtualProtect(target, bytes.size(), old_protection, &ignored) != FALSE;
    const bool cache_flushed = !written || FlushInstructionCache(GetCurrentProcess(), target, bytes.size()) != FALSE;

    return written && protection_restored && cache_flushed;
}

bool write_specialty_slot(std::uintptr_t slot, std::uintptr_t object) {
    DWORD old_protection = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(slot - 8), 0x14, PAGE_READWRITE, &old_protection)) {
        return false;
    }
    std::uint32_t old_enabled = 0, old_bought = 0;
    std::uintptr_t old_object = 0;
    const bool read = read_memory(slot - 8, std::as_writable_bytes(std::span(&old_enabled, 1))) &&
        read_memory(slot, std::as_writable_bytes(std::span(&old_object, 1))) &&
        read_memory(slot + 8, std::as_writable_bytes(std::span(&old_bought, 1)));
    constexpr std::uint32_t ENABLED = 1, BOUGHT = 0;
    bool written = false;
    if (read) {
        written = write_memory(slot - 8, std::as_bytes(std::span(&ENABLED, 1))) &&
            write_memory(slot, std::as_bytes(std::span(&object, 1))) &&
            write_memory(slot + 8, std::as_bytes(std::span(&BOUGHT, 1)));
        std::uint32_t enabled = 0, bought = 0;
        std::uintptr_t current = 0;
        written = written && read_memory(slot - 8, std::as_writable_bytes(std::span(&enabled, 1))) &&
            read_memory(slot, std::as_writable_bytes(std::span(&current, 1))) &&
            read_memory(slot + 8, std::as_writable_bytes(std::span(&bought, 1))) &&
            enabled == ENABLED && (current & 0x0000ffffffffffffULL) == object && bought == BOUGHT;
        if (!written) {
            write_memory(slot - 8, std::as_bytes(std::span(&old_enabled, 1)));
            write_memory(slot, std::as_bytes(std::span(&old_object, 1)));
            write_memory(slot + 8, std::as_bytes(std::span(&old_bought, 1)));
        }
    }
    DWORD ignored = 0;
    VirtualProtect(reinterpret_cast<void*>(slot - 8), 0x14, old_protection, &ignored);
    return written;
}

} // namespace s8::platform::windows
