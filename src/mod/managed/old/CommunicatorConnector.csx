using System;
using System.Net;
using System.Xml;
using FreeSWITCH;
using FreeSWITCH.Native;
using System.Linq;
using System.Collections.Generic;
using System.Timers;
using System.Threading.Tasks;
using System.IO;
using System.Text;


public class CommunicatorConnector : IApiPlugin, IAppPlugin, ILoadNotificationPlugin
{
    public static List<string> ProtectedGUIDs = new List<string>();

    // Options
    static bool debugMode = true;
	
    int dialInConferenceNumberStart = 9000;
    const string apiServer = "http://localhost/api/";
    const string moduleName = "aes67"; // or portaudio
    const string modulePrefix = "aes67"; // or pa


	const bool InTestMode = false;	//do not set true for production, it will overload the call queue
	
	
    // References
    FreeSWITCH.Native.Api fsApi = new FreeSWITCH.Native.Api(null);

    // Timers
    Timer DelayExecutionUntilAfterLoad = new Timer(5000);
    Timer PeriodicCheck = new Timer(10000);


    /// <summary>
    /// Runs through some housekeeping checks every so often
    /// </summary>
    private void PeriodicCheck_Elapsed(object sender, ElapsedEventArgs e)
    {
        CheckChannelListSyncsWithConferenceList();
    }

    /// <summary>
    /// Sets up automatic routes a few seconds after the SIP Server starts
    /// </summary>
    public void StartScheduledTasks(object sender, ElapsedEventArgs e)
    {
        DelayExecutionUntilAfterLoad.Stop();
        Guid id = Guid.Empty;

        try
        {
            id = MakeCall("Announce", "99991").Result;


            if (!id.Equals(Guid.Empty))
            {

                WriteToLog(LogLevel.Info, "Successfully made Announce call - ID is " + id);
            }
        }
        catch (Exception ex)
        {
            WriteToLog(LogLevel.Error, "Failed to add announce call - error was " + ex.Message);
        }


        try
        {
            id = MakeCall("BeltpackMessages", "&endless_playback(theoperatorhasbeencalledbeep48k.wav)").Result;


            if (!id.Equals(Guid.Empty))
            {

                WriteToLog(LogLevel.Info, "Successfully made BeltpackMessages call - ID is " + id);
            }
        }
        catch (Exception ex)
        {
            WriteToLog(LogLevel.Error, "Failed to add beltpack message call - error was " + ex.Message);
        }

    }

    // Storage
    static internal List<string> FoundOrphanedCallsWaitingToResolve = new List<string>();
    static internal Dictionary<string, string> ConferenceRoutes = new Dictionary<string, string>();
    static internal Dictionary<string, Guid> EavesdropIDs = new Dictionary<string, Guid>();
    static internal List<Guid> StoredAllCalls = new List<Guid>();
    static int MaxWirelessConf = 0;
    static int MaxWiredConf = 0;

    Guid EavesdropOnAnnounce = Guid.Empty;

    internal async Task<Guid> MakeCall(string portAudioInterface, string destination)
    {
        return MakeCall(portAudioInterface, destination, 0).Result;
    }

    internal async Task<Guid> MakeCall(string portAudioInterface, string destination, int attempts)
    {
        if (attempts > 5)
        {
            WriteToLog(LogLevel.Error, string.Format("Failed to connect {0} to {1} on attempt {2}. Giving up.", portAudioInterface, destination, attempts));

            throw new Exception("Unable to open a " + moduleName + " stream");
        }
        var newID = fsApi.ExecuteString("create_uuid");
        WriteToLog(LogLevel.Info, "Trying to use " + newID + " as an ID");
        string allCallAttempt1Result = fsApi.Execute("bgapi", "originate {origination_uuid=" + newID + "}" + moduleName + "/endpoints/" + portAudioInterface + " " + destination);

        WriteToLog(LogLevel.Info, "Attempt to make call: " + allCallAttempt1Result);
        Guid uuid = Guid.Empty;
        int count = 0;

        while (true)
        {
            WriteToLog(LogLevel.Info, "Waiting a second for background processing before attempt " + (1 + count) + "...");
            await Task.Delay(2000);

            if (count > 10)
            {
                return MakeCall(portAudioInterface, destination, ++attempts).Result;
            }
            string result = fsApi.ExecuteString("eval uuid:" + newID + " ${channel-state}");

            switch (result.ToLower().Trim())
            {
                case "":
                    return MakeCall(portAudioInterface, destination, ++attempts).Result;
                    break;

                case "cs_execute":

                    WriteToLog(LogLevel.Info, "Successfully added " + portAudioInterface + " to " + destination + " on attempt number " + (1 + count) + ".");

                    if (Guid.TryParse(newID, out uuid))
                    {
                        WriteToLog(LogLevel.Info, "ID is " + uuid.ToString());
                        return uuid;
                    }
                    else
                    {
                        return MakeCall(portAudioInterface, destination, ++attempts).Result;
                    }
                    break;
                case "cs_routing":
                    break;
                default:
                    WriteToLog(LogLevel.Error, string.Format("Failed to connect {0} to {1} on attempt {2}. Result was : {3}.", portAudioInterface, destination, 1 + count, result));
                    ++count;
                    break;
            }
        }
    }

    internal string RefreshVarValues()
    {
        MaxWirelessConf = int.Parse(fsApi.ExecuteString("eval ${MaxWirelessConf}"));
        MaxWiredConf = int.Parse(fsApi.ExecuteString("eval ${MaxWiredConf}"));


        var result = string.Format("Refreshed values. Have MaxWirelessConf:{0}, MaxWiredConf:{1}.", MaxWirelessConf, MaxWiredConf);
        WriteToLog(LogLevel.Info, result);

        return result;
    }


    internal void PutEavesdropOnHold(string conference)
    {
        WriteToLog(LogLevel.Info, "Putting " + conference + " on hold");
        conference = conference.Trim();
        var xmlConference = GetSingleConferenceXml(conference);

        foreach (XmlElement node in xmlConference.ChildNodes[1].ChildNodes)
        {

            if (node.HasAttribute("name"))
            {
                var conferenceNameNew = node.Attributes["name"].Value;

                // WriteToLog(LogLevel.Info, "Have found conference " + conferenceNameNew + " and checking against " + conference);

                if (conferenceNameNew.Trim() == conference.Trim())
                {
                    WriteToLog(LogLevel.Info, "Matching " + conferenceNameNew);


                    var memberCount = node.Attributes["member-count"].Value;

                    int members = int.Parse(memberCount.ToString());
                    // WriteToLog(LogLevel.Info, "Conference " + conference + " has " + members + " members " + node.OuterXml);

                    XmlNode membersNode = GetMembersNode(node);
                    foreach (XmlNode childNode in membersNode.ChildNodes)
                    {
                        var uuid = "";
                        var port = "";

                        foreach (XmlNode attribute in childNode.ChildNodes)
                        {
                            switch (attribute.Name)
                            {
                                case "uuid":
                                    uuid = attribute.InnerText;
                                    break;

                                case "caller_id_number":
                                    WriteToLog(LogLevel.Notice, "PORT  " + attribute.InnerText);
                                    port = attribute.InnerText;
                                    break;

                            }

                        }

                        // WriteToLog(LogLevel.Info, "Have found Port " + port);


                        if ((port.Trim().ToLower() == "announce") || (port.Trim().ToLower() == "99997"))
                        {
                            Guid guid = Guid.Empty;
                            if (Guid.TryParse(uuid, out guid))
                            {
                                if (EavesdropIDs.ContainsKey(conference))
                                {
                                    EavesdropIDs[conference] = guid;
                                }
                                else
                                {
                                    EavesdropIDs.Add(conference, guid);
                                }

                                // WriteToLog(LogLevel.Warning, EavesdropIDs.Count.ToString());
                                var holdcall = fsApi.ExecuteString("uuid_hold " + uuid);
                                WriteToLog(LogLevel.Info, "Eavesdrop for " + conference + " put on hold - result was " + holdcall);
                            }
                        }


                    }
                }
            }
        }
    }

    internal void TakeEavesdropOffHold(string conference)
    {
        WriteToLog(LogLevel.Info, "In TakeEavesdropOffHold with " + conference + " and ID count " + EavesdropIDs.Count);

        conference = conference.Trim();

        if (EavesdropIDs.ContainsKey(conference.Trim()))
        {
            Guid uuid = EavesdropIDs[conference];
            var unholdcall = fsApi.ExecuteString("uuid_hold off " + uuid);
            WriteToLog(LogLevel.Info, "Eavesdrop for " + conference + " taken off hold - result was " + unholdcall);
        }
    }

