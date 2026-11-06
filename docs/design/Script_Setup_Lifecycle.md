# 전역 설정과 세션 월드 초기설정

현재 컴포넌트 콜백과 호출 시점은 [C# 스크립트 생명주기](Script_Lifecycle.md)에 정리한다.

개발자가 SessionStarted 구독 순서를 맞추는 대신, 공개 설정 클래스를 상속해서 작성한다.
표준 모드 진입점이 GameApplication을 만들고 설정 클래스를 자동 수집·적용한다.
어셈블리 특성이나 설정 클래스 등록 목록을 추가할 필요는 없다.

| 범위 | 초기설정 | 적용 시점 | 유지 기간 |
| --- | --- | --- | --- |
| 게임 전역 | GameSetup | 모드 로드 시 한 번 | 타이틀·시나리오 선택·여러 세이브를 오가는 동안 |
| 로드된 세션 | WorldSetup | 새 세션을 관측하고 주인공을 연결한 뒤 | 해당 세션 종료/다른 세이브 로드까지 |
| SDK 씬 | 기존 GameScene | 명시적 EnterScene | 해당 씬 종료까지 |

전역 설정도 현재 모드 인스턴스 소유다. 모드 해제/교체 때 해제되고 새 모드에서는 다시 구성한다.
세션 교체 시 GameSetup과 전역 컴포넌트의 필드는 유지된다.

## 세션의 장수·도시에 컴포넌트 추가

```csharp
public sealed class MyWorldSetup : WorldSetup
{
    public override void Configure(GameWorld world)
    {
        var officer = world.GetPlayer().Character;
        officer.AddComponent<MyOfficerComponent>();

        var city = world.GetCity(1);
        city.AddComponent<MyCityComponent>();
    }
}
```

MyOfficerComponent/MyCityComponent는 모드 개발자가 작성하는 Component 클래스다.
Configure는 세션당 한 번 호출되므로 전투 이벤트마다 AddComponent하지 않는다.
다른 세이브를 로드하면 이전 컴포넌트를 해제하고 새 설정 인스턴스로 다시 Configure한다.

## 타이틀·편집·기본 설정을 위한 전역 컴포넌트

```csharp
public sealed class MyGameSetup : GameSetup
{
    public override void Configure(GameApplication application)
    {
        application.Root.AddComponent<MyDefinitionSettingsComponent>();

        var editor = application.CreateObject("MyDefinitionEditor");
        editor.AddComponent<MyDefinitionEditorComponent>();
    }

    public override bool Handle(ScriptEvent kind, int owner, int value)
    {
        // 이 설정이 소유한 메뉴/창의 이벤트만 처리하고 true를 반환한다.
        return false;
    }
}
```

전역 객체는 Application을 참조하고 World/Scene은 null이다.
월드 소속 객체에서는 GameObject.Application으로 같은 전역 애플리케이션에 접근할 수 있다.
예제의 두 컴포넌트도 개발자가 작성하는 클래스다.

기존 타이틀·인게임 기재 편집 메뉴와 보주 편집 진입점은 실제 GameSetup 구현인
S8.GameScripts.EditorMenusSetup으로 이동했다. 모드 진입점에서 개별 메뉴 등록 코드를 직접 실행하지 않는다.
치트 편집 메뉴는 다시 등록하지 않는다.

이 범위는 기재 정의·등록무장 기본값 편집 서비스가 놓일 위치를 제공한다.
등록무장의 기본 데이터와 로드된 시나리오의 장수 데이터는 같은 것으로 취급하지 않는다.
이번 구조 변경 자체가 등록무장 기본 스탯 setter나 시나리오 선택 화면 전환 훅을 새로 구현한 것은 아니다.
GameSetup.Handle에는 현재 네이티브 계층이 전달하는 ScriptEvent만 전달한다.
미연결 화면의 Enter/Exit 이벤트를 임의로 만들지 않았다.

## 자동 수집과 순서

- 모드 어셈블리의 공개·비추상·비제네릭 파생 클래스만 수집한다.
- public 매개변수 없는 생성자가 필요하다. 게임 작업은 생성자 대신 Configure에 작성한다.
- 수집은 애플리케이션/월드 생성 시 이루어지며 프레임마다 타입을 스캔하지 않는다.
- Order가 작은 설정부터 실행하고, 같으면 클래스 전체 이름의 ordinal 순서로 실행한다.
- GameSetup은 모드 로드당 새 인스턴스, WorldSetup은 세션당 새 인스턴스다.
- WorldSetup 적용을 마친 다음 SessionStarted 알림과 세션 시작 이벤트를 전달한다.
- Dispose는 역순으로 호출한다. 설정이 직접 구독한 이벤트 등의 해제를 이곳에 작성한다.
- GameObject 컴포넌트와 월드 이벤트의 해제는 각 소유 범위가 담당한다.
- Configure가 실패하면 부분 구성된 해당 범위를 해제한다.

GameWorld를 직접 만드는 고급 모드는 기본적으로 생성자를 호출한 모드 어셈블리에서 수집한다.
SDK 팩터리/여러 어셈블리를 관리하는 경우 scriptAssembly 인수로 수집 대상을 명시할 수 있다.
현재는 지정한 한 모드 어셈블리에서 수집하며 프로세스 전체나 의존 어셈블리를 재귀 스캔하지 않는다.

표준 초기화 순서는 공통 서비스 준비 → GameSetup → 최초 월드 관측 → WorldSetup → SessionStarted다.
같은 세션의 RefreshSession은 초기설정을 반복하지 않는다.

## 검증 상태

자동 수집·설정 순서·세션 중복 적용 방지·전역 상태 유지·세션 교체·초기설정 실패 정리의
테스트 소스를 추가했다. 이번 변경의 빌드·테스트 실행·게임 적용은 하지 않았다.
