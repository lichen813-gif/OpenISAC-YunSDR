@echo off
setlocal

if "%~1"=="" (
    echo Usage: %~nx0 VIDEO_FILE [VIDEO_KBIT_S] [SNR_DB]
    echo Example: %~nx0 "D:\video\sample.mp4" 1500 45
    exit /b 2
)
set "VIDEO_BITRATE=%~2"
if not defined VIDEO_BITRATE set "VIDEO_BITRATE=1500"
set "SNR_DB=%~3"
if not defined SNR_DB set "SNR_DB=45"

start "OpenISAC PHY Channel" cmd /k call "%~dp0run_video_channel_sim.cmd" --snr %SNR_DB% --cfo 300 --sfo 20 --tdl 0:0:0+3:-4:45+9:-8:-80
timeout /t 1 /nobreak >nul
call "%~dp0start_vlc_video_receiver.cmd"
if errorlevel 1 exit /b 1
timeout /t 1 /nobreak >nul
call "%~dp0start_vlc_video_sender.cmd" "%~1" %VIDEO_BITRATE%
exit /b %ERRORLEVEL%
