#include "native_ui.h"
#include "script_api.h"
#include "../platform/windows/hook.h"
#include "../../third_party/minhook/include/MinHook.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <array>
#include <atomic>
#include <cwchar>
#include <fstream>

// The mod manager enters the game as xinput1_4.dll, independently of the cheat
// proxies (hid/dxgi/dinput8). The game imports XInput by ordinal (2 and 3), so
// every forwarder keeps the system DLL's ordinal.
#pragma comment(linker, "/export:XInputGetState=C:\\Windows\\System32\\xinput1_4.XInputGetState,@2")
#pragma comment(linker, "/export:XInputSetState=C:\\Windows\\System32\\xinput1_4.XInputSetState,@3")
#pragma comment(linker, "/export:XInputGetCapabilities=C:\\Windows\\System32\\xinput1_4.XInputGetCapabilities,@4")
#pragma comment(linker, "/export:XInputEnable=C:\\Windows\\System32\\xinput1_4.XInputEnable,@5")
#pragma comment(linker, "/export:XInputGetAudioDeviceIds=C:\\Windows\\System32\\xinput1_4.XInputGetAudioDeviceIds,@6")
#pragma comment(linker, "/export:XInputGetBatteryInformation=C:\\Windows\\System32\\xinput1_4.XInputGetBatteryInformation,@7")
#pragma comment(linker, "/export:XInputGetKeystroke=C:\\Windows\\System32\\xinput1_4.XInputGetKeystroke,@8")
#pragma comment(linker, "/export:XInputGetStateEx=C:\\Windows\\System32\\xinput1_4.#100,@100,NONAME")
#pragma comment(linker, "/export:XInputWaitForGuideButton=C:\\Windows\\System32\\xinput1_4.#101,@101,NONAME")
#pragma comment(linker, "/export:XInputCancelGuideButtonWait=C:\\Windows\\System32\\xinput1_4.#102,@102,NONAME")
#pragma comment(linker, "/export:XInputPowerOffController=C:\\Windows\\System32\\xinput1_4.#103,@103,NONAME")
#pragma comment(linker, "/export:XInputGetBaseBusInformation=C:\\Windows\\System32\\xinput1_4.#104,@104,NONAME")
#pragma comment(linker, "/export:XInputGetCapabilitiesEx=C:\\Windows\\System32\\xinput1_4.#108,@108,NONAME")

namespace s8::scripting {
  const Connection &game_connection();
}

namespace {
  using namespace s8::scripting;

  constexpr DWORD poll_interval_ms = 100;
  constexpr DWORD unpack_timeout_ms = 120000;
  // Decryption is complete once these prologues (also checked by game_connection)
  // read back exactly. No component hooks them.
  struct Probe {
    std::uintptr_t rva;
    std::array<std::uint8_t, 16> bytes;
  };
  constexpr Probe probes[]{
      {0x145A6B0, {0x48, 0x8b, 0xc4, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8d}},
      {0x1461BA0, {0x44, 0x88, 0x4c, 0x24, 0x20, 0x57, 0x48, 0x83, 0xec, 0x50, 0x48, 0xc7, 0x44, 0x24, 0x30, 0xfe}},
      {0x194A050, {0x40, 0x55, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8d, 0xac, 0x24, 0x70, 0x5c}}};

  std::atomic<bool> stopping{false};

  // The first hook failure, written to scripts\mod_manager.log for diagnosis.
  struct HookFailure {
    const char *step = nullptr;
    std::uintptr_t rva = 0;
    int status = 0;
  } hook_failure;

  void record_failure(const char *step, void *target, int status) {
    if (hook_failure.step)
      return;
    const auto module = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    hook_failure = {step, reinterpret_cast<std::uintptr_t>(target) - module, status};
  }

  void write_log(const char *message) {
    wchar_t path[MAX_PATH]{};
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&write_log), &self);
    GetModuleFileNameW(self, path, MAX_PATH);
    if (wchar_t *slash = wcsrchr(path, L'\\'))
      wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - path), L"scripts\\mod_manager.log");
    std::ofstream(path, std::ios::app) << message << '\n';
  }

  // Rejections are shown only inside the game; other hosts (tests) stay silent.
  void report(Status status) {
    wchar_t image[MAX_PATH]{};
    GetModuleFileNameW(nullptr, image, MAX_PATH);
    const wchar_t *name = wcsrchr(image, L'\\');
    if (_wcsicmp(name ? name + 1 : image, L"SAN8RPK.exe") == 0)
      MessageBoxW(nullptr, status_message(status), L"모드 매니저 연결 거부", MB_OK | MB_ICONERROR);
    OutputDebugStringW(status_message(status));
  }

  bool code_ready(std::uintptr_t module) {
    for (const auto &probe : probes) {
      std::array<std::uint8_t, 16> current{};
      SIZE_T copied = 0;
      if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void *>(module + probe.rva), current.data(),
                             current.size(), &copied) ||
          current != probe.bytes)
        return false;
    }
    return true;
  }

  bool attach() {
    const auto initialized = s8::platform::windows::initialize_hooks();
    if (initialized != MH_OK && initialized != MH_ERROR_ALREADY_INITIALIZED)
      return false;

    UiHookApi hooks{};
    hooks.version = ui_host_version;
    hooks.size = sizeof(hooks);
    hooks.create = [](void *target, void *detour, void **original) -> int {
      const auto hook = s8::platform::windows::create_hook(target, detour);
      if (!hook) {
        record_failure("create", target, hook.error());
        return hook.error();
      }
      *original = hook->original;
      return 0;
    };
    hooks.enable = [](void *target) {
      const int status = s8::platform::windows::enable_hook({target, nullptr});
      if (status != 0)
        record_failure("enable", target, status);
      return status;
    };
    hooks.disable = [](void *target) { return s8::platform::windows::disable_hook({target, nullptr}); };
    return attach_native_ui(hooks);
  }

  DWORD WINAPI run_manager(void *) {
    const auto module = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    const auto started = GetTickCount64();
    while (!code_ready(module)) {
      if (stopping.load() || GetTickCount64() - started > unpack_timeout_ms) {
        OutputDebugStringW(L"[S8 mods] 게임 코드가 준비되지 않아 모드 매니저를 시작하지 않았습니다.\n");
        return 0;
      }
      Sleep(poll_interval_ms);
    }

    // game_connection() compares in-memory code and caches the result for the
    // process, so it must run only after the executable has been decrypted.
    const auto status = game_connection().status;
    if (status != Status::Ready) {
      report(status);
      return 0;
    }

    if (!attach()) {
      char line[160]{};
      sprintf_s(line, "hook %s failed: rva=0x%llX status=%d", hook_failure.step ? hook_failure.step : "init",
                static_cast<unsigned long long>(hook_failure.rva), hook_failure.status);
      write_log(line);
      report(Status::CodeMismatch);
      return 0;
    }

    while (!stopping.load()) {
      refresh_game_publication();
      Sleep(poll_interval_ms);
    }
    return 0;
  }
} // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    DisableThreadLibraryCalls(module);
    if (const HANDLE thread = CreateThread(nullptr, 0, &run_manager, nullptr, 0, nullptr))
      CloseHandle(thread);
  } else if (reason == DLL_PROCESS_DETACH) {
    stopping.store(true);
  }
  return TRUE;
}
