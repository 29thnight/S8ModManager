struct WorldEntityBinding {
  MemoryRef memory;
  std::uint16_t id, personalities[3];
};
static_assert(sizeof(WorldEntityBinding) == 24);

// Included by native_ui_services.inl. World entity references are resolved
// inside the existing admitted game callback, against its current publication.
std::optional<std::int64_t> invoke_world_service(Operation operation, std::uint64_t target,
                                                std::int64_t argument, void *data, std::uint32_t bytes) {
  if (operation == Operation::GetWorldContext) {
    struct WorldContext {
      std::uint64_t epoch;
      std::uint16_t protagonist, reserved;
      std::uint32_t save_generation;
    };
    static_assert(sizeof(WorldContext) == 16);
    if (!data || bytes != sizeof(WorldContext) || target || argument)
      return static_cast<int>(Status::InvalidApi);

    std::lock_guard lock(native_ui_mutex);
    *static_cast<WorldContext *>(data) = {game_publication.epoch, game_publication.protagonist_id, 0,
                                        world_save_generation};
    return 0;
  }

  if (operation == Operation::AddFriendshipTokens) {
    struct TokenChange { std::int32_t delta; std::uint32_t show_message; };
    if (!data || bytes != sizeof(TokenChange) || argument <= 0)
      return static_cast<int>(Status::InvalidApi);
    const auto change = *static_cast<const TokenChange *>(data);
    if (change.show_message > 1)
      return static_cast<int>(Status::InvalidApi);
    {
      std::lock_guard lock(native_ui_mutex);
      if (static_cast<std::uint64_t>(argument) != game_publication.epoch)
        return static_cast<int>(Status::StaleSession);
      if (!game_publication.ready || game_publication.protagonist_id == 0xFFFF ||
          target == 0 || target != game_publication.protagonist_id)
        return static_cast<int>(Status::InvalidBinding);
    }

    // Global protagonist resource operation. The native routine checks its
    // scenario object and clamps tokens to 0..65535; zero delta is ignored.
    reinterpret_cast<void (*)(std::int32_t, std::uint8_t, std::int32_t)>(native_ui_module + 0x1727130)(
        change.delta, static_cast<std::uint8_t>(change.show_message), 0);
    return 0;
  }

  if (operation == Operation::ReadPlayerPrivilege) {
    std::lock_guard lock(native_ui_mutex);
    if (argument <= 0 || static_cast<std::uint64_t>(argument) != game_publication.epoch)
      return static_cast<int>(Status::StaleSession);
    if (!game_publication.ready || target != game_publication.protagonist_id)
      return static_cast<int>(Status::InvalidBinding);
    auto *root = native_field<void *>(reinterpret_cast<void *>(native_ui_module + 0x2E98BC8), 0);
    return native_field<std::uint8_t>(root, 0xEA);
  }

  const bool dialogue = operation == Operation::ShowOfficerDialogue;
  const bool city_name = operation == Operation::ReadCityName;
  if (operation != Operation::ResolveOfficer && operation != Operation::ResolveCity && !dialogue && !city_name)
    return std::nullopt;

  using EntityBinding = WorldEntityBinding;
  static_assert(sizeof(EntityBinding) == 24);
  if (!data || argument <= 0 || (!dialogue && !city_name && bytes != sizeof(EntityBinding)) ||
      (city_name && (bytes < 2 || bytes > 8192 || bytes % sizeof(wchar_t))))
    return static_cast<int>(Status::InvalidApi);

  if (dialogue && (bytes < sizeof(wchar_t) || bytes > 1024 || bytes % sizeof(wchar_t) ||
                   wcsnlen_s(static_cast<const wchar_t *>(data), bytes / sizeof(wchar_t)) + 1 !=
                       bytes / sizeof(wchar_t)))
    return static_cast<int>(Status::InvalidApi);

  const auto session = static_cast<std::uint64_t>(argument);
  {
    std::lock_guard lock(native_ui_mutex);
    if (session != game_publication.epoch)
      return static_cast<int>(Status::StaleSession);
    if (!game_publication.ready || game_publication.protagonist_id == 0xFFFF)
      return static_cast<int>(Status::InvalidBinding);
  }

  EntityBinding dialogue_binding{};
  auto &output = dialogue || city_name ? dialogue_binding : *static_cast<EntityBinding *>(data);
  output = {};
  const bool officer = operation != Operation::ResolveCity && !city_name;
  if (target < 1 || target > (officer ? 5200u : 51u))
    return 0;

  std::uintptr_t root = 0, record = 0;
  SIZE_T copied = 0;
  if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void *>(native_ui_module + 0x2E98BC8),
                         &root, sizeof(root), &copied) || !root)
    return static_cast<int>(Status::InvalidBinding);

  // These are the game's ID-indexed PersonData / CityData pointer tables.
  const auto slot = root + (officer ? 0x21BDF0 : 0x546808) + target * sizeof(void *);
  if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void *>(slot), &record, sizeof(record), &copied))
    return static_cast<int>(Status::InvalidBinding);
  if (!record)
    return 0;

  std::array<std::byte, 0xA0> image{};
  if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void *>(record), image.data(),
                         officer ? image.size() : 0x10, &copied))
    return static_cast<int>(Status::InvalidBinding);

  std::uintptr_t vtable = 0;
  std::uint16_t id = 0;
  std::memcpy(&vtable, image.data(), sizeof(vtable));
  std::memcpy(&id, image.data() + 8, officer ? 2 : 1);
  if (vtable != native_ui_module + (officer ? 0x2688548 : 0x2689638) || id != target)
    return 0;

  // PersonData::IsValid checks the defined record, not whether it is alive.
  const auto valid = reinterpret_cast<bool (*)(void *)>(
      native_ui_module + (officer ? 0x16F8A40 : 0x1704940));
  if (!valid(reinterpret_cast<void *>(record)))
    return 0;

  if (city_name) {
    const auto name = s8::domain::CITY_NAMES[target - 1];
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name.data(),
                                           static_cast<int>(name.size()), nullptr, 0);
    if (length == 0 || static_cast<std::size_t>(length) >= bytes / sizeof(wchar_t))
      return static_cast<int>(Status::InvalidApi);

    auto *output_name = static_cast<wchar_t *>(data);
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name.data(),
                        static_cast<int>(name.size()), output_name, length);
    output_name[length] = L'\0';
    return length;
  }

  if (officer && !dialogue) {
    for (unsigned index = 0; index < 3; ++index) {
      std::uintptr_t personality = 0;
      std::memcpy(&personality, image.data() + 0x88 + index * sizeof(void *), sizeof(personality));
      if (personality && !ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void *>(personality + 8),
                                           &output.personalities[index], sizeof(std::uint16_t), &copied))
        return static_cast<int>(Status::InvalidBinding);
    }
  }

  output.memory = {record, session};
  output.id = id;
  if (dialogue) {
    // AF0B0 resolves the speaker from AF530's second argument. Personality 0
    // takes its non-trait branch: no badge and no trait-configured voice.
    using ShowDialogue = void (*)(std::uint16_t, std::uint16_t, const wchar_t *,
                                  std::uint16_t, std::uintptr_t, std::uintptr_t, std::uintptr_t, bool);
    LARGE_INTEGER before{}, after{};
    QueryPerformanceCounter(&before);
    reinterpret_cast<ShowDialogue>(native_ui_module + 0xAF530)(
        0, id, static_cast<const wchar_t *>(data), 0, 0, 0, 0, false);
    QueryPerformanceCounter(&after);
    battle_presentation_ticks += after.QuadPart - before.QuadPart;
  }
  return 1;
}

