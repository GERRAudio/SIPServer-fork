
// Native mod_managed replacement for the Python sidecar.
// Spawns a background thread on load to monitor the API and WAV folder.
// Uses native FreeSWITCH API execution to break file locks 


using System;
using System.Collections.Generic;
using System.IO;
using System.Net;
using System.Text.RegularExpressions;
using System.Threading;
using FreeSWITCH;
using FreeSWITCH.Native;

public class AsrSidecar : ILoadNotificationPlugin
{
    private const string BASE_URL = "http://localhost";
    private const string FOLDER = @"C:\inetpub\SIPServer\asr-queue";
    private const int POLL_INTERVAL_MS = 500;
    private const int TIMEOUT_S = 25;
    private const double FILE_SETTLE_S = 1.0;

    private static bool _isRunning = false;
    private static Thread _workerThread;

    // Triggered automatically by mod_managed when the script is loaded
    public bool Load()
    {
        if (_isRunning) return true;

        _isRunning = true;
        _workerThread = new Thread(MainLoop) 
        { 
            IsBackground = true, 
            Name = "AsrSidecarWorker" 
        };
        _workerThread.Start();
        
        Log.WriteLine(LogLevel.Info, "[AsrSidecar] Native plugin loaded, background thread started successfully.");
        return true;
    }

    private void MainLoop()
    {
        Directory.CreateDirectory(FOLDER);
        HashSet<string> seenFiles = new HashSet<string>(StringComparer.OrdinalIgnoreCase);

        // Pre-fill seen files so we don't process old artifacts on startup
        foreach (var f in Directory.GetFiles(FOLDER, "*.wav"))
        {
            seenFiles.Add(f);
        }

        string lastCurrentName = null;

        while (_isRunning)
        {
            try
            {
                string bpName = GetCurrentBeltpack(BASE_URL);

                if (string.IsNullOrEmpty(bpName))
                {
                    lastCurrentName = null;
                    Thread.Sleep(POLL_INTERVAL_MS);
                    continue;
                }

                if (bpName == lastCurrentName)
                {
                    Thread.Sleep(POLL_INTERVAL_MS);
                    continue;
                }

                lastCurrentName = bpName;
                Log.WriteLine(LogLevel.Info, "[AsrSidecar] [" + bpName + "] is Current — waiting for audio...");

                string wavPath = WaitForWav(FOLDER, seenFiles, TIMEOUT_S);

                if (!string.IsNullOrEmpty(wavPath))
                {
                    Log.WriteLine(LogLevel.Info, "[AsrSidecar] [" + bpName + "] Audio ready: " + wavPath);
                    PostCommand(BASE_URL, bpName, wavPath);

                    // Give SAPI time to read, then clean up
                    Thread.Sleep(2000);
                    try
                    {
                        if (File.Exists(wavPath))
                        {
                            File.Delete(wavPath);
                            Log.WriteLine(LogLevel.Info, "[AsrSidecar] [" + bpName + "] Cleaned up WAV: " + Path.GetFileName(wavPath));
                        }
                    }
                    catch (Exception ex)
                    {
                        Log.WriteLine(LogLevel.Warning, "[AsrSidecar] [" + bpName + "] Clean up failed: " + ex.Message);
                    }
                }
                else
                {
                    Log.WriteLine(LogLevel.Warning, "[AsrSidecar] [" + bpName + "] No audio in " + TIMEOUT_S + "s — skipping");
                    PostSkip(BASE_URL, bpName, "no_audio_file");
                }

                lastCurrentName = null;
            }
            catch (ThreadAbortException)
            {
                _isRunning = false;
            }
            catch (Exception ex)
            {
                Log.WriteLine(LogLevel.Error, "[AsrSidecar] Exception in main loop: " + ex.Message);
                Thread.Sleep(2000);
            }
        }
    }

    private string GetCurrentBeltpack(string baseUrl)
    {
        try
        {
            using (WebClient wc = new WebClient())
            {
                wc.Headers[HttpRequestHeader.ContentType] = "application/json";
                string json = wc.DownloadString(baseUrl + "/api/asr/queue");
                
                // Lightweight Regex parse to avoid relying on external JSON DLLs in mod_managed
                Match m = Regex.Match(json, "\"(?:current|Current)\"\\s*:\\s*\\{[^}]*\"(?:beltpackName|BeltpackName)\"\\s*:\\s*\"([^\"]+)\"");
                if (m.Success)
                {
                    return m.Groups[1].Value;
                }
            }
        }
        catch (WebException) { /* Ignore timeout polls silently */ }
        catch (Exception ex)
        {
            Log.WriteLine(LogLevel.Debug, "[AsrSidecar] Poll error: " + ex.Message);
        }
        return null;
    }

