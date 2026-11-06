# 도시·내정 스크립트 생명주기 분석 (2026-11-04)

## 분석 기준

- 대상: `C:\Users\lance\Documents\SAN8RPK-analysis-20261104\SAN8RPK.exe.unpacked.exe`
- SHA-256: `f47c1112dfa1fa4067fde43ad2e4fd906bbba341d47fda4f6ef1724be4df43c1`
- 도구: REA 6.8.0 + Ghidra 12.1.4 + JDK 25. 12.2는 현재 REA 공급자가 지원하지 않는다.
- REA 자동 분석은 기본 설정 900초, 아래 제한 설정 600초 모두 시간 초과했다. REA에서 디컴파일 완료 결과를 얻었다고 주장하지 않는다. 제한 시도는 함수 주소 13개를 seed로 지정하고 `Non-Returning Functions - Discovered`, `Basic Constant Reference Analyzer`, `x86 Constant Reference Analyzer`, `Disassemble Entry Points`, `Decompiler Switch Analysis`를 제외했다. 별도로 Ghidra 12.1.4의 자동 분석을 끄고 PE 예외 테이블의 함수 범위로 디스어셈블·디컴파일하여 11개 본문을 얻었다. 전체 자동 분석으로 간주하지 않으며 switch 분기는 원본 명령어로 대조한다. 원형과 일부 인자는 추론된 상태이므로 의사코드를 복원된 원본 C++로 간주하지 않는다.
- 이미지 기준 주소: `0x140000000`. 아래 주소는 모두 RVA.
- RTTI의 클래스 이름은 EXE에 실제로 존재한다. 아래 제안하는 C# 이벤트 이름과 함수 역할 이름은 모드 API를 위한 이름이다.
- 정적 분석과 실행 검증은 구분한다. 이번 작업에서는 실행 중인 게임 프로세스를 찾지 못했다.

## 연결 판단

원본 상태 처리 이후 같은 실행 흐름에서 C# 컴포넌트를 호출하는 구조는 가능하다. 다만 상태 함수의 매 반환은 활동 완료가 아니다. 반복 처리, 연출 대기, 취소, 다음 상태 전환을 구분해야 한다.

기존 전투 구현도 원본 호출을 먼저 수행하고 살아 있는 manager의 다음 상태를 확인한 뒤 이벤트를 보낸다. 도시·내정도 이 원칙으로 연결해야 한다. 상태 전환이 기존 state를 해제할 수 있으므로 원본 호출 전에 대상 ID와 manager를 확보하고, 반환 후 해제된 state를 읽지 않는다.

