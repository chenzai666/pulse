using Filee.Engines.Tests;
using Pulse.FileConversion;
using System.Diagnostics;
using System.IO.Compression;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;

internal static class Program
{
    private static int passed;
    private static int request;

    public static async Task<int> Main(string[] args)
    {
        if (!OperatingSystem.IsWindows()) { Console.Error.WriteLine("定向自检要求 Windows。"); return 1; }
        var root = SafePaths.DirectoryPath(Path.Combine(Path.GetTempPath(), "Pulse-Filee-test-" + Guid.NewGuid().ToString("N")));
        try
        {
            SafetyChecks(root);
            if (args.Length == 2 && args[0] == "--runtime") await RuntimeChecks(Path.GetFullPath(args[1]), root);
            else if (args.Length != 0) throw new ArgumentException("用法：--runtime <Pulse.Filee.Host.exe>");
            Console.WriteLine($"通过 {passed} 项定向检查；" + (args.Length == 0 ? "本次仅检查 BCL 安全逻辑，未验证转换引擎。" : "已运行真实预置引擎与生命周期检查。"));
            return 0;
        }
        catch (Exception ex) { Console.Error.WriteLine(ex); return 1; }
        finally
        {
            // This runner owns this exact GUID directory created above.
            try { SafePaths.RejectLinks(root); Directory.Delete(root, true); }
            catch (Exception ex) when (ex is IOException or UnauthorizedAccessException) { }
        }
    }

    private static void Check(bool condition, string message)
    { if (!condition) throw new InvalidDataException(message); }
    private static void Pass(string description) { passed++; Console.WriteLine("通过：" + description); }
    private static void Reject(Action action)
    {
        try { action(); }
        catch (Exception ex) when (ex is IOException or InvalidDataException or UnauthorizedAccessException or ArgumentException or OperationCanceledException) { return; }
        throw new InvalidDataException("预期操作被拒绝，但实际成功。");
    }
    private static string Hash(string path) { using var stream = File.OpenRead(path); return System.Convert.ToHexString(SHA256.HashData(stream)); }

