#include "../src/scripting/mod_registry.h"

#include <cassert>

using namespace s8::scripting;

namespace {
  ModManifest manifest(const char *id, ModVersion version, ModRuntime runtime, const wchar_t *folder) {
    ModManifest value;
    value.id = id;
    value.version = version;
    value.runtime = runtime;
    value.directory = folder;
    value.entry = L"S8.GameScripts.dll";
    return value;
  }
} // namespace

void mod_selection() {
  // Same version in both forms: JIT wins and the AOT build is not applied.
  auto tie = select_mods({manifest("s8.editors", {0, 0, 1}, ModRuntime::Aot, L"a"),
                          manifest("s8.editors", {0, 0, 1}, ModRuntime::Jit, L"b")});
  assert(tie.active.size() == 1 && tie.active[0].runtime == ModRuntime::Jit);
  assert(tie.skipped.size() == 1 && tie.skipped[0].manifest.runtime == ModRuntime::Aot);

  // A newer AOT build beats an older JIT build.
  auto newer = select_mods({manifest("s8.editors", {0, 0, 1}, ModRuntime::Jit, L"a"),
                            manifest("s8.editors", {0, 1, 0}, ModRuntime::Aot, L"b")});
  assert(newer.active.size() == 1 && newer.active[0].runtime == ModRuntime::Aot);

  // Two copies of the same form are still applied once, chosen by folder order.
  auto copies = select_mods({manifest("s8.editors", {0, 0, 1}, ModRuntime::Jit, L"b"),
                             manifest("s8.editors", {0, 0, 1}, ModRuntime::Jit, L"a")});
  assert(copies.active.size() == 1 && copies.active[0].directory == L"a" && copies.skipped.size() == 1);

  // Different ids are independent.
  auto distinct = select_mods({manifest("s8.editors", {0, 0, 1}, ModRuntime::Jit, L"a"),
                               manifest("other.mod", {1, 0, 0}, ModRuntime::Aot, L"b")});
  assert(distinct.active.size() == 2 && distinct.skipped.empty());

  assert((parse_mod_version("0.0.1") == ModVersion{0, 0, 1}));
  assert(!parse_mod_version("0.1") && !parse_mod_version("1.2.3.4") && !parse_mod_version("a.b.c"));

  const auto jit = parse_mod_manifest(
      R"({"id":"s8.editors","version":"0.0.1","runtime":"jit","entry":"S8.GameScripts.dll","type":"S8.GameScripts.ModApplication"})",
      L"mods");
  assert(jit && jit->runtime == ModRuntime::Jit && jit->type == L"S8.GameScripts.ModApplication");
  assert(!parse_mod_manifest(R"({"id":"s8.editors","version":"0.0.1","runtime":"jit","entry":"x.dll"})", L"m"));
  assert(!parse_mod_manifest(R"({"id":"s8.editors","version":"0.0.1","runtime":"aot","entry":"..\\x.dll"})", L"m"));
  assert(!parse_mod_manifest(R"({"id":"S8 Editors","version":"0.0.1","runtime":"aot","entry":"x.dll"})", L"m"));
  assert(!parse_mod_manifest("not json", L"m"));

  // Declarations: dependencies, conflicts and the API version.
  const auto declared = parse_mod_manifest(
      R"({"id":"a","version":"1.0.0","runtime":"aot","entry":"a.dll","apiVersion":17,"dependencies":{"core":"0.2.0"},"conflicts":["b"]})",
      L"m");
  assert(declared && declared->api == 17 && declared->dependencies.size() == 1 &&
         declared->dependencies[0].id == "core" && (declared->dependencies[0].minimum == ModVersion{0, 2, 0}) &&
         declared->conflicts == std::vector<std::string>{"b"});
  assert(!parse_mod_manifest(R"({"id":"a","version":"1.0.0","runtime":"aot","entry":"a.dll","dependencies":{"a":"1.0.0"}})", L"m"));
  assert(!parse_mod_manifest(R"({"id":"a","version":"1.0.0","runtime":"aot","entry":"a.dll","dependencies":{"core":"1.0"}})", L"m"));
  assert(!parse_mod_manifest(R"({"id":"a","version":"1.0.0","runtime":"aot","entry":"a.dll","conflicts":"b"})", L"m"));
  assert(!parse_mod_manifest(R"({"id":"a","version":"1.0.0","runtime":"aot","entry":"a.dll","apiVersion":0})", L"m"));
  // gameBuild: the executable build stamp as hex; omitted means any build.
  const auto built = parse_mod_manifest(R"({"id":"a","version":"1.0.0","runtime":"aot","entry":"a.dll","gameBuild":"69A67ED1"})", L"m");
  assert(built && built->game_build == 0x69A67ED1 && declared->game_build == 0);
  assert(!parse_mod_manifest(R"({"id":"a","version":"1.0.0","runtime":"aot","entry":"a.dll","gameBuild":"xyz"})", L"m"));
  assert(!parse_mod_manifest(R"({"id":"a","version":"1.0.0","runtime":"aot","entry":"a.dll","gameBuild":1})", L"m"));
  assert(!parse_mod_manifest(R"({"id":"a","version":"1.0.0","runtime":"aot","entry":"a.dll","gameBuild":"169A67ED1"})", L"m"));
}

namespace {
  ModManifest needing(const char *id, const char *dependency, ModVersion minimum, const wchar_t *folder) {
    auto value = manifest(id, {1, 0, 0}, ModRuntime::Jit, folder);
    value.dependencies.push_back({dependency, minimum});
    return value;
  }