| 요청 시점 | 확인한 원본 경로 | 정적 판단 |
|---|---|---|
| 휴양 완료 | `CityStateRest`, vtable `0x2698838`, 처리 `0x182E690` | 확인 함수 `0x16710`의 반환값 0은 상태 3으로 돌아가고, 0이 아닌 경로는 추가 처리 뒤 상태 45로 간다. 주인공이 없는 경로도 45로 간다. 상태 45만으로 성공을 판정할 수 없고 주인공·확인 결과를 함께 구분해야 한다. 단순 함수 진입은 완료 시점이 아니다. |
| 내정 활동 완료 | `CityStateDomesticAffairs`, vtable `0x2698EF8`, 처리 `0x1841070` | `state+0x28`의 1~6단계 switch와 UI 대기 조건이 있다. 단계 3은 `0x1796590`·`0x17A8B10`을 호출한 뒤 상위 manager 상태 3을 요청한다. 단계 6의 `0x18411E1`도 상태 3을 요청하고 단계를 0으로 만든다. 같은 다음 상태여도 실제 활동 실행 여부가 다르다. 이 함수 전체에 무조건 완료 콜백을 붙이면 중복·조기 호출된다. 성공/취소는 하위 결과 경로 추가 구분 필요. |
| 도시 진입 완료 | `CityStateEnter`, 처리 `0x183F890` | `state+0x20` 단계 0에서 리소스를 만들고 10 초과에서 해제·전환한다. 해당 분기에서 `0x168A00(...,3)` 호출. 진입 요청과 조작 가능 시점은 분리해야 한다. |
| 외출 | `CityStateLeave`, 처리 `0x1835FE0` | UI·장면 정리 이후 `0x168A00(...,3)`로 전환. 이 경로만으로 목적지 도착까지 완료했다고 볼 수 없다. 출발 이벤트 후보로 분리한다. |
| 교류 시작 | `CommunicateEnterState`, 처리 `0x1868C70` | 원본은 `0xECA90(manager,4)` 호출. 교류 상태 전환 경계 후보이며 실제 화면 준비 완료 여부는 별도 확인 필요. |
| 담화 처리 후 | `CommunicateTalkState`, 처리 `0x185E910` | `0x185BC50` 원본 처리를 실행한 뒤 `0xECA90(manager,5)` 호출. 원본 처리 후 이벤트 후보. 플레이어가 대사창을 닫은 시점과 같은지는 실행 추적 필요. |
| 교류 종료 | `CommunicateExitState`, 초기화 `0x1868990` | 반복 처리 `0x1868980`과 종료 슬롯 `0x1868970`은 빈 함수. 초기화 쪽에 정리 로직이 있다. Exit라는 이름으로 빈 슬롯을 후킹하면 실제 종료를 포착할 수 없다. |
| 평정 진입 | `ConferenceInitializeState`, 처리 `0x1980F20` | `state+0x20` 0일 때 날짜·세력 문맥을 설정하고 1로 변경한 뒤 `0xECA90(manager,2)` 호출. 원본 문맥 준비 후 진입 이벤트 후보. |
| 평정 종료 | `ConferenceExitState`, 초기화 `0x198D670` | 반복 처리 `0x198D660`은 빈 함수. 초기화 쪽에서 전역 필드 `+0x7280`과 UI를 정리한다. 정리 후 상위 평정 상태가 실제로 빠지는 경계까지 확인해야 한다. |
| 내정 데이터 갱신 | `WorkUpdateDataState`, 처리 `0x1E8AD50` | `0x1726190` 실행 후 `0x1C090(manager,10)`로 전환. 작업 결과 후 콜백 후보이며 월초 이벤트와는 다르다. |
| 월 시작·날짜 진행 | `StrategyPhaseGameState`, `PersonalPhaseGameState` 및 날짜 setter `0x1706DF0` | phase 처리 함수는 반복 실행되며 날짜 setter는 날짜 필드만 쓴다. 월간 일괄 처리가 끝난 경계는 아직 확정하지 않았다. setter나 phase 진입을 MonthStarted로 공개하면 안 된다. |
| 취소 | 내정·교류의 하위 결과 경로 | Exit 경로만으로 성공과 취소를 구분할 수 없다. 결과 코드와 호출 경로의 추가 분석이 필요하다. |

## C# 호출과 게임 효과 적용

이벤트를 통해 세션 월드에 유지되는 GameObject의 컴포넌트 상태를 갱신할 수 있다. 장수·도시의 수치 변경은 현재 제공된 네이티브 API를 호출하는 방식으로 적용한다.

새 대화·선택지·장면 전환은 원본의 정리 또는 상태 전환 중간에 재진입시키지 않고, 기존 전환이 끝난 뒤 게임 상태 머신이 받아들일 경계에서 시작해야 한다. 단순 C# 내부 상태 갱신과 새 게임 연출을 시작하는 것은 연결 조건이 다르다. 백그라운드 반복 조회로 이 시점을 흉내 내지 않는다.

이번 변경에서는 미확정 도시·내정 이벤트 후킹을 추가하지 않았다.

## UI 정리

