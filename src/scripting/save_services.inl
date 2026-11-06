// Opaque managed state, staged on admitted callbacks. File hooks never enter CLR.
ui_detail::Hook archive_stage_save_hook{}, archive_complete_save_hook{}, archive_load_save_hook{},
    archive_scenario_save_hook{};
std::uint32_t world_save_generation = 0;
std::string world_save_blob;
std::unordered_map<void *, std::string> archive_save_snapshots;
constexpr std::size_t MAX_WORLD_SAVE_BYTES = 1024 * 1024;

struct ModSaveHeader {
  std::uint32_t magic = 0x3153384D, version = 1, bytes = 0;
  std::array<unsigned char, 32> game_hash{}, state_hash{};
};
static_assert(sizeof(ModSaveHeader) == 76);

// Hashes bytes once at each file boundary. No timer or directory scan.
bool hash_save_bytes(std::istream &input, std::array<unsigned char, 32> &output) {
  BCRYPT_ALG_HANDLE algorithm = nullptr;
  BCRYPT_HASH_HANDLE hash = nullptr;
  bool valid = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0;
  if (valid)
    valid = BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) >= 0;
  std::array<char, 65536> block{};
  while (valid && input) {
    input.read(block.data(), block.size());
    const auto bytes = input.gcount();
    if (bytes != 0)
      valid = BCryptHashData(hash, reinterpret_cast<unsigned char *>(block.data()),
                            static_cast<ULONG>(bytes), 0) >= 0;
  }
  valid = valid && input.eof() && !input.bad();
  if (valid)
    valid = BCryptFinishHash(hash, output.data(), static_cast<ULONG>(output.size()), 0) >= 0;
  if (hash)
    BCryptDestroyHash(hash);
  if (algorithm)
    BCryptCloseAlgorithmProvider(algorithm, 0);
  return valid;
}

std::filesystem::path archive_save_path(void *archive) {
  const auto length = native_field<std::size_t>(archive, 0x68);
  if (length == 0 || length > 32767)
    return {};
  const char *path = native_field<std::size_t>(archive, 0x70) > 15
      ? native_field<const char *>(archive, 0x58)
      : static_cast<const char *>(archive) + 0x58;
  return std::filesystem::u8path(path, path + length);
}

// Capture before serialization begins; delayed file finalization uses this copy.
std::uint32_t stage_archive_save(void *archive, std::uintptr_t second, const char *path,
                                 int automatic, int deferred, std::uintptr_t sixth) {
  {
    std::lock_guard lock(native_ui_mutex);
    archive_save_snapshots.erase(archive);
    if (game_publication.ready && !world_save_blob.empty())
      archive_save_snapshots[archive] = world_save_blob;
  }
  const auto result = reinterpret_cast<std::uint32_t (*)(void *, std::uintptr_t, const char *, int, int,
                                                        std::uintptr_t)>(archive_stage_save_hook.original)(
      archive, second, path, automatic, deferred, sixth);
  if (result != 0) {
    std::lock_guard lock(native_ui_mutex);
    archive_save_snapshots.erase(archive);
  }
  return result;
}

