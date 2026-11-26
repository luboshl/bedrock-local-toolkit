using System.ComponentModel;
using System.Globalization;
using System.IO;

namespace MinecraftClient.Launcher;

internal sealed class ToolkitSettings
{
    internal static string FilePath { get; } = Path.Combine(AppContext.BaseDirectory, "bedrock-toolkit.ini");
    internal const string ShortcutHelp =
        "Use A–Z, 0–9, F1–F24, Space, Tab, Insert, Home, End, PageUp, PageDown, Delete or Backspace. " +
        "Each binding must be unique. K also opens Client Menu when no other shortcut uses it.";

    private const string Zoom = "Zoom";
    private const string Shortcuts = "Shortcuts";
    private const string Fps = "fps_counter";
    private const string Compatibility = "Compatibility";
    private const string AllowUnsupported = "AllowUnsupportedGameVersion";
    private readonly List<SettingField> fields = [];
    private readonly string path;

    private ToolkitSettings(string path, IniDocument document)
    {
        this.path = path;

        AddShortcut("Zoom (hold)", "Zoom", "T");
        AddShortcut("Nametag", "Nametag", "F7");
        AddShortcut("Always day", "AlwaysDay", "F6");
        AddShortcut("Full Bright", "FullBright", "F8");
        AddShortcut("Client Menu (alternate)", "StatusIndicator", "F9");
        AddShortcut("Safe detach", "SafeDetach", "F10");

        var fov = NumberInRange(1, 120);
        var legacy = document.Get(Zoom, "Fov");
        var legacyFov = legacy is null ? null : fov(legacy);
        if (legacy is not null && legacyFov is null)
            Add(Zoom, "Fov", "Legacy FOV fallback (1–120°)", "25", fov);
        Add(Zoom, "FirstPersonFov", "First-person FOV (1–120°)", legacyFov ?? "25", fov);
        Add(Zoom, "ThirdPersonFov", "Third-person FOV (1–120°)", legacyFov ?? "29", fov);
        Add(Zoom, "TransitionDurationMs", "Transition time (1–10,000 ms)", "75", IntegerInRange(1, 10_000));
        Add(Zoom, "MouseSensitivity", "Sensitivity at minimum FOV (0–100)", "45", NumberInRange(0, 100));
        Add(Zoom, "SensitivityScalingStrength", "Sensitivity scaling strength (1–2)", "1.5", NumberInRange(1, 2));
        Add(Zoom, "CameraSmoothingEnabled", "Smooth mouse rotation during Zoom", "false", NormalizeBoolean, toggle: true);
        Add(Zoom, "CameraSmoothingStrength", "Camera smoothing strength (1–10)", "5", NumberInRange(1, 10));
        Add("nametag", "show_when_hud_hidden", "Show your nametag when the HUD is hidden", "true",
            NormalizeBoolean, toggle: true);
        Add(Fps, "enabled", "Show FPS counter", "true", NormalizeBoolean, toggle: true, invalidFallback: "false");
        Add(Fps, "font_size", "FPS font size (12–48 px)", "28", IntegerInRange(12, 48), invalidFallback: "28");
        Add(Compatibility, AllowUnsupported, "Allow unsupported game versions (experimental)", "false",
            NormalizeBoolean, toggle: true, invalidFallback: "false");

        void AddShortcut(string label, string key, string fallback) =>
            Add(Shortcuts, key, label, fallback, NormalizeShortcut);

        void Add(string section, string key, string label, string fallback, Func<string, string?> normalize,
            bool toggle = false, string? invalidFallback = null)
        {
            var stored = document.Get(section, key);
            var effective = stored is null ? fallback : normalize(stored) ?? invalidFallback ?? stored;
            fields.Add(new SettingField(section, key, label, effective, normalize, toggle));
        }
    }

    internal static ToolkitSettings Load(string? path = null)
    {
        path ??= FilePath;
        return new ToolkitSettings(path, IniDocument.Load(path));
    }

