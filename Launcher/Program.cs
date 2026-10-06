using System.ComponentModel;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;
using System.Globalization;
using Microsoft.Win32.SafeHandles;

namespace MinecraftClient.Launcher;

internal static class Program
{
    private const string ProcessName = "Minecraft.Windows";
    private const string ExpectedPackageFamily = "Microsoft.MinecraftUWP_8wekyb3d8bbwe";
    private const string ExpectedPackageFullName = "Microsoft.MinecraftUWP_1.26.5203.0_x64__8wekyb3d8bbwe";
    private const ushort ImageFileMachineUnknown = 0;
    private const ushort ImageFileMachineAmd64 = 0x8664;
    private const uint ErrorInsufficientBuffer = 122;
    private const uint ProcessQueryLimitedInformation = 0x1000;
    private const uint RequiredRemoteLoadAccess = 0x043A;
    private const uint MemCommit = 0x1000;
    private const uint MemReserve = 0x2000;
    private const uint MemRelease = 0x8000;
    private const uint PageReadWrite = 0x04;
    private const uint WaitObject0 = 0;
    private const uint WaitTimeout = 0x102;
    private const uint RemoteCallTimeoutMs = 30_000;

    private sealed record ZoomConfig(float Fov, uint TransitionDurationMs, float MouseSensitivity,
        string ZoomKey, string NametagKey, string AlwaysDayKey, string FullBrightKey, string IndicatorKey, string ExitKey);

    private static int Main()
    {
        Console.OutputEncoding = Encoding.UTF8;

        try
        {
            var zoomConfigPath = Path.Combine(AppContext.BaseDirectory, "zoom.ini");
            var zoomConfig = LoadZoomConfig(zoomConfigPath);
            Console.WriteLine($"Configuration ({zoomConfigPath}): FOV {zoomConfig.Fov}, transition {zoomConfig.TransitionDurationMs} ms, " +
                $"sensitivity {zoomConfig.MouseSensitivity}; Zoom {zoomConfig.ZoomKey}, Nametag {zoomConfig.NametagKey}, Always day {zoomConfig.AlwaysDayKey}, Full Bright {zoomConfig.FullBrightKey}, " +
                $"indicator {zoomConfig.IndicatorKey}, detach {zoomConfig.ExitKey}.");

            var target = WaitForSupportedGame(TimeSpan.FromSeconds(90));
            Console.WriteLine($"Verified target: PID {target.ProcessId}, {target.PackageFullName}, x64.");

            using var game = NativeMethods.OpenProcess(RequiredRemoteLoadAccess, false, target.ProcessId);
            if (game.IsInvalid)
            {
                var error = Marshal.GetLastWin32Error();
                throw new InvalidOperationException(
                    $"Windows denied the required process access (Win32 {error}: {new Win32Exception(error).Message}). " +
                    "GameMod.dll was not loaded; the launcher did not continue.");
            }

            var recheckedTarget = InspectProcess(game, target.ProcessId);
            EnsureSupportedTarget(recheckedTarget);
            Console.WriteLine("Remote-load access check passed.");

            var dllPath = Path.GetFullPath(Path.Combine(AppContext.BaseDirectory, "GameMod.dll"));
            if (!File.Exists(dllPath))
            {
                throw new FileNotFoundException("Locally built GameMod.dll is missing next to the launcher.", dllPath);
            }

            var pointerConfigPath = Path.GetFullPath(Path.Combine(AppContext.BaseDirectory, "zoom-pointer.ini"));
            if (!File.Exists(pointerConfigPath))
            {
                Console.WriteLine("Saved pointer chain not found; using automatic FOV validation for the supported build.");
            }

            using var targetProcess = Process.GetProcessById(target.ProcessId);
            var preloadedModule = FindModule(targetProcess, "GameMod.dll");
            if (preloadedModule is not null &&
                !string.Equals(Path.GetFullPath(preloadedModule.FileName), dllPath, StringComparison.OrdinalIgnoreCase))
            {
                throw new InvalidOperationException($"A different GameMod.dll is already loaded in the target process: {preloadedModule.FileName}");
            }

            LoadAndStartModule(game, targetProcess, dllPath, preloadedModule);
            Console.WriteLine("GameMod.dll is loaded and initialization has started.");
            Console.WriteLine("Use only in a local single-player world. If sensitivity is verified, it will change during Zoom and be restored when Zoom ends; otherwise, Zoom continues without changing sensitivity. Use the mouse wheel to adjust FOV in 5° increments within 1–120°; the wheel is captured while Zoom is active. Releasing the Zoom key, pressing Esc, or losing focus smoothly restores the original FOV. Nametag displays your name in both third-person views and is occluded by blocks. Preparation starts automatically when the module loads. The detach key first restores FOV, any changed sensitivity, and the Nametag, Always day, and Full Bright rendering hooks. Always day switches local rendering to noon; the default key is F6. Full Bright brightens the local light texture; the default key is F8. The new feature's result is awaiting in-game verification.");
            Console.WriteLine($"Run diagnostics: {Path.Combine(AppContext.BaseDirectory, $"zoom-status-{target.ProcessId}.log")}");
            Console.WriteLine($"Nametag diagnostics: {Path.Combine(AppContext.BaseDirectory, $"nametag-status-{target.ProcessId}.log")}");
            Console.WriteLine($"Always day diagnostics: {Path.Combine(AppContext.BaseDirectory, $"always-day-status-{target.ProcessId}.log")}");
            Console.WriteLine($"Full Bright diagnostics: {Path.Combine(AppContext.BaseDirectory, $"full-bright-status-{target.ProcessId}.log")}");
            return 0;
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine($"Error: {ex.Message}");
            return 1;
        }
    }

