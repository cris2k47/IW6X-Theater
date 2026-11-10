@echo off
setlocal
rem Builds XINPUT1_3.dll (iw6-theater) in Release x64.
rem Usage: build.bat [install]   - "install" copies the DLL into the game folder afterwards.

set ROOT=%~dp0
set PREMAKE=%ROOT%tools\premake5.exe
if not exist "%PREMAKE%" set PREMAKE=%ROOT%..\upstream\cbservers-iw6-mod\tools\premake5.exe
set GAME_DIR=%ROOT%..\..\ghosts_game_files

for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.Component.MSBuild -find MSBuild\**\Bin\MSBuild.exe`) do set MSBUILD=%%i
if not defined MSBUILD (
	echo MSBuild not found. Install Visual Studio 2022 Build Tools with the C++ workload.
	exit /b 1
)

pushd "%ROOT%"
"%PREMAKE%" vs2022 || goto :error
"%MSBUILD%" build\iw6-theater.sln -m -v:minimal -nologo -p:Configuration=Release -p:Platform=x64 || goto :error
popd

if /i "%1"=="install" (
	copy /y "%ROOT%build\bin\x64\Release\XINPUT1_3.dll" "%GAME_DIR%\XINPUT1_3.dll" || exit /b 1
	if not exist "%GAME_DIR%\iw6\ui_scripts\theater" mkdir "%GAME_DIR%\iw6\ui_scripts\theater"
	copy /y "%ROOT%lui\theater\__init__.lua" "%GAME_DIR%\iw6\ui_scripts\theater\__init__.lua" || exit /b 1
	echo Installed to %GAME_DIR%
)
exit /b 0

:error
popd
echo Build failed.
exit /b 1
