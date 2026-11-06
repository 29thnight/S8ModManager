# 스크립트용 장수·도시·보주 API

소속·신분·Rank·태수 조회와 이동/임명 분석 현황은 [Script_Affiliation_API.md](Script_Affiliation_API.md)를 참고한다.

## 접근

`GetOfficer/GetCity`가 처음 엔티티를 연결할 때 아래 표준 컴포넌트를 함께 추가한다.
재조회는 기존 GameObject와 컴포넌트를 유지한다. 조회·변경 요청마다 ID와 세션을 네이티브 입구에서
확인하여 실제 레코드를 다시 찾는다. C#이 전달한 임의 포인터나 함수 주소를 실행하지 않는다.
주기적인 전체 장수·도시 스캔은 추가하지 않았다.

```csharp
var officer = world.GetOfficer(officerId);
var stats = officer.GetComponent<OfficerStatsComponent>()!;
var fame = officer.GetComponent<OfficerFameComponent>()!;
var training = officer.GetComponent<OfficerTrainingComponent>()!;
var traits = officer.GetComponent<OfficerTraitsComponent>()!;

byte applied = stats.Set(OfficerStat.Intelligence, 95);
ushort civil = fame.Set(OfficerFame.Civil, 1000);
training.SetTactic(1, TrainingLevel.Three);
training.SetSkill(1, TrainingLevel.Two);
traits.SetSlot(2, 254);

var city = world.GetCity(cityId).GetComponent<CityAdministrationComponent>()!;
uint gold = city.Add(CityValue.Gold, 5000);
city.Set(CityValue.Development, 800);
```

이벤트의 `CanExecute/Execute`에서도 같은 컴포넌트를 사용할 수 있다.
기재 ID 254는 사용 예일 뿐이며 원하는 기재의 실제 ID를 지정한다.

## 범위와 게임 동작

| 컴포넌트 | 조회/변경 | 게임의 처리 |
| --- | --- | --- |
| OfficerStatsComponent | 통솔·무력·지력·정치·매력 Get/Set | 기본 능력치 byte. setter 자체의 추가 상한 없음. 장비·상태를 합산한 전투 능력치가 아님 |
| OfficerStatsComponent | Gold/SetGold/AddGold | 절대 setter에는 상한 없음. native delta의 양수 경로는 99999 제한. 게임의 32비트 산술을 그대로 사용 |
| OfficerStatsComponent | ActionPoints/SetActionPoints | 게임이 계산한 행동력 상한으로 제한 |
| OfficerFameComponent | Civil·Martial·Infamy Get/Set | 문명·무명·악명. 게임 설정의 명성 상한으로 제한 |
| OfficerTrainingComponent | 전법 1..35, 특기 1..24 Get/Set | 레벨 0..3. UI 행 번호가 아닌 1부터 시작하는 ID. 특기 setter는 +0x374 캐시를 -1로 갱신 |
| OfficerTraitsComponent | 기재 슬롯 0..2 GetSlot/SetSlot | 기재 ID 1..254의 실제 레코드 포인터를 게임 setter로 대입. 0은 해제. +0x374 캐시 갱신 |
| CityAdministrationComponent | Gold·Grain·Troops Get/Set/Add | 절대값 및 증감은 게임의 999999 상한 사용. 병력 setter가 도시 병사 한도로 추가 제한하지는 않음 |
| CityAdministrationComponent | Development·Commerce·Defense·Technology Get/Set | 현재값은 도시의 해당 한도로 제한 |
| CityAdministrationComponent | DevelopmentLimit·CommerceLimit·DefenseLimit Get/Set | 9999 상한. 한도를 낮출 때 현재값까지 동시에 재조정하는 처리는 추가하지 않음 |
| CityAdministrationComponent | TechnologyLimit Get | setter 미확인. Set 요청은 네이티브 입구에서 거절 |
| CityAdministrationComponent | TroopLimit Get/Set | setter 상한 999999 |
| CityAdministrationComponent | Revolt Get/Set | byte 그대로 저장. 반란 이벤트를 실행시키는 API가 아님 |

모든 Set/Add는 적용 후 게임 getter로 읽은 값을 반환한다. 입력값이 그대로 반영됐다고 가정하지 않는다.
City Add는 금·군량·병사에만 제공한다. 지원하지 않는 enum/ID/범위/연산 조합은 네이티브 입구에서 거절한다.
잘못된 요청을 임의의 다른 필드나 메모리 쓰기로 우회하지 않는다.

