// Included by native_ui.cpp. Only the scripted instance receives these vtables;
// the quarterly council keeps its original state machine and command handlers.
struct CouncilRoomView {
  void *state = nullptr, *machine = nullptr;
  void *in_game = nullptr;
  std::uintptr_t *state_table = nullptr, *machine_table = nullptr;
  std::array<std::uintptr_t, 21> state_methods{};
  std::array<std::uintptr_t, 6> machine_methods{}, command_methods{};
  std::array<std::byte, 16> previous_context{};
  std::uint32_t previous_flags = 0;
  std::uint64_t epoch = 0;
  void *previous_stage = nullptr;
  int previous_scene = 0;
  int previous_game_mode = 0;
  int previous_menu_mode = 0;
  std::uint32_t previous_bar_flags = 0;
  std::uint32_t previous_advance_flags = 0;
  bool load_pending = false;
  bool stage_entered = false;
  bool faded_in = false;
  std::array<void *, 2> palace_controls{};
  std::array<std::uint32_t, 2> palace_control_flags{};
  bool closing = false;
  HWND window = nullptr;
  bool exit_game = false;
};

std::shared_ptr<CouncilRoomView> council_room;
HWND city_return_exit_window = nullptr;

bool council_exit_requested() { return council_room && council_room->exit_game; }

void close_council_dialog_for_exit() {
  if (!native_ui_session.frames.empty() && !native_ui_session.text_input_open) {
    auto &frame = native_ui_session.frame();
    if (frame.dialog && !frame.close_requested) {
      frame.close_requested = true;
      auto *methods = native_field<std::uintptr_t *>(frame.dialog, 0);
      reinterpret_cast<void (*)(void *, int)>(methods[0x2C8 / 8])(frame.dialog, 0x7FFFFFFE);
    }
  }
}

LRESULT CALLBACK council_window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam,
                                     UINT_PTR id, DWORD_PTR) {
  if (message == WM_NCDESTROY) {
    RemoveWindowSubclass(window, council_window_proc, id);
    if (council_room)
      council_room->window = nullptr;
    if (city_return_exit_window == window)
      city_return_exit_window = nullptr;
  } else if (message == WM_CLOSE || (message == WM_SYSCOMMAND && (wparam & 0xFFF0) == SC_CLOSE)) {
    if (council_room) {
      council_room->exit_game = true;
      council_room->closing = true;
      close_council_dialog_for_exit();
      return 0;
    }
    if (city_return_exit_window == window)
      return 0;
  }
  return DefSubclassProc(window, message, wparam, lparam);
}

// Set by the palace command builder. The council is entered from this list,
// and our click handler does not run the engine path that closes it.
void *palace_command_layout = nullptr;

// The engine's menu-mode setter takes a third, nullable context argument.
// Engine callers pass null for plain mode changes; omitting it passes garbage.
void set_global_menu_mode(void *global_ui, int mode) {
  reinterpret_cast<void (*)(void *, int, void *)>(native_ui_module + 0x1D0A950)(global_ui, mode, nullptr);
}

bool council_room_active() { return council_room != nullptr; }

// The council room replaced the city scene, so leaving reloads the city the
// same way the engine does, then lifts the fade the loader leaves black.
void *city_return_in_game = nullptr;
bool city_return_loaded = false;

void begin_city_return(void *in_game) {
  reinterpret_cast<void (*)(void *)>(native_ui_module + 0x1BE3D20)(in_game);
  city_return_in_game = in_game;
  city_return_loaded = false;
}

// Polled every city frame until the city scene is loaded and visible.
void continue_city_return() {
  if (!city_return_in_game)
    return;
  if (!city_return_loaded) {
    auto *loader = static_cast<std::byte *>(city_return_in_game) + 0xB78;
    if (!reinterpret_cast<bool (*)(void *)>(native_ui_module + 0x1BDB0A0)(loader))
      return;
    city_return_loaded = true;
  }
  if (reinterpret_cast<bool (*)()>(native_ui_module + 0x1BDFC90)()) {
    city_return_in_game = nullptr;
    if (HWND window = std::exchange(city_return_exit_window, nullptr)) {
      RemoveWindowSubclass(window, council_window_proc, 0);
      PostMessageW(window, WM_SYSCOMMAND, SC_CLOSE, 0);
    }
  }
}

