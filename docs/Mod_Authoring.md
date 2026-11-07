# 모드 제작 안내 (API 21)

모드는 C# 클래스 하나와 `mod.json` 하나로 만듭니다. 게임 기능은 네이티브 DLL이 정해 둔 연산으로만 부를 수 있고, 게임 논리(무엇을 언제 바꿀지)는 모두 모드가 정합니다.

이 문서는 2026-11-07 기준 저장소 코드와 실제 게임 확인 결과를 바탕으로 합니다.

## 1. 가장 작은 모드

### 폴더

```text
<게임 폴더>\scripts\mods\my.hello\
  mod.json
  My.Hello.dll        ← 빌드 결과
  assets\             ← 선택. 모드가 읽는 자료
```

### mod.json

```json
{
  "id": "my.hello",
  "name": "인사 모드",
  "version": "0.1.0",
  "apiVersion": 21,
  "runtime": "jit",
  "entry": "My.Hello.dll",
  "type": "My.Hello.HelloMod"
}
```

| 항목 | 뜻 |
|---|---|
| `id` | 모드 식별자. `mods.json` 의 켜기·끄기, 의존·충돌 선언이 이 값을 씁니다 |
| `name` | 환경설정 → 모드 탭에 보이는 이름 |
| `version` | `x.y.z`. 같은 `id` 가 여럿이면 높은 판, 같으면 JIT 판이 적용됩니다 |
| `apiVersion` | 요구 엔진 API. 매니저와 다르면 건너뜁니다. 지금은 **21** |
| `runtime` | `jit` 또는 `aot` |
| `entry` | 모드 DLL 파일 이름 |
| `type` | `ScriptMod` 를 상속한 클래스의 전체 이름(JIT만) |
| `gameBuild` | 선택. 게임 실행 파일 빌드 값, 16진수 문자열(예 `"69A67ED1"`). 다르면 건너뜁니다 |
| `dependencies` | 선택. `{"다른 모드 id": "최소 판"}`. 필요한 모드가 먼저 불립니다 |
| `conflicts` | 선택. 같이 켤 수 없는 모드 `id` 배열. `mods.json` 순서가 앞선 쪽만 적용됩니다 |

### 프로젝트 파일

```xml
<Project Sdk="Microsoft.NET.Sdk">
  <PropertyGroup>
    <TargetFramework>net10.0</TargetFramework>
    <Nullable>enable</Nullable>
    <ImplicitUsings>enable</ImplicitUsings>
    <AllowUnsafeBlocks>true</AllowUnsafeBlocks>
  </PropertyGroup>
  <ItemGroup>
    <!-- 게임 폴더의 scripts\S8.Scripting.dll. 매니저가 자기 것을 넘겨주므로 복사하지 않아도 됩니다 -->
    <Reference Include="S8.Scripting">
      <HintPath>$(GameDir)\scripts\S8.Scripting.dll</HintPath>
      <Private>false</Private>
    </Reference>
  </ItemGroup>
</Project>
```

저장소 안에서는 `managed\S8.ManagerMod\S8.ManagerMod.csproj` 처럼 `ProjectReference` 로 `S8.Scripting.csproj` 를 걸면 됩니다.

### 코드

```csharp
using S8.Scripting;

namespace My.Hello;

public sealed class HelloMod : ScriptMod
{
    private UiScene? scene;

    protected override void OnLoad() =>
        AddMenu(MenuSurface.TitleEdit, "인사", Open);   // 시작 화면 → 편집 메뉴에 항목 추가

    private void Open()
    {
        scene = new UiScene();
        scene.Create("Hello").Show(DialogKind.Message, "인사 모드", [new UiRow(0, "확인", "안녕하세요.")]);
    }

    protected override void OnUiClosed()   // 게임 창이 모두 닫힘
    {
        scene?.Dispose();
        scene = null;
    }
}
```

빌드한 DLL과 `mod.json` 을 모드 폴더에 넣고 게임에서 편집 메뉴나 시작 화면의 환경설정을 다시 열면 됩니다. 새 모드는 `scripts\mods.json` 끝에 켜진 상태로 덧붙습니다.

## 2. 모드의 수명

