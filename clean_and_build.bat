@echo off
chcp 65001 >nul
setlocal

set BUILD_DIR=%~dp0build
set VCVARS="C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvarsall.bat"

echo =========================================
echo  proJV - Clean & Build
echo =========================================
echo.

where cmake >nul 2>&1
if errorlevel 1 (
    echo [ERROR] CMake not found
    exit /b 1
)

if not exist %VCVARS% (
    echo [ERROR] Visual Studio 2019 not found
    exit /b 1
)

echo [1/4] Loading VS environment...
call %VCVARS% x64 >nul 2>&1

where cl >nul 2>&1
if errorlevel 1 (
    echo [ERROR] MSVC compiler not found
    exit /b 1
)

echo [2/4] Cleaning...
if exist "%BUILD_DIR%" rmdir /s /q "%BUILD_DIR%" 2>nul
mkdir "%BUILD_DIR%" 2>nul

echo [3/4] Configuring...
cd /d "%BUILD_DIR%"
cmake .. -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release
if errorlevel 1 (
    echo [FAIL] Configure failed
    exit /b 1
)

echo [4/4] Building...
nmake /nologo
if errorlevel 1 (
    echo [FAIL] Build failed
    exit /b 1
)

echo.
echo Build OK - Output: %BUILD_DIR%\proJV.exe
endlocal
