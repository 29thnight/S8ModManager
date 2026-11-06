// Engine menu resources and callbacks. Managed registrations supply all added items.
struct MenuRegistration {
  MenuSurface surface;
  int action, template_id, insert_after, native_id = -1;
  std::wstring label;
  int owner;
};

std::vector<MenuRegistration> menu_registrations;
std::atomic<bool> application_ready{false};

#include "native_window.inl"

struct NativeMenuControl {
  std::int32_t type, x, y, width, height, padding;
  std::uint64_t text_id;
  const wchar_t *text;
  std::array<std::byte, 0x38> properties;
};

static_assert(sizeof(NativeMenuControl) == 0x60);
ui_detail::Hook menu_factory_hook{}, title_menu_hook{}, edit_menu_hook{}, function_menu_hook{}, navigation_hook{};
ui_detail::Hook palace_menu_hook{}, city_command_click_hook{};
ui_detail::Hook city_command_update_hook{};
extern std::atomic<bool> privileged_march_pending;

bool ensure_application() {
  if (!application_ready) {
    {
      std::lock_guard lock(native_ui_mutex);
      native_ui_session.source = native_ui_publication;
    }
    application_ready = dispatch_mod_event(Event::Configure, 0);
  }
  return application_ready;
}

void native_menu_controls(void *group, const NativeMenuControl *controls, int count, void *parent) {
  const auto original =
      reinterpret_cast<void (*)(void *, const NativeMenuControl *, int, void *)>(menu_factory_hook.original);
  if (!parent) {
    original(group, controls, count, parent);
    return;
  }
  // Load mods with the first game menu (the title menu), before "resume" can load
  // a save: the trait mod must publish its custom traits before the save is read.
  ensure_application();
  const auto type = native_field<std::uintptr_t>(parent, 0) - native_ui_module;
  if (type == 0x2590970 && count == 8) {
    const bool ready = ensure_application();
    original(group, controls, count, parent);
    if (ready)
      bind_native_window(parent);
    return;
  }

  const auto surface = type == 0x271EB28 && count == 6   ? MenuSurface::TitleEdit
                       : type == 0x271EDC0 && count == 6 ? MenuSurface::InGameEdit
                                                         : MenuSurface::Function;
  if (!((type == 0x271EB28 || type == 0x271EDC0) && count == 6) && !(type == 0x2701888 && count == 13)) {
    original(group, controls, count, parent);
    return;
  }
  if (!ensure_application()) {
    original(group, controls, count, parent);
    return;
  }

  std::vector<MenuRegistration *> additions;
  for (auto &item : menu_registrations)
    if (item.surface == surface)
      additions.push_back(&item);

  std::vector<NativeMenuControl> extended(controls, controls + count);
  const int spacing = surface == MenuSurface::Function ? controls[6].y - controls[5].y : controls[5].y - controls[4].y;
  extended[0].height += spacing * static_cast<int>(additions.size());
  for (int i = 1; i < count; ++i)
    for (const auto *item : additions)
      if (i > item->insert_after)
        extended[i].y += spacing;

  for (std::size_t i = 0; i < additions.size(); ++i) {
    auto &item = *additions[i];
    auto control = controls[item.template_id];
    control.x = controls[1].x;
    control.y = controls[item.insert_after].y + spacing;
    for (std::size_t earlier = 0; earlier < i; ++earlier)
      if (additions[earlier]->insert_after <= item.insert_after)
        control.y += spacing;
    control.text = item.label.c_str();
    item.native_id = static_cast<int>(extended.size());
    extended.push_back(control);
  }
  original(group, extended.data(), static_cast<int>(extended.size()), parent);
}

