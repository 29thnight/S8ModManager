#include "native_ui.h"
#include "native_ui_input.h"
#include "script_api.h"
#include "engine_services.h"
#include "../domain/city_management.h"
#include "../domain/ids.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <cwchar>
#include <cstdio>
#include <expected>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>
#include <unordered_map>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <bcrypt.h>
#include <CommCtrl.h>

#pragma comment(lib, "Comctl32.lib")

extern "C" int S8ScriptDispatch(const s8::scripting::EngineApi *, s8::scripting::Event, int, int);

namespace s8::scripting {
  const Connection &game_connection();

  namespace ui_detail {
    UiHookApi hooks{};

    struct Hook {
      void *target;
      void *original;
    };

    std::expected<Hook, int> create_hook(void *target, void *detour) {
      Hook hook{target, nullptr};
      const int result = hooks.create(target, detour, &hook.original);

      if (result != 0)
        return std::unexpected(result);

      return hook;
    }

    int enable_hook(const Hook &hook) {
      return hooks.enable(hook.target);
    }

    int disable_hook(const Hook &hook) {
      return hooks.disable(hook.target);
    }
  } // namespace ui_detail

  namespace {

    struct NativeListRow {
      std::wstring label;
      std::wstring text;
      std::size_t action;
      int height = 64;
    };

    struct NativeListCell {
      std::int32_t kind = 1, reserved = 0;
      const wchar_t *text = nullptr;
      std::int32_t color = 0x7FFFFFFF, padding = 0;
      void *extra = nullptr;

      std::uint64_t flags = 0;
    };

    static_assert(sizeof(NativeListCell) == 0x28);

    const wchar_t *native_list_label(const NativeListRow *row) {
      return row->label.c_str();
    }

    const wchar_t *native_list_text(const NativeListRow *row) {
      return row->text.c_str();
    }

    NativeListCell *native_label_cell(NativeListCell *output, const NativeListRow *row) {
      *output = {};
      output->text = row->label.c_str();
      return output;
    }

    NativeListCell *native_text_cell(NativeListCell *output, const NativeListRow *row) {
      *output = {};
      output->text = row->text.c_str();
      return output;
    }

    // CCmnInfoList's column descriptor: use editor-owned getters and formatters
    // rather than the original PersonalityData column functions.
    struct NativeListColumn {
      std::int32_t id, reserved;
      const wchar_t *heading;
      std::int32_t width, kind;
      const wchar_t *(*get)(const NativeListRow *);
      void *unused1;
      NativeListCell *(*format)(NativeListCell *, const NativeListRow *);
      void *unused2;
      void *unused3;
      void *count;

      std::uint64_t options;
      std::uint64_t unused4;
    };

    static_assert(sizeof(NativeListColumn) == 0x58);

    std::array<NativeListColumn, 2> native_list_columns{
        {{0, 0, L"항목", 260, 1, native_list_label, nullptr, native_label_cell, nullptr, nullptr, nullptr, 0x10000FFFF,
          0},
         {1, 0, L"설명", 1140, 1, native_list_text, nullptr, native_text_cell, nullptr, nullptr, nullptr, 0x10000FFFF,
          0}}};

    const std::array<int, 3> native_column_order{0, 1, -1};

    struct NativeListTab {
      const int *columns;
      const wchar_t *label;
      std::uint64_t options;
    };

    // The dialog resolves message IDs in-place; 0xFFFF keeps the supplied label.
    std::array<NativeListTab, 2> native_list_tabs{{{native_column_order.data(), L"기본", 0xFFFF}, {}}};

    struct NativeUiPublication {
      domain::GameEpoch epoch{};
      bool ready = false;
    };

    std::mutex native_ui_mutex;
    NativeUiPublication native_ui_publication;
    std::atomic<bool> native_ui_active{false};
    std::uintptr_t native_ui_module = 0;
    ui_detail::Hook native_ui_select_hook{};
    ui_detail::Hook native_ui_dialog_hook{};
    ui_detail::Hook native_ui_close_hook{};
    ui_detail::Hook native_ui_input_hook{};
    ui_detail::Hook native_ui_input_event_hook{};
    ui_detail::Hook native_ui_input_click_hook{};
    ui_detail::Hook native_ui_destroy_hook{};
    ui_detail::Hook native_ui_state_close_hook{};
    ui_detail::Hook native_ui_button_hover_hook{};

    enum class NativeDialogStyle { List, Form, CompactForm, Text, MultilineText, Message, GemTree, MultiSelect, CityMap };

    struct NativeDialogFrame {
      std::vector<NativeListRow> visible;
      std::array<std::uintptr_t, 2> native_rows{};
      std::optional<std::size_t> selection;
      std::vector<int> selection_values;
      std::array<void *, 64> text_nodes{}, buttons{};
      std::array<bool, 64> disabled{};
      std::array<std::array<int, 4>, 64> button_bounds{};
      std::array<std::array<int, 3>, 64> button_labels{};
      std::array<std::array<int, 4>, 64> button_icons{};
      struct RowSprite {
        std::uint32_t row, slot;
        int resource;
      };
      std::vector<RowSprite> row_sprites;
      std::array<std::array<std::uintptr_t, 2>, 6> city_lists{};
      std::array<std::uintptr_t, 24> city_map_methods{};
      std::uintptr_t *city_map_table = nullptr;
      void *city_source = nullptr;
      int script_owner = 0;
      int input_layer = 6;
      bool confirm_closes = true;
      bool hide_form_decoration = false;
      bool close_requested = false;

      std::wstring title, caption, input_initial;
      int input_limit = 0;
      void *dialog = nullptr;

      NativeDialogStyle page = NativeDialogStyle::List;
      alignas(16) std::array<std::byte, 0x50> tree_state{};

      std::array<std::byte, 0x138> tree_context{};
      std::array<std::byte, 0x68> tree_owner{};
      std::array<std::byte, 0x18> tree_root{};
    };

    struct NativeUiSession {
      NativeUiPublication source;
      std::vector<std::unique_ptr<NativeDialogFrame>> frames;

      NativeDialogFrame &frame() {
        return *frames.back();
      }

      NativeInputBuffer input;
      bool text_input_open = false;
      void *text_input_owner = nullptr;
      void *text_input_layout = nullptr;

      std::wstring input_result;
    } native_ui_session;

    struct TreeView {
      void *layout = nullptr;
      void *parent_window = nullptr;
      bool shown = false, categories_bound = false, suspended = false, refresh = false;
      std::unordered_map<std::uint16_t, TreeNode> nodes;

      struct Footer {
        int action;
        bool enabled;
        std::wstring label;
        bool custom_label;
      };

      std::vector<Footer> footers;
    } native_tree_view;

    UiPublication game_publication{};
    bool ui_hooks_installed = false;
    void update_native_tree(void *layout, void *event);
    void native_tree_clicked(void *callback, void **argument);
    void native_tree_footer_clicked(void *callback, void **argument);
    void native_tree_back_clicked(void *callback, void **argument);
    void update_native_window(void *window, void *event);
    void update_native_tree_dialog(void *dialog, void *event);
    bool native_window_dirty(void *window);
    void native_window_exit(void *state);
    void native_tree_visual(void *gem, int animation);
    ui_detail::Hook tree_update_hook{}, tree_click_hook{}, tree_footer_hook{}, tree_back_hook{};
    ui_detail::Hook tree_dialog_hook{}, tree_visual_hook{}, window_update_hook{}, window_dirty_hook{},
        window_exit_hook{};

