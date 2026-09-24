@echo off

echo Finalizing backup... Please do not close this window.
sc failure SIPServer reset=259100 actions=restart/10000
CD %1
FOR /F "skip=1 tokens=1-6" %%A IN ('WMIC Path Win32_LocalTime Get Day^,Hour^,Minute^,Month^,Second^,Year /Format:table') DO (

SET /A sDAY=%%A 2>NUL
SET /A sHOUR=%%B 2>NUL
SET /A sMIN=%%C 2>NUL
SET /A sMON=%%D 2>NUL
SET /A sSEC=%%E 2>NUL
SET /A sYEAR=%%F 2>NUL
)

REN %1\SIPServerTemp SIPServer_%sYEAR%-%sMON%-%sDAY%_%sHOUR%.%sMIN%.%sSEC% 2>NUL >NUL

ECHO Done.