    internal string GetPortAudioUse()
    {
        var endpoints = fsApi.ExecuteString(modulePrefix + " endpoints");
        //WriteToLog(LogLevel.Notice, endpoints);
        var allEndpoints = ProcessAllEndpoints(endpoints);

        var channels = fsApi.ExecuteString("show channels as xml");

        var remainingEndpoints = ProcessUsedEndpoints(channels, allEndpoints, -9, false);

        WriteToLog(LogLevel.Warning, " In GetPortAudioUse with remainingEndpoints = " + remainingEndpoints.Count());
        return String.Join("|", remainingEndpoints);
    }


    internal string FindConferencesWithPortAudio(string portAudioInterface)
    {
		
		List<string> foundConferences = new List<string>();
		
		WriteToLog(LogLevel.Warning, "In Find Conferences " + portAudioInterface);
        var endpoints = fsApi.ExecuteString(modulePrefix + " endpoints");
        //WriteToLog(LogLevel.Notice, endpoints);
        var allEndpoints = ProcessAllEndpoints(endpoints);

        var channels = fsApi.ExecuteString("show channels as xml");

        var remainingEndpoints = ProcessUsedEndpoints(channels, allEndpoints, -9, true);

        WriteToLog(LogLevel.Info, "Conference to PortAudio size :" + ConferenceToPortAudio.Count);

		foreach(var conf in PortAudioToConference)
		{
			WriteToLog(LogLevel.Critical, "PortAudioToConference => " + conf.Key + " and we're looking for " + portAudioInterface); 
			
			if(conf.Key == portAudioInterface){
					string returnString = String.Join("|", conf.Value);
		
		WriteToLog(LogLevel.Critical, "FindConferencesWithPortAudio returns " + returnString);
        return returnString;
			}
		}

	WriteToLog(LogLevel.Critical, "FindConferencesWithPortAudio returns nothing.");
	return "";
    }

    internal string RefreshPortAudioAllCall()
    {
        var endpoints = fsApi.ExecuteString(modulePrefix + " endpoints");
        //WriteToLog(LogLevel.Notice, endpoints);
        var allEndpoints = ProcessAllEndpoints(endpoints);

        var channels = fsApi.ExecuteString("show channels as xml");

        var remainingEndpoints = ProcessUsedEndpoints(channels, allEndpoints, -9, false);

        WriteToLog(LogLevel.Notice, "Chosing Phone - line 328");

        var chosenPhone = remainingEndpoints.Last();

        if (chosenPhone.StartsWith("Phone_"))
        {
            string tempUUIDString = fsApi.ExecuteString("create_UUID");

            string tempResult = fsApi.Execute("bgapi", "originate {origination_uuid=" + tempUUIDString + "}" + moduleName + "/endpoints/" + chosenPhone + " 99996");

            WriteToLog(LogLevel.Info, "Temporary call result = " + tempResult);

            foreach (var uuid in StoredAllCalls)
            {
                KillActiveCall(uuid.ToString(), "Killing previous all call to refresh it.");
            }
            StoredAllCalls.Clear();

            Guid id = Guid.Empty;

            id = MakeCall("Announce", "99996").Result;


            if (!id.Equals(Guid.Empty))
            {

                WriteToLog(LogLevel.Info, "Successfully made Announce call - ID is " + id);
                StoredAllCalls.Add(id);

            }
            KillActiveCall(tempUUIDString, "Temporary All Call backup no longer needed");
        }

        return chosenPhone;

    }
	
	internal string AddToPartyline(string conferenceName)
	{
	
	if(conferenceName.Trim().Length == 4)
	{
		int lastTwoDigitsOfConference = -1;
        int.TryParse(conferenceName.Substring(2, 2), out lastTwoDigitsOfConference);
		
		if(lastTwoDigitsOfConference >= 25 && lastTwoDigitsOfConference <=99)
		{
			return "No need to change AES67 conference";
		}
	}
	 // need to lose AES67 Announce	

	 
	 foreach(var uuid in AnnounceIds(conferenceName)){
	 							WriteToLog(LogLevel.Info, "Removing Announce from " + conferenceName);
								KillActiveCall(uuid, "Temporary All Call backup no longer needed");
	 }
	 return "done";
	}


internal List<string> AnnounceIds(string conferenceName)
{
	 var conferenceXml = GetConferenceXml(conferenceName);
List<string> returnIDs = new List<string>();
foreach(XmlNode conferenceNode in conferenceXml.ChildNodes[1]){
		 if(conferenceNode.NodeType == XmlNodeType.Element){
			 var conferenceFound = conferenceNode.Attributes["name"].Value;
			 if(conferenceFound == conferenceName)
			 {
					foreach(XmlNode subnode in conferenceNode.ChildNodes)
					{
						if(subnode.NodeType == XmlNodeType.Element)
						{
							if(subnode.Name == "members")
							{
								
							foreach(XmlNode memberNode in subnode.ChildNodes)
							{
								var uuid = "";
								var callerID = "";
								
								foreach(XmlNode memberDetail in memberNode.ChildNodes)
								{
									switch(memberDetail.Name)
									{
										case "uuid":
										uuid = memberDetail.InnerText;
										break;
										case "caller_id_number":
										callerID = memberDetail.InnerText;
										break;
									}
								}
								
								if(callerID == "Announce")
								{
									returnIDs.Add(uuid);
	
								}
							}								
							}
						}
					}
				break;
			 }
		 }
	 }


return returnIDs;
}

    internal string RemoveFromPartyline(string conferenceName, string interfaceName)
    {
        string apiUrl = String.Format(apiServer + "conferencedata/{0}/{1}/{2}/sipserverhangup/", "UNUSED", interfaceName, "OFF");

        WriteToLog(LogLevel.Warning, apiUrl);

        var response = "FAILURE";
		
		try{

        using (WebClient client = new WebClient())
        {
            response = client.DownloadString(apiUrl);

            WriteToLog(LogLevel.Warning, response);


            HangUpChannel(interfaceName);


            var cancellationTokenSource = new System.Threading.CancellationTokenSource();
            var cancellationToken = cancellationTokenSource.Token;

            Task.Delay(500).ContinueWith(async (t) =>
            {
                response = client.DownloadString(apiUrl + "?force=" + conferenceName);
                response = client.DownloadString(apiUrl + "?force=" + conferenceName);
                WriteToLog(LogLevel.Warning, "Second attempt: " + apiUrl + " = " + response);
            }, cancellationToken);


        }
		}
		
		catch(Exception ex){}
		
			if(conferenceName.Trim().Length == 4)
		{
		int lastTwoDigitsOfConference = -1;
        int.TryParse(conferenceName.Substring(2, 2), out lastTwoDigitsOfConference);
		
		if((lastTwoDigitsOfConference >= 25) && (lastTwoDigitsOfConference <=99))
		{
			return response;
		}
		}
		AddToPartyline(conferenceName);
		fsApi.ExecuteString("conference " + conferenceName + " dial aes67/endpoints/Announce");
        return response;
    }

    internal bool InterfaceIsAlreadyInUse(string interfaceName)
    {

        WriteToLog(LogLevel.Warning, "In InterfaceIsAlreadyInUse with " + interfaceName);
        interfaceName = interfaceName.Trim();
        if (interfaceName == "ALREADY-IN-USE")
        {
            return true;
        }
        var channels = fsApi.ExecuteString("show channels as xml");
        XmlDocument channelsXml = new XmlDocument();
        channelsXml.LoadXml(channels);


        foreach (XmlNode rowNode in channelsXml.ChildNodes[0].ChildNodes)
        {

            foreach (XmlNode childNode in rowNode)
            {
                switch (childNode.Name)
                {

                    case "name":
                        string endpointName2 = childNode.InnerText.Replace(moduleName + "/endpoint-", "").Trim();
                        WriteToLog(LogLevel.Info, "Checking " + endpointName2 + " against " + interfaceName);
                        if (endpointName2.Equals(interfaceName))
                        {
                            return true;
                        }
                        break;

                }

            }
        }
        return false;
    }