이들은 게임 데이터 getter/setter 호출이다. 내정 임무·전법 발동·특기 연출·명성 획득 알림을 실행하는 API는 아니다.
저장 JSON을 변경하거나 편집 창을 자동으로 새로고침하지 않는다.
기재 슬롯 변경과 기재 정의(이름·설명·효과)의 편집은 별개다. 정의 조회는 기존 PersonalityComponent를 사용하며,
정의 편집기의 파일 저장/네이티브 적용 경로는 이번 확장에서 바꾸지 않았다.

## 플레이어

```csharp
var player = world.GetPlayer();
var character = player.Character; // world.GetOfficer(주인공 ID)와 동일한 객체

uint money = player.Resources.AddMoney(1000);
player.Resources.AddFriendshipTokens(3, showMessage: true);
var jewels = player.Jewels;
```

GetPlayer는 현재 세션의 동일 Player를 반환한다. 주인공 변경 시 Player.Character와 Jewels가
새 장수로 전환되며 일반 장수 객체는 유지한다. 이전 보주 컴포넌트는 해제되어 다시 같은 장수가
주인공이 되어도 사용할 수 없다. 다른 세션으로 로드하면 이전 Player/Resources 참조도 만료된다.
월드의 직접 Jewels 속성과 WorldServices 객체는 제거했다.

Money는 별도 잔액이 아니라 현재 주인공 장수의 소지금 API를 조합한다.
우호의 상징 증감은 새 연산 AddFriendshipTokens(36)에서 실제 게임 함수 1727130을 호출한다.
네이티브 입구에서 세션과 현재 주인공 ID를 확인한다. 게임 함수는 0..65535로 제한하고,
시나리오가 유효하지 않거나 증감이 0이면 무시할 수 있다. 반환은 void이며 반영 성공값을 만들어내지 않는다.
우호의 상징 개수 getter는 이번에 추가하지 않았다. 1727130의 진입 시그니처도 초기 검사에 추가했다.

## 보주

보주는 현재 플레이어의 전용 기능이므로 `world.GetPlayer().Jewels`로 제공한다.
PlayerJewelsComponent는 현재 주인공 GameObject에만 자동으로 붙는다.
세션이 끝난 뒤 보관 중인 이전 JewelComponent를 사용하면 거절한다.
기존 `JewelComponent.Read/Submit`을 유지하며 보주 정의와 단건/일괄 변경 API를 추가했다.

```csharp
// 모드 매니저가 정의 파일을 한 번 로드해 월드에 전달한다.
var jewels = world.GetPlayer().Jewels;

bool opened = jewels.IsOpened(2);
bool forced = jewels.IsForced(2);

// 같은 콜백에서 여러 보주를 바꿀 때는 Apply 한 번으로 요청한다.
jewels.Enable(2); // 단건 요청 예시

// 여러 변경은 위 단건 요청과 별도로, 다음 적용 가능 콜백에서 한 번에 제출한다.
jewels.Apply([
    new JewelChange(2, Opened: true, Forced: true),
    new JewelChange(3, Opened: true)
]);
```

- `JewelDefinition.Id`는 UI/강제 적용 마스크의 ID, `RawId`는 게임 개방 비트의 ID다.
  예를 들어 ID 2의 RawId는 66이다. 두 숫자를 혼용하지 않는다.
- 정의 파일의 ID 범위·이름·중복 ID는 로드 입구에서 확인한다. 검증된 정의로 변경 명령을 만든다.
- `Enable(id)`는 개방, `Disable(id)`는 개방 및 강제 적용 해제, `ForceApply(id)`는 개방과 강제 적용을 한 요청으로 수행한다.
- C# 호출 인수는 JewelDefinition.Id다. C++ 개방 명령에 필요한 RawId는 C# 내부의 정의 조회로 변환한다.
- 개방되지 않은 보주의 강제 적용은 거절한다. 같은 Apply에서 먼저 개방하고 강제 적용할 수 있다.
- Submit은 기존 네이티브 명령 큐에 요청을 넣는다. 즉시 적용 완료를 뜻하지 않는다.
  적용 대기 중 Read/다음 편집은 거절된다. 다음 게임 콜백의 게시 상태에서 반영을 확인한다.
- 보주 변경은 기존 모드 매니저 명령 처리다. 새로운 게임 원본 setter를 발견했다고 주장하지 않는다.

## RE 연결 근거

