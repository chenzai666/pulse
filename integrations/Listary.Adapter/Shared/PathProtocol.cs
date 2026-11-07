using System;
using System.Text;

namespace Listary.FileAppPlugin.PulseAdapter;

internal static class PathProtocol
{
    internal const int MaxBytes = 65536;
    internal const int MaxChars = MaxBytes / 2 - 1;
    internal const uint ManagerTag = 0x504C534D;
    internal const uint PickerTag = 0x504C5354;

    internal static bool IsValid(string? path)
    {
        if (string.IsNullOrEmpty(path) || path!.Length > MaxChars) return false;
        var drive = path.Length >= 3 && ((path[0] >= 'A' && path[0] <= 'Z') ||
            (path[0] >= 'a' && path[0] <= 'z')) && path[1] == ':' && path[2] == '\\';
        if (!drive && !path.StartsWith(@"\\", StringComparison.Ordinal)) return false;
        for (var i = 0; i < path.Length; ++i)
        {
            var c = path[i];
            if (c < 32 || c == '/' || c == '"' || c == '<' || c == '>' || c == '|' ||
                c == '*' || c == '?' || (c == ':' && !(drive && i == 1))) return false;
            if (char.IsHighSurrogate(c))
            {
                if (++i >= path.Length || !char.IsLowSurrogate(path[i])) return false;
            }
            else if (char.IsLowSurrogate(c)) return false;
        }
        var components = path.Substring(drive ? 3 : 2).Split('\\');
        var count = 0;
        for (var i = 0; i < components.Length; ++i)
        {
            var part = components[i];
            if (part.Length == 0)
            {
                if (i != components.Length - 1) return false;
                continue;
            }
            if (part.EndsWith(".", StringComparison.Ordinal) || part.EndsWith(" ", StringComparison.Ordinal)) return false;
            var stem = part.Split('.')[0].TrimEnd(' ').ToUpperInvariant();
            if (stem == "CON" || stem == "PRN" || stem == "AUX" || stem == "NUL" ||
                stem == "CONIN$" || stem == "CONOUT$") return false;
            if (stem.Length == 4 && (stem.StartsWith("COM", StringComparison.Ordinal) ||
                stem.StartsWith("LPT", StringComparison.Ordinal)) &&
                ((stem[3] >= '1' && stem[3] <= '9') || stem[3] == '\u00b9' || stem[3] == '\u00b2' || stem[3] == '\u00b3')) return false;
            ++count;
        }
        return drive || count >= 2;
    }

    internal static byte[]? Encode(string path, bool picker, uint generation)
    {
        if (!IsValid(path) || (!picker && generation == 0)) return null;
        var text = Encoding.Unicode.GetBytes(path + "\0");
        if (picker) return text;
        if (text.Length > MaxBytes - 16) return null;
        var data = new byte[text.Length + 16];
        Buffer.BlockCopy(BitConverter.GetBytes(1U), 0, data, 0, 4);
        Buffer.BlockCopy(BitConverter.GetBytes((uint)path.Length + 1), 0, data, 4, 4);
        Buffer.BlockCopy(BitConverter.GetBytes((ulong)generation), 0, data, 8, 8);
        Buffer.BlockCopy(text, 0, data, 16, text.Length);
        return data;
    }
}
