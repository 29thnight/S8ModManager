// Native resource row binding and retained-window lifecycle; no edit draft lives here.
struct WindowRegistration {
  int slot, action;
  std::wstring label, button;
  int owner;
};

struct WindowBinding {
  int id = 0;
  void *window = nullptr, *state = nullptr;
  bool dirty = false, pending = true;
  std::vector<void *> rows;
  std::vector<int> slots;
};

std::vector<WindowRegistration> window_registrations;
std::unordered_map<void *, WindowBinding> native_windows;
int next_window_id = 0;
WindowBinding *active_window = nullptr;

void bind_native_window(void *window) {
  auto &binding = native_windows[window];
  if (binding.id) {
    dispatch_mod_event(Event::WindowClosing, binding.id, 0);
    if (active_window == &binding)
      active_window = nullptr;
  }
  binding = {++next_window_id, window};
}

void bind_native_window_row(void *window, WindowBinding &binding, const WindowRegistration &registration) {
  // Reuse the managed registration's spare native row slot and cursor hierarchy.
  void *list = native_field<void *>(native_field<void *>(window, 0x238), 0x1A0);
  void *handle = native_field<void *>(list, 0x68);

  if (!handle)
    return;

  const auto index = native_field<std::uint32_t>(handle, 0);
  auto **heads = *reinterpret_cast<void ***>(native_ui_module + 0x34C8630);
  void *row = nullptr;
  int slot = 0;

  for (void *node = heads[index]; node; node = native_field<void *>(node, 8)) {
    void *child = native_field<void *>(node, 0);

    if (native_field<std::uintptr_t>(child, 0) != native_ui_module + 0x258E050)
      continue;

    if (slot++ == registration.slot) {
      row = child;
      break;
    }
  }

  if (!row)
    return;

  binding.rows.push_back(row);
  binding.slots.push_back(registration.slot);
  void *button = native_field<void *>(row, 0x1B8);
  // The spare row has the game's value-edit callback but no backing value.
  // Keep its hover cursor and let only our button handle activation.
  void *row_events = native_field<void *>(row, 0x80);

  if (row_events) {
    const auto event_index = native_field<std::uint32_t>(row_events, 0);

    for (void *node = heads[event_index]; node; node = native_field<void *>(node, 8)) {
      void *entry = native_field<void *>(node, 0);

      if (native_field<int>(entry, 0) == 0x7FFFFFFE)
        *reinterpret_cast<std::function<void(void *)> *>(static_cast<std::byte *>(entry) + 8) = nullptr;
    }
  }

  void *row_handle = native_field<void *>(row, 0x68);

  const auto row_index = native_field<std::uint32_t>(row_handle, 0);

  for (void *node = heads[row_index]; node; node = native_field<void *>(node, 8)) {
    void *child = native_field<void *>(node, 0);

    if (child == button || child == native_field<void *>(row, 0x1A0))
      continue;

    auto *vtable = native_field<std::uintptr_t *>(child, 0);
    reinterpret_cast<void (*)(void *, bool)>(vtable[0x108 / 8])(child, false);
  }

  auto **panes = native_field<void **>(row, 0x148);
  const auto pane_count = native_field<int>(row, 0x150);

  for (int id = 0; id < pane_count; ++id)
    reinterpret_cast<void (*)(void *, bool)>(native_ui_module + 0x1D15BC0)(panes[id], id == 0 || id == 14 || id == 9);
  reinterpret_cast<void (*)(void *, int, const wchar_t *, int)>(native_ui_module + 0x1D17190)(
      static_cast<std::byte *>(row) + 0x140, 14, registration.label.c_str(), 0);

  auto *row_vtable = native_field<std::uintptr_t *>(row, 0);
  reinterpret_cast<void (*)(void *, bool)>(row_vtable[0x108 / 8])(row, true);
  reinterpret_cast<void (*)(void *, bool)>(row_vtable[0x120 / 8])(row, true);

  auto *button_vtable = native_field<std::uintptr_t *>(button, 0);
  reinterpret_cast<void (*)(void *, bool)>(button_vtable[0x108 / 8])(button, true);
  reinterpret_cast<void (*)(void *, bool)>(button_vtable[0x110 / 8])(button, true);
  reinterpret_cast<void (*)(void *, const wchar_t *)>(button_vtable[0x298 / 8])(button, registration.button.c_str());

  std::function<void(void *)> clicked = [window, action = registration.action](void *) {
    if (native_ui_active.load(std::memory_order_acquire))
      return;
    auto &binding = native_windows.at(window);
    {
      std::lock_guard lock(native_ui_mutex);
      if (!native_ui_publication.ready)
        return;
      native_ui_session.source = native_ui_publication;
    }
    binding.state = native_field<void *>(native_field<void *>(window, 0x288), 8);
    active_window = &binding;
    native_ui_active.store(true, std::memory_order_release);
    dispatch_mod_event(Event::WindowClick, binding.id, action);
    active_window = nullptr;
    if (native_ui_session.frames.empty())
      native_ui_active.store(false, std::memory_order_release);
  };

  void *event_handle = native_field<void *>(button, 0x80);

  if (event_handle) {
    const auto event_index = native_field<std::uint32_t>(event_handle, 0);

    for (void *node = heads[event_index]; node; node = native_field<void *>(node, 8)) {
      void *entry = native_field<void *>(node, 0);

      if (native_field<int>(entry, 0) != 0x7FFFFFFE)
        continue;
      *reinterpret_cast<std::function<void(void *)> *>(static_cast<std::byte *>(entry) + 8) = nullptr;
      break;
    }
  }

  reinterpret_cast<void (*)(void *, int, void *)>(native_ui_module + 0xEF40)(button, 0x7FFFFFFE, &clicked);
}