    private static ZoomConfig LoadZoomConfig(string path)
    {
        var defaults = new ZoomConfig(10f, 180, 12f, "C", "F7", "F6", "F8", "F9", "F10");
        if (!File.Exists(path)) return defaults;

        var values = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        var inZoomSection = false;
        foreach (var rawLine in File.ReadLines(path))
        {
            var line = rawLine.Trim();
            if (line.Length == 0 || line[0] is ';' or '#') continue;
            if (line.StartsWith('[') && line.EndsWith(']'))
            {
                inZoomSection = string.Equals(line[1..^1].Trim(), "Zoom", StringComparison.OrdinalIgnoreCase);
                continue;
            }
            if (!inZoomSection) continue;
            var separator = line.IndexOf('=');
            if (separator <= 0) continue;
            values[line[..separator].Trim()] = line[(separator + 1)..].Trim();
        }

        float ReadFloat(string key, float fallback, float minimum, float maximum)
        {
            if (!values.TryGetValue(key, out var text)) return fallback;
            if (!float.TryParse(text, NumberStyles.Float, CultureInfo.InvariantCulture, out var value) ||
                !float.IsFinite(value) || value < minimum || value > maximum)
                throw new InvalidDataException($"Invalid value for {key}='{text}' in {path}.");
            return value;
        }
        uint ReadDuration()
        {
            if (!values.TryGetValue("TransitionDurationMs", out var text)) return defaults.TransitionDurationMs;
            if (!uint.TryParse(text, NumberStyles.None, CultureInfo.InvariantCulture, out var value) || value is < 1 or > 10_000)
                throw new InvalidDataException($"Invalid value for TransitionDurationMs='{text}' in {path}.");
            return value;
        }
        string ReadKey(string key, string fallback) =>
            values.TryGetValue(key, out var value) && IsSupportedKey(value) ? value.Trim().ToUpperInvariant() :
            values.ContainsKey(key) ? throw new InvalidDataException($"Invalid key for {key}='{values[key]}' in {path}.") : fallback;

        var config = new ZoomConfig(ReadFloat("Fov", defaults.Fov, 1f, 120f), ReadDuration(),
            ReadFloat("MouseSensitivity", defaults.MouseSensitivity, 0f, 100f),
            ReadKey("ZoomKey", defaults.ZoomKey), ReadKey("NametagKey", defaults.NametagKey),
            ReadKey("AlwaysDayKey", defaults.AlwaysDayKey), ReadKey("FullBrightKey", defaults.FullBrightKey), ReadKey("IndicatorKey", defaults.IndicatorKey), ReadKey("ExitKey", defaults.ExitKey));
        var keys = new[] { config.ZoomKey, config.NametagKey, config.AlwaysDayKey, config.FullBrightKey, config.IndicatorKey, config.ExitKey };
        if (keys.Distinct(StringComparer.OrdinalIgnoreCase).Count() != keys.Length)
            throw new InvalidDataException($"Keyboard shortcuts in {path} must be unique.");
        return config;
    }