    internal async Task<string> ConferenceChecker(string symbol, string conferenceNumberString, string source)
    {
		//System.Threading.Thread.Sleep(5000);
		
        WriteToLog(LogLevel.Info, string.Format("In Conference Checker - Symbol {0}, Conference Number {1}", symbol, conferenceNumberString));

        bool needToGetAnInterface = false;
        bool needToPlaceIntoConference = false;
        bool wiredConferenceNeedingAllCall = false;
        bool fromDialplan = false;
        bool fromWebGUI = false;
        string chosenInterface = "No interface";

        int conferenceFullNumber = -1;
        int.TryParse(conferenceNumberString, out conferenceFullNumber);


        int firstTwoDigitsOfConference = -1;
        int.TryParse(conferenceNumberString.Substring(0, 2), out firstTwoDigitsOfConference);

        int lastTwoDigitsOfConference = -1;
        int.TryParse(conferenceNumberString.Substring(2, 2), out lastTwoDigitsOfConference);
        int conferenceReference = lastTwoDigitsOfConference - MaxWirelessConf;
		
		string conferenceName = "CONF." + (lastTwoDigitsOfConference - (MaxWirelessConf - 1));


        try
        {
			
			var xmlDocument = GetSingleConferenceXml(conferenceNumberString);
											
            switch(symbol)
            {


                case "+":

                    // mapping conference numbers to Eclipse internal numbers
                    WriteToLog(LogLevel.Info, "Conference Reference = " + conferenceReference);
                    if (conferenceReference < 0)
                    {
                        //    return "Conference reference was not useful. Ignoring";
                    }
                    WriteToLog(LogLevel.Info, "MaxWirelessConf = " + MaxWirelessConf);

                    WriteToLog(LogLevel.Info, "Last two digits = " + lastTwoDigitsOfConference);
                    WriteToLog(LogLevel.Info, "Conference = " + conferenceName);

                    WriteToLog(LogLevel.Info, (MaxWirelessConf == -1).ToString());
                    WriteToLog(LogLevel.Info, (lastTwoDigitsOfConference < MaxWirelessConf).ToString());
                    // WriteToLog(LogLevel.Notice, (lastTwoDigitsOfConference > MaxWiredConf).ToString());

                    if ((MaxWirelessConf == -1) || (lastTwoDigitsOfConference < MaxWirelessConf))
                    {
                        if (source == "FROM_DIALPLAN")
                        {
                            WriteToLog(LogLevel.Info, "Not a wireless conference.");
                            fromDialplan = true;
							
							if(xmlDocument == null)
							{
																fsApi.ExecuteString("conference " + conferenceNumberString + " dial aes67/endpoints/Announce");
							} else{
							
							bool skip = true;
							 foreach(var uuid in AnnounceIds(conferenceNumberString))
							 {
								if(skip)
								{
									 skip = false;
								} else 
								{
									WriteToLog(LogLevel.Info, "Removing Announce from " + conferenceName);
									KillActiveCall(uuid, "Temporary All Call backup no longer needed");
								}
							 }
							 
							 if(skip){
								fsApi.ExecuteString("conference " + conferenceNumberString + " dial aes67/endpoints/Announce");
							 }
							}
                        }
                    }
                    else
                    {

                        // Wireless conferences that need to be auto assigned
                        WriteToLog(LogLevel.Info, "Request from Dial Plan is within the preassigned range - need to assign an interface");
                        needToPlaceIntoConference = true;
                        needToGetAnInterface = true;

                    }

                    if (source.ToUpper() == "FROM-INTERCOM")
                    {
                        WriteToLog(LogLevel.Info, "Request from Web GUI - need to use an interface.");
                        needToGetAnInterface = true;
                        fromWebGUI = true;
                    }


                    int conferenceNumber = -1;

                    if (needToGetAnInterface)
                    {
                        WriteToLog(LogLevel.Notice, "Need to add a Dante/AES67 route");
                        if (int.TryParse(conferenceNumberString, out conferenceNumber))
                        {

                            WriteToLog(LogLevel.Notice, "SIP Conference Number: " + conferenceNumber);
                            string getEndpoints = modulePrefix + " endpoints";
                            var endpoints = fsApi.ExecuteString(getEndpoints);
                            WriteToLog(LogLevel.Notice, "Getting Endpoints with '" + getEndpoints + "', which returned " + endpoints);
                            var allEndpoints = ProcessAllEndpoints(endpoints);

                            var channels = fsApi.ExecuteString("show channels as xml");

                            WriteToLog(LogLevel.Warning, "Sending to ProcessUsedEndpoints Channels " + channels.Count());
                            WriteToLog(LogLevel.Warning, "Sending to ProcessUsedEndpoints allEndpoints " + allEndpoints.Count());
                            WriteToLog(LogLevel.Warning, "Sending to ProcessUsedEndpoints conferenceNumber " + conferenceNumber);


                            var remainingEndpoints = ProcessUsedEndpoints(channels, allEndpoints, conferenceNumber, false);



                            WriteToLog(LogLevel.Warning, "Picking Chosen Interface from " + remainingEndpoints.Count());
                            chosenInterface = remainingEndpoints.First();
                            WriteToLog(LogLevel.Notice, "Chosen Index is " + chosenInterface);

                            bool newDantePort = true;

                            if (chosenInterface == "ALREADY-IN-USE")
                            {
                                WriteToLog(LogLevel.Notice, "Already in use, returning: " + remainingEndpoints[1]);
                                newDantePort = false;
                                if (source.ToUpper() != "FROM_DIALPLAN")
                                {
                                    return remainingEndpoints[1];
                                }
                            }


                            Random random = new Random();

                            int index = random.Next(remainingEndpoints.Count);

                            WriteToLog(LogLevel.Notice, index + " port =" + remainingEndpoints[index]);

                            chosenInterface = remainingEndpoints[index];

                            WriteToLog(LogLevel.Notice, "Chosen Endpoint: " + chosenInterface + " and we're coming from the dialplan so we should make our port " + remainingEndpoints[1]);

                            // chosenInterface = remainingEndpoints[1];

                            string conferenceCommand = "TalkAndListen";
                            if (wiredConferenceNeedingAllCall)
                            {
                                conferenceCommand = "Listen";
                            }

                            if (!InterfaceIsAlreadyInUse(chosenInterface))
                            {
                                Guid callGUID = Guid.NewGuid();

                                string originateString = "originate {interface=" + chosenInterface + ",origination_uuid=" + callGUID.ToString() + "}" + moduleName + "/endpoints/" + chosenInterface + " 99999" + conferenceNumber.ToString();
                                WriteToLog(LogLevel.Warning, originateString);

                                ProtectedGUIDs.Add(callGUID.ToString());
                                WriteToLog(LogLevel.Notice, "Protecting " + callGUID.ToString());


                                var joinConference1 = fsApi.Execute("bgapi", originateString);

                                WriteToLog(LogLevel.Notice, "Job ID " + joinConference1);

                                bool needToWait = true;
                                int counter = 1;

                                while ((needToWait == true) && counter < 10)
                                {
                                    WriteToLog(LogLevel.Info, "Waiting a second for background processing before attempt " + counter + "...");

                                    string result = fsApi.ExecuteString("eval uuid:" + callGUID.ToString() + " ${channel-state}");

                                    WriteToLog(LogLevel.Info, "STATE " + result);


                                    switch (result.ToLower().Trim())
                                    {
                                        case "":

                                            break;

                                        case "cs_exchange_media":
                                            needToWait = false;
                                            ProtectedGUIDs.Remove(callGUID.ToString());
                                            WriteToLog(LogLevel.Info, "Successfully exchanging media for " + moduleName + " " + chosenInterface + " to " + conferenceNumberString);

                                            break;

                                        case "cs_execute":

                                            ProtectedGUIDs.Remove(callGUID.ToString());
                                            WriteToLog(LogLevel.Info, "Successfully executed " + moduleName + " " + chosenInterface + " to " + conferenceNumberString);

                                            needToWait = false;
                                            break;

                                        case "cs_routing":
                                            break;
                                        default:

                                            break;
                                    }


                                    if (needToWait)
                                    {
                                        counter++;
                                        await Task.Delay(2000);
                                    }

                                }
                                // await Task.Delay(500);


                                if ((!needToPlaceIntoConference) && (!wiredConferenceNeedingAllCall))
                                {
                                    WriteToLog(LogLevel.Notice, "No need to place this call automatically into a conference... removing protection from " + callGUID.ToString());
                                    RemoveProtection(callGUID.ToString());
                                }
                                else
                                {

                                    if (!fromWebGUI)
                                    {

                                        string apiUrl = String.Format(apiServer + "conferencedata/{0}/{1}/{2}/sipserverreuse", conferenceName, chosenInterface, conferenceCommand);


                                        if (newDantePort)
                                        {
                                            apiUrl = String.Format(apiServer + "conferencedata/{0}/{1}/{2}/sipservernewchannel", conferenceName, chosenInterface, conferenceCommand);
                                        }

                                        using (WebClient client = new WebClient())
                                        {
                                            WriteToLog(LogLevel.Warning, apiUrl);

                                            var response = client.DownloadString(apiUrl);
                                            WriteToLog(LogLevel.Warning, response);
                                        }
                                    }
                                }


                                if (!newDantePort)
                                {
                                    using (WebClient levelclient = new WebClient())
                                    {

                                        var apiUrlLevel = String.Format(apiServer + "portlevel/{0}/{1}/", conferenceNumber, chosenInterface);
                                        WriteToLog(LogLevel.Warning, apiUrlLevel);
                                        var levelResponse = levelclient.DownloadString(apiUrlLevel);

                                        WriteToLog(LogLevel.Info, levelResponse);
                                    }
                                }


                                foreach (XmlNode node in xmlDocument.ChildNodes[1].ChildNodes)
                                {


                                    var conferenceNameNew = node.Attributes["name"].Value;


                                    if (conferenceNameNew == conferenceNumberString)
                                    {


                                        var memberCount = node.Attributes["member-count"].Value;

                                        int members = int.Parse(memberCount.ToString());
                                        WriteToLog(LogLevel.Notice, "Conference " + conferenceNumberString + " has " + members + " members");


                                        XmlNode membersNode = GetMembersNode(node);
                                        foreach (XmlNode childNode in membersNode.ChildNodes)
                                        {
                                            var uuid = "";
                                            var port = "";

                                            foreach (XmlNode attribute in childNode.ChildNodes)
                                            {
                                                switch (attribute.Name)
                                                {
                                                    case "uuid":
                                                        uuid = attribute.InnerText;
                                                        break;

                                                    case "caller_id_number":
                                                        WriteToLog(LogLevel.Notice, "PORT  " + attribute.InnerText);
                                                        port = attribute.InnerText;
                                                        break;

                                                }


                                            }

                                            WriteToLog(LogLevel.Notice, port);

                                        }
                                    }

                                }


                                if (fromWebGUI)
                                {
                                    WriteToLog(LogLevel.Warning, "Telling the web GUI that we have " + chosenInterface);
                                }

                                RemoveProtection(callGUID.ToString());

                                return chosenInterface;

                            }
                        }
                    } 	
                    break;

                case "-":

                    WriteToLog(LogLevel.Notice, "Checking to see if we are done with the Dante route");
WriteToLog(LogLevel.Notice, xmlDocument.OuterXml);
                    foreach (XmlNode node in xmlDocument.ChildNodes[1].ChildNodes)
                    {
                        List<string> DanteIDs = new List<string>();
                        List<string> DantePorts = new List<string>();

                        var conferenceNameNew = node.Attributes["name"].Value;

                        if (conferenceNameNew == conferenceNumberString)
                        {

                            WriteToLog(LogLevel.Notice, "Focussed on the one conference now");
                            var memberCount = node.Attributes["member-count"].Value;

                            int members = int.Parse(memberCount.ToString());
                            WriteToLog(LogLevel.Notice, "Conference " + conferenceNumberString + " has " + members + " members");

                            XmlNode membersNode = GetMembersNode(node);
                            foreach (XmlNode childNode in membersNode.ChildNodes)
                            {
                                var uuid = "";
                                var port = "";

                                foreach (XmlNode attribute in childNode.ChildNodes)
                                {
                                    switch (attribute.Name)
                                    {
                                        case "uuid":
                                            uuid = attribute.InnerText;
                                            break;

                                        case "caller_id_number":
                                            port = attribute.InnerText;
                                            WriteToLog(LogLevel.Notice, "Conference " + conferenceNumberString + " has " + members + " members - one is " + port);
                                            break;

                                    }


                                }

                                // if (port.ToLower().Contains("99997"))
                                // {
                                //     members--;
                                // }

                                if (port.ToLower().Contains("phone_"))
                                {
                                    members--;
                                    DanteIDs.Add(uuid);
                                    DantePorts.Add(port);
                                }

                            }


                            if (members == 0)
                            {
                                foreach (var id in DanteIDs)
                                {
                                    KillActiveCall(id, "Conference call has been hung up, we are freeing up matrix ports.");
                                }




                                WriteToLog(LogLevel.Warning, "About to remove Conference " + conferenceNumberString + " has " + members + " members");

                                foreach (var danteInterface in DantePorts)
                                {
                                    string conferenceCommand = "Off";

                                    string apiUrl = String.Format(apiServer + "conferencedata/{0}/{1}/{2}", conferenceName, danteInterface, conferenceCommand);

                                    WriteToLog(LogLevel.Warning, apiUrl);
                                    using (WebClient client = new WebClient())
                                    {



                                        var response = client.DownloadString(apiUrl);

                                        WriteToLog(LogLevel.Warning, response);

                                    }

                                    return "Added";
                                }
                            }

                        }
                    }
                    break;
            }
        }
        catch (Exception ex)
        {
            WriteToLog(LogLevel.Warning, ex.Message);
        }

        return chosenInterface;
    }

