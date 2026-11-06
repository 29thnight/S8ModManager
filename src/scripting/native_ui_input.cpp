#include "native_ui_input.h"

#include <cstddef>
#include <utility>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

namespace s8::scripting {
  void NativeInputBuffer::input_text(std::uintptr_t module, std::string initial, bool multiline, int limit,
                                     std::function<void(NativeInputResult, std::string)> completion) {
    // CInputStringState configuration copied at RVA 0x32860. Its native byte
    // strings use the game's language code page, not UTF-8 (RVA 0x17F3250).
    struct InputConfig {
      std::int32_t x, y, width, height;
      const char *initial;
      std::int32_t limit, rows;
      char *output;

      std::int32_t background_width, font_size, flags, reserved;
      std::array<char, 16> string_inline;
      std::uint64_t string_length, string_capacity;
      std::int32_t option1, option2, foreground_layer, padding;
    };

    static_assert(sizeof(InputConfig) == 0x68);
    static_assert(offsetof(InputConfig, output) == 0x20);
    static_assert(offsetof(InputConfig, string_inline) == 0x38);
    static_assert(sizeof(std::function<void(void *)>) == 0x40);

    const std::string &source = initial;
    std::wstring wide(source.size(), L'\0');
    const int units = source.empty() ? 0
                                     : MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, source.data(),
                                                           static_cast<int>(source.size()), wide.data(),
                                                           static_cast<int>(wide.size()));

    if (!source.empty() && units == 0) {
      completion(NativeInputResult::InvalidText, std::string{});
      return;
    }

    const auto code_page = reinterpret_cast<unsigned (*)()>(module + 0x17F3250)();
    input_.fill('\0');

    BOOL substituted = FALSE;
    const int bytes = units == 0
                          ? 0
                          : WideCharToMultiByte(code_page, WC_NO_BEST_FIT_CHARS, wide.data(), units, input_.data(),
                                                static_cast<int>(input_.size() - 1), nullptr, &substituted);

    if (substituted || (units != 0 && bytes == 0)) {
      completion(NativeInputResult::UnrepresentableText, std::string{});
      return;
    }

    initial_input_.assign(input_.data(), static_cast<std::size_t>(bytes));

    InputConfig config{};
    config.x = multiline ? 480 : 600;
    config.y = multiline ? 240 : 430;
    config.width = config.background_width = multiline ? 960 : 720;
    config.height = multiline ? 600 : 96;
    config.initial = initial_input_.c_str();
    config.limit = limit;
    config.rows = multiline ? 16 : 1;
    config.output = input_.data();
    config.font_size = 24;
    config.string_capacity = 15;
    config.option1 = config.option2 = 1;
    // CInputStringState::Init (0x1EC6296) sets layout layer 12 when this
    // flag is enabled. The retained detail dialog is on layer 6.
    config.foreground_layer = 1;

    const auto manager = reinterpret_cast<void *(*)()>(module + 0xF0B0)();
    // Native text editing (0x135F71F) forces text/mouse input mode while
    // CInputStringState is active, instead of automatic keyboard navigation.
    auto *ui_manager = static_cast<std::byte *>(reinterpret_cast<void *(*)()>(module + 0xF410)());
    auto *input_mode = reinterpret_cast<int *>(ui_manager + 0x608);
    const int previous_input_mode = *input_mode;
    *input_mode = 0;
    // UI activation runs outside the state coroutine (manager+0x48 is null).
    // Queue input and return to the UI dispatcher; do not suspend/resume a
    // coroutine here. The completion updates the retained detail layout.
    std::function<void(void *)> closed = [this, code_page, input_mode, previous_input_mode,
                                          completion = std::move(completion)](void *state) {
      *input_mode = previous_input_mode;
      // Native teardown copies output even on cancel. Never consume it
      // unless the state reports the confirmed result.
      const int result = *reinterpret_cast<const int *>(static_cast<const char *>(state) + 8);

      if (result != 0x7FFFFFFD) {
        completion(NativeInputResult::Cancelled, std::string{});
        return;
      }

      std::array<wchar_t, 4096> converted{};
      const int count = MultiByteToWideChar(code_page, MB_ERR_INVALID_CHARS, input_.data(), -1, converted.data(),
                                            static_cast<int>(converted.size()));

      if (count == 0) {
        completion(NativeInputResult::InvalidText, std::string{});
        return;
      }

      std::string text(static_cast<std::size_t>(count - 1) * 3, '\0');
      const int length = count == 1 ? 0
                                    : WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, converted.data(), count - 1,
                                                          text.data(), static_cast<int>(text.size()), nullptr, nullptr);

      if (count != 1 && length == 0) {
        completion(NativeInputResult::InvalidText, std::string{});
        return;
      }

      text.resize(static_cast<std::size_t>(length));
      completion(NativeInputResult::Accepted, std::move(text));
    };

    using Enqueue = void (*)(void *, const char *, const InputConfig *, std::function<void(void *)> *);
    reinterpret_cast<Enqueue>(module + 0x32860)(manager, "CInputStringState", &config, &closed);
  }

  void NativeInputBuffer::input_number(std::uintptr_t module, int initial, int minimum, int maximum,
                                       std::function<void(bool, int)> completion) {
    // RVA 0x173120 copies this 0x28-byte configuration. The native number
    // dialog writes int32, so its destination must not be a smaller record field.
    struct NumberConfig {
      std::int32_t x, y, reserved1, reserved2, minimum, maximum;
      std::int32_t *output;
      std::int32_t flags, padding;
    };

    static_assert(sizeof(NumberConfig) == 0x28);
    number_ = initial;

    NumberConfig config{960, 540, 0, 0, minimum, maximum, &number_, 0x1A, 0};
    std::function<void(void *)> closed = [this, completion = std::move(completion)](void *state) {
      const int result = *reinterpret_cast<const int *>(static_cast<const char *>(state) + 8);
      const bool accepted = result == 0x7FFFFFFD;
      completion(accepted, accepted ? number_ : 0);
    };

    const auto manager = reinterpret_cast<void *(*)()>(module + 0xF0B0)();
    using Enqueue = void (*)(void *, const char *, const NumberConfig *, std::function<void(void *)> *);
    reinterpret_cast<Enqueue>(module + 0x173120)(manager, "CInputKeyDlgState", &config, &closed);
  }

} // namespace s8::scripting