    private static void SafetyChecks(string root)
    {
        foreach (var path in new[] { "relative.txt", "C:relative.txt", @"\\.\NUL", @"\\?\GLOBALROOT\Device\HarddiskVolume1\x",
            @"C:\file:stream", @"C:\CON.txt", @"C:\folder\COM¹.log", @"C:\ambiguous.\file", @"C:\space \file" })
            Reject(() => SafePaths.Absolute(path));
        Check(SafePaths.Absolute(@"\\?\C:\folder\正常文件.txt").StartsWith(@"\\?\C:\"), "长路径前缀被破坏。");
        Check(Protocol.ValidId("case_1-2") && !Protocol.ValidId(null) && !Protocol.ValidId("bad id"), "请求 ID 校验异常。");
        Reject(() => Protocol.ReadLine(new StringReader(new string('a', 1024 * 1024 + 1))));
        Pass("拒绝相对/设备/ADS/模糊路径，保留正常长路径，并限制协议长度");

        var stage = SafePaths.DirectoryPath(Path.Combine(root, "stage"));
        var output = SafePaths.DirectoryPath(Path.Combine(root, "output"));
        var source = Path.Combine(stage, "sample.txt");
        File.WriteAllText(source, "conversion output");
        var occupied = Path.Combine(output, "sample.txt");
        File.WriteAllText(occupied, "existing user file");
        var originalHash = Hash(source);
        var occupiedHash = Hash(occupied);
        var published = SafePaths.Publish(source, output);
        Check(published != occupied && Hash(source) == originalHash && Hash(occupied) == occupiedHash && Hash(published) == originalHash,
            "发布覆盖了输入或已有输出。");
        using var cancelled = new CancellationTokenSource();
        cancelled.Cancel();
        var before = Directory.GetFileSystemEntries(output).Length;
        Reject(() => SafePaths.Publish(source, output, cancelled.Token));
        Check(Directory.GetFileSystemEntries(output).Length == before, "取消发布留下了文件。");
        Pass("发布不会覆盖源或已有输出；取消发布不产生半文件");

        var extracted = SafePaths.DirectoryPath(Path.Combine(stage, "archive"));
        Directory.CreateDirectory(Path.Combine(extracted, "empty"));
        File.WriteAllText(Path.Combine(extracted, "data.txt"), "payload");
        Directory.CreateDirectory(Path.Combine(output, "archive"));
        var directory = SafePaths.Publish(extracted, output);
        Check(Directory.Exists(Path.Combine(directory, "empty")) && File.ReadAllText(Path.Combine(directory, "data.txt")) == "payload" &&
            Directory.GetFileSystemEntries(Path.Combine(output, "archive")).Length == 0, "目录发布丢失内容或覆盖了目录。");
        Pass("解压目录发布保留空目录，已有目录保持不变");

        var profile = SafePaths.DirectoryPath(Path.Combine(root, "profile"));
        var temporaryRoot = SafePaths.DirectoryPath(Path.Combine(profile, "Temp"));
        var stale = MarkedSession(temporaryRoot);
        var alive = MarkedSession(temporaryRoot);
        var untouched = SafePaths.DirectoryPath(Path.Combine(temporaryRoot, "user-content"));
        using (var lease = new FileStream(Path.Combine(alive, "host.lock"), FileMode.Open, FileAccess.ReadWrite, FileShare.None))
        {
            SessionTemp.CleanupStale(temporaryRoot);
            Check(!Directory.Exists(stale) && Directory.Exists(alive) && Directory.Exists(untouched), "过期清理删除活动或未知目录。");
        }
        SessionTemp.CleanupStale(temporaryRoot);
        Check(!Directory.Exists(alive) && Directory.Exists(untouched), "独占锁释放后不能清理过期会话。");
        using (var session = SessionTemp.Create(profile)) Check(Directory.Exists(session.Root), "私有会话目录创建失败。");
        Pass("过期清理受标记、年龄、GUID 与独占锁约束");

        var runtime = SafePaths.DirectoryPath(Path.Combine(root, "manifest"));
        var required = new[] { "Pulse.Filee.Host.exe", "Pulse.Filee.Host.dll", "Pulse.Filee.Host.deps.json", "Pulse.Filee.Host.runtimeconfig.json",
            "Filee.Core.dll", "Filee.Engines.dll", "engines/rhwp/rhwp.exe", "engines/pandoc/pandoc.exe", "engines/7zip/7z.exe",
            "engines/7zip/7z.dll", "engines/ffmpeg/bin/ffmpeg.exe", "engines/ffmpeg/bin/ffprobe.exe" };
        foreach (var file in required)
        {
            var path = Path.Combine(runtime, file);
            Directory.CreateDirectory(Path.GetDirectoryName(path)!);
            File.WriteAllText(path, "x");
        }
        var entries = required.Select(p => new { path = p, bytes = 1, sha256 = Hash(Path.Combine(runtime, p)) }).ToArray();
        File.WriteAllText(Path.Combine(runtime, "runtime-manifest.json"), JsonSerializer.Serialize(new { component = "Pulse.Filee.Host", schema = 1, files = entries }));
        RuntimePackage.Verify(runtime);
        var extra = Path.Combine(runtime, "unlisted.dll");
        File.WriteAllText(extra, "x");
        Reject(() => RuntimePackage.Verify(runtime));
        File.Delete(extra);
        File.WriteAllText(Path.Combine(runtime, required[0]), "y");
        Reject(() => RuntimePackage.Verify(runtime));
        Pass("包清单拒绝未列出文件与同长度 SHA256 篡改");
    }

    private static string MarkedSession(string root)
    {
        var path = SafePaths.DirectoryPath(Path.Combine(root, Guid.NewGuid().ToString("N")));
        var marker = Path.Combine(path, "host.lock");
        File.WriteAllText(marker, "Pulse.Filee.Host temporary session v1", Encoding.ASCII);
        File.SetLastWriteTimeUtc(marker, DateTime.UtcNow.AddMinutes(-6));
        return path;
    }

    private static async Task RuntimeChecks(string executable, string root)
    {
        Check(File.Exists(executable), "宿主可执行文件不存在。");
        var samples = SafePaths.DirectoryPath(Path.Combine(root, "samples"));
        var output = SafePaths.DirectoryPath(Path.Combine(root, "converted"));
        var markdown = Path.Combine(samples, "Markdown 中文样本.md");
        File.WriteAllText(markdown, "# Pulse 中文转换样本\n\n这是用于检查输出的段落。\n\n| 项目 | 结果 |\n| --- | --- |\n| Pulse | 正常 |\n", new UTF8Encoding(false));
        var xlsx = Fixtures.Xlsx(Path.Combine(samples, "工作表.xlsx"));
        var pptx = Fixtures.Pptx(Path.Combine(samples, "幻灯片.pptx"));
        var xls = Path.Combine(samples, "工作表 BIFF8.xls");
        var workbook = new XlsBuilder();
        workbook.Sheet("Pulse").Text(0, 0, "真实 XLS 测试").Number(1, 0, 42);
        workbook.Save(xls);
        var bmp = Fixtures.Bmp(Path.Combine(samples, "图片.bmp"));
        var archive = Fixtures.Zip(Path.Combine(samples, "普通归档.zip"), new Dictionary<string, string> { ["data.txt"] = "archive text", ["empty/"] = "" });
        var badArchive = Fixtures.Zip(Path.Combine(samples, "越界归档.zip"), new Dictionary<string, string> { ["../escaped.txt"] = "must not escape" });
        var original = Directory.GetFiles(samples).ToDictionary(p => p, Hash);
        await using var host = await HostSession.Start(executable, Environment.ProcessId);
        foreach (var bad in new[] { "relative.md", @"\\.\NUL", markdown + ":stream", @"C:\CON.md" })
        {
            var id = Next();
            host.Send(new { id, action = "capabilities", inputs = new[] { bad } });
            var error = await host.Read("error", id);
            Check(error.GetProperty("code").GetString() == "INVALID_REQUEST", "非法路径没有被宿主拒绝。");
        }
        Pass("真实协议拒绝非法输入路径");

        var html = (await Convert(host, markdown, "html", output))[0];
        Check(File.ReadAllText(html).Contains("Pulse") && File.ReadAllText(html).Contains("<html", StringComparison.OrdinalIgnoreCase), "HTML 没有真实内容。");
        var docx = (await Convert(host, markdown, "docx", output))[0];
        using (var document = ZipFile.OpenRead(docx)) Check(document.GetEntry("word/document.xml") is not null, "DOCX 不含正文 XML。");
        var pdfRoute = await Capabilities(host, markdown);
        var pdfTarget = pdfRoute.GetProperty("targets").EnumerateArray().Single(t => t.GetProperty("id").GetString() == "pdf");
        Check(pdfTarget.GetProperty("routes")[0].GetProperty("steps").EnumerateArray().Any(s => s.GetProperty("engine").GetString() == "rhwp"), "Markdown PDF 没有使用 rhwp 路线。");
        var markdownPdf = (await Convert(host, markdown, "pdf", output))[0];
        Pdf(markdownPdf);
        var originalPdfHash = Hash(markdownPdf);
        var secondPdf = (await Convert(host, markdown, "pdf", output))[0];
        Check(secondPdf != markdownPdf && Hash(markdownPdf) == originalPdfHash, "重复转换覆盖了已有 PDF。");
        Pass("Markdown→HTML/DOCX/PDF 有真实内容，PDF 走 rhwp，重复输出不覆盖");

        foreach (var office in new[] { docx, xlsx, xls, pptx }) Pdf((await Convert(host, office, "pdf", output))[0]);
        Pass("DOCX/XLSX/真实 BIFF8 XLS/PPTX→PDF");

        foreach (var target in new[] { "jpg", "png", "webp", "gif" }) _ = await Convert(host, bmp, target, output);
        var imagePdf = (await Convert(host, bmp, "pdf", output))[0];
        Pdf(imagePdf);
        _ = await Convert(host, imagePdf, "png", output);
        Pass("常见图片互转、图片→PDF、PDF→PNG");

        foreach (var target in new[] { "7z", "tar", "tgz" }) _ = await Convert(host, archive, target, output);
        var folders = await Convert(host, archive, "folder", output);
        Check(folders.Any(p => Directory.Exists(p) && File.Exists(Path.Combine(p, "data.txt")) && Directory.Exists(Path.Combine(p, "empty"))), "解压目录缺少数据或空目录。");
        var badId = Next();
        host.Send(new { id = badId, action = "convert", inputs = new[] { badArchive }, target = "folder", outputDirectory = output });
        var badResult = await host.Read("completed", badId);
        Check(badResult.GetProperty("state").GetString() == "failed" && !Directory.GetFiles(root, "escaped.txt", SearchOption.AllDirectories).Any(), "越界归档被解包。");
        Pass("ZIP→7z/TAR/TAR.GZ/文件夹，归档越界在解包前被拒绝");

        var ffmpeg = Path.Combine(Path.GetDirectoryName(executable)!, "engines", "ffmpeg", "bin", "ffmpeg.exe");
        var wav = Fixtures.Wav(Path.Combine(samples, "声音.wav"));
        foreach (var target in new[] { "mp3", "flac", "aac", "m4a" }) _ = await Convert(host, wav, target, output);
        var mp4 = Path.Combine(samples, "视频.mp4");
        await Fixtures.Run(ffmpeg, ["-hide_banner", "-loglevel", "error", "-f", "lavfi", "-i", "testsrc2=size=160x120:rate=12", "-f", "lavfi", "-i", "sine=frequency=440", "-t", "1", "-c:v", "mpeg4", "-q:v", "4", "-c:a", "aac", "-y", mp4]);
        _ = await Convert(host, mp4, "mkv", output);
        _ = await Convert(host, mp4, "mov", output);
        _ = await Convert(host, mp4, "mp3", output);
        Pass("真实 WAV→MP3/FLAC/AAC/M4A 和 MP4→MKV/MOV/音频");
        foreach (var item in original) Check(Hash(item.Key) == item.Value, "转换修改了源文件：" + Path.GetFileName(item.Key));
        Pass("全部原始测试文件 SHA256 保持不变");

        var longVideo = Path.Combine(samples, "取消与父进程退出.mp4");
        await Fixtures.Run(ffmpeg, ["-hide_banner", "-loglevel", "error", "-f", "lavfi", "-i", "testsrc2=size=1280x720:rate=30", "-t", "30", "-an", "-c:v", "mpeg4", "-q:v", "7", "-y", longVideo], 120);
        var jobId = Next();
        host.Send(new { id = jobId, action = "convert", inputs = new[] { longVideo }, target = "mkv", outputDirectory = output });
        var children = await NativeTree.WaitForEngine(host.Process.Id, "ffmpeg.exe");
        var cancelId = Next();
        host.Send(new { id = cancelId, action = "cancel", jobId });
        var done = await host.Read("completed", jobId, 10);
        Check(done.GetProperty("state").GetString() == "cancelled", "真实转换取消后没有取消终态。");
        await NativeTree.WaitGone(children);
        Check(host.History.Count(m => m.GetProperty("type").GetString() == "completed" && m.GetProperty("id").GetString() == jobId) == 1, "取消收到多个终结事件。");
        Pass("真实 FFmpeg 转换取消，单次终结且子进程退出");

        using var owner = Fixtures.Owner();
        await using var ownedHost = await HostSession.Start(executable, owner.Id);
        ownedHost.Send(new { id = Next(), action = "convert", inputs = new[] { longVideo }, target = "mkv", outputDirectory = output });
        var ownedChildren = await NativeTree.WaitForEngine(ownedHost.Process.Id, "ffmpeg.exe");
        owner.Kill();
        await owner.WaitForExitAsync().WaitAsync(TimeSpan.FromSeconds(5));
        await ownedHost.Process.WaitForExitAsync().WaitAsync(TimeSpan.FromSeconds(10));
        await NativeTree.WaitGone(ownedChildren);
        Pass("父进程退出后宿主和真实引擎子树无残留");
    }

    private static string Next() => "t" + Interlocked.Increment(ref request);
    private static void Pdf(string path)
    {
        using var stream = File.OpenRead(path);
        var header = new byte[5];
        stream.ReadExactly(header);
        Check(Encoding.ASCII.GetString(header) == "%PDF-" && stream.Length > 100, "输出不是有效 PDF。");
    }

    private static async Task<JsonElement> Capabilities(HostSession host, string source)
    {
        var id = Next();
        host.Send(new { id, action = "capabilities", inputs = new[] { source } });
        return await host.Read("capabilities", id);
    }

    private static async Task<string[]> Convert(HostSession host, string source, string target, string output)
    {
        var capabilities = await Capabilities(host, source);
        var available = capabilities.GetProperty("engines").EnumerateArray().Where(e => e.GetProperty("available").GetBoolean())
            .Select(e => e.GetProperty("id").GetString()).ToHashSet();
        var route = capabilities.GetProperty("targets").EnumerateArray().SingleOrDefault(t => t.GetProperty("id").GetString() == target);
        Check(route.ValueKind != JsonValueKind.Undefined, "没有实际可用目标：" + Path.GetExtension(source) + "→" + target);
        foreach (var item in route.GetProperty("routes").EnumerateArray())
            foreach (var step in item.GetProperty("steps").EnumerateArray()) Check(available.Contains(step.GetProperty("engine").GetString()), "能力清单返回了不可用引擎。");
        var id = Next();
        host.Send(new { id, action = "convert", inputs = new[] { source }, target, outputDirectory = output });
        var result = await host.Read("completed", id, 120);
        Check(result.GetProperty("state").GetString() == "completed", "转换失败：" + result.GetRawText());
        var files = result.GetProperty("files").EnumerateArray().ToArray();
        Check(files.Length == 1 && files[0].GetProperty("state").GetString() == "done", "缺少成功单文件结果。");
        var paths = files[0].GetProperty("outputs").EnumerateArray().Select(p => p.GetString()!).ToArray();
        Check(paths.Length > 0 && paths.All(p => File.Exists(p) ? new FileInfo(p).Length > 0 : Directory.Exists(p)), "结果为空或输出路径不存在。");
        return paths;
    }
}
