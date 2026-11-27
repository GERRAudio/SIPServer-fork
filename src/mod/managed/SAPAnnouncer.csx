// Version Date: 2026-11-26
//
// Compiled at load by mod_managed with the .NET Framework C# compiler (CodeDom "v4.0"): C# 5 syntax only
// (no $"" strings, ?. , => members, out-variable declarations, tuples or nameof).
//
// 2026-11-26 fixes:
//  - SAP message ID hash is fixed per stream (was a counter bumped on every packet, so receivers such as the
//    GADREC recorder saw every announcement as a new session and listed duplicate streams).
//  - Streams without a tx-address (receive-only, e.g. AnnounceLine) are not announced (was "c=IN IP4 /32").
//  - RTP payload type comes from aes67.conf rtp-payload-type (SDP said 96 while mod_aes67 sends 98).
//  - a=ts-refclk names the real PTP grandmaster (vars.xml aes67_ptp_gmid, else DefaultPtpGmid) and the
//    ptp-domain from aes67.conf (was 00-00-00-00-00-00-00-00 and a fixed domain 0).
//  - Per-stream tx-port / channels / tx-codec / ptime-ms / sample-rate overrides are really used.
//  - a=framecount:N uses the SDP attribute syntax (was "framecount=48") and is computed from ptime/rate.
//  - Announcement logging is debug-only, so the log is not filled every 20 s.

using System;
using System.Collections.Generic;
using System.Net;
using System.Net.Sockets;
using System.Text;
using System.Threading;
using System.Xml;
using FreeSWITCH;
using FreeSWITCH.Native;

/// <summary>
/// SAPAnnouncer - FreeSWITCH managed module plugin.
///
/// Reads the AES67 stream definitions from aes67.conf (via the FreeSWITCH XML configuration API), builds an
/// RFC 4566 SDP per transmitted stream and announces it with RFC 2974 SAP to 239.255.255.255 and
/// 224.2.127.254 (port 9875) every 20 seconds, so AES67 receivers (Dante Controller, the recorder, etc.)
/// can discover the streams.
/// </summary>
public class SAPAnnouncer : IApiPlugin, IAppPlugin, ILoadNotificationPlugin
{
    // -------------------------------------------------------------------------
    // Configuration
    // -------------------------------------------------------------------------

    // PTP grandmaster clock identity used in a=ts-refclk when vars.xml does not set aes67_ptp_gmid.
    // Site value (Bruce, 26 Nov 2026): the Cisco boundary clock 10.8.90.1.
    const string DefaultPtpGmid = "3C-8B-7F-FF-FE-D4-1F-00";

    // Verbose logging of every announcement.
    const bool debugMode = false;

    // SAP announcement interval in milliseconds.
    const int AnnounceIntervalMs = 20000;

    // SAP port (RFC 2974).
    const int SapPort = 9875;

    // AES67 NIC address - read from vars.xml ${multicastIP} on load.
    string multicastIP = "10.8.90.10";


    // -------------------------------------------------------------------------
    // State
    // -------------------------------------------------------------------------

    static Api fsApi = new Api(null);

    // Kept in a static so the timer is not garbage collected.
    static Timer AnnounceTimer;

    UdpClient udpClient;

    // One SAP packet per announced stream (header + SDP), built once at load.
    static List<byte[]> Packets = new List<byte[]>();

    // Guards against double initialisation: mod_managed creates one instance for the App interface and one for
    // the API interface (and Execute() may also call Initialise), but only one may announce. Static = per load.
    static readonly object InitLock = new object();
    static bool initialised = false;


    // =========================================================================
    // Entry points
    // =========================================================================

    public bool Load()
    {
        WriteToLog(LogLevel.Info, "SAPAnnouncer: loading.");
        Initialise();
        return true;
    }

    public void Execute(ApiContext context)
    {
        WriteToLog(LogLevel.Info, "SAPAnnouncer Execute: args='" + context.Arguments + "'");
        Initialise();
        context.Stream.Write(Process(context.Arguments.Split(' ')));
    }

