// Requests contain IDs only. AI requests run after managed activity callbacks;
// explicit privilege requests can also run on a city UI update after dialogs close.
enum class MarchStatus : std::uint32_t {
  Queued = 1, Executing, Invoked, RejectedByGame, DestinationNotAdjacent, NoFaction, InvalidEntity,
  OwnershipChanged, TargetNoLongerEligible, NoPrivilege, ApprovalAuthorityChanged
};

struct MarchRecord {
  std::uint64_t session, ticket;
  std::uint16_t source, destination;
  MarchStatus status;
  std::uint32_t game_return = 0;
  std::uint8_t source_force = 0, destination_force = 0;
  std::uint16_t proposer = 0, governor = 0, lord = 0;
};

std::vector<MarchRecord> march_requests;
std::uint64_t next_march_ticket = 0;
std::atomic_flag march_executing = ATOMIC_FLAG_INIT;
std::atomic<bool> privileged_march_pending{false};

// The native records are already resolved at the service entrance. This checks
// current ownership/approval links, which can change while a request is queued.
MarchStatus validate_privilege_march(void *city, const MarchRecord &request) {
  auto *root = native_field<void *>(reinterpret_cast<void *>(native_ui_module + 0x2E98BC8), 0);
  auto *player = native_field<void *>(root, 0xE0);
  if (!player || native_field<std::uint16_t>(player, 8) != request.proposer ||
      reinterpret_cast<void *(*)(void *)>(native_ui_module + 0x1706E20)(player) != city)
    return MarchStatus::ApprovalAuthorityChanged;

  auto *force = reinterpret_cast<void *(*)(void *)>(native_ui_module + 0x17121D0)(city);
  auto *player_force = reinterpret_cast<void *(*)(void *)>(native_ui_module + 0x1707410)(player);
  if (!force || force != player_force)
    return MarchStatus::ApprovalAuthorityChanged;

  auto *governor = reinterpret_cast<void *(*)(void *)>(native_ui_module + 0x1713F00)(city);
  auto *lord = reinterpret_cast<void *(*)(void *)>(native_ui_module + 0x1713CF0)(force);
  if ((governor ? native_field<std::uint16_t>(governor, 8) : 0) != request.governor ||
      (lord ? native_field<std::uint16_t>(lord, 8) : 0) != request.lord)
    return MarchStatus::ApprovalAuthorityChanged;
  return native_field<std::uint8_t>(root, 0xEA) ? MarchStatus::Queued : MarchStatus::NoPrivilege;
}

