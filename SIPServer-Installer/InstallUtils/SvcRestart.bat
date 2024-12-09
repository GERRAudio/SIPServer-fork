powershell -Command "Start-Process cmd -ArgumentList '/c sc Stop SIPServer' -Verb RunAs"
TIMEOUT /T 5 /NOBREAK
powershell -Command "Start-Process cmd -ArgumentList '/c sc start SIPServer' -Verb RunAs"
echo Restarted!
TIMEOUT /T 3 /NOBREAK