@echo off
rem Builds Quantum's tools from src\ with Quantum itself: no external compiler
rem or toolchain. seed\qrun.exe (qrun compiled to x86-64 by Quantum's own
rem native backend) compiles qrun from src\; that compiler compiles itself
rem again, and the result builds quantum.exe, quantum_stub.exe and qpm.exe.
rem
rem   build-quantum.bat            build\{qrun,quantum,quantum_stub,qpm}.exe
rem   build-quantum.bat --verify   also check the fixed point: the new qrun
rem                                rebuilds itself byte for byte
rem   build-quantum.bat --seed     then make the new qrun the seed
rem The release (the four exes and version.sa) is also put in build\release\:
rem upload those files to the Google Drive folder `quantum update` reads.
setlocal
cd /d "%~dp0"
set OUT=build
set SH=src\tools\selfhost
rem the library headers come from inside the compiler, never from disk
set QUANTUM_NATIVE_LIB=

if not exist seed\qrun.exe (
    echo [error] seed\qrun.exe is missing
    exit /b 1
)

if not exist %OUT% mkdir %OUT%

echo [0/5] version.sa and lib\ into src\generated\
seed\qrun.exe src\tools\embed.sa || goto :fail

echo [1/5] seed\qrun.exe compiles qrun from src\
seed\qrun.exe --native %SH%\qrun_all.sa %OUT%\stage1.exe || goto :fail

echo [2/5] the new qrun compiles itself
%OUT%\stage1.exe --native %SH%\qrun_all.sa %OUT%\qrun.exe || goto :fail

echo [3/5] quantum.exe
%OUT%\qrun.exe --native %SH%\quantum_all.sa %OUT%\quantum.exe || goto :fail

echo [4/5] quantum_stub.exe
%OUT%\qrun.exe --native %SH%\stub_all.sa %OUT%\quantum_stub.exe || goto :fail

echo [5/5] qpm.exe
%OUT%\qrun.exe --native %SH%\qpm_all.sa %OUT%\qpm.exe || goto :fail

if /i "%~1"=="--verify" goto :verify
if /i "%~2"=="--verify" goto :verify
goto :verified

:verify
echo [verify] qrun.exe rebuilds itself
%OUT%\qrun.exe --native %SH%\qrun_all.sa %OUT%\verify.exe || goto :fail
fc /b %OUT%\qrun.exe %OUT%\verify.exe >nul || (
    echo [error] not a fixed point: qrun.exe and its rebuild differ
    exit /b 1
)
echo [verify] fixed point: identical

:verified
if /i "%~1"=="--seed" goto :seed
if /i "%~2"=="--seed" goto :seed
goto :done

:seed
copy /y %OUT%\qrun.exe seed\qrun.exe >nul || goto :fail
echo [seed] seed\qrun.exe updated

:done
rem the release: what goes into the Google Drive folder `quantum update` reads
if not exist %OUT%\release mkdir %OUT%\release
for %%f in (qrun.exe quantum.exe quantum_stub.exe qpm.exe) do (
    copy /y %OUT%\%%f %OUT%\release\%%f >nul || goto :fail
)
copy /y version.sa %OUT%\release\version.sa >nul || goto :fail
set /p QVER=<version.sa
echo.
echo Release %QVER% in %OUT%\release\ (upload its files to the compiler folder on Google Drive)
echo.
echo Built in %OUT%\:
echo   qrun.exe program.sa          run a program
echo   quantum.exe program.sa       compile it into a standalone program.exe
echo   qrun.exe --native f.cpp o.exe  C++ to native x86-64 code
echo   qpm.exe install ^<pkg^>        packages
exit /b 0

:fail
echo [error] build failed
exit /b 1