    template <class T> T &native_field(void *object, std::size_t offset) {
      return *reinterpret_cast<T *>(static_cast<std::byte *>(object) + offset);
    }

    void native_ui_button_hover(void *button) {
      if ((native_field<std::uint32_t>(button, 0x40) & 4) == 0 &&
          native_ui_active.load(std::memory_order_acquire)) {
        for (const auto &frame : native_ui_session.frames) {
          const auto found = std::find(frame->buttons.begin(), frame->buttons.end(), button);
          if (found != frame->buttons.end() && frame->disabled[found - frame->buttons.begin()])
            return;
        }
      }
      reinterpret_cast<void (*)(void *)>(native_ui_button_hover_hook.original)(button);
    }

    void show_native_dialog(NativeDialogStyle page, const scripting::DialogSpec &spec);
    bool council_room_active();
    bool council_exit_requested();
    void close_council_room();
    bool dispatch_mod_event(scripting::Event event, int owner, int action = 0);
    thread_local scripting::Event script_callback_event = scripting::Event::Closed;

    void restore_native_ui_dialog(void *dialog) {
      auto *vtable = native_field<std::uintptr_t *>(dialog, 0);
      void *controller = native_field<void *>(dialog, 0x238);
      // The retained parent's controller is already on the native modal stack.
      // Show without pushing it again: a duplicate push hides that same parent.
      const bool managed = native_field<bool>(dialog, 0x593);
      native_field<bool>(dialog, 0x593) = false;
      reinterpret_cast<void (*)(void *, bool)>(vtable[0x2C0 / 8])(dialog, true);
      native_field<bool>(dialog, 0x593) = managed;
      native_field<bool>(controller, 0x1CC) = managed;
      reinterpret_cast<void (*)(void *, bool)>(vtable[0x120 / 8])(dialog, true);
    }

    void native_ui_state_closed(void *state, void *event) {
      if (native_field<std::uintptr_t>(state, 0) == native_ui_module + 0x257E0B8) {
        void *dialog = native_field<void *>(state, 0x478);

        for (const auto &frame : native_ui_session.frames) {
          if (frame->dialog != dialog)
            continue;

          // Pushing a child also finishes the parent's hide animation and
          // emits event 100. That event must not pop the child state.

          if (frame.get() != &native_ui_session.frame() || native_field<int>(dialog, 0x230) == -1)
            return;

          break;
        }
      }

      reinterpret_cast<void (*)(void *, void *)>(native_ui_state_close_hook.original)(state, event);
    }

    void native_ui_state_destroyed(void *state, void *context) {
      void *dialog = native_field<void *>(state, 0x478);

      for (const auto &frame : native_ui_session.frames) {
        if (frame->dialog != dialog)
          continue;

        // CDataSelect's destructor deletes its list before the base layout
        // unregisters. Remove this popup while its children are still alive.
        auto *vtable = native_field<std::uintptr_t *>(dialog, 0);
        reinterpret_cast<void (*)(void *)>(vtable[0x50 / 8])(dialog);
        break;
      }

      reinterpret_cast<void (*)(void *, void *)>(native_ui_destroy_hook.original)(state, context);
    }

    void read_native_selection(NativeDialogFrame &frame) {
      frame.selection_values.clear();
      void *list = native_field<void *>(frame.dialog, 0x250);
      void *core = native_field<void *>(list, 0x140);
      const auto *handle = native_field<const std::uint32_t *>(core, 0x168);
      auto **heads = *reinterpret_cast<void ***>(native_ui_module + 0x3B94AC0);
      for (void *node = handle ? heads[*handle] : nullptr; node; node = native_field<void *>(node, 8)) {
        const int index = native_field<int>(node, 0);
        if (index >= 0 && static_cast<std::size_t>(index) < frame.visible.size())
          frame.selection_values.push_back(static_cast<int>(frame.visible[index].action));
      }
    }

    void native_ui_dialog_closed(void *dialog, int result) {
      auto &session = native_ui_session;

      for (const auto &frame : session.frames) {
        if (frame->dialog != dialog)
          continue;

        // Only the top popup may close. Native keyboard/back events also use
        // this entry point, independently of our button callbacks.

        if (frame.get() != &session.frame())
          return;

        if (frame->page == NativeDialogStyle::MultiSelect && result == 0x7FFFFFFD)
          read_native_selection(*frame);

        if (session.text_input_open) {
          // The native footer also maps Backspace to cancel. While a text
          // popup owns input, leave that key to CInputString's deletion path.

          if (session.text_input_layout && result == 0x7FFFFFFE && (GetKeyState(VK_BACK) & 0x8000) != 0)
            return;

          if (session.text_input_layout) {
            native_field<int>(session.text_input_layout, 0x208) = result == 0x7FFFFFFD;
            native_field<int>(session.text_input_layout, 0x204) = 1;
          }

          return;
        }

        break;
      }

      reinterpret_cast<void (*)(void *, int)>(native_ui_close_hook.original)(dialog, result);
    }

    bool native_ui_input_initialized(void *state, void *context) {
      const bool result = reinterpret_cast<bool (*)(void *, void *)>(native_ui_input_hook.original)(state, context);
      auto &session = native_ui_session;

      if (session.text_input_open && (session.frame().page == NativeDialogStyle::Text ||
                                      session.frame().page == NativeDialogStyle::MultilineText)) {
        session.text_input_layout = native_field<void *>(state, 0x478);
        // The input state is the textbox inside our popup. Restore the owning
        // dialog after the engine suspends it so its backdrop/footer remain.
        restore_native_ui_dialog(session.text_input_owner);
      }

      return result;
    }

    bool native_ui_input_event(void *layout, void *event) {
      if (layout == native_ui_session.text_input_layout) {
        // CInputString normally cancels on a click outside the textbox. Here
        // the popup footer owns acceptance/cancellation, so let it receive
        // that click without first popping the input state as cancelled.
        return reinterpret_cast<bool (*)(void *, void *)>(native_ui_module + 0x1EDFC50)(layout, event);
      }

      return reinterpret_cast<bool (*)(void *, void *)>(native_ui_input_event_hook.original)(layout, event);
    }

    void native_ui_input_clicked(void *layout) {
      auto &session = native_ui_session;

      if (layout == session.text_input_layout) {
        void *mouse = reinterpret_cast<void *(*)()>(native_ui_module + 0xD4D40)();

        const int x = native_field<int>(mouse, 0x48);
        const int y = native_field<int>(mouse, 0x4C);
        const int left = native_field<int>(layout, 0x20C);
        const int top = native_field<int>(layout, 0x210);

        if (x < left || x > left + native_field<int>(layout, 0x214) || y < top ||
            y > top + native_field<int>(layout, 0x218)) {
          // The textbox captures mouse release. Route releases over its own
          // footer using the controls' live logical positions, not screen pixels.
          void *footer = native_field<void *>(session.text_input_owner, 0x258);
          void *buttons = native_field<void *>(footer, 0x1A8);

          for (int index = 0; index < 6; ++index) {
            void *button = native_field<void *>(buttons, 0x1A0 + index * 8);

            if (!button || !reinterpret_cast<int (*)(void *)>(native_ui_module + 0x1EDE760)(button))
              continue;

            float bx = 0, by = 0;

            for (void *parent = button; parent; parent = native_field<void *>(parent, 8)) {
              bx += native_field<float>(parent, 0x20);
              by += native_field<float>(parent, 0x24);
            }

            if (x >= bx && x <= bx + native_field<int>(button, 0x28) && y >= by &&
                y <= by + native_field<int>(button, 0x2C)) {
              auto *vtable = native_field<std::uintptr_t *>(button, 0);
              reinterpret_cast<void (*)(void *)>(vtable[0x78 / 8])(button);
              break;
            }
          }

          return;
        }
      }

      reinterpret_cast<void (*)(void *)>(native_ui_input_click_hook.original)(layout);
    }

