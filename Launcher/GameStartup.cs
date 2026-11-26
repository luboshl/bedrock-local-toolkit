using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;

namespace MinecraftClient.Launcher;

internal static class GameStartup
{
    private const string GameAppId = GameModuleLauncher.PackageFamily + "!Game";
    private static readonly TimeSpan GameWait = TimeSpan.FromSeconds(90);

    internal static Task<string> RunAsync(IProgress<string> progress, CancellationToken cancellationToken) =>
        Task.Run(async () =>
        {
            void Report(string message)
            {
                LauncherLog.Write(message);
                progress.Report(message);
            }

            Report("Reading toolkit settings…");
            var settings = ToolkitSettings.Load();
            if (!settings.Validate())
                throw new InvalidDataException("Some settings in bedrock-toolkit.ini are invalid. Correct the highlighted fields in Settings, save, and select Retry.");

            Report("Looking for Minecraft for Windows…");
            var reused = HasRunningGame();
            if (!reused)
            {
                cancellationToken.ThrowIfCancellationRequested();
                Report("Starting Minecraft for Windows…");
                LauncherLog.Write($"Windows accepted Minecraft activation (initial PID {ActivateGame()}).");
            }

            Report("Waiting for the Minecraft window (up to 90 seconds)…");
            var processId = await WaitForGameWindowAsync(cancellationToken).ConfigureAwait(false);
            Report(reused ? "Checking the existing Minecraft session…" : "Checking the new Minecraft session…");
            if (settings.AllowUnsupportedVersion)
                LauncherLog.Write("Experimental compatibility enabled. Feature validation remains mandatory.");

            Report("Verifying the game and starting the toolkit module…");
            GameModuleLauncher.Start(processId, settings.AllowUnsupportedVersion,
                message => LauncherLog.Write(message), cancellationToken);

            return reused
                ? "The toolkit module started or was already running in the existing Minecraft session."
                : "Minecraft opened and the toolkit module started.";
        }, cancellationToken);

    private static bool HasRunningGame() => FindGameProcesses(needsWindow: false).Length > 0;

    private static int[] FindGameProcesses(bool needsWindow)
    {
        using var launcher = Process.GetCurrentProcess();
        var processes = Process.GetProcessesByName(GameModuleLauncher.ProcessName);
        try
        {
            var sessionId = launcher.SessionId;
            var candidates = new List<int>();
            foreach (var process in processes)
            {
                try
                {
                    if (!process.HasExited && process.SessionId == sessionId &&
                        (!needsWindow || process.MainWindowHandle != IntPtr.Zero))
                        candidates.Add(process.Id);
                }
                catch (InvalidOperationException) when (process.HasExited)
                {
                    // The process exited while Windows was enumerating its session or window.
                }
            }
            return candidates.ToArray();
        }
        finally
        {
            foreach (var process in processes)
                process.Dispose();
        }
    }

    private static int ActivateGame()
    {
        const int errorInsufficientBuffer = 122;
        uint packages = 0;
        uint length = 0;
        var result = GetPackagesByPackageFamily(GameModuleLauncher.PackageFamily, ref packages, IntPtr.Zero,
            ref length, IntPtr.Zero);
        if (result == 0 && packages == 0)
            throw new InvalidOperationException("Minecraft for Windows is missing. Install or re-register it from Microsoft Store or the Xbox app, then select Retry.");

        if (result is not (0 or errorInsufficientBuffer))
            throw new System.ComponentModel.Win32Exception(result, "Windows could not check the Minecraft for Windows installation. Try opening Minecraft from Start, then select Retry.");

        var manager = (IApplicationActivationManager)new ApplicationActivationManager();
        try
        {
            manager.ActivateApplication(GameAppId, null, 2, out var processId); // AO_NOERRORUI
            return checked((int)processId);
        }
        catch (COMException ex)
        {
            throw new InvalidOperationException(
                $"Windows could not open Minecraft for Windows (0x{ex.HResult:X8}). Try opening it from Start or repair the installation, then select Retry.", ex);
        }
        finally
        {
            Marshal.ReleaseComObject(manager);
        }
    }

    private static async Task<int> WaitForGameWindowAsync(CancellationToken cancellationToken)
    {
        var elapsed = Stopwatch.StartNew();
        do
        {
            cancellationToken.ThrowIfCancellationRequested();
            // Minecraft may replace its initial activation process before opening its window.
            var candidates = FindGameProcesses(needsWindow: true);
            if (candidates.Length > 1)
                throw new InvalidOperationException("More than one Minecraft for Windows window is open. Save and close extra sessions, then select Retry.");
            if (candidates.Length == 1)
                return candidates[0];
            await Task.Delay(250, cancellationToken).ConfigureAwait(false);
        } while (elapsed.Elapsed < GameWait);

        throw new TimeoutException("Minecraft for Windows did not open a window within 90 seconds. Check for a game or sign-in prompt, then select Retry.");
    }

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
    private static extern int GetPackagesByPackageFamily(string packageFamilyName, ref uint count,
        IntPtr packageFullNames, ref uint bufferLength, IntPtr buffer);

    [ComImport, Guid("45BA127D-10A8-46EA-8AB7-56EA9078943C")]
    private class ApplicationActivationManager;

    [ComImport, Guid("2e941141-7f97-4756-ba1d-9decde894a3d"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    private interface IApplicationActivationManager
    {
        void ActivateApplication([MarshalAs(UnmanagedType.LPWStr)] string appUserModelId,
            [MarshalAs(UnmanagedType.LPWStr)] string? arguments, uint options, out uint processId);
    }
}
