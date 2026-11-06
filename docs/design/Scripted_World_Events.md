# 세션 월드의 스크립트 이벤트

우선 확장 범위인 도시 내정·장수 능력치·명성·기재·전법·특기·보주는
[Script_Entity_Data_API.md](Script_Entity_Data_API.md)에 구현 범위와 사용 예제를 정리했다.

## 작성과 연결

이벤트는 `WorldEvent`를 상속하거나 조건·결과 델리게이트를 받는 `ScriptedWorldEvent`로 작성한다.
`world.Events.Attach(instance, owner)`로 현재 세션에 등록한다. owner는 선택 사항이며
`GetOfficer/GetCity`가 반환하는 월드 엔티티를 사용할 수 있다.

등록된 인스턴스와 필드는 도시·평정·전투 씬을 오가는 동안 유지된다.
세션 종료/다른 세이브 로드 시 등록을 해제하고 `OnDetached()`를 호출한다.
다음 세션에는 새 인스턴스를 등록한다. 상태를 세이브 파일에 직렬화하는 기능은 아직 없다.

```csharp
public sealed class MyStorySetup : WorldSetup
{
    public override void Configure(GameWorld world)
    {
        var officer = world.GetOfficer(565);
        world.Events.Attach(new MedicalStory(), officer);
    }
}
```

```csharp
public sealed class MedicalStory() : WorldEvent(
    "example.medical-story", WorldEventTriggers.BattleTurnEnded)
{
    private bool asked;
    private bool approved;
    private bool completed;

    public override bool CanExecute(WorldEventContext context)
    {
        ushort id = context.Owner!.GetComponent<OfficerComponent>()!.Id;
        var unit = context.Battle?.FindOfficer(id)?
            .GetComponent<BattleMembershipComponent>()?.Unit;

        return !completed && (!asked || approved)
            && unit != null && unit.Soldiers < unit.Capacity;
    }

    public override void Execute(WorldEventContext context)
    {
        ushort id = context.Owner!.GetComponent<OfficerComponent>()!.Id;

        if (!asked)
        {
            asked = true;
            EventEffects.Say(context, id, "부상병의 치료를 준비하겠습니다.");
            context.Choose("치료 지시", [
                new UiRow(1, "허가", "다음 턴 종료에 치료한다"),
                new UiRow(2, "거절", "치료를 진행하지 않는다")
            ], (reply, choice) =>
            {
                approved = choice == 1;
                completed = !approved;
            });
            return;
        }

        // 선택지 응답에는 전투 토큰이 없으므로 다음 실제 전투 콜백에서 실행한다.
        EventEffects.Say(context, id, "부상병을 치료하라!");
        EventEffects.RestoreSoldiers(context, id);
        completed = true;
    }
}
```

이 예제는 자동으로 활성화하지 않는다. 사용자 모드에서 명시적으로 등록한다.
기존 마왕의료 컴포넌트는 이 이벤트로 교체하지 않았다.

## 조건과 흐름

- `CanExecute(context)`에 C# 조건식을 작성한다. AND/OR/NOT 조합은 `EventConditions.All/Any/Not`도 제공한다.
- `Enabled`로 개별 등록을 끄고, `Once`로 성공한 Execute를 세션당 한 번만 실행할 수 있다.
- 실행 횟수는 `ExecutionCount`, 선택지 대기는 `AwaitingChoice`로 조회한다.
- `context.Signal("story.next", payload)` 또는 `world.Events.Signal(...)`로 사용자 신호를 보낸다.
- 일반 신호는 현재 핸들러 이후 처리한다. 신호 순환은 한 번의 전달 체인 256건에서 예외로 중단한다.
- 실행 중인 같은 인스턴스의 재진입과 선택지 대기 중 재발동은 건너뛴다.
- `CanExecute/Execute`의 컨텍스트는 콜백 반환 시 만료된다. 다음 콜백까지 저장해서 사용하지 않는다.
- 선택지 결과에는 새 컨텍스트를 제공한다. 취소는 -1이며, 전투 컨텍스트는 제공하지 않는다.
- 이벤트 ID는 한 월드에서 유일하다. `Detach(id)`로 등록을 해제할 수 있다.
- 스레드/Task 타이머로 게임을 호출하지 않는다. 기존 게임 콜백에서만 엔진 서비스를 사용한다.

## 연결된 시점

| 트리거 | 현재 연결 |
| --- | --- |
| SessionStarted | 관리 계층이 새 로드 세션을 관측한 뒤, SessionStarted 구독자가 이벤트를 등록한 다음 |
| SceneEntered / SceneExited | SDK EnterScene/LeaveScene 호출; 도시/평정의 실제 게임 전환 훅은 미연결 |
| BattleTurnStarted | 기존 검증된 전투 턴 시작 콜백 |
| BattleUnitActionEnded | 기존 검증된 개별 부대 행동 종료 콜백 |
| BattleTurnEnded | 기존 검증된 전체 행동 종료 콜백 |
| BattleEnded | 기존 전투 어댑터 해제 후 |
| 사용자 문자열 신호 | 스크립트의 명시적 Signal 호출 |