    void begin_native_ui_text_input(void *dialog) {
      auto &session = native_ui_session;
      auto &frame = session.frame();

      if (!frame.input_limit || session.text_input_open)
        return;

      const int owner = frame.script_owner;
      std::string initial(frame.input_initial.size() * 3, '\0');
      const int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, frame.input_initial.data(),
                                            static_cast<int>(frame.input_initial.size()), initial.data(),
                                            static_cast<int>(initial.size()), nullptr, nullptr);
      initial.resize(bytes);
      session.text_input_open = true;
      session.text_input_owner = dialog;
      session.input.input_text(native_ui_module, std::move(initial), frame.page == NativeDialogStyle::MultilineText,
                               frame.input_limit, [dialog, owner](NativeInputResult result, std::string text) {
                                 auto &current = native_ui_session;
                                 current.text_input_open = false;
                                 current.text_input_owner = nullptr;
                                 current.text_input_layout = nullptr;
                                 current.input_result.resize(text.size());
                                 const int units = text.empty() ? 0 : MultiByteToWideChar(
                                     CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
                                     current.input_result.data(), static_cast<int>(current.input_result.size()));
                                 current.input_result.resize(units);
                                 dispatch_mod_event(scripting::Event::TextInput, owner,
                                                    result == NativeInputResult::Accepted      ? 1
                                                    : result == NativeInputResult::Cancelled   ? 0
                                                    : result == NativeInputResult::InvalidText ? -1
                                                                                               : -2);
                                 native_ui_dialog_closed(dialog, result == NativeInputResult::Accepted ? 0x7FFFFFFD
                                                                                                       : 0x7FFFFFFE);
                               });
    }

    bool apply_native_button_sprite(void *button, std::uint32_t image_slot, int resource) {
      if (!button)
        return false;
      const auto type = native_field<std::uintptr_t>(button, 0) - native_ui_module;
      // The form CButton and the stock command button share the image node array.
      if ((type != 0x25FA310 && type != 0x2602578) || image_slot >= native_field<std::uint32_t>(button, 0x150))
        return false;
      auto **nodes = native_field<void **>(button, 0x148);
      void *node = nodes ? nodes[image_slot] : nullptr;
      if (!node)
        return false;

      const auto node_type = native_field<std::uintptr_t>(node, 0) - native_ui_module;
      if (node_type == 0x274EBA0) {
        using FindImage = void *(*)(void *, void *, int, const char *, int);
        auto *manager = *reinterpret_cast<void **>(native_ui_module + 0x2EA6838);
        void *image = reinterpret_cast<FindImage>(native_ui_module + 0x1EF4A70)(
            manager, native_field<void *>(button, 0x90), resource, nullptr, 1);
        if (!image)
          return false;
        auto *methods = native_field<std::uintptr_t *>(node, 0);
        using GetDrawable = void *(*)(void *, int, int);
        for (int slice = 1; slice <= 9; ++slice)
          if (void *drawable = reinterpret_cast<GetDrawable>(methods[0x88 / 8])(node, 0, slice)) {
            auto *drawable_methods = native_field<std::uintptr_t *>(drawable, 0);
            reinterpret_cast<void (*)(void *, void *)>(drawable_methods[0x20 / 8])(drawable, image);
          }
        return true;
      }
      if (node_type != 0x274E878)
        return false;

      using ApplyImage = void (*)(void *, int, void *, int, int, int, bool);
      // Ordinary form images use direct assignment to the drawable. Command buttons use the scoped resource path.
      const int scoped_resource = type == 0x2602578 ? 1 : 0;
      reinterpret_cast<ApplyImage>(native_ui_module + 0x1D122D0)(node, resource, button, scoped_resource, 1, 0, true);
      return (native_field<std::uint32_t>(node, 0x104) & 0x40) == 0;
    }


    struct NativeButtonParts {
      void *pane;
      void *button;
    };

    NativeButtonParts create_native_button(void *parent, int x, int y, int width, int height) {
      using CreatePane = void *(*)(void *, int, int, int, int);
      void *pane = reinterpret_cast<CreatePane>(native_ui_module + 0x1ED8200)(parent, x, y, width, height);
      void *button = reinterpret_cast<void *(*)(std::size_t)>(native_ui_module + 0xEE70)(0x1E0);
      reinterpret_cast<void (*)(void *)>(native_ui_module + 0x1694A10)(button);
      using InitializeButton = void (*)(void *, int, int, int, int, void *);
      reinterpret_cast<InitializeButton>(native_ui_module + 0x16945C0)(button, x, y, width, height, parent);
      return {pane, button};
    }

