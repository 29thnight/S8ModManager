// Binding for the game's GemUnlockUILayout resource. Values and actions come from C#.
ui_detail::Hook tree_category_filter_hook{};

int native_tree_category_allowed(void *categories, std::uint8_t category) {
  void *layout = native_tree_view.layout;
  if (layout && categories == native_field<void *>(layout, 0x298))
    return category >= 1 && category <= 11;
  return reinterpret_cast<int (*)(void *, std::uint8_t)>(tree_category_filter_hook.original)(categories, category);
}

void native_tree_visual(void *gem, int animation) {
  void *layout = native_tree_view.layout;
  if (layout) {
    void *trees = native_field<void *>(layout, 0x288);
    void *current = native_field<void *>(trees, 0x198 + sizeof(void *) * native_field<std::uint8_t>(trees, 0x1F8));
    const auto node = native_tree_view.nodes.find(native_field<std::uint16_t>(gem, 0x1D8));
    if (native_field<void *>(gem, 8) == current && node != native_tree_view.nodes.end()) {
      static constexpr int closed[] = {0, 1, 1, 1, 3, 5, 5};
      static constexpr int opened[] = {6, 7, 7, 7, 6, 7, 8};
      const int state = native_field<int>(gem, 0x1A0);
      animation = node->second.opened ? opened[state] : closed[state];
    }
  }
  reinterpret_cast<void (*)(void *, int)>(tree_visual_hook.original)(gem, animation);
}

