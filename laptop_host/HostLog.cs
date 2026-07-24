using System;
using System.Diagnostics;
using System.IO;

namespace X3LaptopCompanion
{
    public static class HostLog
    {
        private static readonly object Sync = new object();

        public static string LogPath
        {
            get
            {
                return Path.Combine(
                    Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
                    "X3LaptopCompanion",
                    "host.log");
            }
        }

        public static void Write(string message)
        {
            Write(message, null);
        }

        public static void Write(string message, Exception exception)
        {
            try
            {
                lock (Sync)
                {
                    var directory = Path.GetDirectoryName(LogPath);
                    if (!string.IsNullOrEmpty(directory))
                    {
                        Directory.CreateDirectory(directory);
                    }

                    Debug.WriteLine(DateTimeOffset.Now.ToString("yyyy-MM-dd HH:mm:ss.fff zzz") + " " + message);
                    File.AppendAllText(LogPath,
                        DateTimeOffset.Now.ToString("yyyy-MM-dd HH:mm:ss.fff zzz") + " " + message + Environment.NewLine);
                    if (exception != null)
                    {
                        File.AppendAllText(LogPath, exception + Environment.NewLine);
                        Debug.WriteLine(exception);
                    }
                }
            }
            catch
            {
                // Logging must never break the companion app.
            }
        }
    }
}
