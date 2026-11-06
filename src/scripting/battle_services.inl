// WarTurnEndState step 14 finishes the original turn-end sequence and changes
// state. Keep these bindings separate from dialog selection and UI drafts.
struct BattleUnitRecord {
  std::uint64_t token, identity;
  std::uint32_t soldiers, capacity;
  std::uint16_t officers[3];
  std::uint16_t personalities[3][3];
};

static_assert(sizeof(BattleUnitRecord) == 48);

struct BattleEventContext {
  std::uint64_t session, sequence, acting_unit;
  Event event;
  std::uint32_t reserved = 0;
  std::uint64_t world_session = 0;
};

static_assert(sizeof(BattleEventContext) == 40);

struct BattleUnitBinding {
  BattleUnitRecord record;
  void *unit;
};

ui_detail::Hook battle_turn_hook{}, battle_unit_end_hook{}, battle_select_hook{}, battle_destroy_hook{};
thread_local std::vector<BattleUnitBinding> *battle_units = nullptr;
thread_local const BattleEventContext *battle_context = nullptr;
std::atomic<std::uint64_t> battle_sequence{0};
std::uint64_t battle_session_id = 0, battle_session_epoch = 0, battle_unit_sequence = 0;
void *battle_session_root = nullptr, *battle_session_manager = nullptr;
std::unordered_map<void *, std::pair<void *, std::uint64_t>> battle_unit_ids;
thread_local LONGLONG battle_presentation_ticks = 0;

void reset_battle_session() {
  if (!battle_session_root)
    return;

  battle_session_root = nullptr;
  battle_session_manager = nullptr;
  battle_unit_ids.clear();
  ++battle_session_id;
  if (application_ready)
    dispatch_mod_event(Event::BattleSessionEnded, 0);
}

// Read these counters without breakpoints. Presentation time includes the
// player's dialogue wait and is excluded from script processing time.
struct BattleScriptTiming {
  std::atomic<std::uint64_t> calls{0};
  std::atomic<std::uint64_t> last_microseconds{0};
  std::atomic<std::uint64_t> maximum_microseconds{0};
  std::atomic<std::uint64_t> presentation_microseconds{0};
};

BattleScriptTiming battle_script_timing{};

std::optional<std::int64_t> invoke_battle_service(Operation operation, std::uint64_t target, std::int64_t argument,
                                                  void *data, std::uint32_t bytes) {
  if (operation == Operation::GetBattleContext) {
    if (!data || bytes != sizeof(BattleEventContext) || target || argument)
      return static_cast<int>(Status::InvalidApi);

    *static_cast<BattleEventContext *>(data) = battle_context && battle_context->session == battle_session_id
                                                 ? *battle_context : BattleEventContext{};
    return 0;
  }

  if (operation != Operation::ReadBattleUnits && operation != Operation::SetBattleSoldiers &&
      operation != Operation::ShowBattlePersonality && operation != Operation::ReadBattleSoldiers)
    return std::nullopt;

  if (!battle_units || !battle_context)
    return static_cast<int>(Status::InvalidBinding);
  if (battle_context->session != battle_session_id)
    return static_cast<int>(Status::StaleSession);

  if (operation == Operation::ReadBattleUnits) {
    if (!data && !bytes)
      return battle_units->size();
    if (!data || bytes != battle_units->size() * sizeof(BattleUnitRecord))
      return static_cast<int>(Status::InvalidApi);

    auto *output = static_cast<BattleUnitRecord *>(data);
    for (const auto &binding : *battle_units)
      *output++ = binding.record;
    return battle_units->size();
  }

  const auto found = std::find_if(battle_units->begin(), battle_units->end(),
                                  [target](const auto &binding) { return binding.record.token == target; });
  if (found == battle_units->end())
    return static_cast<int>(Status::StaleSession);
  const auto identity = battle_unit_ids.find(found->unit);
  if (identity == battle_unit_ids.end() || identity->second.second != found->record.identity)
    return static_cast<int>(Status::StaleSession);
  if (battle_context->acting_unit && found->record.identity != battle_context->acting_unit)
    return static_cast<int>(Status::InvalidBinding);

  if (operation == Operation::ShowBattlePersonality) {
    if (argument < 0 || static_cast<std::uint64_t>(argument) > 0xFFFFFFFF || !data ||
        bytes < sizeof(wchar_t) || bytes > 1024 || bytes % sizeof(wchar_t))
      return static_cast<int>(Status::InvalidApi);

    const auto *speech = static_cast<const wchar_t *>(data);
    const auto length = wcsnlen_s(speech, bytes / sizeof(wchar_t));
    const auto officer = static_cast<std::uint16_t>(argument);
    const auto personality = static_cast<std::uint16_t>(argument >> 16);

    if (!length || length + 1 != bytes / sizeof(wchar_t) || personality < 1 || personality > 254)
      return static_cast<int>(Status::InvalidApi);

    bool held = false;
    for (unsigned member = 0; member < 3; ++member) {
      if (found->record.officers[member] == officer)
        held = std::find(std::begin(found->record.personalities[member]),
                         std::end(found->record.personalities[member]), personality) !=
               std::end(found->record.personalities[member]);
    }
    if (!held)
      return static_cast<int>(Status::InvalidBinding);

    // Personality presentation used by 139B300 -> AF530 -> AF0B0. AF0B0
    // resolves the portrait and the trait's voice at +30 and builds the same
    // native dialogue configuration as an engine effect. AF530 waits using
    // the game's presentation loop; the string remains pinned until it returns.
    using ShowPersonality = void (*)(std::uint16_t, std::uint16_t, const wchar_t *,
                                    std::uint16_t, std::uintptr_t, std::uintptr_t, std::uintptr_t, bool);
    LARGE_INTEGER before{}, after{};
    QueryPerformanceCounter(&before);
    reinterpret_cast<ShowPersonality>(native_ui_module + 0xAF530)(
        personality, officer, speech, 0, 0, 0, 0, false);
    QueryPerformanceCounter(&after);
    battle_presentation_ticks += after.QuadPart - before.QuadPart;
    return 0;
  }

  if (data || bytes || argument < 0 || argument > 65535 ||
      (operation == Operation::ReadBattleSoldiers && argument))
    return static_cast<int>(Status::InvalidApi);

  if (operation == Operation::ReadBattleSoldiers)
    return reinterpret_cast<unsigned (*)(void *)>(native_ui_module + 0x1E3A8D0)(found->unit);

  // The engine setter clamps to the current unit capacity.
  reinterpret_cast<void (*)(void *, unsigned)>(native_ui_module + 0x1E3A880)(
      found->unit, static_cast<unsigned>(argument));
  return 0;
}

