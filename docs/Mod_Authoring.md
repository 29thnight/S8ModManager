# 모드 제작 안내 (API 21)

모드는 게임 폴더의 `scripts\mods\<폴더>\` 에 `mod.json` 과 코드를 넣어 만듭니다. 코드는 **Lua 파일 하나**로 쓰거나 **C# 클래스**로 씁니다. 어느 쪽이든 게임은 네이티브 DLL이 정해 둔 연산으로만 만지고, 무엇을 언제 바꿀지는 모드가 정합니다.

이 문서는 2026-11-07 기준 저장소 코드와 실제 게임 확인 결과를 바탕으로 합니다.

## 0. 무엇으로 만들까

| | Lua | C# |
|---|---|---|
| 시작 | `main.lua` 한 파일 | 프로젝트, 빌드 |
| 할 수 있는 일 | 메뉴, 무장·도시 값, 월 변경·평정 사건, 전투 효과, 기록 | 전부(대화창, 환경설정 탭, 기재·보주, 메모리, 세이브 자료 …) |
| 오류를 아는 때 | 실행할 때(`lua.log`) | 대부분 컴파일할 때 |
| 배포 | JIT만 | JIT, AOT |

값 몇 개를 바꾸거나 기재에 효과를 붙이는 정도면 Lua로 충분합니다. 창을 띄우거나 게임 자료를 크게 다루면 C#으로 씁니다. Lua 모드도 안에서는 C# 모드(`S8.LuaHost`)가 돌리므로, 수명·세션 규칙은 같습니다.

## 1. Lua 모드

### 폴더

```text
<게임 폴더>\scripts\mods\my.gold\
  mod.json
  main.lua
  S8.LuaHost.dll, S8.LuaHost.deps.json   ← 저장소 빌드의 out\x64\<구성>\lua\ 에서 복사
  lua51.dll                             ← 같은 곳에서 복사(LuaJIT)
```

### mod.json

```json
{
  "id": "my.gold",
  "name": "금 지급",
  "version": "0.1.0",
  "apiVersion": 21,
  "runtime": "jit",
  "entry": "S8.LuaHost.dll",
  "type": "S8.LuaHost.LuaMod"
}
```

`entry` 와 `type` 은 Lua 모드라면 늘 이 값입니다. 다른 항목은 [3절](#3-modjson)을 보십시오.

### main.lua

```lua
menu.add("in_game_edit", "금 1000 받기", function()
  local me = game.player
  me.gold = me.gold + 1000
  log("지금 금: " .. me.gold)
end)

events.on("month_changed", function(date)
  log(date.year .. "년 " .. date.month .. "월")
end)

