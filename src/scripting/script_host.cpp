#include "engine_services.h"
#include "mod_registry.h"
#include "script_api.h"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <utility>
#include <coreclr_delegates.h>
#include <filesystem>
#include <fstream>
#include <hostfxr.h>
#include <nethost.h>
#include <sstream>
#include <string>
#include <vector>

namespace s8::scripting {
  const Connection &game_connection();
}

namespace {
  using namespace s8::scripting;
  thread_local const EngineApi *current = nullptr;
  // The mod whose load or event callback is running, so a call can be checked
  // against the mod that made it.
  thread_local const std::string *calling_mod = nullptr;

  bool save_state(const std::filesystem::path &path, const ModState &state);
  std::int64_t read_mod_list(void *data, std::uint32_t bytes);
  void log(const std::wstring &message);
  std::int64_t set_mod_enabled(std::int64_t argument, const void *data, std::uint32_t bytes);

  std::int64_t invoke(Operation operation, std::uint64_t target, std::int64_t argument, void *data,
                      std::uint32_t bytes) {
    if (!current)
      return -1;

    if (operation == Operation::NewUiId) {
      static std::atomic<std::int32_t> next{0};
      const std::int32_t id = next.fetch_add(1, std::memory_order_relaxed) + 1;
      return id > 0 ? id : -1;
    }
    if (operation == Operation::ReadModList)
      return read_mod_list(data, bytes);
    if (operation == Operation::SetModEnabled)
      return set_mod_enabled(argument, data, bytes);
    if (operation == Operation::SetOptionsTab && (!calling_mod || !may_set_options_tab(*calling_mod))) {
      log(L"모드 관리가 아닌 모드의 환경설정 탭 등록을 거부했습니다.");
      return -1;
    }

    if ((operation >= Operation::GetPersonality && operation <= Operation::WriteMemory) ||
        operation == Operation::GetGemCategory) {
      const auto session = current->invoke(Operation::GetSession, 0, 0, nullptr, 0);

      if (session < 0)
        return session;

      return invoke_engine(operation, target, argument, data, bytes, static_cast<std::uint64_t>(session));
    }

    return current->invoke(operation, target, argument, data, bytes);
  }

  // JIT mods run in the managed host; AOT mods are native libraries that export the same entry points.
  using ManagedRegister = int(CORECLR_DELEGATE_CALLTYPE *)(const wchar_t *, const wchar_t *, const wchar_t *,
                                                           const wchar_t *, const wchar_t *);
  using ManagedDispatch = int(CORECLR_DELEGATE_CALLTYPE *)(const EngineApi *, int, int, int, int);
  using ManagedUnload = int(CORECLR_DELEGATE_CALLTYPE *)(const EngineApi *, int);
  using AotLoad = int (*)(const wchar_t *, const wchar_t *);
  using AotDispatch = int (*)(const EngineApi *, int, int, int);
  using AotUnload = int (*)(const EngineApi *);

  // A mod switched off during the run keeps its slot (JIT handle or AOT library)
  // and only stops receiving events; switching it back on reuses the slot, and
  // the next load event initializes it again.
  struct LoadedMod {
    ModManifest manifest;
    int handle = -1;
    AotDispatch aot = nullptr;
    AotUnload aot_unload = nullptr;
    bool active = true;
  };

  // What the options screen reports: every mod found this run and what became of it.
  struct ModListing {
    ModManifest manifest;
    bool enabled = true;
    const wchar_t *state = L"loaded";
    std::wstring reason;
  };

  struct ModHost {
    bool loaded = false;
    std::filesystem::path base;
    std::vector<LoadedMod> mods;
    std::vector<ModListing> listing;
    std::vector<ModManifest> found;
    bool reselect = false; // mods.json changed during a callback; applied once delivery ends
    bool changed = false;  // the selection changed; the UI sends a load event for new mods
    ManagedRegister managed_register = nullptr;
    ManagedDispatch managed_dispatch = nullptr;
    ManagedUnload managed_unload = nullptr;
  } host;

  void log(const std::wstring &message) {
    OutputDebugStringW((L"[S8 mods] " + message + L"\n").c_str());
  }

