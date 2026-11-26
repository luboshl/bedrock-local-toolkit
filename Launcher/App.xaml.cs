using System.Windows;

namespace MinecraftClient.Launcher;

public partial class App : Application
{
    private SingleInstance? instance;

    protected override void OnStartup(StartupEventArgs e)
    {
        base.OnStartup(e);
        try
        {
            instance = new SingleInstance();
            if (!instance.IsPrimary)
            {
                instance.ActivatePrimary();
                Shutdown();
                return;
            }

            var window = new MainWindow();
            MainWindow = window;
            _ = instance.ListenAsync(() => Dispatcher.InvokeAsync(window.BringForward));
            window.Show();
        }
        catch (Exception ex)
        {
            LauncherLog.Write(ex.ToString());
            MessageBox.Show(ex.Message, "Bedrock Local Toolkit", MessageBoxButton.OK, MessageBoxImage.Error);
            Shutdown(1);
        }
    }

    protected override void OnExit(ExitEventArgs e)
    {
        instance?.Dispose();
        base.OnExit(e);
    }
}
