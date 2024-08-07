@echo off
echo Setting IP addresses... %1 %2
%3\grep.exe %3\conf\vars.xml  10.8.91.10 %1
%3\grep.exe %3\conf\vars.xml  10.8.90.10 %2
del %3\grep.exe
echo Done.