    public void ExecuteBackground(ApiBackgroundContext context)
    {
        WriteToLog(LogLevel.Debug, "SAPAnnouncer ExecuteBackground: args='" + context.Arguments + "'");
    }

    public void Run(FreeSWITCH.AppContext context)
    {
        WriteToLog(LogLevel.Debug, "SAPAnnouncer Run (AppPlugin).");
    }

    public static void Main()
    {
        switch (FreeSWITCH.Script.ContextType)
        {
            case ScriptContextType.Api:
                FreeSWITCH.Script.GetApiContext().Stream.Write("SAPAnnouncer: running as API.");
                break;

            case ScriptContextType.ApiBackground:
                WriteToLog(LogLevel.Debug, "SAPAnnouncer: running as APIBackground.");
                break;

            case ScriptContextType.App:
                WriteToLog(LogLevel.Debug, "SAPAnnouncer: running as App.");
                break;

            case ScriptContextType.None:
                new SAPAnnouncer().Initialise();
                break;
        }
    }

    public string Process(string[] arguments)
    {
        return "SAPAnnouncer: " + Packets.Count + " stream(s) announced every " + (AnnounceIntervalMs / 1000) + " s.";
    }


    // =========================================================================
    // Initialisation
    // =========================================================================

    public void Initialise()
    {
        lock (InitLock)
        {
            if (initialised)
            {
                WriteToLog(LogLevel.Debug, "SAPAnnouncer: already initialised, skipping.");
                return;
            }
            initialised = true;
        }

        multicastIP = fsApi.ExecuteString("eval ${multicastIP}").Trim();
        WriteToLog(LogLevel.Info, "SAPAnnouncer: AES67 interface IP = " + multicastIP);

        string gmid = fsApi.ExecuteString("eval ${aes67_ptp_gmid}").Trim();
        if (gmid.Length == 0 || gmid.Contains("$")) gmid = DefaultPtpGmid;

        string confXml = fsApi.ExecuteString("xml_locate configuration configuration name aes67.conf");
        XmlDocument conf = new XmlDocument();
        try
        {
            conf.LoadXml(confXml);
        }
        catch (Exception ex)
        {
            Log.WriteLine(LogLevel.Critical, "SAPAnnouncer: cannot read aes67.conf - " + ex.Message);
            return;
        }

        BuildPackets(conf, gmid);
        OpenUdpSocket();

        if (Packets.Count > 0)
        {
            AnnounceTimer = new Timer(SendAnnouncements, null, 1000, AnnounceIntervalMs);
            WriteToLog(LogLevel.Info, "SAPAnnouncer: announcing " + Packets.Count + " stream(s), PTP GM " + gmid + ".");
        }
        else
        {
            WriteToLog(LogLevel.Warning, "SAPAnnouncer: no transmit streams found in aes67.conf.");
        }
    }


    // =========================================================================
    // Configuration parsing
    // =========================================================================

    static void ReadParam(XmlNode param, out string name, out string value)
    {
        name = "";
        value = "";
        if (param.Attributes == null) return;
        foreach (XmlAttribute attr in param.Attributes)
        {
            if (attr.Name.ToLower() == "name") name = attr.Value.ToLower();
            if (attr.Name.ToLower() == "value") value = attr.Value;
        }
    }

    static string Pick(string streamValue, string defaultValue)
    {
        return string.IsNullOrEmpty(streamValue) ? defaultValue : streamValue;
    }

