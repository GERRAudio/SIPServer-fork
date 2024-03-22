using FreeSWITCH;
using FreeSWITCH.Native;
using Serilog;
using SIPServer.Configuration;
using SIPServer.Loaders;
using System.Runtime.InteropServices;

namespace SIPServer
{
    internal class Program
    {

        static void Main(string[] args)
        {
            Serilog.Log.Logger = new LoggerConfiguration()
                .WriteTo.Console()
            .CreateLogger();

            Serilog.Log.Logger.Information("Starting Engine");

            Task.Factory.StartNew(() => { SIPEngine.RunSipServer(); });
            while (true)
            {
                var newCommand = Console.ReadLine();
                ProcessCommand(newCommand);
            }
        }

        static void ProcessCommand(string newCommand)
        {
            if (!string.IsNullOrEmpty(newCommand))
            {
                Serilog.Log.Logger.Debug(newCommand);
                var result = SIPEngine.SIPServerAPI.ExecuteString(newCommand);
                Serilog.Log.Logger.Information(result);
            }
        }

    }
}
