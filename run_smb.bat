@echo off
setlocal
cd /d "%~dp0"

echo [run_smb] Building runner_smb...
cmake --build build\runner_smb --config Release >build\runner_smb\build.log 2>&1
if errorlevel 1 (
    echo [run_smb] BUILD FAILED - see build\runner_smb\build.log
    type build\runner_smb\build.log | findstr /i "error"
    pause
    exit /b 1
)
echo [run_smb] Build OK

echo [run_smb] Cleaning old screenshots...
del /q C:\temp\nes_shot_*.png 2>nul
del /q C:\temp\smb_shot_*.png 2>nul

echo [run_smb] Starting kill watchdog (15 sec)...
start /b powershell -File C:\temp\kill_nes.ps1 -Delay 15

echo [run_smb] Running Super Mario Bros...
"build\runner_smb\Release\NESRecompGame.exe" "Super Mario Bros. (World).nes" --script C:\temp\smb_test.txt >C:\temp\smb_stdout.txt 2>&1
echo [run_smb] Exit code: %ERRORLEVEL%

echo [run_smb] --- stdout ---
type C:\temp\smb_stdout.txt
echo [run_smb] --- done ---
