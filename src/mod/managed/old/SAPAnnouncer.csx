using System;
using System.Net;
using System.Net.Sockets;
using System.Xml;
using FreeSWITCH;
using FreeSWITCH.Native;
using System.Linq;
using System.Collections.Generic;
using System.Threading;
using System.Threading.Tasks;
using System.IO;
using System.Text;
using System.Runtime.InteropServices;

public class SAPAnnouncer : IApiPlugin, IAppPlugin, ILoadNotificationPlugin
{
    static Timer AnnounceTimer;
    const bool cleanForDante = true;
	
    const bool debugMode = true;
    int counter = 0;
    string multicastIP = "10.8.90.10";
    //string IPv4UsedForAES67 = "192.168.86.242";
	string IPv4UsedForAES67 = "10.8.90.10";	//modified below

    UdpClient udpClient = null;
    static Api fsApi = new Api(null);

    List<string> SDPs = new List<string>();

    public void StartSendingSAP()
    {
        IPv4UsedForAES67 = fsApi.ExecuteString("eval ${multicastIP}");		//read from vars.xml
        WriteToLog(LogLevel.Info, "Multicast IP is " + IPv4UsedForAES67);

        var gstreamerConfXML = fsApi.ExecuteString("xml_locate configuration configuration name aes67.conf");
        // var gstreamerConfXML = File.ReadAllText("aes67.conf.xml");
        WriteToLog(LogLevel.Info, gstreamerConfXML);
        XmlDocument gstreamerConf = new XmlDocument();
        gstreamerConf.LoadXml(gstreamerConfXML);

        int sessionID = 1;
        // string ipv4 = fsApi.ExecuteString("eval ${local_ip_v4}");

        udpClient = new UdpClient();
		udpClient.ExclusiveAddressUse = false;
		WriteToLog(LogLevel.Info, IPv4UsedForAES67);
        try
        {
            udpClient.Client.Bind((EndPoint)new IPEndPoint(IPAddress.Parse(IPv4UsedForAES67), 9875));
        }
        catch (Exception ex)
        {
		string exMessage = "Unable to take control of " + IPv4UsedForAES67 + " on port " + 9875.ToString();
		exMessage += " - Is Rav2SAP running? - " + ex.Message + " - " + ex.InnerException;
			Log.WriteLine(LogLevel.Critical, exMessage);
            return;
        }



        string defaultTXmulticastPort = "5004";
        string defaultTXchannels = "";
        string defaultTXCodec = "";
        string defaultTXPtime = "";
        string defaultTXSampleRate = "";
        string RTPnic = IPv4UsedForAES67;


        foreach (XmlNode xmlNode in gstreamerConf.DocumentElement.ChildNodes)
        {
            if ((xmlNode.NodeType == XmlNodeType.Element) && (xmlNode.Name.ToLower() == "settings"))
            {
                foreach (XmlNode settingsNode in xmlNode.ChildNodes)
                {
                    if (settingsNode.NodeType == XmlNodeType.Element)
                    {
                        string currentAttribute = "";

                        foreach (XmlAttribute attribute in settingsNode.Attributes)
                        {
                            if (attribute.Name.ToLower() == "name")
                            {
                                currentAttribute = attribute.Value;
                            }
                            else if (attribute.Name.ToLower() == "value")
                            {
                                switch (currentAttribute.ToLower())
                                {

                                    case "tx-port":
                                        defaultTXmulticastPort = attribute.Value;
                                        break;

                                    case "channels":
                                        defaultTXchannels = attribute.Value;
                                        break;

                                    case "tx-codec":
                                        defaultTXCodec = attribute.Value;
                                        break;

                                    case "ptime-ms":
                                        defaultTXPtime = attribute.Value;
                                        break;

                                    case "sample-rate":
                                        defaultTXSampleRate = attribute.Value;
                                        break;

                                    case "rtp-iface":
                                    case "rtp-interface":
                                        RTPnic = attribute.Value;
                                        break;
                                }
                            }

                        }
                    }
                }
            }

            if ((xmlNode.NodeType == XmlNodeType.Element) && (xmlNode.Name.ToLower() == "streams"))
            {
                foreach (XmlNode streamNode in xmlNode.ChildNodes)
                {
                    if (xmlNode.NodeType == XmlNodeType.Element)
                    {
                        string streamTXAddress = "";
                        string streamTXMulticastPort = "";
                        string streamTXChannels = "";
                        string streamTXCodec = "";
                        string streamTXPtime = "";
                        string streamTXSampleRate = "";



                        string streamName = streamNode.Attributes["name"].InnerText;
                        WriteToLog(LogLevel.Info, streamName);
                        foreach (XmlNode infoNode in streamNode.ChildNodes)
                        {
                            if (infoNode.NodeType == XmlNodeType.Element)
                            {
                                string currentAttribute = "";

                                foreach (XmlAttribute attribute in infoNode.Attributes)
                                {
                                    if (attribute.Name.ToLower() == "name")
                                    {
                                        currentAttribute = attribute.Value;
                                    }
                                    else if (attribute.Name.ToLower() == "value")
                                    {
                                        switch (currentAttribute.ToLower())
                                        {
                                            case "tx-address":
                                                streamTXAddress = attribute.Value;
                                                break;

                                            case "tx-port":
                                                streamTXMulticastPort = attribute.Value;
                                                break;

                                            case "channels":
                                                streamTXChannels = attribute.Value;
                                                break;

                                            case "tx-codec":
                                                streamTXCodec = attribute.Value;
                                                break;

                                            case "ptime-ms":
                                                streamTXPtime = attribute.Value;
                                                break;

                                            case "sample-rate":
                                                streamTXSampleRate = attribute.Value;
                                                break;
                                        }
                                    }


                                }
                            }
                        }


                        string sdpTXAddress = streamTXAddress;
                        string sdpTXMulticastPort = string.IsNullOrEmpty(streamTXMulticastPort) ? defaultTXmulticastPort : streamTXMulticastPort;
                        string sdpTXChannels = string.IsNullOrEmpty(streamTXChannels) ? defaultTXchannels : streamTXChannels;
                        string sdpTXCodec = string.IsNullOrEmpty(streamTXCodec) ? defaultTXCodec : streamTXCodec;
                        string sdpTXPtime = string.IsNullOrEmpty(streamTXPtime) ? defaultTXPtime : streamTXPtime;
                        string sdpTXSampleRate = string.IsNullOrEmpty(streamTXSampleRate) ? defaultTXSampleRate : streamTXSampleRate;


                        StringBuilder sdpBuilder = new StringBuilder();

                        sessionID++;

                        sdpBuilder.AppendLine("v=0");
                        sdpBuilder.AppendLine("o=- " + sessionID + " " + sessionID + " IN IP4 " + IPv4UsedForAES67);

                        sdpBuilder.AppendLine("s=" + streamName);
                        sdpBuilder.AppendLine("c=IN IP4 " + sdpTXAddress + "/32");
                        sdpBuilder.AppendLine("t=0 0");
                        sdpBuilder.AppendLine("m=audio " + defaultTXmulticastPort + " RTP/AVP 96");

                        int channelsInt = 0;

                        if (int.TryParse(defaultTXchannels, out channelsInt))
                        {
                            string channelString = "i=";//  + channelsInt.ToString() + " channels: ";
                            for (int i = 1; i < channelsInt; i++)
                            {
                                channelString += "channel_" + (i - 1).ToString() + ",";
                            }
                            channelString += "channel_" + (channelsInt - 1).ToString();

                            sdpBuilder.AppendLine(channelString);
                        }

                        sdpBuilder.AppendLine("a=clock-domain:PTPv2 0");
                        sdpBuilder.AppendLine("a=ts-refclk:ptp=IEEE1588-2008:00-00-00-00-00-00-00-00:0");
                        sdpBuilder.AppendLine("a=mediaclk:direct=0");
                        sdpBuilder.AppendLine("a=source-filter: incl IN IP4 " + sdpTXAddress + " " + IPv4UsedForAES67);
                        sdpBuilder.AppendLine("a=rtpmap:96 " + defaultTXCodec + "/" + defaultTXSampleRate + "/" + defaultTXchannels);
                        sdpBuilder.AppendLine("a=framecount=48");

                        // sdpBuilder.AppendLine("a=recvonly");


                        sdpBuilder.AppendLine("a=ptime:" + defaultTXPtime);
                        //  sdpBuilder.AppendLine("a=ts-refclk:ptp=IEEE1588-2008:00-00-00-00-00-00-00-00:0");

                        //sdpBuilder.AppendLine("a=framecount=48");

                        WriteToLog(LogLevel.Info,sdpBuilder.ToString());

                        SDPs.Add(sdpBuilder.ToString());

                    }

                }
            }
        }

        //  new Task((Action)(() => this.SendAnnouncements(sdpBuilder.ToString()))).Start();
        AnnounceTimer = new Timer(new TimerCallback(SendAnnouncements), null, 1000, 20000);

    }