| 시점 | 불리는 곳 | 할 일 |
|---|---|---|
| 처음 불릴 때 | `OnLoad()` | 메뉴·창 행·환경설정 탭 등록, 자산 읽기. **등록은 불러오기 사건(`Open`·`Configure`) 안에서만 됩니다.** `OnLoad` 가 그 안에서 돌고, 탭을 새로 그리려면 `OnEvent` 의 `Configure` 에서 다시 등록합니다 |
| 메뉴·행을 누름 | 등록할 때 넘긴 콜백 | 대화창 열기 |
| 대화창 사건 | `DialogComponent` 의 콜백 | 선택·클릭·닫힘·입력 완료 처리 |
| 게임 창이 모두 닫힘 | `OnUiClosed()` | 열 때 만든 `UiScene` 정리 |
| 그 밖의 사건 | `OnEvent(kind, owner, value)` | 아래 표의 앞 단계가 가져가지 않은 사건만 옵니다. 전투 턴·전투 끝, `Configure`, `Open` 등 |
| 모드 관리에서 꺼짐 | `OnDisabled()` 다음 `OnUnload()` | 모드가 게임에 바꿔 둔 것을 되돌리기 |
| 교체·종료 | `OnUnload()` | 자원 정리 |

`ScriptMod` 가 직접 열어 둔 재정의 지점은 위의 다섯(`OnLoad`, `OnUiClosed`, `OnEvent`, `OnDisabled`, `OnUnload`)과 `CanReload` 가 전부입니다. 나머지 사건은 아래 차례로 먼저 배분되고, **앞 단계가 가져가면 `OnEvent` 까지 오지 않습니다.**

| 차례 | 받는 곳 | 가져가는 사건 |
|---|---|---|
| 0 | `World` 세션 갱신(모든 사건 직전) | 없음. 바뀐 것이 있으면 `World.SessionStarted`·`SessionEnded`·`SceneEntered`·`SceneExited`·`ProtagonistChanged` C# 이벤트를 올립니다 |
| 1 | 무장·도시 컴포넌트의 처리기 인터페이스와 `World` 이벤트 | `MonthChanged`(`IMonthChangedHandler`), `CouncilEntered`(`ICouncilEnteredHandler`), `DomesticActivityCompleted`, `TalkCompleted`. 같은 사건이 세계 사건 발화기로도 갑니다 |
| 2 | `GameSetup.Handle` | 설정 클래스가 `true` 를 돌린 사건 |
| 3 | `World.Events`(세계 사건 실행기) | `CouncilRoomReady`, 대기 중인 후속 화면이 있을 때의 `Resume` |
| 4 | `ScriptMod` 자체 | `OptionChanged`(탭 콜백), `MenuClick`·`WindowClick`·`WindowClosing`(등록 콜백), `Closed`(`OnUiClosed`). `BattleSessionEnded` 는 `OnEvent` 로도 보내고 세계 사건에도 알립니다 |
| 5 | 대화창을 만든 `UiScene` | 그 대화창의 `Select`·`Click`·`Closing`·`Resume`·`TextInput`·`NumberInput`·`TreeClick`·`TabChanged` |
| 6 | `OnEvent` | 여기까지 남은 모든 사건 |

그 밖의 수명 지점:

- `Component.OnEnable` / `OnDisable`: 컴포넌트를 붙일 때와 떼거나 객체를 버릴 때.
- `GameSetup.Configure`: 모드 어셈블리 안의 이 형식을 반사로 찾아 `OnLoad` 다음에 한 번 부릅니다(`Order` 순). `Dispose` 는 모드가 내려갈 때.
- `WorldSetup.Configure`: 같은 방식으로 찾되, **게임 세션이 시작될 때마다** 새로 만들어 부릅니다(`SessionStarted` 직전). `Dispose` 는 세션이 끝날 때.
- 전투 효과는 `Effects` 에 등록하고 기재에 묶으면 `ScriptMod` 가 전투 사건마다 부릅니다. 효과는 (무장, 기재, 효과)마다 한 번 붙고, 한 사건에서 부대마다 한 번만 돌며, 기재를 잃으면 떨어지고, 모드가 내려가면 모두 정리됩니다. 원래 사건은 그 뒤에 `OnEvent` 로도 갑니다.

없는 것: 매 프레임 갱신, 세이브 저장·불러오기 직전 알림, 게임 종료 알림은 없습니다. 세션 시작·끝도 게임이 바로 알려 주는 사건이 아니라, **다음 사건이 왔을 때** 세션 값을 다시 읽고 바뀐 것을 알아채는 방식입니다.

알아 둘 규칙:

