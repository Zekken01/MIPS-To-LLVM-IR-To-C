@echo off
setlocal EnableExtensions

rem One-command Windows regression for the native EE instruction-trace branch.
rem Builds ONLY the required host tools, not all 8,046 SDBZ guest modules.
rem Does not run the game, patch guest RAM, or synthesize EE/GS events.
cd /d "%~dp0"
if errorlevel 1 exit /b 1

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo ERROR: Visual Studio vswhere.exe not found: "%VSWHERE%"
    exit /b 1
)
set "VSINSTALL="
for /f "usebackq tokens=*" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSINSTALL=%%I"
if not defined VSINSTALL (
    echo ERROR: Visual Studio C++ build tools not found.
    exit /b 1
)
call "%VSINSTALL%\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 (
    echo ERROR: vcvars64.bat failed.
    exit /b 1
)

if not defined SDBZ_CLANG_CXX set "SDBZ_CLANG_CXX=C:\Program Files\LLVM\bin\clang++.exe"
if not exist "%SDBZ_CLANG_CXX%" (
    echo ERROR: LLVM clang++.exe not found at "%SDBZ_CLANG_CXX%"
    echo Set SDBZ_CLANG_CXX to the absolute path of clang++.exe.
    exit /b 1
)
set "CXX=%SDBZ_CLANG_CXX%"
for %%I in ("%CXX%") do set "PATH=%%~dpI;%PATH%"

where python >nul 2>nul
if errorlevel 1 (
    echo ERROR: Python is missing from PATH.
    exit /b 1
)

echo.
echo === Configure native EE regression tests ===
if exist "build\CMakeCache.txt" (
    cmake -S . -B build
) else (
    cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
)
if errorlevel 1 (
    echo ERROR: CMake configuration failed.
    exit /b 1
)

echo.
echo === Build required native host tools ===
cmake --build build --config Release --target ee_recomp ee_game_runner -j 8
if errorlevel 1 (
    echo ERROR: Building EE host tools failed.
    exit /b 1
)

echo.
echo === Test Windows linker selection, guest PC trace and runner ===
set "TEST_LOG=%CD%\build\sdbz-native-regression-ctest.log"
ctest --test-dir build -C Release --output-on-failure --output-log "%TEST_LOG%" -R "^(ee_chunked_dispatcher|ee_instruction_trace_comparator|game_runner_end_to_end)$"
set "TEST_EXIT=%ERRORLEVEL%"

echo.
echo CTest log: "%TEST_LOG%"
if not "%TEST_EXIT%"=="0" (
    echo ERROR: At least one native EE regression failed. Share the CTest log.
    exit /b %TEST_EXIT%
)

echo PASS: Native EE regression tests passed.
exit /b 0