    internal void SendAnnouncements(object timerinput)
    {
        foreach (var sdp in SDPs)
        {
            List<byte> byteList = new List<byte>();
            byteList.Add((byte)32);
            byteList.Add((byte)0);
            byteList.AddRange((IEnumerable<byte>)new byte[2]
            {
              (byte) counter,
              (byte) ((uint) counter >> 8)
            });
            if ((short)32766 == counter)
                counter = (short)0;
            ++counter;
            byteList.AddRange((IEnumerable<byte>)IPAddress.Parse(IPv4UsedForAES67).GetAddressBytes());
            byteList.AddRange((IEnumerable<byte>)Encoding.UTF8.GetBytes("application/sdp"));
            byteList.Add((byte)0);
            byteList.AddRange((IEnumerable<byte>)Encoding.UTF8.GetBytes(sdp));
            udpClient.Send(byteList.ToArray(), byteList.ToArray().Length, "239.255.255.255", 9875);
            udpClient.Send(byteList.ToArray(), byteList.ToArray().Length, "224.2.127.254", 9875);

            WriteToLog(LogLevel.Info,"Sending " + sdp);

        }
    }





    public void Execute(ApiContext context)
    {

        StartSendingSAP();

        WriteToLog(LogLevel.Info, string.Format("SAP_Announcer executed with args '{0}' and event type {1}.", context.Arguments, context.Event == null ? "<none>" : context.Event.GetEventType()));

        string returnString = Process(context.Arguments.Split(' '));
        context.Stream.Write(returnString);

    }

