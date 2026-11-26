using System.IO;
using System.Runtime.InteropServices;

namespace MinecraftClient.Launcher;

internal static class StartMenuShortcut
{
    internal static void Create()
    {
        var programs = Environment.GetFolderPath(Environment.SpecialFolder.Programs);
        var executable = Path.Combine(AppContext.BaseDirectory, "Launcher.exe");
        if (string.IsNullOrEmpty(programs) || !File.Exists(executable))
            throw new IOException("Extract the complete release ZIP into a permanent folder and open Launcher.exe from there.");

        Directory.CreateDirectory(programs);
        var path = Path.Combine(programs, "Bedrock Local Toolkit.lnk");
        var shellType = Type.GetTypeFromProgID("WScript.Shell", throwOnError: true)!;
        dynamic shell = Activator.CreateInstance(shellType)!;
        try
        {
            dynamic shortcut = shell.CreateShortcut(path);
            try
            {
                shortcut.TargetPath = executable;
                shortcut.Arguments = "";
                shortcut.WorkingDirectory = AppContext.BaseDirectory;
                shortcut.Description = "Bedrock Local Toolkit";
                shortcut.IconLocation = executable + ",0";
                shortcut.Save();
            }
            finally
            {
                _ = Marshal.ReleaseComObject(shortcut);
            }
        }
        finally
        {
            _ = Marshal.ReleaseComObject(shell);
        }
    }
}
