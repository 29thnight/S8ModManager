#pragma once
#include <cstdint>

namespace s8::scripting {
  inline constexpr std::uint32_t api_version = 21;
  inline constexpr std::uint64_t ui_capability = 1, data_capability = 2, memory_capability = 4;
  enum class Status : int {
    Ready = 0,
    InvalidApi = -1,
    ApiVersionMismatch = -2,
    UnsupportedGameBuild = -3,
    CodeMismatch = -4,
    MissingCapability = -5,
    RuntimeUnavailable = -6,
    ScriptFailure = -7,
    InvalidBinding = -8,
    MemoryAccessDenied = -9,
    StaleSession = -10
  };

  struct Connection {
    Status status;
    std::uint32_t game_build;
    std::uint64_t capabilities;
  };


  constexpr const wchar_t *status_message(Status status) {
    switch (status) {
    case Status::Ready:
      return L"연결 완료";

    case Status::ApiVersionMismatch:
      return L"C++ API와 C# SDK 버전이 다릅니다.";

    case Status::UnsupportedGameBuild:
      return L"지원하지 않는 게임 실행 파일입니다. C++ 바인딩 업데이트가 필요합니다.";

    case Status::CodeMismatch:
      return L"게임 내부 코드가 분석 기준과 다릅니다. 네이티브 UI 연결을 중단했습니다.";

    case Status::MissingCapability:
      return L"현재 연결은 모드가 요구하는 기능을 제공하지 않습니다.";

    case Status::RuntimeUnavailable:
      return L"C# Host 또는 .NET 10 런타임을 불러오지 못했습니다.";

    case Status::ScriptFailure:
      return L"C# 스크립트 로드 또는 실행에 실패했습니다.";

    case Status::InvalidBinding:
      return L"검증된 엔진 연결과 전달된 API 정보가 다릅니다.";

    case Status::MemoryAccessDenied:
      return L"메모리 범위 또는 접근 권한이 요청과 맞지 않습니다.";

    case Status::StaleSession:
      return L"게임 세션이 변경되어 이전 참조를 사용할 수 없습니다.";

    default:
      return L"네이티브 API 함수 테이블이 올바르지 않습니다.";
    }
  }
  // Inferred engine bindings and manager memory services; no editor commands.
  enum class Event : int {
    Open,
    Select,
    Click,
    Closing,
    Closed,
    Resume,
    TextInput,
    NumberInput,
    Configure,
    MenuClick,
    WindowClick,
    WindowClosing,
    TreeClick,
    BattleTurnStarted,
    BattleUnitActionEnded,
    BattleTurnEnded,
    BattleSessionEnded,
    DomesticActivityCompleted,
    TalkCompleted,
    MonthChanged,
    CouncilEntered,
    CouncilRoomReady,
    TabChanged,
    OptionChanged
  };
  enum class Operation : int {
    ShowDialog,
    CloseDialog,
    SetRow,
    InputText,
    InputNumber,
    ReadInputText,
    GetPersonality,
    ReadPersonalityName,
    ReadPersonalityDescription,
    InspectMemory,
    ReadMemory,
    WriteMemory,
    GetSession,
    RegisterMenu,
    RegisterWindowRow,
    RequestOverlay,
    ReadGameState,
    SubmitGameCommand,
    SetWindowDirty,
    SetTreeNodes,
    SetTreeFooter,
    GetTreeCategory,
    GetGemCategory,
    ReadSelection,
    ReleaseUiRegistration,
    ReadBattleUnits,
    SetBattleSoldiers,
    ShowBattlePersonality,
    GetBattleContext,
    GetWorldContext,
    ResolveOfficer,
    ResolveCity,
    ShowOfficerDialogue,
    ReadEntityValue,
    SetEntityValue,
    AddEntityValue,
    AddFriendshipTokens,
    ReadOfficerAffiliation,
    ReadCityAffiliation,
    QueryOfficerActionAvailability,
    ReadCityAttackCandidates,
    RequestDomesticActivity,
    RequestCityMarch,
    ReadCityMarchResult,
    ReadWorldSaveData,
    StageWorldSaveData,
    ReadPlayerPrivilege,
    RequestPrivilegedCityMarch,
    DisableDialogRow,
    ReadCityName,
    CloseCouncilRoom,
    OpenCouncilRoom,
    RegisterUiSprite,
    FindUiSprite,
    ApplyUiSprite,
    ReadBattleSoldiers,
    SetFormButtonBounds,
    HideFormDecoration,
    SetFormButtonLabel,
    SetFormButtonIcon,
    ReadArchiveResource,
    SetJewelForceMask,
    SetTraitCatalog,
    SetDialogTabs,
    SetRowBadge,
    SetOptionsTab,
    ReadModList,
    SetModEnabled
  };

