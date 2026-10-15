@echo off
setlocal
set "VSLANG=1033"
call "%~1\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 exit /b 1
rem Keep localized /showIncludes output and Ninja's dependency prefix in UTF-8.
chcp 65001 >nul
if errorlevel 1 exit /b 1
set "SC_CMAKE=%~2"
set "SC_NINJA=%~1\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
set "SC_ROOT=%~dp0..\..\.."
set "SC_BUILD=%~dp0..\..\..\build\native-ninja"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Get-MsvcDependencyPrefix.ps1" -BuildRoot "%SC_BUILD%"
if errorlevel 1 exit /b 1
for /f "usebackq delims=" %%P in ("%SC_BUILD%\msvc-dependency-prefix.txt") do set "SC_DEPS_PREFIX=%%P"
"%SC_CMAKE%" -S "%SC_ROOT%" -B "%SC_BUILD%" -G "Ninja Multi-Config" -DBUILD_TESTING=ON -DCMAKE_MAKE_PROGRAM="%SC_NINJA%" -DSONYCONNECT_MSVC_DEPS_PREFIX="%SC_DEPS_PREFIX%"
if errorlevel 1 exit /b 1
"%SC_CMAKE%" --build "%SC_BUILD%" --config %3 --parallel
if errorlevel 1 exit /b 1
if "%~4"=="skip" exit /b 0
for %%I in ("%SC_CMAKE%") do set "SC_CTEST=%%~dpIctest.exe"
"%SC_CTEST%" --test-dir "%SC_BUILD%" -C %3 --output-on-failure
exit /b %errorlevel%
