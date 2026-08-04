@echo off
CHOICE /C YN /M "Are you sure you want to stop the service and potentially disconnect all calls? "
IF %ERRORLEVEL% EQU 1 GOTO :Proceed
IF %ERRORLEVEL% EQU 2 GOTO :ExitScript



:Proceed
echo Shutting down...
powershell -Command "Start-Process cmd -ArgumentList '/c sc Stop SIPServer' -Verb RunAs"
powershell -Command "Start-Process cmd -ArgumentList '/c sc Stop VoskASR' -Verb RunAs"
timeout /t 2 /nobreak > NUL
goto :EOF
:ExitScript
echo You chose to exit without shutdown.
timeout /t 2 /nobreak > NUL
:EOF
exit
