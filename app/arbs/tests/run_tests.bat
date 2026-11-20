@echo off
rem ARBS-W Windows layer + bridge logic unit tests (B2 D7/D8). Local only: no HID enumeration/opening, no network,
rem no installation. The HID port class is exercised over a local named pipe. All environment changes stay in this
rem cmd process (setlocal). Output folder: <ARBS-APEX>\client-w\build-arbs-w-tests (outside the arbs-w repository).
rem Usage: run_tests.bat [filter-substring]
setlocal enableDelayedExpansion

set HERE=%~dp0
set HERE=%HERE:~0,-1%
for %%i in ("%HERE%\..") do set ARBSDIR=%%~fi
for %%i in ("%HERE%\..\..\..\..\..") do set ROOT=%%~fi
set CORE=%ROOT%\core\arbs-core
set BUILD=%ROOT%\client-w\build-arbs-w-tests

rem --- side-effect guards (same as client-w\scripts\arbs-w-build.bat) ---
set VSCMD_SKIP_SENDTELEMETRY=1
set DOTNET_CLI_TELEMETRY_OPTOUT=1
set DOTNET_NOLOGO=1
set DOTNET_SKIP_FIRST_TIME_EXPERIENCE=1
set DOTNET_GENERATE_ASPNET_CERTIFICATE=false
set DOTNET_ADD_GLOBAL_TOOLS_TO_PATH=false
set DOTNET_CLI_HOME=D:\AI\toolchain\dotnet-home
set NUGET_PACKAGES=D:\AI\toolchain\nuget-packages
set NUGET_HTTP_CACHE_PATH=D:\AI\toolchain\nuget-http-cache
set NUGET_PLUGINS_CACHE_PATH=D:\AI\toolchain\nuget-plugins-cache
set NUGET_SCRATCH=D:\AI\toolchain\nuget-scratch
set MSBUILDDISABLENODEREUSE=1
set CL=

set VCVARS=%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat
if not defined VCToolsInstallDir (
    if not exist "!VCVARS!" (
        echo vcvarsall.bat not found: !VCVARS!
        exit /b 1
    )
    call "!VCVARS!" x64 >nul
    if errorlevel 1 exit /b 1
)
echo [arbs-w-tests] VCToolsVersion=%VCToolsVersion% core=%CORE%

if not exist "%BUILD%\obj" mkdir "%BUILD%\obj"
if not exist "%BUILD%\tmp" mkdir "%BUILD%\tmp"
del /q "%BUILD%\obj\*.obj" 2>nul

set FLAGS=/nologo /std:c++17 /W4 /permissive- /EHsc /utf-8 /O2 /fp:precise /MD /Zc:__cplusplus /DUNICODE /D_UNICODE /I"%CORE%\include" /I"%CORE%\tests" /I"%ARBSDIR%"

echo [arbs-w-tests] compile
cl %FLAGS% /c "%CORE%\src\*.cpp" "%ARBSDIR%\arbs_logic.cpp" "%ARBSDIR%\arbs_log.cpp" "%ARBSDIR%\win_clock.cpp" "%ARBSDIR%\win_hid_port.cpp" "%HERE%\test_arbs_w.cpp" /Fo"%BUILD%\obj\\"
if errorlevel 1 exit /b 2
link /nologo /OUT:"%BUILD%\arbs_w_tests.exe" "%BUILD%\obj\*.obj" hid.lib setupapi.lib avrt.lib
if errorlevel 1 exit /b 3

echo [arbs-w-tests] run
"%BUILD%\arbs_w_tests.exe" "%BUILD%\tmp" %1
exit /b %ERRORLEVEL%
