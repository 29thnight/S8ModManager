// Title screen options (환경설정): one extra tab of choice rows supplied by a mod.
// The screen keeps four tabs mapped to pages 4..7 on the title (0..3 in game). The
// extra tab borrows page map[0] for the length of one tab switch: its entry vector
// is swapped for ours, so the game builds the list, scrollbar and caption itself.
// Entry ids from option_id_base up are past the game's switch (0..0x99), which
// skips their construction and their settings write; the hooks below build the
// row, report the chosen value and supply the description panel text.
constexpr int option_id_base = 0x100;
constexpr std::uintptr_t options_screen_vtable = 0x26CDAA0, options_tab_labels = 0x3B91488;
constexpr int options_game_tabs = 4, options_title_first_page = 4;

struct OptionRowState {
  int action = 0, current = 0;
  std::wstring label, description;
  std::vector<std::wstring> choices;
  std::vector<const wchar_t *> choice_texts;
  std::vector<int> choice_values;
};

struct OptionsTabState {
  int owner = 0;
  std::wstring label;
  std::vector<OptionRowState> rows;
  std::vector<std::array<std::int32_t, 2>> entries; // the game's {id, kind}; kind -1 = setting row
  std::array<const wchar_t *, options_game_tabs + 1> tab_labels{};
};

// The game keeps pointers into a tab's strings and entries while the screen is
// open, so a replaced registration is retired, never freed.
std::vector<std::unique_ptr<OptionsTabState>> option_tabs;
OptionsTabState *option_tab = nullptr;
const wchar_t *option_description = nullptr;

ui_detail::Hook options_set_tabs_hook{}, options_tab_changed_hook{}, options_build_row_hook{},
    options_row_event_hook{}, options_describe_hook{}, options_text_layout_hook{};

struct NativeVector {
  const void *first, *last, *end;
};

bool is_title_options_screen(void *screen) {
  if (!screen || native_field<std::uintptr_t>(screen, 0) != native_ui_module + options_screen_vtable)
    return false;
  const auto *map = native_field<const std::int32_t *>(screen, 0x280);
  const auto *map_end = native_field<const std::int32_t *>(screen, 0x288);
  return map && map_end - map == options_game_tabs && map[0] == options_title_first_page;
}

OptionRowState *option_row(const std::int32_t *entry) {
  if (!option_tab || !entry || entry[1] != -1 || entry[0] < option_id_base)
    return nullptr;
  const auto index = static_cast<std::size_t>(entry[0] - option_id_base);
  return index < option_tab->rows.size() ? &option_tab->rows[index] : nullptr;
}

// vt+0x238 of the tab group: (group, x, y, owner, labels, count, ...).
std::uintptr_t options_set_tabs(void *group, std::uintptr_t x, std::uintptr_t y, void *owner,
                                const wchar_t *const *labels, std::uintptr_t count, std::uintptr_t a7,
                                std::uintptr_t a8) {
  using SetTabs = std::uintptr_t (*)(void *, std::uintptr_t, std::uintptr_t, void *, const wchar_t *const *,
                                     std::uintptr_t, std::uintptr_t, std::uintptr_t);
  const auto original = reinterpret_cast<SetTabs>(options_set_tabs_hook.original);
  if (!option_tab || reinterpret_cast<std::uintptr_t>(labels) != native_ui_module + options_tab_labels ||
      static_cast<std::uint32_t>(count) != options_game_tabs || !is_title_options_screen(owner))
    return original(group, x, y, owner, labels, count, a7, a8);

  auto &extended = option_tab->tab_labels;
  std::copy_n(labels, options_game_tabs, extended.begin());
  extended[options_game_tabs] = option_tab->label.c_str();
  return original(group, x, y, owner, extended.data(), options_game_tabs + 1, a7, a8);
}

// The switch handler reads only the group's selected index (+0x88).
void options_tab_changed(void *screen, void *group) {
  const auto original = reinterpret_cast<void (*)(void *, void *)>(options_tab_changed_hook.original);
  if (!option_tab || !group || native_field<std::int32_t>(group, 0x88) != options_game_tabs ||
      !is_title_options_screen(screen)) {
    original(screen, group);
    return;
  }

  alignas(16) static std::array<std::byte, 0x100> borrowed_group{};
  native_field<std::int32_t>(borrowed_group.data(), 0x88) = 0;
  const int page = native_field<const std::int32_t *>(screen, 0x280)[0];
  auto &pages = native_field<NativeVector>(screen, 0x1C0 + static_cast<std::size_t>(page) * 0x18);
  const auto saved = pages;
  const auto *entries = option_tab->entries.data();
  pages = {entries, entries + option_tab->entries.size(), entries + option_tab->entries.size()};
  original(screen, borrowed_group.data());
  pages = saved;
}

// Row dispatch (row, entry): our ids fall through to the common tail, which only
// enables the row; the choice row is built here afterwards like the game's own.
void options_build_row(void *row, const std::int32_t *entry) {
  reinterpret_cast<void (*)(void *, const std::int32_t *)>(options_build_row_hook.original)(row, entry);
  auto *state = option_row(entry);
  if (!state)
    return;

  auto *vtable = native_field<std::uintptr_t *>(row, 0);
  reinterpret_cast<void (*)(void *, int)>(vtable[0x278 / 8])(row, 0); // ◀ value ▶ style
  const auto &texts = state->choice_texts;
  const auto &values = state->choice_values;
  const NativeVector text_vector{texts.data(), texts.data() + texts.size(), texts.data() + texts.size()};
  const NativeVector value_vector{values.data(), values.data() + values.size(), values.data() + values.size()};
  reinterpret_cast<void (*)(void *, const NativeVector *, const NativeVector *, int)>(vtable[0x238 / 8])(
      row, &text_vector, &value_vector, state->current);
  reinterpret_cast<void (*)(void *, const wchar_t *)>(vtable[0x270 / 8])(row, state->label.c_str());
}

