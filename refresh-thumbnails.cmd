@echo off
setlocal
echo This clears the Windows thumbnail cache and restarts Explorer.
echo Open Explorer windows will close. Desktop icons reappear on their own.
echo.
choice /c yn /m "Continue"
if errorlevel 2 exit /b 0

taskkill /f /im explorer.exe >nul 2>&1
taskkill /f /fi "imagename eq dllhost.exe" /fi "modules eq aepthumb.dll" >nul 2>&1
del /f /q "%LOCALAPPDATA%\Microsoft\Windows\Explorer\thumbcache_*.db" 2>nul
start "" explorer.exe
echo Done.
exit /b 0
