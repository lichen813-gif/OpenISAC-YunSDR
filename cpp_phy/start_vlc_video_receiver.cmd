@echo off
setlocal

set "VLC_EXE=%ProgramFiles%\VideoLAN\VLC\vlc.exe"
if exist "%VLC_EXE%" goto found
set "VLC_EXE=%ProgramFiles(x86)%\VideoLAN\VLC\vlc.exe"
if exist "%VLC_EXE%" goto found
for /f "delims=" %%i in ('where vlc.exe 2^>nul') do if not defined VLC_FOUND set "VLC_FOUND=%%i"
if defined VLC_FOUND set "VLC_EXE=%VLC_FOUND%"
if exist "%VLC_EXE%" goto found

echo ERROR: VLC was not found. Install VLC or add vlc.exe to PATH.
exit /b 1

:found
start "OpenISAC VLC Receiver" "%VLC_EXE%" --no-one-instance "udp://@:50001" --network-caching=500
echo VLC receiver started on udp://@:50001
exit /b 0
