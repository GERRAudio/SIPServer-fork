:: copy over the config instead of doing a full install

copy /y C:\Development\GitHub\GERRAudio\SIPServer\conf\Bruce\conf\autoload_configs\aes67.conf.xml   C:\inetpub\SIPServer\conf\autoload_configs\ 
copy /y C:\Development\GitHub\GERRAudio\SIPServer\conf\Bruce\conf\autoload_configs\conference.conf.xml  C:\inetpub\SIPServer\conf\autoload_configs\
copy /y C:\Development\GitHub\GERRAudio\SIPServer\conf\Bruce\conf\dialplan\default.xml  C:\inetpub\SIPServer\conf\dialplan\
copy /y C:\Development\GitHub\GERRAudio\SIPServer\src\mod\managed\ASRSideCar.csx  C:\inetpub\SIPServer\mod\managed\
copy /y C:\Development\GitHub\GERRAudio\SIPServer\x64\Release\sounds\custom\*.wav  C:\inetpub\SIPServer\sounds\custom\
copy /y C:\inetpub\wwwroot\Configuration\Bruce-pool.hxn   C:\inetpub\wwwroot\Configuration\Bruce.hxn
del /F /Q C:\inetpub\SIPServer\asr-queue\*.*
pause