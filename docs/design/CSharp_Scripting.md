# 모드 매니저와 C# 표준 계층 — API v4

## 현재 구조

```text
xinput1_4.dll(S8.Scripting.Native): 독립 진입점·훅 설치·상태 publication·보주 명령 적용·공통 UI·입력·팝업 수명·게임 빌드 검사·JIT 호스팅·게임 데이터·메모리 도구
  ↕ 공통 EngineApi.Invoke
S8.Scripting.Host: 매니페스트 로딩·API/기능 검사·모듈 교체
  ↕
S8.Scripting: GameObject·Component·표준 UI/게임 데이터/메모리 컴포넌트
  ↕
S8.GameScripts: 기재·보주 에디터와 메뉴 구성
```

기재 편집 전용 `Text.Name`, `Text.Description`, `Command.ShowPage`, `BeginDetail`, `ApplyEffect`, `RestoreEffect`, `Commit` 계약을 제거했다. C++에는 기재·보주 편집 draft, 효과 설명 조합, 음성 목록, 상세 페이지 전환과 편집 확정·취소 결정이 없다. 메뉴 항목·문구·배치·동작은 C#에서 등록한다.

공통 UI 실행부는 `src/scripting/native_ui.cpp`, 입력 버퍼는 `native_ui_input.cpp`, 공개 다이얼로그 연산은 `native_ui_api.inl`에 있다. `native_tree.inl`은 게임 보주 트리 리소스의 생성·표시·노드 상태 적용·클릭 연결, `native_window.inl`은 기존 게임 창의 행 템플릿과 부모 창 종료 결과 연결, `native_menus.inl`은 등록받은 메뉴 항목의 삽입과 게임 입력 순서 연결을 담당한다. `native_ui_services.inl`은 메뉴 등록·선택 결과·트리 표시·publication·명령 배치의 요청 경계이다. 모두 모드 매니저 DLL에서 컴파일한다. 보주 전용 `legacy_jewel_ui.inl`은 제거했다.

모드 매니저는 치트(S8RPKCheats)와 별도 저장소·별도 진입점이다. `mod_entry.cpp`가 게임 코드 복호화를 기다린 뒤 훅을 직접 설치하고, 보주 명령은 `jewel_services.cpp`가 적용한다. 치트 DLL을 불러오거나 치트에 기대지 않는다. 게임 데이터와 메모리 서비스는 `engine_services.cpp`에 있다.

## C++가 제공하는 기능과 근거

| 기능 | 실제 구현 |
|---|---|
| ShowDialog / CloseDialog | 분석한 CDataSelectState 생성과 CUILayout/다이얼로그 가상 함수. 모드가 제목·행·스타일·이벤트 소유 ID를 지정 |
| SetRow | 네이티브 버튼 라벨 슬롯 +0x298, 텍스트 갱신 RVA 0x1D154E0 |
| InputText | CInputStringState enqueue 0x32860, 언어 코드페이지 0x17F3250 |
| InputNumber | CInputKeyDlgState enqueue 0x173120 |
| GetPersonality | 분석한 전역 루트 0x2E98BC8, 레코드 포인터 테이블 +0x57A4D0, ID +8, 효과 +0xA, 음성 +0x30의 읽기 어댑터. 원본 게임의 getter라고 이름 붙이지 않음 |
| ReadPersonalityName | 게임 getter RVA 0x170E2D0 |
| ReadPersonalityDescription | 기재 설명 getter RVA 0x170ABB0(record). 0x171E070은 문자열 관리자 함수이며 record를 this로 넘기지 않는다 |
| InspectMemory / ReadMemory / WriteMemory | 매니저가 작성한 도구. VirtualQuery, ReadProcessMemory, WriteProcessMemory 사용 |
| GetSession | 게임 세션 publication과 현재 콜백 세션의 일치 확인 |

위 공개 이름은 원본 심볼이 아니라 바인딩 이름이다. 다이얼로그 서비스는 여러 분석된 엔진 호출을 조합한 공통 래퍼이며 기재 에디터의 페이지 의미는 갖지 않는다.

메모리 도구는 세션, 주소 범위·오버플로, 최대 전송 64KiB, 커밋된 페이지와 접근 권한을 입구에서 확인한다. 실행 가능 페이지의 쓰기는 제공하지 않는다. 확인과 전송을 한 요청에서 수행하며 타입으로 확정된 데이터를 내부에서 반복 검증하지 않는다. 세션 참조는 객체 소유권을 뜻하지 않고, OS 페이지 검사는 다른 코드의 동시 변경을 잠그지 않는다.

## C# 표준 계층

- `GameObject`, `Component`: 관리 객체 구성과 컴포넌트 생성·조회·종료. 원본 게임이 ECS라는 의미는 아니다.
- `UiScene`, `DialogComponent`: UI 소유 ID와 선택·클릭·닫힘·복귀 이벤트 연결.
- `ButtonComponent`, `TextLabelComponent`, `TextInputComponent`, `NumberInputComponent`: 공통 UI 서비스 조합.
- `PersonalityComponent`: 게임 기재 레코드 읽기, 게임 이름·설명 getter, 표준 메모리 도구를 이용한 효과·음성 쓰기.
- `MemoryReference`, `MemoryAccess`, `MemoryComponent`: 세션이 붙은 주소, 범위 검사와 타입/바이트 단위 읽기·쓰기.

`PersonalityComponent.WriteValues`는 확인된 게임 setter 호출이 아니다. 쓰기 직전에 레코드를 다시 바인딩하고, 효과 36바이트와 음성을 포함한 40바이트 범위를 읽어 미확인 두 바이트를 보존한 뒤 공통 메모리 도구로 쓴다. 이름·설명의 내부 setter는 아직 제공하지 않는다.

현재 표준화한 게임 데이터는 기재이며, 무장·도시·모델·리소스의 전체 분석 목록을 모두 바인딩한 것은 아니다. 콜백은 현재 네이티브 UI 수명 경로에서 동기 실행한다. 임의의 작업 스레드나 범용 Update 이벤트는 제공하지 않는다. 닫힘 콜백에서는 draft·저장을 처리하고, 입력 완료 콜백에서는 draft·텍스트를 갱신한다. 이 콜백에서 새 창 열기·닫기는 거부한다. 입력창 정리가 끝난 Resume 콜백에서 후속 화면을 열 수 있다.

## 기재 에디터 모드

`TraitEditor.cs`가 다음을 소유한다.

- 게임 레코드를 붙인 GameObject, 모드 소유 draft와 취소 복원.
- 목록·상세·효과·매개변수·음성·문자열 입력 화면의 구성과 이동.
- effect_definitions.json 로드, 효과 설명 조합, 음성 선택 목록.
- 이름 길이와 설명 서식 토큰 검사.
- san8r_traits_config.json의 원문 비교, 임시 파일 생성 후 교체를 통한 저장.

설정 파일 저장은 게임 API로 포장하지 않았다. 기존 카탈로그 서비스가 변경 파일을 관찰하고 게임 getter에 이름·설명 오버라이드를 반영한다. 효과·음성은 파일 저장 후 공통 메모리 도구로 현재 레코드에도 적용한다. 외부 변경이 감지되면 저장을 중단한다. 파일 비교와 교체 사이의 다른 프로세스 쓰기까지 잠그는 계약은 아니다.

## 보주 에디터와 메뉴 구성

`ModApplication.cs`가 기재·보주 모듈을 구성하고 `MenuExtensionComponent`로 게임 메뉴와 주인공 창의 보주 행을 등록한다. 게임 함수·리소스의 RVA와 가상 함수 호출은 C++ 바인딩이 소유한다. C#에서 임의의 함수 주소를 실행하지 않는다.

