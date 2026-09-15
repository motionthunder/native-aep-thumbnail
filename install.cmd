@echo off
setlocal

rem The COM class must live in HKLM: the shell will not activate a thumbnail
rem provider registered only per-user. That is the only reason this needs
rem elevation - process isolation stays on, so the DLL runs in dllhost.exe and
rem never loads into explorer.exe.
net session >nul 2>&1
if errorlevel 1 (
  echo Administrator rights are required to register the COM class.
  echo Requesting elevation...
  powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -ArgumentList 'elevated' -Verb RunAs"
  exit /b 0
)

set "SRC=%~dp0build"
set "DEST=%ProgramFiles%\AepThumb"

for %%F in (aepthumb.dll aepbake.exe bake_batch.jsx) do (
  if not exist "%SRC%\%%F" (
    echo ERROR: build\%%F not found. Run build.cmd first.
    goto :end
  )
)

rem Clear anything an earlier run left behind, including the per-user attempt
rem and the in-process setting a previous design needed.
if exist "%LOCALAPPDATA%\AepThumb\aepthumb.dll" (
  regsvr32 /s /u "%LOCALAPPDATA%\AepThumb\aepthumb.dll"
  del /q "%LOCALAPPDATA%\AepThumb\aepthumb.dll" 2>nul
)
rem A running baker holds its own exe open, and the shell host holds the DLL.
taskkill /f /im aepbake.exe >nul 2>&1
if exist "%DEST%\aepthumb.dll" regsvr32 /s /u "%DEST%\aepthumb.dll"
reg delete "HKCU\Software\Classes\CLSID\{8E76F525-03F4-403B-A170-1623A5878F14}" /f >nul 2>&1
reg delete "HKLM\Software\Classes\CLSID\{8E76F525-03F4-403B-A170-1623A5878F14}" /v DisableProcessIsolation /f >nul 2>&1
taskkill /f /fi "imagename eq dllhost.exe" /fi "modules eq aepthumb.dll" >nul 2>&1

if not exist "%DEST%" mkdir "%DEST%"
for %%F in (aepthumb.dll aepbake.exe bake_batch.jsx) do (
  copy /y "%SRC%\%%F" "%DEST%\%%F" >nul
  if errorlevel 1 (
    echo ERROR: could not copy %%F to "%DEST%".
    echo        Stop any running bake and try again.
    goto :end
  )
)

regsvr32 /s "%DEST%\aepthumb.dll"
if errorlevel 1 (
  echo ERROR: registration failed.
  goto :end
)

echo.
echo Installed to %DEST%
echo   aepthumb.dll    thumbnail provider, shows baked frames from the cache
echo   aepbake.exe     baker, drives After Effects
echo   bake_batch.jsx  the script it feeds to After Effects
echo.
echo COM class : HKLM, isolated in dllhost.exe (no DisableProcessIsolation)
echo .aep/.aet : HKLM association; the previous handler is kept for uninstall
echo Cache     : %%LOCALAPPDATA%%\AepThumb\cache
echo.
echo Open any folder with .aep files. Tiles turn into rendered frames on their
echo own within a few seconds, even while After Effects is open.
echo.
echo Optional, to render a whole library up front:
echo     "%DEST%\aepbake.exe" --scan "D:\YOUR\PROJECTS" -r
echo     "%DEST%\aepbake.exe" --doctor

:end
if /i "%~1"=="elevated" (
  echo.
  pause
)
exit /b 0