    private void BuildPackets(XmlDocument conf, string gmid)
    {
        // Global defaults (<settings>).
        string defaultPort = "5004";
        string defaultChannels = "8";
        string defaultCodec = "L24";
        string defaultPtime = "1";
        string defaultSampleRate = "48000";
        string payloadType = "96";
        string ptpDomain = "0";

        foreach (XmlNode section in conf.DocumentElement.ChildNodes)
        {
            if (section.NodeType != XmlNodeType.Element || section.Name.ToLower() != "settings") continue;
            foreach (XmlNode param in section.ChildNodes)
            {
                if (param.NodeType != XmlNodeType.Element) continue;
                string name, value;
                ReadParam(param, out name, out value);
                switch (name)
                {
                    case "tx-port": defaultPort = value; break;
                    case "channels": defaultChannels = value; break;
                    case "tx-codec": defaultCodec = value; break;
                    case "ptime-ms": defaultPtime = value; break;
                    case "sample-rate": defaultSampleRate = value; break;
                    case "rtp-payload-type": payloadType = value; break;
                    case "ptp-domain": ptpDomain = value; break;
                }
            }
        }

        // One SDP per <stream> that has a tx-address (receive-only streams are not announced).
        int sessionID = 1;
        foreach (XmlNode section in conf.DocumentElement.ChildNodes)
        {
            if (section.NodeType != XmlNodeType.Element || section.Name.ToLower() != "streams") continue;
            foreach (XmlNode stream in section.ChildNodes)
            {
                if (stream.NodeType != XmlNodeType.Element) continue;

                XmlAttribute nameAttr = stream.Attributes == null ? null : stream.Attributes["name"];
                string streamName = nameAttr == null ? "Unnamed" : nameAttr.Value;
                string txAddress = "", txPort = "", txChannels = "", txCodec = "", txPtime = "", txRate = "";

                foreach (XmlNode param in stream.ChildNodes)
                {
                    if (param.NodeType != XmlNodeType.Element) continue;
                    string name, value;
                    ReadParam(param, out name, out value);
                    switch (name)
                    {
                        case "tx-address": txAddress = value.Trim(); break;
                        case "tx-port": txPort = value; break;
                        case "channels": txChannels = value; break;
                        case "tx-codec": txCodec = value; break;
                        case "ptime-ms": txPtime = value; break;
                        case "sample-rate": txRate = value; break;
                    }
                }

                sessionID++; // numbering follows the stream order, as before, so session IDs stay the same

                if (txAddress.Length == 0)
                {
                    WriteToLog(LogLevel.Info, "SAPAnnouncer: stream " + streamName + " has no tx-address (receive only) - not announced.");
                    continue;
                }

                string sdp = BuildSDP(streamName, txAddress, Pick(txPort, defaultPort), Pick(txChannels, defaultChannels),
                                      Pick(txCodec, defaultCodec), Pick(txPtime, defaultPtime), Pick(txRate, defaultSampleRate),
                                      payloadType, ptpDomain, gmid, sessionID);
                WriteToLog(LogLevel.Info, "SAPAnnouncer: SDP for " + streamName + ":\n" + sdp);
                Packets.Add(BuildSapPacket(sdp, sessionID));
            }
        }
    }

    private string BuildSDP(string streamName, string txAddress, string port, string channels, string codec,
        string ptime, string sampleRate, string payloadType, string ptpDomain, string gmid, int sessionID)
    {
        StringBuilder sb = new StringBuilder();
        sb.Append("v=0\r\n");
        sb.Append("o=- " + sessionID + " " + sessionID + " IN IP4 " + multicastIP + "\r\n");
        sb.Append("s=" + streamName + "\r\n");
        sb.Append("c=IN IP4 " + txAddress + "/32\r\n");
        sb.Append("t=0 0\r\n");
        sb.Append("m=audio " + port + " RTP/AVP " + payloadType + "\r\n");

        int channelCount;
        if (int.TryParse(channels, out channelCount) && channelCount > 0)
        {
            StringBuilder labels = new StringBuilder("i=");
            for (int i = 0; i < channelCount; i++)
            {
                if (i > 0) labels.Append(",");
                labels.Append("channel_" + i);
            }
            sb.Append(labels.ToString() + "\r\n");
        }

        sb.Append("a=clock-domain:PTPv2 " + ptpDomain + "\r\n");
        sb.Append("a=ts-refclk:ptp=IEEE1588-2008:" + gmid + ":" + ptpDomain + "\r\n");
        sb.Append("a=mediaclk:direct=0\r\n");
        sb.Append("a=source-filter: incl IN IP4 " + txAddress + " " + multicastIP + "\r\n");
        sb.Append("a=rtpmap:" + payloadType + " " + codec + "/" + sampleRate + "/" + channels + "\r\n");

        double ptimeMs, rate;
        if (double.TryParse(ptime, System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out ptimeMs) &&
            double.TryParse(sampleRate, System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out rate))
        {
            sb.Append("a=framecount:" + (int)Math.Round(ptimeMs * rate / 1000.0) + "\r\n");
        }
        sb.Append("a=ptime:" + ptime + "\r\n");
        return sb.ToString();
    }

