# 장수·도시 소속 조회와 다음 엔진 바인딩

## 구현한 조회

`GetOfficer/GetCity`가 새 객체를 연결할 때 각각 `OfficerAffiliationComponent`,
`CityAffiliationComponent`를 자동 구성한다. Read는 요청한 객체와 바로 연결된 참조만 조회한다.
프레임별 스캔이나 전체 장수·도시 수집은 추가하지 않았다.

```csharp
var player = world.GetPlayer();
var officer = player.Character;
var affiliation = officer.GetComponent<OfficerAffiliationComponent>()!.Read();

byte roleId = affiliation.RoleId;
byte? rankId = affiliation.RankId;
byte? forceId = affiliation.ForceId;
ushort? lordId = affiliation.LordId;

if (affiliation.CityId is { } cityId)
{
    var city = world.GetCity(cityId);
    var ownership = city.GetComponent<CityAffiliationComponent>()!.Read();
    bool isGovernor = ownership.GovernorId == officer.GetComponent<OfficerComponent>()!.Id;
}
```

- `RoleId`는 StatusData의 실제 ID다. 표시명과의 전체 대응이 확정되지 않아 추측한 이름의 enum을 만들지 않았다.
- `RankId`는 별도의 RankData 참조 ID다. 신분과 같은 값으로 취급하지 않는다.
- `ForceId`와 도시 ID는 byte, 장수 ID는 ushort다. 현재 월드에서 장수·도시 ID는 1부터 시작한다.
- 장수의 `CityId`는 PersonData가 참조하는 도시다. 이 값만으로 이동 완료를 선언하지 않는다.
- 장수의 `LordId`는 장수의 소속 ForceData가 참조하는 현재 군주다.
- 도시의 `GovernorId`는 CityData의 태수 참조, `LordId`는 도시 소속 ForceData의 현재 군주다.
- `DistrictNumber`는 도시가 참조하는 구역 객체 +18의 관측 번호다.
- 참조 없음은 null이다. 읽기 실패·다른 타입의 포인터는 네이티브 오류로 전달하며, 무소속/빈 도시로 바꾸지 않는다.
- 현재 임무 ID와 명칭 조회는 아직 연결하지 않았다.

이벤트 조건에서 같은 Read API를 사용할 수 있다. 위 예제는 데이터를 변경하지 않는다.

## 기존 바이트 신분 해석과의 차이

기존 roster 편집 코드는 장수 +10의 한 바이트를 0x18/0x28/.../0xC8 등으로 해석한다.
하지만 PersonData::ArchiveRuntimeFields의 공유 처리 16F6F10은 +10을 데이터 참조로 다룬다.

```text
16F6F3F: PersonData+10의 포인터 읽기
16F6F48: 참조 객체+8의 byte ID 저장
16F6F91..16F6FA0: 로드 시 g+5855E0의 ID 포인터 테이블에서 복원
```

따라서 그 바이트는 신분 객체의 주소 일부이며 새 API는 이를 신분 코드로 재사용하지 않는다.
17126C0은 +10 참조의 가상 IsValid를 호출한 뒤 +8의 실제 ID를 반환한다.
기존 ImGui roster의 해석/편집 코드는 이번 변경에서 수정하지 않았다.

## 연결 근거

기준 EXE는 기존 분석용 SAN8RPK.exe.unpacked.exe와 동일하다. 이름은 SDK에서 붙인 이름이다.

| 내용 | 근거 |
| --- | --- |
| StatusData | TD 2DF6188, vtable 2687D28 |
| 실제 신분 ID | 17126C0; PersonData+10 참조 → ID+8 |
| RankData | TD 2DF6260, vtable 26883A8 |
| Rank ID | 1711890; PersonData+F0 참조 → ID+8 |
| Rank 직렬화 | 16F72D3..16F734C; g+546D60 테이블의 ID 1..22 참조 복원 |
| 장수 소속 세력 | PersonData+18, getter body 1707410; 16F6FE5..16F7046이 g+557390의 ForceData ID 참조임을 입증 |
| 장수 도시 | PersonData+20, getter body 1706E20; 16F7063 이후 도시 참조 직렬화 |
| 도시 구역 | CityData+90, getter body 170EA50. 구역 +10/+18은 기존 도시 관리 코드의 세력/번호 조회와 대조 |
| 도시 태수 | CityData+98, getter body 1713F00 |
| 현재 군주 | ForceData+C0, getter body 1713CF0; ForceData 초기화 16FE990도 해당 참조 초기화 |
| 활성 세력 | ForceData::IsValid 16FEBB0; ID 1..120, +204 활성 플래그 |

같은 필드 접근 본문을 가진 getter 주소를 연결했으며, 각 주소가 원래 소스에서 가진 메서드 이름은 주장하지 않는다.
추가한 8개 진입의 16바이트 시그니처는 기존 최초 연결 검사에 포함했다.
연산 37/38, 응답 POD 24바이트다. 기존 세션/엔티티 해석 입구를 공유한다.

## 이동·임명·하야 분석 상태

새 조회를 만들었다고 정식 이동·임명 처리가 확보된 것은 아니다.
현재 식별한 실제 클래스와 후보는 다음과 같다.

| 실제 RTTI 타입 | TD / vtable | 후보 메서드 |
| --- | --- | --- |
| ConferenceMoveState@conference@san8r | 2E22F78 / 26A7FC8 | 19636C0, 1962290, 1963620 |
| ConferenceAssignState@conference@san8r | 2E23768 / 26A83E8 | 19ADC90, 19AC3E0, 19ADB40 |
| StrategyPhaseGameState@san8r | 2E662B0 / 26CF3D8 | 1BD5410, 1BD47F0 |
| PersonalPhaseGameState@san8r | 2E66278 / 26CF528 | 1BD67C0, 1BD6190 |

세부 RTTI 추적은 분석 디렉토리의 `political-state-candidates.json`에 기록했다.
ConferenceMoveState의 1962290은 약 ED바이트의 상태 처리이며, 이동 대상을 ID로 받는 독립 함수가 아니다.
ConferenceAssignState의 19AC3E0도 UI/상태 컨텍스트를 사용한다.

기존 `move_roster_ronin`은 PersonData+20 포인터를 직접 바꾸는 처리다.
이를 MoveTo/임명 API로 포장하지 않았다. 마찬가지로 RankData 포인터 setter 1711B70만 호출하는 것을
관직 임명 전체 처리라고 노출하지 않는다. 목록 소속, 임무, 비용, 평정 진행과 알림 계약의 확인이 남았다.

## 월 시작·평정·도시 진입 이벤트

평정/도시 GameState 클래스와 업데이트 후보는 확인했지만, 아직 새 네이티브 훅은 추가하지 않았다.
1BD47F0/1BD6190은 프레임 상태 처리이며 클래스 이름만으로 평정 시작/종료라고 판정할 수 없다.
현재 스크립트의 SceneEntered/SceneExited는 SDK 씬 전환 이벤트이고, 실제 게임 전환 훅은 아니다.
날짜 setter 1706DF0 호출 역시 월별 처리가 끝났다는 뜻이 아니므로 월 시작 이벤트로 연결하지 않는다.
이동 완료·월 시작·평정 전후 경계는 실제 상태 전환 추적이 남아 있다.

## 검증

신분/Rank ID 디코딩, 세력·군주·태수 참조 분리, 빈 도시의 nullable ID, 읽기 오류 전달에 대한
모의 네이티브 계약 테스트 소스를 추가했다. 실제 게임 함수 실행 검증은 별개다.
이번 변경은 빌드·테스트 실행·게임 적용 전이다.
