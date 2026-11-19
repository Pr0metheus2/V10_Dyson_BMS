@echo off
setlocal EnableExtensions

REM Unlocks a security-locked ATSAMD20E15A by issuing a complete chip erase.
REM This permanently erases the installed firmware and EEPROM-emulation data.
REM Run JLink_Flash_BMS.bat afterwards to program replacement firmware.

set "JLINK_EXE=%ProgramFiles%\SEGGER\JLink\JLink.exe"

if not exist "%JLINK_EXE%" (
    echo ERROR: J-Link Commander was not found at:
    echo %JLINK_EXE%
    pause
    exit /b 1
)

echo.
echo WARNING: This will erase the entire BMS flash and unlock a locked SAMD20.
echo The original Dyson firmware, learned capacity, and EEPROM data will be lost.
set /p "CONFIRM=Type ERASE to continue: "
if /I not "%CONFIRM%"=="ERASE" (
    echo Unlock cancelled.
    pause
    exit /b 1
)

set "COMMAND_FILE=%TEMP%\V10_BMS_JLink_Unlock_%RANDOM%%RANDOM%.jlink"

(
    echo ExitOnError 1
    echo r
    echo h
    echo erase
    echo r
    echo g
    echo exit
) > "%COMMAND_FILE%"

echo.
echo Erasing and unlocking ATSAMD20E15A via SWD at 100 kHz.
echo Ensure the pack is awake before continuing.
echo.

"%JLINK_EXE%" -device ATSAMD20E15 -if SWD -speed 100 -autoconnect 1 -CommandFile "%COMMAND_FILE%"
set "RESULT=%ERRORLEVEL%"
del "%COMMAND_FILE%" >nul 2>&1

if "%RESULT%" EQU "0" (
    echo.
    echo Chip erase completed. Run JLink_Flash_BMS.bat to program firmware.
) else (
    echo.
    echo Unlock FAILED. Wake the pack with the trigger and check SWD wiring.
)

echo.
pause
exit /b %RESULT%