이름은 SDK에서 붙인 것으로 게임의 원래 심볼 이름이 아니다.
기준 EXE와 타입/포인터 테이블은 기존 World entity lookup과 같다.

| 범위 | getter RVA | setter RVA |
| --- | --- | --- |
| 기본 능력치 | 17118D0 | 17118F0 |
| 장수 금 | 17117B0 | 1712620; delta 17103A0 |
| 행동력 | 1711880 | 1711840; 상한 의존 16F6330(기존 검사) |
| 문명 | 1711BA0 | 1711C00 |
| 무명 | 1711B90 | 1711BC0 |
| 악명 | 1711BB0 | 1711C40 |
| 명성 상한 의존 | 685840의 설정 객체 +26C0 | 세 setter가 동일 상한 참조 |
| 전법 | 1713280 | 17133C0 |
| 특기 | 1713150 | 17131E0 |
| 장수 기재 포인터 슬롯 | 1711920 | 1711940 |
| 도시 금·군량·병사 | 17122D0/17122E0/17122F0 | 1712700/1712720/1712740; delta 1710F00/1710F60/1710FC0 |
| 도시 개발·상업·방어 현재값 | 1712220/1712240/1712260 | 1712270/1712290/17122B0 |
| 도시 기술 현재값 | 1712320 | 17128B0 |
| 도시 개발·상업·방어 한도 | 1712210/1712230/1712250 | 17127A0/17127E0/1712820 |
| 도시 기술 한도 | 1712310 | 미확인 |
| 도시 병사 한도 | 170CF90 | 1703540 |
| 도시 반란 | 170FBF0 | 1703590 |

추가한 함수 진입 48곳의 16바이트 시그니처를 기존 초기 연결 검사에 포함했다.
요청마다 함수 본문을 검사하지 않는다. 장수/도시의 현재 세션·엔티티 확인은 네이티브 요청 입구에서 수행한다.
새 계약 연산은 ReadEntityValue(33), SetEntityValue(34), AddEntityValue(35), 요청 POD는 24바이트다.

## 검증 상태

### 장수 행동 가능 여부 조회

`GetOfficer()`로 구성되는 GameObject에 `OfficerActionsComponent`를 기본 추가한다.
게임 콜백 안에서 다음처럼 조회한다.

```csharp
var officer = world.GetOfficer(officerId)!;
var actions = officer.GetComponent<OfficerActionsComponent>()!;
var availability = actions.CheckAvailability(actionId);

if (!availability.CanAct)
    Console.WriteLine(availability.Reason);
```

`actionId`는 게임의 행동 데이터 ID 1..100이다. 메뉴 행 번호가 아니다.
조회는 REA에서 확인한 `0x17A8660`을 호출하고 역할 허용, 금, 행동력을 검사한다.
대상 도시·상대 장수 등 개별 행동의 추가 자격까지 검사하는 API는 아니다.
SDK 이름은 원래 게임 심볼/enum 이름이 아닌 분석 결과에 붙인 명칭이다.

- `Status`: Unavailable(1), InsufficientResources(2), Available(3).
- `Reason`: Allowed(1), RoleNotAllowed(2), PlayerRestriction(3), InsufficientGold(4), InsufficientActionPoints(5).
- 게임 함수의 설명 생성 억제 인수를 사용한다. 행동 실행이나 설명 UI를 요청하지 않는다.
- 전달값은 장수 ID, 세션, 행동 ID이며 장수 포인터를 C#에서 받지 않는다.
- 네이티브 입구에서 현재 장수와 행동/역할 데이터를 확인한다. 이전 세션은 기존 World resolver가 거절한다.
- 기존 enum 번호를 유지하고 `QueryOfficerActionAvailability`를 끝에 추가했다. 요청 POD는 16바이트다.
- 초기 게임 연결 검사에 조회 함수와 금/행동력 요구량 함수의 진입 시그니처 3개를 추가했다.
- 이 추가분은 빌드·테스트·게임 적용 전이다.

### 도시 AI 목표 선택

도시 GameObject에 `CityAiComponent`를 기본 추가한다.

```csharp
var ai = world.GetCity(cityId)!.GetComponent<CityAiComponent>()!;
var target = ai.SelectAttackTarget();

// 기본 정책 대신 스크립트가 직접 선택할 수도 있다.
var candidates = ai.GetAttackCandidates();
```

