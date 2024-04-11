using FreeSWITCH;
using FreeSWITCH.Native;
using Serilog;
using Serilog.Sinks.SystemConsole.Themes;
using SIPServer.Configuration;
using SIPServer.Loaders;
using SIPServerEmbedded.Communication;
using System.Runtime.InteropServices;

namespace SIPServer
{
    internal class Program
    {

        static void Main(string[] args)
        {
            Serilog.Log.Logger = new LoggerConfiguration()
                .WriteTo.Console(theme: AnsiConsoleTheme.Sixteen)
            .CreateLogger();

            Serilog.Log.Logger.Information("Starting Engine");

            Task.Factory.StartNew(() => { SIPEngine.RunSipServer(); });
            while (true)
            {
                var newCommand = Console.ReadLine();
                ProcessCommand(newCommand, true);
            }
        }

        static string ProcessCommandOnEngine(string newCommand)
        {


            if (!string.IsNullOrEmpty(newCommand))
            {
                return SIPServerCommands.SendCommand(newCommand);
            }

            return "";
         
        }

        static string ProcessCommand(string newCommand)
        {
            if (!APICommands.Decode(newCommand))
            {

                return ProcessCommandOnEngine(newCommand);
            }

            return "";
        }

        static string ProcessCommand(string newCommand, bool fromConsole)
        {
            string returnString = ProcessCommand(newCommand);
            Console.Write("MAS>");
            return returnString;

        }

    }
}
