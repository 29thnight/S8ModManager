// ID/session boundary for city AI queries. Called on demand, with no roster scan.
std::optional<std::int64_t> invoke_ai_service(Operation operation, std::uint64_t target,
                                            std::int64_t argument, void *data, std::uint32_t bytes) {
  if (operation != Operation::ReadCityAttackCandidates)
    return std::nullopt;

  struct Candidate { std::uint32_t city, estimated_defense; };
  struct Request {
    std::uint64_t session;
    std::uint32_t count, reserved;
    std::array<Candidate, 6> candidates;
  };
  static_assert(sizeof(Request) == 64);
  if (!data || bytes != sizeof(Request) || argument)
    return static_cast<int>(Status::InvalidApi);

  auto &request = *static_cast<Request *>(data);
  if (!request.session || request.session > INT64_MAX)
    return static_cast<int>(Status::InvalidApi);

  WorldEntityBinding binding{};
  const auto status = invoke_world_service(Operation::ResolveCity, target,
                                           static_cast<std::int64_t>(request.session), &binding, sizeof(binding));
  if (*status <= 0)
    return *status == 0 ? static_cast<int>(Status::InvalidBinding) : *status;

  request.count = 0;
  request.reserved = 0;
  request.candidates = {};
  auto *city = reinterpret_cast<void *>(binding.memory.address);
  auto *force = reinterpret_cast<void *(*)(void *)>(native_ui_module + 0x17121D0)(city);
  if (!force)
    return 0;

  // REA 17B6070: the six links starting at CityData+20 are adjacent cities.
  // Read only these links; eligibility and evaluation use the game routines.
  std::array<std::uintptr_t, 6> adjacent{};
  SIZE_T copied = 0;
  if (!ReadProcessMemory(GetCurrentProcess(), static_cast<std::byte *>(city) + 0x20,
                         adjacent.data(), sizeof(adjacent), &copied))
    return static_cast<int>(Status::InvalidBinding);

  using Eligible = bool (*)(void *, void *);
  using Evaluate = std::uint32_t (*)(void *, void *, void *, const void *);
  for (const auto address : adjacent) {
    if (!address)
      continue;

    struct Header {
      std::uintptr_t vtable;
      std::uint8_t id;
      std::byte reserved[7];
    } header{};
    if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void *>(address), &header, sizeof(header), &copied) ||
        header.vtable != native_ui_module + 0x2689638 || header.id < 1 || header.id > 51)
      return static_cast<int>(Status::InvalidBinding);

    auto *candidate = reinterpret_cast<void *>(address);
    if (!reinterpret_cast<bool (*)(void *)>(native_ui_module + 0x1704940)(candidate) ||
        !reinterpret_cast<Eligible>(native_ui_module + 0x1461CD0)(force, candidate))
      continue;

    // Expose the game's unadjusted evaluation. C# owns the selection policy.
    std::array<std::uint64_t, 8> modifiers{};
    auto *defender = reinterpret_cast<void *(*)(void *)>(native_ui_module + 0x17121D0)(candidate);
    const auto defense = reinterpret_cast<Evaluate>(native_ui_module + 0x17A4070)(
        nullptr, candidate, defender, modifiers.data());
    request.candidates[request.count++] = {header.id, defense};
  }
  return 0;
}
