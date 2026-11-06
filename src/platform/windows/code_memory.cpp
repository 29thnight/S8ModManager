#include "code_memory.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <algorithm>
#include <limits>

namespace s8::platform::windows {

std::uintptr_t allocate_code_near(std::uintptr_t target, std::size_t size) {
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    const auto alignment = static_cast<std::uintptr_t>(info.dwAllocationGranularity);
    constexpr std::uintptr_t DISTANCE = 0x70000000;
    const auto low = target > DISTANCE ? target - DISTANCE : 0x10000;
    const auto max = std::numeric_limits<std::uintptr_t>::max();
    const auto high = target <= max - DISTANCE ? target + DISTANCE : max;

    const auto allocate = [&](std::uintptr_t begin, std::uintptr_t end, bool descending) -> std::uintptr_t {
        if (end <= begin || end - begin < size)
            return 0;
        std::uintptr_t candidate = 0;
        if (descending) {
            candidate = (end - size) & ~(alignment - 1);
            if (candidate < begin)
                return 0;
        } else {
            if (begin > max - (alignment - 1))
                return 0;
            candidate = (begin + alignment - 1) & ~(alignment - 1);
            if (candidate < begin || candidate > end - size)
                return 0;
        }
        return reinterpret_cast<std::uintptr_t>(VirtualAlloc(reinterpret_cast<void*>(candidate), size,
            MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    };

    auto cursor = target;
    while (cursor < high) {
        MEMORY_BASIC_INFORMATION region{};
        if (!VirtualQuery(reinterpret_cast<const void*>(cursor), &region, sizeof(region)))
            break;
        const auto begin = reinterpret_cast<std::uintptr_t>(region.BaseAddress);
        const auto end = begin + region.RegionSize;
        if (end <= cursor)
            break;
        if (region.State == MEM_FREE) {
            const auto result = allocate(std::max(begin, low), std::min(end, high), false);
            if (result)
                return result;
        }
        cursor = end;
    }
    cursor = target;
    while (cursor > low) {
        MEMORY_BASIC_INFORMATION region{};
        if (!VirtualQuery(reinterpret_cast<const void*>(cursor - 1), &region, sizeof(region)))
            break;
        const auto begin = reinterpret_cast<std::uintptr_t>(region.BaseAddress);
        const auto end = begin + region.RegionSize;
        if (end <= begin)
            break;
        if (region.State == MEM_FREE) {
            const auto result = allocate(std::max(begin, low), std::min(end, high), true);
            if (result)
                return result;
        }
        if (begin <= low)
            break;
        cursor = begin;
    }
    return 0;
}

void free_code(std::uintptr_t address) {
    VirtualFree(reinterpret_cast<void*>(address), 0, MEM_RELEASE);
}

bool retain_callback_module(std::uintptr_t callback_address) {
    HMODULE module = nullptr;

    return callback_address && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
        GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCWSTR>(callback_address), &module) != FALSE;
}

} // namespace s8::platform::windows
