@echo off
setlocal EnableExtensions EnableDelayedExpansion

set "ASIO_KEY_MASTER=SOFTWARE\ASIO\MyASIO Splitter"
set "ASIO_KEY_TAP=SOFTWARE\ASIO\MyASIO Splitter (Tap)"
set "CLSID_MASTER={7C9E5B6A-3F2D-4C81-9E4A-5D6F8B1C2A30}"
set "CLSID_TAP={9A4F2D18-6B3C-4E57-A1D0-2C8E7F4B9A61}"

fltmc >nul 2>&1
if errorlevel 1 (
    echo Administrator rights are required. Requesting elevation...
    powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
    if errorlevel 1 (
        echo Could not elevate. Right-click install.bat and choose "Run as administrator".
        pause
    )
    exit /b
)

set "ARCH=%PROCESSOR_ARCHITECTURE%"
if defined PROCESSOR_ARCHITEW6432 set "ARCH=%PROCESSOR_ARCHITEW6432%"
if /i not "%ARCH%"=="AMD64" (
    echo MyASIO Splitter is x64 Windows only.
    pause
    exit /b 1
)

set "REGSVR32=%SystemRoot%\System32\regsvr32.exe"
if defined PROCESSOR_ARCHITEW6432 set "REGSVR32=%SystemRoot%\Sysnative\regsvr32.exe"

set "INSTALL_ROOT=%ProgramFiles%"
if defined ProgramW6432 set "INSTALL_ROOT=%ProgramW6432%"
set "INSTALL_DIR=%INSTALL_ROOT%\MyAsioSplitter"
set "INSTALL_DLL=%INSTALL_DIR%\MyAsioSplitter.dll"

set "SOURCE_DLL=%~dp0MyAsioSplitter.dll"
if not exist "%SOURCE_DLL%" set "SOURCE_DLL=%~dp0build\Release\MyAsioSplitter.dll"

set "INSTALLED=0"
if exist "%INSTALL_DLL%" set "INSTALLED=1"
reg query "HKLM\%ASIO_KEY_MASTER%" >nul 2>&1
if not errorlevel 1 set "INSTALLED=1"
reg query "HKLM\%ASIO_KEY_TAP%" >nul 2>&1
if not errorlevel 1 set "INSTALLED=1"

if "%INSTALLED%"=="1" goto menu
goto install

:menu
choice /C RUC /N /M "MyASIO Splitter is installed. [R]einstall, [U]ninstall, [C]ancel? "
if errorlevel 3 goto done
if errorlevel 2 goto uninstall
goto install

:install
if not exist "%SOURCE_DLL%" (
    echo Could not find MyAsioSplitter.dll.
    echo Put it next to this script, or build the project first.
    goto fail
)

echo Installing MyASIO Splitter...
echo Close Pianoteq, OBS, and any other ASIO host before continuing.
call :unregister

if not exist "%INSTALL_DIR%" mkdir "%INSTALL_DIR%"
copy /y "%SOURCE_DLL%" "%INSTALL_DLL%" >nul
if errorlevel 1 (
    if exist "%INSTALL_DLL%" "%REGSVR32%" /s "%INSTALL_DLL%" >nul 2>&1
    echo Could not copy the DLL. Close any application using it and try again.
    goto fail
)

"%REGSVR32%" /s "%INSTALL_DLL%"
if errorlevel 1 (
    echo regsvr32 failed to register the DLL.
    goto fail
)

reg query "HKLM\%ASIO_KEY_MASTER%" >nul 2>&1
if errorlevel 1 goto registration_failed
reg query "HKLM\%ASIO_KEY_TAP%" >nul 2>&1
if errorlevel 1 goto registration_failed

echo.
echo Installed: %INSTALL_DLL%
echo Start the master application first and select "MyASIO Splitter".
echo Then start OBS and select "MyASIO Splitter (Tap)".
echo Never select the non-Tap device in the second application.
goto done

:registration_failed
echo Registration did not create both ASIO entries.
goto fail

:uninstall
echo Uninstalling MyASIO Splitter...
call :unregister
if exist "%INSTALL_DIR%" rd /s /q "%INSTALL_DIR%"
if exist "%INSTALL_DLL%" (
    echo The DLL is still in use. Close Pianoteq and OBS, then delete:
    echo   %INSTALL_DIR%
)
if exist "%APPDATA%\MyAsioSplitter" rd /s /q "%APPDATA%\MyAsioSplitter"
echo Uninstalled.
goto done

:unregister
for %%G in ("%CLSID_MASTER%" "%CLSID_TAP%") do (
    set "REG_DLL="
    for /f "tokens=2,*" %%A in ('reg query "HKCR\CLSID\%%~G\InprocServer32" /ve 2^>nul ^| findstr /i "REG_SZ"') do set "REG_DLL=%%B"
    if defined REG_DLL (
        if exist "!REG_DLL!" "%REGSVR32%" /s /u "!REG_DLL!" >nul 2>&1
    )
)
if exist "%INSTALL_DLL%" "%REGSVR32%" /s /u "%INSTALL_DLL%" >nul 2>&1

reg delete "HKLM\%ASIO_KEY_MASTER%" /f >nul 2>&1
reg delete "HKLM\%ASIO_KEY_TAP%" /f >nul 2>&1
reg delete "HKLM\SOFTWARE\Classes\CLSID\%CLSID_MASTER%" /f >nul 2>&1
reg delete "HKLM\SOFTWARE\Classes\CLSID\%CLSID_TAP%" /f >nul 2>&1
reg delete "HKCU\SOFTWARE\Classes\CLSID\%CLSID_MASTER%" /f >nul 2>&1
reg delete "HKCU\SOFTWARE\Classes\CLSID\%CLSID_TAP%" /f >nul 2>&1
exit /b 0

:done
echo.
pause
exit /b 0

:fail
echo.
pause
exit /b 1
