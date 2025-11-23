// SAPAnnouncer.csx
// SAP/SESIP announcer for AES67 streams in FreeSWITCH
// Supports aes67.conf configuration, dynamic multicast interface, proper SAP v1 headers

using System;
using System.Collections.Generic;
using System.Collections.Immutable;
using System.Net;
using System.Net.Sockets;
using System.Text;
using System.Threading;
using System.Xml;
using FreeSWITCH;
using FreeSWITCH.Native;

public record Aes67Stream(
    string Name,
    string TxAddress,
    string TxPort,
    int Channels,
    string Codec,
    int SampleRate,
    double PtimeMs);

public record SapConfig(
    string MulticastInterfaceIp,
    string SourceIp,
    IReadOnlyList<Aes67Stream> Streams);

public sealed class SAPAnnouncer : IApiPlugin, IAppPlugin, ILoadNotificationPlugin, IDisposable
{
    private const int SapPort = 9875;
    private const int AnnouncementIntervalMs = 20_000;
    private const int InitialDelayMs = 1_000;
    private const bool DebugLogging = false;

    private readonly Api _api = new(null);
    private UdpClient? _udpClient;
    private Timer? _timer;
    private volatile int _messageId;
    private ImmutableArray<string> _sdps = ImmutableArray<string>.Empty;
    private string _sourceIp = "10.8.90.10";

    public void Execute(ApiContext context) => Start();
    public void ExecuteBackground(ApiBackgroundContext context) => Start();
    public void Run(AppContext context) => Start();
    public bool Load() { Start(); return true; }

    private void Start()
    {
        if (_timer != null) return;

        try
        {
            var config = LoadConfiguration();
            InitializeUdpClient(config.MulticastInterfaceIp);
            BuildSdps(config);
            StartTimer();
            Log(LogLevel.Notice, "SAP Announcer initialized and running.");
        }
        catch (Exception ex)
        {
            Log(LogLevel.Critical, $"Failed to start SAPAnnouncer: {ex.Message}\n{ex.StackTrace}");
        }
    }

    private SapConfig LoadConfiguration()
    {
        _sourceIp = _api.ExecuteString("eval ${multicastIP}".Trim());
        if (string.IsNullOrWhiteSpace(_sourceIp) || _sourceIp.Contains("${"))
            _sourceIp = "10.8.90.10";

        var xmlText = _api.ExecuteString("xml_locate configuration configuration name aes67.conf");
        if (string.IsNullOrWhiteSpace(xmlText))
            throw new Exception("Could not load aes67.conf");

        var doc = new XmlDocument();
        doc.LoadXml(xmlText);

        var defaults = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        var streams = new List<Aes67Stream>();

        foreach (XmlNode section in doc.DocumentElement!.ChildNodes)
        {
            if (section.NodeType != XmlNodeType.Element) continue;

            if (section.Name.Equals("settings", StringComparison.OrdinalIgnoreCase))
            {
                foreach (XmlNode n in section.ChildNodes)
                {
                    if (n.NodeType != XmlNodeType.Element) continue;
                    var name = n.Attributes?["name"]?.Value;
                    var value = n.Attributes?["value"]?.Value;
                    if (name != null && value != null)
                        defaults[name] = value;
                }
            }
            else if (section.Name.Equals("streams", StringComparison.OrdinalIgnoreCase))
            {
                foreach (XmlNode streamNode in section.ChildNodes)
                {
                    if (streamNode.NodeType != XmlNodeType.Element) continue;

                    var name = streamNode.Attributes?["name"]?.Value ?? "Unnamed Stream";
                    var attrs = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);

                    foreach (XmlNode param in streamNode.ChildNodes)
                    {
                        if (param.NodeType != XmlNodeType.Element) continue;
                        var k = param.Attributes?["name"]?.Value;
                        var v = param.Attributes?["value"]?.Value;
                        if (k != null && v != null) attrs[k] = v;
                    }

                    streams.Add(new Aes67Stream(
                        Name: name,
                        TxAddress: Get(attrs, "tx-address", _sourceIp),
                        TxPort: Get(attrs, "tx-port", defaults.GetValueOrDefault("tx-port", "5004")),
                        Channels: int.TryParse(Get(attrs, "channels", defaults.GetValueOrDefault("channels", "2")), out var c) ? c : 2,
                        Codec: Get(attrs, "tx-codec", defaults.GetValueOrDefault("tx-codec", "L24")),
                        SampleRate: int.TryParse(Get(attrs, "sample-rate", defaults.GetValueOrDefault("sample-rate", "48000")), out var sr) ? sr : 48000,
                        PtimeMs: double.TryParse(Get(attrs, "ptime-ms", defaults.GetValueOrDefault("ptime-ms", "1")), out var pt) ? pt : 1.0
                    ));
                }
            }
        }

