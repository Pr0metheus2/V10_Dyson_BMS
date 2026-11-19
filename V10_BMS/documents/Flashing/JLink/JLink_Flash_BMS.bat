@echo off
setlocal EnableExtensions

REM Programs a locked/unlocked BMS through SEGGER J-Link Commander.
REM This script does also a chip erase and change the security state if needed.

set "SCRIPT_DIR=%~dp0"
set "JLINK_EXE=%ProgramFiles%\SEGGER\JLink\JLink.exe"

if not exist "%JLINK_EXE%" (
    echo ERROR: J-Link Commander was not found at:
    echo %JLINK_EXE%
    pause
    exit /b 1
)

REM Find the latest V10_BMS_*.elf
set "ELF="
pushd "%SCRIPT_DIR%" >nul || exit /b 1
for /f "delims=" %%F in ('powershell -NoProfile -Command "Get-ChildItem V10_BMS_*.elf | Sort-Object {[version](($_.BaseName -replace '^V10_BMS_',''))} -Descending | Select-Object -First 1 -ExpandProperty Name"') do set "ELF=%%F"
popd

if not defined ELF (
    echo ERROR: No V10_BMS_*.elf file was found in:
    echo %SCRIPT_DIR%
    pause
    exit /b 1
)

set "COMMAND_FILE=%TEMP%\V10_BMS_JLink_%RANDOM%%RANDOM%.jlink"

(
    echo ExitOnError 1
    echo r
    echo h
    echo loadfile "%SCRIPT_DIR%%ELF%"
    echo r
    echo g
    echo exit
) > "%COMMAND_FILE%"

echo Programming %ELF%
echo Target: ATSAMD20E15A via SWD at 100 kHz
echo Ensure the pack is awake before continuing.
echo.

"%JLINK_EXE%" -device ATSAMD20E15 -if SWD -speed 100 -autoconnect 1 -CommandFile "%COMMAND_FILE%"
set "RESULT=%ERRORLEVEL%"
del "%COMMAND_FILE%" >nul 2>&1

if "%RESULT%" EQU "0" (
    echo.
    echo Flashed %ELF% successfully.
) else (
    echo.
    echo Flashing FAILED. Wake the pack with the trigger and check SWD wiring.
)

echo.
pause
exit /b %RESULT%