    // SAP header (RFC 2974): flags 0x20 (v1, announce, IPv4, no auth/encryption/compression), auth length 0,
    // 16-bit message ID hash, originating source, then "application/sdp\0" and the SDP.
    // The hash must stay the same for as long as the SDP does, so it is the stream's (unique) session ID.
    private byte[] BuildSapPacket(string sdp, int sessionID)
    {
        List<byte> packet = new List<byte>();
        packet.Add(0x20);
        packet.Add(0x00);
        packet.Add((byte)((sessionID >> 8) & 0xFF));
        packet.Add((byte)(sessionID & 0xFF));
        packet.AddRange(IPAddress.Parse(multicastIP).GetAddressBytes());
        packet.AddRange(Encoding.ASCII.GetBytes("application/sdp"));
        packet.Add(0x00);
        packet.AddRange(Encoding.UTF8.GetBytes(sdp));
        return packet.ToArray();
    }


    // =========================================================================
    // UDP socket and sending
    // =========================================================================

    // Bind failures are retried on every announcement cycle: when mod_managed reloads this file it loads the
    // new copy before the old one is unloaded, so the old copy can still hold the port for a moment.
    int bindFailures = 0;

    private void OpenUdpSocket()
    {
        UdpClient client = null;
        try
        {
            client = new UdpClient();
            client.ExclusiveAddressUse = false;
            client.Client.SetSocketOption(SocketOptionLevel.Socket, SocketOptionName.ReuseAddress, true);
            client.Client.Bind(new IPEndPoint(IPAddress.Parse(multicastIP), SapPort));
            udpClient = client;
            WriteToLog(LogLevel.Info, "SAPAnnouncer: UDP socket bound to " + multicastIP + ":" + SapPort +
                       (bindFailures > 0 ? " (after " + bindFailures + " retries)" : ""));
            bindFailures = 0;
        }
        catch (Exception ex)
        {
            if (client != null) client.Close();
            udpClient = null;
            bindFailures++;
            if (bindFailures == 1 || bindFailures % 15 == 0)
                WriteToLog(LogLevel.Warning, "SAPAnnouncer: cannot bind to " + multicastIP + ":" + SapPort +
                           " yet (attempt " + bindFailures + ", retrying every " + (AnnounceIntervalMs / 1000) + " s). Is Rav2SAP running? " + ex.Message);
        }
    }

    internal void SendAnnouncements(object timerState)
    {
        if (udpClient == null) OpenUdpSocket();
        if (udpClient == null) return;

        foreach (byte[] data in Packets)
        {
            try
            {
                udpClient.Send(data, data.Length, "239.255.255.255", SapPort);
                udpClient.Send(data, data.Length, "224.2.127.254", SapPort);
            }
            catch (Exception ex)
            {
                WriteToLog(LogLevel.Error, "SAPAnnouncer: send failed - " + ex.Message);
            }
        }
        WriteToLog(LogLevel.Debug, "SAPAnnouncer: announced " + Packets.Count + " stream(s).");
    }


    // =========================================================================
    // Logging
    // =========================================================================

    public static void WriteToLog(LogLevel logLevel, string logEntry)
    {
        if (logLevel == LogLevel.Debug && !debugMode) return;

        if (fsApi == null)
            Console.WriteLine(logEntry);
        else
            Log.WriteLine(logLevel, logEntry);
    }
}
