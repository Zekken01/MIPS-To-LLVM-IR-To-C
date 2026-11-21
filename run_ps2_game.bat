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
rem Bound each native auto-expansion session; both limits are configurable.
rem EXPANSION_PASSES: 1..64, number of bounded passes (default 8).
rem EXPANSION_PER_PASS: 1..128, newly emitted functions per pass (default 128).
rem Stop early at genuine guest blockers; never fabricate completion.
if not defined EXPANSION_PASSES set "EXPANSION_PASSES=8"
if not defined EXPANSION_PER_PASS set "EXPANSION_PER_PASS=128"
if not defined SDBZ_REPORT_SNAPSHOT_MS set "SDBZ_REPORT_SNAPSHOT_MS=2000"

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

rem Optional WebGPU renderer and raylib Win32 window. The window opens
rem only during the final traced replay; expansion remains headless.
rem An explicitly requested quick probe never captures WebGPU, even if
rem this command prompt has retained snapshot/window flags from a
rem previous full run. It must remain non-traced after cache misses.
if /I "%~1"=="--quick" (
    set "EE_WEBGPU_SNAPSHOT=0"
    set "EE_WEBGPU_WINDOW=0"
)
set "EE_WEBGPU_MODE=OFF"
set "EE_RAYLIB_MODE=OFF"
set "EE_WEBGPU_RUN_ARG="
set "EE_WEBGPU_WINDOW_ARG="
if /I "%EE_WEBGPU_SNAPSHOT%"=="1" (
    set "EE_WEBGPU_MODE=ON"
    set "EE_WEBGPU_RUN_ARG=--gs-webgpu-snapshot=%WORK_DIR%\gs-webgpu.ppm"
)
if /I "%EE_WEBGPU_WINDOW%"=="1" (
    set "EE_WEBGPU_MODE=ON"
    set "EE_RAYLIB_MODE=ON"
    set "EE_WEBGPU_WINDOW_ARG=--gs-webgpu-window"
)
rem Reuse the pinned MSVC SDK in build\_deps or download it once.
rem Set this outside the block: cmd.exe expands %%VARS%% before executing
rem a parenthesized block, so setting and reading inside it passes an empty root.
if "%EE_WEBGPU_MODE%"=="ON" if not defined EE_WEBGPU_ROOT set "EE_WEBGPU_ROOT=%CD%\build\_deps\wgpu-native"
if "%EE_WEBGPU_MODE%"=="ON" (
    python "tools\ensure_wgpu_native.py" --root "%EE_WEBGPU_ROOT%"
    if errorlevel 1 exit /b 1
)
echo === Configuring native host tools ===
if exist "build\CMakeCache.txt" (
    cmake -S . -B build -DEE_ENABLE_WEBGPU=%EE_WEBGPU_MODE% -DEE_ENABLE_RAYLIB_WINDOW=%EE_RAYLIB_MODE% "-DEE_WEBGPU_ROOT=%EE_WEBGPU_ROOT%"
) else (
    cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DEE_ENABLE_WEBGPU=%EE_WEBGPU_MODE% -DEE_ENABLE_RAYLIB_WINDOW=%EE_RAYLIB_MODE% "-DEE_WEBGPU_ROOT=%EE_WEBGPU_ROOT%"
)
if errorlevel 1 exit /b 1

echo === Building native EE runner and recompiler ===
cmake --build build --config Release --parallel 8 --target ee_recomp ee_game_runner
if errorlevel 1 exit /b 1

if not exist "%WORK_DIR%" mkdir "%WORK_DIR%"
if errorlevel 1 exit /b 1

