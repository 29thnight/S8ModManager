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
}