- **모든 콜백은 게임 스레드에서 동기로 돕니다.** 오래 걸리면 화면이 멈춥니다. 매 프레임 불리는 갱신 함수는 없습니다.
- 입력 완료·닫힘 콜백 안에서는 새 창을 열거나 닫을 수 없습니다. 후속 화면은 `Resumed` 콜백에서 엽니다.
- **켜고 끄기는 재시작 없이 바로 적용됩니다.** 꺼지면 `OnDisabled` 에서 게임을 원래대로 돌려야 합니다. 예: 기재 편집 모드는 빈 기재 목록을 다시 올리고 보주 강제 적용을 비웁니다(`S8.GameScripts\ModApplication.cs`). 다시 켜지면 새 인스턴스로 `OnLoad` 부터 다시 시작합니다.
- 저장하지 않은 편집이 있으면 `CanReload` 를 `false` 로 돌려 실행 중 교체를 미룹니다.

## 3. 개발 중 바로 고쳐 보기 (JIT)

JIT 모드에는 두 가지 교체 방식이 있습니다.

**1 → 소스 폴더 방식(권장).** 모드 폴더에 `source\` 폴더를 두고 `.cs` 파일을 넣으면, 매니저가 그 소스를 직접 컴파일해서 씁니다. 메서드 본문만 고친 경우 **다음 사건 때 실행 중인 코드에 바로 반영**됩니다. 멤버 추가·삭제나 서명 변경처럼 구조가 바뀌면 다음에 메뉴를 열 때 모듈 전체를 바꿉니다. 컴파일이 안 되면 기존 코드를 그대로 둡니다.

**2 → DLL 교체 방식.** `source\` 가 없으면 DLL이나 `mod.json` 의 수정 시각이 바뀐 것을 보고, 다음에 메뉴를 열 때 모듈을 바꿉니다.

두 방식 모두 `S8.Scripting.dll` 이나 매니저 자체가 바뀌면 게임을 다시 켜야 합니다. 진단 출력(`[S8 live] ...`)은 디버그 출력 창(DebugView 등)에 남습니다.

## 4. 배포판 (AOT)

AOT 판은 .NET 런타임 없이 네이티브 DLL로 불립니다. 같은 모드를 JIT 판과 함께 둘 수 있고, 판이 같으면 JIT 쪽이 이깁니다.

- `mod.json`: `"runtime": "aot"`, `type` 없음.
- 내보낼 함수 셋. `S8.GameScripts\AotExports.cs` 를 그대로 본뜨면 됩니다.
  ```csharp
  [UnmanagedCallersOnly(EntryPoint = "S8ModLoad")]     → AotModHost<내모드>.Load
  [UnmanagedCallersOnly(EntryPoint = "S8ModDispatch")] → AotModHost<내모드>.Dispatch
  [UnmanagedCallersOnly(EntryPoint = "S8ModUnload")]   → AotModHost<내모드>.Unload
  ```
- **`S8ModUnload` 가 없으면 실행 중에 끌 수 없습니다.** 모드 관리에서 꺼도 "다음 실행부터 빠집니다"로 표시됩니다.
- 빌드: `dotnet publish <프로젝트> -c Release -r win-x64 -p:PublishAot=true -o <게임>\scripts\mods\<폴더>`
- 반사(리플렉션)로 찾는 형식은 잘라 내기에서 지워질 수 있습니다. `GameSetup`·`WorldSetup` 을 쓰면 `TrimmerRootAssembly` 로 붙잡아 두십시오(`S8.GameScripts.csproj` 참고).

## 5. 할 수 있는 것

표의 "확인"은 실제 게임에서 동작을 본 것, "시험판"은 도구에 있지만 0.0.1 배포판에서는 빼 둔 기능입니다.

### 게임 화면 붙이기

| 기능 | 도구 | 상태 |
|---|---|---|
| 메뉴 항목 추가 | `AddMenu(MenuSurface.TitleEdit / InGameEdit / Function / Palace, 이름, 콜백)` | 시작 화면·게임 안 편집 메뉴 확인 |
| 게임 창에 행 추가 | `AddWindowRow(칸, 이름, 단추, 클릭, 닫힘)`. 닫힘은 결정/취소 여부를 받음 | 주인공 편집의 보주 행 확인 |
| 대화창 | `UiScene.Create(...).Show(DialogKind, 제목, 행들, ...)`. 목록·양식·글·알림·다중 선택·보주 나무·도시 지도 | 목록·양식·다중 선택·보주 나무 확인 |
| 양식 탭, 행 끄기, 행 그림 | `Show(..., tabs:)`, `UiRow.Enabled`, `ApplyRowSprite`, `SetRowBadge` | 기재 편집에서 사용 |
| 글자·숫자 입력 | `EditText(초기값, 최대 글자, 여러 줄)`, `EditNumber(초기, 최소, 최대)` | 확인. 단, 자동 한글 입력은 화면에 반영되지 않음 |
| 환경설정 탭 | `SetOptionsTab(이름, OptionRow 목록, 바뀜 콜백)`. 모드당 탭 하나, 줄 1~64개, 줄마다 선택지 1~16개, 설명 칸 문구 | 확인(모드 관리 탭) |
| 모드 목록 읽기·켜고 끄기 | `ModCatalog.Read()`, `ModCatalog.SetEnabled(id, 켬)` | 확인 |

### 게임 자료

| 기능 | 도구 | 상태 |
|---|---|---|
| 기재 이름·설명·효과·음성·등급 바꾸기 | `TraitCatalog.Publish(TraitDefinition?[])`. 빈 칸은 게임 원래 값 | 확인 |
| 기재 레코드 읽기 | `PersonalityComponent` | 확인 |
| 보주 개방·강제 적용 | `JewelComponent`, `PlayerJewelsComponent`, `JewelComponent.RestoreForced` | 확인 |
| 무장 능력치·금·행동력·명성, 도시 값을 속성으로 | `World.PlayerOfficer`, `World.Officer(id)`, `World.City(id)`가 주는 `Officer`·`City` 핸들. 예: `World.PlayerOfficer.Gold += 1000`, `World.City(1).Grain = 400`. 세이브를 불러오면 이전 핸들은 `EngineCallException`으로 거부됨. 읽기 전용 값(`TechnologyLimit`)은 설정자가 없음 | 모의 시험으로 확인, 게임 안에서는 미확인 |
| 훈련·기재 칸, 그 밖의 세부 | `OfficerStatsComponent`, `OfficerFameComponent`, `OfficerTrainingComponent`, `OfficerTraitsComponent`, `CityAdministrationComponent` | 도구에 있음. 이 문서에서 동작을 보증하지 않음 |
| 소속(무장·도시) | `OfficerAffiliationComponent`, `CityAffiliationComponent` | 위와 같음 |
| 세계·장면·주인공 | `World`(`GameWorld`): 세션, 현재 장면, 주인공, 무장·도시 목록과 그 사건 | 위와 같음 |
| 세이브에 모드 자료 붙이기 | `World.SaveData` | 위와 같음 |

### 사건 받기

**인터페이스를 몰라도 됩니다.** `OnLoad` 에서 람다 한 줄로 받는 것이 기본 방법입니다.

```csharp
protected override void OnLoad()
{
    World.MonthChanged += month => { /* month.Current: 새 날짜 */ };
    World.CouncilEntered += council => { /* council.Date */ };
    World.TalkCompleted += talk => { /* talk.Officer, talk.Partner */ };
    World.DomesticActivityCompleted += activity =>
    {
        if (activity.Kind == DomesticActivityKind.Commerce) { /* activity.Officer */ }
    };
}
```

전투 효과도 클래스 없이 등록합니다. 필요한 시점만 넘기면 됩니다.

```csharp
Effects.Register(new("my.heal", "턴 종료 회복", "부대 병력을 회복합니다."),
    onTurnEnd: trait =>
    {
        trait.Show("치료하라!");             // 기재 발동 연출
        trait.Unit.RestoreSoldiers(100);    // 그 기재를 가진 무장의 부대
    });