bool dispatch_menu_action(MenuSurface surface, int id) {
  const auto matches = [=](const auto &item) { return item.surface == surface && item.native_id == id; };
  if (std::none_of(menu_registrations.begin(), menu_registrations.end(), matches))
    return false;
  if (native_ui_active.load(std::memory_order_acquire))
    return true;
  if (!dispatch_mod_event(Event::Configure, 0))
    return true;
  const auto found = std::find_if(menu_registrations.begin(), menu_registrations.end(), matches);
  if (found == menu_registrations.end())
    return true;
  {
    std::lock_guard lock(native_ui_mutex);
    if (!native_ui_publication.ready)
      return true;
    native_ui_session.source = native_ui_publication;
  }
  native_ui_active.store(true, std::memory_order_release);
  dispatch_mod_event(Event::MenuClick, 0, found->action);
  if (native_ui_session.frames.empty() && !council_room_active())
    native_ui_active.store(false, std::memory_order_release);
  return true;
}

bool dispatch_menu_click(MenuSurface surface, void **argument) {
  return *argument && dispatch_menu_action(surface, native_field<int>(*argument, 0x88));
}

void native_palace_commands(void *layout, void *city) {
  reinterpret_cast<void (*)(void *, void *)>(palace_menu_hook.original)(layout, city);
  palace_command_layout = layout;
  if (!ensure_application())
    return;

  // CityUILayoutCommandList owns eight pre-created CityUILayoutCommandListBtn objects.
  // The palace builder has filled the original commands; only unused slots are claimed.
  int slot = 0;
  for (auto &item : menu_registrations) {
    if (item.surface != MenuSurface::Palace)
      continue;

    item.native_id = -1;
    void *button = nullptr;
    for (; slot < 8; ++slot) {
      button = native_field<void *>(layout, 0x1A0 + slot * sizeof(void *));
      if (button && native_field<std::uint16_t>(button, 0x288) >= 0xA6)
        break;
    }
    if (slot == 8)
      continue;

    // Reserved command IDs stay outside the engine's action/cost/availability tables.
    item.native_id = 0xFF00 + slot++;
    native_field<std::uint16_t>(button, 0x288) = static_cast<std::uint16_t>(item.native_id);
    native_field<int>(button, 0x298) = 0;
    auto *group = static_cast<std::byte *>(button) + 0x140;
    reinterpret_cast<void (*)(void *, int, const wchar_t *, bool)>(native_ui_module + 0x1D17190)(
        group, 0x17, item.label.c_str(), true);
    const auto lookup = reinterpret_cast<void *(*)(void *, int)>(native_ui_module + 0x1D15440);
    void *caption = lookup(group, 0x17);
    if (caption)
      reinterpret_cast<void (*)(void *, int)>(native_ui_module + 0x1D167B0)(caption, 0x32);

    // A reused slot may still contain costs or a condition badge from another facility.
    for (const int id : {0x18, 0x19, 0x1A}) {
      if (void *part = lookup(group, id)) {
        auto *vtable = native_field<std::uintptr_t *>(part, 0);
        reinterpret_cast<void (*)(void *, bool)>(vtable[0x108 / 8])(part, false);
      }
    }
    if (void *badge = native_field<void *>(button, 0x290)) {
      auto *vtable = native_field<std::uintptr_t *>(badge, 0);
      reinterpret_cast<void (*)(void *, bool)>(vtable[0x108 / 8])(badge, false);
    }

    auto *vtable = native_field<std::uintptr_t *>(button, 0);
    reinterpret_cast<void (*)(void *, bool)>(vtable[0x110 / 8])(button, true);
    reinterpret_cast<void (*)(void *, bool)>(vtable[0x108 / 8])(button, true);
  }
}

void native_city_command_clicked(void *callback, void **argument) {
  void *button = *argument;
  if (!button) {
    reinterpret_cast<void (*)(void *, void **)>(city_command_click_hook.original)(callback, argument);
    return;
  }
  const auto id = native_field<std::uint16_t>(button, 0x288);
  if (id >= 0xFF00 && id < 0xFF08) {
    if ((native_field<std::uint32_t>(button, 0x40) & 4) != 0)
      dispatch_menu_action(MenuSurface::Palace, id);
    return;
  }
  reinterpret_cast<void (*)(void *, void **)>(city_command_click_hook.original)(callback, argument);
}