void *council_room_in_game() {
  void *manager = reinterpret_cast<void *(*)()>(native_ui_module + 0xF0B0)();
  auto **states = native_field<void **>(manager, 0x20);
  const auto count = native_field<std::size_t>(manager, 0x10);
  for (std::size_t index = 0; index < count; ++index)
    if (std::strcmp(static_cast<const char *>(states[index]) + 0x71, "InGameState") == 0)
      return states[index];
  return nullptr;
}

void close_council_room() {
  council_room->closing = true;
}

void council_command_enter(void *) {
  auto &view = *council_room;
  auto *in_game = static_cast<std::byte *>(view.in_game);
  void *controller = *reinterpret_cast<void **>(native_ui_module + 0x35100B8);
  view.previous_stage = native_field<void *>(controller, 0);
  auto *scene = native_field<int *>(controller, 0x80);
  const auto *scene_end = native_field<int *>(controller, 0x88);
  for (; scene != scene_end; scene += 2)
    if (scene[1] == native_field<int>(controller, 0x98)) {
      view.previous_scene = scene[0];
      break;
    }

  auto *root = *reinterpret_cast<void **>(native_ui_module + 0x2E98BC8);
  native_field<int>(root, 0xD0) = 4;

  // Request the same asynchronous room load as the quarterly council.
  reinterpret_cast<void (*)(void *, bool)>(native_ui_module + 0x1BE3470)(in_game, true);
  view.load_pending = true;
  void *transition = reinterpret_cast<void *(*)()>(native_ui_module + 0xCD3B0)();
  reinterpret_cast<void (*)(void *, bool)>(native_ui_module + 0x1633E10)(transition, false);
}

void council_command_update(void *) {
  // Original entry/update also reset faction plans and consume game commands.
  auto &view = *council_room;
  auto *in_game = static_cast<std::byte *>(view.in_game);
  if (!view.stage_entered) {
    if (!reinterpret_cast<bool (*)(void *)>(native_ui_module + 0x1BDB0A0)(in_game + 0x860))
      return;

    view.load_pending = false;
    void *stage = in_game + 0x4B0;
    if (!native_field<void *>(stage, 0x18)) {
      view.closing = true;
      return;
    }

    auto *root = *reinterpret_cast<void **>(native_ui_module + 0x2E98BC8);
    native_field<int>(root, 0xD0) = 5;
    view.stage_entered = true;
    if (!dispatch_mod_event(Event::CouncilRoomReady, 0))
      view.closing = true;
  }
  // The room loader finishes with the global fade left black; the caller
  // lifts it. Poll the original fade-in step until it reports clear.
  if (!view.faded_in)
    view.faded_in = reinterpret_cast<bool (*)()>(native_ui_module + 0x1BDFC90)();
  reinterpret_cast<void (*)(void *)>(native_ui_module + 0x1895EC0)(in_game + 0x4B0);
  reinterpret_cast<void (*)(void *)>(native_ui_module + 0x1897030)(in_game + 0x750);
}

void council_command_exit(void *) {}

void *council_room_state_factory(void *machine, int id) {
  auto &view = *council_room;
  void *state = reinterpret_cast<void *(*)(void *, int)>(view.machine_table[0x10 / 8])(machine, id);
  if (id == 3) {
    auto *table = native_field<std::uintptr_t *>(state, 0);
    std::copy_n(table - 1, view.command_methods.size(), view.command_methods.data());
    auto *methods = view.command_methods.data() + 1;
    methods[0x08 / 8] = reinterpret_cast<std::uintptr_t>(&council_command_enter);
    methods[0x10 / 8] = reinterpret_cast<std::uintptr_t>(&council_command_update);
    methods[0x18 / 8] = reinterpret_cast<std::uintptr_t>(&council_command_exit);
    native_field<std::uintptr_t *>(state, 0) = methods;
  }
  return state;
}