  // The game's job system links queued objects by 32-bit offsets (in 4-byte units) from
  // a queue in its process heap, so what the game allocates must stay within 8 GB of that
  // heap. A starting .NET GC reserves tens of GB in one block; placed right after the
  // heap, it pushes every later game allocation out of range, and the game crashes in its
  // job-system teardown at exit. Hold the free space after the heap while a runtime
  // starts, so its reservations land beyond it, then give the space back to the game.
  class NearHeapHold {
  public:
    NearHeapHold() {
      constexpr std::uintptr_t span = 16ull << 30, granularity = 0x10000;
      const auto base = reinterpret_cast<std::uintptr_t>(GetProcessHeap());
      for (std::uintptr_t address = base; address < base + span;) {
        MEMORY_BASIC_INFORMATION info{};
        if (!VirtualQuery(reinterpret_cast<void *>(address), &info, sizeof(info)))
          break;
        const auto next = reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize;
        const auto start = (address + granularity - 1) & ~(granularity - 1);
        const auto stop = std::min(next, base + span) & ~(granularity - 1);
        if (info.State == MEM_FREE && stop > start)
          if (void *held = VirtualAlloc(reinterpret_cast<void *>(start), stop - start, MEM_RESERVE, PAGE_NOACCESS))
            held_.push_back(held);
        address = next;
      }
    }
    ~NearHeapHold() {
      for (void *held : held_)
        VirtualFree(held, 0, MEM_RELEASE);
    }
    NearHeapHold(const NearHeapHold &) = delete;
    NearHeapHold &operator=(const NearHeapHold &) = delete;

  private:
    std::vector<void *> held_;
  };

