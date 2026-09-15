@echo off
rem Builds the binaries, then the single setup .exe for people who just want
rem to install it:  dist\NativeAEPThumbnail-<version>-Setup.exe
rem
rem Needs Inno Setup 6 (free): winget install JRSoftware.InnoSetup
setlocal

cd /d "%~dp0"

call "%~dp0build.cmd"
if errorlevel 1 (
  echo ERROR: build failed, installer not made.
  exit /b 1
)

set "ISCC="
for %%P in (
  "%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe"
  "%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe"
  "%ProgramFiles%\Inno Setup 6\ISCC.exe"
) do if not defined ISCC if exist %%P set "ISCC=%%~P"

if not defined ISCC (
  echo ERROR: Inno Setup 6 not found.
  echo        Install it with:  winget install JRSoftware.InnoSetup
  exit /b 1
)

echo.
echo Building the installer...
"%ISCC%" /Q installer\AepThumb.iss
if errorlevel 1 (
  echo ERROR: Inno Setup could not build the installer.
  exit /b 1
)

echo.
echo Installer ready:
dir /b dist\*.exe
exit /b 0
