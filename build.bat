@echo off
setlocal EnableDelayedExpansion

echo.
echo   Building Quantum Language v2.0.0  ^|  Bytecode VM
echo   quantum hello.sa  =^>  hello.exe   ^(compile + bundle^)
echo   qrun    hello.sa  =^>  runs directly  ^(interpret^)
echo.

rem ══════════════════════════════════════════════════════════════════════════════
rem  STEP 1 — MSYS2  ^(provides mingw32-make, gcc, g++^)
rem ══════════════════════════════════════════════════════════════════════════════
set "MSYS2_ROOT="
if exist "C:\msys64"       set "MSYS2_ROOT=C:\msys64"
if exist "C:\msys2"        set "MSYS2_ROOT=C:\msys2"
if exist "%USERPROFILE%\msys64" set "MSYS2_ROOT=%USERPROFILE%\msys64"

if "!MSYS2_ROOT!"=="" (
    echo.
    echo   [MISSING] MSYS2 is not installed.
    echo   MSYS2 provides the MinGW toolchain ^(gcc, g++, mingw32-make^).
    echo.
    set /p "INST_MSYS2=   Install MSYS2 now? [Y/N]: "
    if /i "!INST_MSYS2!"=="Y" (
        where winget >nul 2>&1
        if errorlevel 1 (
            echo   [ERROR] winget not found, so MSYS2 can't be auto-installed.
            echo   Either install "App Installer" from the Microsoft Store and re-run,
            echo   or install MSYS2 manually from https://www.msys2.org/
            pause
            exit /b 1
        )
        echo   Downloading and installing MSYS2 via winget...
        winget install --id MSYS2.MSYS2 --silent --accept-package-agreements --accept-source-agreements
        if errorlevel 1 (
            echo   [ERROR] MSYS2 install failed. Please install manually from https://www.msys2.org/
            pause
            exit /b 1
        )
        rem Default winget install path
        if exist "C:\msys64" set "MSYS2_ROOT=C:\msys64"
        echo   MSYS2 installed to !MSYS2_ROOT!
    ) else (
        echo   Skipped. Cannot build without MSYS2.  Exiting.
        pause
        exit /b 1
    )
)

rem ── Inject MSYS2 ucrt64 toolchain into PATH for this session ──────────────────
set "PATH=!MSYS2_ROOT!\ucrt64\bin;!MSYS2_ROOT!\mingw64\bin;!MSYS2_ROOT!\usr\bin;C:\MinGW\bin;C:\MinGW64\bin;C:\TDM-GCC-64\bin;%PATH%"

rem ── Make sure MinGW toolchain packages are installed inside MSYS2 ─────────────
"!MSYS2_ROOT!\usr\bin\pacman.exe" -Q mingw-w64-ucrt-x86_64-gcc >nul 2>&1
if errorlevel 1 (
    echo.
    echo   [MISSING] MinGW gcc/g++ toolchain not found inside MSYS2.
    set /p "INST_GCC=   Install mingw-w64-ucrt-x86_64-toolchain now? [Y/N]: "
    if /i "!INST_GCC!"=="Y" (
        echo   Installing toolchain ^(this may take a few minutes^)...
        "!MSYS2_ROOT!\usr\bin\pacman.exe" -S --noconfirm mingw-w64-ucrt-x86_64-toolchain
        if errorlevel 1 (
            echo   [ERROR] Toolchain install failed.
            pause
            exit /b 1
        )
    ) else (
        echo   Skipped. Cannot build without gcc/g++.  Exiting.
        pause
        exit /b 1
    )
)

rem ══════════════════════════════════════════════════════════════════════════════
rem  STEP 2 — Locate make
rem ══════════════════════════════════════════════════════════════════════════════
set "MAKE_EXE="
where mingw32-make >nul 2>&1 && set "MAKE_EXE=mingw32-make" && goto :make_found
where make         >nul 2>&1 && set "MAKE_EXE=make"         && goto :make_found

echo.
echo   [MISSING] mingw32-make / make still not found after MSYS2 setup.
echo   Trying to install make package directly...
"!MSYS2_ROOT!\usr\bin\pacman.exe" -S --noconfirm mingw-w64-ucrt-x86_64-make >nul 2>&1
where mingw32-make >nul 2>&1 && set "MAKE_EXE=mingw32-make" && goto :make_found
where make         >nul 2>&1 && set "MAKE_EXE=make"         && goto :make_found

echo   [ERROR] Could not locate make. Please open an MSYS2 UCRT64 shell and run:
echo     pacman -S mingw-w64-ucrt-x86_64-make
pause
exit /b 1

:make_found
echo   make   : !MAKE_EXE!