  std::vector<std::string> ids(const ModSelection &selection) {
    std::vector<std::string> result;
    for (const auto &mod : selection.active)
      result.push_back(mod.id);
    return result;
  }
} // namespace

void mod_state_and_order() {
  // State file round trip; missing "enabled" means enabled.
  const auto state = parse_mod_state(R"({"version":1,"mods":[{"id":"b","enabled":false},{"id":"a"}]})");
  assert(state && state->mods.size() == 2 && !state->mods[0].enabled && state->mods[1].enabled);
  assert(parse_mod_state(write_mod_state(*state)) == *state);
  assert(!parse_mod_state(R"({"mods":[{"id":"a"},{"id":"a"}]})"));
  assert(!parse_mod_state(R"({"mods":[{"id":"a","enabled":"yes"}]})"));
  assert(!parse_mod_state("[]"));

  // Merge keeps listed entries (even absent mods) and appends new ids enabled.
  const auto merged = merge_mod_state(*state, {manifest("c", {1, 0, 0}, ModRuntime::Jit, L"c"),
                                               manifest("a", {1, 0, 0}, ModRuntime::Jit, L"a")});
  assert(merged.mods.size() == 3 && merged.mods[0].id == "b" && merged.mods[2].id == "c" && merged.mods[2].enabled);

  // An unreadable state file loads nothing, so a mod the user switched off stays off.
  const std::vector<ModManifest> found{manifest("a", {1, 0, 0}, ModRuntime::Jit, L"a"),
                                      manifest("b", {1, 0, 0}, ModRuntime::Jit, L"b")};
  const auto blocked = select_mods(found, blocked_mod_state(found));
  assert(blocked.active.empty() && blocked.skipped.size() == 2);

  // Disabled mods are skipped; the state file decides the order.
  ModState order{{{"z", true}, {"y", false}, {"x", true}}};
  auto ordered = select_mods({manifest("x", {1, 0, 0}, ModRuntime::Jit, L"x"),
                              manifest("y", {1, 0, 0}, ModRuntime::Jit, L"y"),
                              manifest("z", {1, 0, 0}, ModRuntime::Jit, L"z")},
                             order);
  assert((ids(ordered) == std::vector<std::string>{"z", "x"}) && ordered.skipped.size() == 1);

  // A dependency loads first even when the state file lists it later.
  ModState late{{{"user", true}, {"core", true}}};
  auto dependent = select_mods({needing("user", "core", {1, 0, 0}, L"u"), manifest("core", {1, 0, 0}, ModRuntime::Jit, L"c")},
                               late);
  assert((ids(dependent) == std::vector<std::string>{"core", "user"}));

  // A dependency that failed to load at run time stops the dependent too.
  const auto user = needing("user", "core", {1, 0, 0}, L"u");
  assert(failed_dependency(user, {"core"}) == "core");
  assert(!failed_dependency(user, {"other"}) && !failed_dependency(user, {}));

  // Missing, disabled or too old dependencies skip the dependent, and that cascades.
  ModState core_off{{{"core", false}}};
  auto cascade = select_mods({manifest("core", {1, 0, 0}, ModRuntime::Jit, L"c"), needing("user", "core", {1, 0, 0}, L"u"),
                              needing("addon", "user", {1, 0, 0}, L"a")},
                             core_off);
  assert(cascade.active.empty() && cascade.skipped.size() == 3);
  auto old = select_mods({manifest("core", {0, 9, 0}, ModRuntime::Jit, L"c"), needing("user", "core", {1, 0, 0}, L"u")});
  assert((ids(old) == std::vector<std::string>{"core"}));

  // A conflict keeps the mod that comes first; either side may declare it.
  auto left = manifest("left", {1, 0, 0}, ModRuntime::Jit, L"l");
  left.conflicts.push_back("right");
  ModState right_first{{{"right", true}, {"left", true}}};
  auto conflict = select_mods({left, manifest("right", {1, 0, 0}, ModRuntime::Jit, L"r")}, right_first);
  assert((ids(conflict) == std::vector<std::string>{"right"}) && conflict.skipped[0].manifest.id == "left");

  // Cycles cannot load.
  auto cycle = select_mods({needing("p", "q", {1, 0, 0}, L"p"), needing("q", "p", {1, 0, 0}, L"q")});
  assert(cycle.active.empty() && cycle.skipped.size() == 2);

  // API mismatch is skipped only when both sides declare a version.
  auto newer_api = manifest("api", {1, 0, 0}, ModRuntime::Jit, L"api");
  newer_api.api = 18;
  assert(select_mods({newer_api}, {}, 17).active.empty() && select_mods({newer_api}, {}, 0).active.size() == 1);

  // A game build mismatch is skipped only when both sides declare one, and it cascades.
  auto pinned = manifest("pinned", {1, 0, 0}, ModRuntime::Jit, L"p");
  pinned.game_build = 0x69A67ED1;
  const auto other_build = select_mods({pinned, needing("user", "pinned", {1, 0, 0}, L"u")}, {}, 0, 0x12345678);
  assert(other_build.active.empty() && other_build.skipped.size() == 2);
  assert(select_mods({pinned}, {}, 0, 0x69A67ED1).active.size() == 1);
  assert(select_mods({pinned}, {}, 0, 0).active.size() == 1);
}
