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