    async Task RemoveProtection(string protectedID)
    {
        WriteToLog(LogLevel.Warning, "Removing protection from " + protectedID);
        await Task.Delay(1000);
        ProtectedGUIDs.Remove(protectedID);
        WriteToLog(LogLevel.Warning, "Currently " + ProtectedGUIDs.Count + " protected GUIDs left");
    }

    private string GetCurrentPINs()
    {
        var response = "FAILURE";

        using (WebClient client = new WebClient())
        {
            response = client.DownloadString(apiServer + "/pin");
        }
        WriteToLog(LogLevel.Warning, response);
        return response;
    }



    /// <summary>
    /// Removes everything associated with a conference
    /// </summary>
    /// <param name="conferenceNumber"></param>
    /// <returns></returns>
    private string HangUpConference(string conferenceNumber)
    {
        if ((conferenceNumber == "0000") || (conferenceNumber == "Announce"))
        {
            WriteToLog(LogLevel.Info, "Not hanging up the announce conference");
            return "Protected Conference";
        }
        WriteToLog(LogLevel.Info, "In HangUpConference with  " + conferenceNumber);

        var xmlDocument = GetSingleConferenceXml(conferenceNumber.Trim());

        foreach (XmlNode currentNode in xmlDocument.ChildNodes[1].ChildNodes) // conference
        {
            string uuid = currentNode.Attributes["uuid"].Value;
            string name = currentNode.Attributes["name"].Value;

            if (conferenceNumber.Trim() == name.Trim())
            {

                string memberUUID = "";
                string memberName = "";
                string memberNumber = "";

                XmlNode membersNode = GetMembersNode(currentNode);


                foreach (XmlNode xmlNode in membersNode.ChildNodes)
                {
                    foreach (XmlNode memberNode in xmlNode.ChildNodes)
                    {
                        switch (memberNode.Name)
                        {
                            case "uuid":
                                memberUUID = memberNode.InnerText;
                                break;

                            case "direction":
                                break;

                            case "created":
                                break;

                            case "name":
                                memberName = memberNode.InnerText;
                                break;

                            case "caller_id_name":
                                memberName = memberNode.InnerText;

                                break;

                            case "caller_id_number":
                                memberNumber = memberNode.InnerText.Replace("%40", "@");
                                break;
                        }
                    }

                    if (memberName.Contains("Operator"))
                    {
                        OperatorLeave(memberNumber);
                    }
                    else
                    {
                        if (!memberName.Contains("Announce"))
                        {
                            KillActiveCall(memberUUID, "Hanging up conference " + conferenceNumber + " and taking down all related channels.");
                        }
                    }

                }

            }
        }
        return "FAILURE";
    }