    void native_ui_dialog_initialized(void *dialog, void *config) {
      reinterpret_cast<void (*)(void *, void *)>(native_ui_dialog_hook.original)(dialog, config);

      auto &session = native_ui_session;

      if (native_field<const void *>(config, 0x18) != native_list_columns.data())
        return;

      session.frame().dialog = dialog;

      auto *dialog_vtable = native_field<std::uintptr_t *>(dialog, 0);
      reinterpret_cast<void (*)(void *, int)>(dialog_vtable[0x80 / 8])(dialog, session.frame().input_layer);

      if (session.frame().page == NativeDialogStyle::GemTree) {
        // The selection state supplies modal lifetime; the game's own tree supplies its UI.
        void *panes = native_field<void *>(dialog, 0x148);
        void *background = native_field<void *>(panes, sizeof(void *));
        reinterpret_cast<void (*)(void *, bool)>(native_ui_module + 0x1D15BC0)(background, false);

        for (std::size_t offset : {0x250u, 0x258u, 0x268u, 0x270u}) {
          void *child = native_field<void *>(dialog, offset);

          if (child) {
            auto *vtable = native_field<std::uintptr_t *>(child, 0);
            reinterpret_cast<void (*)(void *, bool)>(vtable[0x108 / 8])(child, false);
          }
        }

        auto &state = session.frame().tree_state;
        native_field<std::uintptr_t>(state.data(), 0) = native_ui_module + 0x26988C8;
        native_field<int>(state.data(), 0x2C) = 1; // Full unlock tree, including common/status categories.
        // Init resolves its UI parent through context->owner->root->layout.
        // Keep that indirection alive in the frame and point it at this modal,
        // not the suspended city's CityUILayoutRoot.
        auto &frame = session.frame();
        native_field<void *>(state.data(), 0x18) = frame.tree_context.data();
        native_field<void *>(frame.tree_context.data(), 0x130) = frame.tree_owner.data();
        native_field<void *>(frame.tree_owner.data(), 0x60) = frame.tree_root.data();
        native_field<void *>(frame.tree_root.data(), 0x10) = dialog;
        // Init creates the normal native objects, but its game-state Update (which
        // charges courage and changes the selected category) is never called.
        reinterpret_cast<void (*)(void *)>(native_ui_module + 0x183B180)(state.data());
        native_tree_view.layout = native_field<void *>(state.data(), 0x20);
        // CityStateGemUnlock::Init registers the tree but leaves it hidden;
        // normally the city state machine performs Show afterwards. Our modal
        // owns that step. The temporary COfsLayout belongs to the city scene,
        // not this modal: unregister and destroy it before it can receive events.
        void *offset_layout = native_field<void *>(state.data(), 0x30);
        native_field<void *>(state.data(), 0x30) = nullptr;
        alignas(16) std::array<std::byte, 0x38> offset_state{};
        native_field<void *>(offset_state.data(), 0x30) = offset_layout;
        reinterpret_cast<void (*)(void *)>(native_ui_module + 0x183B0E0)(offset_state.data());
        // Init already registered the tree and its children on the inherited
        // input layer. Changing it afterwards leaves callbacks on the old layer.
        native_tree_view.shown = false;
        native_tree_view.categories_bound = false;
        return;
      }

      const bool list_page =
          session.frame().page == NativeDialogStyle::List || session.frame().page == NativeDialogStyle::MultiSelect;

      if (session.frame().page == NativeDialogStyle::MultiSelect) {
        void *list = native_field<void *>(dialog, 0x250);
        void *core = native_field<void *>(list, 0x140);
        const auto *handle = native_field<const std::uint32_t *>(core, 0x168);
        for (int action : session.frame().selection_values) {
          const auto &rows = session.frame().visible;
          const auto found =
              std::find_if(rows.begin(), rows.end(), [action](const auto &row) { return row.action == action; });
          void *node = reinterpret_cast<void *(*)(void *, std::uint32_t, bool)>(native_ui_module + 0x11050)(
              reinterpret_cast<void *>(native_ui_module + 0x3B94AB0), *handle, true);
          *static_cast<int *>(node) = static_cast<int>(found - rows.begin());
        }
      }

      if (session.frame().page == NativeDialogStyle::MultiSelect) {
        reinterpret_cast<void (*)(void *, int, int)>(native_ui_module + 0x37F10)(
            dialog, static_cast<int>(session.frame().selection_values.size()),
            static_cast<int>(session.frame().visible.size()));
      }

      // Keep the game's backdrop and heading. Form controls replace the list,
      // tabs and scrollbar on the editor's own detail pages. Keep the native
      // octagonal confirm/back footer, including its layout and input controls.

      if (!list_page)
        for (std::size_t offset : {0x250u, 0x268u, 0x270u}) {
          void *part = native_field<void *>(dialog, offset);

          if (part) {
            auto *vtable = native_field<std::uintptr_t *>(part, 0);
            reinterpret_cast<void (*)(void *, bool)>(vtable[0x108 / 8])(part, false);
          }
        }

      std::function<void(void *)> footer_clicked = [dialog](void *button) {
        if (native_ui_session.frame().dialog != dialog)
          return;

        void *current_config = native_field<void *>(dialog, 0x290);

        const int confirm_id = native_field<int>(current_config, 0x98) != 0 ? 1 : 0;
        const bool confirmed = native_field<int>(button, 0x88) == confirm_id;
        auto &current = native_ui_session;

        if (confirmed && current.frame().page == NativeDialogStyle::List && !current.frame().selection)
          return;

        if (confirmed && !current.frame().confirm_closes) {
          if (current.frame().selection)
            dispatch_mod_event(scripting::Event::Select, current.frame().script_owner,
                               static_cast<int>(*current.frame().selection));
          return;
        }

        if (!confirmed)
          current.frame().selection.reset();

        auto *vtable = native_field<std::uintptr_t *>(dialog, 0);
        reinterpret_cast<void (*)(void *, int)>(vtable[0x2C8 / 8])(dialog, confirmed ? 0x7FFFFFFD : 0x7FFFFFFE);
      };

      reinterpret_cast<void (*)(void *, void *)>(native_ui_module + 0x1697A50)(native_field<void *>(dialog, 0x258),
                                                                               &footer_clicked);

      if (list_page) {
        reinterpret_cast<void (*)(void *, bool)>(native_ui_module + 0x38550)(dialog, true);
        return;
      }

      if (session.frame().hide_form_decoration) {
        // The backdrop list belongs to the object referenced by dialog+0x180.
        // Keep its input layer and hide only the background controls.
        void *backgrounds = native_field<void *>(dialog, 0x180);
        if (backgrounds) {
          void *handle = native_field<void *>(backgrounds, 8);
          if (handle) {
            const auto index = native_field<std::uint32_t>(handle, 0);
            auto **heads = *reinterpret_cast<void ***>(native_ui_module + 0x34C8630);
            for (void *node = heads[index]; node; node = native_field<void *>(node, 8)) {
              void *entry = native_field<void *>(node, 0);
              const int id = native_field<int>(entry, 0);
              if (id != 2 && id != 3)
                continue;
              void *background = native_field<void *>(entry, 8);
              auto *methods = native_field<std::uintptr_t *>(background, 0);
              reinterpret_cast<void (*)(void *, bool)>(methods[0x108 / 8])(background, false);
            }
          }
        }
        void *group = static_cast<std::byte *>(dialog) + 0x140;
        auto **panes = native_field<void **>(group, 8);
        const int count = native_field<int>(group, 0x10);
        for (int index = 1; index < count; ++index)
          if (panes[index])
            reinterpret_cast<void (*)(void *, bool)>(native_ui_module + 0x1D15BC0)(panes[index], false);
        void *footer = native_field<void *>(dialog, 0x258);
        auto *methods = native_field<std::uintptr_t *>(footer, 0);
        reinterpret_cast<void (*)(void *, bool)>(methods[0x108 / 8])(footer, false);
        reinterpret_cast<void (*)(void *, bool)>(methods[0x120 / 8])(footer, false);
      }

      const bool compact =
          session.frame().page == NativeDialogStyle::Text || session.frame().page == NativeDialogStyle::CompactForm;

      if (compact) {
        // Scale the background subtree, including the generated texture children.
        // Resizing its container alone leaves those textures at their original size.
        void *resource_group = static_cast<std::byte *>(dialog) + 0x140;

        auto **panes = native_field<void **>(resource_group, 8);
        const int count = native_field<int>(resource_group, 0x10);
        void *background = panes[1];

        const auto xy = reinterpret_cast<std::uint64_t (*)(void *)>(native_ui_module + 0x1D16250)(background);
        using SetPaneProperty = void (*)(void *, int, double);
        reinterpret_cast<SetPaneProperty>(native_ui_module + 0xAAB2E0)(background, 6, 0.6);
        reinterpret_cast<SetPaneProperty>(native_ui_module + 0xAAB2E0)(background, 7, 0.5);
        reinterpret_cast<void (*)(void *, float, float)>(native_ui_module + 0x1D12640)(
            background, static_cast<float>(static_cast<std::int32_t>(xy)) * 0.6f + 384.0f,
            static_cast<float>(static_cast<std::int32_t>(xy >> 32)) * 0.5f + 270.0f);

        for (int index = 5; index < count; ++index)
          reinterpret_cast<void (*)(void *, bool)>(native_ui_module + 0x1D15BC0)(panes[index], false);
        void *footer = native_field<void *>(dialog, 0x258);
        // This operation translates the existing footer; it does not set an
        // absolute position. Keep its native horizontal alignment.
        reinterpret_cast<void (*)(void *, int, int)>(native_ui_module + 0x1EE0760)(footer, 0, -300);
      }
      // A form does not select list rows; enable confirm independently of the
      // hidden list's selection count.
      reinterpret_cast<void (*)(void *, bool)>(native_ui_module + 0x38550)(dialog, true);
      // CLayout updates and destroys interactive children through its descriptor
      // group, not merely through the rendered scene/parent relationship.
      void *group = static_cast<std::byte *>(dialog) + 0x140;

      const int first_control = native_field<int>(group, 0x10);
      const int form_count =
          session.frame().page == NativeDialogStyle::Message ? 0 : static_cast<int>(session.frame().visible.size());

      if (form_count > 0) {
        void *manager = reinterpret_cast<void *(*)()>(native_ui_module + 0x14ED0)();
        void *allocator = native_field<void *>(manager, 0x28);

        auto *allocation_vtable = native_field<std::uintptr_t *>(allocator, 0);
        const std::array<std::uint64_t, 2> allocation_tag{0x37, 0};

        for (std::size_t offset : {std::size_t{0}, std::size_t{8}}) {
          const std::size_t stride = offset == 0 ? 0x60 : sizeof(void *);
          const std::size_t old_size = static_cast<std::size_t>(first_control) * stride;
          const std::size_t new_size = static_cast<std::size_t>(first_control + form_count) * stride;
          auto *previous = native_field<std::byte *>(group, offset);
          auto *extended = reinterpret_cast<std::byte *(*)(void *, std::size_t, const void *)>(
              allocation_vtable[0x28 / 8])(allocator, new_size, allocation_tag.data());

          std::copy_n(previous, old_size, extended);
          std::fill(extended + old_size, extended + new_size, std::byte{});
          native_field<std::byte *>(group, offset) = extended;
          reinterpret_cast<void (*)(void *, void *)>(allocation_vtable[0x58 / 8])(allocator, previous);
        }

        native_field<int>(group, 0x10) = first_control + form_count;
      }
      // Resource-based dialogs bypass the descriptor factory. Create native children
      // after the resource layout exists, using the same constructors as type 14/1.
      int y = session.frame().page == NativeDialogStyle::Form ? 240 : 430;

      const int button_x = compact ? 600 : 280;
      const int text_x = compact ? 850 : 530;

      for (std::size_t index = 0; index < session.frame().visible.size(); ++index) {
        const auto *row = &session.frame().visible[index];
        const bool confirm = session.frame().page == NativeDialogStyle::Message;
        const std::size_t action = row->action;

        const auto [label_x, label_y, font_size] = confirm ? std::array<int, 3>{} : session.frame().button_labels[action];

        if (!confirm) {
          // Resource children need a pane in the parent's scene, just like the
          // descriptor factory's type-14 controls. Parenting the C++ object alone
          // registers input/lifetime but does not attach its rendered resource.
          const auto &bounds = session.frame().button_bounds[action];
          const auto [x, top, width, height] = bounds[2] ? bounds : std::array{button_x, y, 220, 48};
          const auto [pane, button] = create_native_button(dialog, x, top, width, height);

          auto *button_vtable = native_field<std::uintptr_t *>(button, 0);
          reinterpret_cast<void (*)(void *, const wchar_t *)>(button_vtable[0x298 / 8])(button, font_size ? L"" : row->label.c_str());
          const int control_id = first_control + static_cast<int>(index);
          native_field<int>(button, 0x88) = control_id;
          native_field<int>(button, 0x8C) = 1;

          auto *descriptor = native_field<std::byte *>(group, 0) + control_id * 0x60;
          native_field<int>(descriptor, 0) = 14;
          native_field<int>(descriptor, 4) = x;
          native_field<int>(descriptor, 8) = top;
          native_field<int>(descriptor, 0xC) = width;
          native_field<int>(descriptor, 0x10) = height;
          native_field<std::uint64_t>(descriptor, 0x18) = 0xFFFF;
          native_field<const wchar_t *>(descriptor, 0x20) = row->label.c_str();
          native_field<void **>(group, 8)[control_id] = pane;
          reinterpret_cast<void (*)(void *, int, void *)>(native_ui_module + 0x1D16E10)(group, control_id, button);
          session.frame().buttons[action] = button;
          if (session.frame().disabled[action]) {
            reinterpret_cast<void (*)(void *, bool)>(button_vtable[0x110 / 8])(button, false);
            reinterpret_cast<void (*)(void *, bool)>(button_vtable[0x120 / 8])(button, false);
          }
          for (const auto &sprite : session.frame().row_sprites)
            if (sprite.row == action)
              apply_native_button_sprite(button, sprite.slot, sprite.resource);

          std::function<void(void *)> clicked = [dialog, action](void *) {
            auto &current = native_ui_session;

            if (current.frame().dialog != dialog || current.text_input_open || current.frame().disabled[action])
              return;

            dispatch_mod_event(scripting::Event::Click, current.frame().script_owner, static_cast<int>(action));
          };
          // CButton emits 0x7FFFFFFE on activation; 0x44C is the tab-change event.
          reinterpret_cast<void (*)(void *, int, void *)>(native_ui_module + 0xEF40)(button, 0x7FFFFFFE, &clicked);
        }

        auto reserve_node = [&] {
          // CreateText does not grow the resource's node array. The native
          // descriptor path first reserves a node via CreatePane (1ED8200).
          // Mirror that reservation before adding text to this loaded layout.
          void *resource = native_field<void *>(dialog, 0x98);
          void *scene = native_field<void *>(resource, 0x10);
          void *nodes = static_cast<std::byte *>(scene) + 0x18;

          auto *nodes_vtable = native_field<std::uintptr_t *>(nodes, 0);
          const auto count = reinterpret_cast<std::size_t (*)(void *)>(nodes_vtable[0x18 / 8])(nodes);
          reinterpret_cast<void (*)(void *, std::size_t)>(nodes_vtable[0x60 / 8])(nodes, count + 1);
        };

        if (!confirm) {
          const auto [resource, x, top, size] = session.frame().button_icons[action];
          if (size) {
            reserve_node();
            using CreateImage = void *(*)(void *, int, int, int, int, int);
            void *icon = reinterpret_cast<CreateImage>(native_ui_module + 0x1ED5570)(
                dialog, x, top, size, size, resource);
            if (icon && (native_field<std::uint32_t>(icon, 0x104) & 0x40)) {
              using ApplyImage = void (*)(void *, int, void *, int, int, int, bool);
              reinterpret_cast<ApplyImage>(native_ui_module + 0x1D122D0)(icon, resource, dialog, 1, 1, 0, true);
            }
          }
        }

        auto create_text = [&](int x, int top, int width, int height, const wchar_t *value, int size, int palette) {
          reserve_node();
          using CreateText = void *(*)(void *, int, int, int, int, const wchar_t *);
          void *text = reinterpret_cast<CreateText>(native_ui_module + 0x1EDAB20)(
              dialog, x, top, width, height, value);
          // Text flags encode size in bits 25..31 and top-left alignment in bit 0.
          reinterpret_cast<void (*)(void *, std::uint64_t)>(native_ui_module +
                                                            0x1D13550)(text, (static_cast<std::uint64_t>(size) << 25) | 1);
          reinterpret_cast<void (*)(void *, int)>(native_ui_module + 0x1D167B0)(text, palette);
          return text;
        };

        if (font_size)
          create_text(label_x, label_y, session.frame().button_bounds[action][2], font_size + 8,
                      row->label.c_str(), font_size, 0);
        if (!session.frame().hide_form_decoration && action < session.frame().text_nodes.size())
          session.frame().text_nodes[action] = create_text(
              text_x, y + 12, compact ? 450 : 1110, row->height > 64 ? row->height : 60, row->text.c_str(), 24, 1);

        y += row->height;
      }

      if (session.frame().page == NativeDialogStyle::Text || session.frame().page == NativeDialogStyle::MultilineText)
        begin_native_ui_text_input(dialog);
    }

