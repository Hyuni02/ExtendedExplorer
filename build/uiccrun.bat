@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
"%WindowsSdkVerBinPath%x86\uicc.exe" ribbon.xml build\ribbon.bml /header:build\ribbon_ids.h /res:build\ribbon.rc /name:APPLICATION
