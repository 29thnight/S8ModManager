# C# 스크립트 생명주기

현재 ABI v10 구현 기준이다. 게임의 함수 이름을 그대로 공개한 목록이 아니라, 네이티브 처리 경계에 연결한 SDK 계약이다. 실행 계측 근거는 [도시·내정 분석 기록](scripting-city-lifecycle-analysis-20261104.md)에 있다.

## 객체 수명과 초기설정

| 소유 범위 | 생성·설정 | 유지 기간 | 해제 |
| --- | --- | --- | --- |
| GameApplication | 모드 초기화 → GameSetup.Configure | 같은 모드 인스턴스 | 모드 해제·교체 |
| GameWorld | 새 세션 관측 → 주인공 연결 → WorldSetup.Configure → SessionStarted | 로드된 세션 | 다른 세이브 로드·세션 종료 관측 |
| 장수·도시 GameObject | world.GetOfficer / GetCity로 실제 엔티티 확인·연결 | 같은 세션에서 같은 ID는 동일 객체 | 세션 해제 |
| 주인공 | world.GetPlayer().Character | 해당 장수의 세션 객체와 동일 | 주인공 변경 시 전용 컴포넌트 분리, 세션 종료 시 객체 해제 |
| 전투 부대 GameObject | BattleWorld.Refresh에서 전투 씬에 생성 | 해당 전투에서 부대가 존재하는 동안 | 부대 제거·전투 씬 해제 |
| Component | AddComponent → 초기화 인수 적용 → OnEnable | 소유 객체에 붙어 있는 동안 | RemoveComponent / 객체 Dispose → OnDisable |

외부 스크립트에서 `new GameObject()`는 사용할 수 없다. 장수·도시는 월드에서 조회한다. 이벤트마다 AddComponent하지 않고 WorldSetup.Configure에서 붙이면 필드를 유지할 수 있다. 이 필드는 게임 세이브에 자동 저장되지 않으며 재로드하면 새로 구성된다.

세션 시작은 로드 버튼을 누른 순간이 아니라 관리 진입점의 RefreshSession이 새 세션을 관측하는 시점이다. 같은 세션의 RefreshSession은 Configure를 반복하지 않는다. 종료 시 WorldSetup.Dispose를 역순으로 호출하고, 월드 이벤트·플레이어·씬·장수·도시를 정리한 뒤 SessionEnded를 알린다. SessionEnded에서 이전 엔티티를 다시 사용하는 구조로 작성하지 않는다.

Component에는 현재 OnEnable/OnDisable만 있다. Awake, Start, 매 프레임 Update, FixedUpdate는 제공하지 않는다. GameSetup/WorldSetup의 자동 수집과 순서는 [초기설정 문서](Script_Setup_Lifecycle.md)를 따른다.

## 도시·달력 생명주기

| 인터페이스 / 메서드 | 호출 경계 | 컴포넌트 대상 | 전달 정보 |
| --- | --- | --- | --- |
| IMonthChangedHandler.OnMonthChanged | 월 진행 원본 함수 반환 후 날짜 변경 확인. 다음 화면 준비 전 | 이미 월드에 생성된 모든 장수·도시 객체 | 세션 ID, 이전·현재 연월 |
| ICouncilEnteredHandler.OnCouncilEntered | 진입 연출 종료 후 평정 명령 상태로 전환 완료 | 이미 월드에 생성된 모든 장수·도시 객체 | 세션 ID, 연월 |
| IDomesticActivityCompletedHandler.OnDomesticActivityCompleted | 보수 실행·대화·수치 연출 후 도시 명령 상태 복귀 | 실행 장수 객체 | 세션 ID, 장수, 활동 종류(개간·상업·보수·치안·기술) |
| ITalkCompletedHandler.OnTalkCompleted | 담화 및 도중 발생한 이벤트 종료 후 교류 명령 상태 복귀 | 실행 장수와 상대 장수 객체. 같은 객체면 한 번 | 세션 ID, 실행 장수, 상대 장수 |

순서는 해당 컴포넌트 핸들러 → GameWorld의 동명 이벤트 → WorldEvent 트리거 발행이다. 월변경·평정 진입은 호출 시작 시 대상 목록을 만든다. 그 호출 중 새로 붙인 컴포넌트는 다음 호출부터 받는다.

| GameWorld 이벤트 | WorldEventTriggers 상수 | 트리거 문자열 |
| --- | --- | --- |
| MonthChanged | MonthChanged | world.month.changed |
| CouncilEntered | CouncilEntered | council.entered |
| DomesticActivityCompleted | DomesticActivityCompleted | domestic.activity.completed |
| TalkCompleted | TalkCompleted | communicate.talk.completed |