void update_native_window(void *window, void *event) {
  if (window == native_tree_view.parent_window)
    return;
  reinterpret_cast<void (*)(void *, void *)>(window_update_hook.original)(window, event);
  const auto found = native_windows.find(window);
  if (found == native_windows.end())
    return;
  auto &binding = found->second;
  if (!binding.state)
    binding.state = native_field<void *>(native_field<void *>(window, 0x288), 8);

  for (void *row : binding.rows) {
    auto *vtable = native_field<std::uintptr_t *>(row, 0);
    reinterpret_cast<void (*)(void *, bool)>(vtable[0x108 / 8])(row, true);
    reinterpret_cast<void (*)(void *, bool)>(vtable[0x120 / 8])(row, true);
  }
  if (!binding.pending)
    return;
  for (const auto &registration : window_registrations)
    if (std::find(binding.slots.begin(), binding.slots.end(), registration.slot) == binding.slots.end())
      bind_native_window_row(window, binding, registration);
  binding.pending = binding.rows.size() != window_registrations.size();
}

bool native_window_dirty(void *window) {
  const bool changed = reinterpret_cast<bool (*)(void *)>(window_dirty_hook.original)(window);
  const auto found = native_windows.find(window);
  return changed || (found != native_windows.end() && found->second.dirty);
}

void native_window_exit(void *state) {
  const auto found = std::find_if(native_windows.begin(), native_windows.end(),
                                  [state](const auto &item) { return item.second.state == state; });
  int window_id = 0;
  bool accepted = false;
  if (found != native_windows.end()) {
    window_id = found->second.id;
    void *context = *native_field<void **>(state, 0x18);
    accepted = native_field<bool>(context, 0x80);
    if (active_window == &found->second)
      active_window = nullptr;
    native_windows.erase(found);
  }

  reinterpret_cast<void (*)(void *)>(window_exit_hook.original)(state);
  if (window_id)
    dispatch_mod_event(Event::WindowClosing, window_id, accepted ? 1 : 0);
}

void suspend_native_window() {
  if (!active_window)
    return;
  native_tree_view.parent_window = active_window->window;
  auto *vtable = native_field<std::uintptr_t *>(active_window->window, 0);
  reinterpret_cast<void (*)(void *, bool)>(vtable[0x120 / 8])(active_window->window, false);
}
