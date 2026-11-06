#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <string>

namespace s8::scripting {

  enum class NativeInputResult { Cancelled, Accepted, InvalidText, UnrepresentableText };

  // Native input buffers remain alive until asynchronous game input completes.
  class NativeInputBuffer {
  public:
    void input_text(std::uintptr_t module, std::string initial, bool multiline, int limit,
                    std::function<void(NativeInputResult, std::string)> completion);
    void input_number(std::uintptr_t module, int initial, int minimum, int maximum,
                      std::function<void(bool, int)> completion);

  private:
    std::array<char, 4096> input_{};
    std::string initial_input_;
    std::int32_t number_ = 0;
  };

} // namespace s8::scripting
