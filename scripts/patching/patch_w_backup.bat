@echo off
setlocal EnableExtensions EnableDelayedExpansion

:: ==========================================
:: Configuration: File Lists
:: ==========================================
set "DLL_FILES=mod_vosk.dll mod_aes67.dll"
set "XML_FILES=vosk.conf.xml 00_asr.xml"
set "WAV_FILES=asr_silence_48k.wav"
set "EXE_FILES=vosk_server.exe"

:: ==========================================
:: Configuration: Directories
:: ==========================================
:: %~dp0 dynamically resolves to the folder where this batch file lives
set "ASSETS_DIR=%~dp0assets"
set "MOD_DIR=C:\inetpub\SIPServer\mod"
set "CONF_DIR=C:\inetpub\SIPServer\conf\autoload_configs"
set "DIAL_DIR=C:\inetpub\SIPServer\conf\dialplan"
set "SND_DIR=C:\inetpub\SIPServer\sounds\custom"
set "VOSK_DIR=C:\inetpub\SIPServer\Vosk"

:: Generate a locale-independent unique Date-Time Stamp (YYYYMMDD_HHMMSS)
for /f %%I in ('powershell -NoProfile -Command "Get-Date -Format 'yyyyMMdd_HHmmss'" 2^>nul') do set "TIMESTAMP=%%I"

:: Fallback to WMIC if PowerShell execution is blocked
if not defined TIMESTAMP (
    for /f "tokens=2 delims==" %%I in ('wmic os get localdatetime /value 2^>nul') do set "dt=%%I"
    if defined dt set "TIMESTAMP=!dt:~0,8!_!dt:~8,6!"
)

if not defined TIMESTAMP (
    echo Error: Failed to acquire system date and time stamp.
    goto :OnError
)

echo ==========================================
echo Backup Session: %TIMESTAMP%
echo ==========================================

:: ==========================================
:: PRE-FLIGHT CHECK
:: ==========================================
echo.
echo Verifying patch assets before stopping services...
if not exist "%ASSETS_DIR%" (
    echo Error: The assets directory was not found at "%ASSETS_DIR%"
    goto :OnError
)

for %%F in (%EXE_FILES% %DLL_FILES% %XML_FILES% %WAV_FILES%) do (
    if not exist "%ASSETS_DIR%\%%F" (
        echo Error: Required patch file "%%F" is missing from the assets folder.
        goto :OnError
    )
)
echo All patch files verified successfully.

:: ==========================================
:: EXECUTE PATCH
:: ==========================================
echo.
echo Cleaning up legacy XML backups...
ren "%CONF_DIR%\*.orig.xml" "*.orig.txt" 2>nul
ren "%CONF_DIR%\*.bak.xml" "*.bak.txt" 2>nul
ren "%DIAL_DIR%\*.orig.xml" "*.orig.txt" 2>nul
ren "%DIAL_DIR%\*.bak.xml" "*.bak.txt" 2>nul

echo.
set /p DUMMY=">>> Press Enter to STOP services and apply patches..."

echo Stopping services...
net stop SIPServer 2>nul
net stop VoskASR 2>nul

:: Ensure no lingering child worker processes hold handle locks on DLLs or EXEs
taskkill /f /im vosk_server.exe 2>nul
timeout /t 2 /nobreak >nul

echo.
echo Patching Executables and Modules...
for %%F in (%EXE_FILES%) do (
    call :PatchFile "%ASSETS_DIR%\%%F" "%VOSK_DIR%"
    if errorlevel 1 goto :OnError
)
for %%F in (%DLL_FILES%) do (
    call :PatchFile "%ASSETS_DIR%\%%F" "%MOD_DIR%"
    if errorlevel 1 goto :OnError
)

echo.
echo Patching Configurations...
call :PatchFile "%ASSETS_DIR%\vosk.conf.xml" "%CONF_DIR%"
if errorlevel 1 goto :OnError

call :PatchFile "%ASSETS_DIR%\00_asr.xml" "%DIAL_DIR%"
if errorlevel 1 goto :OnError

echo.
echo Patching Audio Files...
for %%F in (%WAV_FILES%) do (
    call :PatchFile "%ASSETS_DIR%\%%F" "%SND_DIR%"
    if errorlevel 1 goto :OnError
)

:: ==========================================
:: Restart Services
:: ==========================================
echo.
set /p DUMMY=">>> Press Enter to restart VoskASR and SIPServer..."

echo Starting VoskASR...
net start VoskASR
if errorlevel 1 (
    echo Error: Failed to restart VoskASR service.
    goto :OnError
)

echo Starting SIPServer...
net start SIPServer
if errorlevel 1 (
    echo Error: Failed to restart SIPServer service.
    goto :OnError
)

echo.
echo ==========================================
echo Patch completed successfully.
echo.
echo IMPORTANT: Your backup timestamp for this session is:
echo %TIMESTAMP%
echo.
echo To rollback if needed, run:
echo Rollback.bat %TIMESTAMP%
echo ==========================================
set /p DUMMY=">>> Press Enter to finish..."
exit /b 0


:: ==========================================
:: MACRO SUBROUTINE: PatchFile
:: ==========================================
:PatchFile
set "SRC_FILE=%~1"
set "DEST_DIR=%~2"
set "FILENAME=%~nx1"
set "DEST_PATH=%DEST_DIR%\%FILENAME%"

if not exist "%DEST_DIR%" (
    echo Creating missing directory: "%DEST_DIR%"
    mkdir "%DEST_DIR%" 2>nul
)

if exist "%DEST_PATH%" (
    echo Backing up %FILENAME% -^> %FILENAME%.%TIMESTAMP%.bak
    ren "%DEST_PATH%" "%FILENAME%.%TIMESTAMP%.bak"
    if errorlevel 1 (
        echo Error: Failed to rename "%DEST_PATH%". The file is likely locked by an active process.
        exit /b 1
    )
)

echo Copying %FILENAME% to %DEST_DIR%
copy /Y "%SRC_FILE%" "%DEST_DIR%\" >nul
if errorlevel 1 (
    echo Error: Failed copying "%FILENAME%" to "%DEST_DIR%".
    exit /b 1
)

exit /b 0


:: ==========================================
:: ERROR HANDLER
:: ==========================================
:OnError
echo.
echo ========================================================
echo SCRIPT FAILED: The deployment was interrupted.
echo Please review the error message above.
echo ========================================================
pause
exit /b 1