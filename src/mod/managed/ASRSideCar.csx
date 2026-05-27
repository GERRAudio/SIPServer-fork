// Monitors the asr-queue and manages the prompting etc
//
using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.IO;
using System.Net;
using System.Threading;
using System.Threading.Tasks;
using FreeSWITCH;
using FreeSWITCH.Native;

public class AsrSidecar : ILoadNotificationPlugin
{
    private const string BASE_URL = "http://localhost";
    private const string FOLDER = @"C:\inetpub\SIPServer\asr-queue";
    private const int SCAN_INTERVAL_MS = 300;
    private const double FILE_SETTLE_S = 1.0;

    // Delimiter used in FreeSWITCH dialplan: ${beltpack_name}___${uuid}.wav
    private const string DELIMITER = "___";

    private static bool _isRunning = false;
    private static Thread _workerThread;

    // Thread-safe dictionary to track the growth state of multiple files
    private ConcurrentDictionary<string, FileTracker> _activeFiles;

    private class FileTracker
    {
        public long LastSize { get; set; } = -1;
        public DateTime? StableAt { get; set; } = null;
        public bool IsProcessing { get; set; } = false;
    }

    public bool Load()
    {
        if (_isRunning) return true;

        _isRunning = true;
        _activeFiles = new ConcurrentDictionary<string, FileTracker>(StringComparer.OrdinalIgnoreCase);

        _workerThread = new Thread(MainLoop)
        {
            IsBackground = true,
            Name = "AsrConcurrentScanner"
        };
        _workerThread.Start();

        Log.WriteLine(LogLevel.Info, "[AsrSidecar] Concurrent scanner loaded successfully.");
        return true;
    }

    private void MainLoop()
    {
        Directory.CreateDirectory(FOLDER);

        while (_isRunning)
        {
            try
            {
                string[] currentFiles = Directory.GetFiles(FOLDER, "*.wav");

                foreach (string filepath in currentFiles)
                {
                    // Ignore files already claimed by the unlocker
                    if (filepath.EndsWith("_ready.wav", StringComparison.OrdinalIgnoreCase))
                        continue;

                    _activeFiles.AddOrUpdate(filepath,
                        new FileTracker(),
                        (key, tracker) =>
                        {
                            if (tracker.IsProcessing) return tracker;

                            try
                            {
                                FileInfo fi = new FileInfo(key);
                                if (!fi.Exists) return tracker;

                                long currentSize = fi.Length;

                                if (currentSize == tracker.LastSize && currentSize > 0)
                                {
                                    if (tracker.StableAt == null)
                                    {
                                        tracker.StableAt = DateTime.Now;
                                    }
                                    else if ((DateTime.Now - tracker.StableAt.Value).TotalSeconds >= FILE_SETTLE_S)
                                    {
                                        // File is stable. Mark as processing and spawn a concurrent task.
                                        tracker.IsProcessing = true;
                                        Task.Run(() => ProcessStableFile(key));
                                    }
                                }
                                else
                                {
                                    tracker.LastSize = currentSize;
                                    tracker.StableAt = null;
                                }
                            }
                            catch (Exception) { /* Transient file access error, ignore */ }

                            return tracker;
                        });
                }

                // Cleanup deleted/missing files from tracker dictionary
                var keys = new List<string>(_activeFiles.Keys);
                foreach (var key in keys)
                {
                    if (!File.Exists(key))
                    {
                        _activeFiles.TryRemove(key, out _);
                    }
                }
            }
            catch (ThreadAbortException)
            {
                _isRunning = false;
            }
            catch (Exception ex)
            {
                Log.WriteLine(LogLevel.Error, "[AsrSidecar] Directory scan error: " + ex.Message);
            }

            Thread.Sleep(SCAN_INTERVAL_MS);
        }
    }

    private void ProcessStableFile(string filepath)
    {
        string filename = Path.GetFileNameWithoutExtension(filepath);
        string bpName = "UNKNOWN";
        string uuid = filename;

        // Parse BeltpackName and UUID from filename
        if (filename.Contains(DELIMITER))
        {
            string[] parts = filename.Split(new string[] { DELIMITER }, StringSplitOptions.None);
            if (parts.Length >= 2)
            {
                bpName = parts[0];
                uuid = parts[1];
            }
        }

        Log.WriteLine(LogLevel.Info, "[AsrSidecar] [" + bpName + "] Audio settled. Forcing unlock on UUID: " + uuid);

        try
        {
            // 1. Force FreeSWITCH to drop the record handle natively
            Api fsApi = new Api(null);
            fsApi.ExecuteString("uuid_break " + uuid);

            // 2. Wait for OS to release locks and execute atomic rename
            if (VerifyAndClaimFile(filepath, out string readyPath, 5.0))
            {
                Log.WriteLine(LogLevel.Info, "[AsrSidecar] [" + bpName + "] Audio unlocked and ready.");

                // 3. POST to IIS
                PostCommand(BASE_URL, bpName, readyPath);

                // 4. Cleanup
                Thread.Sleep(2000);
                if (File.Exists(readyPath)) File.Delete(readyPath);
            }
            else
            {
                Log.WriteLine(LogLevel.Warning, "[AsrSidecar] [" + bpName + "] Failed to acquire OS lock. Abandoning file.");
                PostSkip(BASE_URL, bpName, "file_locked");
            }
        }
        catch (Exception ex)
        {
            Log.WriteLine(LogLevel.Error, "[AsrSidecar] [" + bpName + "] Processing error: " + ex.Message);
        }
        finally
        {
            // Remove from tracking dictionary so it isn't scanned again
            _activeFiles.TryRemove(filepath, out _);
        }
    }

    private bool VerifyAndClaimFile(string originalPath, out string readyPath, double maxWaitSeconds)
    {
        readyPath = originalPath.Replace(".wav", "_ready.wav");
        DateTime start = DateTime.Now;

        while ((DateTime.Now - start).TotalSeconds < maxWaitSeconds)
        {
            try
            {
                // Attempt to permanently rename the file to claim ownership
                File.Move(originalPath, readyPath);

                // Pause briefly for Windows Defender to clear
                Thread.Sleep(500);
                return true;
            }
            catch (IOException) { /* Locked */ }
            catch (UnauthorizedAccessException) { /* Locked */ }

            Thread.Sleep(400);
        }

        return false;
    }

    private void PostCommand(string baseUrl, string bpName, string wavPath)
    {
        try
        {
            // No modern string interpolation (C# 6+) here to satisfy older embedded mono compilers
            string escapedPath = wavPath.Replace("\\", "\\\\");
            string json = "{\"beltpackName\":\"" + bpName + "\",\"conference\":\"\",\"transcript\":\"WAV:" + escapedPath + "\"}";

            using (WebClient wc = new WebClient())
            {
                wc.Headers[HttpRequestHeader.ContentType] = "application/json";
                string response = wc.UploadString(baseUrl + "/api/asr/voicecommand", "POST", json);
                Log.WriteLine(LogLevel.Info, "[AsrSidecar] [" + bpName + "] API Success: " + response.Trim());
            }
        }
        catch (WebException ex)
        {
            Log.WriteLine(LogLevel.Warning, "[AsrSidecar] [" + bpName + "] API Error: " + ex.Message);
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
            }
        }
        catch (Exception) { }
    }

    public static void Main() { }
}