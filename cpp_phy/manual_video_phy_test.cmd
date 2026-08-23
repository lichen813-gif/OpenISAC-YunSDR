@echo off
setlocal
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0manual_video_phy_test.ps1" %*
exit /b %ERRORLEVEL%