    public void ExecuteBackground(ApiBackgroundContext context)
    {
        WriteToLog(LogLevel.Info, string.Format("SAP_Announcer on a background thread #({0}), with args '{1}'.", System.Threading.Thread.CurrentThread.ManagedThreadId, context.Arguments));

        //string returnString = Process(context.Arguments.Split(' '));
        // context.Stream.Write(returnString);
    }


    public string Process(string[] arguments)
    {
        string functionRequired = arguments[0].ToLower();
        string returnString = "No result received.";

        WriteToLog(LogLevel.Debug, "Processing " + functionRequired);

        return returnString;
    }

    public bool Load()
    {
        StartSendingSAP();
        return true;
    }

    public void Run(FreeSWITCH.AppContext context)
    {

        WriteToLog(LogLevel.Info, "SAP_Announcer Running in AppPlugin process");


    }


    public static void Main()
    {
        switch (FreeSWITCH.Script.ContextType)
        {
            case ScriptContextType.Api:
                {

                    var ctx = FreeSWITCH.Script.GetApiContext();
                    ctx.Stream.Write("Script executing as API with args: " + ctx.Arguments);
                    break;
                }
            case ScriptContextType.ApiBackground:
                {
                    var ctx = FreeSWITCH.Script.GetApiBackgroundContext();

                    WriteToLog(LogLevel.Debug, "Executing as APIBackground with args: " + ctx.Arguments);
                    break;
                }
            case ScriptContextType.App:
                {
                    var ctx = FreeSWITCH.Script.GetAppContext();
                    WriteToLog(LogLevel.Debug, "Executing as App with args: " + ctx.Arguments);
                    break;
                }
            case ScriptContextType.None:
                {
                    SAPAnnouncer announcer = new SAPAnnouncer();
                    announcer.StartSendingSAP();
                    break;
                }
        }
    }

    public static void WriteToLog(LogLevel logLevel, string logEntry)
    {
        if (fsApi == null)
        {
            Console.WriteLine(logEntry);
        }
        else
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
                    // Log.WriteLine(logLevel, logEntry);
                    break;
            }
        }

    }

}