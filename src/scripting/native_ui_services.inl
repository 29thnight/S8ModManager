#include "world_services.inl"

// Public service boundary. Each POD request is checked once before native use.
std::optional<std::int64_t> invoke_ui_service(Operation operation, std::uint64_t target, std::int64_t argument,
                                              void *data, std::uint32_t bytes) {
  if (const auto result = invoke_save_service(operation, target, argument, data, bytes))
    return result;

  if (const auto result = invoke_march_service(operation, target, argument, data, bytes))
    return result;

  if (const auto result = invoke_ai_service(operation, target, argument, data, bytes))
    return result;

  if (const auto result = invoke_officer_action_service(operation, target, argument, data, bytes))
    return result;

  if (const auto result = invoke_political_service(operation, target, argument, data, bytes))
    return result;
  if (const auto result = invoke_entity_data_service(operation, target, argument, data, bytes))
    return result;
  if (const auto result = invoke_world_service(operation, target, argument, data, bytes))
    return result;

  if (operation == Operation::RegisterMenu) {
    if (!data || bytes != sizeof(MenuItem) ||
        (script_callback_event != Event::Configure && script_callback_event != Event::Open))
      return -1;
    const auto &item = *static_cast<const MenuItem *>(data);
    const int count = item.surface == MenuSurface::Function ? 13 : 6;
    const bool valid_placement = item.surface == MenuSurface::Palace
                                     ? item.template_id == 0 && item.insert_after == 0
                                     : item.template_id >= 1 && item.template_id < count && item.insert_after >= 1 &&
                                           item.insert_after < count;
    if (static_cast<int>(item.surface) < 0 || static_cast<int>(item.surface) > 3 || item.action < 0 ||
        item.owner <= 0 || !valid_placement || !item.label || wcsnlen_s(item.label, 256) == 256)
      return -1;
    const auto found = std::find_if(menu_registrations.begin(), menu_registrations.end(), [&](const auto &value) {
      return value.surface == item.surface && value.action == item.action;
    });
    if (found != menu_registrations.end()) {
      found->label = item.label;
      found->owner = item.owner;
      found->template_id = item.template_id;
      found->insert_after = item.insert_after;
    } else {
      menu_registrations.push_back(
          {item.surface, item.action, item.template_id, item.insert_after, -1, item.label, item.owner});
    }
    return 0;
  }

  if (operation == Operation::RegisterWindowRow) {
    if (!data || bytes != sizeof(WindowRow) ||
        (script_callback_event != Event::Configure && script_callback_event != Event::Open))
      return -1;
    const auto &row = *static_cast<const WindowRow *>(data);
    if (row.slot < 4 || row.slot >= 10 || row.action < 0 || row.owner <= 0 || !row.label || !row.button ||
        wcsnlen_s(row.label, 256) == 256 || wcsnlen_s(row.button, 256) == 256)
      return -1;
    const auto found = std::find_if(window_registrations.begin(), window_registrations.end(),
                                    [&](const auto &value) { return value.slot == row.slot; });
    WindowRegistration registration{row.slot, row.action, row.label, row.button, row.owner};
    if (found == window_registrations.end())
      window_registrations.push_back(std::move(registration));
    else
      *found = std::move(registration);
    return 0;
  }

  if (operation == Operation::ReleaseUiRegistration) {
    std::erase_if(menu_registrations, [target](const auto &item) { return item.owner == target; });
    std::erase_if(window_registrations, [target](const auto &item) { return item.owner == target; });
    return 0;
  }

  if (operation == Operation::ReadSelection) {
    auto &session = native_ui_session;
    if (session.frames.empty() || session.frame().script_owner != target ||
        session.frame().page != NativeDialogStyle::MultiSelect)
      return -8;
    {
      std::lock_guard lock(native_ui_mutex);
      if (session.source.epoch != native_ui_publication.epoch)
        return -10;
    }
    const auto &selection = session.frames.back()->selection_values;
    if (!data || bytes % sizeof(int) || bytes < selection.size() * sizeof(int))
      return -1;
    std::copy(selection.begin(), selection.end(), static_cast<int *>(data));
    return selection.size();
  }

  // Restores the mod's persisted forced-effect mask; valid before a save is loaded.
  if (operation == Operation::SetJewelForceMask) {
    if (!data || bytes != sizeof(std::array<std::uint64_t, 8>))
      return -1;
    std::array<std::uint64_t, 8> mask{};
    std::memcpy(mask.data(), data, bytes);
    if (!set_jewel_force_mask(mask))
      return -9;
    std::lock_guard lock(native_ui_mutex);
    std::copy(mask.begin(), mask.end(), game_publication.mask);
    return 0;
  }

  if (operation == Operation::SetTraitCatalog)
    return set_trait_catalog(data, bytes);

  // The cheat overlay is a separate product; the mod manager has no overlay to open.
  if (operation == Operation::RequestOverlay)
    return static_cast<std::int64_t>(Status::MissingCapability);

  if (operation == Operation::ReadGameState) {
    if (!data || bytes != sizeof(UiPublication))
      return -1;
    std::lock_guard lock(native_ui_mutex);
    if (native_ui_session.source.epoch.value != game_publication.epoch)
      return -10;
    auto &output = *static_cast<UiPublication *>(data);
    output = game_publication;
    return 0;
  }

  if (operation == Operation::SubmitGameCommand) {
    if (!data || !bytes || bytes % sizeof(UiJewelCommand) || bytes > 256 * sizeof(UiJewelCommand))
      return -1;
    const auto *commands = static_cast<const UiJewelCommand *>(data);
    const auto count = bytes / sizeof(UiJewelCommand);
    std::lock_guard lock(native_ui_mutex);
    if (!game_publication.ready || !game_publication.has_bitmap)
      return -8;
    for (std::uint32_t i = 0; i < count; ++i) {
      if (commands[i].epoch != game_publication.epoch)
        return -10;
      if (commands[i].raw_id >= 272)
        return -1;
    }
    // Script callbacks run on the game thread, so the edit is applied in place.
    for (std::uint32_t i = 0; i < count; ++i)
      if (!apply_jewel_command(commands[i]))
        return -9;
    const auto mask = jewel_force_mask();
    std::copy(mask.begin(), mask.end(), game_publication.mask);
    return 0;
  }

  if (operation == Operation::SetWindowDirty) {
    const auto found = std::find_if(native_windows.begin(), native_windows.end(),
                                    [target](const auto &value) { return value.second.id == target; });
    if (found == native_windows.end())
      return -8;
    found->second.dirty = argument != 0;
    return 0;
  }

  if (operation >= Operation::SetTreeNodes && operation <= Operation::GetTreeCategory) {
    auto &session = native_ui_session;
    if (session.frames.empty() || session.frame().script_owner != target ||
        session.frame().page != NativeDialogStyle::GemTree)
      return -8;
    {
      std::lock_guard lock(native_ui_mutex);
      if (session.source.epoch != native_ui_publication.epoch)
        return -10;
    }
    if (script_callback_event == Event::Closing || script_callback_event == Event::Closed)
      return -8;
    if (operation == Operation::GetTreeCategory) {
      if (!native_tree_view.layout)
        return -8;
      void *tree = native_field<void *>(native_tree_view.layout, 0x288);
      return native_field<std::uint8_t>(tree, 0x1F8);
    }
    if (operation == Operation::SetTreeNodes) {
      if ((!data && bytes) || bytes % sizeof(TreeNode) || bytes > 512 * sizeof(TreeNode))
        return -1;
      const auto *nodes = static_cast<const TreeNode *>(data);
      for (std::uint32_t i = 0; i < bytes / sizeof(TreeNode); ++i)
        if (nodes[i].id >= 512 || nodes[i].opened > 1 || nodes[i].enabled > 1)
          return -1;
      native_tree_view.nodes.clear();
      for (std::uint32_t i = 0; i < bytes / sizeof(TreeNode); ++i)
        native_tree_view.nodes.emplace(nodes[i].id, nodes[i]);
      native_tree_view.refresh = true;
      return 0;
    }
    if (!data || bytes != sizeof(TreeFooter))
      return -1;
    const auto &footer = *static_cast<const TreeFooter *>(data);
    if (footer.action < 0 || footer.action > 1 || (footer.label && wcsnlen_s(footer.label, 256) == 256) ||
        (footer.action == 1 && footer.label))
      return -1;
    const auto found = std::find_if(native_tree_view.footers.begin(), native_tree_view.footers.end(),
                                    [&](const auto &item) { return item.action == footer.action; });
    TreeView::Footer item{footer.action, footer.enabled != 0, footer.label ? footer.label : L"",
                          footer.label != nullptr};
    if (found == native_tree_view.footers.end())
      native_tree_view.footers.push_back(std::move(item));
    else
      *found = std::move(item);
    return 0;
  }

  return std::nullopt;
}