`JewelEditor.cs`는 개방 bitmap, 강제 적용 mask, 원본 snapshot, 초기화와 확정·취소를 소유한다. `GemTreeComponent`는 기존 게임 트리 화면을 쓰며 노드 클릭·주 동작·보조 동작·뒤로 이벤트를 C#에 전달한다. 트리 화면에서 돌아온 뒤에도 draft는 부모 주인공 편집창에 묶여 유지된다. 부모 창의 결정 결과가 수락일 때만 C#이 변경 목록을 계산해 한 배치로 제출하며, 취소 결과에서는 제출하지 않는다. 배치의 epoch·크기·범위를 모두 확인한 뒤 큐에 넣고 기존 명령 처리기가 게임 상태와 설정에 적용한다. 이 명령 서비스는 모드 매니저 구현이며 원본 게임 setter로 명명하지 않는다.

강제 적용 목록은 공통 `DialogKind.MultiSelect`, `initialSelection`, `ReadSelection`을 사용한다. 네이티브 목록의 체크 상태는 UI 바인딩이 전달하고, 그 결과를 보주 비트로 바꾸는 처리는 C#에 있다. 트리 노드와 입력 가능 여부도 C#에서 제공한다. 노드 갱신은 클릭 콜백 안에서 컨트롤을 파괴하지 않도록 다음 게임 UI Update에서 반영한다.

보주 정의 185개는 `assets/jewel_definitions.json`으로 분리하고 모드 로드 입구에서 범위·중복을 확인한다. `JewelComponent`는 publication 읽기·게임 데이터의 분류 읽기·명령 배치 제출을 묶은 표준 컴포넌트다. `CanReload`는 모드의 기본 구현을 제공하며, 이 애플리케이션은 부모 창에 보류 중인 draft가 있으면 교체를 미룬다. 메뉴 클릭 시 변경된 어셈블리를 다시 확인하므로 부모 창 종료 후 다음 메뉴 진입에서 교체할 수 있다.

## 모듈 로드와 업데이트

`scripts/*.mod.json`에서 진입 번호·어셈블리 파일·타입을 읽는다. 기재 모드 매니페스트는 다음과 같다.

```json
{"entry":0,"assembly":"S8.GameScripts.dll","type":"S8.GameScripts.ModApplication"}
```

기존 기재 편집 메뉴는 `open_script_mod(0)`으로 진입하는 어댑터다. Host는 기재 타입명을 고정하지 않는다. 현재는 하나의 활성 UI 모듈을 실행하며, 여러 모드의 동시 실행·의존성/우선순위 관리는 아직 없다.

모드 개발자가 어셈블리 특성을 선언할 필요는 없다. SDK의 AssemblyVersion(현재 4.0.0.0)은 모드 컴파일 시 참조 정보에 자동 기록된다. Host는 그 참조 버전과 실제 SDK 버전을 비교한 뒤 `Initialize(ModContext)`와 Open 이벤트를 호출한다. 기능 요구 목록도 작성하지 않으며, 표준 서비스 사용 시 제공 기능을 확인해 MissingCapability로 거부한다. 모드에 게임 폴더와 에셋 폴더를 전달한다. 모드 메뉴를 클릭하거나 Open으로 재진입할 때 어셈블리/매니페스트 수정 시간을 비교해 모듈을 교체한다. 부모 창에 보류 중인 보주 draft가 있으면 CanReload가 false여서 교체를 미룬다. 파일은 메모리에서 로드하며 이전 컨텍스트에 Unload를 요청한다. 최종 메모리 회수 시점은 CLR이 결정한다. SDK·Host 변경은 게임 재시작 대상이다.

현재 JIT 경로만 구현했다. NativeAOT 로더는 없다.

## 게임 업데이트 거부

API v4는 32바이트 함수 테이블과 연결 상태·게임 빌드·기능 비트를 전달한다. Ui=1, GameData=2, Memory=4다.

C++는 UI 훅 설치 전에 x64 PE 형식, 타임스탬프 0x69A67ED1, 이미지 크기 0x440F000, 디스크 EXE SHA256 `000d450345728c23bebca581ab37b8bcf25266172767b5e6defb10a72a8c9457`을 확인한다. 추가로 실행 메모리의 RVA 0x76E930, 0x3C190, 0x392D0, 0x32860, 0x173120, 0x1D154E0에서 각 16바이트를 비교한다. SteamStub 원본 파일의 해시와 실행 코드는 별도로 다룬다. 검사 결과는 프로세스 수명 동안 보관한다.

실패 시 기능 비트는 0이며 네이티브 UI 설치 경로를 진행하지 않는다. C#은 EXE를 다시 검사하지 않고 연결 결과와 모드가 참조한 SDK 버전을 확인한다. ABI가 다른 이전 스크립트는 거부한다.

상태: InvalidApi(-1), ApiVersionMismatch(-2), UnsupportedGameBuild(-3), CodeMismatch(-4), MissingCapability(-5), RuntimeUnavailable(-6), ScriptFailure(-7), InvalidBinding(-8), MemoryAccessDenied(-9), StaleSession(-10).

## 빌드·배포와 검증 상태

기존 S8.Hid.vcxproj의 증분 빌드 경로가 네이티브 프로젝트와 C# 프로젝트를 연결한다. 출력에는 hid.dll, S8.Scripting.Native.dll, nethost.dll, scripts의 Host/SDK/GameScripts DLL·deps/runtimeconfig·모드 매니페스트와 assets/effect_definitions.json·jewel_definitions.json이 필요하다. 기존 컴파일 옵션은 변경하지 않았다. 현재 기본 .NET Host pack은 10.0.14이며 DotNetHostPack 속성으로 위치를 지정할 수 있다.

2026-11-04 Release x64 증분 빌드와 `tests/scripting/S8.Scripting.Smoke.csproj` 실행을 완료했다. 최종 결과는 **44 ABI/컴포넌트 검사 통과**다. 기존 검사에 보주 트리 구성, 변경 직후 제출 금지, 트리 종료만으로 제출하지 않음, 부모 창 취소, 부모 창 확정, 카탈로그 raw ID와 강제 적용 mask 제출 검사를 추가했다. 이 검사는 관리 코드의 모의 엔진을 사용하며 아래 실제 게임 확인과 구분한다.

실제 게임에서 확인한 경로:

- C# 등록 메뉴: 타이틀 편집의 기재·치트 편집, 인게임 기능 메뉴의 치트 편집 항목 표시, 인게임 편집의 기재 항목, 주인공 편집의 보주 행 표시.
- 기재: 목록의 게임 이름·설명 조회, 상세 폼, 이름 입력창 생성, Backspace로 글자를 지우면서 창 유지, 입력 취소 후 원래 이름 유지, 상세→목록→편집 메뉴 복귀.
- 보주: 기존 게임 트리 표시, 노드의 개방 표시 전환, 강제 적용 다중 선택, 자식 목록 결정 후 트리 복귀, 부모 창의 결정 버튼 활성화, 부모 창 중지 후 재진입 시 원래 개방 상태 복원.
- 초기 선택 수 표시를 `0`에서 실제 개수로 갱신하도록 보완했다. 게임의 선택 수 표시 함수 `0x37F10(dialog, selected, total)`를 바인딩했고, 초기 `177 / 179`, 한 항목 해제 후 `176 / 179`, 재선택 후 `177 / 179`를 확인했다.
- 부모 창에서 결정하기 전 설정 mask 불변, 결정 후 예감(ID 2) 강제 적용 해제 한 비트만 변경, 재진입 시 해제 상태 유지, 재선택·부모 결정 후 원복을 확인했다. 원복 후 `S8RPK_cheat_config.json`은 테스트 전 백업과 SHA-256이 일치했다. 게임 세이브 저장이나 시간 진행은 하지 않았다.

테스트 중 수정한 오류:

- 공통 UI 정리 때 남은 UTF-8→UTF-16 변환 호출을 입력 완료 콜백 안에 복구했다.
- 최초 기재 목록 실행에서 설명 조회가 충돌했다. 덤프는 `SAN8RPK+0x171E154`에서 기재 레코드를 문자열 관리자 객체로 잘못 해석한 경로를 보여 주었다. 분석용 EXE에서 `0x171E070`은 문자열 관리자 조회, `0x170ABB0(record)`는 기재 설명 래퍼임을 재확인하고 후자로 수정했다. 수정 후 목록·상세·입력 취소 경로가 실행됐다.

