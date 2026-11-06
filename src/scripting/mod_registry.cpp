#include "mod_registry.h"
#include "../../third_party/nlohmann_json/single_include/nlohmann/json.hpp"

#include <algorithm>
#include <charconv>
#include <map>

namespace s8::scripting {
  namespace {
    std::wstring widen(std::string_view text) {
      return std::filesystem::path(std::u8string(text.begin(), text.end())).wstring();
    }

    bool valid_id(std::string_view id) {
      return !id.empty() && id.size() <= 64 && std::ranges::all_of(id, [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_';
      });
    }

    // A bare file name keeps every mod inside its own directory.
    bool plain_file_name(const std::wstring &file) {
      return !file.empty() && std::filesystem::path(file).filename().wstring() == file && file != L"." &&
             file != L"..";
    }

    // JIT before AOT, then directory order, so ties resolve the same way every run.
    bool preferred(const ModManifest &left, const ModManifest &right) {
      if (left.version != right.version)
        return left.version > right.version;
      if (left.runtime != right.runtime)
        return left.runtime == ModRuntime::Jit;
      return left.directory < right.directory;
    }
  } // namespace

  std::optional<ModVersion> parse_mod_version(std::string_view text) {
    std::uint32_t parts[3]{};
    const char *cursor = text.data();
    const char *end = text.data() + text.size();
    for (int index = 0; index < 3; ++index) {
      const auto [next, error] = std::from_chars(cursor, end, parts[index]);
      if (error != std::errc{} || next == cursor)
        return std::nullopt;
      cursor = next;
      if (index < 2) {
        if (cursor == end || *cursor != '.')
          return std::nullopt;
        ++cursor;
      }
    }
    if (cursor != end)
      return std::nullopt;
    return ModVersion{parts[0], parts[1], parts[2]};
  }

  std::expected<ModManifest, std::wstring> parse_mod_manifest(std::string_view json,
                                                              const std::filesystem::path &directory) {
    const auto root = nlohmann::json::parse(json, nullptr, false);
    if (root.is_discarded() || !root.is_object())
      return std::unexpected(L"mod.json 형식이 올바르지 않습니다.");

    const auto text = [&](const char *key) -> std::optional<std::string> {
      const auto found = root.find(key);
      if (found == root.end() || !found->is_string())
        return std::nullopt;
      return found->get<std::string>();
    };

    ModManifest manifest;
    manifest.directory = directory;

    const auto id = text("id");
    if (!id || !valid_id(*id))
      return std::unexpected(L"id는 영문 소문자·숫자·.-_ 로 된 64자 이하 문자열이어야 합니다.");
    manifest.id = *id;

    const auto version = text("version");
    const auto parsed = version ? parse_mod_version(*version) : std::nullopt;
    if (!parsed)
      return std::unexpected(L"version은 주.부.수 형식이어야 합니다.");
    manifest.version = *parsed;

    manifest.name = widen(text("name").value_or(*id));

    const auto runtime = text("runtime");
    if (runtime == "jit")
      manifest.runtime = ModRuntime::Jit;
    else if (runtime == "aot")
      manifest.runtime = ModRuntime::Aot;
    else
      return std::unexpected(L"runtime은 jit 또는 aot여야 합니다.");

    const auto entry = text("entry");
    manifest.entry = entry ? widen(*entry) : std::wstring{};
    if (!plain_file_name(manifest.entry))
      return std::unexpected(L"entry는 모드 폴더 안의 파일 이름이어야 합니다.");

    if (manifest.runtime == ModRuntime::Jit) {
      const auto type = text("type");
      if (!type || type->empty())
        return std::unexpected(L"JIT 모드는 type이 필요합니다.");
      manifest.type = widen(*type);
    }

    if (const auto api = root.find("apiVersion"); api != root.end()) {
      if (!api->is_number_unsigned() || api->get<std::uint64_t>() == 0 || api->get<std::uint64_t>() > UINT32_MAX)
        return std::unexpected(L"apiVersion은 1 이상의 정수여야 합니다.");
      manifest.api = api->get<std::uint32_t>();
    }

    if (const auto dependencies = root.find("dependencies"); dependencies != root.end()) {
      if (!dependencies->is_object())
        return std::unexpected(L"dependencies는 {\"모드 id\": \"최소 버전\"} 형식이어야 합니다.");
      for (const auto &[dependency, minimum] : dependencies->items()) {
        const auto required = minimum.is_string() ? parse_mod_version(minimum.get<std::string>()) : std::nullopt;
        if (!valid_id(dependency) || !required || dependency == manifest.id)
          return std::unexpected(L"dependencies의 모드 id나 최소 버전이 올바르지 않습니다.");
        manifest.dependencies.push_back({dependency, *required});
      }
    }

    if (const auto conflicts = root.find("conflicts"); conflicts != root.end()) {
      if (!conflicts->is_array())
        return std::unexpected(L"conflicts는 모드 id 배열이어야 합니다.");
      for (const auto &conflict : *conflicts) {
        if (!conflict.is_string() || !valid_id(conflict.get<std::string>()) || conflict.get<std::string>() == manifest.id)
          return std::unexpected(L"conflicts의 모드 id가 올바르지 않습니다.");
        manifest.conflicts.push_back(conflict.get<std::string>());
      }
    }
    return manifest;
  }

