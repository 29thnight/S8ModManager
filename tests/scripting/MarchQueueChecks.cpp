// Runs the production queue against in-process game-call stand-ins.
// No game process, injection, or production validation bypass is involved.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

enum class Operation { RequestCityMarch, ReadCityMarchResult, ResolveCity };
enum class Status { InvalidApi = -1, InvalidBinding = -8, StaleSession = -10 };

struct MemoryRef { std::uintptr_t address; std::uint64_t session; };
struct WorldEntityBinding { MemoryRef memory; };

std::uintptr_t native_ui_module;
std::mutex native_ui_mutex;
struct { std::uint64_t epoch = 1; bool ready = true, pending = false; std::uint16_t protagonist_id = 1; }
    game_publication;
std::vector<int> game_commands;
std::uintptr_t battle_session_root;

template<class T> T native_field(void *object, std::size_t offset) {
  T value;
  std::memcpy(&value, static_cast<std::byte *>(object) + offset, sizeof(value));
  return value;
}

struct Force { std::uint64_t vtable; std::uint8_t id; };
struct City {
  std::byte header[0x20]{};
  std::array<std::uintptr_t, 6> adjacent{};
  Force *force = nullptr;
  bool active = true, eligible = true;
};
std::array<City, 5> cities;
Force attacker{0, 1}, defender_force{0, 2}, replacement{0, 3};

std::optional<std::int64_t> invoke_world_service(Operation, std::uint64_t id, std::int64_t epoch,
                                               void *data, std::uint32_t) {
  if (static_cast<std::uint64_t>(epoch) != game_publication.epoch)
    return static_cast<int>(Status::StaleSession);
  if (id == 0 || id >= cities.size())
    return 0;
  *static_cast<WorldEntityBinding *>(data) = {{reinterpret_cast<std::uintptr_t>(&cities[id]),
                                             game_publication.epoch}};
  return 1;
}

#include "../../src/scripting/march_services.inl"

int checks = 0, calls = 0, mode = 0;
bool failed = false;

void check(bool result, const char *message) {
  ++checks;
  if (!result) {
    failed = true;
    std::cerr << "FAIL: " << message << '\n';
  }
}

std::uint64_t submit(unsigned source, unsigned target) {
  struct { std::uint64_t epoch, ticket; } request{game_publication.epoch, 0};
  const auto result = invoke_march_service(Operation::RequestCityMarch, source, target,
                                         &request, sizeof(request));
  check(result == 1, "Request admitted");
  return request.ticket;
}

void *get_force(void *city) { return static_cast<City *>(city)->force; }
bool active(void *city) { return static_cast<City *>(city)->active; }
bool eligible(void *, void *city) { return static_cast<City *>(city)->eligible; }

std::uint64_t march(std::uintptr_t, void *, void *, std::uintptr_t) {
  ++calls;
  if (calls == 1) {
    if (mode == 1) {
      submit(3, 4);
      execute_pending_march(); // Reentrant dispatch must leave both requests alone.
    }
    else if (mode == 2) {
      ++game_publication.epoch;
      march_requests.clear(); // Same retirement performed by S8UiPublish.
      submit(3, 4);
    }
    else if (mode == 3) {
      game_commands.push_back(1);
    }
    else if (mode == 4) {
      battle_session_root = 1;
    }
    else if (mode == 5) {
      game_publication.pending = true;
    }
  }
  return mode == 6 ? 0 : 1;
}

void reset(int next_mode = 0) {
  ++game_publication.epoch;
  game_publication.pending = false;
  game_publication.ready = true;
  march_requests.clear();
  game_commands.clear();
  battle_session_root = 0;
  calls = 0;
  mode = next_mode;
  for (auto &city : cities)
    city = City{};
  cities[1].force = cities[3].force = &attacker;
  cities[2].force = cities[4].force = &defender_force;
  cities[1].adjacent[0] = reinterpret_cast<std::uintptr_t>(&cities[2]);
  cities[3].adjacent[0] = reinterpret_cast<std::uintptr_t>(&cities[4]);
}

int main() {
  // Match the already bound game RVAs using tiny trampolines in test-owned memory.
  native_ui_module = reinterpret_cast<std::uintptr_t>(VirtualAlloc(nullptr, 0x1800000,
      MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
  if (!native_ui_module)
    return 2;
  for (auto [rva, function] : {std::pair{0x17121D0, reinterpret_cast<std::uintptr_t>(&get_force)},
                              std::pair{0x1704940, reinterpret_cast<std::uintptr_t>(&active)},
                              std::pair{0x1461CD0, reinterpret_cast<std::uintptr_t>(&eligible)},
                              std::pair{0x145A6B0, reinterpret_cast<std::uintptr_t>(&march)}}) {
    std::array<unsigned char, 12> jump{0x48, 0xB8};
    std::memcpy(jump.data() + 2, &function, sizeof(function));
    jump[10] = 0xFF;
    jump[11] = 0xE0;
    std::memcpy(reinterpret_cast<void *>(native_ui_module + rva), jump.data(), jump.size());
  }
  FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void *>(native_ui_module), 0x1800000);

  reset();
  submit(1, 2);
  submit(3, 4);
  execute_pending_march();
  check(calls == 2 && std::ranges::all_of(march_requests, [](const auto &r) {
    return r.status == MarchStatus::Invoked;
  }), "Existing batch drained in one boundary");

  reset(1);
  submit(1, 2);
  execute_pending_march();
  check(calls == 1 && march_requests[1].status == MarchStatus::Queued,
        "Nested submission deferred and reentry suppressed");
  execute_pending_march();
  check(calls == 2, "Deferred request executes at next boundary");

  reset(2);
  submit(1, 2);
  submit(3, 4);
  execute_pending_march();
  check(calls == 1 && march_requests.size() == 1 && march_requests[0].status == MarchStatus::Queued,
        "New session retires old receipt and stops batch");
  execute_pending_march();
  check(calls == 2, "New session request executes at next boundary");

  for (int interruption : {3, 4, 5}) {
    reset(interruption);
    submit(1, 2);
    submit(3, 4);
    execute_pending_march();
    check(calls == 1 && march_requests[1].status == MarchStatus::Queued,
          "Command, battle or publication transition stops batch");
    game_commands.clear();
    battle_session_root = 0;
    game_publication.pending = false;
    execute_pending_march();
    check(calls == 2, "Interrupted batch resumes at next boundary");
  }

  for (bool source_changed : {false, true}) {
    reset();
    submit(1, 2);
    cities[source_changed ? 1 : 2].force = &replacement;
    execute_pending_march();
    check(calls == 0 && march_requests[0].status == MarchStatus::OwnershipChanged,
          "Either ownership change cancels before execution");
  }

  reset();
  submit(1, 2);
  cities[2].eligible = false;
  execute_pending_march();
  check(calls == 0 && march_requests[0].status == MarchStatus::TargetNoLongerEligible,
        "Current target check can cancel queued request");

  reset(6);
  submit(1, 2);
  submit(3, 4);
  execute_pending_march();
  check(calls == 2 && march_requests[0].status == MarchStatus::RejectedByGame &&
        march_requests[1].status == MarchStatus::RejectedByGame,
        "Native rejection does not stall the batch");

  VirtualFree(reinterpret_cast<void *>(native_ui_module), 0, MEM_RELEASE);
  std::cout << "March queue checks: " << checks << (failed ? " FAIL\n" : " PASS\n");
  return failed ? 1 : 0;
}