    /// <summary>
    /// Joins the call to a conference
    /// </summary>
    /// <param name="callerID"></param>
    /// <param name="conference"></param>
    /// <returns></returns>
    public string OperatorJoin(string callerID, string conference)
    {
        string uuid = GetIDFromCaller(callerID);

        WriteToLog(LogLevel.Info, "In OperatorJoin, " + callerID + " resolves to " + uuid + " connecting to " + conference);

        string apiResponse = fsApi.ExecuteString("uuid_transfer " + uuid + " " + conference);
        WriteToLog(LogLevel.Info, apiResponse);
        return apiResponse;

    }

    /// <summary>
    /// Transfers the caller to the operator only
    /// </summary>
    /// <param name="callerID"></param>
    /// <returns></returns>
    public string OperatorLeave(string callerID)
    {
        string uuid = GetIDFromCaller(callerID);

        string operatorNumber = callerID.Substring(3, 1);
        WriteToLog(LogLevel.Info, "In OperatorLeave, " + callerID + " resolves to " + operatorNumber);

        string apiResponse = fsApi.ExecuteString("uuid_transfer " + uuid + " " + operatorNumber);
        WriteToLog(LogLevel.Info, apiResponse);
        return apiResponse;

    }




    /// <summary>
    /// Seems unused. Kills calls an operator has
    /// </summary>
    /// <param name="arguments"></param>
    public string CheckOperator(string operatorNumber, string currentUUID)
    {
        try
        {

            WriteToLog(LogLevel.Info, "In CheckOperator, checking status of " + operatorNumber);

            var showCallsXML = fsApi.ExecuteString("show calls as XML");

            KillActiveOperatorCalls(operatorNumber, showCallsXML, currentUUID);
        }
        catch (Exception ex)
        {
            WriteToLog(LogLevel.Error, ex.Message);
        }

        return "CheckOperator complete.";

    }

    /// <summary>
    /// Used by HangupPorts to make sure ports are hung up.
    /// </summary>
    /// <param name="arguments"></param>
    /// <returns></returns>
    public string HangUpChannel(string channelString)
    {

        try
        {
            WriteToLog(LogLevel.Info, "In HangUpChannel, removing Dante route we are done with");
            var channels = fsApi.ExecuteString("show channels as xml");

            XmlDocument xmlDocument = new XmlDocument();
            xmlDocument.LoadXml(channels);


            foreach (XmlNode node in xmlDocument.ChildNodes[0].ChildNodes)
            {

                var uuid = "";
                var port = "";


                foreach (XmlNode attribute in node.ChildNodes)
                {
                    switch (attribute.Name)
                    {
                        case "uuid":
                            uuid = attribute.InnerText;
                            WriteToLog(LogLevel.Notice, uuid);
                            break;

                        case "cid_num":
                            port = attribute.InnerText;
                            WriteToLog(LogLevel.Notice, port);
                            break;

                    }


                }

                WriteToLog(LogLevel.Notice, "In HangUpChannel, checking channels for " + channelString);
                if (port.Trim().ToLower() == channelString.Trim().ToLower())
                {
                    KillActiveCall(uuid, "Channel list tells us there are no more matrix connections to conference that used this port.");
                }
            }

        }
        catch (Exception ex)
        {
            WriteToLog(LogLevel.Error, ex.Message);
        }


        return "HangUpChannel complete.";
    }

    public void Run(FreeSWITCH.AppContext context)
    {

        WriteToLog(LogLevel.Info, "CommunicatorConnector Running in AppPlugin process");

        string returnString = Process(context.Arguments.Split(' '));
        //context.Write(returnString);

    }

    public void Execute(ApiContext context)
    {

        WriteToLog(LogLevel.Info, string.Format("CommunicatorConnector executed with args '{0}' and event type {1}.", context.Arguments, context.Event == null ? "<none>" : context.Event.GetEventType()));

        string returnString = Process(context.Arguments.Split(' '));
        context.Stream.Write(returnString);

    }

    public void ExecuteBackground(ApiBackgroundContext context)
    {
        WriteToLog(LogLevel.Info, string.Format("CommunicatorConnector on a background thread #({0}), with args '{1}'.", System.Threading.Thread.CurrentThread.ManagedThreadId, context.Arguments));

        string returnString = Process(context.Arguments.Split(' '));
        // context.Stream.Write(returnString);
    }

    /// <summary>
    /// Takes incoming arguments and routes them to the right method
    /// </summary>
    /// <param name="arguments"></param>
    public string Process(string[] arguments)
    {
        string functionRequired = arguments[0].ToLower();
        string returnString = "No result received.";

        WriteToLog(LogLevel.Debug, "Processing " + functionRequired);

        string thirdParameter = "";
        if (arguments.Count() >= 4)
        {
            thirdParameter = arguments[3];
        }


        switch (functionRequired)
        {
            case "portaudioavailable":
                returnString = GetPortAudioUse();
                break;

            case "portaudiouse":
                returnString = FindConferencesWithPortAudio(arguments[1]);
                break;

            case "swapfromeavesdroptodante":
                PutEavesdropOnHold(arguments[1]);
                break;

            case "swapfromdantetoeavesdrop":
                TakeEavesdropOffHold(arguments[1]);
                break;
			case "adddantetocall":
				AddToPartyline(arguments[1]);
				break;

            case "removedantefromcall":
                RemoveFromPartyline(arguments[1], arguments[2]);
                break;

            case "seteavesdropid":
                Guid newEavesdropID = Guid.Empty;
                if (Guid.TryParse(arguments[1], out newEavesdropID))
                {
                    EavesdropOnAnnounce = newEavesdropID;
                    WriteToLog(LogLevel.Info, "New ID to eavesdrop on: " + EavesdropOnAnnounce);
                }
                break;
            case "conferencechecker":
                returnString = ConferenceChecker(arguments[1], arguments[2], thirdParameter).Result;
                break;
            case "getnextconference":
                returnString = GetNextConferenceNumber();
                break;
            case "hangupconference":
				if(!InTestMode){	//optionally suppress cleanup/hangup
					returnString = HangUpConference(arguments[1]);
				}
                break;
            case "checkoperator":
            case "operatorchecker":
                returnString = CheckOperator(arguments[1], arguments[2]);
                break;
            case "operatorjoin":
                returnString = OperatorJoin(arguments[1], arguments[2]);
                break;
            case "operatorleave":
                returnString = OperatorLeave(arguments[1]);
                break;

            case "hangupchannel":
                returnString = HangUpChannel(arguments[1]);
                break;
            case "hangupportaudio":
                returnString = HangUpChannel(arguments[1]);
                break;
            case "hangupcall":
				if(!InTestMode){	//optionally suppress cleanup/hangup
					returnString = KillActiveCall(arguments[1], "Web UI wanted to hang up because: " + arguments[2]);
				}
                break;

            case "refresh":
                returnString = RefreshVarValues();
                break;

            case "reloadallcall":
                returnString = RefreshPortAudioAllCall();
                break;

            case "conferencelist":
                var returnXml = GetConferencesXml();
                if (returnXml != null)
                {
                    returnString = returnXml.OuterXml;
                }
                else
                {
                    returnString = "Xml was null";
                }
                break;

            case "conference":
                var returnIndividualXml = GetConferenceXml(arguments[1]);
                if (returnIndividualXml != null)
                {
                    returnString = returnIndividualXml.OuterXml;
                }
                else
                {
                    returnString = "Xml was null";
                }
                break;

            case "getpins":
                return GetCurrentPINs().Replace("\"", "");
                break;

            case "setdebug":
                bool onOff = false;
                if (bool.TryParse(arguments[1], out onOff))
                {
                    debugMode = onOff;
                    returnString = "Debug mode now " + onOff;
                }
                break;
            default:
                WriteToLog(LogLevel.Error, "Unable to work out what was meant by " + arguments[0]);
                break;
        }

        return returnString;
    }

    public bool Load()
    {
        WriteToLog(LogLevel.Info, "Loading CommunicatorConnector.");
        RefreshVarValues();
        var firstdigit = fsApi.ExecuteString("eval ${FirstNumber}");
        WriteToLog(LogLevel.Info, "Valid first conference numbers are: " + firstdigit);
        DelayExecutionUntilAfterLoad.Elapsed += this.StartScheduledTasks;
        DelayExecutionUntilAfterLoad.Start();
        PeriodicCheck.Elapsed += this.PeriodicCheck_Elapsed;
        PeriodicCheck.Start();
        return true;
    }