    void native_ui_selected(void *dialog, void *control) {
      auto &session = native_ui_session;
      void *config = native_field<void *>(dialog, 0x290);

      if (native_ui_active.load(std::memory_order_acquire) && config &&
          native_field<const void *>(config, 0x18) == native_list_columns.data()) {
        if (session.frame().dialog != dialog || session.text_input_open)
          return;

        if (session.frame().page == NativeDialogStyle::MultiSelect) {
          reinterpret_cast<void (*)(void *, void *)>(native_ui_select_hook.original)(dialog, control);
          reinterpret_cast<void (*)(void *, bool)>(native_ui_module + 0x38550)(dialog, true);
          read_native_selection(session.frame());
          dispatch_mod_event(Event::Select, session.frame().script_owner);
          return;
        }

        void *list = native_field<void *>(dialog, 0x250);
        void *core = native_field<void *>(list, 0x140);

        auto *handle = native_field<const std::uint32_t *>(core, 0x168);

        if (handle) {
          auto **heads = *reinterpret_cast<void ***>(native_ui_module + 0x3B94AC0);
          void *node = heads[*handle];

          if (node) {
            const int index = native_field<int>(node, 0);
            auto **data = native_field<NativeListRow **>(core, 0x1D0);
            session.frame().selection = data[index]->action;
          }
        }

        if (session.frame().selection) {
          const std::size_t action = *session.frame().selection;
          dispatch_mod_event(scripting::Event::Select, session.frame().script_owner, static_cast<int>(action));
        }

        return;
      }

      reinterpret_cast<void (*)(void *, void *)>(native_ui_select_hook.original)(dialog, control);
    }