    private static bool IsSupportedKey(string text)
    {
        var value = text.Trim().ToUpperInvariant();
        if (value.Length == 1 && (value[0] is >= 'A' and <= 'Z' or >= '0' and <= '9')) return true;
        if (value.Length >= 2 && value[0] == 'F' && int.TryParse(value.AsSpan(1), NumberStyles.None,
            CultureInfo.InvariantCulture, out var functionKey) && functionKey is >= 1 and <= 24) return true;
        return value is "SPACE" or "TAB" or "INSERT" or "HOME" or "END" or "PAGEUP" or "PAGEDOWN" or "DELETE" or "BACKSPACE";
    }

    private static TargetInfo WaitForSupportedGame(TimeSpan timeout)
    {
        var deadline = DateTime.UtcNow + timeout;
        var waitingMessageWritten = false;

        while (DateTime.UtcNow < deadline)
        {
            var processes = Process.GetProcessesByName(ProcessName);
            if (processes.Length > 0)
            {
                foreach (var process in processes)
                {
                    using (process)
                    using (var queryHandle = NativeMethods.OpenProcess(ProcessQueryLimitedInformation, false, process.Id))
                    {
                        if (queryHandle.IsInvalid)
                        {
                            var error = Marshal.GetLastWin32Error();
                            throw new InvalidOperationException(
                                $"Cannot verify process {ProcessName}.exe (PID {process.Id}; Win32 {error}: {new Win32Exception(error).Message}).");
                        }

                        var candidate = InspectProcess(queryHandle, process.Id);
                        EnsureSupportedTarget(candidate);
                        return candidate;
                    }
                }
            }

            if (!waitingMessageWritten)
            {
                Console.WriteLine("Waiting for Minecraft for Windows to be started manually; the game will not be started automatically.");
                waitingMessageWritten = true;
            }

            Thread.Sleep(1_000);
        }

        throw new TimeoutException("No supported Minecraft.Windows.exe process was found within 90 seconds.");
    }

    private static TargetInfo InspectProcess(SafeProcessHandle process, int processId)
    {
        var packageFullName = ReadPackageName(process, family: false);
        var packageFamilyName = ReadPackageName(process, family: true);

        if (!NativeMethods.IsWow64Process2(process, out var processMachine, out var nativeMachine))
        {
            throw new Win32Exception(Marshal.GetLastWin32Error(), "Cannot verify process architecture.");
        }

        var imagePath = ReadImagePath(process);
        return new TargetInfo(processId, imagePath, packageFullName, packageFamilyName, processMachine, nativeMachine);
    }

    private static void EnsureSupportedTarget(TargetInfo target)
    {
        if (!string.Equals(Path.GetFileName(target.ImagePath), ProcessName + ".exe", StringComparison.OrdinalIgnoreCase))
        {
            throw new InvalidOperationException($"Rejected process with unexpected path: {target.ImagePath}");
        }

        if (!string.Equals(target.PackageFamilyName, ExpectedPackageFamily, StringComparison.Ordinal) ||
            !string.Equals(target.PackageFullName, ExpectedPackageFullName, StringComparison.Ordinal))
        {
            throw new InvalidOperationException(
                $"Unsupported game package. Found '{target.PackageFullName}' (family '{target.PackageFamilyName}'); " +
                $"expected '{ExpectedPackageFullName}' (family '{ExpectedPackageFamily}'). The module was not loaded.");
        }

        var isNativeX64 = target.ProcessMachine == ImageFileMachineUnknown && target.NativeMachine == ImageFileMachineAmd64;
        var isX64Process = target.ProcessMachine == ImageFileMachineAmd64 && target.NativeMachine == ImageFileMachineAmd64;
        if (!Environment.Is64BitProcess || (!isNativeX64 && !isX64Process))
        {
            throw new InvalidOperationException(
                $"Expected an x64 launcher and native x64 game; found processMachine=0x{target.ProcessMachine:X4}, " +
                $"nativeMachine=0x{target.NativeMachine:X4}. The module was not loaded.");
        }
    }