전투 이벤트는 기재 컴포넌트 실행 뒤, `Battle.EndEvent()` 전에 실행한다.
기존 BattleWorld 스냅샷과 토큰을 공유하므로 이벤트를 위해 전투 부대를 다시 스캔하지 않는다.
네이티브 연출이 중첩 전투 콜백을 펌프하면 그 콜백의 유효 범위 안에서 전달한다.

## 제공한 효과와 연출

| API | 구현과 범위 |
| --- | --- |
| EventEffects.Say | 신규 네이티브 연산 32. 실제 장수 확인 후 AF530의 personality=0 경로 호출. 배지/기재 음성 없음. 정적 분석 기반이며 신규 대화 경로는 실행 미검증 |
| context.Choose | 기존 네이티브 목록 대화상자 + C# 응답 콜백. 초상화가 포함된 게임 이벤트 전용 선택지 레이아웃은 아님 |
| context.Message | 동일 목록 대화상자를 사용하는 한 항목 메시지 |
| EventEffects.ShowPersonality | 기존 전투 기재 연출: 초상화·배지·대사·설정된 기재 음성/효과음. 해당 장수가 그 기재를 보유해야 함 |
| EventEffects.RestoreSoldiers | 기존 게임 병력 회복 getter/setter. amount=0이면 상한까지 회복. 행동 종료에서는 해당 행동 부대만 변경 가능 |

SDK 메서드 이름은 모드 매니저가 붙인 이름이다. 게임의 원래 심볼 이름으로 주장하지 않는다.
일반 C# 실행 코드로 기존 공개 컴포넌트도 조합할 수 있지만, 미분석 게임 행동을 지원하는 것은 아니다.

## 연출 RE 근거와 후속 바인딩

분석 원본: `C:\Users\lance\Documents\SAN8RPK-analysis-20261104\SAN8RPK.exe.unpacked.exe`.
추가 RTTI 추적: 같은 디렉토리 `event-presentation-candidates.json`.

- AF530는 문자열 목록을 만들고 AF0B0로 전달한다. AF0B0는 장수를 PersonData 테이블에서 찾는다.
  AF2CA의 기재 ID 검사에서 0은 AF33C 분기로 간다. AF312의 기재 음성(+0x30) 설정은 이 경로에서 실행하지 않는다.
- `CameraFocusTimelineEvent@war@san8r`는 RTTI로 확인한 실제 클래스 이름이다.
  TD 0x2D7E750, vtable 0x25B3BB0. 0x1401270은 타임라인 객체의 +0x10/+0x14 좌표와
  실행 컨텍스트를 사용하며 지형 높이 0x1808B50을 조회하고 0x1D4E420을 호출한다.
  생성·등록·종료 및 전투 카메라 원상복구 계약은 아직 확정하지 않았다.
- BGMListenUIPlayButtonBehavior TD 0x2D78010, vtable 0x25A71F8.
  0x1370330..0x137049C는 버튼 외형 리소스 선택/설정 경로다. BGM 재생 함수라고 잘못 노출하지 않는다.
  추가로 BGMListenMainState vtable 0x25A6038의 0x136D9E0에서 재생 버튼 상태 분기를 확인했다.
  0x136CF20은 +0x4F8 플래그에 따라 전역 객체(0x2E99738)의 +0x50을 대상으로
  0x1568710/0x1567600을 호출한다. 재생/일시정지 전환으로 추정되지만 독립 호출 계약은 미확정이다.
  0x136CEE0은 0..100 값을 받아 +0x51C에 보관하고 0x15685B0에 float로 전달한다.
  재생 위치 조절인지 음량인지 아직 확정하지 않았다.
- `VoiceOrTimeWaitState@EventView` TD 0x2DFBA40, vtable 0x268EE48.
  `CEventTalkLayout` TD 0x2DFBFC0, vtable 0x268E920.
  임의 무장 음성 ID 재생/완료 대기와 일반 이벤트 초상화 선택지 레이아웃 연결은 추가 분석 대상이다.

따라서 BGM 재생·정지·복원, 임의 음성 재생, 카메라 제어는 아직 공개 API가 없다.
현재 구현을 게임에서 가능한 거의 모든 행동의 이벤트 효과화가 완료된 것으로 보지 않는다.
월별/평정/도시 훅, 외교·임관·이동·전쟁 등의 결과 API도 별도 바인딩이 필요하다.

## 검증 상태

세션 전환·상태 유지·조건부 1회 실행·컨텍스트 만료·신호 순서·실행 중 해제·순환 제한
테스트 소스를 추가했다. 이번 변경은 빌드/테스트 실행/게임 적용 전이다.
