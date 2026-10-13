using System.Text;
using System.Text.Json;

namespace Pulse.FileConversion;

internal sealed record Request
{
    public string Id { get; init; } = "";
    public string Action { get; init; } = "";
    public string[] Inputs { get; init; } = [];
    public string Target { get; init; } = "";
    public string OutputDirectory { get; init; } = "";
    public int Quality { get; init; } = 90;
    public string JobId { get; init; } = "";
}

internal static class Protocol
{
    private static readonly object Gate = new();
    internal static readonly JsonSerializerOptions Json = new() { PropertyNamingPolicy = JsonNamingPolicy.CamelCase };
    private const int MaxLineChars = 1024 * 1024;

    public static void Send(object message)
    {
        var json = JsonSerializer.Serialize(message, Json);
        lock (Gate) { Console.Out.WriteLine(json); Console.Out.Flush(); }
    }

    public static void Error(string id, string code, string detail = "") => Send(new { id, type = "error", code, detail });

    public static bool ValidId(string? id) => id is { Length: > 0 and <= 64 } &&
        id.All(c => char.IsAsciiLetterOrDigit(c) || c is '-' or '_');

    public static string? ReadLine(TextReader reader)
    {
        var text = new StringBuilder();
        while (true)
        {
            var value = reader.Read();
            if (value < 0) return text.Length == 0 ? null : text.ToString();
            if (value == '\n') return text.ToString().TrimEnd('\r');
            if (text.Length >= MaxLineChars) throw new InvalidDataException("Request exceeds the protocol limit.");
            text.Append((char)value);
        }
    }
}