자동 한글 입력은 포커스를 다시 지정해도 화면에 반영되지 않아 이번 테스트의 성공 항목에 포함하지 않는다. Backspace 입력과 취소 복귀만 확인했다. 기재의 실제 게임 파일 저장·효과 숫자 확정, 보주 개방 bitmap의 실제 확정·재로드, C# 모듈의 실제 교체는 이번 실행 검증에 포함하지 않았다. 보주 개방 전환은 draft와 취소 복원까지, 실제 설정 확정은 강제 적용 mask로 확인했다.

배포 전 파일 백업은 `C:/Users/lance/AppData/Local/Temp/S8-api4-test-20261104-094428`에 있다. DLL·스크립트·에셋 16개를 적용하고 해시를 비교했다. 마지막 테스트 게임 PID는 47340이며 `S8.Scripting.Native.dll`과 `coreclr.dll` 로드를 확인했다. 중간 진단용 디버거 분리 시 테스트 프로세스가 종료되어 재실행했으며, 최종 테스트는 디버거 없이 진행했다.

이전 API v1에서 Release 빌드·32개 통합 검사와 실제 UI 입력·저장을 확인했고, 동일 게임 PID에서 서로 다른 C# 코드의 교체·원복도 확인했다. 이 기록은 새 v4의 동작 검증을 대신하지 않는다. 이전 근거는 `C:/Users/lance/Documents/SAN8RPK-analysis-20261104/jit-live-test`에, 재작성 전 소스는 `before-mod-api-v3`에 보관했다.

## 전투 턴 종료 스크립트와 마왕의료 (API v5)

- `마왕의료`: 새 커스텀 기재 ID **254**, 기존 이름·효과가 비어 있는 슬롯에 생성했다. 원본 설정은 게임 폴더의 `san8r_traits_config.json.before-script-traits.bak`에 보관했다. 장수에게 자동 부여하지 않는다. 게임의 기재 부여 화면에서 선택한다.
- 게임 설정 `customNames[253]`에는 이름·설명을, `san8r_script_traits.json`의 `traits[254]`에는 `s8.battle.wounded-recovery`를 저장한다. 네이티브 효과 슬롯 여섯 개는 모두 0이다. 스크립트 효과 ID를 게임의 숫자 효과 디스패처에 넣지 않는다.
- C# `ScriptEffectRegistry.Register<T>`로 효과 이름·설명·컴포넌트를 등록하면 기재 편집기의 **스크립트 효과** 목록에 나타난다. 선택은 편집 초안에 보관하고 부모 화면의 결정 때 저장한다. `마왕의료 추가` 메뉴는 다른 설치에서도 빈 커스텀 슬롯을 찾아 같은 기재를 생성한다.
- `WarTurnEndState` vtable **2707880**: 진입 **1D86E30**, Update **1D86DB0**, Exit **1D86D90**. Update가 부르는 단계 처리 **1D86520**의 점프 테이블 **1D86D2C**는 1..15단계다. 정상 턴 종료는 14단계 **1D86C6A**에서 상태 6으로 전환한다. 15단계 **1D86C80**는 별도 전투 종료 경로로 상태 19를 요청한다. 네이티브 콜백은 14단계의 원래 함수가 반환된 후에 `BattleTurnEnded`를 동기 전달한다. 이전 상태는 전환 중 해제될 수 있으므로 반환 후 다시 읽지 않는다. Present나 날짜 폴링을 사용하지 않는다.
- 전투 컨텍스트의 `+50 → +108` 컬렉션에서 `+20/+28`의 WarUnit 포인터 범위를 읽는다. `WarUnit` vtable은 **2718188**, 구현 모델은 `+8`, 소속 장수 배열은 모델 `+60`의 세 포인터다. PersonData `+8`은 ID, `+88`은 보유 기재 포인터 세 개다. 이 조회는 분석한 객체 필드를 묶은 관리 DLL 서비스다.
- `WarUnit` 실제 게임 메서드: 병력 조회 **1E3A8D0**(vslot30), 상한 조회 **1E3A860**(vslot40), 상한을 적용하는 설정 **1E3A880**(vslot38). `SoldierHealEffectActivator` vtable **2705A60**, Apply **1D68C70**도 이 세 슬롯을 사용한다. 바인딩은 이 getter/setter를 호출한다. Apply 전체를 호출하거나 해당 함수의 연출·로그를 재현하지 않는다. 별도 부상병 카운터를 감소시키는 구현도 아니다. 게임 회복 효과에서 쓰는 현재 병력과 회복 상한의 차이를 회복한다.
- C# `WoundedRecoveryComponent`는 해당 장수의 `BattleMembershipComponent.Unit`만 대상으로, 병력이 0보다 큰 경우 `Capacity - Soldiers`를 요청한다. 같은 부대의 여러 장수가 같은 효과를 보유해도 부대당 효과 ID별 한 번만 실행한다. 회복 여부와 양은 C#에 있고 C++에 기재 ID·이름 조건은 없다.
- 네이티브 서비스는 콜백별 토큰만 C#에 전달한다. 콜백 밖 호출과 지난 턴 토큰은 거부한다. `ReadBattleUnits`와 `RestoreBattleSoldiers`는 GameData 기능으로 분류했다. 접속 시 기존 EXE 해시 검사에 단계 함수 한 개와 병력 메서드 세 개의 코드 검사를 추가했다. SDK 어셈블리와 C++ ABI는 v5다.
- 이번 변경은 소스 검토만 했다. 빌드·실제 전투 실행 검증은 수행하지 않았고, 설치된 DLL도 교체하지 않았다.

## 독립 런타임 전환 (2026-11-04)

`version.dll` 호환 경로를 제거했다. TraitCatalogService는 커스텀 설정 또는 기본 설정을 읽어 우리 TraitRuntimeNative에 전달한다. 외부 DLL 파일 감지, ExternalRuntime 상태, 외부 편집기 WM_COMMAND 전송, 외부 보주 판정의 getter/setter 호출은 없다. 이름·설명·효과·발동 음성은 독립 런타임의 기재 레코드 적용 경로에서 처리한다. 게임 자체의 신규 기재 목록 확장, 등록 장수 역직렬화와 시나리오 기재 ID 처리는 유지한다.

10:31 충돌 덤프 `SAN8RPK.exe.25124.dmp`는 version.dll 작업 스레드에서 `FAST_FAIL_INVALID_ARG`로 종료했다. 호출자는 `[CustomName] idx=%d traitId=%d nameLen=%d descLen=%d name="%s" desc="%s"`를 512 wchar 버퍼에 기록하는 경로이며, 당시 idx129/trait130, 이름22·설명433 길이가 스택에 남아 있다. 이 덤프에는 C# 전투 효과 호출 스택이 없다. 독립 전환 후 실제 저장·불러오기 성공 여부는 별도 실행 확인 대상이다.

## 무장 기재 선택 목록 수정·실행 확인 (2026-11-04)

기재 편집기의 설정 반영 시험과 무장 기재 선택 목록 시험을 구분한다. 이번에는 실제 `기능 → 편집 → 무장 → 손상향 → 기재` 경로에서 확인했다.

