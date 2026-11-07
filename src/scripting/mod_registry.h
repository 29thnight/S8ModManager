#pragma once

#include <compare>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Mod manifests, the manager state file and the selection rule. This file has
// no Windows or game dependency so the rules can be checked in isolation.
namespace s8::scripting {
  enum class ModRuntime { Jit, Aot };

  struct ModVersion {
    std::uint32_t major = 0, minor = 0, patch = 0;
    auto operator<=>(const ModVersion &) const = default;
  };

  struct ModDependency {
    std::string id;
    ModVersion minimum;
  };

  struct ModManifest {
    std::string id;
    std::wstring name;
    ModVersion version;
    ModRuntime runtime = ModRuntime::Jit;
    std::filesystem::path directory;
    std::wstring entry; // file name inside directory
    std::wstring type;  // JIT only: the IScriptModule type name
    std::uint32_t api = 0; // required engine API version; 0 = not declared
    std::uint32_t game_build = 0; // game executable build stamp the mod targets; 0 = not declared
    std::vector<ModDependency> dependencies;
    std::vector<std::string> conflicts;
  };

  // scripts\mods.json: which mods are enabled and the order they load in.
  struct ModStateEntry {
    std::string id;
    bool enabled = true;
    bool operator==(const ModStateEntry &) const = default;
  };

  struct ModState {
    std::vector<ModStateEntry> mods;
    bool operator==(const ModState &) const = default;
  };

  struct SkippedMod {
    ModManifest manifest;
    std::wstring reason;
  };

  struct ModSelection {
    std::vector<ModManifest> active; // in load order
    std::vector<SkippedMod> skipped;
  };

  std::optional<ModVersion> parse_mod_version(std::string_view text);
  std::expected<ModManifest, std::wstring> parse_mod_manifest(std::string_view json,
                                                              const std::filesystem::path &directory);

  std::expected<ModState, std::wstring> parse_mod_state(std::string_view json);
  std::string write_mod_state(const ModState &state);
  // Appends newly found ids (enabled) and keeps entries for absent mods, so a
  // mod that is removed for a while keeps its setting and position.
  ModState merge_mod_state(ModState state, const std::vector<ModManifest> &found);
  // Every found mod switched off: used when mods.json does not parse.
  ModState blocked_mod_state(const std::vector<ModManifest> &found);
  // The first dependency of mod listed in failed, if any.
  std::optional<std::string> failed_dependency(const ModManifest &mod, const std::vector<std::string> &failed);

  // One mod id is applied at most once: the higher version wins, and on a tie
  // JIT wins so a live-coded build replaces the shipped AOT build. Then disabled
  // mods, API mismatches, unmet dependencies and conflicts are skipped, and the
  // rest is ordered by the state file with every dependency loaded first.
  // host_api 0 skips the API check.
  ModSelection select_mods(std::vector<ModManifest> candidates, const ModState &state = {},
                           std::uint32_t host_api = 0);
} // namespace s8::scripting
