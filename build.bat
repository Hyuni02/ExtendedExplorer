@echo off
rem Build ExtendedExplorer. No args: app only. "test": also build+run selftest.
setlocal
set VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat
if not exist "%VCVARS%" (
  echo vcvars64.bat not found: "%VCVARS%"
  exit /b 1
)
call "%VCVARS%" >nul 2>&1
if not exist build mkdir build

rem SPEC 14: ribbon markup -> binary + resource + command ID header (keep this file ASCII: cmd reads it in the ANSI code page)
"%WindowsSdkVerBinPath%x86\uicc.exe" ribbon.xml build\ribbon.bml /header:build\ribbon_ids.h /res:build\ribbon.rc /name:APPLICATION >build\uicc.log
if errorlevel 1 (type build\uicc.log & exit /b 1)
rc /nologo /fo build\ribbon.res build\ribbon.rc
if errorlevel 1 exit /b 1

rc /nologo /fo build\app.res app.rc
if errorlevel 1 exit /b 1

cl /nologo /utf-8 /O2 /W3 /EHsc /std:c++17 /DUNICODE /D_UNICODE /Ibuild ^
   /Fo:build\ /Fe:build\ExtendedExplorer.exe main.cpp build\ribbon.res build\app.res ^
   /link /SUBSYSTEM:WINDOWS
if errorlevel 1 exit /b 1

if /i "%1"=="test" (
  cl /nologo /utf-8 /Od /W3 /EHsc /std:c++17 /DUNICODE /D_UNICODE /DSELFTEST /Ibuild ^
     /Fo:build\t_ /Fe:build\selftest.exe main.cpp /link /SUBSYSTEM:CONSOLE
  if errorlevel 1 exit /b 1
  build\selftest.exe || exit /b 1
)
echo built: build\ExtendedExplorer.exe
