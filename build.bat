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

echo === Configuring ===
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
if errorlevel 1 exit /b %errorlevel%

echo === Building focused SIF and native-runner targets ===
cmake --build build --target iop_cpu_test iop_hw_test sif_dma_roundtrip_test iop_sif1_bridge_test ee_game_runner -j 8
if errorlevel 1 exit /b %errorlevel%

echo === Running focused SIF regressions ===
ctest --test-dir build -C Release --output-on-failure -R "^(iop_cpu|iop_hw|sif_dma_roundtrip|iop_sif1_bridge)$"
if errorlevel 1 exit /b %errorlevel%

echo === Running the real OpenPS2BIOS / SDBZ probe ===
call run_sdbz_cdvd.bat
set "RUN_RESULT=%ERRORLEVEL%"

echo.
echo Focused SIF tests passed. Real boot probe exit code: %RUN_RESULT%
exit /b %RUN_RESULT%