월변경은 192년7→8→9→10월 날짜 전달과 인스턴스 유지, 휴양 취소 시 미발행을 실행 확인했다. 평정 진입은 포상·시작 대사 뒤 명령 화면 진입 1회, 메뉴 복귀 시 중복 없음까지 확인했다. 특수 kind0x3C 전환과 평정 세이브 직접 로드는 미검증이다. 내정 완료는 개간(1)·상업(11)·보수(21)·치안(31)·기술(41) 다섯 활동을 연결했다. 2026-11-07 실측에서 다섯 모두 같은 내정 상태 함수를 지나 도시 명령 상태로 돌아왔고, 확인창 취소는 사건을 내지 않았으며, 다른 무장의 내정은 이 상태를 지나지 않았다.

## 컴포넌트 작성 예

```csharp
using S8.Scripting;

public sealed class CalendarSetup : WorldSetup
{
    public override void Configure(GameWorld world)
    {
        world.GetPlayer().Character.AddComponent<CalendarComponent>();
    }
}

public sealed class CalendarComponent : Component,
    IMonthChangedHandler, ICouncilEnteredHandler
{
    public int ElapsedMonths { get; private set; }

    public GameDate LastCouncil { get; private set; }

    public void OnMonthChanged(MonthChangedInfo month)
    {
        ++ElapsedMonths;
    }

    public void OnCouncilEntered(CouncilEnteredInfo council)
    {
        LastCouncil = council.Date;
        // ElapsedMonths는 같은 세션에서 이전 호출의 값을 유지한다.
    }
}
```

도시 전용 로직은 Configure에서 `world.GetCity(cityId).AddComponent<...>()`로 붙인다. 월드 전체에 하나만 필요한 로직은 WorldSetup에서 world.MonthChanged 등을 구독하고 Dispose에서 해제하거나 WorldEvent를 붙인다. 모든 도시 객체가 자동 생성되지는 않는다.

## 전투 생명주기

| 인터페이스 / 메서드 | 의미 |
| --- | --- |
| IBattleTurnStartEffect.OnBattleTurnStarted | 다음 턴 시작 경계 |
| IBattleUnitActionEndEffect.OnBattleUnitActionEnded | 해당 개별 부대의 행동 정리 완료 |
| IBattleTurnEndEffect.OnBattleTurnEnded | 다음 행동 부대가 없는 전체 행동 종료 경계. 이후 건물 공격 등 턴 전환 처리·연출이 남을 수 있음 |

이 인터페이스들은 현재 ScriptedTraits의 효과 등록·기재 바인딩 경로가 호출한다. 일반 Component에 인터페이스만 붙여도 자동 호출되는 방식은 아니다. 월드 이벤트는 각각 battle.turn.started, battle.unit.action.ended, battle.turn.ended 트리거를 사용할 수 있다.

장수 GameObject는 세션 소유이며 전투 씬이 해제되어도 유지된다. BattleMembershipComponent와 부대 객체는 전투 범위다. 기재 효과 컴포넌트는 기재 바인딩을 유지하는 동안 재사용하며 바인딩 제거·세션 종료·모드 해제 때 정리한다.

BattleUnitComponent의 데이터는 전투 콜백에서 갱신되고 네이티브 작업 token은 해당 콜백 동안만 유효하다. 콜백이 끝난 후 비동기 작업에서 보관한 token으로 병력 변경·연출을 호출하지 않는다. 연출 중 다른 전투 이벤트가 중첩될 수 있고 런타임은 바깥 콜백의 token 문맥을 복원한다.

## 아직 제공하지 않는 시점

휴양 확인·취소·완료 전용 콜백, 도시 진입·외출·교류 시작·교류 종료·평정 종료 전용 컴포넌트 콜백은 아직 연결하지 않았다. 휴양으로 달이 바뀌면 현재는 OnMonthChanged를 받는다. 보수 이외 내정 활동을 보수 완료 이벤트와 같은 것으로 취급하지 않는다.

GameScene의 진입·종료 알림은 SDK에서 EnterScene/LeaveScene할 때 발생한다. 도시·평정의 실제 화면 전환이 모두 SDK 씬 이벤트에 자동 연결된 것은 아니다.

## 이번 문서 작업

소스 동작은 변경하지 않았다. 재컴파일 대상이 없으므로 빌드·테스트를 실행하지 않았다. 위 실행 검증 결과는 직전 ABI v10 검증 기록을 정리한 것이다.
