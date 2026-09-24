using FreeSWITCH;
using FreeSWITCH.Native;
using SIPServer.Configuration;
using System.Runtime.InteropServices;

namespace SIPServer
{
    internal class Program
    {

        static void Main(string[] args)
        {
            Task.Factory.StartNew(() => { Loaders.SIPEngine.RunSipServer(); });
            Console.WriteLine("Out of SIP loop");
            Console.ReadLine();
        }

    }
}