    void finish_native_dialog(void *state) {
      auto &session = native_ui_session;
      auto &frame = session.frame();
      const bool accepted = native_field<int>(state, 8) == 0x7FFFFFFD;

      if (frame.page == NativeDialogStyle::GemTree) {
        // Native tree construction pushes its caption; Exit only destroys layouts.
        reinterpret_cast<void (*)(void *, int)>(native_ui_module +
                                                0x16913E0)(*reinterpret_cast<void **>(native_ui_module + 0x350A4C0), 0);
        reinterpret_cast<void (*)(void *)>(native_ui_module + 0x183B0E0)(frame.tree_state.data());
        native_tree_view.layout = nullptr;
        void *parent = native_tree_view.parent_window;
        native_tree_view.parent_window = nullptr;
        if (parent) {
          // The selection modal hid the enclosing EditInGameUIRootLayout, not
          // just its protagonist window. Restore its retained animation without
          // pushing another entry onto the game's modal stack.
          void *root = native_field<void *>(parent, 8);
          void *animation = native_field<void *>(root, 0x1C8);

          const bool managed = native_field<bool>(animation, 0x1CC);
          native_field<bool>(animation, 0x1CC) = false;

          const auto caption_flags = native_field<std::uint32_t>(animation, 0x1C0);
          native_field<std::uint32_t>(animation, 0x1C0) = caption_flags & ~0x5u;
          reinterpret_cast<void (*)(void *, int, int)>(native_ui_module +
                                                       0x1697F90)(animation, native_field<int>(animation, 0x1C4), 0);
          native_field<std::uint32_t>(animation, 0x1C0) = caption_flags;
          native_field<bool>(animation, 0x1CC) = managed;

          auto *root_vtable = native_field<std::uintptr_t *>(root, 0);
          reinterpret_cast<void (*)(void *, bool)>(root_vtable[0x108 / 8])(root, true);
          reinterpret_cast<void (*)(void *, bool)>(root_vtable[0x120 / 8])(root, true);

          auto *vtable = native_field<std::uintptr_t *>(parent, 0);
          reinterpret_cast<void (*)(void *, bool)>(vtable[0x120 / 8])(parent, true);
        }
      }

      dispatch_mod_event(scripting::Event::Closing, frame.script_owner,
                         accepted ? static_cast<int>(frame.selection.value_or(0)) : -1);
      if (frame.page == NativeDialogStyle::CityMap) {
        for (auto &list : frame.city_lists) {
          auto *vtable = reinterpret_cast<std::uintptr_t *>(list[0]);
          reinterpret_cast<void (*)(void *, unsigned)>(vtable[0])(list.data(), 0);
        }
      } else {
        reinterpret_cast<void (*)(void *)>(native_ui_module + 0x72DD0)(frame.native_rows.data());
      }

      session.frames.pop_back();

      if (session.frames.empty()) {
        // Owner zero resumes deferred event choices after the closed frame is gone.
        if (!council_exit_requested())
          dispatch_mod_event(scripting::Event::Resume, 0);
        if (!session.frames.empty())
          return;
        if (council_room_active())
          return;
        dispatch_mod_event(scripting::Event::Closed, 0);
        native_ui_active.store(false, std::memory_order_release);
        return;
      }

      auto &parent = session.frame();
      restore_native_ui_dialog(parent.dialog);
      if (parent.page == NativeDialogStyle::GemTree) {
        native_tree_view.suspended = false;
        native_tree_view.shown = false;
      }

      if (parent.script_owner)
        dispatch_mod_event(scripting::Event::Resume, parent.script_owner);

      if (parent.input_limit)
        begin_native_ui_text_input(parent.dialog);

      dispatch_mod_event(scripting::Event::Resume, 0);
    }