- 원인 1: 71..200번 설명 조회는 확장 ID 문자열 관리자를 거치지 않고 `PersonalityData`의 formatted description getter RVA `170ABB0`을 사용한다. 기존 `171E070` 훅만으로는 이 경로가 처리되지 않았다. `170ABB0`에 카탈로그 설명을 제공하는 바인딩을 추가했다. 확장 문자열 관리자의 kind0/1/2는 각각 이름/일반 설명/포맷 설명으로 구분하고 등록된 텍스트를 우선한다.
- 원인 2: `PersonalityData::IsValid` RVA `16F9240`은 ID/등급과 함께 첫 숫자 효과가 0이 아닌지 검사한다. 숫자 효과를 비운 스크립트 전용 `마왕의료`(ID254)가 제외됐다. 이름이 등록된 71..254번 커스텀 기재를 허용하는 바인딩을 추가했다. 미등록 레코드는 원래 판정을 사용한다.
- 수정 파일: `src/game/trait_runtime_native.cpp`, `src/game/trait_runtime_native.h`. HOOK_COUNT는 12. 게임·런타임 관련 증분 컴파일 후 Release x64 성공, 게임 `hid.dll` 교체 및 해시 일치.
- 실제 세이브 슬롯33(212년7월 평정)을 불러와 손상향의 기재 선택 목록에서 기존 도주·강행·행군·추진·조타·답파·기장·궁장·공성 설명과 목록 끝 `마왕의료`의 이름·설명을 확인했다. `마왕의료`를 눌러 선택수가 2/3에서 3/3이 되는 것까지 확인했다. 뒤로와 중지로 취소하여 기존 궁요희·서초철기 두 기재를 유지했다. 세이브 저장이나 날짜 진행은 하지 않았다.
- 확인 프로세스 PID35568. 디버거에서 12개 훅 설치, Applied/pending0 확인 후 분리. 수정판 적용 후 충돌 로그 없음.

### 2026-11-04 실제 전투 종료 추적 수정

- PID 35568에서 정상 턴 종료의 1..14단계 실행을 확인했다. 기존 15단계 조건 때문에 회복 이벤트가 발생하지 않았다.
- 손상향(ID 565)의 부대에서 기재 200/204/254와 병력 16553, 회복 상한 17500을 읽었다.
- battle_services.inl 조건을 14로 수정하고, 실행 중인 자체 S8.Scripting.Native.dll의 비교 상수도 디버거로 15에서 14로 변경했다. 디스크 DLL은 아직 빌드·교체하지 않았다. 회복 성공 검증은 다음 턴 종료 시 확인해야 한다.

- 다음 실제 턴 종료에서 TURN_END_STAGE14 → SCRIPT_RECOVERY(token=0x101, amount=1289) → HEAL_NATIVE(unit=0x25e27b1dcc8, before=16211, requested=17500)을 확인했다. 실행 후 손상향 부대의 병력과 상한은 모두 17500이었다. 이벤트 발행 횟수는 1이다. 발동 시점은 적군·아군 전체 행동이 끝나는 전투 턴 종료이며 개별 부대 행동 종료가 아니다.

## 스크립트 끊김·색상·슬롯·발동 연출 (2026-11-04)

### 실행 중 확인한 사실

- PID35568의 턴 추적 로그에는 소프트웨어 중단점 `TURN_STEP` 적중이 1,251회 있었다. 단계4에서 778회, 단계8에서 327회, 단계10에서 135회 발생했다. 자동 계속 실행도 매 적중 때 프로세스를 멈추므로 프레임과 오디오 관찰을 교란한다. 중단점을 전부 제거하고 디버거를 분리했다. 게임은 Responding=true였다. 사용자가 중단점 제거 후 프레임·BGM 끊김이 사라졌다고 확인했다. 이번 재현의 반복 중단 원인을 확인했으며, 수정판 CLR 처리 시간은 아직 측정하지 않았다.
- 사용자가 유지한 궁요희 장면의 게임 스레드 스택은 `1D84350 → 139B3B0 → 139C52A 반환 프레임 → 139B300 → AF530 → AF0B0 → AF486`이었다. 대사 진행을 기다리는 원래 게임 연출 경로이며 C#에서 멈춘 스택이 아니다.
- `139B300`의 발동 레코드는 24바이트다. 실제 주소 `0x25e27ee6220`에서 효과125, 값70, 발동 종류2, 기재200, PersonData `0x25e8ac05068`(손상향565)을 읽었다. 기재 ID는 +0E, 장수 포인터는 +10이다.
- `AF0B0`은 기재 ID를 배지 설정에 넣고 해당 PersonalityData +30의 음성 ID와 장수 객체를 연출 설정으로 전달한다. `AF530`은 장수 ID와 대사 문자열을 받으며 게임의 연출 대기 루프를 사용한다. 실제 호출자는 8번째 인자의 bool을 0으로 지정한다.
- 원래 기재 레코드 +2E의 ushort는 ID1..30에서 1, ID31..61에서 2, ID62..70에서 3이었다. 금색/녹색/적색 등급에 해당한다. 기존 apply_records는 metadata.grade를 적용하지 않았다.

### 소스 변경

- TraitEditor.cs: 금색·녹색·적색 선택, grade 저장. 별도 스크립트 효과 버튼 제거. 기존 효과 1..6 각각의 선택 목록에 등록된 스크립트 효과를 추가했다. 해당 슬롯을 스크립트로 선택하면 숫자 효과는 0으로 저장한다. 기존 게임 레코드가 여섯 슬롯이므로 여섯 번째 슬롯도 보존한다.
- ScriptedTraits.cs: 슬롯별 string/null 배열인 바인딩 파일 v2. v1의 별도 효과 목록은 native 설정의 빈 슬롯으로 옮겨 읽으며, 저장 때 v2로 기록한다. 이벤트 실행 때 파일을 읽지 않고, 초기 로드·편집기 진입·편집 저장의 카탈로그를 사용한다. 외부 JSON 수정은 편집기를 다시 열어 반영한다.
- trait_runtime_native.cpp / trait_runtime_config.cpp: grade 1..3을 설정 입구에서 확인하고 기존 네이티브 적용 경로로 +2E에 기록한다. C#에서 즉시 쓰지 않는다.
- Battle.cs / battle_services.inl: ShowBattlePersonality API. 현재 콜백 토큰과 그 부대가 보유한 장수·기재를 입구에서 확인하고, `AF530`에 기재 ID·장수 ID·C# 대사를 전달한다. 게임 연출의 초상화·배지·음성·효과음 경로를 사용한다. 음성 ID0은 기존처럼 음성 없음이다. 현재 마왕의료 설정은 0이므로 캐릭터 음성을 원하면 편집기에서 발동 음성을 선택해야 한다.
- ScriptedTraitComponent는 활성 기재 ID를 담고 연출 서비스를 호출한다. WoundedRecoveryComponent는 회복할 병력이 있을 때 먼저 연출을 요청한 다음 기존 병력 설정 API로 회복한다. 기재 이름·회복 규칙은 계속 C#에 있다.
- ABI/SDK를 v6으로 올리고 game_binding.cpp의 입구 검사에 AF530의 분석한 16바이트를 추가했다. native_api 크기와 기존 연산 번호는 유지하며 새 연산을 끝에 추가했다.
- battle_script_timing은 calls / last_microseconds / maximum_microseconds / presentation_microseconds를 메모리에 기록한다. 게임의 연출·사용자 대사 대기 시간을 처리 시간에서 분리한다. 매 이벤트 파일 로그와 중단점은 필요 없다. JIT/GC가 실제 병목인지 이 계측 없이 단정하지 않는다.

### 검증·재컴파일 범위

소스와 git diff --check를 검토했다. 이번 요청에서는 빌드·DLL 교체·수정판 연출 실행 시험을 하지 않았다. 실행 중인 프로세스에는 앞서 적용한 14단계 비교 상수 수정만 있고, 이번 v6 변경은 아직 없다.

script_api.h의 직접 의존은 native_ui.cpp / script_host.cpp / game_binding.cpp, engine_services.h를 통한 engine_services.cpp, overlay/native_ui_bridge.cpp다. 이들과 trait_runtime_native.cpp / trait_runtime_config.cpp 및 C# SDK·Host·GameScripts가 재컴파일 대상이다. 설치 시 v6 네이티브와 관리 어셈블리를 함께 교체하고 게임을 재시작해야 한다.

### v6 빌드·설치 (2026-11-04)