- `EditorMenusSetup.cs`: 시작/인게임 편집 메뉴의 `마왕의료 추가` 항목 제거.
- `ModApplication.cs`: 해당 메뉴 항목의 전용 클릭 분기 제거. 기재 데이터와 회복 효과 자체는 유지.
- `feature_menu.cpp`: 기능 메뉴의 치트 편집 버튼과 원형 ImGui 런처 및 런처 문맥 메뉴 제거. 네이티브 서비스 초기화와 편집 허용 상태 판정은 유지.
- `overlay_present.cpp`의 `VK_OEM_3` 단축키 진입은 유지.
- 빌드·테스트·게임 DLL 교체는 실행하지 않았다.

## 상태 교체 함수의 명령어 확인

`0xECA90`과 `0x168A00`은 동일한 형태의 상태 교체 코드를 가진다. manager의 가상 함수 `+0x18`이 허용할 때만 실행하며, `manager+0x38`에 전환 중 표시를 세운다. 이전 `manager+0x30`을 비우고 기존 state의 `vtable+0x18`을 호출한 뒤 기존 참조를 정리한다. 이어 새 state를 만들고 `manager+0x30`에 저장한 후 새 state의 `vtable+0x08`을 호출한다. 마지막으로 전환 중 표시를 내린다.

따라서 callback을 상태 교체 함수의 중간에 넣으면 이전 상태 정리와 새 상태 초기화가 끝나지 않은 채 실행된다. 제안하는 위치는 원본 호출 반환 이후이며 manager의 실제 상태 교체를 확인한 뒤다. 함수가 호출되었다는 사실만으로 전환 성공을 판정하면 안 된다.




## 직접 디컴파일 결과

별도 Ghidra 실행은 2026-11-04 15:51에 완료됐다. 결과 디렉토리는 `C:\Users\lance\Documents\SAN8RPK-analysis-20261104`이며 `ghidra-<RVA>.c` 11개와 `focused-ghidra.log`, 범위를 지정한 `lifecycle-ranges.tsv`, 분석 스크립트 `LifecycleBodies.java`를 남겼다. 빈 함수는 PE 예외 테이블 범위가 없어 이 목록에서 제외했고, 원본 명령어의 `ret`로 확인했다.

직접 디컴파일 성공 RVA: `182E690`, `1841070`, `185E910`, `1980F20`, `1E8AD50`, `183F890`, `1835FE0`, `198D670`, `1868990`, `ECA90`, `168A00`.

내정 단계 3에서 호출되는 `0x17A8B10`의 세부 처리·성공 반환 계약은 아직 복원하지 않았다. 따라서 이 호출 뒤 경계를 활동 결과 이벤트 후보로 식별한 것이며, 모든 내정 활동에 대한 성공 판정까지 확정한 것은 아니다.

## 실행 계측: 휴양 경로

2026-11-04 실행 중인 게임에서 취소 1회, 정상 휴양 2회를 계측했다. 취소는 선택 반환 0 후 도시 명령 상태로 복귀했다. 정상은 선택 반환 1 후 `CityStateExit`로 전환되며 휴양 함수 반환 시점에는 날짜가 이전 달이었다.

두 번째 정상 시험에서 확인된 순서는 다음과 같다.

1. `0x182E8FE`: 휴양 확인 결과 1, 날짜 5월.
2. `0x182EC0B`: 휴양 함수 반환 직전, `CityStateExit`, 날짜 여전히 5월.
3. `0x1BDCAD1`: 월 byte를 6월로 변경. 연도 word 저장 `0x1BDCADC`도 같은 날짜 영역을 쓰므로 watchpoint는 2회 발생했지만 날짜 변화는 1회다.
4. `0x183FAAD`: `CityStateEnter`의 상태 교체 반환 후 `CityStateCommandMain` 확인, 날짜 6월. 1회 발생.

사용자의 다음 달 진입 보고 후에도 선택 반환·휴양 반환·도시 진입은 각각 1회였고 추가 호출은 없었다. 이 결과는 휴양으로 개인 도시 화면에 재진입하는 경로에 한정된다. 평정 등 다른 월 경계도 포함하는 보편적인 월 시작 이벤트는 아직 확정하지 않았다. 휴양 함수 반환을 월 시작 완료로 쓰지 않는다.

