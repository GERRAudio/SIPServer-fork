@echo off
powershell -Command "Start-Process cmd -ArgumentList '/c sc start SIPServer' -Verb RunAs"
powershell -Command "Start-Process cmd -ArgumentList '/c sc start VoskASR' -Verb RunAs"
exit
