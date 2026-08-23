@echo off
setlocal
cd /d "%~dp0"

if not exist ".venv\Scripts\python.exe" (
    echo ERROR: Project Python environment was not found at:
    echo %CD%\.venv\Scripts\python.exe
    echo Run the environment installation first.
    pause
    exit /b 1
)

set "TCL_LIBRARY=%CD%\.venv\tcl\tcl8.6"
set "TK_LIBRARY=%CD%\.venv\tcl\tk8.6"
".venv\Scripts\python.exe" -m openisac_phy.gui
if errorlevel 1 pause
exit /b %errorlevel%
