@echo off
setlocal

if "%~1"=="" (
    echo Usage: %~nx0 VIDEO_FILE [VIDEO_KBIT_S]
    echo Example: %~nx0 "D:\video\sample.mp4" 1500
    exit /b 2
)
if not exist "%~1" (
    echo ERROR: video file not found: %~1
    exit /b 1
)
set "VIDEO_FILE=%~1"
set "VIDEO_BITRATE=%~2"
if not defined VIDEO_BITRATE set "VIDEO_BITRATE=1500"

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
start "OpenISAC VLC Sender" "%VLC_EXE%" --no-one-instance "%VIDEO_FILE%" --sout "#transcode{vcodec=h264,vb=%VIDEO_BITRATE%,acodec=mp4a,ab=128,channels=2}:standard{access=udp,mux=ts,dst=127.0.0.1:50000}" --sout-keep
echo VLC sender started: H.264 %VIDEO_BITRATE% kbit/s plus 128 kbit/s audio to UDP 50000
exit /b 0
