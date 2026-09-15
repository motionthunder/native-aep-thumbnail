@echo off
rem ===========================================================================
rem  AepThumb panic switch.
rem
rem  Run once  - previews off. The .aep thumbnail handler is unregistered, the
rem              previous handler is put back, the baker is stopped, and the
rem              Windows thumbnail cache is flushed so tiles look exactly as
rem              they did before AepThumb was ever installed.
rem  Run again - previews back on, working in the background as before.
rem
rem  Nothing is uninstalled and no baked frames are thrown away, so switching
rem  back on is instant. Safe to copy this file anywhere.
rem ===========================================================================
setlocal

set "IFACE={e357fccd-a995-4576-b01f-234630154e96}"
set "CLSID={8E76F525-03F4-403B-A170-1623A5878F14}"
set "DEST=%ProgramFiles%\AepThumb"

rem Registration lives in HKLM, so this needs administrator rights.
net session >nul 2>&1
if errorlevel 1 (
  echo Requesting administrator rights...
  powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -ArgumentList 'elevated' -Verb RunAs"
  exit /b 0
)

if not exist "%DEST%\aepthumb.dll" (
  echo.
  echo   AepThumb is not installed - nothing to switch.
  echo   Expected: %DEST%\aepthumb.dll
  echo.
  goto :end
)

rem Are we currently the handler for .aep?
reg query "HKLM\Software\Classes\.aep\ShellEx\%IFACE%" /ve 2>nul | findstr /i "%CLSID%" >nul
if errorlevel 1 goto :turn_on

rem -------------------------------------------------------------------- off
echo.
echo   Turning AEP previews OFF...
echo.

rem Stop the baker first so it cannot queue more work or hold its own exe.
taskkill /f /im aepbake.exe >nul 2>&1

rem DllUnregisterServer removes our class and restores whichever handler held
rem .aep before AepThumb took it.
regsvr32 /s /u "%DEST%\aepthumb.dll"
if errorlevel 1 (
  echo   ERROR: could not unregister the handler.
  goto :end
)

rem Let go of the DLL inside the shell's thumbnail host.
taskkill /f /fi "imagename eq dllhost.exe" /fi "modules eq aepthumb.dll" >nul 2>&1

call :flush_thumbnails

echo   Done. AEP files look exactly as they did before.
echo   Baked frames are kept, so switching back on is instant.
echo.
echo   Run this file again to turn previews back on.
goto :end

rem --------------------------------------------------------------------- on
:turn_on
echo.
echo   Turning AEP previews ON...
echo.

regsvr32 /s "%DEST%\aepthumb.dll"
if errorlevel 1 (
  echo   ERROR: could not register the handler.
  goto :end
)

call :flush_thumbnails

echo   Done. Browse a folder of projects and frames will appear -
echo   already-baked ones immediately, new ones after a few seconds.
echo.
echo   Check the setup any time with:
echo     "%DEST%\aepbake.exe" --doctor
goto :end

rem ------------------------------------------------------------------ helper
:flush_thumbnails
rem Windows caches the tiles it has already drawn, so without this the change
rem would only show up on folders viewed for the first time. Explorer restarts
rem by itself; open folder windows do close.
echo   Flushing the Windows thumbnail cache and restarting Explorer...
taskkill /f /im explorer.exe >nul 2>&1
ping -n 3 127.0.0.1 >nul
del /f /q "%LOCALAPPDATA%\Microsoft\Windows\Explorer\thumbcache_*.db" >nul 2>&1
rem Started through PowerShell on purpose: "start explorer.exe" would inherit
rem this script's output handles, so anything piping this file would hang for
rem as long as Explorer kept running.
powershell -NoProfile -Command "Start-Process explorer.exe" >nul 2>&1
ping -n 2 127.0.0.1 >nul
goto :eof

:end
echo.
if /i "%~1"=="elevated" pause
exit /b 0
