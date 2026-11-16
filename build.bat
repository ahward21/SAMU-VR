@echo off
setlocal
set "CMAKE=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
set "CFG=%~1"
if "%CFG%"=="" set "CFG=Release"
cd /d "%~dp0"
if not exist "build\CMakeCache.txt" (
    "%CMAKE%" -S . -B build -G "Visual Studio 17 2022" -A Win32 || exit /b 1
)
"%CMAKE%" --build build --config %CFG% --parallel || exit /b 1
echo.
echo Build OK: dist\d3d9.dll
