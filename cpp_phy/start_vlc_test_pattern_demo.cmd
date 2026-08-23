@echo off
setlocal

set "SNR_DB=%~1"
if not defined SNR_DB set "SNR_DB=45"
start "OpenISAC PHY Channel" cmd /k call "%~dp0run_video_channel_sim.cmd" --snr %SNR_DB% --cfo 300 --sfo 20 --tdl 0:0:0+3:-4:45+9:-8:-80
timeout /t 1 /nobreak >nul
call "%~dp0start_vlc_video_receiver.cmd"
if errorlevel 1 exit /b 1
timeout /t 1 /nobreak >nul
call "%~dp0start_vlc_test_pattern_sender.cmd"
exit /b %ERRORLEVEL%