-- 기재 효과. 어느 기재의 어느 칸에 얼마의 값으로 넣을지는 게임 안 기재 편집기에서 고른다.
effect "부상병 회복" {
  description = "턴이 끝날 때 부상병의 {value}%를 회복한다, 단 최대 3000",

  turn_end = function(unit, value)
    if unit:heal(unit.wounded * value / 100, 3000) > 0 then
      return "부상병을 치료하라!"
    end
  end,
}
```

`main.lua` 는 모드가 불릴 때 한 번 돌고, 그 안에서 등록한 함수가 사건마다 불립니다. 저장소의 `samples\lua-effect\` 가 그대로 도는 예제입니다. 빌드는 이 예제를 게임 폴더에 넣지 않으므로, 써 보려면 위 폴더 구성대로 직접 복사하십시오.

### 쓸 수 있는 것

| 이름 | 뜻 |
|---|---|
| `menu.add(위치, 이름, 함수)` | 메뉴 항목 추가. 위치: `"title_edit"`(시작 화면 편집), `"in_game_edit"`(게임 안 편집), `"function"`, `"palace"` |
| `game.player` | 플레이어가 지금 조작하는 무장 |
| `game.officer(번호)`, `game.city(번호)` | 번호로 무장·도시 얻기. 없으면 오류 |
| `events.on("month_changed", 함수)` | 달이 바뀐 뒤. 인자: `year`, `month`, `previous_year`, `previous_month` |
| `events.on("council_entered", 함수)` | 평정에 들어간 뒤. 인자: `year`, `month` |
| `effect "이름" { ... }` | 기재 효과 정의. 아래 "기재 효과" 참고 |
| `log(글)` | 모드 폴더의 `lua.log` 에 한 줄 |

**무장 값**(읽고 쓰기): `leadership`, `strength`, `intelligence`, `politics`, `charm`, `gold`, `action_points`, `civil_fame`, `martial_fame`, `infamy`. 읽기만: `id`.

**도시 값**(읽고 쓰기): `gold`, `grain`, `troops`, `development`, `commerce`, `defense`, `technology`, `revolt`, `development_limit`, `commerce_limit`, `defense_limit`, `troop_limit`. 읽기만: `id`, `name`, `technology_limit`.

### 기재 효과

효과는 기재가 아니라 **기재 칸에 넣는 부품**입니다. 모드는 효과를 한 번 정의하고, 플레이어가 게임 안 기재 편집기에서 어느 기재의 어느 칸에 얼마의 값으로 넣을지 고릅니다. 한 효과를 여러 기재에 다른 값으로 넣을 수 있습니다.

| 칸 | 뜻 |
|---|---|
| `description` | 기재 설명에 붙는 글. `{value}` 는 칸의 값으로 바뀝니다 |
| `turn_start` | 전투 턴이 시작될 때, 그 효과를 가진 무장의 부대마다 |
| `unit_action_end` | 부대 행동이 끝났을 때 |
| `turn_end` | 전체 행동이 끝났을 때 |
| `id` | 생략 가능. 아래 주의 참고 |

시점 함수는 셋 중 하나 이상 있어야 합니다. 함수는 `(unit, value, e)` 를 받고, **글을 돌려주면 그 부대의 대사로 뜹니다.**

- `unit.soldiers`, `unit.capacity`: 지금 병력과 최대 병력
- `unit.wounded`: 최대 병력에서 지금 병력을 뺀 수. 전멸한 부대는 0
- `unit:heal(양[, 상한])`: 소수점을 버리고 상한을 적용해 회복하고, 실제로 회복한 양을 돌려줌
- `value`: 이 기재 칸에 넣은 값
- `e.id`: 효과를 발동한 기재 번호

이 값들은 그 함수가 도는 동안에만 쓸 수 있습니다.

**주의: 효과는 기본으로 "모드 폴더 이름/효과 이름" 으로 저장됩니다.** 효과 이름을 바꾸면 이미 기재 칸에 넣어 둔 연결이 끊깁니다. 이름을 바꿀 수 있게 하려면 처음부터 `id = "내모드.부상병회복"` 처럼 고정하십시오.

### 규칙

- **없는 값이나 범위 밖 값은 오류입니다.** `me.nope` 은 `nil` 이 아니라 오류이고, `me.gold = -1` 도 오류입니다. 오류 문구에는 `main.lua:줄` 이 붙습니다.
- **콜백 안의 오류는 `lua.log` 에 남고 게임은 계속 돕니다.** `main.lua` 를 처음 돌릴 때 난 오류는 모드 적재를 실패시킵니다.
- **세이브를 불러오면 전에 받아 둔 무장·도시는 쓸 수 없습니다.** 전역 변수에 담아 두었다면 다시 얻으십시오.
- **파일·모듈에는 닿지 않습니다.** 열려 있는 것은 기본 함수, `string`, `table`, `math`, `bit`, `coroutine` 뿐입니다. `io`, `os`, `require`, `ffi`, `debug`, `load`, `loadstring`, `dofile` 은 없습니다.
- 매 프레임 불리는 함수는 없습니다. 사건이 올 때만 돌므로 평소 CPU를 쓰지 않습니다(2026-11-07 ETW 측정에서 `lua51.dll` 0%).
- `main.lua` 를 고치면 게임을 다시 켜야 합니다.

### 확인 상태

메뉴, 무장 값, 게임 적재는 게임에서 확인했습니다. 도시 값, 사건, 오류 처리, 막힌 라이브러리는 실제 LuaJIT 위의 모의 시험(`tests\scripting\LuaChecks.cs`)으로 확인했습니다. 전투 효과는 게임 안에서 아직 확인하지 않았습니다.

## 2. C# 모드

### 폴더

```text
<게임 폴더>\scripts\mods\my.hello\
  mod.json
  My.Hello.dll        ← 빌드 결과
  assets\             ← 선택. 모드가 읽는 자료