Effects.Bind(200, "my.heal");               // 기재 200번을 가진 무장에게 적용. 연결 코드는 필요 없음
```

`onTurnStart`(턴 시작), `onUnitActionEnd`(부대 행동 끝), `onTurnEnd`(전체 행동 끝) 중 쓰는 것만 넘깁니다. 등록하지 않은 효과를 묶으면 그 자리에서 `ArgumentException` 이 납니다. 사용자가 고치는 파일에서 묶음을 읽는 모드는 `Effects.UseBindings(기재번호 => 효과목록)` 을 씁니다(`S8.GameScripts` 의 `ScriptedTraitCatalog` 참고).

처리기 인터페이스(`IMonthChangedHandler`, `IBattleTurnEndEffect` 등)는 같은 일을 무장·도시 컴포넌트 안에서 하는 고급 방법입니다. 무장마다 상태를 오래 붙여 두려는 경우가 아니면 람다로 충분합니다.

| 사건 | 상태 |
|---|---|
| 전투 턴 시작 / 부대 행동 끝 / 전체 행동 끝 / 전투 끝 (`IBattleTurnStartEffect` 등) | 시험판. 전체 행동 끝에서 병력 회복과 기재 발동 연출까지 확인 |
| 내정 완료(개간·상업·성문 보수·치안·기술, 플레이어 무장만), 대화 완료, 월 변경, 평정 진입 (`IMonthChangedHandler` 등) | 시험판. 내정 완료 다섯 종류는 2026-11-07 실측 |
| 스크립트 세계 사건(`WorldEvent`, `EventConditions`, `EventEffects.Say/RestoreSoldiers/ShowPersonality`) | 시험판 |
| 출진 요청, 평정 전략(`MarchRequest`, `CouncilStrategyComponent`) | 시험판 |

시험판 기능을 쓰는 예시 코드는 `S8.GameScripts` 의 `S8_EXPERIMENTAL` 구역과 `docs\design\` 문서에 있습니다.

### 그 밖에

- **메모리 읽기·쓰기**: `MemoryAccess.Read/Write`, `MemoryComponent`. 세션이 맞는 주소만, 한 번에 64KiB까지, 실행 코드 쪽 쓰기는 거부합니다.
- **모드 파일**: `Context.ModDirectory` 에 쓰고 `Context.AssetDirectory`(`assets\`) 에서 읽습니다. `Context.GameDirectory` 는 읽기만 합니다.
- **NuGet 꾸러미**: JIT 모드는 빌드 결과(`deps.json` 과 의존 DLL)를 모드 폴더에 같이 두면 불립니다. 단, `source\` 폴더 방식은 .NET 기본 어셈블리와 `S8.Scripting` 만 참조해 컴파일하므로 외부 꾸러미를 쓸 수 없습니다.

## 6. 할 수 없는 것

- **게임 함수를 직접 부를 수 없습니다.** 주소나 가상 함수 호출은 모두 네이티브 DLL 안에 있고, 모드는 정해진 연산만 씁니다. 새 게임 기능이 필요하면 네이티브 쪽에 연산을 추가하고 `apiVersion` 을 올려야 합니다.
- **임의 스레드나 콜백 밖에서 게임을 만질 수 없습니다.** 콜백 밖에서 부르면 `InvalidOperationException` 이, 지난 세션의 주소·토큰을 쓰면 `EngineCallException`(`StaleSession` 등)이 납니다.
- **환경설정 탭은 시작 화면에서만 보입니다.** 게임 안 환경설정에는 붙지 않습니다.
- **다른 모드의 사건을 가로챌 수 없습니다.** 탭·메뉴 사건은 등록한 모드에게만 갑니다.
- **게임 빌드가 다르면 아무것도 붙지 않습니다.** 매니저가 실행 파일과 코드 바이트를 검사해 맞지 않으면 기능을 모두 끕니다.

## 7. 알려진 제약

- 옵션 탭 글꼴에는 가운뎃점이 없습니다. `·` 는 점 두 개로, `・` 와 게임 탭의 `U+E060` 은 빈칸으로 나옵니다. 모드 관리 탭은 `/` 로 바꿔 씁니다.
- 같은 `id` 의 JIT 판과 AOT 판은 모드 탭에 두 줄로 보이지만 켜기·끄기를 함께 씁니다. 한 줄을 바꾸면 다른 줄은 탭을 다시 열어야 새 값이 보입니다.
- 모드를 건너뛴 이유는 모드 탭 설명 칸과 디버그 출력에 나옵니다. `scripts\mod_manager.log` 에는 후킹 실패만 남습니다.

## 8. 참고할 코드

| 보고 싶은 것 | 파일 |
|---|---|
| 가장 작은 실제 모드(환경설정 탭, 모드 목록) | `src\scripting\managed\S8.ManagerMod\ManagerMod.cs` |
| 메뉴·창 행·끄기 처리 | `src\scripting\managed\S8.GameScripts\ModApplication.cs` |
| 목록·양식·입력 대화창 | `src\scripting\managed\S8.GameScripts\TraitEditor.cs` |
| 보주 나무·다중 선택 | `src\scripting\managed\S8.GameScripts\JewelEditor.cs` |
| 기반 클래스와 사건 배분 | `src\scripting\managed\S8.Scripting\ScriptMod.cs` |
| 사건 번호 전체 | `src\scripting\managed\S8.Scripting\Native.cs` 의 `ScriptEvent` |
| 시험 묶음(가짜 엔진으로 모드 돌리기) | `tests\scripting\Program.cs`, `tests\scripting\OptionsChecks.cs` |