  std::expected<ModState, std::wstring> parse_mod_state(std::string_view json) {
    const auto root = nlohmann::json::parse(json, nullptr, false);
    const auto mods = root.is_object() ? root.find("mods") : root.end();
    if (root.is_discarded() || !root.is_object() || mods == root.end() || !mods->is_array())
      return std::unexpected(L"mods.json은 {\"mods\": [...]} 형식이어야 합니다.");

    ModState state;
    for (const auto &item : *mods) {
      const auto id = item.is_object() ? item.find("id") : item.end();
      const auto enabled = item.is_object() ? item.find("enabled") : item.end();
      if (!item.is_object() || id == item.end() || !id->is_string() || !valid_id(id->get<std::string>()) ||
          (enabled != item.end() && !enabled->is_boolean()))
        return std::unexpected(L"mods.json의 항목은 {\"id\": 문자열, \"enabled\": 참/거짓}이어야 합니다.");
      if (std::ranges::any_of(state.mods, [&](const auto &entry) { return entry.id == id->get<std::string>(); }))
        return std::unexpected(L"mods.json에 같은 id가 두 번 있습니다.");
      state.mods.push_back({id->get<std::string>(), enabled == item.end() || enabled->get<bool>()});
    }
    return state;
  }

  std::string write_mod_state(const ModState &state) {
    auto mods = nlohmann::json::array();
    for (const auto &entry : state.mods)
      mods.push_back({{"id", entry.id}, {"enabled", entry.enabled}});
    return nlohmann::json{{"version", 1}, {"mods", mods}}.dump(2) + "\n";
  }

  ModState merge_mod_state(ModState state, const std::vector<ModManifest> &found) {
    std::vector<std::string> added;
    for (const auto &manifest : found)
      if (std::ranges::none_of(state.mods, [&](const auto &entry) { return entry.id == manifest.id; }) &&
          std::ranges::find(added, manifest.id) == added.end())
        added.push_back(manifest.id);
    std::ranges::sort(added);
    for (auto &id : added)
      state.mods.push_back({std::move(id), true});
    return state;
  }

