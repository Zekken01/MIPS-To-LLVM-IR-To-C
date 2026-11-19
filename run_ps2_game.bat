@echo off
setlocal EnableExtensions DisableDelayedExpansion

rem Boot the repo's extracted PS2_GAME disc through the BIOS-free HLE path.
rem BOOT2 in SYSTEM.CNF chooses the executable; no region/ELF is hardcoded.
rem Never switch branches, fetch ROM0, or require an ISO.
cd /d "%~dp0"
if errorlevel 1 exit /b 1

set "DISC_ROOT=%CD%\PS2_GAME"
set "WORK_DIR=build\ps2-game-hle"
if not defined EMIT_JOBS set "EMIT_JOBS=8"
if not defined SIF_POLL_LIMIT set "SIF_POLL_LIMIT=2048"
rem Up to eight capped auto-expansion passes (128 new functions per pass).
rem Set EXPANSION_PASSES=1..8 to override; stop early at real guest blockers.
if not defined EXPANSION_PASSES set "EXPANSION_PASSES=8"

where python >nul 2>nul
if errorlevel 1 (
    echo ERROR: Python was not found in PATH.
    exit /b 1
)
set "GAME_ELF="
for /f "usebackq delims=" %%I in (`python "tools\resolve_ps2_boot.py" "%DISC_ROOT%"`) do set "GAME_ELF=%%I"
if not defined GAME_ELF (
    echo ERROR: Could not resolve BOOT2 from PS2_GAME\SYSTEM.CNF.
    exit /b 1
)
if not exist "%GAME_ELF%" (
    echo ERROR: BOOT2 executable does not exist: "%GAME_ELF%"
    exit /b 1
)
echo Disc folder: "%DISC_ROOT%"
echo Boot ELF: "%GAME_ELF%"
if /I "%PS2_GAME_RESOLVE_ONLY%"=="1" exit /b 0

rem The CMake/LLVM toolchain on Windows uses the Visual Studio x64 SDK.
if defined VCToolsInstallDir goto :toolchain_ready
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo ERROR: Visual Studio vswhere.exe not found.
    exit /b 1
)
for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSINSTALL=%%I"
if not defined VSINSTALL (
    echo ERROR: Visual Studio C++ build tools not found.
    exit /b 1
)
call "%VSINSTALL%\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 exit /b 1

:toolchain_ready
if not defined CXX (
    if exist "C:\Program Files\LLVM\bin\clang++.exe" (
        set "CXX=C:\Program Files\LLVM\bin\clang++.exe"
    )
)
where cmake >nul 2>nul
if errorlevel 1 (
    echo ERROR: CMake was not found in PATH.
    exit /b 1
)

rem Optional native WebGPU compute rendering: no browser, SDL or frontend.
rem Set EE_WEBGPU_SNAPSHOT=1 to enable the native renderer.
rem The pinned SDK is downloaded into build\_deps\wgpu-native by default.
rem EE_WEBGPU_ROOT can still override the location with a compatible SDK.
set "EE_WEBGPU_RUN_ARG="
if /I "%EE_WEBGPU_SNAPSHOT%"=="1" (
    if not defined EE_WEBGPU_ROOT set "EE_WEBGPU_ROOT=%CD%\build\_deps\wgpu-native"
    set "EE_WEBGPU_RUN_ARG=--gs-webgpu-snapshot=%WORK_DIR%\gs-webgpu.ppm"
)
rem Obtain the same pinned MSVC wgpu-native SDK used in WebGPU CI.
rem Verify its checksum, headers, import library and runtime DLL before CMake.
rem A complete existing SDK is reused offline; an incomplete one fails
rem explicitly rather than silently overwriting local user files.
if /I "%EE_WEBGPU_SNAPSHOT%"=="1" (
    python "tools\ensure_wgpu_native.py" --root "%EE_WEBGPU_ROOT%"
    if errorlevel 1 exit /b 1
)
echo === Configuring native host tools ===
if /I "%EE_WEBGPU_SNAPSHOT%"=="1" (
    if exist "build\CMakeCache.txt" (
        cmake -S . -B build -DEE_ENABLE_WEBGPU=ON "-DEE_WEBGPU_ROOT=%EE_WEBGPU_ROOT%"
    ) else (
        cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DEE_ENABLE_WEBGPU=ON "-DEE_WEBGPU_ROOT=%EE_WEBGPU_ROOT%"
    )
) else (
    if exist "build\CMakeCache.txt" (
        cmake -S . -B build -DEE_ENABLE_WEBGPU=OFF
    ) else (
        cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DEE_ENABLE_WEBGPU=OFF
    )
)
if errorlevel 1 exit /b 1

echo === Building native EE runner and recompiler ===
cmake --build build --config Release --parallel 8 --target ee_recomp ee_game_runner
if errorlevel 1 exit /b 1

if not exist "%WORK_DIR%" mkdir "%WORK_DIR%"
if errorlevel 1 exit /b 1

rem A verified cache may be reused; failed fingerprints trigger a rebuild.
set "CACHE_FLAG="
if exist "%WORK_DIR%\translation-cache.json" (
    python tools\run_game.py "%GAME_ELF%" ^
        --build-dir build --config Release ^
        --work-dir "%WORK_DIR%" --emit-opt=0 ^
        --no-recompile --link-only >nul 2>nul
    if not errorlevel 1 set "CACHE_FLAG=--no-recompile"
)

echo === Starting extracted-disc HLE boot session ===
echo Boot log: "%WORK_DIR%\boot.log"
echo Expansion log: "%WORK_DIR%\expansion.log"
echo Report: "%WORK_DIR%\first-boot.json"
echo Guest display VRAM: "%WORK_DIR%\gs-display.ppm" (only if configured)
echo GS texture upload: "%WORK_DIR%\gs-upload.ppm" (only if complete)
if /I "%EE_WEBGPU_SNAPSHOT%"=="1" echo Native WebGPU raster: "%WORK_DIR%\gs-webgpu.ppm"
python tools\run_boot_session.py "%GAME_ELF%" ^
    --disc-root "%DISC_ROOT%" ^
    --disc-folder "%DISC_ROOT%" ^
    --build-dir build ^
    --config Release ^
    --work-dir "%WORK_DIR%" ^
    --report "%WORK_DIR%\first-boot.json" ^
    --gs-snapshot "%WORK_DIR%\gs-display.ppm" ^
    --gs-upload-snapshot "%WORK_DIR%\gs-upload.ppm" ^
    --expansion-passes=%EXPANSION_PASSES% ^
    --expansion-per-pass=128 ^
    --ll-progress ^
    --hle-startup ^
    --kseg-ram ^
    --emit-jobs=%EMIT_JOBS% ^
    --emit-opt=0 ^
    --max-dispatches=100000 ^
    --loop-budget=1000000 ^
    --sif-poll-limit=%SIF_POLL_LIMIT% ^
    --timeout-seconds=120 %CACHE_FLAG% %EE_WEBGPU_RUN_ARG%
set "EXIT_CODE=%ERRORLEVEL%"
echo.
echo Native HLE boot session exit code: %EXIT_CODE%
echo Logs are in "%WORK_DIR%".
exit /b %EXIT_CODE%
