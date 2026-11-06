#include "native_ui.h"
#include "script_api.h"
#include "../game/trait_runtime_native.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <mutex>
#include <string>

// The trait catalog channel. The C# mod decides every trait value and sends the
// whole table; this file only queues it and drives the native trait runtime
// (hooks, trait table writes, save/scenario admission of custom IDs 201..254).
namespace s8::scripting {
  namespace {
    constexpr std::uint64_t retry_interval_ms = 500;
    constexpr std::uint64_t tick_interval_ms = 500;

    std::mutex pending_mutex;
    own::shared_owner<const domain::TraitRuntimeCatalog> pending_catalog;

    // Only the manager thread touches the runtime itself.
    game::TraitRuntimeNative runtime;
    own::shared_owner<const domain::TraitRuntimeCatalog> applying;
    std::uint64_t next_attempt_ms = 0;

    std::string narrow(const wchar_t *text) {
      if (!text || !*text)
        return {};
      const int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
      if (size <= 1)
        return {};
      std::string result(static_cast<std::size_t>(size - 1), '\0');
      WideCharToMultiByte(CP_UTF8, 0, text, -1, result.data(), size, nullptr, nullptr);
      return result;
    }

    bool retryable(game::TraitRuntimeStatus status) {
      return !runtime.table_uncertain && !runtime.description_uncertain &&
             (status == game::TraitRuntimeStatus::CallbackBusy || status == game::TraitRuntimeStatus::AllocationFailed ||
              status == game::TraitRuntimeStatus::MemoryFailed);
    }
  } // namespace

  // Called inside a mod callback; the record pointers are valid only during the call.
  std::int64_t set_trait_catalog(const void *data, std::uint32_t bytes) {
    if (!data || bytes != trait_catalog_count * sizeof(TraitCatalogRecord))
      return static_cast<std::int64_t>(Status::InvalidApi);

    auto catalog = own::make_shared<domain::TraitRuntimeCatalog>();
    const auto *records = static_cast<const TraitCatalogRecord *>(data);
    for (std::size_t index = 0; index < trait_catalog_count; ++index) {
      const auto &record = records[index];
      auto &entry = catalog->traits[index];
      entry.present = record.present != 0;
      entry.trait.id = static_cast<std::uint16_t>(index + 1);
      for (std::size_t slot = 0; slot < entry.trait.effects.size(); ++slot)
        entry.trait.effects[slot] = {record.effects[slot * 3], record.effects[slot * 3 + 1],
                                     record.effects[slot * 3 + 2]};

      auto &metadata = catalog->metadata[index];
      metadata.present = record.has_metadata != 0;
      metadata.name = narrow(record.name);
      metadata.description = narrow(record.description);
      metadata.clone_source = record.clone_source;
      metadata.background_trait = record.background_trait;
      metadata.grade = record.grade;
      metadata.voice = record.voice;
    }

    std::lock_guard lock(pending_mutex);
    pending_catalog = std::move(catalog);
    return 0;
  }

  void tick_trait_catalog(std::uintptr_t game_root, std::uint64_t epoch) {
    const auto now = GetTickCount64();
    {
      std::lock_guard lock(pending_mutex);
      if (pending_catalog) {
        applying = std::move(pending_catalog);
        pending_catalog = {};
        next_attempt_ms = 0;
      }
    }

    const auto module = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    if (applying && now >= next_attempt_ms) {
      if (game::set_trait_runtime_catalog(runtime, module, applying) || !retryable(runtime.status)) {
        if (runtime.status != game::TraitRuntimeStatus::WaitingForTable &&
            runtime.status != game::TraitRuntimeStatus::Disabled)
          OutputDebugStringW((L"[S8 mods] 기재 정의표를 적용하지 못했습니다: 상태 " +
                              std::to_wstring(static_cast<int>(runtime.status)) + L"\n")
                                 .c_str());
        applying = {};
      } else {
        next_attempt_ms = now + retry_interval_ms;
      }
      return;
    }

    if (!game_root || !runtime.catalog || !runtime.hooks_enabled || runtime.stopping || runtime.table_uncertain)
      return;
    if (runtime.game_root != game_root || runtime.epoch != epoch || runtime.last_tick_ms == 0 ||
        now - runtime.last_tick_ms >= tick_interval_ms)
      game::tick_trait_runtime_native(runtime, game_root, epoch, now);
  }
} // namespace s8::scripting
