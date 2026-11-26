using System.Diagnostics;
using System.IO;
using System.IO.Pipes;
using System.Runtime.InteropServices;
using System.Security.Principal;
using Microsoft.Win32.SafeHandles;

namespace MinecraftClient.Launcher;

internal sealed class SingleInstance : IDisposable
{
    private readonly string pipeName;
    private readonly Mutex mutex;
    private readonly CancellationTokenSource shutdown = new();

    internal SingleInstance()
    {
        using var identity = WindowsIdentity.GetCurrent();
        using var process = Process.GetCurrentProcess();
        pipeName = $"BedrockLocalToolkit-{identity.User?.Value}-{process.SessionId}";
        mutex = new Mutex(false, $@"Local\{pipeName}");
        try
        {
            IsPrimary = mutex.WaitOne(0);
        }
        catch (AbandonedMutexException)
        {
            IsPrimary = true;
        }
    }

    internal bool IsPrimary { get; }

    internal void ActivatePrimary()
    {
        using var pipe = new NamedPipeClientStream(".", pipeName, PipeDirection.Out, PipeOptions.CurrentUserOnly);
        try
        {
            pipe.Connect(5_000);
        }
        catch (TimeoutException ex)
        {
            throw new InvalidOperationException(
                "The toolkit window could not be reached. Wait for the previous launch to finish, then open the toolkit again.", ex);
        }

        if (GetNamedPipeServerProcessId(pipe.SafePipeHandle, out var processId))
            _ = AllowSetForegroundWindow(processId);
        pipe.WriteByte(1);
    }

    internal async Task ListenAsync(Action activate)
    {
        var cancellationToken = shutdown.Token;
        try
        {
            while (!cancellationToken.IsCancellationRequested)
            {
                using var pipe = new NamedPipeServerStream(pipeName, PipeDirection.In, 1,
                    PipeTransmissionMode.Byte, PipeOptions.Asynchronous | PipeOptions.CurrentUserOnly);
                await pipe.WaitForConnectionAsync(cancellationToken);
                var request = new byte[1];
                using var readTimeout = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
                readTimeout.CancelAfter(TimeSpan.FromSeconds(2));
                try
                {
                    if (await pipe.ReadAsync(request, readTimeout.Token) == 1 && request[0] == 1)
                        activate();
                }
                catch (OperationCanceledException) when (!cancellationToken.IsCancellationRequested)
                {
                    // A client connected but did not submit an activation request.
                }
                catch (IOException ex)
                {
                    LauncherLog.Write($"Repeat activation disconnected: {ex.Message}");
                }
            }
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
        {
        }
        catch (Exception ex)
        {
            LauncherLog.Write($"Repeat activation listener stopped: {ex}");
        }
    }

    public void Dispose()
    {
        shutdown.Cancel();
        shutdown.Dispose();
        if (IsPrimary)
            mutex.ReleaseMutex();
        mutex.Dispose();
    }

    [DllImport("kernel32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool GetNamedPipeServerProcessId(SafePipeHandle pipe, out uint serverProcessId);

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool AllowSetForegroundWindow(uint processId);
}