rem Git checkout may rebuild host tools without changing guest LLVM IR.
rem Try exact reuse first; if host inputs changed, relink existing guest objects.
rem --quick forces the non-traced probe even if no guest cache survives.
rem Without --quick, WebGPU requests still require full replay snapshots.
rem Only rebuild guest IR when preserved provenance cannot be verified.
set "CACHE_FLAG="
if exist "%WORK_DIR%\translation-cache.json" (
    echo === Validating saved EE translation ===
    python tools\run_game.py "%GAME_ELF%" ^
        --build-dir build --config Release ^
        --work-dir "%WORK_DIR%" --emit-opt=0 ^
        --no-recompile --link-only >nul 2>nul
    if not errorlevel 1 (
        echo EE translation cache valid; reusing existing functions.
        set "CACHE_FLAG=--no-recompile"
    ) else (
        echo Native cache changed; checking preserved guest IR and objects...
        python tools\run_game.py "%GAME_ELF%" ^
            --build-dir build --config Release ^
            --work-dir "%WORK_DIR%" --emit-opt=0 ^
            --relink-existing --link-only
        if not errorlevel 1 (
            echo Preserved guest IR and objects; relinked host library.
            set "CACHE_FLAG=--no-recompile"
        ) else (
            echo Guest translation provenance changed; full IR rebuild required.
        )
    )
)

rem This is the only fallback that can wipe and regenerate old guest IR.
rem A failed exact-cache validation AND failed safe relink are not permission
rem to delete saved translations. Require an explicit affirmative response
rem if ANY previously generated guest translation artifact still exists.
if not defined CACHE_FLAG call :confirm_guest_cache_rebuild
if errorlevel 1 exit /b 1

echo === Starting extracted-disc HLE boot session ===
set "SESSION_ARGS=--quick-probe --no-iop-catalog"
set "REPORT_NAME=quick-probe.json"
set "DEFAULT_MAX_DISPATCHES=100000"
if not defined CACHE_FLAG set "SESSION_ARGS="
if /I "%~1"=="--quick" set "SESSION_ARGS=--quick-probe --no-iop-catalog"
if /I "%~1"=="--full" set "SESSION_ARGS="
if "%EE_WEBGPU_MODE%"=="ON" set "SESSION_ARGS="
if defined SESSION_ARGS (
    echo Using the cached HLE quick probe.
    echo Boot log: "%WORK_DIR%\quick-probe.log"
) else (
    set "REPORT_NAME=first-boot.json"
    set "DEFAULT_MAX_DISPATCHES=500000"
    set "SESSION_ARGS=--gs-snapshot=%WORK_DIR%\gs-display.ppm --gs-upload-snapshot=%WORK_DIR%\gs-upload.ppm"
    echo Using the full traced HLE boot session.
    echo Boot log: "%WORK_DIR%\boot.log"
    echo Expansion log: "%WORK_DIR%\expansion.log"
    echo Guest display VRAM if configured: "%WORK_DIR%\gs-display.ppm"
    echo GS texture upload if complete: "%WORK_DIR%\gs-upload.ppm"
)
rem Full replays have millions of dispatch boundaries. Retain every
rem actual dispatch and the complete JSON diagnostic summaries while
rem sampling *only* ordinary verbose stdout lines. Set
rem SDBZ_DISPATCH_TRACE_EVERY=1 for the historical full text trace.
rem The environment is host-only and never fingerprints guest LLVM IR.
if not defined SESSION_ARGS if not defined SDBZ_DISPATCH_TRACE_EVERY set "SDBZ_DISPATCH_TRACE_EVERY=1024"
if not defined SESSION_ARGS echo Verbose dispatch trace: first 64, each %SDBZ_DISPATCH_TRACE_EVERY%, and exceptional transitions
rem Keep quick probes short; full retail traces need sufficient real EE/IOP
rem cycles to observe long-running IRX work. Preserve explicit overrides.
if not defined MAX_DISPATCHES set "MAX_DISPATCHES=%DEFAULT_MAX_DISPATCHES%"
if not defined BOOT_TIMEOUT_SECONDS set "BOOT_TIMEOUT_SECONDS=120"
rem Preserve the 1000000-instruction native watchdog. Bounded resumes
rem reset only after genuine guest GS fields, IOP RPC commits or GIF writes,
rem not on arbitrary dispatches, guest instructions or host time.
rem Use LOOP_YIELD_RESUMES=0 to stop on the first native loop watchdog.
if not defined LOOP_YIELD_RESUMES set "LOOP_YIELD_RESUMES=16"
echo Native loop watchdog: 1000000 instructions per native slice, %LOOP_YIELD_RESUMES% resumes between real GS/GIF/IOP progress
echo Expansion budget: %EXPANSION_PASSES% passes, %EXPANSION_PER_PASS% new functions per pass
echo Dispatch budget: %MAX_DISPATCHES% per pass; timeout: %BOOT_TIMEOUT_SECONDS% seconds
echo Report: "%WORK_DIR%\%REPORT_NAME%"
if /I "%EE_WEBGPU_SNAPSHOT%"=="1" echo Native WebGPU raster: "%WORK_DIR%\gs-webgpu.ppm"
if /I "%EE_WEBGPU_WINDOW%"=="1" echo Native WebGPU presentation: raylib Win32 window in final replay
rem Display live LLVM IR file count and percentage in both quick and full
rem sessions; the orchestrator keeps in-place updates out of boot logs.
python tools\run_boot_session.py "%GAME_ELF%" ^
    --ll-progress ^
    --disc-root "%DISC_ROOT%" ^
    --disc-folder "%DISC_ROOT%" ^
    --build-dir build ^
    --config Release ^
    --work-dir "%WORK_DIR%" ^
    --report "%WORK_DIR%\%REPORT_NAME%" ^
    --expansion-passes=%EXPANSION_PASSES% ^
    --expansion-per-pass=%EXPANSION_PER_PASS% ^
    --hle-startup ^
    --kseg-ram ^
    --emit-jobs=%EMIT_JOBS% ^
    --emit-opt=0 ^
    --max-dispatches=%MAX_DISPATCHES% ^
    --loop-budget=1000000 ^
    --loop-yield-resumes=%LOOP_YIELD_RESUMES% ^
    --sif-poll-limit=%SIF_POLL_LIMIT% ^
    --timeout-seconds=%BOOT_TIMEOUT_SECONDS% %CACHE_FLAG% %SESSION_ARGS% %EE_WEBGPU_RUN_ARG% %EE_WEBGPU_WINDOW_ARG%
