:: Makes a timestamped backup of the SIPServer folders
@echo off
:: Attempt to stop and unistall the service or app if present, suppressing errors
ECHO Stopping SIPServer...please do not close this window.
cmd /Q /C "c:\inetpub\SIPserver\SIPServer.exe -stop" 2>NUL >NUL
cmd /Q /C "c:\inetpub\SIPserver\SIPServer.exe -uninstall" 2>NUL >NUL
cmd /Q /C "sc stop SIPServer" 2>NUL >NUL
::cmd /Q /C "sc delete SIPServer" 2>NUL >NUL

FOR /F "skip=1 tokens=1-6" %%A IN ('WMIC Path Win32_LocalTime Get Day^,Hour^,Minute^,Month^,Second^,Year /Format:table') DO (
SET /A sDAY=%%A 2>NUL
SET /A sHOUR=%%B 2>NUL
SET /A sMIN=%%C 2>NUL
SET /A sMON=%%D 2>NUL
SET /A sSEC=%%E 2>NUL
SET /A sYEAR=%%F 2>NUL
)

ECHO Backing up SIPServer folders...please do not close this window
ren "C:\inetpub\SIPServerTemp" "SIPServer_%sYEAR%-%sMON%-%sDAY%_%sHOUR%.%sMIN%.%sSEC%" 2>NUL >NUL

ECHO Done.