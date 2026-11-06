#include "engine_services.h"
#include "mod_registry.h"
#include "script_api.h"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <Windows.h>
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

  std::int64_t invoke(Operation operation, std::uint64_t target, std::int64_t argument, void *data,
                      std::uint32_t bytes) {
    if (!current)
      return -1;

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
  using AotLoad = int (*)(const wchar_t *, const wchar_t *);
  using AotDispatch = int (*)(const EngineApi *, int, int, int);

  struct LoadedMod {
    ModManifest manifest;
    int handle = -1;
    AotDispatch aot = nullptr;
  };

  struct ModHost {
    bool loaded = false;
    std::filesystem::path base;
    std::vector<LoadedMod> mods;
    ManagedRegister managed_register = nullptr;
    ManagedDispatch managed_dispatch = nullptr;
  } host;

  void log(const std::wstring &message) {
    OutputDebugStringW((L"[S8 mods] " + message + L"\n").c_str());
  }

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

    hostfxr_handle context = nullptr;

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
           host.managed_register && host.managed_dispatch;
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

    HMODULE library =
        LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!library)
      return false;

    const auto load = reinterpret_cast<AotLoad>(GetProcAddress(library, "S8ModLoad"));
    mod.aot = reinterpret_cast<AotDispatch>(GetProcAddress(library, "S8ModDispatch"));

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

  void load_mods() {
    host.loaded = true;
    host.base = module_directory();

    auto selection = select_mods(scan_mods(host.base / L"scripts" / L"mods"));

    for (const auto &skipped : selection.skipped)
      log(skipped.manifest.directory.wstring() + L": " + skipped.reason);

    for (auto &manifest : selection.active) {
      LoadedMod mod{std::move(manifest)};
      const bool loaded = mod.manifest.runtime == ModRuntime::Aot ? load_aot(mod) : load_jit(mod);

      if (loaded)
        host.mods.push_back(std::move(mod));
      else
        log(mod.manifest.directory.wstring() + L": 모드를 불러오지 못했습니다.");
    }
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

  if (!host.loaded)
    load_mods();

  const EngineApi facade{api_version, sizeof(EngineApi), connection, invoke};
  const auto *previous = current;
  current = api;

  int result = 0;
  for (const auto &mod : host.mods) {
    const int status = mod.aot ? mod.aot(&facade, static_cast<int>(event), page, action)
                               : host.managed_dispatch(&facade, mod.handle, static_cast<int>(event), page, action);
    if (result == 0)
      result = status;
  }

  current = previous;
  return result;
}