    private static string ReadPackageName(SafeProcessHandle process, bool family)
    {
        uint length = 0;
        var firstResult = family
            ? NativeMethods.GetPackageFamilyName(process, ref length, null)
            : NativeMethods.GetPackageFullName(process, ref length, null);

        if (firstResult != ErrorInsufficientBuffer || length == 0)
        {
            throw new InvalidOperationException(
                $"Process package identity is not readable ({(family ? "family" : "full name")}; Win32 {firstResult}).");
        }

        var value = new StringBuilder(checked((int)length));
        var result = family
            ? NativeMethods.GetPackageFamilyName(process, ref length, value)
            : NativeMethods.GetPackageFullName(process, ref length, value);

        if (result != 0)
        {
            throw new Win32Exception(result, $"Cannot read package identity ({(family ? "family" : "full name")}).");
        }

        return value.ToString();
    }

    private static string ReadImagePath(SafeProcessHandle process)
    {
        var buffer = new StringBuilder(32_768);
        uint length = (uint)buffer.Capacity;
        if (!NativeMethods.QueryFullProcessImageName(process, 0, buffer, ref length))
        {
            throw new Win32Exception(Marshal.GetLastWin32Error(), "Cannot verify process path.");
        }

        return buffer.ToString();
    }

    private static void LoadAndStartModule(SafeProcessHandle game, Process targetProcess, string dllPath, ProcessModule? preloadedModule)
    {
        var localModule = NativeMethods.LoadLibraryW(dllPath);
        if (localModule == IntPtr.Zero)
        {
            throw new Win32Exception(Marshal.GetLastWin32Error(), "Cannot load the local GameMod.dll to inspect its exports.");
        }

        try
        {
            var kernel32 = NativeMethods.GetModuleHandleW("kernel32.dll");
            if (kernel32 == IntPtr.Zero)
            {
                throw new Win32Exception(Marshal.GetLastWin32Error(), "Cannot find kernel32.dll.");
            }

            var localLoadLibrary = NativeMethods.GetProcAddress(kernel32, "LoadLibraryW");
            var localStart = NativeMethods.GetProcAddress(localModule, "StartGameMod");
            var localReadiness = NativeMethods.GetProcAddress(localModule, "GetZoomReadiness");
            var localNametagReadiness = NativeMethods.GetProcAddress(localModule, "GetNametagReadiness");
            var localDayReadiness = NativeMethods.GetProcAddress(localModule, "GetAlwaysDayReadiness");
            var localBrightReadiness = NativeMethods.GetProcAddress(localModule, "GetFullBrightReadiness");
            if (localStart == IntPtr.Zero || localReadiness == IntPtr.Zero || localNametagReadiness == IntPtr.Zero || localDayReadiness == IntPtr.Zero || localBrightReadiness == IntPtr.Zero || (preloadedModule is null && localLoadLibrary == IntPtr.Zero))
            {
                throw new Win32Exception(Marshal.GetLastWin32Error(), "An expected export is missing from GameMod.dll or kernel32.dll.");
            }

            if (preloadedModule is null)
            {
                var remoteLoadLibrary = RelocateAddress(targetProcess, localLoadLibrary);
                var pathBytes = Encoding.Unicode.GetBytes(dllPath + '\0');
                var remotePath = NativeMethods.VirtualAllocEx(
                    game,
                    IntPtr.Zero,
                    new UIntPtr((uint)pathBytes.Length),
                    MemReserve | MemCommit,
                    PageReadWrite);
                if (remotePath == IntPtr.Zero)
                {
                    throw new Win32Exception(Marshal.GetLastWin32Error(), "Cannot allocate a temporary path buffer in the target process.");
                }

                var loadThreadCreated = false;
                var loadThreadFinished = false;
                try
                {
                    if (!NativeMethods.WriteProcessMemory(
                        game,
                        remotePath,
                        pathBytes,
                        new UIntPtr((uint)pathBytes.Length),
                        out var bytesWritten))
                    {
                        throw new Win32Exception(Marshal.GetLastWin32Error(), "Cannot write the DLL path to the target process.");
                    }
                    if (bytesWritten.ToUInt64() != (ulong)pathBytes.Length)
                    {
                        throw new InvalidOperationException($"Wrote an incomplete DLL path ({bytesWritten} of {pathBytes.Length} bytes).");
                    }

                    using var loadThread = NativeMethods.CreateRemoteThread(
                    game,
                    IntPtr.Zero,
                    UIntPtr.Zero,
                    remoteLoadLibrary,
                    remotePath,
                    0,
                    out _);
                    if (loadThread.IsInvalid)
                    {
                        throw new Win32Exception(Marshal.GetLastWin32Error(), "Cannot create a remote thread for LoadLibraryW.");
                    }

                    loadThreadCreated = true;
                    var loadWaitResult = NativeMethods.WaitForSingleObject(loadThread, RemoteCallTimeoutMs);
                    if (loadWaitResult == WaitTimeout)
                    {
                        throw new TimeoutException("Remote call to LoadLibraryW did not finish within 30 seconds.");
                    }

                    if (loadWaitResult != WaitObject0)
                    {
                        throw new Win32Exception(Marshal.GetLastWin32Error(), "Waiting for the remote call to LoadLibraryW failed.");
                    }

                    loadThreadFinished = true;
                    if (!NativeMethods.GetExitCodeThread(loadThread, out var loadExitCode))
                    {
                        throw new Win32Exception(Marshal.GetLastWin32Error(), "Cannot read the result of the remote call to LoadLibraryW.");
                    }

                    if (loadExitCode == 0)
                    {
                        throw new InvalidOperationException(
                            "LoadLibraryW failed in the target process. Check whether the game can read the local GameMod.dll.");
                    }
                }
                finally
                {
                    if (!loadThreadCreated || loadThreadFinished)
                    {
                        if (!NativeMethods.VirtualFreeEx(game, remotePath, UIntPtr.Zero, MemRelease))
                        {
                            Console.Error.WriteLine($"Warning: could not free the temporary path buffer (Win32 {Marshal.GetLastWin32Error()}).");
                        }
                    }
                    else
                    {
                        Console.Error.WriteLine("The load thread did not finish; its buffer is being retained to avoid using freed memory.");
                    }
                }

                _ = WaitForModule(targetProcess, "GameMod.dll", dllPath, TimeSpan.FromSeconds(5))
                    ?? throw new InvalidOperationException("GameMod.dll did not appear in the game module list after LoadLibraryW completed.");
            }
            else
            {
                Console.WriteLine("GameMod.dll is already loaded from the same path in the process; proceeding with initialization only.");
            }

            var remoteStart = RelocateAddress(targetProcess, localStart);

            var startThreadCreated = false;
            var startThreadFinished = false;
            try
            {
                var startExitCode = RunRemoteThread(
                    game,
                    remoteStart,
                    IntPtr.Zero,
                    "StartGameMod",
                    out startThreadCreated,
                    out startThreadFinished);
                if (startExitCode == 2)
                {
                    Console.WriteLine("The module is already running; Zoom preparation and state are preserved.");
                }
                else if (startExitCode != 1)
                {
                    throw new InvalidOperationException(
                        $"GameMod.dll initialization returned exit code {startExitCode}. The module remains loaded " +
                        "because this code may indicate that its worker thread is already running.");
                }
            }
            catch
            {
                if (startThreadCreated && !startThreadFinished)
                {
                    Console.Error.WriteLine("The initialization thread may still be running; the DLL was not unloaded to avoid canceling running code.");
                }

                throw;
            }

            // Initialization is asynchronous. Report actual readiness, never
            // equate a successfully created worker with a verified FOV target.
            var remoteReadiness = RelocateAddress(targetProcess, localReadiness);
            var preparation = Stopwatch.StartNew();
            uint readiness;
            do
            {
                readiness = RunRemoteThread(game, remoteReadiness, IntPtr.Zero, "GetZoomReadiness", out _, out _);
                if (readiness is 2 or 3) break;
                Thread.Sleep(100);
            } while (preparation.Elapsed < TimeSpan.FromSeconds(2));

            Console.WriteLine(readiness switch
            {
                2 => $"Zoom ready: a unique FOV entry was verified (check took {preparation.ElapsedMilliseconds} ms).",
                3 => "Zoom is refusing writes for now: the target is not uniquely verified. See the diagnostic log for details; transient failures are retried automatically.",
                _ => "Zoom is preparing automatically in the background. The Zoom key will work only after unique verification; check the status indicator and diagnostic log for progress."
            });
            var nametagReadiness = RunRemoteThread(game, RelocateAddress(targetProcess, localNametagReadiness),
                IntPtr.Zero, "GetNametagReadiness", out _, out _);
            Console.WriteLine(nametagReadiness switch
            {
                2 => "Nametag ready; press its configured key to enable it.",
                3 => "Nametag rejected: the native rendering profile was not safely verified or prepared. See the log for details.",
                _ => "Nametag is preparing; its key will be available after the rendering profile and camera settings are verified."
            });
            var dayReadiness = RunRemoteThread(game, RelocateAddress(targetProcess, localDayReadiness),
                IntPtr.Zero, "GetAlwaysDayReadiness", out _, out _);
            Console.WriteLine(dayReadiness switch
            {
                2 => "Always day: rendering profile ready; the feature is disabled by default. In-game behavior awaits verification.",
                3 => "Always day rejected: profile validation or safe installation failed. See the log for details.",
                _ => "Always day is preparing; check the indicator and diagnostic log for status."
            });
            var brightReadiness = RunRemoteThread(game, RelocateAddress(targetProcess, localBrightReadiness),
                IntPtr.Zero, "GetFullBrightReadiness", out _, out _);
            Console.WriteLine(brightReadiness switch
            {
                2 => "Full Bright: rendering profile ready; the feature is disabled by default. The result awaits in-game verification.",
                3 => "Full Bright rejected: profile validation or safe installation failed. See the log for details.",
                _ => "Full Bright is preparing; check the indicator and diagnostic log for status."
            });
        }
        finally
        {
            _ = NativeMethods.FreeLibrary(localModule);
        }
    }