- 사용자 요청으로 S8.Hid.vcxproj Release x64 증분 빌드 1회 성공(종료 코드0). C# SDK·Host·GameScripts 빌드도 성공했다. 네이티브에서 기존 NativeDialogFrame 정렬과 native_tree.inl 로컬 이름 가림 경고가 출력됐다.
- 게임이 종료된 상태에서 hid.dll / S8.Scripting.Native.dll / nethost.dll과 PDB, scripts 및 assets 총18개 파일을 게임 폴더로 복사했다. 전 파일 SHA256이 출력물과 일치한다. 설치된 SDK AssemblyVersion은6.0.0.0이다.
- 기존 파일 백업: C:\Users\lance\AppData\Local\Temp\S8-script-v6-deploy-20261104-114822. installed-manifest.json에 설치 파일 해시를 보관했다.
- san8r_traits_config.json / san8r_script_traits.json은 교체 전후 해시가 같다. 슬롯 바인딩 v2는 새 편집기로 저장할 때 기록된다.
- 게임은 실행하지 않았다. 색상 선택·슬롯 바인딩·새 발동 연출과 계측의 실제 실행 확인은 아직이다.

### v6 충돌 재확인 및 이벤트 명칭 근거 (2026-11-04)

- 앞선 v6 실행 PID26580에서 사용자가 적 B 공격 중 정지·종료를 보고했다. 재실행 PID14564에서는 같은 진행으로 충돌이 재현되지 않았고 사용자가 마왕의료 정상 동작을 확인했다. 재현되지 않은 사실을 앞선 충돌의 해결로 판정하지 않는다.
- 사용자 CrashDumps, 사용자 Temp 하위, 사용자 Local/Microsoft/Windows/WER, 게임 설치 폴더, Documents/KoeiTecmo, 시스템 WER Archive/Queue/Temp와 Application 이벤트를 조회했다. 11:48 배포 이후 PID26580에 해당하는 덤프·WER 보고서는 확인되지 않았다. 접근이 허용되지 않는 WER 파일 내용은 읽지 못했다. 가장 최신 기존 덤프의 헤더는 PID25124 / 10:31:17 KST이며 v6 배포 전의 별도 충돌이다. 다음 덤프는 PID44464 / 09:46:20이다.
- PID14564의 디버거는 접근 위반·힙 손상·fail-fast 예외만 중단하도록 연결했다. 이번 재현에서 해당 예외 중단은 없었다. CLR 초기 알림04242420과 처리된 C++ first-chance 예외를 치명적 충돌로 분류하지 않는다.
- RTTI TypeDescriptor RVA2E898C0의 실제 바이트 문자열은 `.?AVWarTurnEndState@war@san8r@@`이다. C++ 표기는 `san8r::war::WarTurnEndState`. 따라서 이 클래스명은 추정 이름이 아니다. vtable은2707880이다.
- RVA1D86520의 원래 함수 이름과 원래 소스 API 선언은 복구되지 않았다. `battle_turn_step`, `BattleTurnEnded`, `DispatchTurnEnd`는 우리 바인딩/SDK에서 붙인 이름이다. 이벤트 이름을 게임이 공개한 API명으로 설명하면 안 된다.
- 상태 팩토리 RVA1D89AE0의 점프 테이블1D8A520을 RTTI와 대조했다: 상태4=WarTurnBeginState(vtable27079D0), 상태5=WarTurnEndState(2707880), 상태6=WarUnitActState(27079A0), 상태7=WarUnitActBeginState(2707758), 상태19=WarResultState(2707A30).
- 실제 이벤트 경계는 상태5의 단계 함수1D86520에 진입할 때 stage==14인 호출의 원본 반환 직후다. 14단계1D86C6A는 1D4F080(root+68D0)을 호출하고 749100으로 상태6을 요청한다. 15단계1D86C80의 상태19 요청은 별도 결과 경로다. 앞서 14단계를 '다음 턴 시작'이라고 적은 설명은 정정한다. 건물 공격·연출까지 전부 끝났다는 보장은 이 연결에서 확인되지 않았다.
- 코드 변경은 위 상태 전환 설명 주석만 정정했다. 이번 분석에서 빌드·배포는 하지 않았다. PID26580 충돌 원인은 아직 미확정이다.

## 컴포넌트 갱신 시점 재판정 (2026-11-04, SDK 소스 v7)

현재 연결 지점은 사용자가 전투 화면에서 확인한 동작을 기준으로 **턴 시작**으로 판정한다. RTTI의 WarTurnEndState라는 내부 이름과 플레이어가 인식하는 턴 경계는 구분한다. 현재 후킹 주소나 호출 순서는 이동하지 않았다.

| 컴포넌트 이벤트 | 처리 경계 | 판정·연결 상태 |
| --- | --- | --- |
| BattleTurnStarted / IBattleTurnStartEffect.OnBattleTurnStarted | WarTurnEndState(상태5)의 stage14 원본 반환 후, WarUnitActState(상태6) 전환 요청 직후 | 사용자 관찰로 턴 시작 판정. 현재 연결된 이벤트 |
| BattleUnitActionEnded | WarUnitActEndState(상태16)의 행동 정리 완료 후 다음 상태 요청 경계 | 정적 분석 후보. 아직 바인딩하지 않음 |
| BattleTurnEnding | WarUnitActState(상태6)에서 다음 행동 부대가 없다고 판정하고 상태5로 전환하는 경계 | 정적 분석 후보. 아직 바인딩하지 않음 |
| BattleTurnEnded | 종료 처리와 건물 공격 등 후속 처리가 끝난 별도 경계 | 독립 경계는 미확정. 현재 이벤트로 제공하지 않음 |

- 상태6 Update RVA1D85B60은 컬렉션 메서드1E35FA0의 반환값을 사용한다. 반환값이 있으면 상태7(WarUnitActBeginState)을 요청하고, 없으면 RVA1D85D8F..1D85D9D에서 상태5를 요청한다. 이 후자의 분기를 턴 종료 **처리 진입** 후보로 구분한다. 종료 처리 완료라고 부르지 않는다.
- 상태16 Update RVA1D82CC0은 진행값+20에 따라 0단계1D82A40 / 1단계1D827F0을 처리한다. 1D827F0은 현재 부대·임시 참조 정리를 수행하고 보통1D82A0D에서 상태6을 요청한다. 같은 함수에는 상태17(WarInstantProcessState),18(WarPauseState),19(WarResultState) 분기가 있다. 단계 함수는 연출 대기 중 반복될 수 있으므로 Update 진입마다 컴포넌트를 호출하면 안 된다.
- 개별 부대 이벤트를 추가할 때는 정리 전 state+38의 해당 부대를 보관하고, 실제 상태 전환 경계를 통과했을 때 그 부대의 컴포넌트만 한 번 갱신하는 방식으로 검증한다. 전체 부대 조회·갱신을 재사용해 모든 부대 효과를 실행하지 않는다. 이동/공격/대기와 추가 행동의 호출 횟수는 아직 실시간 검증하지 않았다.
- 현재 WoundedRecoveryComponent는 턴 시작 이벤트에서만 실행한다. 전체 턴 시작 시 부대별 기재 효과를 평가하며, 부대당 같은 스크립트 효과는 한 번만 실행한다. 개별 부대 행동 종료·턴 종료 이벤트는 아직 없다. 일반적인 매 프레임 Component.Update 루프도 현재 구현에는 없다.
- 소스 이름을 BattleTurnEnded → BattleTurnStarted, IBattleTurnEndEffect → IBattleTurnStartEffect, OnBattleTurnEnded → OnBattleTurnStarted, DispatchTurnEnd → DispatchTurnStart로 변경했다. 효과 표시와 신규 생성 설명도 턴 시작으로 정정했다. 영속 효과 ID s8.battle.wounded-recovery는 유지한다.
- 이벤트 의미 변경이므로 API/SDK 소스 버전을7로 올렸다. 이벤트 번호13은 유지하며 v6 SDK 모드를 v7 호스트가 그대로 수용하지 않도록 어셈블리 참조 버전 검사를 사용한다.
- 소스 검토와 git diff --check만 수행했다. 빌드·설치 파일·기존 JSON·실행 중인 게임은 변경하지 않았다. 설치된 v6의 발동 시점은 원래부터 이 턴 시작 경계이며 이름만 이전 표기다.

## 개별 부대·전체 행동 종료 경계 실측 및 바인딩 (2026-11-04)

### PID14564 실제 추적