    public static void Main()
    {
        switch (FreeSWITCH.Script.ContextType)
        {
            case ScriptContextType.Api:
                {
                    var ctx = FreeSWITCH.Script.GetApiContext();
                    // ctx.Stream.Write("Script executing as API with args: " + ctx.Arguments);
                    break;
                }
            case ScriptContextType.ApiBackground:
                {
                    var ctx = FreeSWITCH.Script.GetApiBackgroundContext();

                    // WriteToLog(LogLevel.Debug, "Executing as APIBackground with args: " + ctx.Arguments);
                    break;
                }
            case ScriptContextType.App:
                {
                    var ctx = FreeSWITCH.Script.GetAppContext();
                    // WriteToLog(LogLevel.Debug, "Executing as App with args: " + ctx.Arguments);
                    break;
                }
        }
    }


    public static void WriteToLog(LogLevel logLevel, string logEntry)
    {
        switch (logLevel)
        {
            /*
             * 0 - CONSOLE
             * 1 - ALERT
             * 2 - CRIT
             * 3 - ERR
             * 4 - WARNING
             * 5 - NOTICE
             * 6 - INFO
             * 7 - DEBUG
             */

            case LogLevel.Debug:
                if (debugMode)
                {
                    Log.WriteLine(logLevel, logEntry);
                }
                break;

            default:
                Log.WriteLine(logLevel, logEntry);
                break;
        }

    }



    private string ConferenceNeedsAnnounce(string conferenceNumberString)
    {
        return "Not being used";

        var xmlDocument = GetConferenceXml(conferenceNumberString.Trim());
        foreach (XmlNode node in xmlDocument.ChildNodes[1].ChildNodes)
        {


            var conferenceNameNew = node.Attributes["name"].Value;


            if (conferenceNameNew == conferenceNumberString)
            {


                var memberCount = node.Attributes["member-count"].Value;

                int members = int.Parse(memberCount.ToString());
                WriteToLog(LogLevel.Notice, "Conference " + conferenceNumberString + " has " + members + " members");


                XmlNode membersNode = GetMembersNode(node);
                foreach (XmlNode childNode in membersNode.ChildNodes)
                {
                    var uuid = "";
                    var port = "";

                    foreach (XmlNode attribute in childNode.ChildNodes)
                    {
                        switch (attribute.Name)
                        {
                            case "uuid":
                                uuid = attribute.InnerText;
                                break;

                            case "caller_id_number":
                                WriteToLog(LogLevel.Notice, "PORT  " + attribute.InnerText);
                                port = attribute.InnerText;
                                break;

                        }


                    }

                    WriteToLog(LogLevel.Notice, port);

                    //  if (port == "99997" || port == "Announce")
                    //  {
                    //      WriteToLog(LogLevel.Notice, "Removing all call eavesdrop");
                    //      // var killResult = fsApi.ExecuteString("uuid_kill " + uuid);
                    //  }




                }
            }

        }
        return "true";

    }











    private string GetIDFromCaller(string callerID)
    {
        WriteToLog(LogLevel.Debug, "In GetIDFromCaller, looking up " + callerID);
        if (callerID == "")
        {
            return "NO CALLER ID";
        }
        string apiResponse = fsApi.ExecuteString("show channels as xml");

        List<string> allChannels = new List<string>();

        XmlDocument xmlDocument = new XmlDocument();
        xmlDocument.LoadXml(apiResponse);

        foreach (XmlNode currentNode in xmlDocument.ChildNodes[0].ChildNodes) // conference
        {

            string uuid = "";
            string name = "";
            string callerIDName = "";
            string callerIDNum = "";

            foreach (XmlNode xmlNode in currentNode.ChildNodes)
            {
                switch (xmlNode.Name)
                {
                    case "uuid":
                        uuid = xmlNode.InnerText;
                        break;

                    case "direction":
                        break;

                    case "created":
                        break;

                    case "name":
                        name = xmlNode.InnerText;
                        break;

                    case "cid_name":
                        callerIDName = xmlNode.InnerText;
                        break;

                    case "cid_num":
                        callerIDNum = xmlNode.InnerText;
                        break;
                }

            }

            if (callerID == callerIDNum)
            {
                WriteToLog(LogLevel.Debug, string.Format("Found caller UUID {0} for caller ID {1}.", uuid, callerID));
                return uuid;
            }
        }

        WriteToLog(LogLevel.Debug, "Unable to find a UUID for " + callerID);
        return "NotFound";
    }

    internal void KillActiveOperatorCalls(string operatorNumber, string showCallsXML, string currentUUID)
    {
        WriteToLog(LogLevel.Debug, "Finding and hanging up operator calls except the active one.");
        XmlDocument activeCalls = new XmlDocument();
        activeCalls.LoadXml(showCallsXML);

        foreach (XmlNode xmlNode in activeCalls.FirstChild.ChildNodes)
        {
            string uuid = "";
            bool killUUID = false;


            foreach (XmlNode childNode in xmlNode.ChildNodes)
            {
                if (childNode.Name == "uuid")
                {
                    if (childNode.InnerText != currentUUID)
                    {
                        uuid = childNode.InnerText;
                    }
                }

                if (childNode.Name == "dest")
                {
                    if (childNode.InnerText == operatorNumber)
                    {
                        killUUID = true;
                    }
                }


            }

            if ((uuid != "") && killUUID)
            {
                KillActiveCall(uuid, "Cleaning up multiple Operator calls so there can be just one.");
            }
        }
    }

    private List<string> GetAllConferences()
    {

        List<string> allConferences = new List<string>();

        WriteToLog(LogLevel.Debug, "Getting all conferences as a list");
        var xmlDocument = GetConferencesXml();

        foreach (XmlNode currentNode in xmlDocument.ChildNodes[1].ChildNodes) // conference
        {
            allConferences.Add(currentNode.Attributes["name"].Value);
        }

        return allConferences;
    }

    public string GetNextConferenceNumber()
    {
        List<string> conferences = GetAllConferences();
        WriteToLog(LogLevel.Debug, "In GetConference with " + conferences.Count + " conferences.");

        bool haveFound = false;

        while (!haveFound)
        {
            if (!conferences.Contains(dialInConferenceNumberStart.ToString()))
            {
                haveFound = true;
                return dialInConferenceNumberStart.ToString();

            }
            dialInConferenceNumberStart++;
        }

        return "9876";
    }


    private void CheckChannelListSyncsWithConferenceList()
    {
        try
        {
            WriteToLog(LogLevel.Debug, "Checking Channel list against Conference list.");
            XmlDocument channels = new XmlDocument();

            var channelString = fsApi.ExecuteString("show channels as xml");

            channels.LoadXml(channelString);
            var conference = GetConferencesXml();
            var channelList = CheckChannels(channels);

            var newOrphans = CheckConferences(conference, channelList);

            foreach (var oldOrphan in FoundOrphanedCallsWaitingToResolve)
            {
                foreach (var newOrphan in newOrphans)
                {
                    if (oldOrphan == newOrphan)
                    {
                        HangUpOrphanedChannel(oldOrphan, channels);
                        break;
                    }
                }
            }

            FoundOrphanedCallsWaitingToResolve = newOrphans;
        }
        catch (Exception ex)
        {
            WriteToLog(LogLevel.Error, ex.Message);
        }
    }

    private void HangUpOrphanedChannel(string oldOrphan, XmlDocument channels)
    {
        try
        {
            WriteToLog(LogLevel.Warning, "Going to kill " + oldOrphan);


            foreach (XmlNode row in channels.FirstChild.ChildNodes)
            {

                string phone = "";
                string uuid = "";
                bool isConference = false;

                if (row.NodeType == XmlNodeType.Element)
                {
                    foreach (XmlNode rowDetail in row.ChildNodes)
                    {
                        if (rowDetail.NodeType == XmlNodeType.Element)
                        {
                            switch (rowDetail.Name)
                            {
                                case "name":

                                    if (rowDetail.InnerText.StartsWith(moduleName + "/endpoint-Phone_"))
                                    {
                                        phone = rowDetail.InnerText.Replace(moduleName + "/endpoint-", "");

                                    }
                                    break;

                                case "application":
                                    if (rowDetail.InnerText == "conference")
                                    {
                                        isConference = true;
                                    }
                                    break;

                                case "call_uuid":
                                    uuid = rowDetail.InnerText.Trim();
                                    break;
                            }
                        }

                    }


                }
                if (isConference)
                {
                    if (phone == oldOrphan)
                    {
                        if (uuid.Length > 0)
                        {
                            KillActiveCall(uuid, "Channel has been unassociated with any conference for too long. Going to free it up.");
                        }
                    }
                }
            }

        }
        catch (Exception ex)
        {
            WriteToLog(LogLevel.Error, ex.Message);
        }
    }