rem ══════════════════════════════════════════════════════════════════════════════
rem  STEP 3 — Locate cmake
rem ══════════════════════════════════════════════════════════════════════════════
set "CMAKE_EXE="
where cmake >nul 2>&1 && set "CMAKE_EXE=cmake" && goto :cmake_found
if exist "C:\Program Files\CMake\bin\cmake.exe" (
    set "CMAKE_EXE=C:\Program Files\CMake\bin\cmake.exe"
    goto :cmake_found
)
if exist "!MSYS2_ROOT!\ucrt64\bin\cmake.exe" (
    set "CMAKE_EXE=!MSYS2_ROOT!\ucrt64\bin\cmake.exe"
    goto :cmake_found
)

echo.
echo   [MISSING] CMake is not installed.
echo   CMake is the build-system generator required to configure the project.
echo.
set /p "INST_CMAKE=   Install CMake now? [Y/N]: "
if /i "!INST_CMAKE!"=="Y" (
    echo   Installing CMake via winget...
    winget install --id Kitware.CMake --silent --accept-package-agreements --accept-source-agreements
    if errorlevel 1 (
        echo   winget install failed — trying pacman inside MSYS2...
        "!MSYS2_ROOT!\usr\bin\pacman.exe" -S --noconfirm mingw-w64-ucrt-x86_64-cmake
    )
    rem Refresh PATH
    set "PATH=C:\Program Files\CMake\bin;!MSYS2_ROOT!\ucrt64\bin;%PATH%"
    where cmake >nul 2>&1 && set "CMAKE_EXE=cmake" && goto :cmake_found
    if exist "C:\Program Files\CMake\bin\cmake.exe" set "CMAKE_EXE=C:\Program Files\CMake\bin\cmake.exe"
    if exist "!MSYS2_ROOT!\ucrt64\bin\cmake.exe"    set "CMAKE_EXE=!MSYS2_ROOT!\ucrt64\bin\cmake.exe"
    if "!CMAKE_EXE!"=="" (
        echo   [ERROR] CMake still not found after install. Please reopen the terminal and retry.
        pause
        exit /b 1
    )
    goto :cmake_found
) else (
    echo   Skipped. Cannot build without CMake.  Exiting.
    pause
    exit /b 1
)

:cmake_found
echo   cmake  : !CMAKE_EXE!

rem ══════════════════════════════════════════════════════════════════════════════
rem  STEP 4 — Clean + Configure + Build
rem ══════════════════════════════════════════════════════════════════════════════
echo.
echo   Cleaning old build...
if exist build rmdir /S /Q build
mkdir build
cd build

echo   Configuring...
"%CMAKE_EXE%" .. -DCMAKE_BUILD_TYPE=Release -G "MinGW Makefiles" -DCMAKE_MAKE_PROGRAM="!MAKE_EXE!" > ..\build_log.txt 2>&1
if errorlevel 1 (
    cd ..
    echo.
    echo   [ERROR] CMake configure failed — full log:
    type build_log.txt
    pause
    exit /b 1
)

echo   Compiling...
"!MAKE_EXE!" 2> ..\build_errors.txt
if errorlevel 1 (
    cd ..
    echo.
    echo   [ERROR] Compile failed:
    type build_errors.txt
    pause
    exit /b 1
)

cd ..

rem ══════════════════════════════════════════════════════════════════════════════
rem  STEP 5 — Copy binaries to project root
rem ══════════════════════════════════════════════════════════════════════════════
copy /Y build\quantum.exe      quantum.exe      >nul
copy /Y build\qrun.exe         qrun.exe         >nul
copy /Y build\quantum_stub.exe quantum_stub.exe >nul
copy /Y build\qpm.exe          qpm.exe          >nul

echo.
echo   Build successful
echo.
echo   Binaries copied to project root:
echo     quantum.exe       ^<-- compiler + bundler
echo     qrun.exe          ^<-- direct interpreter
echo     quantum_stub.exe  ^<-- standalone runtime  ^(template for hello.exe etc.^)
echo     qpm.exe           ^<-- standalone npm-compatible package manager
echo.
echo   Usage:
echo     quantum hello.sa        ^<-- compiles hello.sa into hello.exe, then runs it
echo     hello.exe               ^<-- run the compiled program directly
echo     qrun    hello.sa        ^<-- interprets hello.sa in-place, no .exe created
echo.
echo   Other flags:
echo     quantum --debug hello.sa    ^<-- dump bytecode then run
echo     quantum --dis   hello.sa    ^<-- dump bytecode only
echo     quantum --check hello.sa    ^<-- parse + type-check only
echo     quantum --test  examples    ^<-- batch test all .sa files
echo.
echo   Package manager:
echo     qpm install              ^<-- downloads all npm packages from package.json
echo     qpm install express      ^<-- adds + installs a specific package
echo     qpm run dev               ^<-- runs a package.json "scripts" entry
echo     qpm start                 ^<-- shorthand for `qpm run start`
echo.
endlocal