std::optional<std::int64_t> invoke_march_service(Operation operation, std::uint64_t target,
                                               std::int64_t argument, void *data, std::uint32_t bytes) {
  const bool privileged = operation == Operation::RequestPrivilegedCityMarch;
  const bool submitting = operation == Operation::RequestCityMarch || privileged;
  if (!submitting && operation != Operation::ReadCityMarchResult)
    return std::nullopt;

  struct Submission { std::uint64_t session, ticket; };
  struct PrivilegedSubmission {
    Submission request;
    std::uint16_t proposer, approver, governor, lord;
  };
  struct Receipt {
    MarchStatus status;
    std::uint32_t game_return;
    std::uint16_t source, destination;
    std::uint32_t reserved;
  };
  static_assert(sizeof(Submission) == 16 && sizeof(Receipt) == 16);
  if (!data || bytes != (privileged ? sizeof(PrivilegedSubmission) :
                        submitting ? sizeof(Submission) : sizeof(Receipt)))
    return static_cast<int>(Status::InvalidApi);

  if (submitting) {
    auto &request = *static_cast<Submission *>(data);
    if (!request.session || request.session > INT64_MAX || target < 1 || target > 51 || argument < 1 || argument > 51)
      return static_cast<int>(Status::InvalidApi);
    WorldEntityBinding source{}, destination{};
    for (auto [id, binding] : {std::pair{target, &source},
                               std::pair{static_cast<std::uint64_t>(argument), &destination}}) {
      const auto status = *invoke_world_service(Operation::ResolveCity, id,
          static_cast<std::int64_t>(request.session), binding, sizeof(WorldEntityBinding));
      if (status != 1)
        return status == 0 ? static_cast<int>(Status::InvalidBinding) : status;
    }

    using GetForce = void *(*)(void *);
    const auto get_force = reinterpret_cast<GetForce>(native_ui_module + 0x17121D0);
    auto *source_force = get_force(reinterpret_cast<void *>(source.memory.address));
    auto *destination_force = get_force(reinterpret_cast<void *>(destination.memory.address));

    MarchRecord queued{request.session, 0, static_cast<std::uint16_t>(target),
                       static_cast<std::uint16_t>(argument), MarchStatus::Queued, 0,
                       source_force ? native_field<std::uint8_t>(source_force, 8) : std::uint8_t{0},
                       destination_force ? native_field<std::uint8_t>(destination_force, 8) : std::uint8_t{0}};
    if (privileged) {
      const auto &approval = *static_cast<PrivilegedSubmission *>(data);
      if (!approval.proposer || !approval.approver || approval.approver == approval.proposer ||
          (approval.approver != approval.governor && approval.approver != approval.lord))
        return static_cast<int>(Status::InvalidApi);
      queued.proposer = approval.proposer;
      queued.governor = approval.governor;
      queued.lord = approval.lord;
      if (validate_privilege_march(reinterpret_cast<void *>(source.memory.address), queued) != MarchStatus::Queued)
        return static_cast<int>(Status::InvalidBinding);
    }

    std::lock_guard lock(native_ui_mutex);
    if (request.session != game_publication.epoch)
      return static_cast<int>(Status::StaleSession);
    if (!game_publication.ready || game_publication.protagonist_id == 0xFFFF)
      return static_cast<int>(Status::InvalidBinding);
    if (march_requests.size() == 64)
      return 0;

    request.ticket = ++next_march_ticket;
    queued.ticket = request.ticket;
    march_requests.push_back(queued);
    if (privileged)
      privileged_march_pending.store(true, std::memory_order_release);
    return 1;
  }

  if (!target || argument <= 0)
    return static_cast<int>(Status::InvalidApi);
  std::lock_guard lock(native_ui_mutex);
  if (static_cast<std::uint64_t>(argument) != game_publication.epoch)
    return static_cast<int>(Status::StaleSession);

  const auto found = std::find_if(march_requests.begin(), march_requests.end(),
                                 [target](const auto &request) { return request.ticket == target; });
  if (found == march_requests.end())
    return static_cast<int>(Status::InvalidBinding);

  *static_cast<Receipt *>(data) = {found->status, found->game_return, found->source, found->destination, 0};
  if (found->status != MarchStatus::Queued && found->status != MarchStatus::Executing)
    march_requests.erase(found);
  return 0;
}