void dispatch_battle_event(void *root, void *manager, Event event, void *selected_unit = nullptr) {
  if (!ensure_application())
    return;

  std::uint64_t epoch;
  {
    std::lock_guard lock(native_ui_mutex);
    epoch = native_ui_publication.epoch.value;
  }
  if (root != battle_session_root || manager != battle_session_manager || epoch != battle_session_epoch) {
    reset_battle_session();
    battle_session_root = root;
    battle_session_manager = manager;
    battle_session_epoch = epoch;
    ++battle_session_id;
  }

  LARGE_INTEGER started{}, finished{};
  QueryPerformanceCounter(&started);

  auto *services = native_field<void *>(root, 0x50);
  auto *collection = native_field<void *>(services, 0x108);
  auto **begin = native_field<void **>(collection, 0x20);
  auto **end = native_field<void **>(collection, 0x28);
  const auto first = reinterpret_cast<std::uintptr_t>(begin);
  const auto last = reinterpret_cast<std::uintptr_t>(end);
  if (last < first || (last - first) % sizeof(void *) || (last - first) / sizeof(void *) > 60)
    return;

  std::vector<BattleUnitBinding> bindings;
  const auto sequence = ++battle_sequence;
  BattleEventContext context{battle_session_id, sequence, 0, event, 0, epoch};
  for (auto **entry = begin; entry != end; ++entry) {
    void *unit = *entry;
    if (!unit || native_field<std::uintptr_t>(unit, 0) != native_ui_module + 0x2718188)
      continue;

    BattleUnitBinding binding{};
    binding.unit = unit;
    std::fill(std::begin(binding.record.officers), std::end(binding.record.officers), std::uint16_t{0xFFFF});
    binding.record.token = (sequence << 8) | (bindings.size() + 1);
    binding.record.soldiers = reinterpret_cast<unsigned (*)(void *)>(native_ui_module + 0x1E3A8D0)(unit);
    binding.record.capacity = reinterpret_cast<unsigned (*)(void *)>(native_ui_module + 0x1E3A860)(unit);
    auto *model = native_field<void *>(unit, 8);
    auto [identity, inserted] = battle_unit_ids.try_emplace(unit, model, 0);
    if (inserted || identity->second.first != model) {
      identity->second = {model, ++battle_unit_sequence};
    }
    binding.record.identity = identity->second.second;
    if (unit == selected_unit)
      context.acting_unit = binding.record.identity;
    for (unsigned member = 0; member < 3; ++member) {
      auto *officer = native_field<void *>(model, 0x60 + member * sizeof(void *));
      if (!officer)
        continue;
      binding.record.officers[member] = native_field<std::uint16_t>(officer, 8);
      for (unsigned slot = 0; slot < 3; ++slot) {
        auto *personality = native_field<void *>(officer, 0x88 + slot * sizeof(void *));
        if (personality)
          binding.record.personalities[member][slot] = native_field<std::uint16_t>(personality, 8);
      }
    }
    bindings.push_back(binding);
  }

  std::erase_if(battle_unit_ids, [&bindings](const auto &entry) {
    return std::none_of(bindings.begin(), bindings.end(), [&entry](const auto &binding) {
      return binding.unit == entry.first;
    });
  });

  // Read the full roster for lifetime reconciliation, but allow an action-end
  // callback to mutate only its acting unit. Removed units have no live token.
  if (selected_unit && !context.acting_unit)
    return;

  auto *previous = battle_units;
  const auto *previous_context = battle_context;
  battle_units = &bindings;
  battle_context = &context;
  const auto previous_presentation = battle_presentation_ticks;
  battle_presentation_ticks = 0;
  dispatch_mod_event(event, 0, native_field<int>(root, 0x28));
  QueryPerformanceCounter(&finished);

  static const LONGLONG frequency = [] {
    LARGE_INTEGER value{};
    QueryPerformanceFrequency(&value);
    return value.QuadPart;
  }();
  const auto processing = static_cast<std::uint64_t>(
      (finished.QuadPart - started.QuadPart - battle_presentation_ticks) * 1000000 / frequency);
  battle_script_timing.last_microseconds.store(processing, std::memory_order_relaxed);
  auto maximum = battle_script_timing.maximum_microseconds.load(std::memory_order_relaxed);
  while (maximum < processing && !battle_script_timing.maximum_microseconds.compare_exchange_weak(
                                     maximum, processing, std::memory_order_relaxed)) {
  }
  battle_script_timing.presentation_microseconds.store(
      static_cast<std::uint64_t>(battle_presentation_ticks * 1000000 / frequency), std::memory_order_relaxed);
  battle_script_timing.calls.fetch_add(1, std::memory_order_relaxed);
  battle_presentation_ticks = previous_presentation;
  battle_units = previous;
  battle_context = previous_context;
}

