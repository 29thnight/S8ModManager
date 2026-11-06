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
    return manifest;
  }

  ModSelection select_mods(std::vector<ModManifest> candidates) {
    std::map<std::string, std::vector<ModManifest>> groups;
    for (auto &candidate : candidates)
      groups[candidate.id].push_back(std::move(candidate));

    ModSelection selection;
    for (auto &[id, group] : groups) {
      std::ranges::sort(group, preferred);
      selection.active.push_back(group.front());
      for (std::size_t index = 1; index < group.size(); ++index)
        selection.skipped.push_back({std::move(group[index]), L"같은 id의 다른 모드가 선택되어 중복 적용을 막았습니다."});
    }
    return selection;
  }
} // namespace s8::scripting
