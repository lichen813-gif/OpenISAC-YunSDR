@echo off
setlocal

set "PYTHON_EXE=%~dp0..\python_phy\.venv\Scripts\python.exe"
set "PATTERN_DIR=%~dp0..\measurement\vlc_video_smoke"
set "PATTERN_FILE=%PATTERN_DIR%\moving_pattern.yuv"
if not exist "%PYTHON_EXE%" (
    echo ERROR: project Python was not found: %PYTHON_EXE%
    exit /b 1
)
if not exist "%PATTERN_FILE%" (
    "%PYTHON_EXE%" "%~dp0tools\generate_video_test_pattern.py" "%PATTERN_FILE%" --seconds 10 --fps 15 --width 320 --height 180
    if errorlevel 1 exit /b 1
)

set "VLC_EXE=%ProgramFiles%\VideoLAN\VLC\vlc.exe"
if exist "%VLC_EXE%" goto found
set "VLC_EXE=%ProgramFiles(x86)%\VideoLAN\VLC\vlc.exe"
if exist "%VLC_EXE%" goto found
for /f "delims=" %%i in ('where vlc.exe 2^>nul') do if not defined VLC_FOUND set "VLC_FOUND=%%i"
if defined VLC_FOUND set "VLC_EXE=%VLC_FOUND%"
if exist "%VLC_EXE%" goto found
echo ERROR: VLC was not found.
exit /b 1

:found
start "OpenISAC Test Pattern Sender" /min "%VLC_EXE%" --no-one-instance -I dummy --demux=rawvid --rawvid-width=320 --rawvid-height=180 --rawvid-fps=15 --rawvid-chroma=I420 "%PATTERN_FILE%" --sout "#transcode{vcodec=h264,vb=800,fps=15}:standard{access=udp,mux=ts,dst=127.0.0.1:50000}" --play-and-exit
echo Dynamic 320x180 test pattern started for 10 seconds.
exit /b 0
