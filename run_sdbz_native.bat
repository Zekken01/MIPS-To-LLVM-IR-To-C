@echo off
setlocal

rem Build and run the local Super Dragon Ball Z USA ELF.
rem Real-game execution happens only on this Windows PC.
cd /d "%~dp0"
if errorlevel 1 goto :failed

set "ELF=PS2_GAME\SLUS_214.42"
set "WORK_DIR=build\sdbz-thread-pr38"
set "EMIT_JOBS=4"

if not exist "%ELF%" (
    echo Missing local game ELF: "%ELF%"
    goto :failed
)
if not exist "%WORK_DIR%" mkdir "%WORK_DIR%"
if errorlevel 1 goto :failed

echo === Updating repository ===
git switch main
if errorlevel 1 goto :failed
git pull --ff-only
if errorlevel 1 goto :failed

rem CMake may have cached cl.exe even if the batch was opened in a
rem regular console with no MSVC C++ headers configured. Explicitly load
rem the VS x64 developer environment before compiling.
echo === Initializing Visual Studio x64 build environment ===
set "VS_DEV_CMD="
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" (
    for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
        if exist "%%I\Common7\Tools\VsDevCmd.bat" (
            set "VS_DEV_CMD=%%I\Common7\Tools\VsDevCmd.bat"
        )
    )
)

rem Fallback for machines without vswhere.
if not defined VS_DEV_CMD (
    for %%V in (18 2026 2022 2019) do (
        for %%E in (Community Professional Enterprise BuildTools) do (
            if not defined VS_DEV_CMD (
                if exist "%ProgramFiles%\Microsoft Visual Studio\%%V\%%E\Common7\Tools\VsDevCmd.bat" (
                    set "VS_DEV_CMD=%ProgramFiles%\Microsoft Visual Studio\%%V\%%E\Common7\Tools\VsDevCmd.bat"
                )
            )
        )
    )
)

if not defined VS_DEV_CMD (
    echo ERROR: Visual Studio C++ build tools were not found.
    echo Install the "Desktop development with C++" workload in Visual Studio Installer.
    goto :failed
)
call "%VS_DEV_CMD%" -arch=amd64 -host_arch=amd64 -no_logo
if errorlevel 1 goto :failed

if not defined VCToolsInstallDir (
    echo ERROR: The MSVC developer environment was not initialized.
    goto :failed
)
if not exist "%VCToolsInstallDir%include\cstdint" (
    echo ERROR: MSVC C++ headers are missing: "%VCToolsInstallDir%include\cstdint"
    echo Install the MSVC x64 compiler and C++ libraries in Visual Studio Installer.
    goto :failed
)

echo === Building native tools ===
cmake --build build --config Release --parallel 8 --target ee_recomp ee_game_runner
if errorlevel 1 goto :failed

rem The guest DLL linker uses CXX; use installed LLVM clang++ when available.
if not defined CXX (
    if exist "C:\Program Files\LLVM\bin\clang++.exe" (
        set "CXX=C:\Program Files\LLVM\bin\clang++.exe"
    )
)

echo === Running local ELF ===
echo Log: "%WORK_DIR%\boot.log"
echo Report: "%WORK_DIR%\first-boot.json"

python tools\run_game.py "%ELF%" ^
    --build-dir build ^
    --config Release ^
    --work-dir "%WORK_DIR%" ^
    --hle-startup ^
    --kseg-ram ^
    --auto-expand=64 ^
    --emit-jobs=%EMIT_JOBS% ^
    --emit-opt=0 ^
    --max-dispatches=100000 ^
    --timeout-seconds=120 ^
    --trace ^
    --report "%WORK_DIR%\first-boot.json" > "%WORK_DIR%\boot.log" 2>&1

set "EXIT_CODE=%ERRORLEVEL%"
echo.
echo Run completed with exit code %EXIT_CODE%.
echo Review "%WORK_DIR%\boot.log" and "%WORK_DIR%\first-boot.json".
goto :done

:failed
set "EXIT_CODE=1"
echo.
echo The build or test could not complete. Review the error above.

:done
echo.
pause
exit /b %EXIT_CODE%