void council_room_update(void *state) {
  auto &view = *council_room;
  if (view.exit_game)
    close_council_dialog_for_exit();
  if (view.closing && !view.load_pending && native_ui_session.frames.empty()) {
    // Pop this view directly. ConferenceExitState and the quarterly result
    // application path are deliberately absent from the scripted state graph.
    native_field<int>(state, 8) = 0x7FFFFFFE;
    void *manager = reinterpret_cast<void *(*)()>(native_ui_module + 0xF0B0)();
    reinterpret_cast<void (*)(void *)>(native_ui_module + 0xF150)(manager);
    return;
  }
  reinterpret_cast<void (*)(void *)>(view.state_table[0x30 / 8])(state);
}

void open_council_room(void *in_game, bool hide_palace_controls) {
  council_room = std::make_shared<CouncilRoomView>();
  auto &view = *council_room;
  view.epoch = native_ui_session.source.epoch.value;
  view.in_game = in_game;
  if (HWND window = GetActiveWindow())
    if (SetWindowSubclass(window, council_window_proc, 0, 0))
      view.window = window;
  native_ui_active.store(true, std::memory_order_release);
  if (hide_palace_controls && palace_command_layout) {
    void *city_layout = native_field<void *>(in_game, 0xBB8);
    for (std::size_t index = 0; index < 10; ++index) {
      void *facility = native_field<void *>(city_layout, 0x210 + index * sizeof(void *));
      if (!facility || native_field<void *>(facility, 0x1D8) != palace_command_layout)
        continue;

      view.palace_controls = {palace_command_layout, native_field<void *>(facility, 0x1E0)};
      for (std::size_t control_index = 0; control_index < view.palace_controls.size(); ++control_index) {
        void *control = view.palace_controls[control_index];
        if (!control)
          continue;
        view.palace_control_flags[control_index] = native_field<std::uint32_t>(control, 0x40);
        auto *methods = native_field<std::uintptr_t *>(control, 0);
        reinterpret_cast<void (*)(void *, bool)>(methods[0x108 / 8])(control, false);
        reinterpret_cast<void (*)(void *, bool)>(methods[0x120 / 8])(control, false);
      }
      break;
    }
  }
  auto *root = *reinterpret_cast<std::byte **>(native_ui_module + 0x2E98BC8);
  view.previous_game_mode = native_field<int>(root, 0xD0);
  std::copy_n(root + 0x2169E0, view.previous_context.size(), view.previous_context.data());
  view.previous_flags = native_field<std::uint32_t>(root, 0x71E4) & 0x81u;
  native_field<std::uint32_t>(root, 0x71E4) &= ~1u;

  void *global_ui = *reinterpret_cast<void **>(native_ui_module + 0x2E99460);
  void *bar = native_field<void *>(global_ui, 0x10);
  view.previous_menu_mode = native_field<int>(bar, 0x218);
  view.previous_bar_flags = native_field<std::uint32_t>(bar, 0x40);
  view.previous_advance_flags = native_field<std::uint32_t>(native_field<void *>(bar, 0x1B8), 0x40);
  // The engine destroys its model after calling this completion. Keep the
  // instance vtables alive until its stored completion is itself destroyed.
  std::function<void(void *)> completed = [retained = council_room](void *state) {
    auto &finished = *retained;
    native_field<std::uintptr_t *>(state, 0) = finished.state_table;
    native_field<std::uintptr_t *>(static_cast<std::byte *>(finished.machine) + 0x10, 0) = finished.machine_table;
    bool same_session;
    {
      std::lock_guard lock(native_ui_mutex);
      same_session = game_publication.epoch == finished.epoch;
    }
    if (same_session) {
      if (finished.stage_entered) {
        auto *stage = static_cast<std::byte *>(finished.in_game) + 0x4B0;
        reinterpret_cast<void (*)(void *)>(native_ui_module + 0x1896D60)(stage);
        if (finished.previous_stage)
          reinterpret_cast<void (*)(void *, bool)>(native_ui_module + 0x1808E40)(finished.previous_stage, true);

        void *controller = *reinterpret_cast<void **>(native_ui_module + 0x35100B8);
        native_field<void *>(controller, 0) = finished.previous_stage;
        reinterpret_cast<void (*)(void *, bool, int)>(native_ui_module + 0x1808900)(
            controller, finished.previous_scene != 0, finished.previous_scene);
      }

      auto *root = *reinterpret_cast<std::byte **>(native_ui_module + 0x2E98BC8);
      native_field<int>(root, 0xD0) = finished.previous_game_mode;
      std::copy(finished.previous_context.begin(), finished.previous_context.end(), root + 0x2169E0);
      auto &flags = native_field<std::uint32_t>(root, 0x71E4);
      flags = (flags & ~0x81u) | finished.previous_flags;
      void *global_ui = *reinterpret_cast<void **>(native_ui_module + 0x2E99460);
      void *bar = native_field<void *>(global_ui, 0x10);
      set_global_menu_mode(global_ui, finished.previous_menu_mode);
      for (std::size_t index = 0; index < finished.palace_controls.size(); ++index)
        if (void *control = finished.palace_controls[index])
          native_field<std::uint32_t>(control, 0x40) = finished.palace_control_flags[index];
      native_field<std::uint32_t>(bar, 0x40) = finished.previous_bar_flags;
      native_field<std::uint32_t>(native_field<void *>(bar, 0x1B8), 0x40) = finished.previous_advance_flags;
      begin_city_return(finished.in_game);
    }
    if (finished.window) {
      if (finished.exit_game && same_session) {
        city_return_exit_window = finished.window;
      } else {
        RemoveWindowSubclass(finished.window, council_window_proc, 0);
        if (finished.exit_game)
          PostMessageW(finished.window, WM_SYSCOMMAND, SC_CLOSE, 0);
      }
    }
    council_room.reset();
    dispatch_mod_event(Event::Closed, 0);
    native_ui_active.store(false, std::memory_order_release);
  };
  const std::array<std::uintptr_t, 2> config{};
  void *manager = reinterpret_cast<void *(*)()>(native_ui_module + 0xF0B0)();
  using Enqueue = void (*)(void *, const char *, const void *, std::function<void(void *)> *);
  reinterpret_cast<Enqueue>(native_ui_module + 0x683BD0)(manager, "ConferenceGameState", config.data(), &completed);
  const auto count = native_field<std::size_t>(manager, 0x30);
  auto *queue = native_field<std::byte *>(manager, 0x40);
  view.state = native_field<void *>(queue, (count - 1) * 0x10 + 8);
  view.state_table = native_field<std::uintptr_t *>(view.state, 0);
  std::copy_n(view.state_table - 1, view.state_methods.size(), view.state_methods.data());
  auto *methods = view.state_methods.data() + 1;
  methods[0x30 / 8] = reinterpret_cast<std::uintptr_t>(&council_room_update);
  native_field<std::uintptr_t *>(view.state, 0) = methods;

  view.machine = native_field<void *>(native_field<void *>(view.state, 0x478), 8);
  void *machine_interface = static_cast<std::byte *>(view.machine) + 0x10;
  view.machine_table = native_field<std::uintptr_t *>(machine_interface, 0);
  std::copy_n(view.machine_table - 1, view.machine_methods.size(), view.machine_methods.data());
  auto *machine_methods = view.machine_methods.data() + 1;
  machine_methods[0x10 / 8] = reinterpret_cast<std::uintptr_t>(&council_room_state_factory);
  native_field<std::uintptr_t *>(machine_interface, 0) = machine_methods;
}
