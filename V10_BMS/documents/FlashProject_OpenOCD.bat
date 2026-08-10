@echo off

REM Find the latest V10_BMS_*.elf
for /f "delims=" %%F in ('powershell -NoProfile -Command "Get-ChildItem V10_BMS_*.elf | Sort-Object {[version](($_.BaseName -replace '^V10_BMS_',''))} -Descending | Select-Object -First 1 -ExpandProperty Name"') do set "ELF=%%F"

echo Flashing %ELF%
echo.
openocd ^
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

if %ERRORLEVEL% EQU 0 (
    echo.
    echo Flashed %ELF% successfully.
) else (
    echo.
    echo Flashing %ELF% FAILED or battery needs Wake-Up.
)
echo.
pause
