// CommunicatorConnector.csx
// AES67/PortAudio ↔ Conference bridge for Eclipse HX integration

using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.Linq;
using System.Net.Http;
using System.Threading;
using System.Threading.Tasks;
using System.Xml;
using FreeSWITCH;
using FreeSWITCH.Native;

public sealed class CommunicatorConnector : IApiPlugin, IAppPlugin, ILoadNotificationPlugin
{
    private const string Module = "aes67";
    private const string ApiBase = "http://localhost/api/";
    private const bool Debug = false;

    private readonly Api _api = new(null);
    private readonly HttpClient _http = new();
    private readonly Timer _startup = new(5_000) { AutoReset = false };
    private readonly Timer _sync = new(10_000) { AutoReset = true };

    private int MaxWireless => GetVarInt("MaxWirelessConf", 24);
    private int MaxWired => GetVarInt("MaxWiredConf", 99);

    private readonly ConcurrentDictionary<string, Guid> _eavesdrops = new();
    private readonly ConcurrentBag<string> _protectedUuids = new();
    private readonly object _lock = new();

    public bool Load()
    {
        _startup.Elapsed += (_, _) => Task.Run(StartPermanentChannels);
        _startup.Start();
        _sync.Elapsed += (_, _) => Task.Run(SyncChannelsAndConferences);
        _sync.Start();

        Log.Info("CommunicatorConnector loaded.");
        return true;
    }

    private async Task StartPermanentChannels()
    {
        await OriginatePermanent("Announce", "99991");
        await OriginatePermanent("BeltpackMessages", "&endless_playback(theoperatorhasbeencalledbeep48k.wav)");
    }

    private async Task OriginatePermanent(string endpoint, string dest)
    {
        try
        {
            var uuid = await Originate($"{Module}/endpoints/{endpoint}", dest);
            Log.Notice($"Permanent channel {endpoint} → {dest} ({uuid})");
        }
        catch (Exception ex)
        {
            Log.Error($"Failed to start permanent channel {endpoint}: {ex.Message}");
        }
    }

    private async Task<Guid> Originate(string endpoint, string destination, int maxTries = 6)
    {
        for (int i = 0; i < maxTries; i++)
        {
            var uuid = _api.ExecuteString("create_uuid");
            _api.Execute("bgapi", $"originate {{origination_uuid={uuid}}} {endpoint} {destination}");

            if (await WaitForState(uuid, "CS_EXECUTE", 12))
                return Guid.Parse(uuid);

            await Task.Delay(1500);
        }
        throw new Exception($"Failed to originate {endpoint}");
    }

    private Task<bool> WaitForState(string uuid, string state, int seconds)
    {
        return Task.Run(async () =>
        {
            for (int i = 0; i < seconds * 2; i++)
            {
                var s = _api.ExecuteString($"eval uuid:{uuid} ${{channel-state}}").Trim();
                if (s.Equals(state, StringComparison.OrdinalIgnoreCase))
                    return true;
                await Task.Delay(500);
            }
            return false;
        });
    }

    public void Execute(ApiContext ctx)
    {
        var args = (ctx.Arguments ?? "").Split(' ', StringSplitOptions.RemoveEmptyEntries);
        if (args.Length == 0) { ctx.Stream.Write("-ERR no args"); return; }

        var result = args[0].ToLower() switch
        {
            "join" when args.Length >= 2 => Task.Run(() => HandleJoin(args[1])).Result,
            "leave" when args.Length >= 2 => Task.Run(() => HandleLeave(args[1])).Result,
            "refresh" => RefreshConfig(),
            "status" => GetStatus(),
            _ => "-ERR unknown command"
        };

        ctx.Stream.Write(result);
    }

    private string HandleJoin(string conf)
    {
        if (IsWiredConference(conf))
        {
            _api.ExecuteString($"conference {conf} dial {Module}/endpoints/Announce");
            return "+OK Announce added";
        }

        var port = GetFreeEndpoint();
        if (port == null) return "-ERR no free ports";

        Task.Run(async () =>
        {
            var uuid = Guid.NewGuid();
            lock (_lock) _protectedUuids.Add(uuid.ToString());

            await Originate($"{Module}/endpoints/{port}", $"99999{conf}");

            await NotifyGui(conf, port, "TalkAndListen");
            lock (_lock) _protectedUuids.Remove(uuid.ToString());
        });

        return $"+OK {port}";
    }

    private string HandleLeave(string conf)
    {
        // Simplified — real version removes specific port
        return "+OK";
    }

    private string GetFreeEndpoint()
    {
        var all = GetAllEndpoints();
        var used = GetUsedEndpoints();
        return all.Except(used).FirstOrDefault(f => f.StartsWith("Phone_"));
    }

    private HashSet<string> GetUsedEndpoints()
    {
        var xml = _api.ExecuteString("show channels as xml");
        var doc = new XmlDocument();
        doc.LoadXml(xml);
        var set = new HashSet<string>();

        foreach (XmlNode node in doc.SelectNodes("//row")!)
        {
            var name = node.SelectSingleNode("name")?.InnerText;
            if (name?.Contains($"{Module}/endpoint-") == true)
            {
                var endpoint = name["endpoint-".Length..];
                if (endpoint.StartsWith("Phone_")) set.Add(endpoint);
            }
        }
        return set;
    }

    private List<string> GetAllEndpoints()
    {
        var lines = _api.ExecuteString($"{Module} endpoints").Split('\n');
        return lines.Where(l => l.Trim().Length > 0 && l.Contains("Phone_"))
                    .Select(l => l.Split(' ', StringSplitOptions.RemoveEmptyEntries).Last())
                    .Where(s => s.StartsWith("Phone_"))
                    .ToList();
    }

    private bool IsWiredConference(string conf) => int.TryParse(conf, out var n) && n > MaxWireless;

    private Task NotifyGui(string conf, string port, string mode)
    {
        var url = $"{ApiBase}conferencedata/CONF.{conf}/{port}/{mode}/sipservernewchannel";
        return _http.GetAsync(url).ContinueWith(_ => { }, TaskContinuationOptions.OnlyOnFaulted);
    }

    private void SyncChannelsAndConferences()
    {
        // Lightweight orphan cleanup — full version in original
        Log.Debug("Sync check running...");
    }

    private int GetVarInt(string var, int @default)
    {
        var val = _api.ExecuteString($"eval ${{{var}}}");
        return int.TryParse(val, out var i) ? i : @default;
    }

    private string RefreshConfig() => $"MaxWireless={MaxWireless}, MaxWired={MaxWired}";
    private string GetStatus() => $"Active eavesdrops: {_eavesdrops.Count}, Protected: {_protectedUuids.Count}";

    public void ExecuteBackground(ApiBackgroundContext ctx) { }
    public void Run(AppContext ctx) { }

    private static class Log
    {
        public static void Info(string msg) => Write(LogLevel.Info, msg);
        public static void Notice(string msg) => Write(LogLevel.Notice, msg);
        public static void Error(string msg) => Write(LogLevel.Error, msg);
        public static void Debug(string msg) { if (Debug) Write(LogLevel.Debug, msg); }
        private static void Write(LogLevel level, string msg) => Log.WriteLine(level, $"[Communicator] {msg}");
    }

    public static void Main() { }
}