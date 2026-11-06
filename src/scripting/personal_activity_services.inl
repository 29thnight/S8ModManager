// Measured personal activity and calendar/state-transition boundaries.
// Initializers and screen teardown callbacks can run on worker threads.
ui_detail::Hook domestic_activity_hook{}, communicate_talk_hook{}, month_advance_hook{}, council_entry_hook{};

void dispatch_personal_activity(Event event, int officer, int value, std::uint64_t epoch) {
  {
    std::lock_guard lock(native_ui_mutex);
    if (!game_publication.ready || game_publication.epoch != epoch)
      return;
  }

  LARGE_INTEGER started{}, finished{}, frequency{};
  QueryPerformanceCounter(&started);
  const bool success = dispatch_mod_event(event, officer, value);
  QueryPerformanceCounter(&finished);
  QueryPerformanceFrequency(&frequency);

  const auto microseconds = (finished.QuadPart - started.QuadPart) * 1000000 / frequency.QuadPart;
  char message[256];
  std::snprintf(message, sizeof(message),
                "[S8 activity] event=%d officer=%d value=%d success=%d nativeThread=%lu dispatchUs=%lld\n",
                static_cast<int>(event), officer, value, success, GetCurrentThreadId(), microseconds);
  OutputDebugStringA(message);

  if (success)
    execute_pending_march();
}

void domestic_activity_step(void *state) {
  void *activity = native_field<void *>(state, 0x20);
  const auto category = activity ? native_field<std::uint8_t>(activity, 0x38C) : 0;
  void *root = *reinterpret_cast<void **>(native_ui_module + 0x2E98BC8);
  void *officer = native_field<void *>(root, 0xE0);
  const auto officer_id = officer ? native_field<std::uint16_t>(officer, 8) : 0;
  void *city = native_field<void *>(native_field<void *>(state, 0x18), 0x130);
  void *manager = static_cast<std::byte *>(city) + 0x10;
  const bool ready = category == 0x15 && officer && ensure_application();
  std::uint64_t epoch;

  {
    std::lock_guard lock(native_ui_mutex);
    epoch = game_publication.epoch;
  }

  reinterpret_cast<void (*)(void *)>(domestic_activity_hook.original)(state);

  // The original can replace the old state. Inspect the surviving manager,
  // and publish only after the measured return to CityStateCommandMain.
  if (ready && native_field<std::uintptr_t>(native_field<void *>(manager, 0x30), 0) ==
                   native_ui_module + 0x2699018)
    dispatch_personal_activity(Event::DomesticActivityCompleted, officer_id, category, epoch);
}

void communicate_talk_step(void *state) {
  void *manager = native_field<void *>(state, 0x10);
  void *root = native_field<void *>(native_field<void *>(state, 0x18), 0);
  void *officer = native_field<void *>(root, 0x68);
  void *partner = native_field<void *>(root, 0x60);
  const auto officer_id = native_field<std::uint16_t>(officer, 8);
  const auto partner_id = native_field<std::uint16_t>(partner, 8);
  const bool ready = ensure_application();
  std::uint64_t epoch;

  {
    std::lock_guard lock(native_ui_mutex);
    epoch = game_publication.epoch;
  }

  reinterpret_cast<void (*)(void *)>(communicate_talk_hook.original)(state);

  // Random talk events and their input waits belong to the original call.
  // Dispatch after it returns to CommunicateMainCommandState, not inside it.
  if (ready && native_field<std::uintptr_t>(native_field<void *>(manager, 0x30), 0) ==
                   native_ui_module + 0x2699AF0)
    dispatch_personal_activity(Event::TalkCompleted, officer_id, partner_id, epoch);
}

void month_advance(void *state) {
  void *root = *reinterpret_cast<void **>(native_ui_module + 0x2E98BC8);
  const auto previous = native_field<std::uint32_t>(root, 0x72D0) & 0xFFFFFF;
  const bool ready = ensure_application();
  std::uint64_t epoch;

  {
    std::lock_guard lock(native_ui_mutex);
    epoch = game_publication.epoch;
  }

  reinterpret_cast<void (*)(void *)>(month_advance_hook.original)(state);

  const auto current = native_field<std::uint32_t>(root, 0x72D0) & 0xFFFFFF;
  if (ready && current != previous)
    dispatch_personal_activity(Event::MonthChanged, previous, current, epoch);
}

void council_entry_step(void *state) {
  void *manager = native_field<void *>(state, 0x10);
  const bool ready = ensure_application();
  std::uint64_t epoch;

  {
    std::lock_guard lock(native_ui_mutex);
    epoch = game_publication.epoch;
  }

  reinterpret_cast<void (*)(void *)>(council_entry_hook.original)(state);

  // This step returns after the entrance presentation and the transition
  // to the council command state. The initialization step precedes it.
  void *current = native_field<void *>(manager, 0x30);
  if (ready && native_field<std::uintptr_t>(current, 0) == native_ui_module + 0x26A7B18 &&
      native_field<std::uint8_t>(manager, 0x38) == 0) {
    void *root = *reinterpret_cast<void **>(native_ui_module + 0x2E98BC8);
    const auto date = native_field<std::uint32_t>(root, 0x72D0) & 0xFFFFFF;
    dispatch_personal_activity(Event::CouncilEntered, 0, date, epoch);
  }
}
