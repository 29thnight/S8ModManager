#include "native_ui.h"
#include "../game/jewel_open.h"
#include "../game/secondary_jewel_hook.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <algorithm>
#include <mutex>

// Jewel edits are applied by the mod manager itself. The forced-effect mask is
// owned by the C# mod, which persists it and restores it through this API.
namespace s8::scripting {
  namespace {
    constexpr std::uintptr_t game_root_rva = 0x2E98BC8;

    std::mutex jewel_mutex;
    std::array<std::uint64_t, 8> current_mask{};
    game::JewelOpen jewel_open;

    std::uintptr_t module_base() {
      return reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    }

    bool install_mask(const std::array<std::uint64_t, 8> &mask) {
      domain::SecondaryJewelMask value{};
      value.words = mask;
      const auto base = module_base();
      if (!game::set_secondary_jewels(value, base, base + game_root_rva))
        return false;
      current_mask = mask;
      return true;
    }
  } // namespace

  std::array<std::uint64_t, 8> jewel_force_mask() {
    std::lock_guard lock(jewel_mutex);
    return current_mask;
  }

  bool set_jewel_force_mask(const std::array<std::uint64_t, 8> &mask) {
    std::lock_guard lock(jewel_mutex);
    return install_mask(mask);
  }

  bool apply_jewel_command(const UiJewelCommand &command) {
    std::lock_guard lock(jewel_mutex);
    if (command.raw_id) {
      std::uintptr_t game_base = 0;
      SIZE_T copied = 0;
      if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void *>(module_base() + game_root_rva),
                             &game_base, sizeof(game_base), &copied) ||
          !game_base || !game::set_jewel_open(jewel_open, game_base, command.raw_id, command.open))
        return false;
    }

    std::array<std::uint64_t, 8> mask{};
    std::copy(std::begin(command.mask), std::end(command.mask), mask.begin());
    return install_mask(mask);
  }
} // namespace s8::scripting
