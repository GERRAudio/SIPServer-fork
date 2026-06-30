using System;
using System.Collections.Generic;
using System.Net;
using System.Net.Sockets;
using System.Text;
using System.Threading;
using FreeSWITCH;
using FreeSWITCH.Native;

/// <summary>
/// SAPAnnouncer — FreeSWITCH managed module plugin.
///
/// Reads AES67 stream definitions from aes67.conf (via the FreeSWITCH XML
/// configuration API), builds RFC 2974 Session Announcement Protocol (SAP)
/// packets containing SDP stream descriptions, and broadcasts them via UDP
/// to the standard SAP multicast groups every 20 seconds.
///
/// This allows AES67-capable receivers (Dante Controller, etc.) to discover
/// the FreeSWITCH AES67 streams automatically without manual SDP import.
/// </summary>
public class SAPAnnouncer : IApiPlugin, IAppPlugin, ILoadNotificationPlugin
{
    // -------------------------------------------------------------------------
    // Configuration
    // -------------------------------------------------------------------------

    // Multicast IP used for AES67 RTP streams — overridden from vars.xml on load.
    string multicastIP = "10.8.90.10";

    // Enable verbose debug logging.
    const bool debugMode = true;

    // SAP announcement interval in milliseconds.
    const int AnnounceIntervalMs = 20000;

    // SAP port (RFC 2974 §3).
    const int SapPort = 9875;


    // -------------------------------------------------------------------------
    // State
    // -------------------------------------------------------------------------

    static Api fsApi = new Api(null);

    // Timer that fires SendAnnouncements periodically.
    static Timer AnnounceTimer;

    // UDP socket bound to the AES67 NIC, used for SAP multicast sends.
    UdpClient udpClient;

    // SDP strings built from aes67.conf, one per stream.
    List<string> SDPs = new List<string>();

    // SAP message ID counter. Wraps at 32766 per RFC 2974.
    int sapCounter = 0;

    // Guards against double-initialisation if both Load() and Execute() are called.
    bool initialised = false;


    // =========================================================================
    // ILoadNotificationPlugin — called once when the module is loaded
    // =========================================================================

    public bool Load()
    {
        WriteToLog(LogLevel.Info, "SAPAnnouncer: loading.");
        Initialise();
        return true;
    }


    // =========================================================================
    // IApiPlugin / IAppPlugin entry points
    // =========================================================================

    /// <summary>
    /// Handles synchronous API calls. Initialises if not already done
    /// (e.g. when invoked directly rather than via Load).
    /// </summary>
    public void Execute(ApiContext context)
    {
        WriteToLog(LogLevel.Info,
            $"SAPAnnouncer Execute: args='{context.Arguments}' event={context.Event?.GetEventType() ?? "<none>"}");
        Initialise();
        context.Stream.Write(Process(context.Arguments.Split(' ')));
    }

    /// <summary>Handles background API calls — no action required.</summary>
    public void ExecuteBackground(ApiBackgroundContext context)
    {
        WriteToLog(LogLevel.Info,
            $"SAPAnnouncer ExecuteBackground: thread #{Thread.CurrentThread.ManagedThreadId} args='{context.Arguments}'");
    }

    /// <summary>Handles dialplan application calls — no action required.</summary>
    public void Run(FreeSWITCH.AppContext context)
    {
        WriteToLog(LogLevel.Info, "SAPAnnouncer Run (AppPlugin).");
    }

