#pragma once

#include <cstddef>
#include <cstdint>

namespace s8::platform::windows {

std::uintptr_t allocate_code_near(std::uintptr_t target, std::size_t size);
void free_code(std::uintptr_t address);
// Retained stubs can call DLL functions after their entry patch is restored.
// Pin that code's module before publishing such a callback address.
bool retain_callback_module(std::uintptr_t callback_address);

} // namespace s8::platform::windows
