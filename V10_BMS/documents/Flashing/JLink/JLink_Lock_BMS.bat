@echo off
setlocal EnableExtensions

REM Sets the ATSAMD20E15A NVM security bit without erasing the firmware.
REM Once set, external SWD flash/debug access is blocked until a complete chip erase.

set "JLINK_EXE=%ProgramFiles%\SEGGER\JLink\JLink.exe"

if not exist "%JLINK_EXE%" (
    echo ERROR: J-Link Commander was not found at:
    echo %JLINK_EXE%
    pause
    exit /b 1
)

echo.
echo WARNING: This sets the SAMD20 security bit and locks external SWD access.
echo The installed firmware remains intact, but it cannot be programmed or debugged
echo until a successful full-chip erase unlocks the device.
set /p "CONFIRM=Type LOCK to continue: "
if /I not "%CONFIRM%"=="LOCK" (
    echo Lock cancelled.
    pause
    exit /b 1
)

set "COMMAND_FILE=%TEMP%\V10_BMS_JLink_Lock_%RANDOM%%RANDOM%.jlink"

(
    echo ExitOnError 1
    echo r
    echo h
    REM NVMCTRL CTRLA: CMDEX=0xA5, CMD=0x45 (Set Security Bit).
    echo w2 0x41004000, 0xA545
    echo sleep 100
    echo g
    echo exit
) > "%COMMAND_FILE%"

echo.
echo Setting the ATSAMD20E15A security bit via SWD at 100 kHz.
echo Ensure the pack is awake before continuing.
echo.

"%JLINK_EXE%" -device ATSAMD20E15 -if SWD -speed 100 -autoconnect 1 -CommandFile "%COMMAND_FILE%"
set "RESULT=%ERRORLEVEL%"
del "%COMMAND_FILE%" >nul 2>&1

if "%RESULT%" EQU "0" (
    echo.
    echo Security bit set. Disconnect/reconnect the debugger before testing the lock.
) else (
    echo.
    echo Lock FAILED. Wake the pack with the trigger and check SWD wiring.
)

echo.
pause
exit /b %RESULT%
