using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Windows;

namespace MinecraftClient.Launcher;

public partial class MainWindow : Window
{
    private ToolkitSettings? settings;
    private CancellationTokenSource? startup;
    private bool closeRequested;

    public MainWindow()
    {
        InitializeComponent();
        ShortcutHelp.Text = ToolkitSettings.ShortcutHelp;
    }

    internal void BringForward()
    {
        closeRequested = false;
        if (!IsVisible)
            Show();
        if (WindowState == WindowState.Minimized)
            WindowState = WindowState.Normal;
        Activate();
    }

    private async void OnContentRendered(object? sender, EventArgs e)
    {
        ContentRendered -= OnContentRendered;
        LoadSettings();
        await StartGameAsync();
    }

    private async void OnRetry(object sender, RoutedEventArgs e) => await StartGameAsync();

    private async Task StartGameAsync()
    {
        if (startup is not null)
            return;

        using var operation = new CancellationTokenSource();
        startup = operation;
        SaveButton.IsEnabled = false;
        RetryButton.Visibility = Visibility.Collapsed;
        StartupHeading.Text = "Starting the toolkit…";
        StartupDetail.Text = "";
        StartupProgress.Visibility = Visibility.Visible;
        var progress = new Progress<string>(message =>
        {
            if (startup == operation)
                StartupDetail.Text = message;
        });
        try
        {
            StartupDetail.Text = await GameStartup.RunAsync(progress, operation.Token);
            StartupHeading.Text = "Module started";
        }
        catch (OperationCanceledException) when (operation.IsCancellationRequested)
        {
            StartupHeading.Text = "Startup canceled";
            StartupDetail.Text = "Select Retry to start the toolkit.";
            RetryButton.Visibility = Visibility.Visible;
        }
        catch (Exception ex)
        {
            var logged = LauncherLog.Write($"Startup failed: {ex}");
            StartupHeading.Text = "Startup failed";
            StartupDetail.Text = logged ? ex.Message : $"{ex.Message} The launcher log could not be written.";
            RetryButton.Visibility = Visibility.Visible;
        }
        finally
        {
            startup = null;
            StartupProgress.Visibility = Visibility.Collapsed;
            SaveButton.IsEnabled = settings is not null;
            if (closeRequested)
                Close();
        }
    }

    private void OnClosing(object? sender, CancelEventArgs e)
    {
        if (startup is null)
            return;

        // Close the window immediately and let any in-flight module call finish before releasing the instance lock.
        e.Cancel = true;
        closeRequested = true;
        Hide();
        startup.Cancel();
    }

    private void OnOpenLog(object sender, RoutedEventArgs e)
    {
        try
        {
            if (!File.Exists(LauncherLog.FilePath) && !LauncherLog.Write("Launcher log opened."))
                throw new IOException("The launcher log is unavailable.");
            using var viewer = Process.Start(new ProcessStartInfo(LauncherLog.FilePath) { UseShellExecute = true });
            ShellFeedback.Text = "";
        }
        catch (Exception ex)
        {
            ShellFeedback.Text = $"Could not open the launcher log. {ex.Message}";
        }
    }

    private void OnAddToStartMenu(object sender, RoutedEventArgs e)
    {
        try
        {
            StartMenuShortcut.Create();
            ShellFeedback.Text = "Shortcut saved. Search Start for Bedrock Local Toolkit to open or pin it.";
        }
        catch (Exception ex)
        {
            LauncherLog.Write($"Start menu shortcut failed: {ex}");
            ShellFeedback.Text = $"Could not add the Start menu shortcut. {ex.Message}";
        }
    }

    private void OnReloadSettings(object sender, RoutedEventArgs e) => LoadSettings();

    private void LoadSettings()
    {
        try
        {
            BindSettings(ToolkitSettings.Load());
            SettingsFeedback.Text = settings!.Validate() ? "" : "Correct the highlighted settings before starting the toolkit.";
        }
        catch (Exception ex)
        {
            BindSettings(null);
            SettingsFeedback.Text = $"Could not read bedrock-toolkit.ini. {ex.Message}";
        }
    }

    private void OnSaveSettings(object sender, RoutedEventArgs e)
    {
        if (settings is null || startup is not null)
            return;

        try
        {
            BindSettings(settings.Save());
            SettingsFeedback.Text = "Settings saved.";
        }
        catch (Exception ex)
        {
            SettingsFeedback.Text = ex.Message;
        }
    }

    private void BindSettings(ToolkitSettings? newSettings)
    {
        settings = newSettings;
        ShortcutFields.ItemsSource = settings?.ShortcutsFields;
        ZoomFields.ItemsSource = settings?.ZoomFields;
        NametagFields.ItemsSource = settings?.NametagFields;
        FpsFields.ItemsSource = settings?.FpsFields;
        CompatibilityFields.ItemsSource = settings?.CompatibilityFields;
        SaveButton.IsEnabled = settings is not null && startup is null;
    }
}