  std::filesystem::path module_directory() {
    wchar_t image[32768]{};
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&module_directory), &self);
    if (!GetModuleFileNameW(self, image, 32768))
      return {};
    return std::filesystem::path(image).parent_path();
  }

  bool load_managed_host(const std::filesystem::path &directory) {
    wchar_t fxr_path[32768]{};
    std::size_t capacity = std::size(fxr_path);

    if (get_hostfxr_path(fxr_path, &capacity, nullptr) != 0)
      return false;

    HMODULE fxr =
        LoadLibraryExW(fxr_path, nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);

    if (!fxr)
      return false;

    const auto init = reinterpret_cast<hostfxr_initialize_for_runtime_config_fn>(
        GetProcAddress(fxr, "hostfxr_initialize_for_runtime_config"));

    const auto get_delegate =
        reinterpret_cast<hostfxr_get_runtime_delegate_fn>(GetProcAddress(fxr, "hostfxr_get_runtime_delegate"));

    const auto close = reinterpret_cast<hostfxr_close_fn>(GetProcAddress(fxr, "hostfxr_close"));

    if (!init || !get_delegate || !close)
      return false;

    // Lets the host apply method-body edits to Debug-built mods in place (hot
    // reload). The runtime reads it once at start; Release assemblies are unaffected.
    SetEnvironmentVariableW(L"DOTNET_MODIFIABLE_ASSEMBLIES", L"debug");

    hostfxr_handle context = nullptr;
    const NearHeapHold hold;

    const auto config = directory / L"S8.Scripting.Host.runtimeconfig.json";
    const int result = init(config.c_str(), nullptr, &context);

    if (result < 0 || !context)
      return false;

    load_assembly_and_get_function_pointer_fn load = nullptr;

    const int delegate_result =
        get_delegate(context, hdt_load_assembly_and_get_function_pointer, reinterpret_cast<void **>(&load));
    close(context);

    if (delegate_result != 0 || !load)
      return false;

    const auto assembly = directory / L"S8.Scripting.Host.dll";
    constexpr auto type = L"S8.Scripting.Host.Entry, S8.Scripting.Host";

    return load(assembly.c_str(), type, L"Register", UNMANAGEDCALLERSONLY_METHOD, nullptr,
                reinterpret_cast<void **>(&host.managed_register)) == 0 &&
           load(assembly.c_str(), type, L"Dispatch", UNMANAGEDCALLERSONLY_METHOD, nullptr,
                reinterpret_cast<void **>(&host.managed_dispatch)) == 0 &&
           load(assembly.c_str(), type, L"Unload", UNMANAGEDCALLERSONLY_METHOD, nullptr,
                reinterpret_cast<void **>(&host.managed_unload)) == 0 &&
           host.managed_register && host.managed_dispatch && host.managed_unload;
  }

  std::vector<ModManifest> scan_mods(const std::filesystem::path &root) {
    std::vector<ModManifest> manifests;
    std::error_code error;

    for (const auto &entry : std::filesystem::directory_iterator(root, error)) {
      const auto path = entry.path() / L"mod.json";
      if (!entry.is_directory(error) || !std::filesystem::is_regular_file(path, error))
        continue;

      std::ifstream file(path, std::ios::binary);
      std::ostringstream content;
      content << file.rdbuf();
      auto manifest = parse_mod_manifest(content.str(), entry.path());

      if (manifest)
        manifests.push_back(std::move(*manifest));
      else
        log(path.wstring() + L": " + manifest.error());
    }
    return manifests;
  }

  bool load_aot(LoadedMod &mod) {
    const auto path = mod.manifest.directory / mod.manifest.entry;

    // A library already in the process was applied by someone else; loading it again would double-apply.
    if (GetModuleHandleW(path.c_str())) {
      log(path.wstring() + L": 이미 적재된 AOT 모드입니다.");
      return false;
    }

    const NearHeapHold hold; // a NativeAOT library starts its own GC
    HMODULE library =
        LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!library)
      return false;

    const auto load = reinterpret_cast<AotLoad>(GetProcAddress(library, "S8ModLoad"));
    mod.aot = reinterpret_cast<AotDispatch>(GetProcAddress(library, "S8ModDispatch"));
    mod.aot_unload = reinterpret_cast<AotUnload>(GetProcAddress(library, "S8ModUnload")); // optional

    if (!load || !mod.aot || load(mod.manifest.directory.c_str(), host.base.c_str()) != 0) {
      mod.aot = nullptr;
      FreeLibrary(library);
      return false;
    }
    return true;
  }

  bool load_jit(LoadedMod &mod) {
    if (!host.managed_dispatch && !load_managed_host(host.base / L"scripts"))
      return false;

    const auto id = std::filesystem::path(std::u8string(mod.manifest.id.begin(), mod.manifest.id.end())).wstring();
    const auto assembly = mod.manifest.directory / mod.manifest.entry;

    mod.handle = host.managed_register(id.c_str(), mod.manifest.directory.c_str(), host.base.c_str(),
                                       assembly.c_str(), mod.manifest.type.c_str());
    return mod.handle >= 0;
  }

  // A missing file is created and newly found mods are appended. A file that
  // does not parse is left untouched so the user's edits are not overwritten,
  // and no mod loads: treating it as empty would re-enable mods the user switched off.
  ModState load_state(const std::filesystem::path &path, const std::vector<ModManifest> &found) {
    ModState state;
    std::error_code error;
    const bool exists = std::filesystem::is_regular_file(path, error);
    if (exists) {
      std::ifstream file(path, std::ios::binary);
      std::ostringstream content;
      content << file.rdbuf();
      auto parsed = parse_mod_state(content.str());
      if (!parsed) {
        log(path.wstring() + L": " + parsed.error() + L" 파일을 고칠 때까지 모드를 하나도 적재하지 않습니다.");
        return blocked_mod_state(found);
      }
      state = std::move(*parsed);
    }

    auto merged = merge_mod_state(state, found);
    if (!exists || merged != state)
      save_state(path, merged);
    return merged;
  }

  std::filesystem::path state_path() {
    return host.base / L"scripts" / L"mods.json";
  }

  bool save_state(const std::filesystem::path &path, const ModState &state) {
    std::error_code error;
    const auto temporary = path.wstring() + L".tmp";
    {
      // A short write (full disk) must not replace the good file with a partial one.
      std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
      file << write_mod_state(state);
      file.close();
      if (!file) {
        std::filesystem::remove(temporary, error);
        log(path.wstring() + L": 임시 파일을 쓰지 못해 상태 파일을 저장하지 않았습니다.");
        return false;
      }
    }
    std::filesystem::rename(temporary, path, error);
    if (error)
      log(path.wstring() + L": 상태 파일을 저장하지 못했습니다.");
    return !error;
  }

  std::wstring version_text(const ModVersion &version) {
    return std::to_wstring(version.major) + L"." + std::to_wstring(version.minor) + L"." +
           std::to_wstring(version.patch);
  }

  std::wstring hex(std::uint32_t value) {
    wchar_t text[16]{};
    if (value)
      swprintf_s(text, L"%08X", value);
    return text;
  }

  // Tabs and line breaks separate fields and records, so they never appear inside one.
  std::wstring field(std::wstring text) {
    std::ranges::replace(text, L'\t', L' ');
    std::ranges::replace(text, L'\n', L' ');
    std::ranges::replace(text, L'\r', L' ');
    return text;
  }

  std::wstring widen_id(const std::string &id) {
    return std::filesystem::path(std::u8string(id.begin(), id.end())).wstring();
  }

  // Proxy DLLs other tools put beside the game: hid.dll (the overlay cheat) and
  // version.dll (an older trait editor). The game loads them, not the manager, so
  // the list shows them as runtime "external", which SetModEnabled does not accept.
  std::wstring external_mods() {
    wchar_t executable[MAX_PATH]{};
    if (!GetModuleFileNameW(nullptr, executable, MAX_PATH))
      return {};
    const auto directory = std::filesystem::path(executable).parent_path();
    std::wstring lines;
    struct External {
      const wchar_t *id, *file, *name;
    };
    for (const auto &[id, file, name] : {External{L"external.hid", L"hid.dll", L"종합 치트 모드"},
                                         External{L"external.version", L"version.dll", L"종합편집기 모드"}}) {
      const auto path = directory / file;
      std::error_code error;
      if (!std::filesystem::is_regular_file(path, error))
        continue;
      // By full path: the system DLL of the same name is often loaded as well.
      const bool running = GetModuleHandleW(path.c_str()) != nullptr;
      lines += std::wstring(id) + L"\t" + name + L"\t-\texternal\t1\t" + (running ? L"loaded" : L"skipped") + L"\t" +
               (running ? L"" : L"게임이 불러오지 않았습니다.") + L"\t0\t\t\t\n";
    }
    return lines;
  }

  std::int64_t read_mod_list(void *data, std::uint32_t bytes) {
    std::wstring text = std::to_wstring(api_version) + L"\t" + hex(game_connection().game_build) + L"\n";
    text += external_mods();
    for (const auto &item : host.listing) {
      const auto &mod = item.manifest;
      std::wstring dependencies, conflicts;
      for (const auto &dependency : mod.dependencies)
        dependencies += (dependencies.empty() ? L"" : L",") + widen_id(dependency.id) + L">=" +
                        version_text(dependency.minimum);
      for (const auto &conflict : mod.conflicts)
        conflicts += (conflicts.empty() ? L"" : L",") + widen_id(conflict);
      text += field(widen_id(mod.id)) + L"\t" + field(mod.name) + L"\t" + version_text(mod.version) + L"\t" +
              (mod.runtime == ModRuntime::Aot ? L"aot" : L"jit") + L"\t" + (item.enabled ? L"1" : L"0") + L"\t" +
              item.state + L"\t" + field(item.reason) + L"\t" + std::to_wstring(mod.api) + L"\t" +
              hex(mod.game_build) + L"\t" + dependencies + L"\t" + conflicts + L"\n";
    }
    // The caller learns the length first and retries with room for all of it.
    if (data && bytes >= text.size() * sizeof(wchar_t))
      std::memcpy(data, text.data(), text.size() * sizeof(wchar_t));
    return static_cast<std::int64_t>(text.size());
  }

  std::int64_t set_mod_enabled(std::int64_t argument, const void *data, std::uint32_t bytes) {
    if (!data || bytes == 0 || bytes > 256 || bytes % sizeof(wchar_t) != 0 || (argument != 0 && argument != 1))
      return -1;
    const std::wstring wide(static_cast<const wchar_t *>(data), bytes / sizeof(wchar_t));
    const auto narrow = std::filesystem::path(wide).u8string();
    const std::string id(narrow.begin(), narrow.end());
    if (std::ranges::none_of(host.listing, [&](const ModListing &item) { return item.manifest.id == id; }))
      return -1;

    ModState state;
    const auto path = state_path();
    {
      std::ifstream file(path, std::ios::binary);
      std::ostringstream content;
      content << file.rdbuf();
      auto parsed = parse_mod_state(content.str());
      if (!parsed) {
        log(path.wstring() + L": " + parsed.error() + L" 고쳐 쓰지 않습니다.");
        return -1;
      }
      state = std::move(*parsed);
    }
    const auto entry = std::ranges::find(state.mods, id, &ModStateEntry::id);
    if (entry == state.mods.end())
      state.mods.push_back({id, argument == 1});
    else
      entry->enabled = argument == 1;
    if (!save_state(path, state))
      return -1;
    for (auto &item : host.listing)
      if (item.manifest.id == id)
        item.enabled = argument == 1;
    host.reselect = true;
    return 0;
  }

  bool same_mod(const ModManifest &left, const ModManifest &right) {
    return left.id == right.id && left.directory == right.directory;
  }

  // Stops a mod: it releases its registrations and undoes its own changes. An AOT
  // library without S8ModUnload cannot be stopped and keeps running this session.
  bool unload(LoadedMod &mod, const EngineApi *api) {
    if (mod.aot ? !mod.aot_unload : mod.handle < 0)
      return false;
    const int status = mod.aot ? mod.aot_unload(api) : host.managed_unload(api, mod.handle);
    if (status != 0)
      log(mod.manifest.directory.wstring() + L": 모드를 내리는 중 오류가 있었습니다.");
    mod.active = false;
    return true;
  }

  // Applies a selection to the running mods: mods that left it are stopped, the
  // rest run in its order, and new ones are loaded (or their kept slot reused).
  void apply_selection(const ModState &state, const EngineApi *api) {
    auto selection = select_mods(host.found, state, api_version, game_connection().game_build);
    const auto enabled = [&](const std::string &id) {
      const auto entry = std::ranges::find(state.mods, id, &ModStateEntry::id);
      return entry == state.mods.end() || entry->enabled;
    };
    const auto selected = [&](const ModManifest &manifest) {
      return std::ranges::any_of(selection.active, [&](const auto &other) { return same_mod(manifest, other); });
    };

    host.listing.clear();
    for (const auto &skipped : selection.skipped) {
      log(skipped.manifest.directory.wstring() + L": " + skipped.reason);
      ModListing listing{skipped.manifest, enabled(skipped.manifest.id), L"skipped", skipped.reason};
      const auto running = std::ranges::find_if(host.mods, [&](const LoadedMod &mod) {
        return mod.active && same_mod(mod.manifest, skipped.manifest);
      });
      if (running != host.mods.end() && !unload(*running, api)) {
        listing.state = L"loaded";
        listing.reason = L"이 AOT 모드는 내릴 수 없어 다음 실행부터 빠집니다.";
      }
      host.listing.push_back(std::move(listing));
    }

    std::vector<LoadedMod> ordered;
    // Dependencies come first in the selection, so a failure here reaches every
    // dependent below it, and through them their own dependents.
    std::vector<std::string> failed;
    for (auto &manifest : selection.active) {
      ModListing listing{manifest, true};
      const auto kept = std::ranges::find_if(host.mods, [&](const LoadedMod &mod) { return same_mod(mod.manifest, manifest); });
      if (kept != host.mods.end()) {
        kept->active = true;
        kept->manifest = manifest;
        ordered.push_back(std::move(*kept));
        host.mods.erase(kept);
      } else if (const auto dependency = failed_dependency(manifest, failed)) {
        failed.push_back(manifest.id);
        listing.state = L"failed";
        listing.reason = L"의존 모드 " + widen_id(*dependency) + L"를 불러오지 못해 건너뜁니다.";
        log(manifest.directory.wstring() + L": " + listing.reason);
      } else {
        LoadedMod mod{manifest};
        const auto *caller = std::exchange(calling_mod, &mod.manifest.id);
        const bool loaded = mod.manifest.runtime == ModRuntime::Aot ? load_aot(mod) : load_jit(mod);
        calling_mod = caller;
        if (loaded) {
          ordered.push_back(std::move(mod));
        } else {
          failed.push_back(manifest.id);
          log(manifest.directory.wstring() + L": 모드를 불러오지 못했습니다.");
          listing.state = L"failed";
          listing.reason = L"모드를 불러오지 못했습니다.";
        }
      }
      host.listing.push_back(std::move(listing));
    }
    // Stopped mods (and AOT mods that could not stop) keep their slots after the selection.
    for (auto &mod : host.mods)
      if (!selected(mod.manifest)) {
        if (mod.active && std::ranges::none_of(host.found, [&](const auto &manifest) {
              return same_mod(mod.manifest, manifest);
            }))
          unload(mod, api);
        ordered.push_back(std::move(mod));
      }
    host.mods = std::move(ordered);
  }

  void reselect_mods(const EngineApi *api) {
    host.reselect = false;
    const auto path = state_path();
    std::ifstream file(path, std::ios::binary);
    std::ostringstream content;
    content << file.rdbuf();
    auto state = parse_mod_state(content.str());
    if (!state) {
      log(path.wstring() + L": " + state.error() + L" 모드 구성을 바꾸지 않습니다.");
      return;
    }
    apply_selection(*state, api);
    host.changed = true;
  }

  void load_mods() {
    host.loaded = true;
    host.base = module_directory();

    host.found = scan_mods(host.base / L"scripts" / L"mods");
    apply_selection(load_state(state_path(), host.found), nullptr);
  }
} // namespace

