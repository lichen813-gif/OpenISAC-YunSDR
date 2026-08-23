@echo off
setlocal
cd /d "%~dp0.."
call cpp_phy\build_windows_vs2019.cmd
if errorlevel 1 exit /b %errorlevel%

set "OPENISAC_FRAMES=%~1"
if not defined OPENISAC_FRAMES set "OPENISAC_FRAMES=200"
set "OPENISAC_LDPC_WORKERS=%~2"
if not defined OPENISAC_LDPC_WORKERS set "OPENISAC_LDPC_WORKERS=12"

cpp_phy\build\ninja-vs2019\openisac_phy_4x4_time_pipeline_benchmark.exe measurement\cpp_4x4_time_pipeline\frames.csv %OPENISAC_FRAMES% %OPENISAC_LDPC_WORKERS%
exit /b %errorlevel%
