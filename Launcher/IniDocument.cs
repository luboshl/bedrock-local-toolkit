using System.IO;
using System.Text;
using System.Text.RegularExpressions;

namespace MinecraftClient.Launcher;

internal sealed class IniDocument
{
    private static readonly Regex Lines = new(@"(?<text>[^\r\n]*)(?<ending>\r\n|\r|\n|\z)");
    private static readonly Encoding Utf8Bom = new UTF8Encoding(true, true);
    private static readonly Encoding Utf16 = new UnicodeEncoding(false, true, true);
    private readonly byte[]? original;
    private readonly string path;
    private readonly List<string> lines = [];
    private readonly string newline;
    private readonly Encoding encoding;

    private IniDocument(string path, byte[]? original)
    {
        this.path = path;
        this.original = original;
        var bytes = original.AsSpan();
        // The profile APIs also accept ANSI INIs. Preserve every byte in files without a BOM.
        encoding = bytes.StartsWith(Utf8Bom.Preamble) ? Utf8Bom :
            bytes.StartsWith(Utf16.Preamble) ? Utf16 : Encoding.Latin1;
        var content = encoding.GetString(bytes[encoding.Preamble.Length..]);
        newline = Environment.NewLine;
        foreach (Match match in Lines.Matches(content))
        {
            if (match.Length == 0)
                break;
            if (lines.Count == 0 && match.Groups["ending"].Value is { Length: > 0 } ending)
                newline = ending;
            lines.Add(match.Value);
        }
    }

    internal static IniDocument Load(string path) => new(path, ReadFile(path));

    internal string? Get(string section, string key)
    {
        var index = FindKey(section, key, out _);
        if (index < 0)
            return null;
        var value = lines[index][(lines[index].IndexOf('=') + 1)..].Trim();
        if (encoding != Utf8Bom && value.Length >= 2 &&
            value[0] is '"' or '\'' && value[0] == value[^1])
            value = value[1..^1].Trim();
        return value;
    }

    internal void Set(string section, string key, string value)
    {
        var index = FindKey(section, key, out var sectionEnd);
        if (index >= 0)
        {
            var line = lines[index];
            var start = line.IndexOf('=') + 1;
            while (start < line.Length && line[start] is ' ' or '\t')
                start++;
            var end = line.TrimEnd().Length;
            lines[index] = line[..start] + value + line[Math.Max(start, end)..];
            return;
        }

        if (sectionEnd < 0)
        {
            EnsureLineEndingBefore(lines.Count);
            if (lines.Count > 0 && lines[^1].Trim().Length > 0)
                lines.Add(newline);
            lines.Add($"[{section}]{newline}");
            sectionEnd = lines.Count;
        }
        EnsureLineEndingBefore(sectionEnd);
        lines.Insert(sectionEnd, $"{key}={value}{newline}");
    }

    internal void Save()
    {
        var tempPath = Path.Combine(Path.GetDirectoryName(path)!, $".bedrock-toolkit-{Guid.NewGuid():N}.tmp");
        try
        {
            File.WriteAllText(tempPath, string.Concat(lines), encoding);
            var latest = ReadFile(path);
            if ((original is null) != (latest is null) || !original.AsSpan().SequenceEqual(latest))
                throw new IOException("The INI changed while saving. Select Save again to apply your edits to the latest file.");
            File.Move(tempPath, path, overwrite: true);
        }
        finally
        {
            File.Delete(tempPath);
        }
    }

    private int FindKey(string section, string key, out int sectionEnd)
    {
        var inSection = false;
        sectionEnd = -1;
        for (var index = 0; index < lines.Count; index++)
        {
            var line = lines[index].Trim();
            if (line.Length == 0 || line[0] is ';' or '#')
                continue;
            if (line[0] == '[' && line[^1] == ']')
            {
                if (inSection && sectionEnd == -1)
                {
                    sectionEnd = index;
                    // Win32 profile readers use the first matching section.
                    if (encoding != Utf8Bom)
                        break;
                }
                inSection = string.Equals(line[1..^1].Trim(), section, StringComparison.OrdinalIgnoreCase);
                continue;
            }
            var separator = line.IndexOf('=');
            if (inSection && separator > 0 &&
                string.Equals(line[..separator].Trim(), key, StringComparison.OrdinalIgnoreCase))
                return index;
        }
        if (inSection && sectionEnd == -1)
            sectionEnd = lines.Count;
        return -1;
    }

    private void EnsureLineEndingBefore(int index)
    {
        if (index > 0 && lines[index - 1][^1] is not ('\r' or '\n'))
            lines[index - 1] += newline;
    }

    private static byte[]? ReadFile(string path)
    {
        try
        {
            return File.ReadAllBytes(path);
        }
        catch (FileNotFoundException)
        {
            return null;
        }
    }
}
