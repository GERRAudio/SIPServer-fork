@echo off
sc query SIPServer
echo ========================================================
echo Note: 
echo you can also optionally use the readonly Web interface:
echo 		http://localhost:8080/portal/index.html 
echo 		and login as uid:freeswitch  pwd:works
echo ========================================================
Pause
exit