    private static uint RunRemoteThread(
        SafeProcessHandle process,
        IntPtr startAddress,
        IntPtr parameter,
        string operation,
        out bool threadCreated,
        out bool threadFinished)
    {
        threadCreated = false;
        threadFinished = false;
        using var thread = NativeMethods.CreateRemoteThread(process, IntPtr.Zero, UIntPtr.Zero, startAddress, parameter, 0, out _);
        if (thread.IsInvalid)
        {
            throw new Win32Exception(Marshal.GetLastWin32Error(), $"Cannot create a remote thread for {operation}.");
        }

        threadCreated = true;
        var waitResult = NativeMethods.WaitForSingleObject(thread, RemoteCallTimeoutMs);
        if (waitResult == WaitTimeout)
        {
            throw new TimeoutException($"Remote call to {operation} did not finish within 30 seconds.");
        }

        if (waitResult != WaitObject0)
        {
            throw new Win32Exception(Marshal.GetLastWin32Error(), $"Waiting for remote call {operation} failed.");
        }

        threadFinished = true;
        if (!NativeMethods.GetExitCodeThread(thread, out var exitCode))
        {
            throw new Win32Exception(Marshal.GetLastWin32Error(), $"Cannot read the result of remote call {operation}.");
        }

        return exitCode;
    }