  // SetTraitCatalog: one record per trait ID 1..254 (index = ID - 1). The C# mod
  // decides every value; the native runtime only applies them to the game.
  struct TraitCatalogRecord {
    const wchar_t *name;        // null keeps the game's text
    const wchar_t *description; // null keeps the game's text
    std::uint16_t effects[18];  // (type, value, param) x 6
    std::uint8_t present;       // the effect row is defined
    std::uint8_t has_metadata;  // name/description/clone/grade fields apply
    std::uint16_t reserved;
    std::int32_t clone_source, background_trait, grade, voice; // -1 = unset
  };
  static_assert(sizeof(TraitCatalogRecord) == 72);
  inline constexpr std::uint32_t trait_catalog_count = 254;

  enum class DialogKind : int { List, Form, CompactForm, Text, MultilineText, Message, GemTree, MultiSelect, CityMap };

  struct UiRow {
    int action, height;

    const wchar_t *label;
    const wchar_t *text;
  };

  struct DialogSpec {
    int owner;

    DialogKind kind;
    int confirm_closes;
    int reserved;

    const wchar_t *title;
    const wchar_t *caption;
    const UiRow *rows;
    int count;
    int padding;
    const int *selection;
    int selection_count, selection_padding;
  };

  static_assert(sizeof(DialogSpec) == 64);

  // SetDialogTabs: game tab bar on a form dialog. Each row shows only on its tab;
  // switching tabs raises Event::TabChanged with the new tab index.
  struct DialogTabs {
    const wchar_t *const *labels;
    int count, reserved;
    std::uint8_t row_tabs[64]; // indexed by row action
  };
  static_assert(sizeof(DialogTabs) == 80);

  // SetRowBadge: draw a form row's value as a game badge (pill image with the
  // row text on top). Valid before or after the dialog is built; afterwards it
  // swaps the image and text colour in place.
  struct RowBadge {
    int resource, width, height, palette;
  };
  static_assert(sizeof(RowBadge) == 16);

  // SetOptionsTab: one extra tab on the title screen's options (환경설정). Each row
  // is a game choice row (◀ value ▶); hovering it shows its description in the
  // game's description panel. A changed choice raises Event::OptionChanged with
  // owner = the registration owner and value = (row action << 16) | choice index.
  struct OptionRow {
    int action, current;
    const wchar_t *label;
    const wchar_t *description;
    const wchar_t *const *choices;
    int choice_count, reserved;
  };
  static_assert(sizeof(OptionRow) == 40);

  struct OptionsTab {
    int owner, count;
    const wchar_t *label;
    const OptionRow *rows;
  };
  static_assert(sizeof(OptionsTab) == 24);

  // ReadModList: every mod the manager found this run, one per line, fields split
  // by tabs: id, name, version, runtime (jit/aot), enabled in mods.json (0/1),
  // state (loaded/skipped/failed), reason, required API (0 = any), game build
  // (hex, empty = any), dependencies (id>=x.y.z, comma separated), conflicts.
  // The first line is: host API, current game build (hex).
  // SetModEnabled: data = mod id (UTF-16), argument = 0/1; written to mods.json and
  // applied on the next start.

  struct TextInput {
    const wchar_t *initial;
    int limit, multiline;
  };

  struct NumberInput {
    int initial, minimum, maximum, reserved;
  };

  struct MemoryRef {
    std::uint64_t address, session;
  };

  struct MemoryRegion {
    std::uint64_t base, size;
    std::uint32_t protection, writable;
  };

  struct Personality {
    MemoryRef memory;
    std::uint16_t id, voice;
    std::uint16_t effects[18];
  };

  enum class MenuSurface : int { TitleEdit, InGameEdit, Function, Palace };

  struct MenuItem {
    MenuSurface surface;
    int action, template_id, insert_after;
    const wchar_t *label;
    int owner, reserved;
  };

  struct WindowRow {
    int slot, action;
    const wchar_t *label;
    const wchar_t *button;
    int owner, reserved;
  };

  struct TreeNode {
    std::uint16_t id;
    std::uint8_t opened, enabled;
  };

  static_assert(sizeof(TreeNode) == 4);

  struct TreeFooter {
    int action, enabled;
    const wchar_t *label;
  };

  struct EngineApi {
    std::uint32_t version, size;
    Connection connection;
    std::int64_t (*invoke)(Operation, std::uint64_t, std::int64_t, void *, std::uint32_t);
  };

  using Dispatch = int (*)(const EngineApi *, Event, int, int);
  static_assert(sizeof(EngineApi) == 32);
  static_assert(sizeof(Personality) == 56);
} // namespace s8::scripting
