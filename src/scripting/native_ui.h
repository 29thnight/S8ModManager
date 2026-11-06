#pragma once

#include <array>
#include <cstdint>

namespace s8::scripting {

  inline constexpr std::uint32_t ui_host_version = 3;

  struct UiHookApi {
    std::uint32_t version, size;
    int (*create)(void *target, void *detour, void **original);
    int (*enable)(void *target);
    int (*disable)(void *target);
  };

  struct UiPublication {
    std::uint64_t epoch;
    std::uint64_t mask[8];
    std::uint8_t bitmap[34];
    bool ready, has_bitmap, pending;
    bool menus_installed;
    std::uint16_t protagonist_id = 0xFFFF;
  };

  struct UiJewelCommand {
    std::uint64_t epoch;
    std::uint64_t mask[8];
    std::uint16_t raw_id;
    bool open;
  };

  static_assert(sizeof(UiPublication) == 112);
  static_assert(sizeof(UiJewelCommand) == 80);

  // Mod manager services (mod_entry.cpp, jewel_services.cpp, native_ui.cpp).
  bool attach_native_ui(const UiHookApi &hooks);
  void refresh_game_publication();
  std::array<std::uint64_t, 8> jewel_force_mask();
  bool set_jewel_force_mask(const std::array<std::uint64_t, 8> &mask);
  bool apply_jewel_command(const UiJewelCommand &command);
  // trait_services.cpp: the catalog is queued by the mod and applied by the manager thread.
  std::int64_t set_trait_catalog(const void *data, std::uint32_t bytes);
  void tick_trait_catalog(std::uintptr_t game_root, std::uint64_t epoch);
  // script_host.cpp: true once after mods were switched on or off during a callback.
  bool take_mod_selection_change();

} // namespace s8::scripting
