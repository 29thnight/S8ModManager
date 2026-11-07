@rem Builds LuaJIT from a copy of the submodule (msvcbuild.bat writes objects next to
@rem its sources) and lays out the sample Lua mod.
@rem   %1 LuaJIT checkout  %2 work directory  %3 sample mod output  %4 configuration
@setlocal
@rem msvcbuild.bat runs its helper tools from the current directory.
@set NoDefaultCurrentDirectoryInExePath=
@set SOURCE=%~1
@set WORK=%~2
@set MODDIR=%~3
robocopy "%SOURCE%src" "%WORK%src" /E /NFL /NDL /NJH /NJS /NP >nul
@if errorlevel 8 exit /b 1
robocopy "%SOURCE%dynasm" "%WORK%dynasm" /E /NFL /NDL /NJH /NJS /NP >nul
@if errorlevel 8 exit /b 1
@pushd "%WORK%src"
@rem Always the release CRT (/MD): the game machine has no debug CRT.
@if not exist lua51.dll call msvcbuild.bat
@if not exist lua51.dll (popd & exit /b 1)
@popd
@if not exist "%MODDIR%" mkdir "%MODDIR%"
copy /y "%WORK%src\lua51.dll" "%MODDIR%" >nul || exit /b 1
dotnet build "%~dp0..\managed\S8.LuaHost\S8.LuaHost.csproj" -c %4 -o "%MODDIR:~0,-1%" --nologo -v q || exit /b 1
copy /y "%~dp0sample\*" "%MODDIR%" >nul || exit /b 1
@exit /b 0