- C++은 현재 도시의 인접 연결 최대 6개만 읽는다. 배경 수집이나 전체 도시 순회는 없다.
- `0x17121D0`으로 소속을 구하고 `0x1461CD0`으로 관계·전략 금지 대상 조건을 검사한다.
- `0x17A4070`에 대상 도시의 방어 세력과 0으로 초기화한 보정 자료를 전달해 평가값을 구한다.
  이는 `0x1459B70`에서 확인한 평가 호출 형식이다.
- 네이티브는 후보 ID·평가값만 반환한다. 최저 평가값을 고르는 기본 선택 정책은 C#에 있다.
- 결과의 Target은 같은 세션에서 `world.GetCity()`로 조회하는 정규 GameObject다.
- 이것은 인접 목표 선택 정책이다. 세력 전체의 확률 판단·출진 시기·동원 가능 장수·부대 편성을
  모두 재현하는 API가 아니며, 선택만으로 공격을 실행하지 않는다.
- 새 연산 `ReadCityAttackCandidates`의 POD는 64바이트다.

### 게임 내정 행동 실행 요청

```csharp
var actions = world.GetOfficer(officerId)!.GetComponent<OfficerActionsComponent>()!;
var result = actions.RequestDomesticActivity((byte)DomesticActivityKind.GateRepair);
```

- 허용 ID는 원본 실행 함수가 받는 1, 11, 21, 31, 41이다. 21은 실측한 성문 보수다.
- `0x17A8660`의 장수 조건 뒤 `0x17A8930`의 현재 도시 조건을 검사한다.
  도시 조건의 원래 정수 두 개는 TargetCondition/TargetReason으로 보존한다.
- 두 검사를 통과하면 `0x17A8B10(도시, 행동 ID, 장수, 빈 참여자 목록, null, 빈 콜백, 빈 콜백)`을 호출한다.
  비용·결과·조건부 연출은 원래 게임 경로가 처리한다. 추가 참여자나 C# 콜백은 전달하지 않는다.
- 빈 참여자 토큰은 REA `0x1AD90`에서 확인한 빈 목록 경로이며,
  빈 콜백은 원본에서 검사하는 +0x38 구현 포인터가 null인 64바이트 자료다.
- 호출은 기존 게임 콜백에서 동기로 처리한다. 요청 큐나 비동기 완료 통지는 추가하지 않았다.
- Status는 Invoked(1), ActionUnavailable(2), TargetUnavailable(3)이다.
  Invoked는 원본 실행 함수가 반환했다는 뜻이며 특정 수치 증가를 보장하는 결과명이 아니다.
- 새 연산 `RequestDomesticActivity`의 POD는 32바이트다. 기존 연산 번호는 유지한다.
- 출진은 아래의 별도 요청 API로 처리한다. 내정 실행 연산에 출진 ID를 끼워 넣지 않는다.
- 이번 두 API 추가분은 빌드·테스트·게임 적용 전이다.

### 실제 도시 출진 요청

```csharp
// 월 변경/평정 진입 등 게임 콜백에서 요청한다.
var ai = world.GetCity(sourceCityId)!.GetComponent<CityAiComponent>()!;
var target = ai.SelectAttackTarget();
MarchRequest? march = target is null ? null : ai.RequestMarch(target.Target);

// 핸들을 컴포넌트 필드에 보관하고 다음 게임 콜백에서 확인한다.
var result = march?.GetResult();
```

`RequestMarch()`는 선택한 목표를 바꾸지 않고 출발 도시·도착 도시의 ID와 세션을 네이티브에 전달한다.
접수 결과는 출진 완료 결과가 아니다. 다음 처리 경계는 기존에 계측된 보수 완료, 담화 완료,
월 변경, 평정 진입이다. 같은 콜백에서 접수했다면 그 관리 코드가 반환한 직후 처리한다.
다른 UI 콜백에서 접수했다면 다음 처리 경계까지 Queued로 남는다.

#### REA에서 확인한 호출·수명 계약

- 기준은 SHA256 `f47c1112dfa1fa4067fde43ad2e4fd906bbba341d47fda4f6ef1724be4df43c1`의
  복호화 EXE, REA 6.8.0 / Ghidra 12.1.4다. 아래 역할명은 추정 이름이다.
- `0x145A6B0`은 원본 도시 AI의 시도→편성→실행→정리를 함께 수행한다.
  어셈블리에서 출발 CityData는 RDX, 목표 CityData는 R8이며 첫 인수 RCX는 사용하지 않는다.