```

`mod.json` 은 Lua와 같고 `entry` 를 내 DLL, `type` 을 내 클래스의 전체 이름(예 `"My.Hello.HelloMod"`)으로 씁니다.

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

저장소 안에서는 `ProjectReference` 로 `S8.Scripting.csproj` 를 겁니다(`S8.ManagerMod.csproj` 참고). VS Code 확장(`tools\vscode-s8-mod`)이 이 뼈대를 만들어 줍니다.

### 게임 값 바꾸기

```csharp
using S8.Scripting;

namespace My.Gold;

public sealed class GoldMod : ScriptMod
{
    protected override void OnLoad()
    {
        AddMenu(MenuSurface.InGameEdit, "금 지급", () => World.PlayerOfficer.Gold += 1000);

        World.MonthChanged += month => { /* month.Current.Year, month.Current.Month */ };

        Effects.Register(new("my.heal", "턴 종료 회복", "부대 병력을 회복합니다."),
            onTurnEnd: trait =>
            {
                trait.Show("치료하라!");
                trait.Unit.RestoreSoldiers(100);
            });
        Effects.Bind(254, "my.heal");   // 기재 254번을 가진 무장의 부대에 적용
    }
}
```

- `World.PlayerOfficer`, `World.Officer(번호)`, `World.City(번호)` 는 번호와 세션만 든 값입니다. `Gold`, `Intelligence`, `Grain` 같은 속성으로 읽고 씁니다. 세이브를 불러온 뒤 옛 값을 쓰면 `EngineCallException`(`StaleSession`)이 납니다. 읽기 전용 값(`TechnologyLimit`)은 설정자가 없습니다.
- 속성 설정은 게임이 실제로 적용한 값을 돌려주지 않습니다. 상한에 걸렸는지 알아야 하면 다시 읽거나 컴포넌트의 `Set`·`Add`(적용값 반환)를 쓰십시오. 훈련·기재 칸도 컴포넌트로 다룹니다(`docs\design\Script_Entity_Data_API.md`).
- 전투 효과는 `Effects.Register` 로 등록하고 `Effects.Bind(기재, 효과)` 로 묶으면 SDK가 실행합니다. 등록하지 않은 효과를 묶으면 그 자리에서 `ArgumentException` 이 납니다. 사용자가 고치는 파일에서 묶음을 읽으려면 `Effects.UseBindings(기재 => 효과목록)` 을 씁니다(`S8.GameScripts\ScriptedTraits.cs`).

### 창 띄우기

```csharp
public sealed class HelloMod : ScriptMod
{
    private UiScene? scene;

