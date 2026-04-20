@echo off
setlocal EnableExtensions EnableDelayedExpansion

rem buildopal.bat
rem Windows counterpart for build/buildopal.sh
rem
rem Usage:
rem   buildopal.bat [INSTALLDIR]
rem
rem Optional environment variables:
rem   VERSION=10
rem   PATCH=7
rem

if "%~1"=="" (
  set "INSTALLDIR=C:\opal-local"
) else (
  set "INSTALLDIR=%~1"
)

set "SVN_CMD="
if not "%SVN_EXE%"=="" if exist "%SVN_EXE%" set "SVN_CMD=%SVN_EXE%"
if "%SVN_CMD%"=="" (
  where svn >nul 2>nul
  if not errorlevel 1 set "SVN_CMD=svn"
)
if "%SVN_CMD%"=="" if defined ProgramFiles if exist "%ProgramFiles%\TortoiseSVN\bin\svn.exe" set "SVN_CMD=%ProgramFiles%\TortoiseSVN\bin\svn.exe"
if "%SVN_CMD%"=="" if defined ProgramW6432 if exist "%ProgramW6432%\TortoiseSVN\bin\svn.exe" set "SVN_CMD=%ProgramW6432%\TortoiseSVN\bin\svn.exe"
if "%SVN_CMD%"=="" if defined LocalAppData if exist "%LocalAppData%\Programs\TortoiseSVN\bin\svn.exe" set "SVN_CMD=%LocalAppData%\Programs\TortoiseSVN\bin\svn.exe"
if "%SVN_CMD%"=="" if exist "C:\Program Files\TortoiseSVN\bin\svn.exe" set "SVN_CMD=C:\Program Files\TortoiseSVN\bin\svn.exe"
if "%SVN_CMD%"=="" if exist "C:\Program Files (x86)\TortoiseSVN\bin\svn.exe" set "SVN_CMD=C:\Program Files (x86)\TortoiseSVN\bin\svn.exe"
if "%SVN_CMD%"=="" if exist "C:\Tools\TortoiseSVN\bin\svn.exe" set "SVN_CMD=C:\Tools\TortoiseSVN\bin\svn.exe"
if "%SVN_CMD%"=="" (
  echo Need SVN installed. Set SVN_EXE or install TortoiseSVN command line tools.
  echo Tried PATH and common SVN locations:
  echo   %%ProgramFiles%%\TortoiseSVN\bin\svn.exe
  echo   C:\Program Files (x86)\TortoiseSVN\bin\svn.exe
  echo   %%ProgramW6432%%\TortoiseSVN\bin\svn.exe
  echo   %%LocalAppData%%\Programs\TortoiseSVN\bin\svn.exe
  echo   C:\Program Files\TortoiseSVN\bin\svn.exe
  echo   C:\Program Files ^(x86^)\TortoiseSVN\bin\svn.exe
  echo   C:\Tools\TortoiseSVN\bin\svn.exe
  exit /b 1
)

echo Using SVN: %SVN_CMD%

where msbuild >nul 2>nul
if errorlevel 1 (
  echo Need MSBuild installed and on PATH.
  exit /b 1
)

set "SCRIPT_DIR=%~dp0"
pushd "%SCRIPT_DIR%.." >nul
set "FS_DIR=%CD%"

set "PKG_CONFIG_PATH=%INSTALLDIR%\lib\pkgconfig"

if "%VERSION%"=="" (
  set "PTLIB_VERSION=trunk"
  set "OPAL_VERSION=trunk"
) else (
  if "%PATCH%"=="" (
    set "PTLIB_VERSION=branches/v2_%VERSION%"
    set "OPAL_VERSION=branches/v3_%VERSION%"
  ) else (
    set "PTLIB_VERSION=tags/v2_%VERSION%_%PATCH%"
    set "OPAL_VERSION=tags/v3_%VERSION%_%PATCH%"
  )
)

echo FS_DIR=%FS_DIR%
echo INSTALLDIR=%INSTALLDIR%
echo PTLIB_VERSION=%PTLIB_VERSION%
echo OPAL_VERSION=%OPAL_VERSION%

if not exist "%FS_DIR%\libs" mkdir "%FS_DIR%\libs"

if not exist "%FS_DIR%\libs\ptlib\.svn" (
  echo Checking out PTLib...
  "%SVN_CMD%" co "https://svn.code.sf.net/p/opalvoip/code/ptlib/%PTLIB_VERSION%" "%FS_DIR%\libs\ptlib"
  if errorlevel 1 goto :fail
) else (
  echo PTLib already present - skipping checkout.
)

if not exist "%FS_DIR%\libs\opal\.svn" (
  echo Checking out OPAL...
  "%SVN_CMD%" co "https://svn.code.sf.net/p/opalvoip/code/opal/%OPAL_VERSION%" "%FS_DIR%\libs\opal"
  if errorlevel 1 goto :fail
) else (
  echo OPAL already present - skipping checkout.
)

rem PTLib build (best-effort, project layout differs by version)
if exist "%FS_DIR%\libs\ptlib\ptlib.sln" (
  echo Building PTLib solution...
  msbuild "%FS_DIR%\libs\ptlib\ptlib.sln" /m /p:Configuration=Release /p:Platform=x64
  if errorlevel 1 goto :fail
) else if exist "%FS_DIR%\libs\ptlib\configure.bat" (
  echo Running PTLib configure.bat...
  call "%FS_DIR%\libs\ptlib\configure.bat"
  if errorlevel 1 goto :fail
) else (
  echo WARNING: Could not find ptlib.sln or configure.bat under libs\ptlib.
)

rem OPAL build (best-effort, project layout differs by version)
if exist "%FS_DIR%\libs\opal\opal.sln" (
  echo Building OPAL solution...
  msbuild "%FS_DIR%\libs\opal\opal.sln" /m /p:Configuration=Release /p:Platform=x64
  if errorlevel 1 goto :fail
) else if exist "%FS_DIR%\libs\opal\configure.bat" (
  echo Running OPAL configure.bat...
  call "%FS_DIR%\libs\opal\configure.bat"
  if errorlevel 1 goto :fail
) else (
  echo WARNING: Could not find opal.sln or configure.bat under libs\opal.
)

rem Build mod_opal in current tree
if exist "%FS_DIR%\src\mod\endpoints\mod_opal\mod_opal.2017.vcxproj" (
  echo Building mod_opal project...
  msbuild "%FS_DIR%\src\mod\endpoints\mod_opal\mod_opal.2017.vcxproj" /m /p:Configuration=Debug /p:Platform=Win32
  if errorlevel 1 goto :fail
) else (
  echo WARNING: mod_opal.2017.vcxproj not found.
)

echo ======================================
echo PTLib/OPAL checkout/build script completed
echo ======================================
popd >nul
exit /b 0

:fail
echo Build failed.
popd >nul
exit /b 1