### 6월→7월 평정 진입 전 이벤트 계측

6월→7월 휴양에서는 월 진행 함수 `0x1BDC950`의 진입·반환이 각각 1회였고, 해당 객체는 RTTI `InGameState`였다. 반환 시 날짜는 7월, 다음 진행 모드는 2였다.

사용자가 평정 진입 설전에서 기다린 시점의 실제 스택은 `StrategyPhaseGameState` → `UserStrategyPhaseGameState` → `conference::ClashOfOpinionPerformState` → `CmnEventInputWaitState`였다. 이때 도시 명령 준비 지점과 `ConferenceInitializeState` 후보는 모두 0회였다. 월 진행 함수 반환은 이 설전보다 앞이며, 전략 단계 진입과 평정 명령 화면 준비도 분리해서 판정해야 한다.


설전 종료 후 사용자가 평정 명령 선택 화면에서 기다린 시점에는 `ConferenceInitializeState`의 문맥 초기화 지점이 1회 발생했고, 월 진행 진입·반환은 각각 1회로 유지됐다. 상위 스택은 `ConferenceGameState`, 내부 manager의 현재 상태는 RTTI `ConferenceMainCommandState`(vtable `0x26A7B18`, kind 3)였다. 따라서 진입 설전과 실제 평정 명령 준비는 분리된다. 초기화 지점 자체가 UI 준비 완료와 같은 순간인지는 아직 확정하지 않았다.

### 평정 종료→도시 복귀 계측

평정을 정상 종료한 시험에서 다음 순서를 확인했다. 각 지점은 1회 발생했고 도시 화면에서 기다린 뒤에도 추가 발생은 없었다.

1. `0x198D670`: `ConferenceExitState` 초기화 진입, 스레드 `0x7810`.
2. `0x1898965`: `ConferenceGameState` 종료 처리 함수의 반환 직전, 스레드 `0x31B0`.
3. `0x183FAAD`: 도시 상태 교체 후 `CityStateCommandMain`, 스레드 `0x7810`.

날짜는 모두 7월 그대로였고 월 진행 함수의 추가 호출은 0회였다. 최종 상위 스택은 `PersonalPhaseGameState`/`UserPersonalPhaseGameState`였다.

특히 화면 종료 함수의 반환 스레드가 다르다. 해당 반환에서 곧바로 관리 컴포넌트나 게임 API를 실행해도 된다고 확정하지 않는다. 후킹 경계에서 사실을 수집하는 처리와 관리 컴포넌트·게임 API 실행 경계를 구분하고, 단일 실행 경로를 확인한 뒤 연결해야 한다. 아직 이 이벤트를 소스에 연결하지 않았다.

### 내정 보수 NPC 대화 계측

보수 확인창에서 취소한 동일 객체는 단계 6으로 1회 진행했고 실제 활동 처리 반환 지점은 0회였다. 이후 보수를 결정하고 노동자 NPC 대화창에서 기다린 시점에는 내정 상태가 단계 3, 활동 분류가 `0x15`였으며 원본 활동 함수의 반환은 아직 0회였다.

실제 스택은 `CmnEventState`/`CmnEventInputWaitState`였고, 스레드 `0x7810`의 호출 스택에서 원본 함수 안의 `0x17A9A54` → 복귀 주소 `0x1840C73`를 확인했다. 원본 `0x17A8B10`이 대화 입력을 기다리는 중이며 함수 반환 이후 콜백과 NPC 대화 시점은 구분된다. 이 관측만으로 수치 쓰기가 대화 전후 어느 순간인지까지 판정하지 않는다.


