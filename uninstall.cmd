@echo off
setlocal

net session >nul 2>&1
if errorlevel 1 (
  echo Administrator rights are required to remove the COM class.
  echo Requesting elevation...
  powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -ArgumentList 'elevated' -Verb RunAs"
  exit /b 0
)

set "DEST=%ProgramFiles%\AepThumb"
set "OLD=%LOCALAPPDATA%\AepThumb"

if exist "%DEST%\aepthumb.dll" regsvr32 /s /u "%DEST%\aepthumb.dll"
if exist "%OLD%\aepthumb.dll"  regsvr32 /s /u "%OLD%\aepthumb.dll"
taskkill /f /im dllhost.exe >nul 2>&1

del /q "%DEST%\aepthumb.dll" 2>nul
rd "%DEST%" 2>nul
del /q "%OLD%\aepthumb.dll" 2>nul
rd "%OLD%" 2>nul

rem Drop leftovers from the per-user attempt.
reg delete "HKCU\Software\Classes\CLSID\{8E76F525-03F4-403B-A170-1623A5878F14}" /f >nul 2>&1

echo.
echo Removed. The Ardfry PSD codec handler in HKLM takes .aep back, which
echo means blank thumbnails again - its keys were never modified.
echo Run refresh-thumbnails.cmd to clear the cached tiles.

if /i "%~1"=="elevated" (
  echo.
  pause
)
exit /b 0