- `0x1461BA0`의 실행 장수 검사, `0x1461CD0`의 목표 조건 검사 뒤
  `0xEA4A0`으로 자료를 초기화한다. 편성 본체는 자료 시작 +0x18이며
  `0x1899D10`, `0x1899E30`, `0x189B9D0`로 출발 도시·목표·모드를 설정/검사한다.
- `0x1462CC0`이 원본 규칙으로 장수·병종·병력·장비를 자동 편성한다.
  원본 래퍼는 병량·전력·전술 및 확률 조건을 추가로 검사하며 거절할 수 있다.
- 래퍼의 `0x194A050` 호출이 실제 실행 경로다. 전투·연출과 결과 처리 분기를 포함한다.
  호출 뒤 전략 분류 갱신과 편성 자료 해제가 이어진다.
- 편성에는 0x48바이트 원소 10개가 있다. `0x189D0D0` 및 래퍼의 해제 경로가
  원본 원소 소멸자를 호출한다. 브리지는 이를 직접 배치하거나 C#에 노출하지 않는다.
- `0x145A6B0`의 원본 호출자는 일반 도시 AI `0x145BA10`과 연결되는 경로를 포함하며,
  다른 `0x194A050` 호출자에는 방랑군 경로 `0x1459B70`, 별도 명령 `0x195D2E0/0x195DB40`도 있다.
  이 API는 방랑군용 자료를 일반 도시용으로 재해석하지 않는다.
- 조회와 달리 실행은 관리 호출 스택을 보존한 채 들어가지 않는다.
  `dispatch_personal_activity()`에서 S8ScriptDispatch 반환과 계측 로그 기록을 마친 뒤
  네이티브 큐를 한 건 처리한다. 게임 함수가 반환할 때까지 브리지 mutex를 잡지 않는다.

#### 요청·결과 관리

- 대기/실행/완료 영수증은 세션에 속한다. 게임 세션 게시값이 바뀌면 이전 영수증도 제거한다.
- 큐와 미회수 결과를 합쳐 최대 64건이다. 완료 결과를 읽으면 네이티브 영수증을 회수하고
  같은 C# 핸들의 이후 조회는 캐시된 완료 결과를 반환한다. 미완료 상태는 캐시하지 않는다.
- 전투가 추적 중이거나 기존 네이티브 변경이 적용 대기 중이면 실행을 뒤로 미룬다.
  한 경계에서 최대 한 건만 실행하고, 실행 중 재진입한 콜백은 추가 실행을 시작하지 않는다.
- 실행 직전에 출발·목표 도시를 ID로 다시 resolve한다.
  인접 여부와 소속을 확인한 다음 원본 도시 AI 함수를 호출한다. 장수·병종 편성은 원본 함수가 처리한다.
- Queued(1), Executing(2), Invoked(3), RejectedByGame(4), DestinationNotAdjacent(5),
  NoFaction(6), InvalidEntity(7)를 제공한다. 이전 세션의 미회수 결과는 StaleSession으로 거절한다.
- Invoked는 원본 래퍼가 1을 반환한 상태다. 이 경로는 `0x194A050`을 호출한 뒤 1을 반환하지만,
  전투 승리·도시 점령·특정 부대 생성의 증명은 아니다. 원본의 입력 대기·취소도 게임이 처리한다.
- 원본이 편성/군량/전술/확률 조건에서 0을 반환하면 RejectedByGame이며,
  브리지가 이를 우회하거나 병력을 강제 편성하지 않는다.
- 새 연산은 RequestCityMarch/ReadCityMarchResult이며 두 POD는 각각 16바이트다.
  계약 번호는 끝에 추가해 기존 번호를 유지했다. 게임 연결 검사에는 주요 진입 시그니처 6개를 추가했다.
- 2026-11-05 Release 빌드·게임 적용 및 기존 ABI/컴포넌트 검사 140개 통과.
  평정 진입의 거절 결과 전달과 보수 완료 직후 실제 출진·점령 알림을 확인했다.
  세부 거절 분기와 시험 범위는 [Script_March_Validation.md](Script_March_Validation.md)에 기록했다.

### 기존 엔티티 API

컴포넌트 자동 구성·요청 대상/ID·적용값 반환·부호 있는 증감·기재 스냅샷 갱신·보주 ID 분리·
세션 교체 후 이전 컴포넌트 거절에 대한 테스트 소스를 추가했다.
이는 C# 계약 검사용 모의 네이티브 콜백이다. 실제 게임 함수 실행 검증과 구분한다.
이번 변경은 빌드·테스트 실행·게임 적용 전이다.
