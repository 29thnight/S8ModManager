#include "engine_services.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <Windows.h>

#include <array>
#include <cstring>

namespace s8::scripting {
  std::int64_t invoke_engine(Operation operation, std::uint64_t target, std::int64_t argument, void *data,
                             std::uint32_t bytes, std::uint64_t session) {
    const auto module = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));

    if (operation == Operation::GetGemCategory) {
      if (target >= 512)
        return -1;
      std::uintptr_t root = 0, record = 0;
      std::uint8_t category = 0;
      SIZE_T copied = 0;
      if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void *>(module + 0x2E98BC8), &root, sizeof(root),
                             &copied) ||
          !root ||
          !ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void *>(root + 0x588EF0 + target * 8), &record,
                             sizeof(record), &copied) ||
          !record ||
          !ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void *>(record + 0xC), &category, sizeof(category),
                             &copied))
        return -8;
      return category;
    }

    if (operation >= Operation::GetPersonality && operation <= Operation::ReadPersonalityDescription) {
      if (!data || target < 1 || target > 254)
        return -1;

      std::uintptr_t root = 0, record = 0;
      SIZE_T copied = 0;

      if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void *>(module + 0x2E98BC8), &root, sizeof(root),
                             &copied) ||
          !root ||
          !ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void *>(root + 0x57A4D0 + target * 8), &record,
                             sizeof(record), &copied))
        return -8;

      if (!record)
        return operation == Operation::GetPersonality ? 0 : -8;

      std::array<std::byte, 0x40> image{};

      if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void *>(record), image.data(), image.size(),
                             &copied))
        return operation == Operation::GetPersonality ? 0 : -8;

      std::uint16_t id = 0;
      std::memcpy(&id, image.data() + 8, 2);

      if (id != target)
        return operation == Operation::GetPersonality ? 0 : -8;

      if (operation == Operation::GetPersonality) {
        if (bytes != sizeof(scripting::Personality))
          return -1;

        auto &output = *static_cast<scripting::Personality *>(data);
        output.memory = {record, session};
        output.id = id;

        std::memcpy(&output.voice, image.data() + 0x30, 2);
        std::memcpy(output.effects, image.data() + 0xA, 36);
        return 1;
      }

      if (bytes < 2 || bytes > 8192 || bytes % 2)
        return -1;

      // Existing game text hooks participate just as they do for game callers.
      const wchar_t *text =
          operation == Operation::ReadPersonalityName
              ? reinterpret_cast<const wchar_t *(*)(void *)>(module + 0x170E2D0)(reinterpret_cast<void *>(record))
              : reinterpret_cast<const wchar_t *(*)(void *)>(module + 0x170ABB0)(reinterpret_cast<void *>(record));

      if (!text)
        return -8;

      // Returned game string is external data; copy bounded units once here.
      auto *output = static_cast<wchar_t *>(data);

      for (std::uint32_t i = 0; i < bytes / 2; ++i) {
        if (!ReadProcessMemory(GetCurrentProcess(), text + i, output + i, 2, &copied))
          return -8;

        if (!output[i])
          return i;
      }

      return -1;
    }

    if (operation >= Operation::InspectMemory && operation <= Operation::WriteMemory) {
      if (argument < 0 || static_cast<std::uint64_t>(argument) != session)
        return -10;

      if (!data || !bytes || bytes > 65536 || target < 0x10000 || target > UINTPTR_MAX - bytes)
        return -9;

      const bool writing = operation == Operation::WriteMemory;
      MEMORY_BASIC_INFORMATION first{}, page{};
      std::uint64_t address = target;

      while (address < target + bytes) {
        if (!VirtualQuery(reinterpret_cast<void *>(address), &page, sizeof(page)) || page.State != MEM_COMMIT ||
            (page.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
          return -9;

        const DWORD access = page.Protect & 0xFF;
        const bool writable = access == PAGE_READWRITE || access == PAGE_WRITECOPY;
        const bool readable = writable || access == PAGE_READONLY || access == PAGE_EXECUTE_READ ||
                              access == PAGE_EXECUTE_READWRITE || access == PAGE_EXECUTE_WRITECOPY;

        if (!readable || (writing && !writable))
          return -9;

        if (address == target)
          first = page;

        const auto end = reinterpret_cast<std::uintptr_t>(page.BaseAddress) + page.RegionSize;

        if (end <= address)
          return -9;

        address = end;
      }

      if (operation == Operation::InspectMemory) {
        *static_cast<scripting::MemoryRegion *>(data) = {
            reinterpret_cast<std::uintptr_t>(first.BaseAddress), first.RegionSize, first.Protect,
            (first.Protect & 0xFF) == PAGE_READWRITE || (first.Protect & 0xFF) == PAGE_WRITECOPY ? 1u : 0u};
        return 0;
      }

      SIZE_T copied = 0;
      const BOOL result =
          writing ? WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<void *>(target), data, bytes, &copied)
                  : ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void *>(target), data, bytes, &copied);
      return result && copied == bytes ? static_cast<std::int64_t>(copied) : -9;
    }

    return -1;
  }
} // namespace s8::scripting
