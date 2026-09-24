# Deployment instructions

0. Prerequisites
   
	- SIPServer_Installer.msi
 	- Files in this repo, including conf subfolders and files

1. update the following folders from Github/GERRAudio
	- C:\inetpub\SIPserver\conf	(your config for IP telephony)

2. Provided installers will update as well as backup older versions of:
	- C:\inetpub\SipServer	(telephony interface) and install it as a service

3. Verify conf\vars.xml to check that the AES67 NIC name corresponds exactly to the value in the file (multicast IP - AES67)
	- Note that the NIC's on the machine must be set at AES67 (UDP/multicast) and Admin (TCP/IP)

4. Configure the server certificates by updating:
	- C:\SipServer\certs
	- using IIS to provide certs to the Communicator server pages
    - port certs at server level, edit site binding
    - conf05ha01/02  certs correspond to your server IP addresses
	- any client browser should have certs to enable secure connections to the server

5. Check that the Microsoft system dependencies are in place:
	
	- dotNET 4.8 or later
	- ASP.NET 6.0.0 Hosting Bundle or later, which includes Runtime and IIS support 
	
6. Check firewall rules to allow SIPServer. They can be imported from the .wvw file

7. Check C:\SipServer\sounds and recordings to ensure all required files are present

8. Test by
	
	- starting/restarting IIS
	- starting the SIPServer service using the provided desktop or start menu shortcuts
	- Using Miscrosoft Edge to access
	- Communicator at localhost 
	
9. If you require licensing for certain modules, please contact GERR Audio directly for instructions