    protected override void OnLoad() =>
        AddMenu(MenuSurface.TitleEdit, "인사", Open);

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

창은 `UiScene` 이 만들고, 게임 창이 모두 닫힐 때(`OnUiClosed`) 모드가 정리합니다.

## 3. mod.json

| 항목 | 뜻 |
|---|---|
| `id` | 모드 식별자. 켜기·끄기, 의존·충돌 선언이 이 값을 씁니다 |
| `name` | 환경설정 → 모드 탭에 보이는 이름 |
| `version` | `x.y.z`. 같은 `id` 가 여럿이면 높은 판, 같으면 JIT 판이 적용됩니다 |
| `apiVersion` | 요구 엔진 API. 매니저와 다르면 건너뜁니다. 지금은 **21** |
| `runtime` | `jit` 또는 `aot` |
| `entry` | 모드 DLL 파일 이름 |
| `type` | `ScriptMod` 를 상속한 클래스의 전체 이름(JIT만) |
| `gameBuild` | 선택. 게임 실행 파일 빌드 값, 16진수 문자열(예 `"69A67ED1"`). 다르면 건너뜁니다 |
| `dependencies` | 선택. `{"다른 모드 id": "최소 판"}`. 필요한 모드가 먼저 불립니다 |
| `conflicts` | 선택. 같이 켤 수 없는 모드 `id` 배열. `mods.json` 순서가 앞선 쪽만 적용됩니다 |

새 모드는 게임이 다음에 모드를 훑을 때 `scripts\mods.json` 끝에 켜진 상태로 덧붙습니다.

## 4. 모드의 수명 (C#)

| 시점 | 불리는 곳 | 할 일 |
|---|---|---|
| 처음 불릴 때 | `OnLoad()` | 메뉴·창 행·환경설정 탭·사건·효과 등록, 자산 읽기. 등록은 불러오기 사건(`Open`·`Configure`) 안에서만 되고 `OnLoad` 가 그 안에서 돕니다. 탭을 새로 그리려면 `OnEvent` 의 `Configure` 에서 다시 등록합니다 |
| 메뉴·행을 누름 | 등록한 콜백 | 대화창 열기, 값 바꾸기 |
| 대화창 사건 | `DialogComponent` 의 콜백 | 선택·클릭·닫힘·입력 완료 |
| 게임 창이 모두 닫힘 | `OnUiClosed()` | 열 때 만든 `UiScene` 정리 |
| 전투 사건 | 묶인 전투 효과, 그다음 `OnEvent` | |
| 그 밖의 사건 | `OnEvent(kind, owner, value)` | 앞 단계가 가져가지 않은 사건만 옵니다 |
| 모드 관리에서 꺼짐 | `OnDisabled()` 다음 `OnUnload()` | 게임에 바꿔 둔 것을 되돌리기 |
| 교체·종료 | `OnUnload()` | 자원 정리 |

사건은 아래 차례로 배분되고, **앞 단계가 가져가면 `OnEvent` 까지 오지 않습니다.**

| 차례 | 받는 곳 | 가져가는 사건 |
|---|---|---|
| 0 | `World` 세션 갱신(모든 사건 직전) | 없음. 바뀐 것이 있으면 `SessionStarted`·`SessionEnded`·`SceneEntered`·`SceneExited`·`ProtagonistChanged` 를 올립니다 |
| 1 | 무장·도시 처리기와 `World` 이벤트 | `MonthChanged`, `CouncilEntered`, `DomesticActivityCompleted`, `TalkCompleted`. 세계 사건 실행기로도 갑니다 |
| 2 | `GameSetup.Handle` | 설정 클래스가 `true` 를 돌린 사건 |
| 3 | `World.Events` | `CouncilRoomReady`, 대기 중인 후속 화면이 있을 때의 `Resume` |
| 4 | `ScriptMod` 자체 | `OptionChanged`, `MenuClick`·`WindowClick`·`WindowClosing`, `Closed`. 전투 턴 사건은 효과를 돌린 뒤 `OnEvent` 로, `BattleSessionEnded` 는 전투를 정리한 뒤 `OnEvent` 와 세계 사건으로 |
| 5 | 대화창을 만든 `UiScene` | 그 대화창의 `Select`·`Click`·`Closing`·`Resume`·`TextInput`·`NumberInput`·`TreeClick`·`TabChanged` |
| 6 | `OnEvent` | 남은 모든 사건 |

그 밖의 수명 지점:

- `Component.OnEnable` / `OnDisable`: 컴포넌트를 붙일 때, 떼거나 객체를 버릴 때.
- `GameSetup.Configure`: 모드 어셈블리의 이 형식을 반사로 찾아 `OnLoad` 다음에 한 번(`Order` 순). `Dispose` 는 모드가 내려갈 때.
- `WorldSetup.Configure`: 같은 방식으로 찾되 **게임 세션이 시작될 때마다** 새로 만들어 부릅니다. `Dispose` 는 세션이 끝날 때.

알아 둘 규칙:

- **모든 콜백은 게임 스레드에서 동기로 돕니다.** 오래 걸리면 화면이 멈춥니다. 매 프레임 갱신, 세이브 저장·불러오기 직전 알림, 게임 종료 알림은 없습니다.
- 세션 시작·끝은 게임이 바로 알려 주지 않습니다. **다음 사건이 왔을 때** 세션 값을 다시 읽고 바뀐 것을 알아챕니다.
- 입력 완료·닫힘 콜백 안에서는 새 창을 열거나 닫을 수 없습니다. 후속 화면은 `Resumed` 콜백에서 엽니다.
- **켜고 끄기는 재시작 없이 바로 적용됩니다.** 꺼지면 `OnDisabled` 에서 게임을 되돌려야 합니다(예: `S8.GameScripts\ModApplication.cs` 는 기재 목록과 보주 강제 적용을 비웁니다). 다시 켜지면 새 인스턴스로 `OnLoad` 부터 시작합니다.
- 저장하지 않은 편집이 있으면 `CanReload` 를 `false` 로 돌려 실행 중 교체를 미룹니다.

## 5. 개발 중 바로 고쳐 보기 (C# JIT)

**1 → 소스 폴더 방식(권장).** 모드 폴더에 `source\` 를 두고 `.cs` 파일을 넣으면 매니저가 직접 컴파일합니다. 메서드 본문만 고치면 **다음 사건 때 실행 중인 코드에 반영**됩니다. 멤버 추가·삭제나 서명 변경은 다음에 메뉴를 열 때 모듈 전체를 바꿉니다. 컴파일이 안 되면 기존 코드를 그대로 둡니다. 이 방식은 .NET 기본 어셈블리와 `S8.Scripting` 만 참조하므로 NuGet 꾸러미를 쓸 수 없습니다.

**2 → DLL 교체 방식.** `source\` 가 없으면 DLL이나 `mod.json` 의 수정 시각을 보고, 다음에 메뉴를 열 때 모듈을 바꿉니다. 빌드 결과의 `deps.json` 과 의존 DLL을 같이 두면 NuGet 꾸러미도 불립니다.

두 방식 모두 `S8.Scripting.dll` 이나 매니저 자체가 바뀌면 게임을 다시 켜야 합니다. 진단 출력(`[S8 live] ...`)은 디버그 출력 창(DebugView 등)에 남습니다.

## 6. 배포판 (C# AOT)

AOT 판은 .NET 런타임 없이 네이티브 DLL로 불립니다. JIT 판과 함께 둘 수 있고, 판이 같으면 JIT 쪽이 이깁니다.

- `mod.json`: `"runtime": "aot"`, `type` 없음.
- 내보낼 함수 셋. `S8.GameScripts\AotExports.cs` 를 본뜨면 됩니다.
  ```csharp
  [UnmanagedCallersOnly(EntryPoint = "S8ModLoad")]     → AotModHost<내모드>.Load
  [UnmanagedCallersOnly(EntryPoint = "S8ModDispatch")] → AotModHost<내모드>.Dispatch
  [UnmanagedCallersOnly(EntryPoint = "S8ModUnload")]   → AotModHost<내모드>.Unload
  ```
- **`S8ModUnload` 가 없으면 실행 중에 끌 수 없습니다.** 모드 관리에서 꺼도 "다음 실행부터 빠집니다"로 표시됩니다.
- 빌드: `dotnet publish <프로젝트> -c Release -r win-x64 -p:PublishAot=true -o <게임>\scripts\mods\<폴더>`
- 반사로 찾는 형식은 잘라 내기에서 지워질 수 있습니다. `GameSetup`·`WorldSetup` 을 쓰면 `TrimmerRootAssembly` 로 붙잡아 두십시오(`S8.GameScripts.csproj` 참고).
- Lua 모드는 AOT 판이 없습니다.

## 7. C#으로 할 수 있는 것

"확인"은 게임에서 동작을 본 것, "시험판"은 도구에 있지만 0.0.1 배포판에서 빼 둔 기능입니다.

### 게임 화면

| 기능 | 도구 | 상태 |
|---|---|---|
| 메뉴 항목 | `AddMenu(MenuSurface.TitleEdit / InGameEdit / Function / Palace, 이름, 콜백)` | 시작 화면·게임 안 편집 메뉴 확인 |
| 게임 창에 행 추가 | `AddWindowRow(칸, 이름, 단추, 클릭, 닫힘)`. 닫힘은 결정/취소 여부를 받음 | 주인공 편집의 보주 행 확인 |
| 대화창 | `UiScene.Create(...).Show(DialogKind, 제목, 행들, ...)`. 목록·양식·글·알림·다중 선택·보주 나무·도시 지도 | 목록·양식·다중 선택·보주 나무 확인 |
| 양식 탭, 행 끄기, 행 그림 | `Show(..., tabs:)`, `UiRow.Enabled`, `ApplyRowSprite`, `SetRowBadge` | 기재 편집에서 사용 |
| 글자·숫자 입력 | `EditText(초기값, 최대 글자, 여러 줄)`, `EditNumber(초기, 최소, 최대)` | 확인. 자동 한글 입력은 화면에 반영되지 않음 |
| 환경설정 탭 | `SetOptionsTab(이름, OptionRow 목록, 바뀜 콜백)`. 모드당 하나, 줄 1~64개, 줄마다 선택지 1~16개 | 확인(모드 관리 탭) |
| 모드 목록 읽기·켜고 끄기 | `ModCatalog.Read()`, `ModCatalog.SetEnabled(id, 켬)` | 확인 |

### 게임 자료

| 기능 | 도구 | 상태 |
|---|---|---|
| 기재 이름·설명·효과·음성·등급 | `TraitCatalog.Publish(TraitDefinition?[])`. 빈 칸은 게임 원래 값 | 확인 |
| 기재 레코드 읽기 | `PersonalityComponent` | 확인 |
| 보주 개방·강제 적용 | `JewelComponent`, `PlayerJewelsComponent`, `JewelComponent.RestoreForced` | 확인 |
| 무장 능력치·금·행동력·명성, 도시 값 | `Officer`·`City` 값(`World.PlayerOfficer`, `World.Officer`, `World.City`) | 금·능력치는 Lua 예제로 게임 확인, 나머지는 모의 시험 |
| 훈련·기재 칸 | `OfficerTrainingComponent`, `OfficerTraitsComponent` | 도구에 있음. 동작 보증 안 함 |
| 소속(무장·도시) | `OfficerAffiliationComponent`, `CityAffiliationComponent` | 위와 같음 |
| 세계·장면·주인공 | `World`(`GameWorld`) | 위와 같음 |
| 세이브에 모드 자료 붙이기 | `World.SaveData` | 위와 같음 |

### 사건

| 사건 | 상태 |
|---|---|
| 전투 턴 시작 / 부대 행동 끝 / 전체 행동 끝 / 전투 끝 | 시험판. 전체 행동 끝에서 병력 회복과 기재 발동 연출 확인 |
| 내정 완료(개간·상업·성문 보수·치안·기술, 플레이어 무장만), 대화 완료, 월 변경, 평정 진입 | 시험판. 내정 완료 다섯 종류는 2026-11-07 실측 |
| 스크립트 세계 사건(`WorldEvent`, `EventConditions`, `EventEffects`) | 시험판 |
| 출진 요청, 평정 전략(`MarchRequest`, `CouncilStrategyComponent`) | 시험판 |

사건은 `World.MonthChanged += ...` 처럼 람다로 받는 것이 기본입니다. 처리기 인터페이스(`IMonthChangedHandler`, `IBattleTurnEndEffect` 등)는 무장·도시마다 상태를 오래 붙여 둘 때 씁니다. 시험판 예시는 `S8.GameScripts` 의 `S8_EXPERIMENTAL` 구역과 `docs\design\` 에 있습니다.

### 그 밖에

- **메모리 읽기·쓰기**: `MemoryAccess.Read/Write`, `MemoryComponent`. 세션이 맞는 주소만, 한 번에 64KiB까지, 실행 코드 쪽 쓰기는 거부합니다.
- **모드 파일**: `Context.ModDirectory` 에 쓰고 `Context.AssetDirectory`(`assets\`)에서 읽습니다. `Context.GameDirectory` 는 읽기만 합니다.

## 8. 할 수 없는 것

- **게임 함수를 직접 부를 수 없습니다.** 주소나 가상 함수 호출은 모두 네이티브 DLL 안에 있습니다. 새 기능이 필요하면 네이티브 쪽에 연산을 추가하고 `apiVersion` 을 올려야 합니다.
- **임의 스레드나 콜백 밖에서 게임을 만질 수 없습니다.** 콜백 밖이면 `InvalidOperationException`, 지난 세션의 값을 쓰면 `EngineCallException`(`StaleSession` 등)이 납니다.
- **환경설정 탭은 시작 화면에서만 보입니다.**
- **다른 모드의 사건을 가로챌 수 없습니다.** 탭·메뉴 사건은 등록한 모드에게만 갑니다.
- **게임 빌드가 다르면 아무것도 붙지 않습니다.** 매니저가 실행 파일과 코드 바이트를 검사해 맞지 않으면 기능을 모두 끕니다.

## 9. 알려진 제약

- 옵션 탭 글꼴에는 가운뎃점이 없습니다. `·` 는 점 두 개로, `・` 와 `U+E060` 은 빈칸으로 나옵니다.
- 같은 `id` 의 JIT 판과 AOT 판은 모드 탭에 두 줄로 보이지만 켜기·끄기를 함께 씁니다. 한 줄을 바꾸면 다른 줄은 탭을 다시 열어야 새 값이 보입니다.
- 모드를 건너뛴 이유는 모드 탭 설명 칸과 디버그 출력에 나옵니다. `scripts\mod_manager.log` 에는 후킹 실패만 남습니다.
- 두 모드가 같은 기재에 전투 효과를 묶으면 둘 다 발동합니다.

## 10. 참고할 코드

| 보고 싶은 것 | 파일 |
|---|---|
| Lua 예제 모드 | `samples\lua-effect\main.lua` |
| Lua 연결 층 | `src\scripting\managed\S8.LuaHost\LuaMod.cs`, `Prelude.cs` |
| 가장 작은 C# 모드(환경설정 탭, 모드 목록) | `src\scripting\managed\S8.ManagerMod\ManagerMod.cs` |
| 메뉴·창 행·끄기 처리, 전투 효과 등록 | `src\scripting\managed\S8.GameScripts\ModApplication.cs` |
| 목록·양식·입력 대화창 | `src\scripting\managed\S8.GameScripts\TraitEditor.cs` |
| 보주 나무·다중 선택 | `src\scripting\managed\S8.GameScripts\JewelEditor.cs` |
| 기반 클래스와 사건 배분 | `src\scripting\managed\S8.Scripting\ScriptMod.cs` |
| 무장·도시 값, 전투 효과 실행 | `src\scripting\managed\S8.Scripting\Handles.cs`, `BattleEffects.cs` |
| 사건 번호 전체 | `src\scripting\managed\S8.Scripting\Native.cs` 의 `ScriptEvent` |
| 시험 묶음(가짜 엔진으로 모드 돌리기) | `tests\scripting\Program.cs`, `LuaChecks.cs`, `OptionsChecks.cs` |
