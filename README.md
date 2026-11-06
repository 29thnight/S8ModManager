# S8ModManager

삼국지 8 REMAKE with 파워업키트용 모드 매니저입니다. C# 모드를 JIT(라이브 코딩)나 NativeAOT로 불러와 게임 고유 UI에 붙입니다.

치트 오버레이([S8RPKCheats](https://github.com/29thnight/S8RPKCheats))와는 따로 설치하는 별도 제품입니다. 치트는 모드 매니저를 불러오지 않고, 모드 매니저도 치트 DLL에 기대지 않습니다.

## 구조

- C++ 네이티브 DLL(`xinput1_4.dll`)은 게임 기능을 API로 노출하는 일만 맡습니다. 게임 논리는 C# 모드에 둡니다.
- `src/scripting`: 네이티브 DLL(대리 진입, 후킹, 엔진 API, 모드 적재기)과 C# SDK·호스트·모드.
- `src/game`, `src/domain`, `src/platform/windows`: S8RPKCheats에서 복사한 보주 후킹·메모리 접근 코드. 경로를 원래대로 두어 `#include`를 바꾸지 않았습니다.
- `third_party`: MinHook, nlohmann/json, ownership_cpp 하위 모듈.

## 설치

`out\x64\<구성>`의 `xinput1_4.dll`, `nethost.dll`, `scripts` 폴더를 게임 실행 파일과 같은 폴더에 복사합니다. 게임이 XInput을 서수 2·3번으로 불러오므로 대리 DLL은 시스템 DLL의 서수를 그대로 유지합니다.

## 모드

- 모드는 `scripts\mods\<폴더>\mod.json`으로 선언합니다. `runtime`은 `jit` 또는 `aot`입니다.
- 같은 `id`의 모드가 여러 개면 하나만 적용합니다. 버전이 높은 쪽, 버전이 같으면 JIT 쪽입니다.
- 선택 항목: `apiVersion`(요구 엔진 API 버전, 다르면 건너뜀), `dependencies`(`{"모드 id": "최소 버전"}`), `conflicts`(같이 켤 수 없는 모드 id 배열).
- `scripts\mods.json`이 켜기·끄기와 적재 순서를 정합니다. 없으면 만들고, 새 모드는 켜진 상태로 끝에 덧붙입니다. 형식이 틀리면 고치지 않고 모든 모드를 켜진 것으로 봅니다.
  ```json
  { "version": 1, "mods": [ { "id": "s8.editors", "enabled": true } ] }
  ```
- 적재 규칙: 꺼진 모드, API 버전이 다른 모드, 필요한 모드가 없거나 버전이 낮은 모드는 건너뜁니다(연쇄 적용). 충돌하면 순서가 앞선 쪽만 적용합니다. 의존 모드는 항상 먼저 불러오고, 의존이 순환하면 모두 건너뜁니다. 건너뛴 이유는 `scripts\mod_manager.log`가 아니라 디버그 출력에 남습니다.
- 모드는 자기 폴더만 씁니다. 자산은 `assets\`에서 읽고, 저장 파일(예: `san8r_traits_config.json`, `S8.Editors.jewels.json`)은 모드 폴더에 씁니다. 게임 폴더는 읽기만 합니다.
- 0.0.1(`s8.editors`)은 기재 편집과 보주 편집만 포함합니다. 다른 기능은 Release 빌드에서 제외합니다.
- AOT판: `dotnet publish src/scripting/managed/S8.GameScripts -c Release -r win-x64 -p:PublishAot=true -o <게임>\scripts\mods\s8.editors.aot`

## 빌드

Visual Studio 2026, MSVC v145, Windows SDK 10.0.26100.0, .NET 10 SDK를 사용합니다.

```powershell
git clone --recurse-submodules <저장소>
msbuild src/scripting/S8.Scripting.Native.vcxproj /m /p:Configuration=Release /p:Platform=x64
```

시험:

```powershell
msbuild tests/S8.ModManager.Tests.vcxproj /p:Configuration=Debug /p:Platform=x64
out\x64\Debug\S8.ModManager.Tests.exe
dotnet run --project tests/scripting/S8.Scripting.Smoke.csproj -- out\x64\Debug
```

후킹에 실패하면 `scripts\mod_manager.log`에 실패한 주소와 상태값을 남깁니다.