set "EXIT_CODE=%ERRORLEVEL%"
echo.
echo Native HLE boot session exit code: %EXIT_CODE%
echo Logs are in "%WORK_DIR%".
exit /b %EXIT_CODE%

:confirm_guest_cache_rebuild
rem Fresh builds with no saved guest output need no confirmation.
rem Detect remnants even when translation-cache.json itself was removed.
set "SAVED_GUEST_TRANSLATION="
if exist "%WORK_DIR%\translation-cache.json" set "SAVED_GUEST_TRANSLATION=1"
if exist "%WORK_DIR%\ir\ee_functions.json" set "SAVED_GUEST_TRANSLATION=1"
if exist "%WORK_DIR%\ir\*.ll" set "SAVED_GUEST_TRANSLATION=1"
if exist "%WORK_DIR%\objects\objects.json" set "SAVED_GUEST_TRANSLATION=1"
if exist "%WORK_DIR%\game-translated.dll" set "SAVED_GUEST_TRANSLATION=1"
if not defined SAVED_GUEST_TRANSLATION exit /b 0

echo.
echo ================================================================
echo WARNING: The existing guest LLVM IR cache could not be reused.
echo Both exact cache validation and safe host-only relinking failed,
echo or saved translation metadata is missing.
echo.
echo A full rebuild may DELETE existing .ll modules and REPLACE the
echo translated game library, objects, and translation-cache.json in:
echo   "%WORK_DIR%"
echo.
echo Back up this directory before continuing if you need its contents.
echo Press Y to authorize the rebuild, or N to abort without rebuilding.
echo ================================================================
choice /C YN /N /M "Rebuild saved guest translations? [Y/N] "
rem CHOICE: Y=1; N=2; error/no-console=255. Anything but Y aborts.
if errorlevel 2 (
    echo Guest translation rebuild cancelled; saved IR was not regenerated.
    exit /b 1
)
if errorlevel 1 (
    echo Guest translation rebuild explicitly authorized.
    exit /b 0
)
echo ERROR: Cache rebuild confirmation was not received.
exit /b 1
