using System.Collections.Concurrent;
using System.Diagnostics;
using System.Text;
using System.Text.Json;
using System.Threading.Channels;

internal sealed class HostSession : IAsyncDisposable
{
    private readonly Channel<JsonElement> messages = Channel.CreateUnbounded<JsonElement>();
    private readonly Task reader;
    private readonly Task<string> errors;
    public Process Process { get; }
    public ConcurrentQueue<JsonElement> History { get; } = new();

    private HostSession(Process process)
    {
        Process = process;
        errors = Task.Run(async () =>
        {
            var output = new StringBuilder();
            string? line;
            while ((line = await process.StandardError.ReadLineAsync()) is not null)
            {
                Console.Error.WriteLine("PULSE_TEST_HOST " + line);
                if (output.Length < 128 * 1024) output.AppendLine(line);
            }
            return output.ToString();
        });
        reader = Task.Run(async () =>
        {
            try
            {
                string? line;
                while ((line = await process.StandardOutput.ReadLineAsync()) is not null)
                {
                    using var json = JsonDocument.Parse(line);
                    var message = json.RootElement.Clone();
                    History.Enqueue(message);
                    await messages.Writer.WriteAsync(message);
                }
                messages.Writer.TryComplete();
            }
            catch (Exception ex) { messages.Writer.TryComplete(ex); }
        });
    }

    public static async Task<HostSession> Start(string executable, int owner)
    {
        var start = new ProcessStartInfo(executable) { UseShellExecute = false, CreateNoWindow = true,
            RedirectStandardInput = true, RedirectStandardOutput = true, RedirectStandardError = true,
            StandardInputEncoding = new UTF8Encoding(false), StandardOutputEncoding = Encoding.UTF8, StandardErrorEncoding = Encoding.UTF8 };
        start.ArgumentList.Add("--owner-pid");
        start.ArgumentList.Add(owner.ToString(System.Globalization.CultureInfo.InvariantCulture));
        var process = Process.Start(start) ?? throw new IOException("转换宿主无法启动。");
        var session = new HostSession(process);
        try { await session.Read("ready", "", 60); return session; }
        catch { await session.DisposeAsync(); throw; }
    }

    public void Send(object message)
    { Process.StandardInput.WriteLine(JsonSerializer.Serialize(message)); Process.StandardInput.Flush(); }

    public async Task<JsonElement> Read(string type, string id, int seconds = 30)
    {
        using var deadline = new CancellationTokenSource(TimeSpan.FromSeconds(seconds));
        await foreach (var message in messages.Reader.ReadAllAsync(deadline.Token))
        {
            if (message.GetProperty("type").GetString() == type && message.GetProperty("id").GetString() == id) return message;
            if (message.GetProperty("type").GetString() == "error") throw new IOException("宿主请求失败：" + message.GetRawText());
        }
        throw new IOException("宿主提前退出。" + (errors.IsCompletedSuccessfully ? errors.Result : ""));
    }

    public async ValueTask DisposeAsync()
    {
        try
        {
            if (!Process.HasExited)
            {
                try { Send(new { id = "test_shutdown", action = "shutdown" }); Process.StandardInput.Close(); }
                catch (IOException) { }
                if (!Process.WaitForExit(5000)) Process.Kill(entireProcessTree: true);
                await Process.WaitForExitAsync();
            }
            await reader;
            await errors;
        }
        finally { Process.Dispose(); }
    }
}
