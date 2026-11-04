@echo off
setlocal
cd /d "%~dp0"

rem Repository settings are environment variables set by config.bat.
call "%~dp0config.bat"
call "%VCVARS64%" >nul 2>&1
if errorlevel 1 exit /b 1

if not exist out mkdir out
if errorlevel 1 exit /b 1
cl /nologo /O2 /W4 /WX /GS- /Gs262144 /Oi- /std:c11 small_linker.c /Foout\small_linker.obj /Feout\small_linker.exe /link /SUBSYSTEM:CONSOLE /ENTRY:mainCRTStartup /NODEFAULTLIB /MACHINE:X64 kernel32.lib
if errorlevel 1 exit /b 1
cl /nologo /O2 /W4 /WX /GS- /Gs262144 /Oi- /std:c11 tests\link_tests.c /Foout\link_tests.obj /Feout\link_tests.exe /link /SUBSYSTEM:CONSOLE /ENTRY:mainCRTStartup /NODEFAULTLIB /MACHINE:X64 kernel32.lib
if errorlevel 1 exit /b 1

if /I not "%~1"=="test" exit /b 0
out\small_linker.exe --self-test
if errorlevel 1 (
    echo self-test failed, see out\self_test.txt
    type out\self_test.txt
    exit /b 1
)
out\link_tests.exe
exit /b %errorlevel%