void update_native_tree(void *layout, void *event) {
  if (layout != native_tree_view.layout) {
    reinterpret_cast<void (*)(void *, void *)>(tree_update_hook.original)(layout, event);
    return;
  }

  if (native_tree_view.suspended)
    return;

  if (!native_tree_view.shown) {
    void *dialog = native_ui_session.frame().dialog;
    void *controller = native_field<void *>(dialog, 0x238);

    if (native_field<int>(controller, 0x1C8) != 1)
      return;

    // The wrapper's full-screen input blocker otherwise intercepts every
    // tree click. Wait until its Show has completed before removing it.
    void *blocker = native_field<void *>(dialog, 0x248);

    auto *blocker_vtable = native_field<std::uintptr_t *>(blocker, 0);
    reinterpret_cast<void (*)(void *, bool)>(blocker_vtable[0x108 / 8])(blocker, false);
    void *animation = native_field<void *>(layout, 0x1E0);
    // Show returns without doing anything while a footer is animating.
    // Leave tree_shown false so the wrapper retries once hiding finishes.

    if (reinterpret_cast<bool (*)(void *)>(native_ui_module + 0x1698930)(animation) ||
        reinterpret_cast<bool (*)(void *)>(native_ui_module + 0x16989B0)(animation))
      return;

    native_field<bool>(animation, 0x1CC) = false;

    auto *vtable = native_field<std::uintptr_t *>(layout, 0);
    reinterpret_cast<void (*)(void *, bool)>(vtable[0x108 / 8])(layout, true);
    // Show reattaches the shared footer only if its owner is active already.
    // The retained CDataSelect owns the shared caption stack. Showing this
    // unmanaged tree must not push another caption on every child return:
    // destroying the tree does not pop those entries, leaking the title.
    const auto animation_flags = native_field<std::uint32_t>(animation, 0x1C0);
    native_field<std::uint32_t>(animation, 0x1C0) = animation_flags & ~0x5u;
    reinterpret_cast<void (*)(void *, int, int)>(native_ui_module + 0x1697F90)(animation, 0x589, 0);
    native_field<std::uint32_t>(animation, 0x1C0) = animation_flags;

    auto *dialog_vtable = native_field<std::uintptr_t *>(dialog, 0);
    reinterpret_cast<void (*)(void *, bool)>(dialog_vtable[0x120 / 8])(dialog, true);
    // Hiding the paper's texture pane alone leaves its two native BG
    // controls hit-testable above the tree on the wrapper's input layer.
    void *backgrounds = native_field<void *>(dialog, 0x180);
    void *background_handle = native_field<void *>(backgrounds, 8);

    const auto background_index = native_field<std::uint32_t>(background_handle, 0);
    auto **background_heads = *reinterpret_cast<void ***>(native_ui_module + 0x34C8630);

    for (void *node = background_heads[background_index]; node; node = native_field<void *>(node, 8)) {
      void *entry = native_field<void *>(node, 0);

      const int id = native_field<int>(entry, 0);

      if (id != 2 && id != 3)
        continue;

      void *background = native_field<void *>(entry, 8);

      auto *background_vtable = native_field<std::uintptr_t *>(background, 0);
      reinterpret_cast<void (*)(void *, bool)>(background_vtable[0x108 / 8])(background, false);
      reinterpret_cast<void (*)(void *, bool)>(background_vtable[0x120 / 8])(background, false);
    }

    reinterpret_cast<void (*)(void *, bool)>(vtable[0x238 / 8])(layout, true);
    native_tree_view.shown = true;
    native_tree_view.categories_bound = false;
  }

  reinterpret_cast<void (*)(void *, void *)>(tree_update_hook.original)(layout, event);
  // The normal city state consumes category selection. This editor does not
  // run that state (it spends courage), so switch only the native tree UI.
  void *tree = native_field<void *>(layout, 0x288);
  void *category_list = native_field<void *>(native_field<void *>(layout, 0x298), 0x1A0);

  const auto category = native_field<std::uint8_t>(category_list, 0x26C);

  if (category != native_field<std::uint8_t>(tree, 0x1F8)) {
    reinterpret_cast<void (*)(void *, std::uint8_t)>(native_ui_module + 0x1349220)(tree, category);
    reinterpret_cast<void (*)(void *)>(native_ui_module + 0x15ECD90)(layout);
    native_tree_view.categories_bound = false;
  }

  if (native_field<int>(layout, 0x2C4) == 1)
    native_field<int>(layout, 0x2C4) = 0;

  if (native_field<int>(layout, 0x2C4) == 2) {
    native_field<int>(layout, 0x2C4) = 0;
    dispatch_mod_event(Event::TreeClick, native_ui_session.frame().script_owner, -3);
    return;
  }

  if (native_tree_view.refresh) {
    native_tree_view.refresh = false;
    reinterpret_cast<void (*)(void *)>(native_ui_module + 0x15ECD90)(layout);
    native_tree_view.categories_bound = false;
  }

  for (const auto &item : native_tree_view.footers) {
    void *footer = native_field<void *>(layout, 0x1E8);
    reinterpret_cast<void (*)(void *, int, bool)>(native_ui_module + 0x1693230)(footer, item.action, item.enabled);
    if (item.action == 0 && item.custom_label) {
      void *bar = native_field<void *>(footer, 0x1A0);
      void *bar_layout = native_field<void *>(bar, 0x1A0);
      void *button = native_field<void *>(bar_layout, 0x1D0);
      reinterpret_cast<void (*)(void *, int, const wchar_t *, int)>(native_ui_module + 0x1D17190)(
          static_cast<std::byte *>(button) + 0x140, 23, item.label.c_str(), 0);
    }
  }

  {
    void *handle = native_field<void *>(category_list, 0x68);

    const auto index = native_field<std::uint32_t>(handle, 0);
    auto **heads = *reinterpret_cast<void ***>(native_ui_module + 0x34C8630);

    for (void *node = heads[index]; node; node = native_field<void *>(node, 8)) {
      void *button = native_field<void *>(node, 0);

      if (native_field<std::uintptr_t>(button, 0) != native_ui_module + 0x259FF20)
        continue;

      // The editor can switch trees even when ordinary unlocking is unavailable.
      auto *vtable = native_field<std::uintptr_t *>(button, 0);
      reinterpret_cast<void (*)(void *, bool)>(vtable[0x110 / 8])(button, true);
      if (native_tree_view.categories_bound)
        continue;

      const auto category = native_field<std::uint8_t>(button, 0x1C8);
      std::function<void(void *)> clicked = [layout, category_list, category](void *) {
        if (native_tree_view.suspended)
          return;

        native_field<std::uint8_t>(category_list, 0x26C) = category;
        void *tree = native_field<void *>(layout, 0x288);
        reinterpret_cast<void (*)(void *, std::uint8_t)>(native_ui_module + 0x1349220)(tree, category);
        reinterpret_cast<void (*)(void *)>(native_ui_module + 0x15ECD90)(layout);
        native_tree_view.categories_bound = false;
      };
      // RegisterCallback keeps an occupied event unchanged. Release the
      // game's category-selection callback before installing the editor's.
      void *event_handle = native_field<void *>(button, 0x80);

      if (event_handle) {
        const auto event_index = native_field<std::uint32_t>(event_handle, 0);

        for (void *event_node = heads[event_index]; event_node; event_node = native_field<void *>(event_node, 8)) {
          void *entry = native_field<void *>(event_node, 0);

          if (native_field<int>(entry, 0) != 0x7FFFFFFE)
            continue;
          *reinterpret_cast<std::function<void(void *)> *>(static_cast<std::byte *>(entry) + 8) = nullptr;
          break;
        }
      }

      reinterpret_cast<void (*)(void *, int, void *)>(native_ui_module + 0xEF40)(button, 0x7FFFFFFE, &clicked);
    }

    native_tree_view.categories_bound = true;
  }

  // Apply the managed enabled/opened presentation to native gem controls.
  void *current_tree = native_field<void *>(tree, 0x198 + sizeof(void *) * native_field<std::uint8_t>(tree, 0x1F8));
  void *children = native_field<void *>(current_tree, 0x68);

  const auto child_index = native_field<std::uint32_t>(children, 0);
  auto **child_heads = *reinterpret_cast<void ***>(native_ui_module + 0x34C8630);

  for (void *node = child_heads[child_index]; node; node = native_field<void *>(node, 8)) {
    void *gem = native_field<void *>(node, 0);

    auto *gem_vtable = native_field<std::uintptr_t *>(gem, 0);

    if (reinterpret_cast<std::uintptr_t>(gem_vtable) != native_ui_module + 0x25A0A28)
      continue;

    const auto id = native_field<std::uint16_t>(gem, 0x1D8);
    const auto item = native_tree_view.nodes.find(id);
    if (item == native_tree_view.nodes.end())
      continue;

    reinterpret_cast<void (*)(void *, bool)>(gem_vtable[0x110 / 8])(gem, item->second.enabled != 0);
    void *marker = native_field<void *>(gem, 0x1E0);
    if (marker)
      reinterpret_cast<void (*)(void *, int)>(native_ui_module + 0x134A770)(marker, item->second.opened ? 2 : 0);
  }
}

