// Version Date: 2026-11-19
//
// Compiled at load by mod_managed with the .NET Framework C# compiler (CodeDom "v4.0"): C# 5 syntax only
// (no $"" strings, ?. , => members, tuples or nameof).
//
// Threading: FreeSWITCH runs every API/dialplan call on its own thread and the timers on pool threads, so all
// static collections below are only touched under StateLock. Never hold StateLock across fsApi calls, HTTP
// requests or awaits.

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

    static bool debugMode = false;		//do not leave on for prod
    const bool InTestMode = false;

    int dialInConferenceNumberStart = 9000;
    const string apiServer = "http://localhost/api/";
    const string moduleName = "aes67"; 
    const string modulePrefix = "aes67";

    FreeSWITCH.Native.Api fsApi = new FreeSWITCH.Native.Api(null);

    Timer DelayExecutionUntilAfterLoad = new Timer(5000);
    Timer PeriodicCheck = new Timer(10000);

    static internal List<string> FoundOrphanedCallsWaitingToResolve = new List<string>();
    static internal Dictionary<string, Guid> EavesdropIDs = new Dictionary<string, Guid>();
    static internal List<Guid> StoredAllCalls = new List<Guid>();
    
    static int MaxWirelessConf = 0;
    static int MaxWiredConf = 0;
    Guid EavesdropOnAnnounce = Guid.Empty;

    internal static string lastAllConferencesString = "";
    internal static XmlDocument lastAllConferencesXML = null;

    private static Dictionary<string, List<string>> ConferenceToPortAudio = new Dictionary<string, List<string>>();
    private static Dictionary<string, List<string>> PortAudioToConference = new Dictionary<string, List<string>>();

    // Guards every static collection above (see header).
    private static readonly object StateLock = new object();

    // Phone_ ports handed out by ConferenceChecker whose channel may not be in "show channels" yet: two calls
    // arriving together could otherwise both pick the same free port.
    private static readonly HashSet<string> ReservedPorts = new HashSet<string>();
    private static readonly Random PortRandom = new Random();

    // 1 while a PeriodicCheck tick is running: an overlapping tick is skipped (same 10 s cadence, no overlap).
    private int periodicCheckRunning = 0;

    public bool Load()
    {
        WriteToLog(LogLevel.Info, "Loading CommunicatorConnector.");
        RefreshVarValues();
        
        var firstdigit = fsApi.ExecuteString("eval ${FirstNumber}");
        WriteToLog(LogLevel.Info, "Valid first conference numbers are: " + firstdigit);
        
        CleanOutPhoneConferences();
        WriteToLog(LogLevel.Info, "Cleaned out phone conferences");
        
        DelayExecutionUntilAfterLoad.Elapsed += this.StartScheduledTasks;
        DelayExecutionUntilAfterLoad.Start();
        
        PeriodicCheck.Elapsed += this.PeriodicCheck_Elapsed;
        PeriodicCheck.Start();
        
        return true;
    }

    private void PeriodicCheck_Elapsed(object sender, ElapsedEventArgs e)
    {
        // System.Timers.Timer fires on schedule even if the previous tick is still running; skip instead of overlapping
        if (System.Threading.Interlocked.CompareExchange(ref periodicCheckRunning, 1, 0) != 0) return;
        try
        {
            CheckChannelListSyncsWithConferenceList();
            EnsureBeltpackMessagesAlive(); //the beep generator
        }
        finally
        {
            System.Threading.Interlocked.Exchange(ref periodicCheckRunning, 0);
        }
    }

    private volatile bool _beltpackMessagesRestarting = false;
    private void EnsureBeltpackMessagesAlive()
    {
        if (_beltpackMessagesRestarting) return;
        try
        {
            var channels = fsApi.ExecuteString("show channels as xml");
            bool alive = channels != null &&
                channels.IndexOf(moduleName + "/endpoint-BeltpackMessages", StringComparison.OrdinalIgnoreCase) >= 0;

            if (!alive)
            {
                _beltpackMessagesRestarting = true;
                WriteToLog(LogLevel.Warning, "BeltpackMessages channel missing - re-originating.");
                MakeCall("BeltpackMessages", "&endless_playback(single_beep_silence.wav)").Wait();
            }
        }
        catch (Exception ex) { WriteToLog(LogLevel.Error, "EnsureBeltpackMessagesAlive failed: " + ex.Message); }
        finally { _beltpackMessagesRestarting = false; }
    }



    // ---- Permanent calls --------------------------------------------------------------------------------------
    // Two calls must run all the time (essential - do not change their timing):
    //   Announce         aes67/endpoints/Announce -> 99991: the broadcast/all-call stream. Started once 5 s after
    //                    load (DelayExecutionUntilAfterLoad). "reloadallcall" (RefreshPortAudioAllCall) replaces it
    //                    with a new Announce -> 99996 call, bridging the gap with a temporary call on a free Phone_.
    //                    Nothing restarts it if it drops (the dialplan's LostBroadcastAnnounce hook has no handler).
    //   BeltpackMessages aes67/endpoints/BeltpackMessages -> endless_playback(single_beep_silence.wav): the beep
    //                    a beltpack hears when its button is pressed. Started 5 s after load and re-originated by
    //                    the 10 s PeriodicCheck whenever its channel is missing (EnsureBeltpackMessagesAlive).
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
            // this is why pressing the A button beeps immediately
            id = MakeCall("BeltpackMessages", "&endless_playback(single_beep_silence.wav)").Result;
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
        WriteToLog(LogLevel.Info, "Spawning snappy background origination link for UUID: " + newID);
        
        string allCallAttempt1Result = fsApi.Execute("bgapi", "originate {origination_uuid=" + newID + "}" + moduleName + "/endpoints/" + portAudioInterface + " " + destination);
        WriteToLog(LogLevel.Info, "Background call launched: " + allCallAttempt1Result);

        Guid uuid = Guid.Empty;
        if (Guid.TryParse(newID, out uuid))
        {
            return uuid;
        }
        
        return MakeCall(portAudioInterface, destination, ++attempts).Result;
    }

    // Requires IIS/Communicator to be up and running. If IIS is restarted, there is no need to clean out phone bridges.
    // Made Async 3.28.26 so it's fire and forget - if IIS isn't running, we don't care if it succeeds
    internal void CleanOutPhoneConferences()
    {
        string apiUrl = String.Format(apiServer + "phone/CleanUpAllPhoneBridges/on/Start-Up");
        WriteToLog(LogLevel.Warning, apiUrl);
            
        try
        {
            using (WebClient client = new WebClient())
            {
                client.DownloadStringAsync(new Uri(apiUrl));
            }
        } 
        catch (Exception ex)
        {
            WriteToLog(LogLevel.Warning, ex.Message);
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
        if (xmlConference == null)
        {
            WriteToLog(LogLevel.Warning, "PutEavesdropOnHold: conference " + conference + " not found.");
            return;
        }

        foreach (XmlElement node in xmlConference.ChildNodes[1].ChildNodes)
        {
            if (node.HasAttribute("name"))
            {
                var conferenceNameNew = node.Attributes["name"].Value;
                if (conferenceNameNew.Trim() == conference.Trim())
                {
                    WriteToLog(LogLevel.Info, "Matching " + conferenceNameNew);
                    var memberCount = node.Attributes["member-count"].Value;

                    int members = int.Parse(memberCount.ToString());
                    XmlNode membersNode = GetMembersNode(node);
                    
                    foreach (XmlNode childNode in membersNode.ChildNodes)
                    {
                        string uuid, port;
                        ReadMember(childNode, out uuid, out port);
                        WriteToLog(LogLevel.Debug, "PORT  " + port);

                        if ((port.Trim().ToLower() == "announce") || (port.Trim().ToLower() == "99997"))
                        {
                            Guid guid = Guid.Empty;
                            if (Guid.TryParse(uuid, out guid))
                            {
                                lock (StateLock) { EavesdropIDs[conference] = guid; }

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
        conference = conference.Trim();
        Guid uuid;
        bool known;
        int count;
        lock (StateLock)
        {
            count = EavesdropIDs.Count;
            known = EavesdropIDs.TryGetValue(conference, out uuid);
        }
        WriteToLog(LogLevel.Info, "In TakeEavesdropOffHold with " + conference + " and ID count " + count);

        if (known)
        {
            var unholdcall = fsApi.ExecuteString("uuid_hold off " + uuid);
            WriteToLog(LogLevel.Info, "Eavesdrop for " + conference + " taken off hold - result was " + unholdcall);
        }
    }

    internal string GetPortAudioUse()
    {
        var endpoints = fsApi.ExecuteString(modulePrefix + " endpoints");
        var allEndpoints = ProcessAllEndpoints(endpoints);
        var channels = fsApi.ExecuteString("show channels as xml");

        var remainingEndpoints = ProcessUsedEndpoints(channels, allEndpoints, -9, false) ?? new List<string>();
        WriteToLog(LogLevel.Info, " In GetPortAudioUse with remainingEndpoints = " + remainingEndpoints.Count());

        return String.Join("|", remainingEndpoints);
    }

    internal string FindConferencesWithPortAudio(string portAudioInterface)
    {
        WriteToLog(LogLevel.Info, "In Find Conferences " + portAudioInterface);

        var endpoints = fsApi.ExecuteString(modulePrefix + " endpoints");
        var allEndpoints = ProcessAllEndpoints(endpoints);
        var channels = fsApi.ExecuteString("show channels as xml");

        // Rebuild and read the lookup in one locked step, so another thread's rebuild cannot change it mid-read
        lock (StateLock)
        {
            ProcessUsedEndpoints(channels, allEndpoints, -9, true);
            WriteToLog(LogLevel.Info, "Conference to PortAudio size :" + ConferenceToPortAudio.Count);

            foreach (var conf in PortAudioToConference)
            {
                WriteToLog(LogLevel.Debug, "PortAudioToConference => " + conf.Key + " and we're looking for " + portAudioInterface);
                if (conf.Key == portAudioInterface)
                {
                    string returnString = String.Join("|", conf.Value);
                    WriteToLog(LogLevel.Info, "FindConferencesWithPortAudio returns " + returnString);
                    return returnString;
                }
            }
        }

        WriteToLog(LogLevel.Info, "FindConferencesWithPortAudio returns nothing.");
        return "";
    }

    internal string RefreshPortAudioAllCall()
    {
        var endpoints = fsApi.ExecuteString(modulePrefix + " endpoints");
        var allEndpoints = ProcessAllEndpoints(endpoints);
        var channels = fsApi.ExecuteString("show channels as xml");

        var remainingEndpoints = ProcessUsedEndpoints(channels, allEndpoints, -9, false);
        WriteToLog(LogLevel.Notice, "Choosing Phone for the all call refresh");
        if (remainingEndpoints == null || remainingEndpoints.Count == 0)
        {
            WriteToLog(LogLevel.Warning, "RefreshPortAudioAllCall: no free Phone_ endpoint.");
            return "";
        }

        var chosenPhone = remainingEndpoints.Last();
        if (chosenPhone.StartsWith("Phone_"))
        {
            string tempUUIDString = fsApi.ExecuteString("create_UUID");
            string tempResult = fsApi.Execute("bgapi", "originate {origination_uuid=" + tempUUIDString + "}" + moduleName + "/endpoints/" + chosenPhone + " 99996");
            WriteToLog(LogLevel.Info, "Temporary call result = " + tempResult);

            List<Guid> previousAllCalls;
            lock (StateLock)
            {
                previousAllCalls = new List<Guid>(StoredAllCalls);
                StoredAllCalls.Clear();
            }
            foreach (var uuid in previousAllCalls)
            {
                KillActiveCall(uuid.ToString(), "Killing previous all call to refresh it.");
            }

            Guid id = Guid.Empty;
            id = MakeCall("Announce", "99996").Result;

            if (!id.Equals(Guid.Empty))
            {
                WriteToLog(LogLevel.Info, "Successfully made Announce call - ID is " + id);
                lock (StateLock) { StoredAllCalls.Add(id); }
            }
            
            KillActiveCall(tempUUIDString, "Temporary All Call backup no longer needed");
        }

        return chosenPhone;
    }

    internal string AddToPartyline(string conferenceName)
    {
        if (conferenceName.Trim().Length == 4)
        {
            int lastTwoDigitsOfConference = -1;
            int.TryParse(conferenceName.Substring(2, 2), out lastTwoDigitsOfConference);
            
            if (lastTwoDigitsOfConference >= 25 && lastTwoDigitsOfConference <= 99)
            {
                return "No need to change AES67 conference";
            }
        }
         
        foreach (var uuid in AnnounceIds(conferenceName))
        {
            WriteToLog(LogLevel.Info, "Removing Announce from " + conferenceName);
            KillActiveCall(uuid, "Temporary All Call backup no longer needed");
        }
        
        return "done";
    }

    internal List<string> AnnounceIds(string conferenceName)
    {
        var conferenceXml = GetConferenceXml(conferenceName);
        List<string> returnIDs = new List<string>();
        if (conferenceXml == null) return returnIDs; // conference does not exist (-ERR ... not found)

        foreach (XmlNode conferenceNode in conferenceXml.ChildNodes[1])
        {
            if (conferenceNode.NodeType == XmlNodeType.Element)
            {
                var conferenceFound = conferenceNode.Attributes["name"].Value;
                if (conferenceFound == conferenceName)
                {
                    foreach (XmlNode subnode in conferenceNode.ChildNodes)
                    {
                        if (subnode.NodeType == XmlNodeType.Element)
                        {
                            if (subnode.Name == "members")
                            {
                                foreach (XmlNode memberNode in subnode.ChildNodes)
                                {
                                    string uuid, callerID;
                                    ReadMember(memberNode, out uuid, out callerID);

                                    if (callerID == "Announce")
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
        
        try
        {
            using (WebClient client = new WebClient())
            {
                response = client.DownloadString(apiUrl);
                WriteToLog(LogLevel.Warning, response);

                HangUpChannel(interfaceName);

                // Fire and forget (Communicator may be down): 500 ms later, send the forced hang-up twice.
                // Uses its own WebClient: 'client' is disposed when this using block ends, before the delay is over.
                Task.Delay(500).ContinueWith(t =>
                {
                    try
                    {
                        using (WebClient forceClient = new WebClient())
                        {
                            string forced = forceClient.DownloadString(apiUrl + "?force=" + conferenceName);
                            forced = forceClient.DownloadString(apiUrl + "?force=" + conferenceName);
                            WriteToLog(LogLevel.Info, "Second attempt: " + apiUrl + " = " + forced);
                        }
                    }
                    catch (Exception forceEx)
                    {
                        WriteToLog(LogLevel.Warning, "Second attempt failed: " + apiUrl + " - " + forceEx.Message);
                    }
                });
            }
        }
        catch (Exception ex)
        {
            WriteToLog(LogLevel.Warning, "RemoveFromPartyline: " + apiUrl + " failed - " + ex.Message);
        }
        
        if (conferenceName.Trim().Length == 4)
        {
            int lastTwoDigitsOfConference = -1;
            int.TryParse(conferenceName.Substring(2, 2), out lastTwoDigitsOfConference);
            
            if ((lastTwoDigitsOfConference >= 25) && (lastTwoDigitsOfConference <= 99))
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

    // "+": a conference needs audio. Wired/dialplan conferences get an Announce; conferences in the wireless range
    //      (and intercom requests from the web GUI) get a free Phone_ (Dante/AES67) endpoint, which is originated
    //      into the conference and reported to Communicator. Returns the chosen endpoint (the web GUI uses it).
    // "-": a conference member hung up. If only Phone_ endpoints are left, they are hung up and their routes
    //      are turned off in Communicator.
    // Timing as before: the originate is a bgapi call, its channel state is polled up to 9 times every 2 s.
    internal async Task<string> ConferenceChecker(string symbol, string conferenceNumberString, string source)
    {
        WriteToLog(LogLevel.Info, string.Format("In Conference Checker - Symbol {0}, Conference Number {1}", symbol, conferenceNumberString));

        bool needToGetAnInterface = false;
        bool needToPlaceIntoConference = false;
        bool fromWebGUI = false;
        string chosenInterface = "No interface";
        string reservedPort = null; // released in finally
        string protectedId = null;  // released in finally if an exception interrupts the flow
        bool failed = false;

        // Conference numbers are 4 digits (first two + last two); anything shorter used to throw here
        if (conferenceNumberString == null || conferenceNumberString.Length < 4)
        {
            WriteToLog(LogLevel.Warning, "ConferenceChecker: conference number '" + conferenceNumberString + "' is not 4 digits.");
            return chosenInterface;
        }
        if (source == null) source = "";

        int lastTwoDigitsOfConference = -1;
        int.TryParse(conferenceNumberString.Substring(2, 2), out lastTwoDigitsOfConference);
        int conferenceReference = lastTwoDigitsOfConference - MaxWirelessConf;

        string conferenceName = "CONF." + (lastTwoDigitsOfConference - (MaxWirelessConf - 1));

        try
        {
            var xmlDocument = GetSingleConferenceXml(conferenceNumberString); // null if the conference does not exist

            switch (symbol)
            {
                case "+":
                    WriteToLog(LogLevel.Info, string.Format("Conference Reference = {0}, MaxWirelessConf = {1}, last two digits = {2}, conference = {3}",
                        conferenceReference, MaxWirelessConf, lastTwoDigitsOfConference, conferenceName));

                    if ((MaxWirelessConf == -1) || (lastTwoDigitsOfConference < MaxWirelessConf))
                    {
                        if (source == "FROM_DIALPLAN")
                        {
                            WriteToLog(LogLevel.Info, "Not a wireless conference.");

                            if (xmlDocument == null)
                            {
                                fsApi.ExecuteString("conference " + conferenceNumberString + " dial aes67/endpoints/Announce");
                            }
                            else
                            {
                                // Keep exactly one Announce in the conference
                                bool skip = true;
                                foreach (var uuid in AnnounceIds(conferenceNumberString))
                                {
                                    if (skip)
                                    {
                                        skip = false;
                                    }
                                    else
                                    {
                                        WriteToLog(LogLevel.Info, "Removing Announce from " + conferenceName);
                                        KillActiveCall(uuid, "Temporary All Call backup no longer needed");
                                    }
                                }

                                if (skip)
                                {
                                    fsApi.ExecuteString("conference " + conferenceNumberString + " dial aes67/endpoints/Announce");
                                }
                            }
                        }
                    }
                    else
                    {
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
                    if (needToGetAnInterface && int.TryParse(conferenceNumberString, out conferenceNumber))
                    {
                        WriteToLog(LogLevel.Notice, "Need to add a Dante/AES67 route for SIP conference " + conferenceNumber);

                        string getEndpoints = modulePrefix + " endpoints";
                        var endpoints = fsApi.ExecuteString(getEndpoints);
                        WriteToLog(LogLevel.Debug, "Getting Endpoints with '" + getEndpoints + "', which returned " + endpoints);

                        var allEndpoints = ProcessAllEndpoints(endpoints);
                        var channels = fsApi.ExecuteString("show channels as xml");

                        var remainingEndpoints = ProcessUsedEndpoints(channels, allEndpoints, conferenceNumber, false);
                        if (remainingEndpoints == null || remainingEndpoints.Count == 0)
                        {
                            WriteToLog(LogLevel.Warning, "No free Phone_ endpoint for conference " + conferenceNumberString);
                            break; // "No interface", as before
                        }

                        chosenInterface = remainingEndpoints.First();
                        WriteToLog(LogLevel.Notice, "Picking from " + remainingEndpoints.Count + ", first is " + chosenInterface);

                        // ProcessUsedEndpoints returns { "ALREADY-IN-USE", <port> } when a Phone_ port already serves this conference
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

                        // Random choice as before, but never a port that another call has just been given and whose
                        // channel is not in "show channels" yet (two calls arriving together picked the same port)
                        lock (StateLock)
                        {
                            List<string> candidates = remainingEndpoints;
                            if (newDantePort)
                            {
                                var unreserved = remainingEndpoints.Where(p => !ReservedPorts.Contains(p)).ToList();
                                if (unreserved.Count > 0) candidates = unreserved;
                            }
                            chosenInterface = candidates[PortRandom.Next(candidates.Count)];
                            if (newDantePort && ReservedPorts.Add(chosenInterface)) reservedPort = chosenInterface;
                        }
                        WriteToLog(LogLevel.Notice, "Chosen Endpoint: " + chosenInterface);

                        string conferenceCommand = "TalkAndListen";

                        if (!InterfaceIsAlreadyInUse(chosenInterface))
                        {
                            Guid callGUID = Guid.NewGuid();
                            string originateString = "originate {interface=" + chosenInterface + ",origination_uuid=" + callGUID.ToString() + "}" + moduleName + "/endpoints/" + chosenInterface + " 99999" + conferenceNumber.ToString();
                            WriteToLog(LogLevel.Notice, originateString);

                            lock (StateLock) { ProtectedGUIDs.Add(callGUID.ToString()); }
                            protectedId = callGUID.ToString();
                            WriteToLog(LogLevel.Notice, "Protecting " + callGUID.ToString());

                            var joinConference1 = fsApi.Execute("bgapi", originateString);
                            WriteToLog(LogLevel.Notice, "Job ID " + joinConference1);

                            // Wait for the background originate: up to 9 checks, 2 s apart
                            bool needToWait = true;
                            int counter = 1;

                            while ((needToWait == true) && counter < 10)
                            {
                                WriteToLog(LogLevel.Info, "Waiting for background processing, attempt " + counter + "...");
                                string result = fsApi.ExecuteString("eval uuid:" + callGUID.ToString() + " ${channel-state}");
                                WriteToLog(LogLevel.Info, "STATE " + result);

                                switch (result.ToLower().Trim())
                                {
                                    case "cs_exchange_media":
                                        needToWait = false;
                                        lock (StateLock) { ProtectedGUIDs.Remove(callGUID.ToString()); }
                                        WriteToLog(LogLevel.Info, "Successfully exchanging media for " + moduleName + " " + chosenInterface + " to " + conferenceNumberString);
                                        break;
                                    case "cs_execute":
                                        lock (StateLock) { ProtectedGUIDs.Remove(callGUID.ToString()); }
                                        WriteToLog(LogLevel.Info, "Successfully executed " + moduleName + " " + chosenInterface + " to " + conferenceNumberString);
                                        needToWait = false;
                                        break;
                                    default: // "", cs_routing, ...: keep waiting
                                        break;
                                }

                                if (needToWait)
                                {
                                    counter++;
                                    await Task.Delay(2000);
                                }
                            }

                            if (!needToPlaceIntoConference)
                            {
                                WriteToLog(LogLevel.Notice, "No need to place this call automatically into a conference... removing protection from " + callGUID.ToString());
                                RemoveProtection(callGUID.ToString()); // not awaited (1 s delay runs in the background)
                            }
                            else if (!fromWebGUI)
                            {
                                string apiUrl = String.Format(apiServer + "conferencedata/{0}/{1}/{2}/sipserverreuse", conferenceName, chosenInterface, conferenceCommand);
                                if (newDantePort)
                                {
                                    apiUrl = String.Format(apiServer + "conferencedata/{0}/{1}/{2}/sipservernewchannel", conferenceName, chosenInterface, conferenceCommand);
                                }
                                WriteToLog(LogLevel.Notice, apiUrl);
                                WriteToLog(LogLevel.Notice, HttpGet(apiUrl));
                            }

                            if (!newDantePort)
                            {
                                var apiUrlLevel = String.Format(apiServer + "portlevel/{0}/{1}/", conferenceNumber, chosenInterface);
                                WriteToLog(LogLevel.Notice, apiUrlLevel);
                                WriteToLog(LogLevel.Info, HttpGet(apiUrlLevel));
                            }

                            if (debugMode && xmlDocument != null)
                            {
                                // diagnostics only: list the members (from before the originate)
                                foreach (XmlNode node in xmlDocument.ChildNodes[1].ChildNodes)
                                {
                                    if (node.Attributes["name"].Value != conferenceNumberString) continue;
                                    WriteToLog(LogLevel.Debug, "Conference " + conferenceNumberString + " has " + node.Attributes["member-count"].Value + " members");
                                    foreach (XmlNode childNode in GetMembersNode(node).ChildNodes)
                                    {
                                        string uuid, port;
                                        ReadMember(childNode, out uuid, out port);
                                        WriteToLog(LogLevel.Debug, "PORT  " + port);
                                    }
                                }
                            }

                            if (fromWebGUI)
                            {
                                WriteToLog(LogLevel.Notice, "Telling the web GUI that we have " + chosenInterface);
                            }

                            RemoveProtection(callGUID.ToString()); // not awaited
                            return chosenInterface;
                        }
                    }
                    break;

                case "-":
                    WriteToLog(LogLevel.Notice, "Checking to see if we are done with the Dante route");
                    if (xmlDocument == null)
                    {
                        WriteToLog(LogLevel.Notice, "Conference " + conferenceNumberString + " no longer exists.");
                        break;
                    }
                    WriteToLog(LogLevel.Debug, xmlDocument.OuterXml);

                    foreach (XmlNode node in xmlDocument.ChildNodes[1].ChildNodes)
                    {
                        List<string> DanteIDs = new List<string>();
                        List<string> DantePorts = new List<string>();

                        var conferenceNameNew = node.Attributes["name"].Value;

                        if (conferenceNameNew == conferenceNumberString)
                        {
                            int members = int.Parse(node.Attributes["member-count"].Value);
                            WriteToLog(LogLevel.Notice, "Conference " + conferenceNumberString + " has " + members + " members");

                            XmlNode membersNode = GetMembersNode(node);
                            foreach (XmlNode childNode in membersNode.ChildNodes)
                            {
                                string uuid, port;
                                ReadMember(childNode, out uuid, out port);
                                WriteToLog(LogLevel.Info, "Conference " + conferenceNumberString + " member: " + port);

                                if (port.ToLower().Contains("phone_"))
                                {
                                    members--;
                                    DanteIDs.Add(uuid);
                                    DantePorts.Add(port);
                                }
                            }

                            // Only Phone_ endpoints left: free them and turn their routes off
                            if (members == 0)
                            {
                                foreach (var id in DanteIDs)
                                {
                                    KillActiveCall(id, "Conference call has been hung up, we are freeing up matrix ports.");
                                }

                                WriteToLog(LogLevel.Notice, "About to remove Conference " + conferenceNumberString + " routes for " + DantePorts.Count + " port(s)");
                                foreach (var danteInterface in DantePorts)
                                {
                                    string apiUrl = String.Format(apiServer + "conferencedata/{0}/{1}/{2}", conferenceName, danteInterface, "Off");
                                    WriteToLog(LogLevel.Notice, apiUrl);
                                    WriteToLog(LogLevel.Notice, HttpGet(apiUrl));
                                }
                                // (returned after the FIRST port before, so the other ports' routes stayed on)
                                if (DantePorts.Count > 0) return "Added";
                            }
                        }
                    }
                    break;
            }
        }
        catch (Exception ex)
        {
            failed = true;
            WriteToLog(LogLevel.Warning, "ConferenceChecker " + symbol + " " + conferenceNumberString + ": " + ex.Message);
        }
        finally
        {
            // e.g. Communicator did not answer: before, the call stayed protected forever and could never be cleaned up
            if (failed && protectedId != null) RemoveProtection(protectedId); // not awaited
            if (reservedPort != null)
            {
                lock (StateLock) { ReservedPorts.Remove(reservedPort); }
            }
        }

        return chosenInterface;
    }

    // Called without await on purpose: the protection ends 1 s later in the background.
    async Task RemoveProtection(string protectedID)
    {
        WriteToLog(LogLevel.Info, "Removing protection from " + protectedID);
        await Task.Delay(1000);
        int left;
        lock (StateLock)
        {
            ProtectedGUIDs.Remove(protectedID);
            left = ProtectedGUIDs.Count;
        }
        WriteToLog(LogLevel.Info, "Currently " + left + " protected GUIDs left");
    }

    private string GetCurrentPINs()
    {
        // (the double slash in ".../api//pin" is what has always been sent; kept as is)
        var response = HttpGet(apiServer + "/pin");
        WriteToLog(LogLevel.Info, response);
        return response;
    }

    private string HangUpConference(string conferenceNumber)
    {
        if ((conferenceNumber == "0000") || (conferenceNumber == "Announce"))
        {
            WriteToLog(LogLevel.Info, "Not hanging up the announce conference");
            return "Protected Conference";
        }
        
        WriteToLog(LogLevel.Info, "In HangUpConference with  " + conferenceNumber);
        var xmlDocument = GetSingleConferenceXml(conferenceNumber.Trim());
        if (xmlDocument == null)
        {
            WriteToLog(LogLevel.Info, "HangUpConference: conference " + conferenceNumber + " not found.");
            return "FAILURE";
        }

        // (returns "FAILURE" even after hanging up, as it always has: PhoneController passes it to the web page)
        foreach (XmlNode currentNode in xmlDocument.ChildNodes[1].ChildNodes)
        {
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
    /// 
    public string OperatorJoin(string callerID, string conference)
    {
        string uuid = GetIDFromCaller(callerID);
        WriteToLog(LogLevel.Info, "In OperatorJoin, " + callerID + " resolves to " + uuid + " connecting to " + conference);
        string apiResponse = fsApi.ExecuteString("uuid_transfer " + uuid + " " + conference);
        WriteToLog(LogLevel.Info, apiResponse);
        return apiResponse;
    }

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
    }



    // Command dispatcher for "managed CommunicatorConnector <command> [args]" (dialplan) and the API calls from
    // Communicator. Arguments are split on spaces. The commands and their results are unchanged; a command that
    // is missing arguments now returns "-ERR ..." instead of failing with an index exception.
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
                if (!HasArgs(arguments, 1, out returnString)) break;
                returnString = FindConferencesWithPortAudio(arguments[1]);
                break;
            case "swapfromeavesdroptodante":
                if (!HasArgs(arguments, 1, out returnString)) break;
                PutEavesdropOnHold(arguments[1]);
                returnString = "No result received.";
                break;
            case "swapfromdantetoeavesdrop":
                if (!HasArgs(arguments, 1, out returnString)) break;
                TakeEavesdropOffHold(arguments[1]);
                returnString = "No result received.";
                break;
            case "adddantetocall":
                if (!HasArgs(arguments, 1, out returnString)) break;
                AddToPartyline(arguments[1]);
                returnString = "No result received.";
                break;
            case "removedantefromcall":
                if (!HasArgs(arguments, 2, out returnString)) break;
                RemoveFromPartyline(arguments[1], arguments[2]);
                returnString = "No result received.";
                break;
            case "seteavesdropid":
                if (!HasArgs(arguments, 1, out returnString)) break;
                returnString = "No result received.";
                Guid newEavesdropID = Guid.Empty;
                if (Guid.TryParse(arguments[1], out newEavesdropID))
                {
                    EavesdropOnAnnounce = newEavesdropID;
                    WriteToLog(LogLevel.Info, "New ID to eavesdrop on: " + EavesdropOnAnnounce);
                }
                break;
            case "conferencechecker":
                if (!HasArgs(arguments, 2, out returnString)) break;
                returnString = ConferenceChecker(arguments[1], arguments[2], thirdParameter).Result;
                break;
            case "getnextconference":
                returnString = GetNextConferenceNumber();
                break;
            case "hangupconference":
                if (!HasArgs(arguments, 1, out returnString)) break;
                returnString = "No result received.";
                if (!InTestMode)
                {
                    returnString = HangUpConference(arguments[1]);
                }
                break;
            case "checkoperator":
            case "operatorchecker":
                if (!HasArgs(arguments, 2, out returnString)) break;
                returnString = CheckOperator(arguments[1], arguments[2]);
                break;
            case "operatorjoin":
                if (!HasArgs(arguments, 2, out returnString)) break;
                returnString = OperatorJoin(arguments[1], arguments[2]);
                break;
            case "operatorleave":
                if (!HasArgs(arguments, 1, out returnString)) break;
                returnString = OperatorLeave(arguments[1]);
                break;
            case "hangupchannel":
            case "hangupportaudio":
                if (!HasArgs(arguments, 1, out returnString)) break;
                returnString = HangUpChannel(arguments[1]);
                break;
            case "hangupcall":
                if (!HasArgs(arguments, 2, out returnString)) break;
                returnString = "No result received.";
                if (!InTestMode)
                {
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
                returnString = returnXml != null ? returnXml.OuterXml : "Xml was null";
                break;
            case "conference":
                if (!HasArgs(arguments, 1, out returnString)) break;
                var returnIndividualXml = GetConferenceXml(arguments[1]);
                returnString = returnIndividualXml != null ? returnIndividualXml.OuterXml : "Xml was null";
                break;
            case "getpins":
                return GetCurrentPINs().Replace("\"", "");
            case "setdebug":
                if (!HasArgs(arguments, 1, out returnString)) break;
                returnString = "No result received.";
                bool onOff = false;
                if (bool.TryParse(arguments[1], out onOff))
                {
                    debugMode = onOff;
                    returnString = "Debug mode now " + onOff;
                }
                break;
            default:
                // (also reached by the dialplan hang-up hooks LostToneStreamForBeltpacks / LostBroadcastAnnounce,
                // which have no handler here)
                WriteToLog(LogLevel.Error, "Unable to work out what was meant by " + arguments[0]);
                break;
        }

        return returnString;
    }

    // True when 'arguments' has at least 'count' arguments after the command name; otherwise logs and sets 'error'.
    private static bool HasArgs(string[] arguments, int count, out string error)
    {
        if (arguments.Length > count)
        {
            error = null;
            return true;
        }
        error = "-ERR " + arguments[0] + " needs " + count + " argument(s)";
        WriteToLog(LogLevel.Error, error + ", got: " + String.Join(" ", arguments));
        return false;
    }

    public static void Main()
    {
        switch (FreeSWITCH.Script.ContextType)
        {
            case ScriptContextType.Api:
                {
                    var ctx = FreeSWITCH.Script.GetApiContext();
                    break;
                }
            case ScriptContextType.ApiBackground:
                {
                    var ctx = FreeSWITCH.Script.GetApiBackgroundContext();
                    break;
                }
            case ScriptContextType.App:
                {
                    var ctx = FreeSWITCH.Script.GetAppContext();
                    break;
                }
        }
    }

    public static void WriteToLog(LogLevel logLevel, string logEntry)
    {
        switch (logLevel)
        {
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

    // ---- Shared helpers --------------------------------------------------------------------------------------

    // Reads one <member> of "conference ... xml_list": its uuid and caller_id_number ("" when absent).
    internal static void ReadMember(XmlNode memberNode, out string uuid, out string callerIdNumber)
    {
        uuid = "";
        callerIdNumber = "";
        foreach (XmlNode detail in memberNode.ChildNodes)
        {
            switch (detail.Name)
            {
                case "uuid":
                    uuid = detail.InnerText;
                    break;
                case "caller_id_number":
                    callerIdNumber = detail.InnerText;
                    break;
            }
        }
    }

    // Synchronous GET to Communicator, exactly as before (WebClient, default timeout). Exceptions propagate to the
    // caller, which decides - as before - whether a failure stops its processing.
    internal static string HttpGet(string url)
    {
        using (WebClient client = new WebClient())
        {
            return client.DownloadString(url);
        }
    }

    // Parses an fsApi XML result; null when it is not XML (e.g. "-ERR Conference 1234 not found").
    internal static XmlDocument ParseApiXml(string apiResponse, string what)
    {
        try
        {
            XmlDocument xmlDocument = new XmlDocument();
            using (MemoryStream stream = new MemoryStream(Encoding.UTF8.GetBytes(apiResponse)))
            {
                XmlTextReader reader = new XmlTextReader(stream);
                xmlDocument.Load(reader);
            }
            return xmlDocument;
        }
        catch (Exception)
        {
            WriteToLog(LogLevel.Info, what + " - not XML: " + apiResponse);
            return null;
        }
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

        foreach (XmlNode currentNode in xmlDocument.ChildNodes[0].ChildNodes) 
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
        if (xmlDocument == null) return allConferences;

        foreach (XmlNode currentNode in xmlDocument.ChildNodes[1].ChildNodes)
        {
            allConferences.Add(currentNode.Attributes["name"].Value);
        }

        return allConferences;
    }

    public string GetNextConferenceNumber()
    {
        List<string> conferences = GetAllConferences();
        WriteToLog(LogLevel.Debug, "In GetConference with " + conferences.Count + " conferences.");

        // Dial-in conferences are 9000-9999 (4 digits, as the rest of the code assumes). The search still continues
        // from the last number handed out, but now wraps back to 9000 instead of growing past 9999.
        for (int tries = 0; tries < 1000; tries++)
        {
            if (dialInConferenceNumberStart > 9999) dialInConferenceNumberStart = 9000;
            if (!conferences.Contains(dialInConferenceNumberStart.ToString()))
            {
                return dialInConferenceNumberStart.ToString();
            }
            dialInConferenceNumberStart++;
        }

        return "9876"; // all 1000 in use
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

            var newOrphans = conference != null ? CheckConferences(conference, channelList) : new List<string>();

            // A Phone_ channel is hung up only when it is orphaned on two consecutive checks (10 s apart)
            List<string> previousOrphans;
            lock (StateLock) { previousOrphans = new List<string>(FoundOrphanedCallsWaitingToResolve); }
            foreach (var oldOrphan in previousOrphans)
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

            lock (StateLock) { FoundOrphanedCallsWaitingToResolve = newOrphans; }
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
            // Remove every Phone_ that a conference lists from the channel list; what is left is a Phone_ channel
            // in a conference that does not list it (an orphan)
            foreach (XmlNode conferenceNode in conference.ChildNodes[1].ChildNodes)
            {
                if (conferenceNode.NodeType != XmlNodeType.Element) continue;
                XmlAttribute nameAttribute = conferenceNode.Attributes["name"];
                if (nameAttribute == null) continue;
                string conferenceName = nameAttribute.Value;
                if (!channelList.ContainsKey(conferenceName)) continue; // no Phone_ channel in this conference

                foreach (XmlNode conferenceMember in GetMembersNode(conferenceNode).ChildNodes)
                {
                    foreach (XmlNode conferenceDetail in conferenceMember.ChildNodes)
                    {
                        if (conferenceDetail.NodeType == XmlNodeType.Element && conferenceDetail.Name == "caller_id_number"
                            && conferenceDetail.InnerText.StartsWith("Phone_"))
                        {
                            string phone = conferenceDetail.InnerText.Trim();
                            if (!channelList[conferenceName].Contains(phone))
                            {
                                WriteToLog(LogLevel.Warning, "MISMATCH Conference has phone that channel list doesn't -  Conference " + conferenceName + " and Phone " + phone);
                            }
                            else
                            {
                                channelList[conferenceName].Remove(phone);
                            }
                        }
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

    // One conference ("conference <name> xml_list"); null if it does not exist.
    internal XmlDocument GetSingleConferenceXml(string conferenceName)
    {
        WriteToLog(LogLevel.Debug, "In Get Single Conference looking for " + conferenceName);
        string apiResponse = fsApi.ExecuteString("conference " + conferenceName + " xml_list");
        WriteToLog(LogLevel.Debug, "conference " + conferenceName + " xml_list: " + apiResponse);
        return ParseApiXml(apiResponse, "conference " + conferenceName + " xml_list");
    }

    // All conferences ("conference xml_list"). The parsed document is reused while the text is unchanged.
    internal XmlDocument GetConferencesXml()
    {
        string apiResponse = fsApi.ExecuteString("conference xml_list");
        lock (StateLock)
        {
            if (apiResponse == lastAllConferencesString && lastAllConferencesXML != null)
            {
                return lastAllConferencesXML;
            }
        }

        XmlDocument xmlDocument = ParseApiXml(apiResponse, "conference xml_list");
        if (xmlDocument != null)
        {
            lock (StateLock)
            {
                lastAllConferencesString = apiResponse;
                lastAllConferencesXML = xmlDocument;
            }
        }
        return xmlDocument;
    }

    // One conference, by ID; null if it does not exist. (It no longer overwrites the all-conferences cache with a
    // single conference's XML.)
    internal XmlDocument GetConferenceXml(string conferenceID)
    {
        string command = string.Format("conference {0} xml_list", conferenceID.Trim());
        return ParseApiXml(fsApi.ExecuteString(command), command);
    }
    private static List<string> ProcessAllEndpoints(string endpoints)
    {
        List<string> allEndpoints = new List<string>();
        var endpointArray = endpoints.Split('\n');

        WriteToLog(LogLevel.Debug, "ProcessAllEndpoints => endpoints length " + endpointArray.Length);

        // "aes67 endpoints" lines: "endpoint name: <name> \t instream: ..., outstream: ..." -> 3rd word.
        // The summary line "Total endpoints: N" adds a harmless entry "N" (callers only use names containing
        // "Phone"); the element after the final newline is skipped. (mod_portaudio parsing removed: moduleName is aes67.)
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
                    allEndpoints.Add(endpointName);
                }
            }
        }
        WriteToLog(LogLevel.Debug, "Returning All Endpoints " + allEndpoints.Count);
        
        return allEndpoints;
    }

    // From all aes67 endpoints and "show channels as xml": the free Phone_ endpoints (sorted), or
    // { "ALREADY-IN-USE", <port> } when a Phone_ already serves 'conferenceNumber' (unless ignoreFound).
    // Rebuilds the ConferenceToPortAudio / PortAudioToConference lookups. null if the channel list is not XML.
    private static List<string> ProcessUsedEndpoints(string endpoints, List<string> allEndpoints, int conferenceNumber, bool ignoreFound)
    {
        lock (StateLock) // the lookups are shared by all threads (re-entrant: FindConferencesWithPortAudio holds it too)
        {
            return ProcessUsedEndpointsLocked(endpoints, allEndpoints, conferenceNumber, ignoreFound);
        }
    }

    private static List<string> ProcessUsedEndpointsLocked(string endpoints, List<string> allEndpoints, int conferenceNumber, bool ignoreFound)
    {
        try
        {
            ConferenceToPortAudio.Clear();
            PortAudioToConference.Clear();

            WriteToLog(LogLevel.Debug, "ProcessUsedEndpoints for conference " + conferenceNumber);
            XmlDocument channelsXml = new XmlDocument();
            channelsXml.LoadXml(endpoints);
            var endpointList = new List<string>(allEndpoints);

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
            WriteToLog(LogLevel.Warning, "ProcessUsedEndpoints: " + ex.Message); // (went to Debug.WriteLine = nowhere)
        }

        return null;
    }

    internal string KillActiveCall(string callUUID, string reason)
    {
        bool isProtected;
        int protectedCount;
        lock (StateLock)
        {
            isProtected = ProtectedGUIDs.Contains(callUUID);
            protectedCount = ProtectedGUIDs.Count;
        }
        WriteToLog(LogLevel.Notice, string.Format("In KillActiveCall - Hanging up active call {0} because {1}. Protected count {2}.", callUUID, reason, protectedCount));
        if (isProtected)
        {
            WriteToLog(LogLevel.Notice, "Not killing " + callUUID + " as it was protected");
            return "Not killing " + callUUID + " as it was protected.";
        }
        else
        {
            string result = fsApi.ExecuteString("eval uuid:" + callUUID + " ${channel-state}");
            WriteToLog(LogLevel.Notice, "STATE OF GUID TO BE KILLED = " + result);

            // full channel dump only with "setdebug true" (it was written at Notice for every hang-up)
            if (debugMode)
            {
                result = fsApi.ExecuteString("uuid_dump " + callUUID);
                WriteToLog(LogLevel.Debug, "CHANNEL DUMP OF GUID TO BE KILLED = " + result);
            }

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