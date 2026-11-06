#pragma once

#include "../domain/secondary_jewel.h"
#include <cstdint>

namespace s8::game {

// The hook and its callback state must remain loaded until callbacks are quiescent.
bool set_secondary_jewels(const domain::SecondaryJewelMask& mask, std::uintptr_t module_base,
                          std::uintptr_t game_base_pointer);
void reset_secondary_jewel_cache();

} // namespace s8::game