    private static IntPtr RelocateAddress(Process targetProcess, IntPtr localFunction)
    {
        using var currentProcess = Process.GetCurrentProcess();
        ProcessModule? localOwner = null;
        foreach (ProcessModule module in currentProcess.Modules)
        {
            var moduleStart = module.BaseAddress.ToInt64();
            var moduleEnd = checked(moduleStart + module.ModuleMemorySize);
            if (localFunction.ToInt64() >= moduleStart && localFunction.ToInt64() < moduleEnd)
            {
                localOwner = module;
                break;
            }
        }

        if (localOwner is null)
        {
            throw new InvalidOperationException("Cannot determine which module owns the remotely called export.");
        }

        var remoteOwner = FindModule(targetProcess, localOwner.ModuleName)
            ?? throw new InvalidOperationException($"Module {localOwner.ModuleName} is missing from the target process.");
        var relativeAddress = localFunction.ToInt64() - localOwner.BaseAddress.ToInt64();
        if (relativeAddress < 0 || relativeAddress >= remoteOwner.ModuleMemorySize)
        {
            throw new InvalidOperationException($"The export is outside the range of module {localOwner.ModuleName}.");
        }

        return new IntPtr(checked(remoteOwner.BaseAddress.ToInt64() + relativeAddress));
    }

