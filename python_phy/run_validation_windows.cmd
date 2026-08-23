@echo off
setlocal
cd /d "%~dp0"

set "PYTHON_EXE="

if exist "%~dp0.venv\Scripts\python.exe" (
    set "PYTHON_EXE=%~dp0.venv\Scripts\python.exe"
)

if defined OPENISAC_PYTHON (
    if exist "%OPENISAC_PYTHON%" set "PYTHON_EXE=%OPENISAC_PYTHON%"
)

if not defined PYTHON_EXE (
    where python.exe >nul 2>nul
    if not errorlevel 1 set "PYTHON_EXE=python.exe"
)

if not defined PYTHON_EXE (
    where py.exe >nul 2>nul
    if not errorlevel 1 set "PYTHON_EXE=py.exe"
)

if not defined PYTHON_EXE (
    set "CODEX_PYTHON=%USERPROFILE%\.cache\codex-runtimes\codex-primary-runtime\dependencies\python\python.exe"
    if exist "%USERPROFILE%\.cache\codex-runtimes\codex-primary-runtime\dependencies\python\python.exe" (
        set "PYTHON_EXE=%USERPROFILE%\.cache\codex-runtimes\codex-primary-runtime\dependencies\python\python.exe"
    )
)

if not defined PYTHON_EXE (
    echo ERROR: Python 3 was not found.
    echo Install Python from https://www.python.org/downloads/windows/
    echo During installation, enable "Add python.exe to PATH".
    echo Alternatively set OPENISAC_PYTHON to the full path of python.exe.
    exit /b 1
)

echo Using Python: %PYTHON_EXE%
"%PYTHON_EXE%" -c "import sys; print(sys.version)"
if errorlevel 1 exit /b %errorlevel%

"%PYTHON_EXE%" -c "import numpy; print('NumPy', numpy.__version__)"
if errorlevel 1 (
    echo ERROR: NumPy is not installed for this Python interpreter.
    echo Run: "%PYTHON_EXE%" -m pip install numpy
    exit /b 1
)

"%PYTHON_EXE%" tests\run_tests.py
if errorlevel 1 exit /b %errorlevel%

"%PYTHON_EXE%" experiments\validate_explicit.py %*
exit /b %errorlevel%