    /// <summary>
    /// Script entry point — used when the module is invoked as a standalone
    /// script rather than as a loaded plugin.
    /// </summary>
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
                // Direct script invocation — initialise and run standalone.
                new SAPAnnouncer().Initialise();
                break;
        }
    }


    // =========================================================================
    // Command dispatcher
    // =========================================================================

    /// <summary>
    /// Routes incoming commands. Currently a placeholder — extend as needed.
    /// </summary>
    public string Process(string[] arguments)
    {
        WriteToLog(LogLevel.Debug, "SAPAnnouncer Process: " + arguments[0]);
        return "No command handled.";
    }


    // =========================================================================
    // Initialisation — idempotent, safe to call from both Load() and Execute()
    // =========================================================================

    /// <summary>
    /// Reads the AES67 configuration, builds SDP strings for each stream, opens
    /// the UDP socket, and starts the periodic SAP announcement timer.
    /// Safe to call multiple times — only runs on the first call.
    /// </summary>
    public void Initialise()
    {
        if (initialised)
        {
            WriteToLog(LogLevel.Info, "SAPAnnouncer: already initialised, skipping.");
            return;
        }
        initialised = true;

        // Read the multicast IP from FreeSWITCH vars.xml.
        multicastIP = fsApi.ExecuteString("eval ${multicastIP}");
        WriteToLog(LogLevel.Info, "SAPAnnouncer: multicast IP = " + multicastIP);

        // Load aes67.conf via the FreeSWITCH XML configuration API.
        string confXml = fsApi.ExecuteString(
            "xml_locate configuration configuration name aes67.conf");
        WriteToLog(LogLevel.Info, "SAPAnnouncer: aes67.conf loaded.");

        var conf = new System.Xml.XmlDocument();
        conf.LoadXml(confXml);

        BuildSDPs(conf);
        OpenUdpSocket();

        if (SDPs.Count > 0)
        {
            // First announcement after 1 second, then every AnnounceIntervalMs.
            AnnounceTimer = new Timer(SendAnnouncements, null, 1000, AnnounceIntervalMs);
            WriteToLog(LogLevel.Info,
                $"SAPAnnouncer: timer started. {SDPs.Count} stream(s) will be announced.");
        }
        else
        {
            WriteToLog(LogLevel.Warning, "SAPAnnouncer: no streams found in aes67.conf.");
        }
    }


    // =========================================================================
    // Configuration parsing
    // =========================================================================

    /// <summary>
    /// Parses aes67.conf, reads global defaults from the &lt;settings&gt; section,
    /// then builds one SDP string per stream in the &lt;streams&gt; section.
    /// Per-stream values override the global defaults where present.
    /// </summary>
    private void BuildSDPs(System.Xml.XmlDocument conf)
    {
        // Global defaults from the <settings> section.
        string defaultPort       = "5004";
        string defaultChannels   = "";
        string defaultCodec      = "";
        string defaultPtime      = "";
        string defaultSampleRate = "";
        string rtpNic            = multicastIP;

        int sessionID = 1;

        foreach (System.Xml.XmlNode section in conf.DocumentElement.ChildNodes)
        {
            if (section.NodeType != System.Xml.XmlNodeType.Element) continue;

            // -----------------------------------------------------------------
            // <settings> — global transmission defaults
            // -----------------------------------------------------------------
            if (section.Name.ToLower() == "settings")
            {
                foreach (System.Xml.XmlNode param in section.ChildNodes)
                {
                    if (param.NodeType != System.Xml.XmlNodeType.Element) continue;

                    string name = "", value = "";
                    foreach (System.Xml.XmlAttribute attr in param.Attributes)
                    {
                        if (attr.Name.ToLower() == "name")  name  = attr.Value;
                        if (attr.Name.ToLower() == "value") value = attr.Value;
                    }

                    switch (name.ToLower())
                    {
                        case "tx-port":        defaultPort       = value; break;
                        case "channels":       defaultChannels   = value; break;
                        case "tx-codec":       defaultCodec      = value; break;
                        case "ptime-ms":       defaultPtime      = value; break;
                        case "sample-rate":    defaultSampleRate = value; break;
                        case "rtp-iface":
                        case "rtp-interface":  rtpNic            = value; break;
                    }
                }
            }

            // -----------------------------------------------------------------
            // <streams> — one SDP per <stream> child
            // -----------------------------------------------------------------
            if (section.Name.ToLower() == "streams")
            {
                foreach (System.Xml.XmlNode stream in section.ChildNodes)
                {
                    if (stream.NodeType != System.Xml.XmlNodeType.Element) continue;

                    // Per-stream overrides (empty = use global default).
                    string txAddress    = "";
                    string txPort       = "";
                    string txChannels   = "";
                    string txCodec      = "";
                    string txPtime      = "";
                    string txSampleRate = "";
                    string streamName   = stream.Attributes["name"]?.InnerText ?? "Unnamed";

                    WriteToLog(LogLevel.Info, "SAPAnnouncer: building SDP for stream: " + streamName);

                    foreach (System.Xml.XmlNode param in stream.ChildNodes)
                    {
                        if (param.NodeType != System.Xml.XmlNodeType.Element) continue;

                        string name = "", value = "";
                        foreach (System.Xml.XmlAttribute attr in param.Attributes)
                        {
                            if (attr.Name.ToLower() == "name")  name  = attr.Value;
                            if (attr.Name.ToLower() == "value") value = attr.Value;
                        }

                        switch (name.ToLower())
                        {
                            case "tx-address":   txAddress    = value; break;
                            case "tx-port":      txPort       = value; break;
                            case "channels":     txChannels   = value; break;
                            case "tx-codec":     txCodec      = value; break;
                            case "ptime-ms":     txPtime      = value; break;
                            case "sample-rate":  txSampleRate = value; break;
                        }
                    }

                    // Fall back to global defaults for any unset per-stream values.
                    string sdpPort       = string.IsNullOrEmpty(txPort)       ? defaultPort       : txPort;
                    string sdpChannels   = string.IsNullOrEmpty(txChannels)   ? defaultChannels   : txChannels;
                    string sdpCodec      = string.IsNullOrEmpty(txCodec)      ? defaultCodec      : txCodec;
                    string sdpPtime      = string.IsNullOrEmpty(txPtime)      ? defaultPtime      : txPtime;
                    string sdpSampleRate = string.IsNullOrEmpty(txSampleRate) ? defaultSampleRate : txSampleRate;

                    sessionID++;
                    string sdp = BuildSDP(streamName, txAddress, sdpPort, sdpChannels,
                                          sdpCodec, sdpPtime, sdpSampleRate, sessionID);

                    WriteToLog(LogLevel.Info, "SAPAnnouncer: SDP built:\n" + sdp);
                    SDPs.Add(sdp);
                }
            }
        }
    }

    /// <summary>
    /// Constructs a single RFC 4566 SDP string for one AES67 stream.
    /// The SDP includes AES67-mandatory PTP clock attributes.
    /// </summary>
    private string BuildSDP(string streamName, string txAddress, string port,
        string channels, string codec, string ptime, string sampleRate, int sessionID)
    {
        var sb = new StringBuilder();

        sb.AppendLine("v=0");
        // o= origin: username sessionID version network-type addr-type address
        sb.AppendLine($"o=- {sessionID} {sessionID} IN IP4 {multicastIP}");
        sb.AppendLine("s=" + streamName);
        // c= connection: network-type addr-type address/ttl
        sb.AppendLine($"c=IN IP4 {txAddress}/32");
        sb.AppendLine("t=0 0");
        sb.AppendLine($"m=audio {port} RTP/AVP 96");

        // Channel information line — lists each channel by index.
        if (int.TryParse(channels, out int channelCount) && channelCount > 0)
        {
            var channelLabel = new StringBuilder("i=");
            for (int i = 0; i < channelCount; i++)
            {
                if (i > 0) channelLabel.Append(",");
                channelLabel.Append("channel_" + i);
            }
            sb.AppendLine(channelLabel.ToString());
        }

        // AES67-mandatory PTP clock domain and reference clock attributes.
        sb.AppendLine("a=clock-domain:PTPv2 0");
        sb.AppendLine("a=ts-refclk:ptp=IEEE1588-2008:00-00-00-00-00-00-00-00:0");
        sb.AppendLine("a=mediaclk:direct=0");
        // Source filter — restricts reception to the declared source address.
        sb.AppendLine($"a=source-filter: incl IN IP4 {txAddress} {multicastIP}");
        // RTP payload type 96 dynamic mapping.
        sb.AppendLine($"a=rtpmap:96 {codec}/{sampleRate}/{channels}");
        // AES67 standard frame count for 1ms ptime at 48kHz.
        sb.AppendLine("a=framecount=48");
        sb.AppendLine("a=ptime:" + ptime);

        return sb.ToString();
    }


    // =========================================================================
    // UDP socket
    // =========================================================================

    /// <summary>
    /// Opens and binds the UDP socket to the AES67 network interface.
    /// Logs a critical error and leaves udpClient null if binding fails
    /// (e.g. port already taken by Rav2SAP or another SAP announcer).
    /// </summary>
    private void OpenUdpSocket()
    {
        try
        {
            udpClient = new UdpClient();
            udpClient.ExclusiveAddressUse = false;
            udpClient.Client.Bind(new IPEndPoint(IPAddress.Parse(multicastIP), SapPort));
            WriteToLog(LogLevel.Info,
                $"SAPAnnouncer: UDP socket bound to {multicastIP}:{SapPort}");
        }
        catch (Exception ex)
        {
            string msg = $"SAPAnnouncer: cannot bind to {multicastIP}:{SapPort}. " +
                         $"Is Rav2SAP running? {ex.Message}";
            Log.WriteLine(LogLevel.Critical, msg);
            udpClient = null;
        }
    }


    // =========================================================================
    // SAP announcement broadcast
    // =========================================================================

    /// <summary>
    /// Timer callback — broadcasts a SAP/SDP packet for each configured stream
    /// to the two standard SAP multicast groups:
    ///   239.255.255.255 (local SAP group, RFC 2974 §4)
    ///   224.2.127.254   (global SAP group, RFC 2974 §4)
    ///
    /// SAP packet structure (RFC 2974 §5):
    ///   Byte 0:    flags = 0x20 (SAP v1, announce, no encryption, no compression)
    ///   Byte 1:    auth length = 0
    ///   Bytes 2-3: message ID hash (little-endian 16-bit counter)
    ///   Bytes 4-7: originating source IPv4 address
    ///   Bytes 8+:  MIME type ("application/sdp") + NUL + SDP payload
    /// </summary>
    internal void SendAnnouncements(object timerState)
    {
        if (udpClient == null)
        {
            WriteToLog(LogLevel.Warning, "SAPAnnouncer: UDP socket not available — skipping announcement.");
            return;
        }

        byte[] originAddress = IPAddress.Parse(multicastIP).GetAddressBytes();
        byte[] mimeType      = Encoding.UTF8.GetBytes("application/sdp");

        foreach (string sdp in SDPs)
        {
            // Build SAP header.
            var packet = new List<byte>
            {
                0x20,                          // flags: SAP v1 announce
                0x00,                          // auth length: none
                (byte)(sapCounter & 0xFF),     // message ID low byte
                (byte)((sapCounter >> 8) & 0xFF) // message ID high byte
            };

            // Wrap counter at 32766 per RFC 2974.
            if (sapCounter >= 32766) sapCounter = 0;
            else sapCounter++;

            packet.AddRange(originAddress);    // source IPv4 (4 bytes)
            packet.AddRange(mimeType);         // MIME type string
            packet.Add(0x00);                  // NUL terminator after MIME type
            packet.AddRange(Encoding.UTF8.GetBytes(sdp)); // SDP payload

            byte[] data = packet.ToArray();

            try
            {
                udpClient.Send(data, data.Length, "239.255.255.255", SapPort);
                udpClient.Send(data, data.Length, "224.2.127.254",   SapPort);
                WriteToLog(LogLevel.Info, "SAPAnnouncer: announced — " + sdp);
            }
            catch (Exception ex)
            {
                WriteToLog(LogLevel.Error, "SAPAnnouncer: send failed — " + ex.Message);
            }
        }
    }


    // =========================================================================
    // Logging
    // =========================================================================

    /// <summary>
    /// Writes to the FreeSWITCH log, or falls back to the console if the API
    /// handle is not yet available (e.g. during standalone script execution).
    /// Debug-level entries are suppressed unless debugMode is true.
    /// </summary>
    public static void WriteToLog(LogLevel logLevel, string logEntry)
    {
        if (logLevel == LogLevel.Debug && !debugMode) return;

        if (fsApi == null)
            Console.WriteLine(logEntry);
        else
            Log.WriteLine(logLevel, logEntry);
    }
}
