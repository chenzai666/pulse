using Filee.Core.Conversion;
using Filee.Core.Formats;
using Filee.Core.Presets;

namespace Pulse.FileConversion;

internal sealed class ConversionHost(EngineSet engines, string sessionRoot, CancellationToken lifetime, WindowsJob processJob)
{
    private readonly object gate = new();
    private ConversionJob? active;
    private Task? activeTask;
    private string activeId = "";
    private bool terminalSent;
    private bool publishing;

    private sealed record Result(string Source, string State, string[] Outputs, string? ErrorKey, string? Error);

    public void Handle(Request request)
    {
        if (!Protocol.ValidId(request.Id)) { Protocol.Error("", "INVALID_ID"); return; }
        switch (request.Action)
        {
            case "cancel": Cancel(request); return;
            case "capabilities":
                if (Busy()) { Protocol.Error(request.Id, "BUSY"); return; }
                Protocol.Send(engines.Capabilities(request.Id, Inputs(request.Inputs)));
                return;
            case "convert":
                lock (gate)
                {
                    if (activeTask is { IsCompleted: false } && !terminalSent) { Protocol.Error(request.Id, "BUSY"); return; }
                    var files = Inputs(request.Inputs);
                    if (request.Quality is < 1 or > 100) throw new InvalidDataException("Quality must be from 1 to 100.");
                    foreach (var file in files)
                        if (FormatRegistry.Detect(file) is not { } from || engines.Route(from.Id, request.Target) is null)
                            throw new NotSupportedException("The selected files have no available route to that target.");
                    var output = SafePaths.DirectoryPath(request.OutputDirectory);
                    var stage = SafePaths.DirectoryPath(Path.Combine(sessionRoot, "outputs-" + Guid.NewGuid().ToString("N")));
                    var preset = new Preset { TargetFormat = request.Target, Name = request.Target,
                        Output = new OutputRule { Location = OutputLocation.CustomFolder, CustomFolder = stage, Conflict = ConflictPolicy.Rename },
                        Image = new ImageOptions { Quality = request.Quality } };
                    var job = new ConversionJob(files, preset, request.Target);
                    active = job;
                    activeId = request.Id;
                    terminalSent = publishing = false;
                    activeTask = Task.Run(() => ConvertAsync(request.Id, job, output, stage), CancellationToken.None);
                }
                return;
            default: Protocol.Error(request.Id, "INVALID_ACTION"); return;
        }
    }

    private static string[] Inputs(string[]? paths)
    {
        if (paths is null || paths.Length is < 1 or > 128) throw new InvalidDataException("Select from 1 to 128 files.");
        return paths.Select(SafePaths.Input).Distinct(StringComparer.OrdinalIgnoreCase).ToArray();
    }

    private bool Busy() { lock (gate) return activeTask is { IsCompleted: false } && !terminalSent; }

    private void Cancel(Request request)
    {
        lock (gate)
        {
            var cancelled = activeTask is { IsCompleted: false } && activeId == request.JobId && !terminalSent;
            if (cancelled) { active!.Cancel(); _ = EscalateAsync(active!); }
            Protocol.Send(new { id = request.Id, type = "cancelled", jobId = request.JobId, cancelled });
        }
    }

    private async Task EscalateAsync(ConversionJob job)
    {
        Task? work;
        lock (gate) work = ReferenceEquals(active, job) ? activeTask : null;
        if (work is null || await Task.WhenAny(work, Task.Delay(TimeSpan.FromSeconds(3))) == work) return;
        lock (gate)
        {
            if (!ReferenceEquals(active, job) || work.IsCompleted) return;
            if (!terminalSent)
            {
                terminalSent = true;
                Protocol.Send(new { id = activeId, type = "completed", state = "cancelled", files = job.Files.Select(f => new Result(
                    f.SourcePath, "cancelled", [], "error.cancelled", "Conversion was cancelled.")).ToArray() });
            }
            // A native decoder may ignore cancellation. Process teardown closes
            // the Windows job and removes its engine children as well.
            Environment.Exit(3);
        }
    }

    public async Task StopAsync()
    {
        Task? task;
        lock (gate) { active?.Cancel(); task = activeTask; }
        if (task is not null && await Task.WhenAny(task, Task.Delay(TimeSpan.FromSeconds(3))) != task)
            Environment.Exit(3);
        if (task is not null) await task;
    }