  ModSelection select_mods(std::vector<ModManifest> candidates, const ModState &state, std::uint32_t host_api) {
    std::map<std::string, std::vector<ModManifest>> groups;
    for (auto &candidate : candidates)
      groups[candidate.id].push_back(std::move(candidate));

    ModSelection selection;
    std::vector<ModManifest> winners;
    for (auto &[id, group] : groups) {
      std::ranges::sort(group, preferred);
      winners.push_back(group.front());
      for (std::size_t index = 1; index < group.size(); ++index)
        selection.skipped.push_back({std::move(group[index]), L"같은 id의 다른 모드가 선택되어 중복 적용을 막았습니다."});
    }

    // Base order: the state file, then unlisted ids by id (winners are already id-sorted).
    const auto position = [&](const std::string &id) {
      const auto found = std::ranges::find(state.mods, id, &ModStateEntry::id);
      return found == state.mods.end() ? state.mods.size() : static_cast<std::size_t>(found - state.mods.begin());
    };
    std::ranges::stable_sort(winners, {}, [&](const ModManifest &mod) { return position(mod.id); });

    const auto skip = [&](ModManifest &&mod, std::wstring reason) {
      selection.skipped.push_back({std::move(mod), std::move(reason)});
    };

    std::vector<ModManifest> active;
    for (auto &mod : winners) {
      const auto entry = std::ranges::find(state.mods, mod.id, &ModStateEntry::id);
      if (entry != state.mods.end() && !entry->enabled)
        skip(std::move(mod), L"mods.json에서 꺼진 모드입니다.");
      else if (host_api != 0 && mod.api != 0 && mod.api != host_api)
        skip(std::move(mod), L"모드가 요구하는 API 버전(" + std::to_wstring(mod.api) + L")이 모드 매니저(" +
                                 std::to_wstring(host_api) + L")와 다릅니다.");
      else
        active.push_back(std::move(mod));
    }

    const auto find_active = [&](const std::string &id) {
      return std::ranges::find(active, id, &ModManifest::id);
    };

    // Removing a mod can break a mod that needs it, so repeat until nothing changes.
    const auto remove_unmet = [&] {
      for (bool changed = true; changed;) {
        changed = false;
        for (auto mod = active.begin(); mod != active.end(); ++mod) {
          for (const auto &dependency : mod->dependencies) {
            const auto found = find_active(dependency.id);
            if (found != active.end() && found->version >= dependency.minimum)
              continue;
            const auto &minimum = dependency.minimum;
            skip(std::move(*mod), L"필요한 모드 " + widen(dependency.id) + L" " + std::to_wstring(minimum.major) + L"." +
                                      std::to_wstring(minimum.minor) + L"." + std::to_wstring(minimum.patch) +
                                      (found == active.end() ? L" 이상이 없거나 꺼져 있습니다." : L" 이상보다 버전이 낮습니다."));
            active.erase(mod);
            changed = true;
            break;
          }
          if (changed)
            break;
        }
      }
    };
    remove_unmet();

    // A conflict keeps whichever mod comes first in the base order.
    std::vector<ModManifest> kept;
    for (auto &mod : active) {
      const auto clash = std::ranges::find_if(kept, [&](const ModManifest &other) {
        return std::ranges::find(mod.conflicts, other.id) != mod.conflicts.end() ||
               std::ranges::find(other.conflicts, mod.id) != other.conflicts.end();
      });
      if (clash != kept.end())
        skip(std::move(mod), widen(clash->id) + L" 모드와 충돌하여 먼저 오는 쪽만 적용합니다.");
      else
        kept.push_back(std::move(mod));
    }
    active = std::move(kept);
    remove_unmet();

    // Stable topological order: take the earliest mod whose dependencies are placed.
    while (!active.empty()) {
      const auto ready = std::ranges::find_if(active, [&](const ModManifest &mod) {
        return std::ranges::all_of(mod.dependencies, [&](const ModDependency &dependency) {
          return std::ranges::find(selection.active, dependency.id, &ModManifest::id) != selection.active.end();
        });
      });
      if (ready == active.end()) {
        for (auto &mod : active)
          skip(std::move(mod), L"의존 관계가 순환하여 불러올 수 없습니다.");
        break;
      }
      selection.active.push_back(std::move(*ready));
      active.erase(ready);
    }
    return selection;
  }
} // namespace s8::scripting