    private static ProcessModule? FindModule(Process process, string moduleName)
    {
        process.Refresh();
        foreach (ProcessModule module in process.Modules)
        {
            if (string.Equals(module.ModuleName, moduleName, StringComparison.OrdinalIgnoreCase))
            {
                return module;
            }
        }

        return null;
    }

    private static ProcessModule? WaitForModule(Process process, string moduleName, string expectedPath, TimeSpan timeout)
    {
        var deadline = DateTime.UtcNow + timeout;
        do
        {
            var module = FindModule(process, moduleName);
            if (module is not null)
            {
                if (!string.Equals(Path.GetFullPath(module.FileName), expectedPath, StringComparison.OrdinalIgnoreCase))
                {
                    throw new InvalidOperationException($"A different module with the same name is already loaded in the process: {module.FileName}");
                }

                return module;
            }

            Thread.Sleep(100);
        }
        while (DateTime.UtcNow < deadline);

        return null;
    }

    private sealed record TargetInfo(
        int ProcessId,
        string ImagePath,
        string PackageFullName,
        string PackageFamilyName,
        ushort ProcessMachine,
        ushort NativeMachine);

    private static class NativeMethods
    {
        [DllImport("kernel32.dll", SetLastError = true)]
        internal static extern SafeProcessHandle OpenProcess(uint desiredAccess, bool inheritHandle, int processId);

        [DllImport("kernel32.dll", SetLastError = true)]
        internal static extern bool CloseHandle(IntPtr handle);

        [DllImport("kernel32.dll", SetLastError = true)]
        internal static extern bool IsWow64Process2(SafeProcessHandle process, out ushort processMachine, out ushort nativeMachine);

        [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
        internal static extern int GetPackageFullName(SafeProcessHandle process, ref uint packageFullNameLength, StringBuilder? packageFullName);

        [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
        internal static extern int GetPackageFamilyName(SafeProcessHandle process, ref uint packageFamilyNameLength, StringBuilder? packageFamilyName);

        [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
        internal static extern bool QueryFullProcessImageName(SafeProcessHandle process, uint flags, StringBuilder imageName, ref uint size);

        [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
        internal static extern IntPtr LoadLibraryW(string fileName);

        [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
        internal static extern IntPtr GetModuleHandleW(string moduleName);

        [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Ansi)]
        internal static extern IntPtr GetProcAddress(IntPtr module, string procedureName);

        [DllImport("kernel32.dll", SetLastError = true)]
        internal static extern bool FreeLibrary(IntPtr module);

        [DllImport("kernel32.dll", SetLastError = true)]
        internal static extern IntPtr VirtualAllocEx(SafeProcessHandle process, IntPtr address, UIntPtr size, uint allocationType, uint protect);

        [DllImport("kernel32.dll", SetLastError = true)]
        internal static extern bool VirtualFreeEx(SafeProcessHandle process, IntPtr address, UIntPtr size, uint freeType);

        [DllImport("kernel32.dll", SetLastError = true)]
        internal static extern bool WriteProcessMemory(SafeProcessHandle process, IntPtr baseAddress, byte[] buffer, UIntPtr size, out UIntPtr bytesWritten);

        [DllImport("kernel32.dll", SetLastError = true)]
        internal static extern SafeWaitHandle CreateRemoteThread(SafeProcessHandle process, IntPtr threadAttributes, UIntPtr stackSize, IntPtr startAddress, IntPtr parameter, uint creationFlags, out uint threadId);

        [DllImport("kernel32.dll", SetLastError = true)]
        internal static extern uint WaitForSingleObject(SafeWaitHandle handle, uint milliseconds);

        [DllImport("kernel32.dll", SetLastError = true)]
        internal static extern bool GetExitCodeThread(SafeWaitHandle thread, out uint exitCode);
    }
}
