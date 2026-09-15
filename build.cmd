@echo off
setlocal enabledelayedexpansion

rem Locate a Visual Studio C++ toolchain.
set "VCVARS="
for %%E in (BuildTools Community Professional Enterprise Preview) do (
  if not defined VCVARS (
    if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\2022\%%E\VC\Auxiliary\Build\vcvars64.bat" (
      set "VCVARS=%ProgramFiles(x86)%\Microsoft Visual Studio\2022\%%E\VC\Auxiliary\Build\vcvars64.bat"
    )
  )
  if not defined VCVARS (
    if exist "%ProgramFiles%\Microsoft Visual Studio\2022\%%E\VC\Auxiliary\Build\vcvars64.bat" (
      set "VCVARS=%ProgramFiles%\Microsoft Visual Studio\2022\%%E\VC\Auxiliary\Build\vcvars64.bat"
    )
  )
)
if not defined VCVARS (
  echo ERROR: no Visual Studio 2022 x64 toolchain found.
  exit /b 1
)

call "%VCVARS%" >nul
if errorlevel 1 exit /b 1

cd /d "%~dp0"
for %%D in (obj-exe obj-bake obj-dll) do if not exist build\%%D mkdir build\%%D

set "CFLAGS=/nologo /std:c++17 /EHsc /O2 /W3 /MT /GS /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN"
set "LIBS=gdiplus.lib user32.lib gdi32.lib ole32.lib oleaut32.lib advapi32.lib shell32.lib uuid.lib"

rem Version information and the icon. rc runs from the repository root, which
rem is what the icon path inside the .rc files is relative to.
echo [0/3] resources
rc /nologo /i src /fo build\obj-bake\aepbake.res src\aepbake.rc
if errorlevel 1 exit /b 1
rc /nologo /i src /fo build\obj-dll\aepthumb.res src\aepthumb.rc
if errorlevel 1 exit /b 1

echo [1/3] build\aepinfo.exe
cl %CFLAGS% tools\aepinfo.cpp src\aep.cpp src\render.cpp src\cache.cpp ^
   /Fe:build\aepinfo.exe /Fo:build\obj-exe\ /Fd:build\obj-exe\ ^
   /link /INCREMENTAL:NO %LIBS%
if errorlevel 1 exit /b 1

echo [2/3] build\aepbake.exe
cl %CFLAGS% src\bake.cpp src\aep.cpp src\cache.cpp build\obj-bake\aepbake.res ^
   /Fe:build\aepbake.exe /Fo:build\obj-bake\ /Fd:build\obj-bake\ ^
   /link /INCREMENTAL:NO %LIBS%
if errorlevel 1 exit /b 1

echo [3/3] build\aepthumb.dll
cl %CFLAGS% /LD src\thumb.cpp src\aep.cpp src\render.cpp src\cache.cpp build\obj-dll\aepthumb.res ^
   /Fe:build\aepthumb.dll /Fo:build\obj-dll\ /Fd:build\obj-dll\ ^
   /link /INCREMENTAL:NO /DEF:src\aepthumb.def %LIBS%
if errorlevel 1 exit /b 1

rem The baker launches this script and looks for it beside its own exe.
copy /y tools\bake_batch.jsx build\ >nul

echo.
echo Build OK.
dir /b build\aepinfo.exe build\aepbake.exe build\aepthumb.dll build\bake_batch.jsx 2>nul
exit /b 0