    void show_native_dialog(NativeDialogStyle page, const scripting::DialogSpec &spec) {
      auto &session = native_ui_session;

      const int input_layer = session.frames.empty() ? 6
                                                     : session.frame().input_layer +
                                                           (session.frame().page == NativeDialogStyle::GemTree ? 2 : 1);

      if (!session.frames.empty()) {
        void *parent = session.frame().dialog;

        auto *vtable = native_field<std::uintptr_t *>(parent, 0);
        reinterpret_cast<void (*)(void *, bool)>(vtable[0x120 / 8])(parent, false);
        if (session.frame().page == NativeDialogStyle::GemTree) {
          native_tree_view.suspended = true;
          auto *tree_vtable = native_field<std::uintptr_t *>(native_tree_view.layout, 0);
          reinterpret_cast<void (*)(void *, bool)>(tree_vtable[0x120 / 8])(native_tree_view.layout, false);
          reinterpret_cast<void (*)(void *, bool)>(tree_vtable[0x108 / 8])(native_tree_view.layout, false);
        }
      }

      session.frames.push_back(std::make_unique<NativeDialogFrame>());
      session.frame().page = page;
      session.frame().input_layer = input_layer;

      auto &frame = session.frame();
      frame.script_owner = spec.owner;
      frame.confirm_closes = spec.confirm_closes != 0;
      frame.title = spec.title;
      frame.caption = spec.caption;
      if (spec.selection_count)
        frame.selection_values.assign(spec.selection, spec.selection + spec.selection_count);

      for (int i = 0; i < spec.count; ++i)
        frame.visible.push_back({spec.rows[i].label, spec.rows[i].text, static_cast<std::size_t>(spec.rows[i].action),
                                 spec.rows[i].height});

      if (page == NativeDialogStyle::CityMap) {
        // The original selector owns its map, native city list/detail windows,
        // animation and cancellation. Mode 3 selects a city; it does not dispatch a march.
        struct CityNode { void *city; CityNode *next; };
        auto *root = native_field<void *>(reinterpret_cast<void *>(native_ui_module + 0x2E98BC8), 0);
        std::vector<CityNode> nodes(spec.count);
        for (int index = 0; index < spec.count; ++index)
          nodes[index] = {native_field<void *>(root, 0x546808 + spec.rows[index].action * sizeof(void *)),
                          index + 1 < spec.count ? &nodes[index + 1] : nullptr};

        alignas(16) std::array<std::byte, 0x80> config{};
        for (std::size_t index = 0; index < frame.city_lists.size(); ++index) {
          auto &list = frame.city_lists[index];
          list[0] = native_ui_module + 0x258BFE8;
          reinterpret_cast<void (*)(void *)>(native_ui_module + 0x10D10)(list.data());
          native_field<void *>(config.data(), index * sizeof(void *)) = list.data();
        }
        CityNode *first = nodes.data(), *end = nullptr;
        reinterpret_cast<void (*)(void *, CityNode **, CityNode **)>(native_ui_module + 0x30340)(
            frame.city_lists[3].data(), &first, &end);
        void *source = native_field<void *>(root, 0x546808 + spec.reserved * sizeof(void *));
        frame.city_source = source;
        native_field<std::uint8_t>(config.data(), 0x30) = 3;
        native_field<void *>(config.data(), 0x38) = source;
        native_field<void *>(config.data(), 0x48) =
            reinterpret_cast<void *(*)(void *)>(native_ui_module + 0x17121D0)(source);
        native_field<int>(config.data(), 0x78) = 0x7FFFFFFF;

        std::function<void(void *)> completed = [](void *state) {
          auto &selected = native_ui_session.frame();
          native_field<std::uintptr_t *>(state, 0) = selected.city_map_table;
          const auto *handle = reinterpret_cast<const std::uint32_t *>(selected.city_lists[0][1]);
          auto **heads = *reinterpret_cast<void ***>(native_ui_module + 0x34C8630);
          void *node = handle ? heads[*handle] : nullptr;
          if (node)
            selected.selection = native_field<std::uint8_t>(native_field<void *>(node, 0), 8);
          finish_native_dialog(state);
        };
        void *manager = reinterpret_cast<void *(*)()>(native_ui_module + 0xF0B0)();
        using Enqueue = void (*)(void *, const char *, void *, std::function<void(void *)> *);
        reinterpret_cast<Enqueue>(native_ui_module + 0xF3120)(
            manager, "ConferenceDepartCityDlgState", config.data(), &completed);
        const auto count = native_field<std::size_t>(manager, 0x30);
        auto *queue = native_field<std::byte *>(manager, 0x40);
        void *state = native_field<void *>(queue, (count - 1) * 0x10 + 8);
        frame.city_map_table = native_field<std::uintptr_t *>(state, 0);
        std::copy_n(frame.city_map_table - 1, frame.city_map_methods.size(), frame.city_map_methods.data());
        auto *methods = frame.city_map_methods.data() + 1;
        methods[0x10 / 8] = reinterpret_cast<std::uintptr_t>(+[](void *state) {
          auto &view = native_ui_session.frame();
          reinterpret_cast<void (*)(void *)>(view.city_map_table[0x10 / 8])(state);
          void *map = native_field<void *>(state, 0x480);
          void *cities = native_field<void *>(map, 0x1D8);
          native_field<void *>(cities, 0x210) = view.city_source;
          void *caption = native_field<void *>(map, 0x538);
          reinterpret_cast<void (*)(void *, const wchar_t *)>(native_ui_module + 0x1EF6360)(
              native_field<void *>(caption, 0x1A0), view.title.c_str());
        });
        methods[0x30 / 8] = reinterpret_cast<std::uintptr_t>(+[](void *state) {
          auto &view = native_ui_session.frame();
          void *manager = reinterpret_cast<void *(*)()>(native_ui_module + 0xF0B0)();
          const auto count = native_field<std::size_t>(manager, 0x10);
          auto **states = native_field<void **>(manager, 0x20);
          void *map = native_field<void *>(state, 0x480);
          if ((native_field<std::uint8_t>(state, 0x478) == 6 || council_exit_requested()) &&
              count && states[count - 1] == state &&
              (!map || !reinterpret_cast<bool (*)(void *)>(native_ui_module + 0x168EB70)(map))) {
            native_field<int>(state, 8) = 0x7FFFFFFE;
            reinterpret_cast<void (*)(void *)>(native_ui_module + 0xF150)(manager);
            return;
          }
          reinterpret_cast<void (*)(void *)>(view.city_map_table[0x30 / 8])(state);
        });
        native_field<std::uintptr_t *>(state, 0) = methods;
        return;
      }

      // The count at config+0x10 limits selection; it does not enumerate rows.
      // Supply the engine's linked list at config+0. Its nodes own only row pointers.
      struct RowNode {
        NativeListRow *row;
        RowNode *next;
      };

      std::vector<RowNode> nodes(session.frame().visible.size());

      for (std::size_t index = 0; index < nodes.size(); ++index) {
        nodes[index] = {&session.frame().visible[index], index + 1 < nodes.size() ? &nodes[index + 1] : nullptr};
      }

      session.frame().native_rows = {native_ui_module + 0x25869F8, 0};
      reinterpret_cast<void (*)(void *)>(native_ui_module + 0x10D10)(session.frame().native_rows.data());
      RowNode *first = nodes.empty() ? nullptr : nodes.data();
      RowNode *end = nullptr;
      reinterpret_cast<void (*)(void *, RowNode **, RowNode **)>(native_ui_module + 0x30340)(
          session.frame().native_rows.data(), &first, &end);

      // Use the engine constructor/destructor for every embedded callback and
      // string in this configuration. State creation deep-copies it before return.
      alignas(16) std::array<std::byte, 0x2580> config{};
      reinterpret_cast<void (*)(void *)>(native_ui_module + 0x1A870)(config.data());
      native_field<void *>(config.data(), 0) = session.frame().native_rows.data();
      native_field<const void *>(config.data(), 8) = native_list_tabs.data();
      native_field<int>(config.data(), 0x10) = static_cast<int>(session.frame().visible.size());
      native_field<const void *>(config.data(), 0x18) = native_list_columns.data();
      native_field<int>(config.data(), 0x20) = page == NativeDialogStyle::MultiSelect ? 2 : 1;
      native_field<bool>(config.data(), 0x9C) = true;
      native_field<const wchar_t *>(config.data(), 0x130) = session.frame().title.c_str();
      native_field<const wchar_t *>(config.data(), 0x138) = session.frame().caption.c_str();

      std::function<void *(int)> get_row = [frame = &session.frame()](int index) -> void * {
        auto &rows = frame->visible;
        // The engine supplies this index, including terminal lookup probes.

        if (index < 0 || static_cast<std::size_t>(index) >= rows.size())
          return nullptr;

        return &rows[static_cast<std::size_t>(index)];
      };

      reinterpret_cast<void (*)(void *, const void *)>(native_ui_module + 0x10830)(config.data() + 0x48, &get_row);

      std::function<void(void *)> completed = finish_native_dialog;
      void *manager = reinterpret_cast<void *(*)()>(native_ui_module + 0xF0B0)();
      using Enqueue = void (*)(void *, const char *, void *, std::function<void(void *)> *);
      reinterpret_cast<Enqueue>(native_ui_module + 0x5D530)(manager, "CDataSelectState<PersonalityData>", config.data(),
                                                            &completed);
      reinterpret_cast<void (*)(void *)>(native_ui_module + 0x179F0)(config.data());
    }

    void suspend_native_window();
    void execute_pending_march(bool privileged_only = false);
    std::optional<std::int64_t> invoke_ui_service(scripting::Operation, std::uint64_t, std::int64_t, void *,
                                                  std::uint32_t);

std::optional<std::int64_t> invoke_battle_service(scripting::Operation, std::uint64_t, std::int64_t, void *,
                                                     std::uint32_t);

#include "council_room_ui.inl"
#include "native_ui_api.inl"

