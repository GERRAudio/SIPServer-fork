# Deployment instructions
0. Prerequisites
   - SIPServer_Installer.msi
   - Bruce_Communicator_Framework_Installer.msi
   - files in this repo, including conf subfolders and files

1. update the following folders from Github/GERRAudio
	- [Installation folder]\Sipserver\conf	(config for IP telephony)
   
2. Provided installers will update as well as backup older version of:
	- [Installation folder]\SipServer	(telephony interface) and install it as a service
  	- C:\inet\wwwroot			(Web server for Communicator and Kiosk)
  
3. Verify vars.xml to check that the AES67 NIC name corresponds exactly to the value in the file (multicast IP - AES67)
	- Note that the NIC's on the machine must be set at AES67 (UDP/multicast) and Bruce.91 (TCP/IP)

4. Configure the certificates by updating:
	- [Installation folder]\certs
	- using IIS to provide certs to the Communicator pages
        	- port certs at server level, edit site binding
        	- conf05ha01/02  certs correspond to .10 and .11
        	- any client browser (i.e. Kiosk or Operator) should have both certs 01 and 02 to talk to either server

6. Check that the Microsoft system dependencies are in place:
	- .NET Extensibility 3.5 and 4.8
	- Application Initialization
	- ASP
	- ASP.NET 3.5 and 4.8
	- ISAPI Extensions
	- ISAPI Filters
	- Server Side Includes
	- WebSocket Protocol
	
7. Check firewall rules to allow SIPServer. They can be imported from the .wvw file
8. Check[Installation folder]\sounds and recordings to ensure all required files are present
9. Test by
	- starting/restarting IIS
	- starting the SIPServer service using the provided desktop or start menu shortcuts
	- Using Miscrosoft Edge to access
		- Communicator at 10.8.91.10 or .11 (depending on where the server is running)
		- Kiosk at 10.8.91.10/Kiosk  or .11/Kiosk