    private async Task ConvertAsync(string id, ConversionJob job, string output, string stage)
    {
        var results = new List<Result>();
        string? fatal = null;
        using var watchdog = new CancellationTokenSource();
        var quota = MonitorAsync(job, watchdog.Token);
        try
        {
            Protocol.Send(new { id, type = "started", files = job.Sources.Count });
            await using var queue = new JobQueue(engines.Catalog, engines.Converters.OfType<IPdfMerger>().FirstOrDefault(), combiner: null);
            var lastProgress = 0L;
            queue.JobUpdated += (_, updated) =>
            {
                if (updated != job) return;
                var now = Environment.TickCount64;
                var previous = Interlocked.Read(ref lastProgress);
                if (now - previous < 150 || Interlocked.CompareExchange(ref lastProgress, now, previous) != previous) return;
                lock (gate)
                {
                    if (!ReferenceEquals(active, job) || terminalSent || publishing) return;
                    Protocol.Send(new { id, type = "progress", progress = job.Progress, files = job.Files.Select(f => new {
                        source = f.SourcePath, state = f.State.ToString().ToLowerInvariant(), progress = f.Progress
                    }).ToArray() });
                }
            };
            using var cancelRegistration = lifetime.Register(job.Cancel);
            var work = queue.RunAsync(job);
            if (await Task.WhenAny(work, Task.Delay(TimeSpan.FromMinutes(30), watchdog.Token)) != work)
            {
                job.Cancel();
                _ = EscalateAsync(job);
            }
            await work;
            lock (gate) publishing = true;
            foreach (var file in job.Files)
            {
                var outputs = new List<string>();
                var error = file.ErrorDetail;
                var errorKey = file.ErrorKey;
                var state = file.State.ToString().ToLowerInvariant();
                if (file.State == FileState.Done)
                {
                    try
                    {
                        job.CancellationToken.ThrowIfCancellationRequested();
                        if (file.Outputs.Count == 0) throw new IOException("The engine produced no output.");
                        foreach (var staged in file.Outputs)
                        {
                            if (!SafePaths.Within(staged, stage)) throw new IOException("Engine output escaped the private staging directory.");
                            outputs.Add(SafePaths.Publish(staged, output, job.CancellationToken));
                        }
                    }
                    catch (OperationCanceledException) { state = "cancelled"; errorKey = "error.cancelled"; error = "Conversion was cancelled."; }
                    catch (Exception ex) { state = "failed"; errorKey = "error.publish_failed"; error = ex.Message; }
                }
                else if (file.State is FileState.Pending or FileState.Running)
                {
                    state = job.CancellationToken.IsCancellationRequested ? "cancelled" : "failed";
                    errorKey = "error.conversion_failed";
                    error ??= "The conversion did not finish.";
                }
                results.Add(new(file.SourcePath, state, outputs.ToArray(), errorKey, error));
            }
        }
        catch (Exception ex) { fatal = ex.Message; }
        finally
        {
            watchdog.Cancel();
            await quota;
            // Converter cancellation can finish before a native child has
            // exited. Confirm our actual Windows Job is empty before terminal.
            var drained = await processJob.DrainChildrenAsync();
            if (!drained) job.Cancel();
            foreach (var file in job.Files.Skip(results.Count))
                results.Add(new(file.SourcePath, job.CancellationToken.IsCancellationRequested ? "cancelled" : "failed", [],
                    "error.conversion_failed", fatal ?? "The conversion did not finish."));
            var state = job.CancellationToken.IsCancellationRequested ? "cancelled"
                : results.All(f => f.State is "done" or "skipped") ? "completed"
                : results.Any(f => f.State == "done") ? "completedwitherrors" : "failed";
            lock (gate)
            {
                if (ReferenceEquals(active, job) && !terminalSent)
                {
                    terminalSent = true;
                    Protocol.Send(new { id, type = "completed", state, files = results });
                }
                publishing = false;
            }
            try { SafePaths.RejectLinks(stage); Directory.Delete(stage, true); }
            catch (Exception ex) when (ex is IOException or UnauthorizedAccessException) { }
            if (!drained) Environment.Exit(3);
        }
    }

    private async Task MonitorAsync(ConversionJob job, CancellationToken cancellation)
    {
        try
        {
            while (!cancellation.IsCancellationRequested)
            {
                await Task.Delay(1000, cancellation);
                long bytes = 0;
                var count = 0;
                foreach (var entry in SafePaths.TreeEntries(sessionRoot))
                {
                    try { if (entry is FileInfo file) bytes = checked(bytes + file.Length); }
                    catch (FileNotFoundException) { continue; }
                    if (++count > 50000 || bytes > 4L * 1024 * 1024 * 1024) { job.Cancel(); _ = EscalateAsync(job); return; }
                }
            }
        }
        catch (OperationCanceledException) { }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or OverflowException)
        { job.Cancel(); _ = EscalateAsync(job); }
    }
}
