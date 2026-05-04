@echo off
setlocal

set "ROOT=%~dp0.."
set "SRC_DIR=%ROOT%\builder\src"
set "INC_DIR=%ROOT%\builder\include"
set "RC_FILE=%ROOT%\builder\font2c.rc"
set "RES_FILE=%ROOT%\builder\font2c_icon.res"
set "OUT=%ROOT%\font2c.exe"
set "ICON_FILE=%ROOT%\builder\icon_64x64.ico"
set "MSYS2_BIN="

for %%R in (C:\msys64 D:\msys64 %HOMEDRIVE%\msys64) do (
    if not defined MSYS2_BIN if exist "%%~R\mingw64\bin\gcc.exe" set "MSYS2_BIN=%%~R\mingw64\bin"
)

if defined CC (
    set "CC=%CC%"
) else if defined MSYS2_BIN (
    set "CC=%MSYS2_BIN%\gcc.exe"
) else (
    set "CC=gcc"
)

if defined PKG_CONFIG (
    set "PKG_CONFIG=%PKG_CONFIG%"
) else if defined MSYS2_BIN (
    set "PKG_CONFIG=%MSYS2_BIN%\pkg-config.exe"
) else (
    set "PKG_CONFIG=pkg-config"
)

if defined WINDRES (
    set "WINDRES=%WINDRES%"
) else if defined MSYS2_BIN (
    set "WINDRES=%MSYS2_BIN%\windres.exe"
) else (
    set "WINDRES=windres"
)

if exist "%CC%" goto have_cc
where %CC% >nul 2>nul
if errorlevel 1 (
    echo compiler not found: %CC%
    exit /b 1
)
:have_cc

if exist "%PKG_CONFIG%" goto have_pkg_config
where %PKG_CONFIG% >nul 2>nul
if errorlevel 1 (
    echo pkg-config not found: %PKG_CONFIG%
    exit /b 1
)
:have_pkg_config

if exist "%WINDRES%" goto have_windres
where %WINDRES% >nul 2>nul
if errorlevel 1 (
    echo windres not found: %WINDRES%
    exit /b 1
)
:have_windres

for /f "usebackq delims=" %%i in (`"%PKG_CONFIG%" --cflags freetype2`) do set "FT_CFLAGS=%%i"
for /f "usebackq delims=" %%i in (`"%PKG_CONFIG%" --static --libs freetype2`) do set "FT_LIBS=%%i"

if exist "%ICON_FILE%" (
    "%WINDRES%" "%RC_FILE%" -O coff -o "%RES_FILE%"
    if errorlevel 1 exit /b 1
    set "RESOURCE_OBJ=%RES_FILE%"
) else (
    set "RESOURCE_OBJ="
)

"%CC%" -std=c11 -O2 -Wall -Wextra -pedantic -D_CRT_SECURE_NO_WARNINGS -I"%INC_DIR%" %FT_CFLAGS% "%SRC_DIR%\common.c" "%SRC_DIR%\json_config.c" "%SRC_DIR%\font_build.c" "%SRC_DIR%\main.c" %RESOURCE_OBJ% -o "%OUT%" -static -static-libgcc -static-libstdc++ %FT_LIBS% -lstdc++
if errorlevel 1 exit /b 1

echo Built %OUT%
exit /b 0
