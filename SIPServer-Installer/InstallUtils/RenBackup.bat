@echo off
echo Installing service... Please do not close this window.
C:\inetpub\SIPServer\SIPServer.exe -install SIPServer 2>NUL >NUL
:: or C:\inetpub\SIPServer>sc create SIPServer binpath= "C:\inetpub\SIPServer\SIPServer.exe"
sc config SIPServer start= demand binpath= "C:\inetpub\SIPServer\SIPServer.exe -install"
sc failure SIPServer reset=259100 actions=restart/10000

FOR /F "skip=1 tokens=1-6" %%A IN ('WMIC Path Win32_LocalTime Get Day^,Hour^,Minute^,Month^,Second^,Year /Format:table') DO (

SET /A sDAY=%%A 2>NUL
SET /A sHOUR=%%B 2>NUL
SET /A sMIN=%%C 2>NUL
SET /A sMON=%%D 2>NUL
SET /A sSEC=%%E 2>NUL
SET /A sYEAR=%%F 2>NUL
)

REN C:\inetpub\SIPServerTemp SIPServer_%sYEAR%-%sMON%-%sDAY%_%sHOUR%.%sMIN%.%sSEC% 2>NUL >NUL


ECHO Done.