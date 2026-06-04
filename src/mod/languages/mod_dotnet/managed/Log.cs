/* 
 * FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application - mod_managed
 * Copyright (C) 2008, Michael Giagnocavo <mgg@giagnocavo.net>
 *
 * Version: MPL 1.1
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application - mod_managed
 *
 * The Initial Developer of the Original Code is
 * Michael Giagnocavo <mgg@giagnocavo.net>
 * Portions created by the Initial Developer are Copyright (C)
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 * 
 * Michael Giagnocavo <mgg@giagnocavo.net>
 * 
 * Log.cs -- Log wrappers
 *
 */
using FreeSWITCH.Native;
using System;
using System.Collections.Generic;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text;
using static FreeSWITCH.EventBinding;

namespace FreeSWITCH
{
    public static class Log
    {


        public static void Write(LogLevel level, string message)
        {
            Native.freeswitch.console_log(level.ToLogString(), message);
        }
        public static void Write(LogLevel level, string format, params object[] args)
        {
            Native.freeswitch.console_log(level.ToLogString(), string.Format(format, args));
        }
        public static void WriteLine(LogLevel level, string message)
        {
            Native.freeswitch.console_log(level.ToLogString(), message + Environment.NewLine);
        }
        public static void WriteLine(LogLevel level, string format, params object[] args)
        {
            Native.freeswitch.console_log(level.ToLogString(), string.Format(format, args) + Environment.NewLine);
        }

        static string ToLogString(this LogLevel level)
        {
            switch (level)
            {
                case LogLevel.Console: return "CONSOLE";
                case LogLevel.Alert: return "ALERT";
                case LogLevel.Critical: return "CRIT";
                case LogLevel.Debug: return "DEBUG";
                case LogLevel.Error: return "ERR";
                case LogLevel.Info: return "INFO";
                case LogLevel.Notice: return "NOTICE";
                case LogLevel.Warning: return "WARNING";
                default:
                    System.Diagnostics.Debug.Fail("Invalid LogLevel: " + level.ToString() + " (" + (int)level + ").");
                    return "INFO";
            }
        }
    }

    /*switch_log.c:
    tatic const char *LEVELS[] = {
	"CONSOLE",
	"ALERT",
	"CRIT",
	"ERR",
	"WARNING",
	"NOTICE",
	"INFO",
	"DEBUG",
	NULL
    };*/
    public enum LogLevel
    {
        Console,
        Debug,
        Info,
        Error,
        Critical,
        Alert,
        Warning,
        Notice,
    }


    public class LogBinding : IDisposable
    {
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public delegate void switch_log_callback_delegate(IntPtr logData, switch_log_level_t level);
     //   public delegate void switch_log_callback_delegate(switch_log_node_t node);
        readonly switch_log_callback_delegate del; // Prevent GC
        readonly SWIGTYPE_p_f_p_q_const__switch_log_node_t_enum_switch_log_level_t__switch_status_t function;

        private LogBinding(SWIGTYPE_p_f_p_q_const__switch_log_node_t_enum_switch_log_level_t__switch_status_t function, switch_log_callback_delegate origDelegate)
        {
            this.function = function;
            this.del = origDelegate;
        }
        bool disposed;
        public void Dispose()
        {
            dispose();
            GC.SuppressFinalize(this);
        }
        void dispose()
        {
            if (disposed) return;
            // HACK: FS crashes if we unbind after shutdown is pretty complete. This is still a race condition.
            if (freeswitch.switch_core_ready() == switch_bool_t.SWITCH_FALSE) return;
            freeswitch.switch_log_unbind_logger(this.function);
            disposed = true;
        }
        ~LogBinding()
        {
            dispose();
        }

      

        public static IDisposable Bind(switch_log_level_t logLevel, switch_bool_t isConsole, switch_log_callback_delegate logging)
        {

            var fp = Marshal.GetFunctionPointerForDelegate(logging);
            var swigFp = new SWIGTYPE_p_f_p_q_const__switch_log_node_t_enum_switch_log_level_t__switch_status_t(fp, false);
            var res = freeswitch.switch_log_bind_logger(swigFp, logLevel, isConsole);
            if (res != switch_status_t.SWITCH_STATUS_SUCCESS)
            {
                throw new InvalidOperationException("Call to switch_log_bind_logger failed, result: " + res + ".");
            }
            return new LogBinding(swigFp, logging);
        }
    }
}
