@echo off
setlocal enabledelayedexpansion
title Rogue Squadron 64 for New 3DS - Setup
echo Rogue Squadron 64 for New 3DS - one-time setup
echo.
echo This installs the build tools (only what is missing), then builds the game
echo from your own ROM and copies it to your SD card. It takes a while the first time.
echo.

where winget >nul 2>nul || (echo winget is missing: install "App Installer" from the Microsoft Store, then run Setup again. & pause & exit /b 1)

call :need git "C:\Program Files\Git\cmd\git.exe" Git.Git
call :need cmake "C:\Program Files\CMake\bin\cmake.exe" Kitware.CMake
call :need python "%LOCALAPPDATA%\Programs\Python\Python312\python.exe" Python.Python.3.12
call :need clang "C:\Program Files\LLVM\bin\clang.exe" LLVM.LLVM
if not exist "%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" (
    echo A C++ compiler is needed ^(Visual Studio Build Tools, about 2 GB^).
    choice /m "Install it now"
    if errorlevel 2 exit /b 1
    winget install -e --id Microsoft.VisualStudio.2022.BuildTools --accept-source-agreements --accept-package-agreements --override "--quiet --wait --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
)
if not exist "C:\devkitPro\devkitARM\bin\arm-none-eabi-g++.exe" (
    echo devkitPro ^(the 3DS toolchain^) is needed. Its installer opens next: keep the default folder C:\devkitPro
    echo and tick "3DS Development". Come back here when it has finished.
    powershell -NoProfile -Command "$r = Invoke-RestMethod https://api.github.com/repos/devkitPro/installer/releases/latest; $a = $r.assets | Where-Object { $_.name -like '*.exe' } | Select-Object -First 1; Invoke-WebRequest $a.browser_download_url -OutFile $env:TEMP\devkitProUpdater.exe; Start-Process -Wait $env:TEMP\devkitProUpdater.exe"
    if not exist "C:\devkitPro\devkitARM\bin\arm-none-eabi-g++.exe" (echo devkitPro was not installed. & pause & exit /b 1)
)

set "PATH=%PATH%;C:\Program Files\Git\cmd;C:\Program Files\CMake\bin;%LOCALAPPDATA%\Programs\Python\Python312;C:\Program Files\LLVM\bin"

rem the source: this folder when Setup runs from a clone, otherwise a clone in your user folder
set "REPO=%~dp0..\.."
if not exist "%REPO%\port3ds\build_from_rom.py" (
    set "REPO=%USERPROFILE%\rogue-squadron-3ds"
    if not exist "!REPO!\.git" git clone https://github.com/yaboimanguy-a11y/rogue-squadron-3ds "!REPO!"
)
python "%REPO%\port3ds\build_from_rom.py" --pick
echo.
pause
exit /b

:need
where %1 >nul 2>nul && exit /b 0
if exist %2 exit /b 0
echo %1 is needed.
choice /m "Install it now"
if errorlevel 2 exit /b 1
winget install -e --id %3 --accept-source-agreements --accept-package-agreements
exit /b 0