보수 결과의 자동 수치 연출이 끝나 성문 메뉴로 복귀한 뒤에는 `0x1840C73`(원본 활동 함수 반환 직후)과 `0x1840C8C`(상위 도시 상태 교체 후)가 각각 1회 발생했다. 둘 다 스레드 `0x7810`이었고, 다음 상태는 `CityStateCommandMain`이었다. 동일 확인창에서 취소한 시험은 두 지점 모두 0회였다. 보수에 한해 대화·자동 연출을 포함한 원본 처리 이후의 완료 콜백 후보가 확인됐다. 다른 내정 활동을 전부 검증한 것으로 확대하지 않는다. 아직 C# 이벤트는 연결하지 않았다.

## 교류·담화 실행 계측

교류 명령 선택 화면의 내부 상태는 `CommunicateMainCommandState`(vtable `0x2699AF0`, kind 5)였고 담화 시작은 0회였다. 담화를 누르자 `CommunicateTalkState`(vtable `0x2699868`, kind 6)로 들어갔으며, 관심 있는 이야기를 나누는 랜덤 이벤트에서는 `PersonSpeechState`/`CmnEventInputWaitState`가 올라왔다.

그 시점의 스레드 `0x7810` 호출 스택은 원본 담화 처리 안의 `0x185D1F8` → 복귀 주소 `0x185E954`였다. 랜덤 이벤트는 원본 `0x185BC50`의 반환 전에 실행 중이었다.

이벤트를 마치고 교류 메뉴로 복귀하자 담화 함수 반환 직전 `0x185E9D3`이 1회 발생했고, 현재 상태는 다시 `CommunicateMainCommandState`였다. 교류 종료 카운터는 증가하지 않았다. 따라서 이번 시험에서는 랜덤 이벤트를 포함한 담화 완료와 교류 종료가 분리된다. 새 C# 이벤트는 아직 연결하지 않았다.

### 교류 종료와 기존 도시 화면 재개

떠나기 후 도시로 복귀한 시험에서 `0x1868990` 진입과 `0x1868A76` 반환 직전은 각각 1회 발생했다. 두 지점 모두 스레드 `0x792C`였고 날짜는 7월 그대로였다. 월 진행은 0회, `CityStateEnter`의 상태 교체 완료 지점도 0회였다.

복귀 후 전체 스택에서는 `CommunicatePhaseGameState`가 사라졌고, 방문 전과 동일한 `PersonalPhaseGameState`와 `UserPersonalPhaseGameState` 객체가 유지됐다. 따라서 이번 교류 종료는 기존 도시 문맥을 재개하는 경로이다. 월 변경·평정 종료에서 확인한 도시 진입 지점을 모든 도시 복귀에 공통 적용하면 이 경로를 놓친다.

특히 종료 초기화 진입과 반환 때 모두 manager의 전환 중 표시(`+0x38`)가 1이었다. 담화 완료의 스레드 `0x7810`과도 다르다. 종료 초기화 함수가 반환됐다는 이유로 상태 교체 전체가 끝났다고 판정하거나 그 위치에서 직접 C# 컴포넌트·게임 UI를 호출하지 않는다. 전환 완료 이후 직렬화된 실행 경계를 추가로 확인해야 한다.

이 시험까지의 추적점은 모두 제거하고 게임을 재개했다. 디버거 연결은 유지했지만 활성 추적 중단점은 없다. 새 네이티브 후킹이나 C# 이벤트 연결은 아직 하지 않았다.

## C# 실제 연결 및 실행 검증 (2026-11-04)

`DomesticActivityCompleted`(현재 검증 범위: `GateRepair=0x15`)와 `TalkCompleted`를 네이티브 원본 처리 반환 이후 연결했다. 컴포넌트에서는 `IDomesticActivityCompletedHandler`와 `ITalkCompletedHandler`를 구현하며, 월드 이벤트 및 선언형 이벤트 트리거도 함께 제공한다. API/SDK 계약은 v9다.

- 네이티브: `personal_activity_services.inl`, `native_ui.cpp`, `game_binding.cpp`, `script_api.h`.
- C#: `PersonalActivities.cs`, `GameObject.cs`, `GameWorld.cs`, `WorldEvents.cs`, `Native.cs`, SDK 프로젝트 버전, `ModApplication.cs`.
- 검증: `PersonalActivityProbe.cs`, `tests/scripting/PersonalActivityChecks.cs`, `Program.cs`.
- 재컴파일 범위: 네이티브 스크립팅 DLL과 계약 헤더 사용 번역 단위, SDK 및 이를 참조하는 Host/GameScripts/검증 프로젝트. 전체 clean/rebuild는 하지 않았다.

