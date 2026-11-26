@echo off
rem ===========================================================================
rem  Build Release and deploy a self-contained copy into .\dist
rem  Usage: double-click this file, or run it from a terminal.
rem ===========================================================================
setlocal

set QTDIR=D:\QT\6.12.0\mingw_64
set MINGW=D:\QT\Tools\mingw1310_64
set CMAKE=D:\QT\Tools\CMake_64\bin
set PATH=%CMAKE%;%MINGW%\bin;%QTDIR%\bin;%PATH%

cd /d "%~dp0"

echo [1/4] configure Release ...
cmake -S . -B build\Release -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=%QTDIR%
if errorlevel 1 goto fail

echo [2/4] compile ...
cmake --build build\Release -j4
if errorlevel 1 goto fail

echo [3/4] deploy runtime into dist ...
if not exist dist mkdir dist
copy /y build\Release\shujianffuhe.exe dist\ >nul
windeployqt.exe --release --no-translations --no-system-d3d-compiler --no-opengl-sw dist\shujianffuhe.exe
if errorlevel 1 goto fail

echo [4/4] done.
echo.
echo   Run: %~dp0dist\shujianffuhe.exe
echo.
pause
exit /b 0

:fail
echo.
echo BUILD FAILED, see messages above.
pause
exit /b 1