void native_city_command_updated(void *layout) {
  reinterpret_cast<void (*)(void *)>(city_command_update_hook.original)(layout);
  continue_city_return();
  // A privileged player request is explicit work, not an AI evaluation tick.
  // Wait until its confirmation dialog has fully left the native popup stack.
  if (privileged_march_pending.load(std::memory_order_acquire) &&
      !native_ui_active.load(std::memory_order_acquire) && native_ui_session.frames.empty())
    execute_pending_march(true);
}

void native_title_menu_clicked(void *callback, void **argument) {
  if (!dispatch_menu_click(MenuSurface::TitleEdit, argument))
    reinterpret_cast<void (*)(void *, void **)>(title_menu_hook.original)(callback, argument);
}

void native_edit_menu_clicked(void *callback, void **argument) {
  if (!dispatch_menu_click(MenuSurface::InGameEdit, argument))
    reinterpret_cast<void (*)(void *, void **)>(edit_menu_hook.original)(callback, argument);
}

void native_function_menu_clicked(void *callback, void **argument) {
  if (!dispatch_menu_click(MenuSurface::Function, argument))
    reinterpret_cast<void (*)(void *, void **)>(function_menu_hook.original)(callback, argument);
}

void native_menu_navigation(void *layout, void *output) {
  reinterpret_cast<void (*)(void *, void *)>(navigation_hook.original)(layout, output);
  if (native_field<std::uintptr_t>(layout, 0) != native_ui_module + 0x2701888)
    return;
  const auto lookup = reinterpret_cast<void *(*)(void *, int)>(native_ui_module + 0x1D15440);

  struct Controls {
    void **begin, **end, **capacity;
  };

  auto &items = *static_cast<Controls *>(output);
  for (const auto &registration : menu_registrations) {
    if (registration.surface != MenuSurface::Function || registration.native_id < 0)
      continue;
    void *button = lookup(static_cast<std::byte *>(layout) + 0x140, registration.native_id);
    void *anchor = lookup(static_cast<std::byte *>(layout) + 0x140, registration.insert_after);
    const auto insertion = std::find(items.begin, items.end, anchor);
    if (!button || insertion == items.end || std::find(items.begin, items.end, button) != items.end)
      continue;
    const auto index = insertion - items.begin + 1;
    if (items.end == items.capacity)
      reinterpret_cast<void (*)(void *, void **, void **)>(native_ui_module + 0x33F50)(output, items.end, &button);
    else
      *items.end++ = button;
    std::rotate(items.begin + index, items.end - 1, items.end);
  }
}

bool install_native_menu_hooks() {
  struct Binding {
    std::uintptr_t rva;
    void *callback;
    ui_detail::Hook *hook;
  };

  const Binding bindings[]{{0x1D13E60, reinterpret_cast<void *>(&native_menu_controls), &menu_factory_hook},
                           {0x76EC40, reinterpret_cast<void *>(&native_title_menu_clicked), &title_menu_hook},
                           {0x76E930, reinterpret_cast<void *>(&native_edit_menu_clicked), &edit_menu_hook},
                           {0x744990, reinterpret_cast<void *>(&native_function_menu_clicked), &function_menu_hook},
                           {0x16094B0, reinterpret_cast<void *>(&native_palace_commands), &palace_menu_hook},
                           {0x16E7A0, reinterpret_cast<void *>(&native_city_command_clicked), &city_command_click_hook},
                           {0x1600E60, reinterpret_cast<void *>(&native_city_command_updated), &city_command_update_hook},
                           {0x1D04F90, reinterpret_cast<void *>(&native_menu_navigation), &navigation_hook}};
  std::size_t enabled = 0;
  for (const auto &binding : bindings) {
    const auto hook =
        ui_detail::create_hook(reinterpret_cast<void *>(native_ui_module + binding.rva), binding.callback);
    if (hook)
      *binding.hook = *hook;
    if (!hook || ui_detail::enable_hook(*binding.hook) != 0) {
      for (std::size_t i = 0; i < enabled; ++i)
        ui_detail::disable_hook(*bindings[i].hook);
      return false;
    }
    ++enabled;
  }
  return true;
}
