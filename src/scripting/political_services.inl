// Read a single officer/city and its immediate ownership links. No roster scan.
std::optional<std::int64_t> invoke_political_service(Operation operation, std::uint64_t target,
                                                    std::int64_t argument, void *data, std::uint32_t bytes) {
  if (operation != Operation::ReadOfficerAffiliation && operation != Operation::ReadCityAffiliation)
    return std::nullopt;

  struct Snapshot {
    std::uint16_t entity, lord, governor;
    std::uint8_t rank, reserved;
    std::uint8_t role, force, city, flags;
    std::uint32_t padding;
    std::uint64_t district_number;
  };
  static_assert(sizeof(Snapshot) == 24);
  if (!data || bytes != sizeof(Snapshot) || argument <= 0)
    return static_cast<int>(Status::InvalidApi);

  const bool officer = operation == Operation::ReadOfficerAffiliation;
  WorldEntityBinding binding{};
  const auto status = invoke_world_service(officer ? Operation::ResolveOfficer : Operation::ResolveCity,
                                           target, argument, &binding, sizeof(binding));
  if (*status <= 0)
    return *status == 0 ? static_cast<int>(Status::InvalidBinding) : *status;

  Snapshot output{};
  output.entity = binding.id;
  auto *record = reinterpret_cast<void *>(binding.memory.address);
  auto pointer = [](void *object, std::uintptr_t rva) {
    return reinterpret_cast<void *(*)(void *)>(native_ui_module + rva)(object);
  };

  // Linked engine objects enter this service here. Null means no relation;
  // an unreadable/wrong-type object is an error, not an empty relationship.
  auto identity = [](void *object, std::uint32_t vtable_rva, unsigned maximum,
                     unsigned width) -> std::optional<std::uint16_t> {
    if (!object)
      return 0;
    std::array<std::byte, 16> header{};
    SIZE_T copied = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), object, header.data(), header.size(), &copied))
      return std::nullopt;
    std::uintptr_t vtable = 0;
    std::uint16_t id = 0;
    std::memcpy(&vtable, header.data(), sizeof(vtable));
    std::memcpy(&id, header.data() + 8, width);
    if (vtable != native_ui_module + vtable_rva || id < 1 || id > maximum)
      return std::nullopt;
    return id;
  };

  void *force = nullptr;
  void *city = officer ? pointer(record, 0x1706E20) : record;
  if (officer) {
    // +10 is a role DATA POINTER, not the low address byte used by the old UI.
    output.role = reinterpret_cast<std::uint8_t (*)(void *)>(native_ui_module + 0x17126C0)(record);
    output.rank = reinterpret_cast<std::uint8_t (*)(void *)>(native_ui_module + 0x1711890)(record);
    if (output.rank)
      output.flags |= 32;
    force = pointer(record, 0x1707410);
  }

  const auto city_id = officer ? identity(city, 0x2689638, 51, 1)
                               : std::optional<std::uint16_t>(binding.id);
  if (!city_id)
    return static_cast<int>(Status::InvalidBinding);
  if (*city_id) {
    output.city = static_cast<std::uint8_t>(*city_id);
    output.flags |= 2;

    if (!officer) {
      auto *governor = pointer(city, 0x1713F00);
      const auto governor_id = identity(governor, 0x2688548, 5200, 2);
      if (!governor_id)
        return static_cast<int>(Status::InvalidBinding);
      if (*governor_id) {
        output.governor = *governor_id;
        output.flags |= 8;
      }

      // CityData +90 links its district. ArchiveRuntimeFields serializes that
      // reference; district+10 links ForceData and +18 is its observed number.
      auto *district = pointer(city, 0x170EA50);
      if (district) {
        SIZE_T copied = 0;
        std::array<std::uintptr_t, 2> links{};
        if (!ReadProcessMemory(GetCurrentProcess(), static_cast<std::byte *>(district) + 0x10,
                               links.data(), sizeof(links), &copied))
          return static_cast<int>(Status::InvalidBinding);
        force = reinterpret_cast<void *>(links[0]);
        output.district_number = links[1];
        output.flags |= 16;
      }
      else if (governor)
        force = pointer(governor, 0x1707410);
    }
  }

  const auto force_id = identity(force, 0x2688C98, 120, 1);
  if (!force_id)
    return static_cast<int>(Status::InvalidBinding);
  if (*force_id && reinterpret_cast<bool (*)(void *)>(native_ui_module + 0x16FEBB0)(force)) {
    output.force = static_cast<std::uint8_t>(*force_id);
    output.flags |= 1;
    const auto lord_id = identity(pointer(force, 0x1713CF0), 0x2688548, 5200, 2);
    if (!lord_id)
      return static_cast<int>(Status::InvalidBinding);
    if (*lord_id) {
      output.lord = *lord_id;
      output.flags |= 4;
    }
  }

  *static_cast<Snapshot *>(data) = output;
  return 0;
}
