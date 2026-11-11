@echo off
setlocal EnableExtensions EnableDelayedExpansion

if "%~1"=="" (
    echo Error: You must provide a timestamp to rollback.
    echo Usage: Rollback.bat YYYYMMDD_HHMMSS
    pause
    exit /b 1
)
set "TIMESTAMP=%~1"

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
set "MOD_DIR=C:\inetpub\SIPServer\mod"
set "CONF_DIR=C:\inetpub\SIPServer\conf\autoload_configs"
set "DIAL_DIR=C:\inetpub\SIPServer\conf\dialplan"
set "SND_DIR=C:\inetpub\SIPServer\sounds\custom"
set "VOSK_DIR=C:\inetpub\SIPServer\Vosk"

echo ==========================================
echo Starting Rollback for Session: %TIMESTAMP%
echo ==========================================
echo.
set /p DUMMY=">>> Press Enter to STOP services and begin rollback..."

net stop SIPServer 2>nul
net stop VoskASR 2>nul

echo.
echo Rolling back Executables and Modules...
for %%F in (%EXE_FILES%) do call :RollbackFile "%VOSK_DIR%" "%%F"
for %%F in (%DLL_FILES%) do call :RollbackFile "%MOD_DIR%" "%%F"

echo Rolling back Configurations...
for %%F in (vosk.conf.xml) do call :RollbackFile "%CONF_DIR%" "%%F"
for %%F in (00_asr.xml) do call :RollbackFile "%DIAL_DIR%" "%%F"

echo Rolling back Audio Files...
for %%F in (%WAV_FILES%) do call :RollbackFile "%SND_DIR%" "%%F"

echo.
set /p DUMMY=">>> Press Enter to restart VoskASR and SIPServer..."
net start SIPServer
net start VoskASR

echo ==========================================
echo Rollback to %TIMESTAMP% complete.
pause
exit /b 0

:: ==========================================
:: MACRO SUBROUTINE: RollbackFile
:: ==========================================
:RollbackFile
set "DIR=%~1"
set "FILENAME=%~2"
set "CURRENT_FILE=%DIR%\%FILENAME%"
set "BACKUP_FILE=%DIR%\%FILENAME%.%TIMESTAMP%.bak"

if exist "%BACKUP_FILE%" (
    echo Restoring %FILENAME%...
    if exist "%CURRENT_FILE%" del /f /q "%CURRENT_FILE%"
    ren "%BACKUP_FILE%" "%FILENAME%" || echo Error: Failed to restore %FILENAME%
) else (
    echo Warning: Backup "%BACKUP_FILE%" not found. Skipping.
)
exit /b 0