void battle_turn_step(void *state) {
  const auto stage = native_field<unsigned>(state, 0x20);
  void *manager = native_field<void *>(state, 0x10);
  void *root = native_field<void *>(native_field<void *>(state, 0x18), 0);

  reinterpret_cast<void (*)(void *)>(battle_turn_hook.original)(state);

  // Stage 14 can remain pending during presentation. A stage number alone
  // does not establish completion: wait for the manager to own WarUnitActState.
  if (stage == 14) {
    void *next = native_field<void *>(manager, 0x30);
    if (native_field<std::uintptr_t>(next, 0) == native_ui_module + 0x27079A0)
      dispatch_battle_event(root, manager, Event::BattleTurnStarted);
  }
}

void battle_select_unit(void *state) {
  void *manager = native_field<void *>(state, 0x10);
  void *root = native_field<void *>(native_field<void *>(state, 0x18), 0);

  reinterpret_cast<void (*)(void *)>(battle_select_hook.original)(state);

  // WarUnitActState selects state 5 only when no next acting unit remains.
  // Read the surviving manager, not the old state released by ChangeState.
  void *next = native_field<void *>(manager, 0x30);
  if (native_field<std::uintptr_t>(next, 0) == native_ui_module + 0x2707880)
    dispatch_battle_event(root, manager, Event::BattleTurnEnded);
}

bool battle_unit_action_end(void *state) {
  void *manager = native_field<void *>(state, 0x10);
  void *root = native_field<void *>(native_field<void *>(state, 0x18), 0);
  void *unit = native_field<void *>(state, 0x38);

  const bool completed = reinterpret_cast<bool (*)(void *)>(battle_unit_end_hook.original)(state);

  // The step can wait for presentation. Emit once only after native cleanup
  // transitions away; a battle result has no subsequent unit callback.
  void *next = native_field<void *>(manager, 0x30);
  if (completed && unit && next != state && native_field<std::uint8_t>(next, 8) != 19)
    dispatch_battle_event(root, manager, Event::BattleUnitActionEnded, unit);

  return completed;
}

void *battle_state_machine_destroy(void *manager, unsigned flags) {
  if (manager == battle_session_manager)
    reset_battle_session();

  return reinterpret_cast<void *(*)(void *, unsigned)>(battle_destroy_hook.original)(manager, flags);
}
