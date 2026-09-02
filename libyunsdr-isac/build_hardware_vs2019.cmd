@echo off
call "%~dp0build_vs2019.cmd" ninja-vs2019-hardware ON
exit /b %errorlevel%
