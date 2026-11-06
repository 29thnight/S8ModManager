#pragma once

#include <compare>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Mod manifests and the AOT/JIT selection rule. This file has no Windows or
// game dependency so the rule can be checked in isolation.
namespace s8::scripting {
  enum class ModRuntime { Jit, Aot };

  struct ModVersion {
    std::uint32_t major = 0, minor = 0, patch = 0;
    auto operator<=>(const ModVersion &) const = default;
  };

  struct ModManifest {
    std::string id;
    std::wstring name;
    ModVersion version;
    ModRuntime runtime = ModRuntime::Jit;
    std::filesystem::path directory;
    std::wstring entry; // file name inside directory
    std::wstring type;  // JIT only: the IScriptModule type name
  };

  struct SkippedMod {
    ModManifest manifest;
    std::wstring reason;
  };

  struct ModSelection {
    std::vector<ModManifest> active; // one per id, ordered by id
    std::vector<SkippedMod> skipped;
  };

  std::optional<ModVersion> parse_mod_version(std::string_view text);
  std::expected<ModManifest, std::wstring> parse_mod_manifest(std::string_view json,
                                                              const std::filesystem::path &directory);

  // One mod id is applied at most once. The higher version wins; on a tie JIT
  // wins so a live-coded build replaces the shipped AOT build of that version.
  ModSelection select_mods(std::vector<ModManifest> candidates);
} // namespace s8::scripting
