using FreeSWITCH.Native;
using FreeSWITCH;
using SIPServer.Configuration;
using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.Threading.Tasks;

namespace SIPServer.Events
{
    internal class Handler
    {
        internal static void SIPEventHandler(EventBinding.EventBindingArgs args)
        {
            var eventID = args.EventObj.event_id;

            if ((eventID == switch_event_types_t.SWITCH_EVENT_RE_SCHEDULE) || (eventID == switch_event_types_t.SWITCH_EVENT_HEARTBEAT) || (eventID == switch_event_types_t.SWITCH_EVENT_SESSION_HEARTBEAT))
            {
                return;
            }
            var eventDictionary = CreateEventDictionary(args);

            switch (args.EventObj.event_id)
            {

                case switch_event_types_t.SWITCH_EVENT_RELOADXML:

                    break;


                case switch_event_types_t.SWITCH_EVENT_API:
                    {
                        if (eventDictionary.ContainsKey("API-Command") && eventDictionary.ContainsKey("API-Command-Argument"))
                        {
                            Console.WriteLine($"API request => {eventDictionary["API-Command"]} {eventDictionary["API-Command-Argument"]}");
                        }

                    }
                    break;

                case switch_event_types_t.SWITCH_EVENT_MODULE_LOAD:
                case switch_event_types_t.SWITCH_EVENT_MODULE_UNLOAD:
                    {
                        if (eventDictionary.ContainsKey("name"))
                        {
                            Modules.ReportModifyModules(eventDictionary["name"], args.EventObj.event_id == switch_event_types_t.SWITCH_EVENT_MODULE_LOAD);
                        }

                    }
                    break;
            }
            // throw new NotImplementedException();
        }

        private static Dictionary<string, string> CreateEventDictionary(EventBinding.EventBindingArgs args)
        {
            Dictionary<string, string> returnDictionary = new Dictionary<string, string>();

            var firstHeader = args.EventObj.headers;
            while (true)
            {
                returnDictionary.Add(firstHeader.name, firstHeader.value);
                if (firstHeader.next != null)
                {
                    firstHeader = firstHeader.next;
                }
                else
                {
                    break;
                }
            }

            return returnDictionary;
        }
    }
}
