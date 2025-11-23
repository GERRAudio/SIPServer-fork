// FreeSwitch.Modules.Tests/SAPAnnouncerTests.cs
using System;
using System.Net;
using System.Text;
using System.Xml;
using FreeSWITCH.Native;
using Moq;
using Xunit;

public class SAPAnnouncerTests
{
    private const string BasicAes67Conf = """
        <configuration name="aes67.conf" description="AES67 Settings">
          <settings>
            <param name="tx-port" value="5004"/>
            <param name="channels" value="2"/>
            <param name="tx-codec" value="L24"/>
            <param name="ptime-ms" value="1"/>
            <param name="sample-rate" value="48000"/>
          </settings>
          <streams>
            <stream name="Studio Main">
              <param name="tx-address" value="239.10.10.10"/>
              <param name="tx-port" value="5006"/>
            </stream>
            <stream name="Backup Feed">
              <param name="tx-address" value="239.10.10.11"/>
            </stream>
          </streams>
        </configuration>
        """;

    [Fact]
    public void LoadConfiguration_Parses_Streams_Correctly()
    {
        var apiMock = new Mock<Api>(null);
        apiMock.Setup(a => a.ExecuteString(It.Is<string>(s => s.Contains("multicastIP"))))
               .Returns("10.8.90.10");
        apiMock.Setup(a => a.ExecuteString(It.Is<string>(s => s.Contains("xml_locate"))))
               .Returns(BasicAes67Conf);

        var announcer = new SAPAnnouncerForTest(apiMock.Object);

        var config = announcer.TestLoadConfiguration();

        Assert.Equal("10.8.90.10", config.MulticastInterfaceIp);
        Assert.Equal(2, config.Streams.Count);
        Assert.Equal("Studio Main", config.Streams[0].Name);
        Assert.Equal("239.10.10.10", config.Streams[0].TxAddress);
        Assert.Equal("5006", config.Streams[0].TxPort);
        Assert.Equal("239.10.10.11", config.Streams[1].TxAddress);
        Assert.Equal("5004", config.Streams[1].TxPort); // fallback
    }

    [Fact]
    public void BuildSdp_Generates_Valid_SDP()
    {
        var stream = new Aes67Stream("Test Stream", "239.1.1.1", "5004", 2, "L24", 48000, 1.0);
        var config = new SapConfig("10.0.0.1", "10.0.0.1", new[] { stream });

        var announcer = new SAPAnnouncerForTest(null!);
        var sdp = announcer.TestBuildSdp(stream, config, 12345);

        Assert.Contains("s=Test Stream", sdp);
        Assert.Contains("c=IN IP4 239.1.1.1/32", sdp);
        Assert.Contains("a=rtpmap:96 L24/48000/2", sdp);
        Assert.Contains("a=ptime:0.001", sdp);
        Assert.Contains("a=source-filter: incl IN IP4 239.1.1.1 10.0.0.1", sdp);
    }

    [Fact]
    public void BuildSapPayload_Has_Correct_Header()
    {
        var announcer = new SAPAnnouncerForTest(null!);
        announcer.SetSourceIp("192.168.1.100");
        announcer.SetSdps(new[] { "v=0\r\no=- 1 1 IN IP4 192.168.1.100\r\ns=Test" });

        var payload = announcer.TestBuildSapPayload(0x1234);

        Assert.Equal(0x20, payload[0]); // Version 1, no auth
        Assert.Equal(0x00, payload[1]);
        Assert.Equal(0x34, payload[2]); // message ID low
        Assert.Equal(0x12, payload[3]); // message ID high
        Assert.Equal("192.168.1.100", new IPAddress(new Span<byte>(payload, 4, 4)).ToString());
        Assert.Contains(Encoding.ASCII.GetBytes("application/sdp\0"), payload);
    }
}

// Helper class to expose protected methods for testing
public class SAPAnnouncerForTest : SAPAnnouncer
{
    public SAPAnnouncerForTest(Api api) : base() 
    {
        var field = typeof(SAPAnnouncer).GetField("_api", System.Reflection.BindingFlags.NonPublic | System.Reflection.BindingFlags.Instance);
        field?.SetValue(this, api);
    }

    public SapConfig TestLoadConfiguration() => LoadConfiguration();
    public string TestBuildSdp(Aes67Stream stream, SapConfig config, int sessionId) 
        => BuildSdp(stream, config, sessionId);
    public byte[] TestBuildSapPayload(ushort id) => BuildSapPayload(id);
    public void SetSourceIp(string ip) => _sourceIp = ip;
    public void SetSdps(string[] sdps) => _sdps = sdps.ToImmutableArray();
}