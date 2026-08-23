@echo off
setlocal

set "BRIDGE=%~dp0build\ninja-vs2019\openisac_phy_video_bridge.exe"
if not exist "%BRIDGE%" (
    echo Building the Windows C++ PHY first...
    call "%~dp0build_windows_vs2019.cmd"
    if errorlevel 1 exit /b 1
)

"%BRIDGE%" %*
exit /b %ERRORLEVEL%
