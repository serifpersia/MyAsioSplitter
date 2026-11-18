@echo off
setlocal EnableExtensions

cd /d "%~dp0"

where cmake >nul 2>&1
if errorlevel 1 (
    echo CMake was not found in PATH.
    goto fail
)

cmake -S . -B build -G "Visual Studio 17 2022" -A x64
if errorlevel 1 goto fail

cmake --build build --config Release
if errorlevel 1 (
    echo.
    echo If linking failed because MyAsioSplitter.dll is in use,
    echo close Pianoteq and OBS, then run this script again.
    goto fail
)

echo.
echo Built build\Release\MyAsioSplitter.dll
exit /b 0

:fail
echo Build failed.
pause
exit /b 1