프레임별 Update 진입이 아니라 상태 전환 경계3곳에 자동 계속 중단점을 설치했다. 사용자가 손상향의 행동을 종료하고 다음 턴까지 진행했다. 이번 턴에는 건물 공격이 없었다.

1. UNIT_ACTION_BOUNDARY 6회: 0x22305476F38 → 0x22305476F78 → 0x22305476F68 → 0x22305476F48 → 0x22305476F58 → 0x22305476F88.
2. NO_MORE_UNITS 1회. root=0x2230545A070.
3. TURN_START_BOUNDARY 1회. root는 동일했다.

각 부대 포인터를 실행 메모리의 WarUnit 컬렉션과 대조했다. 전부 vtable=EXE+2718188이며 첫 포인터의 장수는 손상향565였다. 나머지는 각각 555/988/618, 959/185, 656/127/923, 684/787, 997/412였다. 전환 인자는 DL만 사용한다. 로그의 EDX=5126은 하위8비트6으로 해석하며 상태5126이 아니다. 확인 후 중단점3개를 모두 제거하고 실행을 재개했다.

### API 경계 정의

- BattleUnitActionEnded: 현재 부대의 WarUnitActEndState 단계1 정리 완료 후 상태를 전환한 경계. 이번 전투에서 부대별1회였다. 해당 부대만 이벤트 스냅샷에 노출한다. 전투 결과 상태로 종료하거나 정리 후 컬렉션에서 사라진 부대는 노출하지 않는다.
- BattleTurnEnded: 다음 행동 부대가 없어서 WarUnitActState가 WarTurnEndState로 전환한 뒤. **전체 부대 행동 완료**의 의미다. 뒤따르는 게임의 턴 전환 처리·모든 연출까지 완료됐다는 의미는 아니다. 건물 공격이 있는 턴의 화면상 순서는 별도 실측이 필요하다.
- BattleTurnStarted: 앞서 정정한 WarTurnEndState stage14 원본 반환 후. 다음 행동 선택 상태로 전환 요청한 경계다.

### 소스 연결

- battle_services.inl: WarUnitActState Update(1D85B60), WarUnitActEndState 단계1(1D827F0)에 바인딩을 추가했다. 원본 처리 뒤 살아 있는 상태 관리자+30을 확인한다. 파괴될 수 있는 이전 state는 원본 반환 후 역참조하지 않는다.
- dispatch_battle_event는 세 이벤트의 공통 스냅샷·토큰·실행 시간 계측을 처리한다. 개별 이벤트는 정리 전 포인터를 식별자로만 보관하고 정리 후 현재 컬렉션에서 다시 찾은 해당 부대만 읽는다. nullptr 대상이 전체 부대 갱신으로 바뀌지 않도록 제외한다.
- native_ui.cpp에서 기존 설치/해제 루프에 두 훅을 포함시켰다. game_binding.cpp의 진입 코드 검사에 두 함수의 원래16바이트를 추가했다. 범용 ChangeState 함수 전체는 후킹하지 않는다.
- C++/C# 이벤트 열거형에 BattleUnitActionEnded(14), BattleTurnEnded(15)를 추가했다. 턴 시작은13이다. SDK 소스는v7이다.
- IBattleUnitActionEndEffect.OnBattleUnitActionEnded / IBattleTurnEndEffect.OnBattleTurnEnded를 추가했다. 기존 IBattleTurnStartEffect와 함께 구현할 수 있다. 등록 시 인터페이스별 이벤트 마스크를 계산하고, 해당 이벤트를 처리하는 컴포넌트만 생성·호출한다.
- ModApplication → ScriptedTraitCatalog.DispatchBattleEvent → 대응 컴포넌트 인터페이스로 전달한다. 마왕의료 WoundedRecoveryComponent는 계속 턴 시작만 구독하므로 부대 종료·전체 행동 종료에서 중복 회복하지 않는다.

기존 게임 코드 경계의 실행 추적과 소스 검토, git diff --check를 완료했다. 새 바인딩의 빌드·설치·C# 이벤트 실행 시험은 아직 수행하지 않았다. 설치된 게임은v6이다. 변경 범위는 script_api.h 의존 네이티브4개cpp와 overlay/native_ui_bridge.cpp, C# SDK·Host·GameScripts다. 새 전투 훅 구현은 native_ui.cpp에 포함되는 inl에 한정한다.

### 마왕의료 전체 행동 종료 시험판 v7 적용 (2026-11-04)

- WoundedRecoveryComponent를 IBattleTurnEndEffect / OnBattleTurnEnded로 변경했다. 효과 표시명은 턴 종료 부상병 전량 회복, 신규 기재 설명은 전체 부대 행동 종료 시로 정정했다. 영속 효과 ID와 회복·연출 코드는 유지한다.
- 첫 증분 빌드는 S8.Scripting.Native.pdb 갱신 실패 LNK1201로 중단됐다. 기존 게임 디버거를 분리·종료하고 PDB의 배타적 쓰기 열기를 확인한 후 같은 증분 빌드를 재개하여 성공했다. clean/rebuild/빌드 설정 변경은 하지 않았다.
- 게임 종료 후 SDK7.0.0.0을 포함한18개 파일을 교체하고 SHA256 일치를 확인했다. 백업 C:\Users\lance\AppData\Local\Temp\S8-turn-end-v7-20261104-121803. 두 설정 JSON의 전후 해시는 동일하다.
- 실제 검증 항목: 손상향 개별 행동 종료에서는 회복하지 않음, 양측 전체 행동 종료 경계에서 한 번만 요청, 요청 전후 손상향 부대 병력과 회복 상한 대조, 이어지는 턴 시작에서 중복 발동 없음. 현재 재실행·전투 로드 대기 중이며 성공 판정은 아직 하지 않았다.

### v7 전체 행동 종료 실제 시험 결과 및 턴 시작 중복 발견

PID15684, root0x1B9B1FA00A0에서 개별 부대 종료(kind14)6회 뒤 전체 행동 종료(kind15)1회를 확인했다. 사용자도 마왕의료 '부상병을 치료하라!' 발동 연출이 동작한다고 확인했다. 대사 대기 중 손상향 부대는16799/17500이었다. 연출 종료 후 네이티브 병력 설정과 실제 메모리에서17500 도달을 확인했다(회복701). 이후 읽기에서는17099였으며, 다음 전투 진행 중 수치이므로 추가 감소 원인을 이번 추적으로 확정하지 않는다. 화면 관찰에서도 새 날짜 표시와 손상향17500을 확인했고 자동 입력은 하지 않았다.

같은 추적의 턴 시작(kind13)은97회였다. 기존 구현은 원본 함수가 실제 상태 전환을 했는지와 관계없이 진입 stage14만으로 이벤트를 발생시켰다. 원본은 연출·내부 대기 조건에서 stage14 상태로 반환할 수 있다. 이 중복은 이번에 발견한 별도 결함이며, 턴 종료만 구독하는 WoundedRecoveryComponent가 이 시작 이벤트에서 회복하지 않는 것은 확인했다.

battle_turn_step의 시작 이벤트 조건을 stage14에 더해 처리 후 상태 관리자+30의 객체가 WarUnitActState(vtable27079A0)인 경우로 보완했다. 이 소스 보완은 아직 빌드·배포·실행 검증 전이다. 진단 중단점은 제거했고 예외 감시만 남겼다. 로그는 C:\Users\lance\AppData\Local\Temp\S8-turn-end-v7-test-15684\trace.log에 있다.

턴 시작 중복 보완판은 추가로 발견한 결함을 수정한 뒤 Release x64 증분 빌드에 성공했다. 이번에는 디버거가 별도 symbols 복사본을 사용하여 출력 PDB 잠금은 없었다. 게임 종료 후 S8.Scripting.Native.dll/PDB 교체 대기 중이다.