// Officer eligibility and the five domestic execution requests share admission.
// City execution requirements are checked separately after officer eligibility.
std::optional<std::int64_t> invoke_officer_action_service(Operation operation, std::uint64_t target,
                                                        std::int64_t argument, void *data, std::uint32_t bytes) {
  const bool executing = operation == Operation::RequestDomesticActivity;
  if (operation != Operation::QueryOfficerActionAvailability && !executing)
    return std::nullopt;

  struct Result { std::int32_t status, reason; };
  struct Request {
    std::uint64_t session;
    Result result;
  };
  struct ExecutionRequest {
    Request availability;
    std::int32_t status, target_condition, target_reason;
    std::uint32_t reserved;
  };
  static_assert(sizeof(Result) == 8 && sizeof(Request) == 16 && sizeof(ExecutionRequest) == 32);

  if (!data || bytes != (executing ? sizeof(ExecutionRequest) : sizeof(Request)) || argument < 1 || argument > 100 ||
      (executing && argument != 1 && argument != 11 && argument != 21 && argument != 31 && argument != 41))
    return static_cast<int>(Status::InvalidApi);

  auto &request = executing ? static_cast<ExecutionRequest *>(data)->availability : *static_cast<Request *>(data);
  if (!request.session || request.session > INT64_MAX)
    return static_cast<int>(Status::InvalidApi);

  WorldEntityBinding binding{};
  const auto status = invoke_world_service(Operation::ResolveOfficer, target,
                                           static_cast<std::int64_t>(request.session), &binding, sizeof(binding));
  if (*status <= 0)
    return *status == 0 ? static_cast<int>(Status::InvalidBinding) : *status;

  std::uintptr_t root = 0, action = 0, role = 0;
  SIZE_T copied = 0;
  if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void *>(native_ui_module + 0x2E98BC8),
                         &root, sizeof(root), &copied) || !root ||
      !ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void *>(root + 0x5469E0 + argument * 8),
                         &action, sizeof(action), &copied) || !action ||
      !ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void *>(binding.memory.address + 0x10),
                         &role, sizeof(role), &copied) || !role)
    return static_cast<int>(Status::InvalidBinding);

  // The game check dereferences the role before its switch. Admit the live
  // role and action record here, before entering that native routine.
  const auto valid = [](std::uintptr_t record) {
    auto **object = reinterpret_cast<void ***>(record);
    return reinterpret_cast<bool (*)(void *)>((*object)[9])(object);
  };
  if (!valid(role) || !valid(action) || native_field<std::uint8_t>(reinterpret_cast<void *>(action), 8) != argument)
    return static_cast<int>(Status::InvalidBinding);

  // REA: RCX=result, RDX=action, R8=officer, R9 unused; stack arguments
  // are an explanation string slot and the suppress-explanation byte.
  // Suppression skips the global message formatting and string output paths.
  using Query = Result *(*)(Result *, void *, void *, std::uintptr_t, const wchar_t **, std::uint8_t);
  const wchar_t *explanation = nullptr;
  reinterpret_cast<Query>(native_ui_module + 0x17A8660)(
      &request.result, reinterpret_cast<void *>(action), reinterpret_cast<void *>(binding.memory.address),
      0, &explanation, 1);

  if (!executing)
    return 0;

  auto &execution = *static_cast<ExecutionRequest *>(data);
  execution.status = 2; // Officer action requirements rejected.
  execution.target_condition = 0;
  execution.target_reason = 0;
  execution.reserved = 0;
  if (request.result.status != 3)
    return 0;

  execution.status = 3; // Current city requirements rejected.
  auto *officer = reinterpret_cast<void *>(binding.memory.address);
  auto *city = reinterpret_cast<void *(*)(void *)>(native_ui_module + 0x1706E20)(officer);
  if (!city || !reinterpret_cast<bool (*)(void *)>(native_ui_module + 0x1704940)(city))
    return 0;

  // REA 1443FF0 -> 17A8930: two unused slots precede the explanation slot.
  // Preserve the city's independent result pair instead of translating it
  // into the officer availability enum. Five is the city's allowed status.
  using CheckCity = Result *(*)(Result *, void *, void *, std::uintptr_t, std::uintptr_t, const wchar_t **);
  Result city_result{};
  reinterpret_cast<CheckCity>(native_ui_module + 0x17A8930)(
      &city_result, city, reinterpret_cast<void *>(action), 0, 0, &explanation);
  execution.target_condition = city_result.status;
  execution.target_reason = city_result.reason;
  if (city_result.status != 5)
    return 0;

  // 17A8B10 accepts the game's empty participant list and two empty callback
  // objects. REA 1AD90 handles a null list token; callbacks use slot +38 only.
  struct Participants {
    std::uintptr_t vtable;
    void *token;
  } participants{native_ui_module + 0x2578A30, nullptr};
  struct EmptyCallback { std::uint64_t slots[8]{}; } before, after;
  static_assert(sizeof(Participants) == 16 && sizeof(EmptyCallback) == 64);

  using Execute = void (*)(void *, std::uint8_t, void *, Participants *, void *, EmptyCallback *, EmptyCallback *);
  reinterpret_cast<Execute>(native_ui_module + 0x17A8B10)(
      city, static_cast<std::uint8_t>(argument), officer, &participants, nullptr, &before, &after);
  execution.status = 1; // Invoked; this is not a promise of a particular stat delta.
  return 0;
}

#include "entity_data_services.inl"
#include "political_services.inl"
#include "ai_services.inl"
#include "march_services.inl"
