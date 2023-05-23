@echo off
setlocal EnableExtensions
rem Run in a VS2015 Update 3 x64 Native Tools command prompt.
rem Usage: build.cmd "C:\SDK\Adobe After Effects CC 15.0 Win SDK"
if "%~1"=="" goto usage
where cl.exe >nul 2>nul
if errorlevel 1 goto toolerror
where rc.exe >nul 2>nul
if errorlevel 1 goto toolerror
set "SKB_SDK=%~f1"
set "SKB_ROOT=%~dp0"
set "SKB_HEADERS=%SKB_SDK%\Examples\Headers"
set "SKB_RESOURCES=%SKB_SDK%\Examples\Resources"
if not exist "%SKB_HEADERS%\AE_GeneralPlug.h" goto sdkerror
if not exist "%SKB_RESOURCES%\PiPLtool.exe" goto sdkerror
if not exist "%SKB_ROOT%build" mkdir "%SKB_ROOT%build"
pushd "%SKB_ROOT%build"
rem /TC: preprocess Rez source as C. Both stages match Adobe's Windows samples.
cl /nologo /TC /EP /DWIN32 /D_WINDOWS /DWIN_ENV /D_WIN64 /I"%SKB_HEADERS%" /I"%SKB_RESOURCES%" "%SKB_ROOT%resources\SoundKeysBridge.r" > SoundKeysBridge.rr
if errorlevel 1 goto failed
"%SKB_RESOURCES%\PiPLtool.exe" SoundKeysBridge.rr SoundKeysBridge.rrc
if errorlevel 1 goto failed
cl /nologo /TC /EP /DMSWindows SoundKeysBridge.rrc > SoundKeysBridgePiPL.rc
if errorlevel 1 goto failed
rc /nologo /fo SoundKeysBridgePiPL.res SoundKeysBridgePiPL.rc
if errorlevel 1 goto failed
rc /nologo /fo Version.res "%SKB_ROOT%resources\Version.rc"
if errorlevel 1 goto failed
rem No /std flag: VS2015 uses its C++14 implementation by default.
rem /MT avoids deploying a separate MSVC runtime DLL to the historical worker.
cl /nologo /LD /O2 /W4 /EHsc /MT /DWIN32 /D_WINDOWS /DWIN_ENV /D_WIN64 /D_WIN32_WINNT=0x0601 /DNOMINMAX /DUNICODE /D_UNICODE /I"%SKB_ROOT%include" /I"%SKB_HEADERS%" /I"%SKB_HEADERS%\SP" /I"%SKB_HEADERS%\Win" /I"%SKB_RESOURCES%" "%SKB_ROOT%src\Protocol.cpp" "%SKB_ROOT%src\WindowsIO.cpp" "%SKB_ROOT%src\AEHost.cpp" "%SKB_ROOT%src\Plugin.cpp" SoundKeysBridgePiPL.res Version.res version.lib /link /MACHINE:X64 /OUT:SoundKeysBridge.aex
if errorlevel 1 goto failed
echo Built %SKB_ROOT%build\SoundKeysBridge.aex
popd
exit /b 0
:failed
popd
exit /b 1
:usage
echo Usage: build.cmd "path to Adobe After Effects CC 15.0 or 16.0 Win SDK"
exit /b 2
:toolerror
echo Run from the VS2015 Update 3 x64 Native Tools command prompt with Windows SDK installed.
exit /b 2
:sdkerror
echo Missing Examples\Headers\AE_GeneralPlug.h or Examples\Resources\PiPLtool.exe in SDK root.
exit /b 2