사용자가 종료한 뒤 디버거를 분리·종료했다. 턴 시작 중복 보완판 S8.Scripting.Native.dll/PDB 2개를 게임에 교체하고 SHA256 일치를 확인했다. 백업 C:\Users\lance\AppData\Local\Temp\S8-turn-start-once-v7-20261104-122558. C#의 OnBattleTurnEnded 구독과 기존 설정 파일은 유지했다. 이 보완판의 중복 억제 실행 검증은 아직이며, 보완 전 v7에서 턴 종료 회복·연출 시험이 통과한 사실과 구분한다.

### 턴 시작 중복 억제판 실제 재확인 (2026-11-04)

- 사용자가 게임을 재실행한 PID32816에 설치된 보완판을 대상으로 이벤트 전달 함수만 추적했다. root=0x216CE669090.
- 이번 진행에서는 개별 부대 행동 종료3회, 전체 행동 종료1회, 턴 시작1회를 관측했다. 디버거 최종 카운터 t2=3 / t1=1 / t0=1을 직접 읽었다.
- 사용자 진행 후 추가10초 대기와 이어진 출력 확인 동안 턴 시작 호출은 늘지 않았다. 최종까지1회였다. 이전 시험의97회 중복이 이번 보완판 시험에서는 발생하지 않았다.
- 게임 Responding=true. 확인 후 중단점을 전부 제거하고 디버거를 분리·종료했다. 게임 실행은 유지했다. 로그 C:\Users\lance\AppData\Local\Temp\S8-start-once-test-32816\trace.log.
- 이번 작업은 실행 검증과 문서 기록만이며 소스 수정·재컴파일·DLL 교체는 하지 않았다.

## 전투 세션 동안 GameObject·Component 유지 (SDK 소스 v8)

- BattleWorld.cs 추가: 세션 ID별로 부대 GameObject(고정 부대 identity)와 장수 GameObject(장수 ID)를 유지한다. 매 이벤트 전체 명단을 읽어 기존 BattleUnitComponent/BattleMembershipComponent의 스냅샷만 갱신한다. EventUnits는 개별 행동 종료일 때 해당 부대만, 턴 경계일 때 전체 부대를 제공한다.
- ScriptedTraits.cs의 효과 캐시는 (장수 ID, 기재 ID, 효과 ID)로 인스턴스를 유지한다. 해당 효과가 붙어 있는 동안 OnEnable은 최초 생성 때만 호출된다. 이벤트에서는 저장한 컴포넌트의 대응 메서드만 호출한다. 같은 효과의 부대당 중복 실행 방지는 유지한다. 슬롯 이동만으로는 인스턴스를 교체하지 않는다.
- 기재·효과 변경은 다음 전투 이벤트에서 명단/설정과 대조해 반영한다. 제거된 효과는 GameObject.RemoveComponent로 OnDisable을 한 번 호출하고 다른 컴포넌트는 유지한다. 명단에서 사라진 장수·부대는 해당 GameObject를 Dispose한다. 장수가 다른 부대로 이동해도 같은 장수 객체와 효과 인스턴스를 유지하며 membership을 갱신한다.
- ScriptedTraitComponent를 효과의 공통 베이스 클래스로 사용한다. 인스턴스별 PersonalityId를 보유하며 GameObject.AddComponent 초기화 콜백에서 OnEnable 전에 지정한다. WoundedRecoveryComponent는 이 클래스를 상속하고 기존 OnBattleTurnEnded만 구독한다. 여러 효과가 하나의 가변 활성 기재 컨텍스트를 공유하지 않는다.
- BattleWorld.EndEvent는 객체를 Dispose하지 않고 네이티브 토큰만 무효화한다. 컴포넌트의 사용자 필드와 최근 스냅샷은 유지된다. 연출의 중첩 이벤트는 스코프 스택으로 바깥 CurrentEvent/EventUnits/토큰을 복원한다. finally로 이벤트 스코프를 닫는다.
- 네이티브 BattleEventContext는 session/sequence/acting_unit/event를 제공한다(32바이트). BattleUnitRecord는 토큰과 별개 고정 identity를 추가해48바이트다. 전체 명단을 제공하되 개별 부대 이벤트의 쓰기/연출은 acting_unit에만 허용한다. 콜백·세션이 다르거나 현재 identity가 아닌 토큰은 입구에서 거부한다.
- 고정 부대 identity는 네이티브 부대 포인터+모델을 추적하는 일련번호다. 현재 명단에서 빠진 항목은 제거하고 재등장하면 새 identity를 부여한다. 세션 키는 root/WarStateMachine/기존 GameEpoch이며, 달라지면 이전 세션을 해제한다. 로드 후 epoch가 변경됐지만 상태 머신을 재사용한 경우에는 다음 전투 콜백에서 정리한다.
- WarStateMachine vtable2707A60의 deleting destructor RVA751130을 정적 RTTI/코드로 확인하고 훅을 추가했다. 활성 세션의 관리자 파괴 시 BattleSessionEnded를 보내 C# BattleWorld 전체를 Dispose한다. 실제 게임 종료/로드 경로에서 이 새 소멸자 훅이 호출되는지는 실행 검증 전이다. ModApplication.Dispose(모듈 교체 포함)에서도 카탈로그와 World를 해제한다.
- script_api.h / Native.cs / S8.Scripting.csproj: ABI/SDK8, GetBattleContext 연산28, BattleSessionEnded 이벤트16 추가. game_binding.cpp에751130 원래16바이트 검사를 추가했다. native_ui.cpp 기존 훅 설치·해제 목록에 소멸자 훅을 포함했다.
- 회귀 검사 BattleLifetimeChecks.cs를 기존 tests/scripting/Program.cs에 연결했다. 객체/필드/효과 인스턴스 유지, OnEnable 초기값·1회 호출, 명단 제거·세션 교체·효과 제거의 OnDisable, 중첩 이벤트 토큰 복원, 재사용 identity의 세션 분리, 반복 Dispose를 다룬다.
- 이번 요청에서는 빌드·테스트·DLL 교체를 실행하지 않았다. 소스와 git diff --check만 검토했다. 현재 게임 설치는 여전히v7이다. 새로운 수명 모델과 소멸자 훅은 아직 실행 검증하지 않았다.
- 변경 파일: managed/S8.Scripting/{BattleWorld.cs,Battle.cs,GameObject.cs,Native.cs,S8.Scripting.csproj}, managed/S8.GameScripts/{ScriptedTraits.cs,ModApplication.cs,WoundedRecovery.cs}, battle_services.inl, script_api.h, native_ui.cpp, game_binding.cpp, tests/scripting/{BattleLifetimeChecks.cs,Program.cs}. API 공통 헤더의 직접 의존4개 네이티브cpp와 overlay/native_ui_bridge.cpp, C# SDK·Host·GameScripts가 재컴파일 대상이다.

## 로드된 게임 세션의 World / Scene 골격

### 소유권과 수명

GameWorld.cs에 로드된 게임 단위의 GameWorld, GameScene, OfficerComponent, CityComponent를 추가했다. GameWorld.SessionId는 기존 네이티브 GameEpoch를 사용하고, BattleWorld.SessionId는 개별 전투 식별자로 분리했다.

- GameWorld.GetOfficer/GetCity는 ID마다 같은 GameObject를 반환하는 지연 생성 레지스트리다. 전투 명단에서 빠지거나 전투가 끝나도 월드 엔티티는 파괴하지 않는다.
- GameSceneKind는 City / Council / Battle이다. 씬은 월드 엔티티에 대한 참조와 자신이 소유한 임시 객체를 구분한다. 씬 종료 시 소유 객체만 Dispose한다.
- GameObject.World / Scene에서 소유 관계를 확인한다. 장수·도시의 Scene은 null이고, 전투 부대 등의 Scene은 소유 씬이다. GameScene.Track는 참조 추가이며 소유권 이전이 아니다.
- GameWorld.BeginSession / EndSession, EnterScene / LeaveScene과 SessionStarted / SessionEnded / SceneEntered / SceneExited를 제공한다. 이들은 SDK 관리 알림이며 미분석 도시·평정 게임 API의 이름이 아니다.

### 기존 전투 경로 연결