        return new SapConfig(_sourceIp, _sourceIp, streams);
    }

    private static string Get(Dictionary<string, string> dict, string key, string fallback)
        => dict.TryGetValue(key, out var v) && !string.IsNullOrWhiteSpace(v) ? v : fallback;

    private void InitializeUdpClient(string bindIp)
    {
        _udpClient = new UdpClient();
        _udpClient.ExclusiveAddressUse = false;
        try
        {
            _udpClient.Client.Bind(new IPEndPoint(IPAddress.Parse(bindIp), SapPort));
            Log(LogLevel.Info, $"SAP bound to {bindIp}:{SapPort}");
        }
        catch (Exception ex)
        {
            Log(LogLevel.Critical, $"Cannot bind to {bindIp}:{SapPort} — Is Rav2SAP or another announcer running? {ex.Message}");
            throw;
        }
    }

    private void BuildSdps(SapConfig config)
    {
        var sdps = new List<string>();
        int sessionId = Environment.TickCount;

        foreach (var stream in config.Streams)
        {
            sessionId++;
            var sdp = new StringBuilder();
            sdp.AppendLine("v=0");
            sdp.AppendLine($"o=- {sessionId} {sessionId} IN IP4 {config.SourceIp}");
            sdp.AppendLine($"s={stream.Name}");
            sdp.AppendLine($"c=IN IP4 {stream.TxAddress}/32");
            sdp.AppendLine("t=0 0");
            sdp.AppendLine($"m=audio {stream.TxPort} RTP/AVP 96");
            sdp.AppendLine($"a=rtpmap:96 {stream.Codec}/{stream.SampleRate}/{stream.Channels}");
            sdp.AppendLine($"a=ptime:{stream.PtimeMs / 1000:F3}");
            sdp.AppendLine("a=framecount:48");
            sdp.AppendLine("a=ts-refclk:ptp=IEEE1588-2008:00-00-00-00-00-00-00-00:0");
            sdp.AppendLine("a=mediaclk:direct=0");
            sdp.AppendLine("a=clock-domain:PTPv2 0");
            sdp.AppendLine($"a=source-filter: incl IN IP4 {stream.TxAddress} {config.SourceIp}");
            sdps.Add(sdp.ToString());
        }

        _sdps = sdps.ToImmutableArray();
        Log(LogLevel.Notice, $"Built {_sdps.Length} SDP announcements.");
    }

    private void StartTimer()
    {
        _timer = new Timer(_ => Broadcast(), null, InitialDelayMs, AnnouncementIntervalMs);
    }

    private void Broadcast()
    {
        if (_sdps.IsEmpty || _udpClient == null) return;

        var messageId = (ushort)Interlocked.Increment(ref _messageId);
        var payload = BuildSapPayload(messageId);

        try
        {
            _udpClient.Send(payload, payload.Length, "239.255.255.255", SapPort);
            _udpClient.Send(payload, payload.Length, "224.2.127.254", SapPort);
        }
        catch (Exception ex)
        {
            Log(LogLevel.Error, $"SAP send failed: {ex.Message}");
        }
    }

    private byte[] BuildSapPayload(ushort messageId)
    {
        var packet = new List<byte> { 0x20, 0x00 }; // v1, no auth
        packet.AddRange(BitConverter.GetBytes(messageId));
        packet.AddRange(IPAddress.Parse(_sourceIp).GetAddressBytes());
        packet.AddRange(Encoding.ASCII.GetBytes("application/sdp\0"));

        foreach (var sdp in _sdps)
            packet.AddRange(Encoding.UTF8.GetBytes(sdp));

        return packet.ToArray();
    }

    private static void Log(LogLevel level, string message)
    {
        if (level == LogLevel.Debug && !DebugLogging) return;
        Log.WriteLine(level, $"[SAPAnnouncer] {message}");
    }

    public void Dispose()
    {
        _timer?.Dispose();
        _udpClient?.Close();
    }

    public static void Main() { }
}