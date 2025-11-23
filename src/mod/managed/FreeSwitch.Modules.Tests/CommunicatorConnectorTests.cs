// FreeSwitch.Modules.Tests/CommunicatorConnectorTests.cs
using System;
using System.Threading.Tasks;
//using FreeSWITCH.Native;
using Moq;
using Xunit;

public class CommunicatorConnectorTests
{
    [Theory]
    [InlineData("9001", 24, false)] // wireless
    [InlineData("9025", 24, true)]  // wired
    [InlineData("9050", 99, false)]
    public void IsWiredConference_Correctly_Classifies(string conf, int maxWireless, bool expected)
    {
        var mock = new Mock<CommunicatorConnectorForTest>(null!);
        mock.Setup(m => m.GetVarInt("MaxWirelessConf", It.IsAny<int>())).Returns(maxWireless);

        var result = mock.Object.TestIsWiredConference(conf);

        Assert.Equal(expected, result);
    }

    [Fact]
    public async Task Originate_Waits_For_CS_EXECUTE_State()
    {
        var apiMock = new Mock<Api>(null);
        var uuid = "12345678-1234-5678-1234-567812345678";

        apiMock.SetupSequence(a => a.ExecuteString(It.IsAny<string>()))
               .Returns(uuid)
               .Returns("CS_INIT")
               .Returns("CS_ROUTING")
               .Returns("CS_EXECUTE");

        var connector = new CommunicatorConnectorForTest(apiMock.Object);

        var result = await connector.TestOriginate("aes67/endpoints/Test", "99991");

        Assert.Equal(Guid.Parse(uuid), result);
    }

    [Fact]
    public void GetFreeEndpoint_Returns_Unused_Phone()
    {
        var apiMock = new Mock<Api>(null);
        apiMock.Setup(a => a.ExecuteString("aes67 endpoints"))
               .Returns("1: Phone_01 ready\n2: Phone_02 ready\n3: Announce ready");
        apiMock.Setup(a => a.ExecuteString("show channels as xml"))
               .Returns(CreateChannelsXml("Phone_01")); // Phone_02 is free

        var connector = new CommunicatorConnectorForTest(apiMock.Object);

        var free = connector.TestGetFreeEndpoint();

        Assert.Equal("Phone_02", free);
    }

    private static string CreateChannelsXml(params string[] usedEndpoints)
    {
        var rows = string.Join("", usedEndpoints.Select(e => 
            $"<row><name>aes67/endpoint-{e}</name><uuid>1111-1111</uuid></row>"));
        return $"<result><rows>{rows}</rows></result>";
    }
}

public class CommunicatorConnectorForTest : CommunicatorConnector
{
    public CommunicatorConnectorForTest(Api api) : base()
    {
        var field = typeof(CommunicatorConnector).GetField("_api", System.Reflection.BindingFlags.NonPublic | System.Reflection.BindingFlags.Instance);
        field?.SetValue(this, api);
    }

    public bool TestIsWiredConference(string conf) => IsWiredConference(conf);
    public Task<Guid> TestOriginate(string endpoint, string dest) => Originate(endpoint, dest);
    public string TestGetFreeEndpoint() => GetFreeEndpoint();
    public int GetVarInt(string var, int @default) => base.GetVarInt(var, @default);
}