Release x64 증분 빌드 성공. ABI/컴포넌트 검사 132개 통과. 게임 종료 후 배포하고 DLL 해시를 비교했다. 실제 UI 조작은 computer-use로 수행했으며, 중단점 없이 OutputDebugString과 C# 기록을 관찰했다.

### 실제 게임 결과

시험 전 상태는 빈 34번 수동 저장 슬롯에 보존했다(항적, 192년 7월, 완, 금190, 방어841). 기존 저장 슬롯은 덮어쓰지 않았다.

1. 보수 확인창 취소: C# 완료 이벤트 0회. NPC 대화 중에도 0회.
2. 보수 대화와 자동 수치 연출 종료 후 성문 메뉴 복귀: 완료 1회. 방어841→854, 금190→180.
3. 자택 → 교류 → 이풍 → 담화: 담화 분위기 결과뿐 아니라 사냥 권유 랜덤 이벤트까지 마친 뒤 완료 1회. 해당 이벤트/대화 도중에는 완료 0회.
4. 첫 실행에서 담화 문맥 `+0x60`이 상대이고 `+0x68`이 주인공임을 확인해 ID 순서를 수정했다. 수정판 빌드·배포·재실행에서 주인공2008, 상대919가 정확하게 전달됐다.
5. 수정판 동일 세션에서 담화 후 보수 실행: 같은 컴포넌트 instance `a823f72c5e534077af4262e7c1041a99`, session2, `total=2 repairs=1 talks=1`. 두 이벤트 모두 `sameObject=True`, 네이티브 API로 읽은 정치24. 교류 종료나 메뉴 대기에서 중복 증가는 없었다.

수정판 기록:

```text
event=18 officer=2008 value=919 success=1 nativeThread=9792 dispatchUs=14372
event=17 officer=2008 value=21 success=1 nativeThread=9792 dispatchUs=11402
```

위 시간은 진단 컴포넌트의 파일 기록·조회와 첫 핸들러 실행을 포함한 전체 관리 호출 시간이다. 프레임/오디오 성능 통과 판정이나 일반적인 반복 실행 비용으로 해석하지 않는다. 진단 컴포넌트는 `scripts/lifecycle-probe.enabled`가 있을 때만 세션에 추가되며 게임 데이터를 쓰지 않는다.

근거 파일은 `C:/Users/lance/Documents/SAN8RPK-analysis-20261104/`의 `lifecycle-smoke-20261104.log`, `lifecycle-build-20261104.log`, `lifecycle-fix-build.log`, `lifecycle-jit-debug.log`, `lifecycle-jit-fixed.log`, `lifecycle-probe.log`다. 앞 절의 '아직 C# 연결하지 않았다'는 해당 계측 단계 당시의 기록이다.
재로드 session4: 새 instance b0543e987edb4b37b2c75aa272ad6af2, 모든 카운터0 확인. 초기화는 로드 이후 첫 관리 이벤트의 RefreshSession에서 수행된다. 진단 마커 제거 후 기준 세이브를 다시 로드해 진단 컴포넌트를 비활성화했다.

## 월변경·평정 진입 C# 연결 및 실행 검증 (2026-11-04)

ABI v10에서 `IMonthChangedHandler.OnMonthChanged(MonthChangedInfo)`와 `ICouncilEnteredHandler.OnCouncilEntered(CouncilEnteredInfo)`를 추가했다. 월드 이벤트 `MonthChanged`, `CouncilEntered`, 스크립트 이벤트 트리거 `world.month.changed`, `council.entered`도 같은 경계에서 발행한다. 세션에 이미 생성된 장수·도시 GameObject의 컴포넌트에 전달하며 컴포넌트를 재생성하지 않는다.