void execute_pending_march(bool privileged_only) {
  if (march_executing.test_and_set(std::memory_order_acquire))
    return;

  struct ReleaseExecution {
    ~ReleaseExecution() { march_executing.clear(std::memory_order_release); }
  } release;

  std::uint64_t batch_session, last_ticket;
  std::size_t batch_size;
  {
    std::lock_guard lock(native_ui_mutex);
    batch_session = game_publication.epoch;
    last_ticket = next_march_ticket;
    batch_size = march_requests.size();
  }

  // Drain only the bounded batch present on entry. Nested game callbacks may
  // submit new requests, but those belong to the next measured boundary.
  for (std::size_t processed = 0; processed < batch_size; ++processed) {
    MarchRecord request{};
    {
      std::lock_guard lock(native_ui_mutex);
      if (game_publication.epoch != batch_session || !game_publication.ready || game_publication.pending ||
          battle_session_root)
        return;

      const auto found = std::find_if(march_requests.begin(), march_requests.end(),
                                     [=](const auto &value) {
                                       return value.session == batch_session && value.ticket <= last_ticket &&
                                              value.status == MarchStatus::Queued && (!privileged_only || value.proposer);
                                     });
      if (found == march_requests.end()) {
        if (privileged_only)
          privileged_march_pending.store(false, std::memory_order_release);
        return;
      }

      found->status = MarchStatus::Executing;
      request = *found;
    }

    // Resolve again at execution time: the request can outlive a UI callback.
    WorldEntityBinding source{}, destination{};
    const auto source_status = *invoke_world_service(Operation::ResolveCity, request.source,
        static_cast<std::int64_t>(request.session), &source, sizeof(source));
    const auto destination_status = *invoke_world_service(Operation::ResolveCity, request.destination,
        static_cast<std::int64_t>(request.session), &destination, sizeof(destination));

    request.status = MarchStatus::InvalidEntity;
    if (source_status == 1 && destination_status == 1) {
      auto *city = reinterpret_cast<void *>(source.memory.address);
      auto *target_city = reinterpret_cast<void *>(destination.memory.address);
      std::array<std::uintptr_t, 6> adjacent{};
      SIZE_T copied = 0;
      if (ReadProcessMemory(GetCurrentProcess(), static_cast<std::byte *>(city) + 0x20,
                             adjacent.data(), sizeof(adjacent), &copied)) {
        request.status = MarchStatus::DestinationNotAdjacent;
        if (std::find(adjacent.begin(), adjacent.end(), destination.memory.address) != adjacent.end()) {
          request.status = MarchStatus::NoFaction;
          using GetForce = void *(*)(void *);
          const auto get_force = reinterpret_cast<GetForce>(native_ui_module + 0x17121D0);
          auto *force = get_force(city);
          auto *defender = get_force(target_city);
          const auto source_force = force ? native_field<std::uint8_t>(force, 8) : 0;
          const auto destination_force = defender ? native_field<std::uint8_t>(defender, 8) : 0;
          if (source_force != request.source_force || destination_force != request.destination_force) {
            request.status = MarchStatus::OwnershipChanged;
          }
          else if (force) {
            request.status = MarchStatus::TargetNoLongerEligible;
            if (reinterpret_cast<bool (*)(void *)>(native_ui_module + 0x1704940)(target_city) &&
                reinterpret_cast<bool (*)(void *, void *)>(native_ui_module + 0x1461CD0)(force, target_city)) {
              // REA 145A6B0: RDX=source CityData, R8=destination CityData.
              // RCX is unused. The original routine owns formation construction,
              // supply/tactical checks, 194A050 execution and all cleanup afterward.
              using TryMarch = std::uint64_t (*)(std::uintptr_t, void *, void *, std::uintptr_t);
              request.status = request.proposer ? validate_privilege_march(city, request)
                                               : MarchStatus::Queued;
              if (request.status == MarchStatus::Queued) {
                // Original consumption updates the quantity, usage counter and
                // game notifications. Never perform a managed memory decrement.
                if (request.proposer)
                  reinterpret_cast<void (*)()>(native_ui_module + 0x17275E0)();
                const auto result = reinterpret_cast<TryMarch>(native_ui_module + 0x145A6B0)(0, city, target_city, 0);
                request.game_return = static_cast<std::uint8_t>(result);
                request.status = request.game_return ? MarchStatus::Invoked : MarchStatus::RejectedByGame;
              }
            }
          }
        }
      }
    }

    // Native presentation can publish another session or run another script.
    // Hold no bridge lock across the game call, and never restore a retired receipt.
    std::lock_guard lock(native_ui_mutex);
    const auto found = std::find_if(march_requests.begin(), march_requests.end(),
                                   [&](const auto &value) { return value.ticket == request.ticket; });
    if (found != march_requests.end())
      *found = request;
  }
}
