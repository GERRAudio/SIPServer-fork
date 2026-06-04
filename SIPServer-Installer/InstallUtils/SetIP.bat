@echo off
echo Setting IP addresses... %1 %2
%3\grep.exe %3\conf\vars.xml  10.8.91.10 %1  2>NUL >NUL
%3\grep.exe %3\conf\vars.xml  10.8.90.10 %2  2>NUL >NUL
del %3\grep.exe
echo Done.