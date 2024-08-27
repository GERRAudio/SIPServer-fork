@echo off
powershell -Command "Start-Process cmd -ArgumentList '/c sc Stop SIPServer' -Verb RunAs"
exit