// REA 16D04C0: replace old destination with the completed temporary game file.
void complete_archive_save(void *archive) {
  reinterpret_cast<void (*)(void *)>(archive_complete_save_hook.original)(archive);
  std::string snapshot;
  {
    std::lock_guard lock(native_ui_mutex);
    const auto found = archive_save_snapshots.find(archive);
    if (found == archive_save_snapshots.end())
      return;
    snapshot = std::move(found->second);
    archive_save_snapshots.erase(found);
  }
  if (native_field<std::uint32_t>(archive, 8) != 0)
    return;
  const auto path = archive_save_path(archive);
  if (path.empty())
    return;
  ModSaveHeader header;
  header.bytes = static_cast<std::uint32_t>(snapshot.size());
  std::ifstream game_file(path, std::ios::binary);
  std::istringstream state_stream(snapshot);
  if (!game_file || !hash_save_bytes(game_file, header.game_hash) ||
      !hash_save_bytes(state_stream, header.state_hash)) {
    OutputDebugStringA("[S8 saves] Could not hash completed game save.\n");
    return;
  }
  auto sidecar = path;
  sidecar += L".s8mods";
  auto temporary = sidecar;
  temporary += L".tmp";
  std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
  file.write(reinterpret_cast<const char *>(&header), sizeof(header));
  file.write(snapshot.data(), static_cast<std::streamsize>(snapshot.size()));
  file.close();
  if (!file || !MoveFileExW(temporary.c_str(), sidecar.c_str(),
                           MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    OutputDebugStringA("[S8 saves] Could not commit mod save sidecar.\n");
}

// REA 16CCC70 returns the archive status from +8, including checksum validation.
std::uint32_t load_archive_save(void *archive, int slot, const char *path, int automatic) {
  const char *resolved = path && *path ? path
      : reinterpret_cast<const char *(*)(int, const char *, int)>(native_ui_module + 0x1791110)(slot, nullptr, automatic);
  const auto game_path = std::filesystem::u8path(resolved);
  const auto result = reinterpret_cast<std::uint32_t (*)(void *, int, const char *, int)>(
      archive_load_save_hook.original)(archive, slot, path, automatic);
  if (result != 0)
    return result;
  std::string restored;
  if (!game_path.empty()) {
    auto sidecar = game_path;
    sidecar += L".s8mods";
    std::ifstream file(sidecar, std::ios::binary);
    ModSaveHeader header;
    if (file.read(reinterpret_cast<char *>(&header), sizeof(header)) &&
        header.magic == 0x3153384D && header.version == 1 && header.bytes <= MAX_WORLD_SAVE_BYTES) {
      std::string candidate(header.bytes, '\0');
      std::array<unsigned char, 32> game_hash{}, state_hash{};
      std::ifstream game_file(game_path, std::ios::binary);
      if (file.read(candidate.data(), header.bytes) && file.peek() == std::char_traits<char>::eof() && game_file) {
        std::istringstream state_stream(candidate);
        if (hash_save_bytes(game_file, game_hash) && hash_save_bytes(state_stream, state_hash) &&
            game_hash == header.game_hash && state_hash == header.state_hash)
          restored = std::move(candidate);
        else
          OutputDebugStringA("[S8 saves] Mod sidecar hash mismatch; using empty mod state.\n");
      }
    }
  }
  std::lock_guard lock(native_ui_mutex);
  world_save_blob = std::move(restored);
  archive_save_snapshots.clear();
  ++world_save_generation;
  return result;
}

// Scenario replacement also runs before a disk load. A successful disk load
// subsequently installs its own verified sidecar; a new scenario starts empty.
void load_scenario_save(void *root, int scenario) {
  if (root == *reinterpret_cast<void **>(native_ui_module + 0x2E98BC8)) {
    std::lock_guard lock(native_ui_mutex);
    world_save_blob.clear();
    archive_save_snapshots.clear();
    ++world_save_generation;
  }
  reinterpret_cast<void (*)(void *, int)>(archive_scenario_save_hook.original)(root, scenario);
}

std::optional<std::int64_t> invoke_save_service(Operation operation, std::uint64_t session,
                                               std::int64_t generation, void *data, std::uint32_t bytes) {
  if (operation != Operation::ReadWorldSaveData && operation != Operation::StageWorldSaveData)
    return std::nullopt;
  if (session == 0 || generation < 0 || generation > UINT32_MAX || bytes > MAX_WORLD_SAVE_BYTES)
    return static_cast<int>(Status::InvalidApi);
  std::lock_guard lock(native_ui_mutex);
  if (session != game_publication.epoch || static_cast<std::uint32_t>(generation) != world_save_generation)
    return static_cast<int>(Status::StaleSession);
  if (!game_publication.ready)
    return static_cast<int>(Status::InvalidBinding);
  if (operation == Operation::ReadWorldSaveData) {
    if (!data && bytes == 0)
      return static_cast<std::int64_t>(world_save_blob.size());
    if (!data || bytes != world_save_blob.size())
      return static_cast<int>(Status::InvalidApi);
    std::memcpy(data, world_save_blob.data(), bytes);
  }
  else {
    if (!data || bytes == 0)
      return static_cast<int>(Status::InvalidApi);
    world_save_blob.assign(static_cast<const char *>(data), bytes);
  }
  return 0;
}
