@echo off
rem Shows whether AepThumb is wired up and whether After Effects will co-operate.
"%~dp0aepbake.exe" --doctor
echo.
echo ---------------------------------------------------------------
echo If a line above says After Effects blocks scripts from writing
echo files, open After Effects and turn on:
echo.
echo   Edit ^> Preferences ^> Scripting and Expressions ^>
echo   "Allow Scripts to Write Files and Access Network"
echo.
echo then restart After Effects. Nothing can be rendered without it.
echo ---------------------------------------------------------------
echo.
echo To bake an existing library up front instead of waiting for
echo folders to be browsed:
echo.
echo   "%~dp0aepbake.exe" --scan "D:\YOUR\PROJECTS" -r
echo.
pause