- 월변경: RVA `0x1BDC950` 원본 반환 뒤 날짜가 바뀐 경우 전달한다. 이전/현재 연월을 포함한다. 다음 도시/평정 화면 준비보다 앞선 경계다.
- 평정 진입: RVA `0x197A1F0` 원본 반환 뒤 현재 상태가 명령 상태(vtable RVA `0x26A7B18`)이고 상태 전환 중이 아닐 때 전달한다. 이번 시험에서는 성과·포상·평정 시작 대사를 모두 마친 명령 화면 진입 시점이다.
- 초기 연결했던 `0x1980F20`은 명령 상태로 직접 가지 않았다. 반환 직후 계측 결과 `ready=1`, `changing=0`, 다음 vtable `0x26A7F38`이었다. 그 중간 상태의 step `0x197A1F0`이 kind3 명령 상태(특수 플래그가 있으면 kind0x3C)로 전환한다. 따라서 연결 위치를 옮겼다. kind0x3C 경로와 평정 세이브 직접 로드는 이번 실행 검증 범위에 포함되지 않는다.

실제 게임 UI를 조작하고 JIT 진단 컴포넌트로 확인했다.

1. 첫 실행에서 192년7→8→9→10월: 같은 instance `9f612f2aa12b436286a1d6f39d8a6cce`에서 월 카운터1→2→3. 휴양 확인 취소 때는 월변경 없음.
2. 최종 수정판 9→10월: instance `77622fd937ab4a639105d614ba455d2b`, session2. 월변경 `total=1 months=1 councils=0`, 평정 명령 진입 `total=2 months=1 councils=1`. 두 호출 모두 `sameObject=True`, 정치24 조회 성공, 관리 thread2.
3. 평정 시작 대사 대기 중에는 평정 진입 호출이 없었다. 명령 화면에서 인사 메뉴를 열고 복귀한 후에도 councils1 유지.
4. 최종 native 출력: event19 success1 thread48412 dispatch5251us, event20 success1 같은 thread dispatch14895us. 이 시간은 JIT·파일 기록·장수 조회를 포함하므로 일반 실행 비용이나 오디오 성능 판정으로 사용하지 않는다.

Release x64 증분 빌드 성공. C# 변경 후 ABI/컴포넌트 검사 140개 통과. 실제 진입 실패를 발견해 네이티브 연결을 수정한 후 최종 증분 빌드도 성공했다. 최종 DLL 배포 후 SHA256 일치를 확인했다. 새 ABI 때문에 네이티브 계약 사용 번역 단위와 SDK·Host·스크립트 소비자가 재컴파일 대상이다. 실제 마지막 연결 수정은 native_ui.cpp와 game_binding.cpp 및 네이티브 링크 범위다.

변경 파일: `src/scripting/script_api.h`, `game_binding.cpp`, `native_ui.cpp`, `personal_activity_services.inl`; `src/scripting/managed/S8.Scripting/{Native.cs,PersonalActivities.cs,WorldEvents.cs,S8.Scripting.csproj}`; `src/scripting/managed/S8.GameScripts/PersonalActivityProbe.cs`; `tests/scripting/{PersonalActivityChecks.cs,Program.cs}`.

진단 마커를 제거하고 수동 슬롯34(192년7월)를 다시 불러왔다. 기재 편집 목록을 변경 없이 열어 세션 갱신을 반영한 뒤 도시 조작 화면으로 복귀했다. 수동 슬롯34를 덮어쓰지 않았다. 시험 중 정상 자동 저장은 생성됐다. 디버거 중단점을 지우고 분리했다.

근거: `C:/Users/lance/Documents/SAN8RPK-analysis-20261104/`의 `month-council-probe.log`, `month-council-fixed-live.log`, `month-council-entry-live.log`, `month-council-smoke.log`, `month-council-entry-build.log`, `month-council-entry-hashes.json`, `month-council-final.diff`.