    private List<string> CheckConferences(XmlDocument conference, Dictionary<string, List<string>> channelList)
    {

        List<string> orphans = new List<string>();

        try
        {

            foreach (XmlNode conferenceNode in conference.ChildNodes[1].ChildNodes)
            {
                string conferenceName = "";
                var stopProcessing = false;

                bool hasallcall = false;
                if (conferenceNode.NodeType == XmlNodeType.Element)
                {
                    foreach (XmlAttribute currentAttribute in conferenceNode.Attributes)
                    {
                        if (currentAttribute.Name == "name")
                        {
                            conferenceName = currentAttribute.Value;

                            if (!channelList.ContainsKey(conferenceName))
                            {
                                XmlNode membersNode = GetMembersNode(conferenceNode);
                                foreach (XmlNode conferenceMember in membersNode.ChildNodes)
                                {
                                    foreach (XmlNode conferenceDetail in conferenceMember.ChildNodes)
                                    {
                                        if (conferenceDetail.NodeType == XmlNodeType.Element)
                                        {
                                            switch (conferenceDetail.Name)
                                            {
                                                case "caller_id_number":

                                                    /*
                                                        //	 WriteToLog(LogLevel.Alert, "CALLER ID " + conferenceDetail.InnerText);
                                                        if (conferenceDetail.InnerText.StartsWith("99997") || conferenceDetail.InnerText.ToLower().Trim() == "announce")
                                                        {
                                                            hasallcall = true;
                                                            WriteToLog(LogLevel.Debug, "Conference Sync Conference " + conferenceName + " has all call already");

                                                        }

                                                        */
                                                    break;
                                            }
                                        }
                                    }
                                }


                                if (!hasallcall)
                                {
                                    //fsApi.Execute("bgapi","originate loopback/99997 &conference(" + conferenceName + "++flags{deaf})");
                                }
                                stopProcessing = true;
                            }
                            else
                            {
                                XmlNode childNode = GetMembersNode(conferenceNode);

                                foreach (XmlNode conferenceMember in childNode.ChildNodes)
                                {
                                    foreach (XmlNode conferenceDetail in conferenceMember.ChildNodes)
                                    {
                                        if (conferenceDetail.NodeType == XmlNodeType.Element)
                                        {
                                            switch (conferenceDetail.Name)
                                            {
                                                case "caller_id_number":
                                                    if (conferenceDetail.InnerText.StartsWith("Phone_"))
                                                    {
                                                        string phone = conferenceDetail.InnerText.Trim();
                                                        if (!channelList[conferenceName].Contains(phone))
                                                        {
                                                            WriteToLog(LogLevel.Alert, "MISMATCH Conference has phone that channel list doesn't -  Conference " + conferenceName + " and Phone " + phone);
                                                        }
                                                        else
                                                        {
                                                            channelList[conferenceName].Remove(phone);
                                                        }
                                                    }
                                                    break;
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }

                    if (stopProcessing)
                    {
                        continue;
                    }


                }
            }


            foreach (var channel in channelList.Keys)
            {
                if (channelList[channel].Count > 0)
                {
                    foreach (string phone in channelList[channel])
                    {
                        WriteToLog(LogLevel.Warning, "Orphaned channel - conference " + channel + ", phone " + phone);

                        if (!orphans.Contains(phone))
                        {
                            orphans.Add(phone);
                        }
                    }
                }
            }


        }
        catch (Exception ex)
        {
            WriteToLog(LogLevel.Error, ex.Message);
        }

        return orphans;
    }

    private Dictionary<string, List<string>> CheckChannels(XmlDocument channels)
    {
        Dictionary<string, List<string>> ActiveConferences = new Dictionary<string, List<string>>();

        try
        {
            foreach (XmlNode row in channels.FirstChild.ChildNodes)
            {
                if (row.NodeType == XmlNodeType.Element)
                {
                    bool isConference = false;
                    string conferenceNumber = "";
                    string phone = "";


                    foreach (XmlNode rowDetail in row.ChildNodes)
                    {


                        if (rowDetail.NodeType == XmlNodeType.Element)
                        {
                            switch (rowDetail.Name)
                            {
                                case "name":

                                    if (rowDetail.InnerText.StartsWith(moduleName + "/endpoint-Phone_"))
                                    {
                                        phone = rowDetail.InnerText.Replace(moduleName + "/endpoint-", "");
                                    }
                                    break;

                                case "application":
                                    if (rowDetail.InnerText == "conference")
                                    {
                                        isConference = true;
                                    }
                                    break;



                                case "application_data":
                                    conferenceNumber = rowDetail.InnerText.Trim();

                                    break;
                            }
                        }

                    }

                    if (isConference)
                    {
                        if (phone.Length > 4)
                        {
                            int testNumber = -1;

                            int.TryParse(conferenceNumber, out testNumber);

                            if (testNumber > 0)
                            {
                                if (!ActiveConferences.ContainsKey(conferenceNumber))
                                {
                                    ActiveConferences.Add(conferenceNumber, new List<string>() { phone });
                                }
                                else
                                {
                                    ActiveConferences[conferenceNumber].Add(phone);
                                }
                            }
                        }
                    }
                }

            }
        }
        catch (Exception ex)
        {
            WriteToLog(LogLevel.Error, ex.Message);
        }

        return ActiveConferences;
    }

    internal static string lastAllConferencesString = "";
    internal static XmlDocument lastAllConferencesXML = null;


    internal XmlDocument GetSingleConferenceXml(string conferenceName)
    {

	WriteToLog(LogLevel.Critical, "In Get Single Conference looking for " + conferenceName);


        string apiResponse = "No response getting Single Conference.";
        try
        {

            apiResponse = fsApi.ExecuteString("conference " + conferenceName + " xml_list");
            WriteToLog(LogLevel.Warning, "conference " + conferenceName + " xml_list");
			WriteToLog(LogLevel.Warning, apiResponse);
            XmlDocument xmlDocument = new XmlDocument();
            MemoryStream stream = new MemoryStream();
            byte[] data = Encoding.UTF8.GetBytes(apiResponse);
            stream.Write(data, 0, data.Length);
            stream.Seek(0, SeekOrigin.Begin);
            XmlTextReader reader = new XmlTextReader(stream);
            xmlDocument.Load(reader);

            // WriteToLog(LogLevel.Warning, "XML is good");
            return xmlDocument;

        }
        catch (Exception ex)
        {
            WriteToLog(LogLevel.Error, "Failed to understand: " + apiResponse);
        }
        return null;
    }

    internal XmlDocument GetConferencesXml()
    {

        string apiResponse = "No response.";

        try
        {
            apiResponse = fsApi.ExecuteString("conference xml_list");

            //  WriteToLog(LogLevel.Warning, apiResponse);

            if (!apiResponse.Equals(lastAllConferencesString))
            {
                //  WriteToLog(LogLevel.Warning, "API is new");
                XmlDocument xmlDocument = new XmlDocument();
                MemoryStream stream = new MemoryStream();
                byte[] data = Encoding.UTF8.GetBytes(apiResponse);
                stream.Write(data, 0, data.Length);
                stream.Seek(0, SeekOrigin.Begin);
                XmlTextReader reader = new XmlTextReader(stream);
                xmlDocument.Load(reader);

                // WriteToLog(LogLevel.Warning, "XML is good");


                lastAllConferencesString = apiResponse;
                lastAllConferencesXML = xmlDocument;

                return xmlDocument;
            }
            else
            {
                return lastAllConferencesXML;
            }
        }
        catch (Exception ex)
        {
            WriteToLog(LogLevel.Error, "Failed to understand: " + apiResponse);
        }
        return null;
    }

    internal XmlDocument GetConferenceXml(string conferenceID)
    {

        string apiResponse = "No response.";

        try
        {
            apiResponse = fsApi.ExecuteString(string.Format("conference {0} xml_list", conferenceID.Trim()));

            XmlDocument xmlDocument = new XmlDocument();
            MemoryStream stream = new MemoryStream();
            byte[] data = Encoding.UTF8.GetBytes(apiResponse);
            stream.Write(data, 0, data.Length);
            stream.Seek(0, SeekOrigin.Begin);
            XmlTextReader reader = new XmlTextReader(stream);
            xmlDocument.Load(reader);

            // WriteToLog(LogLevel.Warning, "XML is good");


            lastAllConferencesString = apiResponse;
            lastAllConferencesXML = xmlDocument;

            return xmlDocument;
            //   }
            //   else
            //   {
            //       return lastAllConferencesXML;
            //   }
        }
        catch (Exception ex)
        {
            WriteToLog(LogLevel.Error, "Failed to understand: " + apiResponse);
        }
        return null;
    }

    private static List<string> ProcessAllEndpoints(string endpoints)
    {
        //WriteToLog(LogLevel.Warning, "ProcessAllEndpoints => endpoints: " + endpoints);

        List<string> allEndpoints = new List<string>();

        var endpointArray = endpoints.Split('\n');

        WriteToLog(LogLevel.Warning, "ProcessAllEndpoints => endpoints length " + endpointArray.Length);


        if (moduleName == "portaudio")
        {
            for (int x = 0; x < endpointArray.Length - 1; x++)
            {
                if (endpointArray[x].Contains(">"))
                {
                    string endpointName = endpointArray[x].Substring(0, endpointArray[x].IndexOf(">"));
                    if (!allEndpoints.Contains(endpointName))
                    {
                        WriteToLog(LogLevel.Warning, "Adding PA " + endpointName);
                        allEndpoints.Add(endpointName);
                    }
                }
            }
        }
        else if (moduleName == "aes67")
        {
            for (int x = 0; x < endpointArray.Length - 1; x++)
            {
                string currentName = endpointArray[x].Trim();
                while (currentName.IndexOf("  ") != -1)
                {
                    currentName = currentName.Replace("  ", " ");
                }
                var endpointFindName = currentName.Split(' ');

                if (endpointFindName.Length > 2)
                {
                    string endpointName = endpointFindName[2];

                    if (!allEndpoints.Contains(endpointName))
                    {
                        //WriteToLog(LogLevel.Warning, "Adding AES " + endpointName);
                        allEndpoints.Add(endpointName);
                    }
                }
            }
        }
        WriteToLog(LogLevel.Warning, "Returning All Endpoints " + allEndpoints.Count);
        return allEndpoints;
    }


    private static Dictionary<string, List<string>> ConferenceToPortAudio = new Dictionary<string, List<string>>();
    private static Dictionary<string, List<string>> PortAudioToConference = new Dictionary<string, List<string>>();

    private static List<string> ProcessUsedEndpoints(string endpoints, List<string> allEndpoints, int conferenceNumber, bool ignoreFound)
    {
        try
        {
            ConferenceToPortAudio.Clear();
            PortAudioToConference.Clear();

            WriteToLog(LogLevel.Notice, "CONFERENCE " + conferenceNumber);
            //      WriteToLog(LogLevel.Notice, "ENDPOINTS " + endpoints);


            XmlDocument channelsXml = new XmlDocument();
            channelsXml.LoadXml(endpoints);
            var endpointList = new List<string>(allEndpoints);

            WriteToLog(LogLevel.Notice, "ProcessUsedEndpoints, 1928");
            foreach (var currentEndpoint in allEndpoints)
            {
                if (!currentEndpoint.Contains("Phone"))
                {
                    endpointList.Remove(currentEndpoint);
                }
            }

            foreach (XmlNode rowNode in channelsXml.ChildNodes[0].ChildNodes)
            {
                string currentEndpointName = "";
                string application = "";

                foreach (XmlNode childNode in rowNode)
                {
                    switch (childNode.Name)
                    {
                        case "dest":

                            string endpointName1 = childNode.InnerText.Replace("endpoints/", "");
                            if (endpointName1.Contains("Phone_"))
                            {
                                endpointList.Remove(endpointName1);
                                currentEndpointName = endpointName1;
                            }
                            break;

                        case "name":
                            string endpointName2 = childNode.InnerText.Replace(moduleName + "/endpoint-", "");
                            if (endpointName2.Contains("Phone_"))
                            {
                                endpointList.Remove(endpointName2);
                                currentEndpointName = endpointName2;
                                WriteToLog(LogLevel.Notice, "Removing " + endpointName2);
                            }
                            break;

                        case "application":
                            application = childNode.InnerText;


                            break;

                        case "application_data":
                            string conf_data = childNode.InnerText;



                            if (currentEndpointName.Contains("Phone_"))
                            {

                                if (conf_data.StartsWith(conferenceNumber.ToString()) || (conf_data.EndsWith(conferenceNumber.ToString())))
                                {
                                    WriteToLog(LogLevel.Notice, "Found searched-for conference and Dante is " + currentEndpointName);



                                    if (currentEndpointName != conferenceNumber.ToString())
                                    {
                                        if (!ignoreFound)
                                        {
                                            List<string> returnList = new List<string>();
                                            return new List<string> { "ALREADY-IN-USE", currentEndpointName };
                                        }
                                    }
                                }

                                try
                                {

                                    string numericConference = new String(conf_data.Where(Char.IsDigit).ToArray());

                                    WriteToLog(LogLevel.Notice, "Found a conference " + numericConference + " as application " + application + " where Dante is " + currentEndpointName);


                                    if (application.Trim().ToLower() == "transfer")
                                    {
                                        numericConference = numericConference.Replace("99999", "");
                                    }
                                    if (!ConferenceToPortAudio.ContainsKey(numericConference))
                                    {
										 WriteToLog(LogLevel.Notice, "Adding conference " + numericConference + " to lookup");
                                        ConferenceToPortAudio.Add(numericConference, new List<string>());
                                    }

                                    if (!ConferenceToPortAudio[numericConference].Contains(currentEndpointName))
                                    {
										WriteToLog(LogLevel.Notice, "Adding endpoint "  + currentEndpointName + " to " + numericConference);
                                        ConferenceToPortAudio[numericConference].Add(currentEndpointName);
                                    }

                                    if (!PortAudioToConference.ContainsKey(currentEndpointName))
                                    {
                                        PortAudioToConference.Add(currentEndpointName, new List<string>());
                                    }

                                    if (!PortAudioToConference[currentEndpointName].Contains(numericConference))
                                    {
                                        PortAudioToConference[currentEndpointName].Add(numericConference);
                                    }
                                }
                                catch (Exception exception2)
                                {
                                    WriteToLog(LogLevel.Notice, "Updating Conference to PortAudio list - exception " + exception2);
                                }

                            }
                            break;
                    }

                }
            }

            endpointList.Sort();

            WriteToLog(LogLevel.Notice, "Returning endpoint list of " + endpointList.Count);

            return endpointList;
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine(ex.Message);
        }

        return null;
    }

    internal string KillActiveCall(string callUUID, string reason)
    {
        WriteToLog(LogLevel.Notice, string.Format("In KillActiveCall - Hanging up active call {0} because {1}. Protected count {2}.", callUUID, reason, ProtectedGUIDs.Count));

        if (ProtectedGUIDs.Contains(callUUID))
        {
            WriteToLog(LogLevel.Notice, "Not killing " + callUUID + " as it was protected");
            return "Not killing " + callUUID + " as it was protected.";

        }
        else
        {

            string result = fsApi.ExecuteString("eval uuid:" + callUUID + " ${channel-state}");
            WriteToLog(LogLevel.Notice, "STATE OF GUID TO BE KILLED = " + result);

            result = fsApi.ExecuteString("uuid_dump " + callUUID);
            WriteToLog(LogLevel.Notice, "STATE OF GUID TO BE KILLED = " + result);


            string killString = "uuid_kill " + callUUID;
            var apiResult = fsApi.ExecuteString(killString);
            WriteToLog(LogLevel.Notice, "The result of killing ID" + callUUID + " was " + apiResult);
            return "The result of killing ID" + callUUID + " was " + apiResult;
        }



    }

    internal XmlNode GetMembersNode(XmlNode parentNode)
    {
        XmlNode membersNode = parentNode.FirstChild;

        if (membersNode.Name.ToLower().Trim() != "members")
        {
            foreach (XmlNode testNode in parentNode.ChildNodes)
            {
                if (testNode.Name.ToLower().Trim() == "members")
                {
                    return testNode;
                }
            }
        }

        return membersNode;
    }
}