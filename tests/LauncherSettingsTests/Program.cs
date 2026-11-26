using System.Text;
using MinecraftClient.Launcher;

var directory = Directory.CreateTempSubdirectory();
try
{
    var path = Path.Combine(directory.FullName, "bedrock-toolkit.ini");
    var defaults = ToolkitSettings.Load(path);
    Check(defaults.Validate() && !defaults.AllowUnsupportedVersion, "missing INI uses strict defaults");
    Check(Find(defaults.ZoomFields, "TransitionDurationMs").Value == "75" &&
          Find(defaults.ZoomFields, "CameraSmoothingEnabled").Checked == false, "missing Zoom settings use built-in defaults");

    var iniEncoding = new UTF8Encoding(true);
    const string original = """
        [Zoom]
        Fov=19
        FirstPersonFov=22
        [Shortcuts]
        Zoom = t
        zoom=W
        Nametag=F7
        [Compatibility]
        AllowUnsupportedGameVersion=unknown
        [fps_counter]
        enabled=unknown
        font_size=99
        ; auxiliary settings
        [User]
        Favorite=Café menu
        """;
    File.WriteAllText(path, original, iniEncoding);
    var settings = ToolkitSettings.Load(path);
    Check(Find(settings.ZoomFields, "FirstPersonFov").Value == "22" &&
          Find(settings.ZoomFields, "ThirdPersonFov").Value == "19", "legacy FOV applies only to missing values");
    Check(!settings.AllowUnsupportedVersion && Find(settings.FpsFields, "enabled").Checked == false &&
          Find(settings.FpsFields, "font_size").Value == "28", "invalid optional values use their effective fallbacks");

    var zoomKey = Find(settings.ShortcutsFields, "Zoom");
    var nametagKey = Find(settings.ShortcutsFields, "Nametag");
    Check(zoomKey.Value == "T", "the first shortcut occurrence wins");
    zoomKey.Value = "Alt+T";
    Check(!settings.Validate() && zoomKey.Error.Length > 0, "invalid key produces field feedback");
    zoomKey.Value = "f07";
    Check(!settings.Validate() && zoomKey.Error.Length > 0 && nametagKey.Error.Length > 0,
        "aliases for the same virtual key produce feedback on both fields");
    zoomKey.Value = "K";
    Check(settings.Validate(), "K can be assigned to a feature and disables the implicit menu binding");
    zoomKey.Value = "C";
    Find(settings.FpsFields, "font_size").Value = "36";
    File.AppendAllText(path, "\nExternal=kept", iniEncoding);
    settings.Save();
    var bytes = File.ReadAllBytes(path);
    Check(bytes.AsSpan().StartsWith(iniEncoding.Preamble), "save preserves UTF-8 BOM");
    Check(File.ReadAllText(path) == original.Replace("Zoom = t", "Zoom = C").Replace("font_size=99", "font_size=36") +
          "\nExternal=kept", "save updates only edited entries and keeps external edits, duplicate entries, comments and Unicode");
    Check(Find(ToolkitSettings.Load(path).ShortcutsFields, "Zoom").Value == "C", "shortcut survives reopening");

    File.WriteAllText(path, "[Zoom]\r\n[zoom]\r\nFov=12\r\n");
    Check(Find(ToolkitSettings.Load(path).ZoomFields, "FirstPersonFov").Value == "25",
        "conventional INI uses the first section, as Win32 does");
    Console.WriteLine("Launcher settings tests passed.");
}
finally
{
    directory.Delete(recursive: true);
}

static SettingField Find(IEnumerable<SettingField> fields, string key) => fields.Single(field => field.Key == key);

static void Check(bool condition, string description)
{
    if (!condition)
        throw new InvalidOperationException(description);
}
