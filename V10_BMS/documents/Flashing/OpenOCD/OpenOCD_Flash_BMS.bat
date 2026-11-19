@echo off
setlocal EnableExtensions

set "SCRIPT_DIR=%~dp0"
set "OPENOCD_EXE=%SCRIPT_DIR%xpack-openocd-0.12.0-7\bin\openocd.exe"
set "OPENOCD_SCRIPTS=%SCRIPT_DIR%xpack-openocd-0.12.0-7\openocd\scripts"

if not exist "%OPENOCD_EXE%" (
    echo ERROR: Bundled OpenOCD was not found at:
    echo %OPENOCD_EXE%
    pause
    exit /b 1
)

REM Find the latest V10_BMS_*.elf
pushd "%SCRIPT_DIR%" >nul || exit /b 1
for /f "delims=" %%F in ('powershell -NoProfile -Command "Get-ChildItem V10_BMS_*.elf | Sort-Object {[version](($_.BaseName -replace '^V10_BMS_',''))} -Descending | Select-Object -First 1 -ExpandProperty Name"') do set "ELF=%%F"

if not defined ELF (
    echo ERROR: No V10_BMS_*.elf file was found in:
    echo %SCRIPT_DIR%
    popd
    pause
    exit /b 1
)

echo Flashing %ELF%
echo.
"%OPENOCD_EXE%" -s "%OPENOCD_SCRIPTS%" ^
-f interface/cmsis-dap.cfg ^
-c "transport select swd" ^
-c "adapter speed 100" ^
-c "reset_config none" ^
-c "swd newdap samd cpu -expected-id 0x0bc11477" ^
-c "dap create samd.dap -chain-position samd.cpu" ^
-c "target create samd.cpu cortex_m -dap samd.dap" ^
-c "flash bank samd.flash at91samd 0x00000000 0 0 0 samd.cpu" ^
-c "init" ^
-c "reset halt" ^
-c "program %ELF% verify reset" ^
-c "shutdown"

set "RESULT=%ERRORLEVEL%"
popd

if "%RESULT%" EQU "0" (
    echo.
    echo Flashed %ELF% successfully.
) else (
    echo.
    echo Flashing %ELF% FAILED or battery needs Wake-Up.
)
echo.
pause
exit /b %RESULT%
