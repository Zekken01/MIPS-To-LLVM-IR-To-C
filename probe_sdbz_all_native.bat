@echo off
setlocal

cd /d "%~dp0"

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo ERROR: vswhere.exe was not found.
    exit /b 1
)

for /f "usebackq tokens=*" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSINSTALL=%%I"
if not defined VSINSTALL (
    echo ERROR: Visual Studio C++ tools were not found.
    exit /b 1
)

call "%VSINSTALL%\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 exit /b %errorlevel%

set "CXX=C:\Program Files\LLVM\bin\clang++.exe"
if not exist "%CXX%" (
    echo ERROR: LLVM clang++.exe was not found at %CXX%.
    exit /b 1
)

set "SDBZ_ISO=D:\Games\PS2_ISO\Super Dragon Ball Z (USA).iso"
if exist "%SDBZ_ISO%" goto iso_found
echo ERROR: SDBZ ISO was not found at "%SDBZ_ISO%".
exit /b 1

:iso_found

set "SDBZ_WORK=build\sdbz-all-native"
set "SDBZ_LOG=%SDBZ_WORK%\probe.log"
if not exist "%SDBZ_WORK%\game-translated.dll" (
    echo ERROR: Native SDBZ DLL is missing. Run build_sdbz_all_native.bat first.
    exit /b 1
)

echo === Probing the prebuilt native SDBZ DLL ===
python tools\run_game.py PS2_GAME\SLUS_214.42 ^
    --work-dir=%SDBZ_WORK% ^
    --build-dir=build ^
    --config=Release ^
    --no-recompile ^
    --auto-expand=128 ^
    --emit-jobs=8 ^
    --emit-opt=0 ^
    --max-dispatches=100000 ^
    --loop-budget=1000000 ^
    --sif-poll-limit=2048 ^
    --timeout-seconds=120 ^
    --report=%SDBZ_WORK%\first-boot.json ^
    --hle-startup ^
    --kseg-ram ^
    --disc-image="%SDBZ_ISO%" > "%SDBZ_LOG%" 2>&1
set "PROBE_RESULT=%ERRORLEVEL%"
type "%SDBZ_LOG%"
exit /b %PROBE_RESULT%