// Row event handler: kinds 5 and 6 are ◀ and ▶. The game writes ids up to 0x99 to
// its settings; ours are reported to the mod instead.
void options_row_event(void *row, void *event) {
  reinterpret_cast<void (*)(void *, void *)>(options_row_event_hook.original)(row, event);
  if (!event || !option_tab)
    return;
  const int kind = native_field<std::int32_t>(event, 0x88);
  auto *state = option_row(native_field<const std::int32_t *>(row, 0x218));
  if ((kind != 5 && kind != 6) || !state)
    return;

  const auto *values = native_field<const std::int32_t *>(row, 0x1E8);
  const auto *values_end = native_field<const std::int32_t *>(row, 0x1F0);
  const int index = native_field<std::int32_t>(row, 0x200);
  if (!values || index < 0 || index >= values_end - values)
    return;
  const int value = values[index];
  if (value < 0 || value >= static_cast<int>(state->choices.size()) || value == state->current)
    return;
  state->current = value;
  dispatch_mod_event(Event::OptionChanged, option_tab->owner, (state->action << 16) | value);
  // A mod switched on or off runs from now: a load event initializes newly
  // started mods and lets the tab's owner refresh its rows.
  if (take_mod_selection_change())
    dispatch_mod_event(Event::Configure, 0);
}

// Description panel refresh (screen): the game picks the text for the hovered
// row and lays it out with 0x1714E40, called from 0x1B2BF01.
void options_describe(void *screen) {
  option_description = nullptr;
  void *list = option_tab ? native_field<void *>(screen, 0x1A0) : nullptr;
  if (list) {
    void **rows = native_field<void **>(list, 0x1E0);
    const int count = native_field<std::int32_t>(list, 0x1F4);
    const auto hovered = reinterpret_cast<bool (*)(void *)>(native_ui_module + 0x1B39C90);
    for (int i = 0; rows && i < count; ++i) {
      if (!rows[i] || !hovered(rows[i]))
        continue;
      if (const auto *state = option_row(native_field<const std::int32_t *>(rows[i], 0x218)))
        option_description = state->description.c_str();
      break;
    }
  }
  reinterpret_cast<void (*)(void *)>(options_describe_hook.original)(screen);
  option_description = nullptr;
}

std::uintptr_t options_text_layout(void *text, const wchar_t *source, std::uintptr_t a3, std::uintptr_t a4,
                                   std::uintptr_t a5, std::uintptr_t a6, std::uintptr_t a7) {
  using Layout = std::uintptr_t (*)(void *, const wchar_t *, std::uintptr_t, std::uintptr_t, std::uintptr_t,
                                    std::uintptr_t, std::uintptr_t);
  if (option_description &&
      reinterpret_cast<std::uintptr_t>(_ReturnAddress()) == native_ui_module + 0x1B2BF01)
    source = option_description;
  return reinterpret_cast<Layout>(options_text_layout_hook.original)(text, source, a3, a4, a5, a6, a7);
}

std::optional<std::int64_t> invoke_options_service(Operation operation, std::uint64_t target, void *data,
                                                   std::uint32_t bytes) {
  if (operation == Operation::ReleaseUiRegistration) {
    if (option_tab && option_tab->owner == static_cast<int>(target))
      option_tab = nullptr;
    return std::nullopt; // menus and window rows release their own registrations too
  }
  if (operation != Operation::SetOptionsTab)
    return std::nullopt;
  if (!data || bytes != sizeof(OptionsTab) ||
      (script_callback_event != Event::Configure && script_callback_event != Event::Open))
    return -1;

  const auto &spec = *static_cast<const OptionsTab *>(data);
  const auto text = [](const wchar_t *value, std::size_t limit) {
    return value && wcsnlen_s(value, limit) < limit;
  };
  if (spec.owner <= 0 || spec.count < 1 || spec.count > 64 || !spec.rows || !text(spec.label, 32))
    return -1;

  auto tab = std::make_unique<OptionsTabState>();
  tab->owner = spec.owner;
  tab->label = spec.label;
  for (int i = 0; i < spec.count; ++i) {
    const auto &row = spec.rows[i];
    if (row.action < 0 || row.action > 0x7FFF || !text(row.label, 64) || !text(row.description, 1024) ||
        !row.choices || row.choice_count < 1 || row.choice_count > 16 || row.current < 0 ||
        row.current >= row.choice_count ||
        std::ranges::any_of(tab->rows, [&](const auto &other) { return other.action == row.action; }))
      return -1;
    OptionRowState state{row.action, row.current, row.label, row.description};
    for (int choice = 0; choice < row.choice_count; ++choice) {
      if (!text(row.choices[choice], 32))
        return -1;
      state.choices.emplace_back(row.choices[choice]);
      state.choice_values.push_back(choice);
    }
    for (const auto &choice : state.choices)
      state.choice_texts.push_back(choice.c_str());
    tab->rows.push_back(std::move(state));
    tab->entries.push_back({option_id_base + i, -1});
  }
  option_tab = tab.get();
  option_tabs.push_back(std::move(tab));
  return 0;
}
