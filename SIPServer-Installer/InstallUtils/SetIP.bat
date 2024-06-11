@echo off
echo Setting IP addresses... %1 %2
C:\inetpub\SIPServer\grep.exe C:\inetpub\SIPServer\conf\vars.xml  10.8.91.10 %1
C:\inetpub\SIPServer\grep.exe C:\inetpub\SIPServer\conf\vars.xml  10.8.90.10 %2
del C:\inetpub\SIPServer\grep.exe
echo Done.