    internal IEnumerable<SettingField> ShortcutsFields => ForSection(Shortcuts);
    internal IEnumerable<SettingField> ZoomFields => ForSection(Zoom);
    internal IEnumerable<SettingField> NametagFields => ForSection("nametag");
    internal IEnumerable<SettingField> FpsFields => ForSection(Fps);
    internal IEnumerable<SettingField> CompatibilityFields => ForSection(Compatibility);
    internal bool AllowUnsupportedVersion => CompatibilityFields.Single().Checked == true;

    internal bool Validate()
    {
        foreach (var field in fields)
        {
            field.Error = field.Normalized is not null ? "" : field.Section == Shortcuts ?
                "Enter a supported key." : field.IsToggle ? "Choose on or off." :
                "Enter a number in the indicated range. Use a decimal point for fractions.";
        }

        var duplicates = ShortcutsFields.Where(field => field.Normalized is not null)
            .GroupBy(field => field.Normalized).Where(group => group.Count() > 1);
        foreach (var group in duplicates)
            foreach (var field in group)
                field.Error = $"{group.Key} is also used by {string.Join(", ", group.Where(other => other != field).Select(other => other.Label))}.";

        return fields.All(field => field.Error.Length == 0);
    }

    internal ToolkitSettings Save()
    {
        if (!Validate())
            throw new InvalidDataException("Correct the highlighted settings before saving.");

        var document = IniDocument.Load(path);
        var changed = fields.Where(field => field.Normalized != field.Original).ToArray();
        foreach (var field in changed)
            document.Set(field.Section, field.Key, field.Normalized!);

        var merged = new ToolkitSettings(path, document);
        if (!merged.Validate())
            throw new InvalidDataException("The INI contains conflicting or invalid settings on disk. Reload Settings to review them before saving.");

        if (changed.Length > 0)
            document.Save();
        return merged;
    }

    private IEnumerable<SettingField> ForSection(string section) => fields.Where(field => field.Section == section);

    private static string? NormalizeBoolean(string value) =>
        bool.TryParse(value.Trim(), out var parsed) ? (parsed ? "true" : "false") : null;

    private static Func<string, string?> NumberInRange(float minimum, float maximum) => value =>
        float.TryParse(value, NumberStyles.Float, CultureInfo.InvariantCulture, out var parsed) &&
        float.IsFinite(parsed) && parsed >= minimum && parsed <= maximum
            ? parsed.ToString("G", CultureInfo.InvariantCulture) : null;

    private static Func<string, string?> IntegerInRange(int minimum, int maximum) => value =>
        int.TryParse(value, NumberStyles.Integer, CultureInfo.InvariantCulture, out var parsed) &&
        parsed >= minimum && parsed <= maximum ? parsed.ToString(CultureInfo.InvariantCulture) : null;

    private static string? NormalizeShortcut(string text)
    {
        var value = text.Trim().ToUpperInvariant();
        if (value.Length == 1 && value[0] is >= 'A' and <= 'Z' or >= '0' and <= '9')
            return value;
        if (value.Length >= 2 && value[0] == 'F' && int.TryParse(value.AsSpan(1), NumberStyles.None,
            CultureInfo.InvariantCulture, out var number) && number is >= 1 and <= 24)
            return $"F{number}";
        return value is "SPACE" or "TAB" or "INSERT" or "HOME" or "END" or "PAGEUP" or "PAGEDOWN" or "DELETE" or "BACKSPACE"
            ? value : null;
    }
}

internal sealed class SettingField(string section, string key, string label, string original,
    Func<string, string?> normalize, bool toggle) : INotifyPropertyChanged
{
    private string error = "";

    public string Section { get; } = section;
    public string Key { get; } = key;
    public string Label { get; } = label;
    public bool IsToggle { get; } = toggle;
    public string Value { get; set; } = original;
    public string Error
    {
        get => error;
        set
        {
            if (value == error)
                return;
            error = value;
            PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(nameof(Error)));
        }
    }
    internal string? Original { get; } = normalize(original);
    internal string? Normalized => normalize(Value);

    public bool? Checked
    {
        get => bool.TryParse(Value, out var value) ? value : null;
        set => Value = value.HasValue ? (value.Value ? "true" : "false") : "";
    }

    public event PropertyChangedEventHandler? PropertyChanged;
}
