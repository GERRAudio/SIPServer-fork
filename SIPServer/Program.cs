using FreeSWITCH;
using FreeSWITCH.Native;
using System.Runtime.InteropServices;

namespace SIPServer
{
    internal class Program
    {
        static Api SIPServerAPI;


        static void Main(string[] args)
        {
            String err = "";
            const uint flags = (uint)(switch_core_flag_enum_t.SCF_USE_SQL | switch_core_flag_enum_t.SCF_CLEAR_SQL);
            freeswitch.switch_core_set_globals();
            /*Next 3 lines only needed if you want to bind to the initial event or xml config search loops */
            freeswitch.switch_core_init(flags, switch_bool_t.SWITCH_FALSE, out err);
            var search_bind = SwitchXmlSearchBinding.Bind(XMLProvider, switch_xml_section_enum_t.SWITCH_XML_SECTION_CONFIG);
            var event_bind  = EventBinding.Bind("SIPClient", switch_event_types_t.SWITCH_EVENT_ALL, null, SIPEventHandler, true);
            var log_bind    = LogBinding.Bind(switch_log_level_t.SWITCH_LOG_DEBUG, switch_bool_t.SWITCH_TRUE, LogHandler);
            


            /* End Optional Lines */
            freeswitch.switch_core_init_and_modload(flags, switch_bool_t.SWITCH_FALSE, out err);

            freeswitch.switch_console_loop();
        }



        private static void LogHandler(IntPtr nodePointer, switch_log_level_t level)
        {
            switch_log_node_t nodeReference = new switch_log_node_t(nodePointer, false);


            var time = Marshal.ReadInt64(nodeReference.timestamp.Ptr.Handle);

            DateTime unixEpochTime = DateTime.UnixEpoch.AddMicroseconds(time);
           

            Console.WriteLine(nodeReference.data);
            Console.WriteLine(unixEpochTime);

        }

   


        private static void SIPEventHandler(EventBinding.EventBindingArgs args)
        {
            Console.WriteLine(args.EventObj.event_id.ToString());
            switch(args.EventObj.event_id)
            { 
            case switch_event_types_t.SWITCH_EVENT_API:
                {
                    Console.WriteLine(args.EventObj.body);
                }
                break;
            }
           // throw new NotImplementedException();
        }

        private static string XMLProvider(SwitchXmlSearchBinding.XmlBindingArgs args)
        {
            //  throw new NotImplementedException();
            Console.WriteLine(args.KeyValue.ToString());
            return "";
        }
    }
}
