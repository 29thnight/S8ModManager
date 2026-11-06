// Included after world entity resolution. Requests use IDs and a session,
// never a caller-supplied object pointer or function address.
std::optional<std::int64_t> invoke_entity_data_service(Operation operation, std::uint64_t target,
                                                       std::int64_t argument, void *data, std::uint32_t bytes) {
  if (operation != Operation::ReadEntityValue && operation != Operation::SetEntityValue &&
      operation != Operation::AddEntityValue)
    return std::nullopt;

  struct Request {
    std::uint64_t session;
    std::uint32_t kind, index;
    std::int64_t value;
  };
  static_assert(sizeof(Request) == 24);
  if (!data || bytes != sizeof(Request) || argument)
    return static_cast<int>(Status::InvalidApi);

  const auto request = *static_cast<const Request *>(data);
  const bool reading = operation == Operation::ReadEntityValue;
  const bool adding = operation == Operation::AddEntityValue;
  if (!request.session || request.session > INT64_MAX || request.kind > 7 ||
      (reading && request.value != 0) ||
      (adding && (request.value < INT32_MIN || request.value > INT32_MAX)) ||
      (!reading && !adding && (request.value < 0 || request.value > UINT32_MAX)))
    return static_cast<int>(Status::InvalidApi);

  const auto index = request.index;
  std::uint32_t get = 0, set = 0, add = 0;
  unsigned maximum = UINT32_MAX;
  struct CityAccessor { std::uint32_t get, set, add; unsigned maximum; };
  constexpr CityAccessor city[]{
      {0x17122D0, 0x1712700, 0x1710F00, UINT32_MAX}, // Gold
      {0x17122E0, 0x1712720, 0x1710F60, UINT32_MAX}, // Grain
      {0x17122F0, 0x1712740, 0x1710FC0, UINT32_MAX}, // Troops
      {0x1712220, 0x1712270, 0, UINT16_MAX},        // Development
      {0x1712240, 0x1712290, 0, UINT16_MAX},        // Commerce
      {0x1712260, 0x17122B0, 0, UINT16_MAX},        // Defense
      {0x1712320, 0x17128B0, 0, UINT16_MAX},        // Technology
      {0x1712210, 0x17127A0, 0, UINT16_MAX},        // Development limit
      {0x1712230, 0x17127E0, 0, UINT16_MAX},        // Commerce limit
      {0x1712250, 0x1712820, 0, UINT16_MAX},        // Defense limit
      {0x1712310, 0, 0, UINT16_MAX},               // Technology limit: read only
      {0x170CF90, 0x1703540, 0, UINT32_MAX},       // Troop limit
      {0x170FBF0, 0x1703590, 0, UINT8_MAX}};       // Revolt

  switch (request.kind) {
  case 0: // Base stat IDs are one-based.
    if (index < 1 || index > 5)
      return static_cast<int>(Status::InvalidApi);
    maximum = UINT8_MAX;
    break;
  case 1:
    get = 0x17117B0;
    set = 0x1712620;
    add = 0x17103A0;
    break;
  case 2:
    get = 0x1711880;
    set = 0x1711840;
    maximum = UINT8_MAX;
    break;
  case 3: {
    constexpr std::uint32_t getters[]{0x1711BA0, 0x1711B90, 0x1711BB0};
    constexpr std::uint32_t setters[]{0x1711C00, 0x1711BC0, 0x1711C40};
    if (index >= 3)
      return static_cast<int>(Status::InvalidApi);
    get = getters[index];
    set = setters[index];
    maximum = UINT16_MAX;
    break;
  }
  case 4:
  case 5:
    if (index < 1 || index > (request.kind == 4 ? 35u : 24u))
      return static_cast<int>(Status::InvalidApi);
    maximum = 3;
    break;
  case 6:
    if (index >= 3)
      return static_cast<int>(Status::InvalidApi);
    maximum = 254;
    break;
  case 7:
    if (index >= std::size(city))
      return static_cast<int>(Status::InvalidApi);
    get = city[index].get;
    set = city[index].set;
    add = city[index].add;
    maximum = city[index].maximum;
    break;
  }

  if ((adding && !add) || (!reading && !adding && static_cast<std::uint64_t>(request.value) > maximum) ||
      (!reading && !adding && request.kind == 7 && !set) ||
      ((request.kind == 1 || request.kind == 2) && index != 0))
    return static_cast<int>(Status::InvalidApi);

  WorldEntityBinding binding{};
  const auto status = invoke_world_service(request.kind == 7 ? Operation::ResolveCity : Operation::ResolveOfficer,
                                           target, static_cast<std::int64_t>(request.session), &binding, sizeof(binding));
  if (*status <= 0)
    return *status == 0 ? static_cast<int>(Status::InvalidBinding) : *status;
  auto *record = reinterpret_cast<void *>(binding.memory.address);
  const auto value = static_cast<unsigned>(request.value);

  if (request.kind == 0 || request.kind == 4 || request.kind == 5) {
    const auto getter = native_ui_module + (request.kind == 0 ? 0x17118D0 : request.kind == 4 ? 0x1713280 : 0x1713150);
    const auto setter = native_ui_module + (request.kind == 0 ? 0x17118F0 : request.kind == 4 ? 0x17133C0 : 0x17131E0);
    if (!reading)
      reinterpret_cast<void (*)(void *, std::uint8_t, std::uint8_t)>(setter)(
          record, static_cast<std::uint8_t>(value), static_cast<std::uint8_t>(index));
    return reinterpret_cast<std::uint8_t (*)(void *, std::uint8_t)>(getter)(record, static_cast<std::uint8_t>(index));
  }

  if (request.kind == 6) {
    if (!reading) {
      Personality personality{};
      if (value && invoke_engine(Operation::GetPersonality, value, 0, &personality, sizeof(personality),
                                 request.session) != 1)
        return static_cast<int>(Status::InvalidBinding);
      reinterpret_cast<void (*)(void *, void *, unsigned)>(native_ui_module + 0x1711940)(
          record, reinterpret_cast<void *>(personality.memory.address), index);
    }
    auto *personality = reinterpret_cast<void *(*)(void *, unsigned)>(native_ui_module + 0x1711920)(record, index);
    std::uint16_t id = 0;
    SIZE_T copied = 0;
    if (personality && !ReadProcessMemory(GetCurrentProcess(), static_cast<std::byte *>(personality) + 8,
                                        &id, sizeof(id), &copied))
      return static_cast<int>(Status::InvalidBinding);
    return id;
  }

  if (adding)
    reinterpret_cast<void (*)(void *, std::int32_t)>(native_ui_module + add)(record, static_cast<std::int32_t>(request.value));
  else if (!reading)
    reinterpret_cast<void (*)(void *, unsigned)>(native_ui_module + set)(record, value);
  return reinterpret_cast<unsigned (*)(void *)>(native_ui_module + get)(record);
}