void update_native_tree_dialog(void *dialog, void *event) {
  reinterpret_cast<void (*)(void *, void *)>(tree_dialog_hook.original)(dialog, event);
  if (native_tree_view.layout && !native_tree_view.shown && !native_ui_session.frames.empty() &&
      native_ui_session.frame().dialog == dialog && native_ui_session.frame().page == NativeDialogStyle::GemTree &&
      native_field<int>(dialog, 0x230) == -1)
    update_native_tree(native_tree_view.layout, event);
}

void native_tree_clicked(void *callback, void **argument) {
  if (native_field<void *>(callback, 8) != native_tree_view.layout) {
    reinterpret_cast<void (*)(void *, void **)>(tree_click_hook.original)(callback, argument);
    return;
  }
  if (*argument && !native_tree_view.suspended) {
    const auto id = native_field<std::uint16_t>(*argument, 0x1D8);
    const auto found = native_tree_view.nodes.find(id);
    if (found != native_tree_view.nodes.end() && found->second.enabled)
      dispatch_mod_event(Event::TreeClick, native_ui_session.frame().script_owner, id);
  }
}

void native_tree_footer_clicked(void *callback, void **argument) {
  if (native_field<void *>(callback, 8) != native_tree_view.layout) {
    reinterpret_cast<void (*)(void *, void **)>(tree_footer_hook.original)(callback, argument);
    return;
  }
  if (*argument && !native_tree_view.suspended)
    dispatch_mod_event(Event::TreeClick, native_ui_session.frame().script_owner,
                       -1 - native_field<int>(*argument, 0x88));
}

void native_tree_back_clicked(void *callback, void **argument) {
  if (native_field<void *>(callback, 8) != native_tree_view.layout) {
    reinterpret_cast<void (*)(void *, void **)>(tree_back_hook.original)(callback, argument);
    return;
  }
  if (!native_tree_view.suspended)
    dispatch_mod_event(Event::TreeClick, native_ui_session.frame().script_owner, -3);
}
