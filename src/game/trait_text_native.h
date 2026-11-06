#pragma once

// The cheat's 72-row legacy name editor is not part of the mod manager. Every
// name now comes from the trait catalog the C# mod publishes, so the runtime's
// legacy override hook point simply keeps the game's text.
namespace s8::game {

inline const wchar_t *trait_text_name_override(const wchar_t *original) noexcept { return original; }

} // namespace s8::game
