@echo off
setlocal
if "%~1"=="" (
    echo Usage: run_cpp_replay_iq.cmd capture.oiq [ldpc_workers] [max_samples]
    exit /b 2
)
cd /d "%~dp0.."
call cpp_phy\build_windows_vs2019.cmd
if errorlevel 1 exit /b 1
set "OPENISAC_REPLAY_WORKERS=8"
if not "%~2"=="" set "OPENISAC_REPLAY_WORKERS=%~2"
set "OPENISAC_REPLAY_MAX_SAMPLES=4096"
if not "%~3"=="" set "OPENISAC_REPLAY_MAX_SAMPLES=%~3"
cpp_phy\build\ninja-vs2019\openisac_phy_replay_iq.exe "%~1" "%OPENISAC_REPLAY_WORKERS%" "%OPENISAC_REPLAY_MAX_SAMPLES%"
exit /b %errorlevel%
