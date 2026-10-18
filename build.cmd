@echo off
setlocal

rem Optional first argument selects a single target, e.g. "build.cmd stm_selftest".
set "STM_BUILD_TARGET="
if not "%~1"=="" set "STM_BUILD_TARGET=--target %~1"

call "D:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"

rem --- Windows SDK fallback ---------------------------------------------------
rem vcvars64.bat locates the Windows SDK through reg.exe. In a sandboxed or
rem locked-down shell that call can be blocked, and vcvars still reports success:
rem INCLUDE ends up holding only the VC toolchain paths, and every compile then
rem dies on "cannot open include file: stdio.h" / "d3d11.h" / "Windows.h".
rem Re-derive the SDK paths from the filesystem when vcvars did not supply them.
rem findstr, not find: under Git Bash the POSIX find.exe can shadow the Windows
rem one and this check then always fails, silently forcing the fallback path.
echo %INCLUDE% | findstr /I /C:"Windows Kits" >nul
if not errorlevel 1 goto sdk_ready

set "STM_SDK_ROOT=%ProgramFiles(x86)%\Windows Kits\10"
if not exist "%STM_SDK_ROOT%\Include" set "STM_SDK_ROOT=C:\Program Files (x86)\Windows Kits\10"
if not exist "%STM_SDK_ROOT%\Include" goto sdk_missing

rem Newest installed SDK wins; the folder names are build numbers, so a plain
rem descending name sort is the right ordering.
set "STM_SDK_VER="
for /f "delims=" %%V in ('powershell -NoProfile -Command "(Get-ChildItem -LiteralPath '%STM_SDK_ROOT%\Include' -Directory | Sort-Object Name -Descending | Select-Object -First 1).Name" 2^>nul') do set "STM_SDK_VER=%%V"
if not defined STM_SDK_VER goto sdk_missing

echo [build] Windows SDK not in INCLUDE, using %STM_SDK_ROOT%\Include\%STM_SDK_VER%
set "INCLUDE=%STM_SDK_ROOT%\Include\%STM_SDK_VER%\ucrt;%STM_SDK_ROOT%\Include\%STM_SDK_VER%\um;%STM_SDK_ROOT%\Include\%STM_SDK_VER%\shared;%STM_SDK_ROOT%\Include\%STM_SDK_VER%\winrt;%INCLUDE%"
set "LIB=%STM_SDK_ROOT%\Lib\%STM_SDK_VER%\ucrt\x64;%STM_SDK_ROOT%\Lib\%STM_SDK_VER%\um\x64;%LIB%"
goto sdk_ready

:sdk_missing
echo [build] ERROR: the Windows SDK include path is not on INCLUDE and no SDK was
echo [build]        found under "%STM_SDK_ROOT%". Install the Windows 10/11 SDK
echo [build]        (Visual Studio Installer -^> Modify -^> Individual components).
exit /b 1

:sdk_ready

set "PATH=C:\Users\Shangyang\AppData\Roaming\Python\Python314\Scripts;%PATH%"

cd /d "%~dp0"

cmake -B build -G "Ninja" ^
    -DCMAKE_BUILD_TYPE=Release ^
    -DCMAKE_C_COMPILER=cl ^
    -DCMAKE_CXX_COMPILER=cl ^
    -DCMAKE_TOOLCHAIN_FILE=D:/Projects/vcpkg/scripts/buildsystems/vcpkg.cmake ^
    -DVCPKG_TARGET_TRIPLET=x64-windows-static-md ^
    -DVCPKG_INSTALLED_DIR=D:/Projects/OpenAnimationReplacer/build/vcpkg_installed

if %ERRORLEVEL% neq 0 exit /b %ERRORLEVEL%

cmake --build build --config Release %STM_BUILD_TARGET%
if %ERRORLEVEL% neq 0 exit /b %ERRORLEVEL%

echo Build finished successfully!