BattleWorld는 GameWorld를 주입받고 부대만 씬 소유 객체로 생성한다. 장수는 GameWorld.GetOfficer로 기존 객체를 참조하며 BattleMembershipComponent로 전투 소속을 연결한다. 전투 종료·씬 이탈에서는 부대를 파괴하고 전투 소속만 제거한다. 장수와 기재 효과 컴포넌트는 남긴다.

ScriptedTraitCatalog는 GameWorld 전체 장수와 OfficerComponent.PersonalityIds를 기준으로 효과를 유지한다. BattleSessionEnded는 EndBattle만 호출한다. 로드된 세션 종료 때 월드가 모든 엔티티를 파괴하며, 효과 캐시도 SessionEnded에서 비운다. 모듈 해제 시에는 카탈로그, 월드 순으로 정리한다.

BattleEventContext에 world_session을 추가해40바이트로 변경했다. 기존 전투 세션·콜백 토큰·sequence와 별개로 GameEpoch를 전달한다. SDK v8은 아직 빌드·배포하지 않은 작업 버전이므로 같은v8 안에서 구조를 갱신했다. 게임 설치는 계속v7이다.

### 연결 범위와 검증 상태

도시·평정은 논리 씬과 엔티티 관리 골격만 추가했다. 실제 도시 진입, 평정 시작·종료, 타이틀 복귀, 게임 로드 직후의 공통 상태 감지와 컴포넌트 이벤트는 아직 네이티브에 연결하지 않았다. 현재 월드 세션 동기화는 전투 콜백이 전달하는 GameEpoch를 사용한다. 도시 상태에서 다른 세션을 불러온 즉시 월드를 교체하려면 추후 검증한 해당 경계에서 BeginSession/EndSession을 호출해야 한다. 스크립트 상태의 세이브 파일 영속화도 이번 범위에 포함하지 않았다.

기존 회귀 검사를 소유 관계에 맞게 변경했다. 도시→평정에서 장수·도시 인스턴스와 필드 유지, 전투 종료 시 소속만 제거, 다음 전투의 같은 장수 재사용, 로드된 세션 교체 때만 장수 해제를 추가했다. 이번에도 빌드·테스트는 실행하지 않았으며 소스 검토와 git diff --check만 수행했다.

변경 파일: GameWorld.cs(신규), GameObject.cs, BattleWorld.cs, ScriptedTraits.cs, ModApplication.cs, battle_services.inl, tests/scripting/BattleLifetimeChecks.cs. 재컴파일 범위는 native_ui.cpp와 C# SDK·Host·GameScripts 및 회귀 검사다. 공통 네이티브 헤더, 빌드 설정, 설치 DLL·설정 JSON은 이번에 변경하지 않았다.
## 주인공 GameObject 참조

- GameWorld.Protagonist와 ProtagonistComponent를 추가했다. 참조는 GetOfficer(주인공ID)와 같은 인스턴스이며 별도 장수 객체를 중복 생성하지 않는다. 컴포넌트에서는 GameObject.World.Protagonist로, ModApplication에서는 공개 World.Protagonist로 접근한다.
- BeginSession(id, protagonistId)는 주인공 객체와 역할 표식을 먼저 준비한 뒤 SessionStarted를 알린다. ObserveProtagonist는 관리 참조를 갱신하며 게임의 주인공을 변경하는 명령이 아니다. 같은 장수면 그대로 유지하고 주인공 교체 시 표식만 옮긴다. 이전 장수와 그 컴포넌트 상태는 월드에 남긴다. ProtagonistChanged를 제공하며 세션 종료에서는 참조를 null로 비운다.
- 기존 ConfigSnapshot.protagonist_id의 검증된 게시값을 overlay/native_ui_bridge.cpp에서 네이티브 UiPublication으로 전달한다. ready가 아니거나 ID가 없으면0xFFFF이며 장수ID0은 유효하다. 기존112바이트 구조체의 말미 여유2바이트를 사용한다. UI 호스트 계약은3으로 올렸으며 관련 직접 의존은 native_ui.cpp와 overlay/native_ui_bridge.cpp다.
- GetWorldContext 연산29는 epoch/주인공ID의16바이트 읽기 전용 스냅샷을 제공한다. 현재 게임 스냅샷을 읽으므로 과거 UI 편집 세션 epoch 일치 여부에 의존하지 않는다. 새 메모리 오프셋 추측이나 C# 직접 메모리 읽기를 추가하지 않았다.
- GameWorld.RefreshSession을 ModApplication 초기화와 각 스크립트 콜백 진입에 연결했다. 알려진 주인공과 epoch로 월드를 준비하며 다른 세션이면 이전 월드를 정리한다. 주인공을 읽지 못한 같은epoch의 일시적 대기에서는 기존 참조를 보존한다. Protagonist 속성 자체는 캐시된 참조이며 네이티브 호출을 하지 않는다.
- 로드 완료 순간의 독립적인 전용 엔진 훅을 새로 추가한 것은 아니다. 실제 스크립트 초기화/콜백 진입 때 기존 publication을 동기화한다. C# 코드가 전혀 호출되지 않는 동안 즉시 관리 월드 알림을 보내는 기능은 후속 세션/씬 경계 연결 범위다.
- 회귀 검사에 주인공 인스턴스 동일성, SessionStarted 전에 참조 준비, 역할 교체 시 장수 상태 유지, 세션 재로드 시 새 객체와 참조 정리를 추가했다. 빌드·테스트·배포는 수행하지 않았고 소스와 git diff --check만 검토했다. SDK 작업 버전은v8, 현재 설치는v7이다.
- 이번 변경 파일은 GameWorld.cs, ModApplication.cs, Native.cs, script_api.h, native_ui.h, native_ui_services.inl, overlay/native_ui_bridge.cpp, tests/scripting/{BattleLifetimeChecks.cs,Program.cs}다. 공통 API 헤더를 포함한 네이티브cpp들과 C# SDK·Host·GameScripts가 재컴파일 대상이다.


### World entity lookup (B안)

컴포넌트 초기 추가는 [GameSetup/WorldSetup 자동 초기설정](Script_Setup_Lifecycle.md)을 사용한다.

세션 이벤트 작성과 연출 API의 구현 범위는 [Scripted_World_Events.md](Scripted_World_Events.md)를 참고한다.

`world.GetOfficer(id)` / `world.GetCity(id)`는 현재 로드 세션을 확인한 뒤 네이티브 엔티티를 조회한다.
장수는 게임 루트의 PersonData 포인터 테이블(+0x21BDF0), 도시는 CityData 테이블(+0x546808)을 사용한다.
네이티브 입구에서 ID, 실제 레코드의 타입과 ID를 확인하고 게임의 IsValid를 호출한다.
장수 ID 범위는 1..5200, 도시 ID 범위는 1..51이다. 장수의 유효성은 생존 여부와 다르다.
해당 레코드가 없거나 유효하지 않으면 KeyNotFoundException이며 GameObject를 새로 등록하지 않는다.

동일 세션의 동일 종류·ID는 동일 GameObject를 반환한다. 재조회는 OfficerComponent/CityComponent의
Memory 참조를 갱신하고 사용자가 추가한 컴포넌트와 필드를 유지한다. 조회 실패 시 기존 참조는 무효화된다.
로드 세션이 변경되면 기존 객체를 해제하고 새 객체를 만든다. Protagonist도 같은 장수 레지스트리를 사용한다.
FindOfficer/FindCity는 이미 등록된 객체만 찾는 캐시 조회이며 네이티브 존재 확인을 수행하지 않는다.
전체 장수·도시를 주기적으로 스캔하지 않고 Get 호출 시 해당 ID만 조회한다.

```csharp
var officer = world.GetOfficer(officerId);
officer.AddComponent<MyOfficerComponent>();

var city = world.GetCity(cityId);
city.AddComponent<MyCityComponent>();
```

이 변경은 소스 ABI v8에 포함되며 빌드·게임 적용·실행 검증은 아직 수행하지 않았다.
조회/동일 객체 유지/네이티브 주소 갱신/없는 도시 처리에 대한 테스트 코드를 함께 갱신했다.
