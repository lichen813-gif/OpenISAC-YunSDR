@echo off
setlocal

call "%~dp0build_windows_vs2019.cmd"
if errorlevel 1 exit /b 1

set "BATCHES=%~1"
if not defined BATCHES set "BATCHES=20"

echo.
echo Running current-PHY sensing benchmark with %BATCHES% coherent batches...
"%~dp0build\ninja-vs2019\openisac_phy_sensing_benchmark.exe" %BATCHES%
exit /b %ERRORLEVEL%
