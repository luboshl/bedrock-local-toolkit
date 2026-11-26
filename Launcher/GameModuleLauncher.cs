using System.IO;
using System.ComponentModel;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;
using Microsoft.Win32.SafeHandles;

namespace MinecraftClient.Launcher;

internal static class GameModuleLauncher
{
    internal const string ProcessName = "Minecraft.Windows";
    internal const string PackageFamily = "Microsoft.MinecraftUWP_8wekyb3d8bbwe";
    private const string ExpectedPackageFullName = "Microsoft.MinecraftUWP_1.26.6501.0_x64__8wekyb3d8bbwe";
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

    internal static void Start(int processId, bool allowUnsupportedGameVersion,
        Action<string> log, CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        using var queryHandle = NativeMethods.OpenProcess(ProcessQueryLimitedInformation, false, processId);
        if (queryHandle.IsInvalid)
        {
            var error = Marshal.GetLastWin32Error();
            throw new InvalidOperationException(
                $"Cannot verify {ProcessName}.exe (PID {processId}; Win32 {error}: {new Win32Exception(error).Message}).");
        }

        var target = InspectProcess(queryHandle, processId);
        EnsureSupportedTarget(target, allowUnsupportedGameVersion);
        log($"Verified target: PID {processId}, {target.PackageFullName}, x64.");

        using var game = NativeMethods.OpenProcess(RequiredRemoteLoadAccess, false, processId);
        if (game.IsInvalid)
        {
            var error = Marshal.GetLastWin32Error();
            throw new InvalidOperationException(
                $"Windows denied the required process access (Win32 {error}: {new Win32Exception(error).Message}). " +
                "GameMod.dll was not loaded. Run Minecraft and the toolkit as the same Windows user.");
        }

        var recheckedTarget = InspectProcess(game, processId);
        EnsureSupportedTarget(recheckedTarget, allowUnsupportedGameVersion);
        log("Remote-load access check passed.");

        var dllPath = Path.Combine(AppContext.BaseDirectory, "GameMod.dll");
        if (!File.Exists(dllPath))
            throw new FileNotFoundException("GameMod.dll is missing. Re-extract the complete release ZIP.", dllPath);

        if (!File.Exists(Path.Combine(AppContext.BaseDirectory, "zoom-pointer.ini")))
            log("Saved pointer chain not found; using automatic FOV validation for the supported build.");

        using var targetProcess = Process.GetProcessById(processId);
        var preloadedModule = FindModule(targetProcess, "GameMod.dll");
        if (preloadedModule is not null &&
            !string.Equals(Path.GetFullPath(preloadedModule.FileName), dllPath, StringComparison.OrdinalIgnoreCase))
        {
            throw new InvalidOperationException(
                $"GameMod.dll from another toolkit folder is already loaded: {preloadedModule.FileName}. " +
                "Save and restart Minecraft before using this copy.");
        }

        cancellationToken.ThrowIfCancellationRequested();
        LoadAndStartModule(game, targetProcess, dllPath, preloadedModule, log);
        log("GameMod.dll is loaded and initialization has started.");
        foreach (var feature in new[] { "zoom", "nametag", "always-day", "full-bright" })
            log($"Module diagnostics: {Path.Combine(AppContext.BaseDirectory, $"{feature}-status-{processId}.log")}");
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

    private static void EnsureSupportedTarget(TargetInfo target, bool allowUnsupportedGameVersion)
    {
        if (!string.Equals(Path.GetFileName(target.ImagePath), ProcessName + ".exe", StringComparison.OrdinalIgnoreCase))
        {
            throw new InvalidOperationException($"Rejected process with unexpected path: {target.ImagePath}");
        }

        if (!string.Equals(target.PackageFamilyName, PackageFamily, StringComparison.Ordinal))
        {
            throw new InvalidOperationException(
                $"Rejected package family '{target.PackageFamilyName}'; expected '{PackageFamily}'. The module was not loaded.");
        }

        if (!allowUnsupportedGameVersion &&
            !string.Equals(target.PackageFullName, ExpectedPackageFullName, StringComparison.Ordinal))
        {
            throw new InvalidOperationException(
                $"Unsupported game package. Found '{target.PackageFullName}' (family '{target.PackageFamilyName}'); " +
                $"expected '{ExpectedPackageFullName}' (family '{PackageFamily}'). The module was not loaded. Use a toolkit release that supports your installed game version.");
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

    private static void LoadAndStartModule(SafeProcessHandle game, Process targetProcess, string dllPath, ProcessModule? preloadedModule, Action<string> log)
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
                            log($"Warning: could not free the temporary path buffer (Win32 {Marshal.GetLastWin32Error()}).");
                        }
                    }
                    else
                    {
                        log("The load thread did not finish; its buffer is being retained to avoid using freed memory.");
                    }
                }

                _ = WaitForModule(targetProcess, "GameMod.dll", dllPath, TimeSpan.FromSeconds(5))
                    ?? throw new InvalidOperationException("GameMod.dll did not appear in the game module list after LoadLibraryW completed.");
            }
            else
            {
                log("GameMod.dll is already loaded from the same path in the process; proceeding with initialization only.");
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
                    log("The module is already running; Zoom preparation and state are preserved.");
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
                    log("The initialization thread may still be running; the DLL was not unloaded to avoid canceling running code.");
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

            log(readiness switch
            {
                2 => $"Zoom ready: a unique FOV entry was verified (check took {preparation.ElapsedMilliseconds} ms).",
                3 => "Zoom is refusing writes for now: the target is not uniquely verified. See the diagnostic log for details; transient failures are retried automatically.",
                _ => "Zoom is preparing automatically in the background. The Zoom key will work only after unique verification; check the Client Menu and diagnostic log for progress."
            });
            var nametagReadiness = RunRemoteThread(game, RelocateAddress(targetProcess, localNametagReadiness),
                IntPtr.Zero, "GetNametagReadiness", out _, out _);
            log(nametagReadiness switch
            {
                2 => "Nametag ready and enabled by default; its configured key toggles it.",
                3 => "Nametag rejected: the native rendering profile was not safely verified or prepared. See the log for details.",
                _ => "Nametag is preparing; its key will be available after the rendering profile and camera settings are verified."
            });
            var dayReadiness = RunRemoteThread(game, RelocateAddress(targetProcess, localDayReadiness),
                IntPtr.Zero, "GetAlwaysDayReadiness", out _, out _);
            log(dayReadiness switch
            {
                2 => "Always day: rendering profile ready and enabled by default. In-game behavior awaits verification.",
                3 => "Always day rejected: profile validation or safe installation failed. See the log for details.",
                _ => "Always day is preparing; check the Client Menu and diagnostic log for status."
            });
            var brightReadiness = RunRemoteThread(game, RelocateAddress(targetProcess, localBrightReadiness),
                IntPtr.Zero, "GetFullBrightReadiness", out _, out _);
            log(brightReadiness switch
            {
                2 => "Full Bright: rendering profile ready and enabled by default. The result awaits in-game verification.",
                3 => "Full Bright rejected: profile validation or safe installation failed. See the log for details.",
                _ => "Full Bright is preparing; check the Client Menu and diagnostic log for status."
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