// Every active mod receives each event. The first failing status is reported,
// but one mod's failure does not stop delivery to the others.
// Exported only so the smoke test can check the rejection paths outside the game.
extern "C" __declspec(dllexport) int S8ScriptDispatch(const s8::scripting::EngineApi *api, s8::scripting::Event event, int page,
                                int action) {
  using namespace s8::scripting;

  if (!api)
    return static_cast<int>(Status::InvalidApi);

  if (api->version != api_version || api->size != sizeof(*api))
    return static_cast<int>(Status::ApiVersionMismatch);

  const auto &connection = game_connection();

  if (connection.status != Status::Ready)
    return static_cast<int>(connection.status);

  if (api->connection.status != connection.status || api->connection.game_build != connection.game_build ||
      api->connection.capabilities != connection.capabilities)
    return static_cast<int>(Status::InvalidBinding);

  if (!api->invoke)
    return static_cast<int>(Status::InvalidApi);

  const bool first_load = !host.loaded;
  if (first_load)
    load_mods();

  const EngineApi facade{api_version, sizeof(EngineApi), connection, invoke};
  const auto *previous = current;
  current = api;

  // Menu construction requests discovery before any controls are built.
  if (!first_load && !previous && event == Event::Configure && action == 1) {
    host.found = scan_mods(host.base / L"scripts" / L"mods");
    apply_selection(load_state(state_path(), host.found), &facade);
  }

  int result = 0;
  bool configured = false;
  for (const auto &mod : host.mods) {
    if (!mod.active)
      continue;
    const auto *caller = std::exchange(calling_mod, &mod.manifest.id);
    const int status = mod.aot ? mod.aot(&facade, static_cast<int>(event), page, action)
                               : host.managed_dispatch(&facade, mod.handle, static_cast<int>(event), page, action);
    calling_mod = caller;
    if (result == 0)
      result = status;
    configured |= status == 0;
  }

  // A mod switched on or off in a callback takes effect once every mod has had
  // the event; the list is not changed while it is being walked.
  if (!previous && host.reselect)
    reselect_mods(&facade);

  current = previous;
  return event == Event::Configure && configured ? 0 : result;
}

namespace s8::scripting {
  bool take_mod_selection_change() {
    return std::exchange(host.changed, false);
  }
} // namespace s8::scripting
