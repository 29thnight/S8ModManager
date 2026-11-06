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

} // namespace s8::scripting