    bool dispatch_mod_event(scripting::Event event, int owner, int action) {
      static const scripting::EngineApi api{scripting::api_version, sizeof(scripting::EngineApi), game_connection(),
                                            script_engine_invoke};

      const auto previous = script_callback_event;
      script_callback_event = event;

      const auto status = static_cast<scripting::Status>(S8ScriptDispatch(&api, event, owner, action));

      const bool success = status == scripting::Status::Ready;
      script_callback_event = previous;

      if (!success) {
        OutputDebugStringW(scripting::status_message(status));

        if (event == scripting::Event::Open)
          MessageBoxW(nullptr, scripting::status_message(status), L"모드 실행", MB_OK | MB_ICONERROR);
      }

      return success;
    }

#include "native_tree.inl"
#include "native_menus.inl"
#include "battle_services.inl"
#include "personal_activity_services.inl"
#include "save_services.inl"
#include "native_ui_services.inl"

  } // namespace

  bool install_native_ui_hooks(std::uintptr_t module) {
    native_ui_module = module;

    struct Binding {
      std::uintptr_t rva;
      void *callback;
      ui_detail::Hook *hook;
    };

    const Binding bindings[]{
        {0x16D0270, reinterpret_cast<void *>(&stage_archive_save), &archive_stage_save_hook},
        {0x16D04C0, reinterpret_cast<void *>(&complete_archive_save), &archive_complete_save_hook},
        {0x16CCC70, reinterpret_cast<void *>(&load_archive_save), &archive_load_save_hook},
        {0x16E6860, reinterpret_cast<void *>(&load_scenario_save), &archive_scenario_save_hook},
        {0x1BDC950, reinterpret_cast<void *>(&month_advance), &month_advance_hook},
        {0x197A1F0, reinterpret_cast<void *>(&council_entry_step), &council_entry_hook},
        {0x1840AB0, reinterpret_cast<void *>(&domestic_activity_step), &domestic_activity_hook},
        {0x185E910, reinterpret_cast<void *>(&communicate_talk_step), &communicate_talk_hook},
        {0x1D86520, reinterpret_cast<void *>(&battle_turn_step), &battle_turn_hook},
        {0x1D827F0, reinterpret_cast<void *>(&battle_unit_action_end), &battle_unit_end_hook},
        {0x1D85B60, reinterpret_cast<void *>(&battle_select_unit), &battle_select_hook},
        {0x751130, reinterpret_cast<void *>(&battle_state_machine_destroy), &battle_destroy_hook},
        {0x3C190, reinterpret_cast<void *>(&native_ui_selected), &native_ui_select_hook},
        {0x1631F60, reinterpret_cast<void *>(&native_ui_button_hover), &native_ui_button_hover_hook},
        {0x392D0, reinterpret_cast<void *>(&native_ui_dialog_initialized), &native_ui_dialog_hook},
        {0x294B0, reinterpret_cast<void *>(&native_ui_dialog_closed), &native_ui_close_hook},
        {0x1EC60B0, reinterpret_cast<void *>(&native_ui_input_initialized), &native_ui_input_hook},
        {0x1EC65E0, reinterpret_cast<void *>(&native_ui_input_event), &native_ui_input_event_hook},
        {0x1ECA700, reinterpret_cast<void *>(&native_ui_input_clicked), &native_ui_input_click_hook},
        {0x5C2B0, reinterpret_cast<void *>(&native_ui_state_destroyed), &native_ui_destroy_hook},
        {0x11B10, reinterpret_cast<void *>(&native_ui_state_closed), &native_ui_state_close_hook},
        {0x15EB4A0, reinterpret_cast<void *>(&update_native_tree), &tree_update_hook},
        {0x16F860, reinterpret_cast<void *>(&native_tree_clicked), &tree_click_hook},
        {0x16F660, reinterpret_cast<void *>(&native_tree_footer_clicked), &tree_footer_hook},
        {0x16F6D0, reinterpret_cast<void *>(&native_tree_back_clicked), &tree_back_hook},
        {0x12D4870, reinterpret_cast<void *>(&update_native_window), &window_update_hook},
        {0x28B10, reinterpret_cast<void *>(&update_native_tree_dialog), &tree_dialog_hook},
        {0x12D47B0, reinterpret_cast<void *>(&native_window_dirty), &window_dirty_hook},
        {0x1349BB0, reinterpret_cast<void *>(&native_tree_visual), &tree_visual_hook},
        {0x12E56F0, reinterpret_cast<void *>(&native_window_exit), &window_exit_hook}};

    for (const auto &binding : bindings) {
      const auto hook = ui_detail::create_hook(reinterpret_cast<void *>(module + binding.rva), binding.callback);
      if (!hook)
        return false;
      *binding.hook = *hook;
    }

    std::size_t enabled = 0;
    for (const auto &binding : bindings) {
      if (ui_detail::enable_hook(*binding.hook) != 0)
        break;
      ++enabled;
    }
    if (enabled == std::size(bindings) && install_native_menu_hooks())
      return true;

    for (std::size_t i = enabled; i > 0; --i)
      ui_detail::disable_hook(*bindings[i - 1].hook);
    return false;
  }

  // The mod manager owns these entry points; no other DLL publishes game state.
  bool attach_native_ui(const UiHookApi &hooks) {
    if (hooks.version != ui_host_version || hooks.size != sizeof(UiHookApi) || !hooks.create || !hooks.enable ||
        !hooks.disable || game_connection().status != Status::Ready)
      return false;

    ui_detail::hooks = hooks;
    ui_hooks_installed = install_native_ui_hooks(reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr)));
    return ui_hooks_installed;
  }

  // Polled by the manager thread. A new epoch starts when the game root, the
  // player object or the loaded save generation changes.
  void refresh_game_publication() {
    if (!native_ui_module)
      return;

    const auto read = [](std::uintptr_t address, void *output, std::size_t bytes) {
      SIZE_T copied = 0;
      return address && ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void *>(address), output, bytes,
                                          &copied) &&
             copied == bytes;
    };

    std::uintptr_t root = 0, player = 0;
    std::uint16_t protagonist = 0;
    std::uint8_t bitmap[34]{};
    read(native_ui_module + 0x2E98BC8, &root, sizeof(root));
    if (root)
      read(root + 0xE0, &player, sizeof(player));
    if (player > 0x10000)
      read(player + 8, &protagonist, sizeof(protagonist));
    const bool has_bitmap = root && read(root + 0x71E0, bitmap, sizeof(bitmap));
    const auto mask = jewel_force_mask();

    static std::uintptr_t last_root = 0, last_player = 0;
    static std::uint32_t last_generation = 0;
    static std::uint64_t epoch = 0;

    std::lock_guard lock(native_ui_mutex);
    if (epoch == 0 || root != last_root || player != last_player || world_save_generation != last_generation) {
      ++epoch;
      last_root = root;
      last_player = player;
      last_generation = world_save_generation;
      march_requests.clear();
    }

    UiPublication publication{};
    publication.epoch = epoch;
    std::copy(mask.begin(), mask.end(), publication.mask);
    std::copy(std::begin(bitmap), std::end(bitmap), publication.bitmap);
    publication.ready = root != 0;
    publication.has_bitmap = has_bitmap && protagonist != 0;
    publication.protagonist_id = protagonist ? protagonist : 0xFFFF;
    publication.menus_installed = ui_hooks_installed && application_ready;
    game_publication = publication;
    native_ui_publication = {{epoch}, publication.ready};
  }

} // namespace s8::scripting
