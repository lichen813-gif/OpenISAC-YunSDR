@echo off
setlocal

call "%~dp0run_video_channel_sim.cmd" --self-test 20 %*
exit /b %ERRORLEVEL%
