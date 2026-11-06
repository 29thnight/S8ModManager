// Included by the existing native UI backend. All entry points run inside its
// game-thread callback scope. These operations contain no editor state machine.
std::int64_t script_engine_invoke(scripting::Operation operation, std::uint64_t target, std::int64_t argument,
                                  void *data, std::uint32_t bytes) {
  using scripting::Operation;

  if (const auto result = invoke_battle_service(operation, target, argument, data, bytes))
    return *result;

  if (const auto result = invoke_ui_service(operation, target, argument, data, bytes))
    return *result;

  if (operation == Operation::ReadArchiveResource) {
    if (target > UINT32_MAX || argument < 0 || (!data && bytes))
      return -1;
    wchar_t executable[MAX_PATH];
    const auto length = GetModuleFileNameW(nullptr, executable, MAX_PATH);
    if (!length || length >= MAX_PATH)
      return -8;
    wchar_t filename[24];
    swprintf_s(filename, L"0x%08x.fdata", static_cast<unsigned>(target));
    const auto path = std::filesystem::path(executable).parent_path() / L"0002" / filename;
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream)
      return -8;
    const auto size = static_cast<std::int64_t>(stream.tellg());
    struct Record {
      char magic[8];
      std::uint64_t total, stored, decoded;
      std::uint32_t fields[4];
    } record{};
    constexpr std::uint64_t limit = 256 * 1024 * 1024;
    if (size < static_cast<std::int64_t>(sizeof(record)) || argument > size - static_cast<std::int64_t>(sizeof(record)) ||
        !stream.seekg(argument).read(reinterpret_cast<char *>(&record), sizeof(record)) ||
        std::memcmp(record.magic, "IDRK0000", 8) != 0 || record.total < sizeof(record) ||
        record.total > static_cast<std::uint64_t>(size - argument) ||
        record.stored > record.total - sizeof(record) || record.stored > limit || record.decoded > limit)
      return -1;

    if (!data)
      return record.decoded;
    if (bytes != record.decoded)
      return -1;
    stream.seekg(argument + record.total - record.stored);
    if (record.stored == record.decoded)
      return stream.read(static_cast<char *>(data), bytes) ? static_cast<std::int64_t>(bytes) : -8;

    std::vector<unsigned char> compressed(static_cast<std::size_t>(record.stored));
    if (!stream.read(reinterpret_cast<char *>(compressed.data()), compressed.size()))
      return -8;
    auto *output = static_cast<unsigned char *>(data);
    for (const std::size_t wrapper : {10, 4}) {
      std::size_t source = 0, written = 0;
      while (written < bytes && compressed.size() - source >= wrapper) {
        std::uint32_t block_size = 0;
        std::memcpy(&block_size, compressed.data() + source, wrapper == 10 ? 2 : 4);
        source += wrapper;
        if (!block_size || block_size > compressed.size() - source)
          break;
        const std::size_t end = source + block_size;
        const std::size_t block_start = written;
        const std::size_t output_end = std::min<std::size_t>(written + 16384, bytes);
        auto read_length = [&](std::size_t count) {
          if (count == 15) {
            while (source < end) {
              const auto extension = compressed[source++];
              count += extension;
              if (extension != 255)
                return count;
            }
            return SIZE_MAX;
          }
          return count;
        };
        while (source < end) {
          const auto token = compressed[source++];
          const auto literals = read_length(token >> 4);
          if (literals > end - source || literals > output_end - written)
            break;
          std::memcpy(output + written, compressed.data() + source, literals);
          source += literals;
          written += literals;
          if (source == end)
            break;
          if (end - source < 2 || output_end - written < 4)
            break;
          const auto distance = static_cast<std::size_t>(compressed[source] | (compressed[source + 1] << 8));
          source += 2;
          const auto match = read_length(token & 15);
          if (!distance || distance > written - block_start || match > output_end - written - 4)
            break;
          for (std::size_t i = 0; i < match + 4; ++i, ++written)
            output[written] = output[written - distance];
        }
        if (source != end || written != output_end)
          break;
      }
      if (written == bytes && (source == compressed.size() ||
          (compressed.size() - source == 2 && !compressed[source] && !compressed[source + 1])))
        return bytes;
    }
    return -8;
  }

  if (operation == Operation::GetSession) {
    std::lock_guard lock(native_ui_mutex);
    return game_publication.ready ? static_cast<std::int64_t>(game_publication.epoch) : -8;
  }

  auto &session = native_ui_session;

  {
    std::lock_guard lock(native_ui_mutex);

    if (session.source.epoch != native_ui_publication.epoch)
      return -10;
  }

  if (operation == Operation::RegisterUiSprite || operation == Operation::FindUiSprite ||
      operation == Operation::ApplyUiSprite) {
    if (!session.source.ready || !data || bytes < 4 || bytes > 512 || bytes % sizeof(wchar_t))
      return -1;
    const auto *name = static_cast<const wchar_t *>(data);
    const auto length = bytes / sizeof(wchar_t) - 1;
    if (wcsnlen_s(name, length + 1) != length)
      return -1;

    static domain::GameEpoch sprite_epoch{};
    static std::unordered_map<std::wstring, int> sprites;
    if (sprite_epoch != session.source.epoch) {
      sprites.clear();
      sprite_epoch = session.source.epoch;
    }

    if (operation == Operation::RegisterUiSprite) {
      if (target || argument < 1 || argument > 0xFFFF)
        return -1;
      sprites.insert_or_assign(std::wstring(name, length), static_cast<int>(argument));
      return 0;
    }

    const auto sprite = sprites.find(std::wstring(name, length));
    if (operation == Operation::FindUiSprite)
      return target || argument ? -1 : sprite == sprites.end() ? 0 : sprite->second;

    if (sprite == sprites.end() || session.frames.empty())
      return -8;
    auto &frame = session.frame();
    const auto row = static_cast<std::uint32_t>(static_cast<std::uint64_t>(argument) >> 32);
    const auto image_slot = static_cast<std::uint32_t>(argument);
    if (target != static_cast<std::uint64_t>(frame.script_owner) || row >= frame.buttons.size())
      return -8;
    if ((frame.page != NativeDialogStyle::Form && frame.page != NativeDialogStyle::CompactForm) ||
        std::none_of(frame.visible.begin(), frame.visible.end(),
                     [row](const auto &item) { return item.action == row; }))
      return -8;

    if (frame.buttons[row] && !apply_native_button_sprite(frame.buttons[row], image_slot, sprite->second))
      return -8;

    auto existing = std::find_if(frame.row_sprites.begin(), frame.row_sprites.end(),
                                 [row, image_slot](const auto &item) {
                                   return item.row == row && item.slot == image_slot;
                                 });
    if (existing == frame.row_sprites.end())
      frame.row_sprites.push_back({row, image_slot, sprite->second});
    else
      existing->resource = sprite->second;
    return 0;
  }

  if (operation == Operation::OpenCouncilRoom || operation == Operation::CloseCouncilRoom) {
    const bool opening = operation == Operation::OpenCouncilRoom;
    if (target || data || bytes || argument < 0 || argument > (opening ? 1 : 0))
      return -1;
    if (!session.frames.empty() || opening == council_room_active())
      return -8;

    if (opening) {
      if (!session.source.ready)
        return -8;
      void *in_game = council_room_in_game();
      void *root = *reinterpret_cast<void **>(native_ui_module + 0x2E98BC8);
      if (!in_game || !root || native_field<int>(root, 0xD0) != 7 || city_return_in_game)
        return -8;
      open_council_room(in_game, argument != 0);
    } else {
      close_council_room();
    }
    return 0;
  }

  if (operation == Operation::ReadInputText) {
    if (!data || bytes < 2 || bytes > 8192 || bytes % 2 || session.input_result.size() + 1 > bytes / 2)
      return -1;

    std::copy(session.input_result.begin(), session.input_result.end(), static_cast<wchar_t *>(data));
    static_cast<wchar_t *>(data)[session.input_result.size()] = 0;
    return session.input_result.size();
  }

  if (script_callback_event == scripting::Event::Closing || script_callback_event == scripting::Event::Closed)
    return -8;

  if ((operation == Operation::ShowDialog || operation == Operation::CloseDialog) &&
      (script_callback_event == scripting::Event::TextInput || script_callback_event == scripting::Event::NumberInput))
    return -8;

  if (operation == Operation::SetDialogTabs) {
    // Tabs for the next ShowDialog of this owner (a form); rows are checked by the caller.
    if (!data || bytes != sizeof(scripting::DialogTabs) || argument || target == 0 || target > INT32_MAX)
      return -1;
    const auto &tabs = *static_cast<const scripting::DialogTabs *>(data);
    if (!tabs.labels || tabs.count < 2 || tabs.count > 8)
      return -1;
    for (int i = 0; i < tabs.count; ++i)
      if (!tabs.labels[i] || !*tabs.labels[i] || wcsnlen_s(tabs.labels[i], 64) == 64)
        return -1;
    if (std::any_of(std::begin(tabs.row_tabs), std::end(tabs.row_tabs), [&](auto tab) { return tab >= tabs.count; }))
      return -1;

    session.pending_tabs_owner = static_cast<int>(target);
    session.pending_tab_labels.assign(tabs.labels, tabs.labels + tabs.count);
    std::copy(std::begin(tabs.row_tabs), std::end(tabs.row_tabs), session.pending_row_tabs.begin());
    return 0;
  }

  if (operation == Operation::ShowDialog) {
    if (!data || bytes != sizeof(scripting::DialogSpec) || session.frames.size() >= 16 || session.text_input_open)
      return -1;

    const auto &spec = *static_cast<const scripting::DialogSpec *>(data);

    if (spec.owner <= 0 || static_cast<int>(spec.kind) < 0 || static_cast<int>(spec.kind) > 8 || spec.count < 0 ||
        spec.count > ((spec.kind == DialogKind::List || spec.kind == DialogKind::MultiSelect) ? 8192 : 64) ||
        !spec.title || !spec.caption || wcsnlen_s(spec.title, 4096) == 4096 || wcsnlen_s(spec.caption, 4096) == 4096 ||
        (spec.count && !spec.rows))
      return -1;

    if (spec.kind == DialogKind::CityMap) {
      if (!council_room_active() || !session.frames.empty() || spec.reserved < 1 || spec.reserved > 51 ||
          spec.count < 1 || spec.count > 51)
        return -1;
      auto *root = native_field<void *>(reinterpret_cast<void *>(native_ui_module + 0x2E98BC8), 0);
      for (int index = -1; index < spec.count; ++index) {
        const int id = index == -1 ? spec.reserved : spec.rows[index].action;
        if (id < 1 || id > 51)
          return -1;
        void *city = native_field<void *>(root, 0x546808 + id * sizeof(void *));
        if (!city || native_field<std::uintptr_t>(city, 0) != native_ui_module + 0x2689638)
          return -8;
      }
    }

    for (const auto &frame : session.frames)
      if (frame->script_owner == spec.owner)
        return -8;

    std::array<bool, 64> actions{};

    for (int i = 0; i < spec.count; ++i) {
      const auto &row = spec.rows[i];

      if (row.action < 0 || row.action > 65535 || row.height < 32 || row.height > 600 || !row.label || !row.text ||
          wcsnlen_s(row.label, 4096) == 4096 || wcsnlen_s(row.text, 4096) == 4096)
        return -1;

      if (spec.kind != DialogKind::List && spec.kind != DialogKind::MultiSelect) {
        if (row.action >= 64 || actions[row.action])
          return -1;

        actions[row.action] = true;
      }
    }

    if (spec.selection_count < 0 || spec.selection_count > spec.count ||
        (spec.selection_count && (!spec.selection || spec.kind != DialogKind::MultiSelect)))
      return -1;
    for (int i = 0; i < spec.selection_count; ++i) {
      const int action = spec.selection[i];
      bool found = false;
      for (int row = 0; row < spec.count; ++row)
        found |= spec.rows[row].action == action;
      if (!found || std::find(spec.selection, spec.selection + i, action) != spec.selection + i)
        return -1;
    }

    constexpr NativeDialogStyle styles[]{NativeDialogStyle::List,          NativeDialogStyle::Form,
                                         NativeDialogStyle::CompactForm,   NativeDialogStyle::Text,
                                         NativeDialogStyle::MultilineText, NativeDialogStyle::Message,
                                         NativeDialogStyle::GemTree,       NativeDialogStyle::MultiSelect,
                                         NativeDialogStyle::CityMap};
    if (spec.kind == DialogKind::GemTree) {
      if (std::any_of(session.frames.begin(), session.frames.end(),
                      [](const auto &frame) { return frame->page == NativeDialogStyle::GemTree; }))
        return -8;
      native_tree_view = {};
      suspend_native_window();
    }
    show_native_dialog(styles[static_cast<int>(spec.kind)], spec);
    return 0;
  }

  if (session.frames.empty() || session.frame().script_owner != target || session.text_input_open)
    return -8;

  auto &frame = session.frame();

  if (operation == Operation::HideFormDecoration) {
    if (data || bytes || argument || frame.page != NativeDialogStyle::Form)
      return -1;
    if (frame.dialog)
      return -8;
    frame.hide_form_decoration = true;
    return 0;
  }

  if (operation == Operation::SetFormButtonBounds) {
    if (!data || bytes != sizeof(std::array<int, 4>) || argument < 0 || argument >= 64 ||
        (frame.page != NativeDialogStyle::Form && frame.page != NativeDialogStyle::CompactForm) ||
        std::none_of(frame.visible.begin(), frame.visible.end(),
                     [argument](const auto &row) { return row.action == argument; }))
      return -1;
    if (frame.dialog)
      return -8;
    const auto &bounds = *static_cast<const std::array<int, 4> *>(data);
    const auto [x, y, width, height] = bounds;
    if (width < 1 || height < 1 || width > 1920 || height > 1080 ||
        x < 0 || y < 0 || x > 1920 - width || y > 1080 - height)
      return -1;
    frame.button_bounds[argument] = bounds;
    return 0;
  }

  if (operation == Operation::SetFormButtonIcon) {
    if (!data || bytes != sizeof(std::array<int, 4>) || argument < 0 || argument >= 64 ||
        (frame.page != NativeDialogStyle::Form && frame.page != NativeDialogStyle::CompactForm) ||
        !frame.button_bounds[argument][2])
      return -1;
    if (frame.dialog)
      return -8;
    const auto &icon = *static_cast<const std::array<int, 4> *>(data);
    const auto [resource, x, y, size] = icon;
    if (resource < 0 || size < 1 || size > 1080 || x < 0 || y < 0 || x > 1920 - size || y > 1080 - size)
      return -1;
    frame.button_icons[argument] = icon;
    return 0;
  }

  if (operation == Operation::SetRowBadge) {
    if (!data || bytes != sizeof(scripting::RowBadge) || argument < 0 || argument >= 64 ||
        (frame.page != NativeDialogStyle::Form && frame.page != NativeDialogStyle::CompactForm) ||
        std::none_of(frame.visible.begin(), frame.visible.end(),
                     [argument](const auto &row) { return row.action == argument; }))
      return -1;
    const auto badge = *static_cast<const scripting::RowBadge *>(data);
    if (badge.resource < 1 || badge.width < 1 || badge.width > 1110 || badge.height < 1 || badge.height > 128 ||
        badge.palette < 0 || badge.palette > 255)
      return -1;
    auto &current = frame.row_badges[argument];
    if (frame.dialog) {
      // A built row can only restyle the badge it was laid out with.
      if (!current.resource || badge.width != current.width || badge.height != current.height)
        return -8;
      if (void *image = frame.row_nodes[argument][2]) {
        using ApplyImage = void (*)(void *, int, void *, int, int, int, bool);
        reinterpret_cast<ApplyImage>(native_ui_module + 0x1D122D0)(image, badge.resource, frame.dialog, 1, 1, 0, true);
      }
      if (void *text = frame.text_nodes[argument])
        reinterpret_cast<void (*)(void *, int)>(native_ui_module + 0x1D167B0)(text, badge.palette);
    }
    current = badge;
    return 0;
  }

  if (operation == Operation::SetFormButtonLabel) {
    if (!data || bytes != sizeof(std::array<int, 3>) || argument < 0 || argument >= 64 ||
        (frame.page != NativeDialogStyle::Form && frame.page != NativeDialogStyle::CompactForm) ||
        !frame.button_bounds[argument][2])
      return -1;
    if (frame.dialog)
      return -8;
    const auto &style = *static_cast<const std::array<int, 3> *>(data);
    const auto [x, y, font_size] = style;
    if (x < 0 || x >= 1920 || y < 0 || y >= 1080 || font_size < 1 || font_size > 127)
      return -1;
    frame.button_labels[argument] = style;
    return 0;
  }

  if (operation == Operation::DisableDialogRow) {
    if (data || bytes || argument < 0 || argument >= 64 ||
        (frame.page != NativeDialogStyle::Form && frame.page != NativeDialogStyle::CompactForm) ||
        std::none_of(frame.visible.begin(), frame.visible.end(),
                     [argument](const auto &row) { return row.action == argument; }))
      return -1;

    frame.disabled[argument] = true;
    if (void *button = frame.buttons[argument]) {
      auto *vtable = native_field<std::uintptr_t *>(button, 0);
      reinterpret_cast<void (*)(void *, bool)>(vtable[0x110 / 8])(button, false);
      reinterpret_cast<void (*)(void *, bool)>(vtable[0x120 / 8])(button, false);
    }
    return 0;
  }

  if (operation == Operation::InputText) {
    if (!data || bytes != sizeof(scripting::TextInput) ||
        (frame.page != NativeDialogStyle::Text && frame.page != NativeDialogStyle::MultilineText))
      return -1;

    const auto &input = *static_cast<const scripting::TextInput *>(data);

    if (input.multiline != (frame.page == NativeDialogStyle::MultilineText ? 1 : 0))
      return -1;

    if (!input.initial || input.limit < 1 || input.limit > 513 || wcsnlen_s(input.initial, 4096) == 4096)
      return -1;

    frame.input_initial = input.initial;
    frame.input_limit = input.limit;

    if (frame.dialog)
      begin_native_ui_text_input(frame.dialog);
    return 0;
  }

  if (!frame.dialog)
    return -8;

  if (operation == Operation::CloseDialog) {
    auto *vtable = native_field<std::uintptr_t *>(frame.dialog, 0);
    reinterpret_cast<void (*)(void *, int)>(vtable[0x2C8 / 8])(frame.dialog, argument ? 0x7FFFFFFD : 0x7FFFFFFE);
    return 0;
  }

  if (operation == Operation::SetRow) {
    if (!data || bytes != sizeof(scripting::UiRow))
      return -1;

    const auto &value = *static_cast<const scripting::UiRow *>(data);

    if (value.action < 0 || value.action > 65535 || !value.label || !value.text ||
        wcsnlen_s(value.label, 4096) == 4096 || wcsnlen_s(value.text, 4096) == 4096)
      return -1;

    for (auto &row : frame.visible)
      if (row.action == value.action) {
        row.label = value.label;
        row.text = value.text;

        if (row.action < frame.buttons.size() && frame.buttons[row.action]) {
          void *button = frame.buttons[row.action];

          auto *vtable = native_field<std::uintptr_t *>(button, 0);
          reinterpret_cast<void (*)(void *, const wchar_t *)>(vtable[0x298 / 8])(button, row.label.c_str());
        }

        if (row.action < frame.text_nodes.size() && frame.text_nodes[row.action])
          reinterpret_cast<void (*)(void *, const wchar_t *)>(native_ui_module + 0x1D154E0)(
              frame.text_nodes[row.action], row.text.c_str());
        return 0;
      }

    return -1;
  }

  if (operation == Operation::InputNumber) {
    if (!data || bytes != sizeof(scripting::NumberInput))
      return -1;

    const auto input = *static_cast<const scripting::NumberInput *>(data);

    if (input.minimum < 0 || input.maximum < input.minimum || input.initial < input.minimum ||
        input.initial > input.maximum)
      return -1;

    void *dialog = frame.dialog;

    const int owner = frame.script_owner;
    session.text_input_open = true;
    session.text_input_owner = dialog;
    session.input.input_number(native_ui_module, input.initial, input.minimum, input.maximum,
                               [dialog, owner](bool accepted, int value) {
                                 auto &current = native_ui_session;
                                 current.text_input_open = false;
                                 current.text_input_owner = nullptr;
                                 dispatch_mod_event(scripting::Event::NumberInput, owner, accepted ? value : -1);
                                 // Keypad teardown already leaves its parent visible; only enable input.
                                 auto *vtable = native_field<std::uintptr_t *>(dialog, 0);
                                 reinterpret_cast<void (*)(void *, bool)>(vtable[0x120 / 8])(dialog, true);
                                 dispatch_mod_event(scripting::Event::Resume, owner);
                               });
    return 0;
  }

  return -1;
}