    private string WaitForWav(string folder, HashSet<string> seen, int timeoutS)
    {
        DateTime deadline = DateTime.Now.AddSeconds(timeoutS);
        string candidate = null;
        long lastSize = -1;
        DateTime? stableAt = null;

        while (DateTime.Now < deadline)
        {
            if (candidate == null)
            {
                string[] files = Directory.GetFiles(folder, "*.wav");
                foreach (string file in files)
                {
                    if (!seen.Contains(file))
                    {
                        candidate = file;
                        lastSize = -1;
                        stableAt = null;
                        Log.WriteLine(LogLevel.Info, "[AsrSidecar] --> NEW FILE DETECTED ON DISK: " + Path.GetFileName(candidate));
                        break;
                    }
                }
            }

            if (candidate != null)
            {
                try
                {
                    FileInfo fi = new FileInfo(candidate);
                    if (!fi.Exists)
                    {
                        candidate = null;
                        continue;
                    }

                    long size = fi.Length;

                    if (size == lastSize && size > 0)
                    {
                        if (stableAt == null)
                        {
                            stableAt = DateTime.Now;
                        }
                        else if ((DateTime.Now - stableAt.Value).TotalSeconds >= FILE_SETTLE_S)
                        {
                            Log.WriteLine(LogLevel.Info, "[AsrSidecar] Size settled at " + size + " bytes. PTT released.");

                            // Break FreeSWITCH Lock internally using Native API!
                            ForceFreeswitchUnlock(candidate);

                            if (VerifyFileUnlocked(candidate, 5.0))
                            {
                                seen.Add(candidate);
                                return candidate;
                            }
                            else
                            {
                                candidate = null;
                                stableAt = null;
                            }
                        }
                    }
                    else
                    {
                        lastSize = size;
                        stableAt = null;
                    }
                }
                catch (Exception ex)
                {
                    Log.WriteLine(LogLevel.Error, "[AsrSidecar] Error checking file " + candidate + ": " + ex.Message);
                    candidate = null;
                }
            }

            Thread.Sleep(300);
        }

        return null;
    }

    private void ForceFreeswitchUnlock(string filepath)
    {
        string filename = Path.GetFileNameWithoutExtension(filepath);
        Log.WriteLine(LogLevel.Info, "[AsrSidecar] Forcing FreeSWITCH to release lock for UUID: " + filename);
        
        // Execute the break command directly in FreeSWITCH's memory space using the Native API object
        Api fsApi = new Api(null);
        fsApi.ExecuteString("uuid_break " + filename);
    }

    private bool VerifyFileUnlocked(string filepath, double maxWaitSeconds)
    {
        DateTime start = DateTime.Now;
        int attempt = 1;
        Log.WriteLine(LogLevel.Info, "[AsrSidecar] Verifying file locks for: " + Path.GetFileName(filepath));

        while ((DateTime.Now - start).TotalSeconds < maxWaitSeconds)
        {
            try
            {
                // Atomic self-rename test to guarantee the OS has relinquished the file
                string temp = filepath + ".locktest";
                File.Move(filepath, temp);
                File.Move(temp, filepath);

                Log.WriteLine(LogLevel.Info, "[AsrSidecar] Attempt " + attempt + ": SUCCESS. File is completely unlocked by OS.");
                return true;
            }
            catch (IOException ex)
            {
                Log.WriteLine(LogLevel.Debug, "[AsrSidecar] Attempt " + attempt + ": LOCKED (IOException). OS reports: " + ex.Message);
            }
            catch (UnauthorizedAccessException ex)
            {
                Log.WriteLine(LogLevel.Debug, "[AsrSidecar] Attempt " + attempt + ": LOCKED (UnauthorizedAccess). OS reports: " + ex.Message);
            }

            Thread.Sleep(500);
            attempt++;
        }

        Log.WriteLine(LogLevel.Error, "[AsrSidecar] TIMEOUT. File remained locked.");
        return false;
    }

    private void PostCommand(string baseUrl, string bpName, string wavPath)
    {
        try
        {
            // Escape backslashes for JSON mapping
            string escapedPath = wavPath.Replace("\\", "\\\\");
            string json = "{\"beltpackName\":\"" + bpName + "\",\"conference\":\"\",\"transcript\":\"WAV:" + escapedPath + "\"}";

            using (WebClient wc = new WebClient())
            {
                wc.Headers[HttpRequestHeader.ContentType] = "application/json";
                Log.WriteLine(LogLevel.Info, "[AsrSidecar] POSTing to " + baseUrl + "/api/asr/voicecommand");
                
                string response = wc.UploadString(baseUrl + "/api/asr/voicecommand", "POST", json);
                Log.WriteLine(LogLevel.Info, "[AsrSidecar] [" + bpName + "] API Success: " + response.Trim());
            }
        }
        catch (WebException ex)
        {
            string responseText = "";
            if (ex.Response != null)
            {
                using (var reader = new StreamReader(ex.Response.GetResponseStream()))
                {
                    responseText = reader.ReadToEnd();
                }
            }
            Log.WriteLine(LogLevel.Warning, "[AsrSidecar] [" + bpName + "] API Error: " + responseText);
        }
        catch (Exception ex)
        {
            Log.WriteLine(LogLevel.Error, "[AsrSidecar] [" + bpName + "] POST failed: " + ex.Message);
        }
    }

    private void PostSkip(string baseUrl, string bpName, string reason)
    {
        try
        {
            string json = "{\"beltpackName\":\"" + bpName + "\",\"reason\":\"" + reason + "\"}";

            using (WebClient wc = new WebClient())
            {
                wc.Headers[HttpRequestHeader.ContentType] = "application/json";
                wc.UploadString(baseUrl + "/api/asr/skip", "POST", json);
                Log.WriteLine(LogLevel.Info, "[AsrSidecar] [" + bpName + "] skipped (" + reason + ")");
            }
        }
        catch (Exception ex)
        {
            Log.WriteLine(LogLevel.Error, "[AsrSidecar] [" + bpName + "] skip POST failed: " + ex.Message);
        }
    }

    // Required by the mod_managed C# compiler to satisfy entry point requirements for CSX files
    public static void Main()
    {
        // Leave empty. Execution happens via the ILoadNotificationPlugin interface hook.
    }
}