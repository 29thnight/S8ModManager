#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace s8::platform::windows {

bool read_memory(std::uintptr_t address, std::span<std::byte> bytes);
bool write_memory(std::uintptr_t address, std::span<const std::byte> bytes);
bool write_data_memory(std::uintptr_t address, std::span<const std::byte> bytes);
bool write_code_memory(std::uintptr_t address, std::span<const std::byte> bytes);
bool write_specialty_slot(std::uintptr_t slot, std::uintptr_t object);
bool is_committed_memory(std::uintptr_t address, std::size_t size);

} // namespace s8::platform::windows
