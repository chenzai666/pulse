using Filee.Core.Conversion;
using Filee.Core.Formats;
using Filee.Engines;
using Filee.Engines.Infrastructure;
using System.Text.RegularExpressions;

namespace Pulse.FileConversion;

internal sealed class EngineSet
{
    private static readonly HashSet<string> EngineIds = ["magick", "pdfsharp", "pdfium", "rhwp", "hwpx-writer",
        "docx-writer", "ooxml", "spreadsheet", "markdown", "pandoc", "archive", "ffmpeg"];
    private static readonly HashSet<string> ImageInputs = ["jpg", "png", "webp", "bmp", "gif", "heic", "avif"];
    private static readonly HashSet<string> OfficeInputs = ["docx", "docm", "dotx", "dotm", "xlsx", "xlsm", "xltx", "xls", "pptx", "pptm", "potx", "ppsx"];
    private static readonly HashSet<string> VideoInputs = ["mp4", "mkv", "mov"];
    private static readonly HashSet<string> AudioInputs = ["mp3", "wav", "flac", "aac", "m4a"];
    private static readonly HashSet<string> ArchiveInputs = ["zip", "7z", "tar", "tgz", "gz", "rar"];
    private static readonly string[] Images = ["jpg", "png", "webp", "bmp", "gif"];
    private static readonly Dictionary<string, string[]> RequiredCodecs = new()
    {
        ["mp4"] = ["mpeg4", "aac"], ["mkv"] = ["mpeg4", "aac"], ["mov"] = ["mpeg4", "aac"],
        ["mp3"] = ["libmp3lame"], ["wav"] = ["pcm_s16le"], ["flac"] = ["flac"], ["aac"] = ["aac"], ["m4a"] = ["aac"], ["gif"] = ["gif"]
    };
    private readonly HashSet<string> codecs;
    public ConverterCatalog Catalog { get; }
    public IReadOnlyList<IConverter> Converters { get; }

    private EngineSet(IReadOnlyList<IConverter> converters, HashSet<string> availableCodecs)
    {
        Converters = converters;
        codecs = availableCodecs;
        Catalog = new ConverterCatalog(converters) { Priority = ["markdown", "pandoc", "magick", "pdfsharp", "pdfium", "hwpx-writer", "rhwp", "archive", "ffmpeg", "spreadsheet"] };
    }

    public static async Task<EngineSet> CreateAsync(string profile, CancellationToken cancellation)
    {
        EngineEnvironment.SearchSystem = false;
        EngineEnvironment.DownloadRoot = Path.Combine(profile, "Cache", "UnusedEngineDownloads");
        EngineEnvironment.OwnCopies = new Dictionary<string, string>();
        var converters = EngineRegistry.CreateAll(new EngineEnvironment(profile)).Where(c => EngineIds.Contains(c.Id)).ToList();
        var availableCodecs = new HashSet<string>(StringComparer.Ordinal);
        var ffmpeg = EngineEnvironment.BundledProgram("ffmpeg", "ffmpeg.exe");
        if (ffmpeg is not null)
        {
            try
            {
                var result = await ProcessRunner.RunAsync(ffmpeg, ["-hide_banner", "-encoders"], TimeSpan.FromSeconds(10), cancellation);
                if (result.ExitCode == 0)
                    foreach (Match match in Regex.Matches(result.StandardOutput, @"(?m)^\s*[VAS][A-Z.]{5}\s+(\S+)\s"))
                        availableCodecs.Add(match.Groups[1].Value);
            }
            catch (Exception ex) when (ex is not OperationCanceledException) { Console.Error.WriteLine("FFmpeg codec query failed: " + ex.GetType().Name); }
        }
        return new EngineSet(converters, availableCodecs);
    }

    public static string[] AllowedTargets(string source)
    {
        if (ImageInputs.Contains(source)) return [.. Images, "pdf"];
        if (OfficeInputs.Contains(source)) return ["pdf"];
        if (source == "pdf") return Images;
        if (source == "md") return ["html", "docx", "pdf"];
        if (VideoInputs.Contains(source)) return ["mp4", "mkv", "mov", "mp3", "wav", "flac", "aac", "m4a", "gif"];
        if (AudioInputs.Contains(source)) return ["mp3", "wav", "flac", "aac", "m4a"];
        if (ArchiveInputs.Contains(source)) return source == "gz" ? ["zip", "7z", "tar", "tgz", "folder", "gz"] : ["zip", "7z", "tar", "tgz", "folder"];
        return [];
    }

    public Route? Route(string source, string target)
    {
        if (!AllowedTargets(source).Contains(target)) return null;
        var route = Catalog.CreatePlanner().Plan(source, target);
        if (route is not null && route.Steps.Any(s => s.Converter.Id == "ffmpeg") &&
            (!RequiredCodecs.TryGetValue(target, out var required) || !required.All(codecs.Contains))) return null;
        return route;
    }

    public object Capabilities(string id, string[] paths)
    {
        var inputs = paths.Select(p => new { path = p, format = FormatRegistry.Detect(p)?.Id ?? "unknown" }).ToArray();
        string[] allTargets = inputs.Length == 0 ? [] : AllowedTargets(inputs[0].format).Distinct().ToArray();
        var targets = new List<object>();
        foreach (var target in allTargets)
        {
            var routes = inputs.Select(i => Route(i.format, target)).ToArray();
            if (routes.Any(r => r is null)) continue;
            var format = FormatRegistry.Get(target);
            targets.Add(new { id = target, label = Label(target),
                extension = format.PrimaryExtension, routes = routes.Select((r, i) => new {
                    source = inputs[i].path, steps = r!.Steps.Select(s => new { engine = s.Converter.Id, from = s.From, to = s.To }).ToArray()
                }).ToArray() });
        }
        return new { id, type = "capabilities", inputs, targets, engines = Converters.Select(c => new {
            id = c.Id, available = Catalog.StatusOf(c).IsAvailable, reason = Catalog.StatusOf(c).ReasonKey
        }).ToArray(), limitations = new[] {
            "旧版 DOC、PPT 需要未预置的 LibreOffice，本包不提供这些转换。",
            "Office 与 Markdown 转 PDF 采用内置文档读取器和 rhwp，复杂图表、公式、修订及版式可能无法完整保留。",
            "能力清单表示格式和引擎路由可用；损坏、加密文件及缺少音频流等内容问题仍可能导致单文件失败。"
        } };
    }

    private static string Label(string target) => target switch
    {
        "folder" => "解压到文件夹", "pdf" => "PDF 文档", "docx" => "Word 文档（DOCX）", "html" => "HTML 网页",
        "tgz" => "TAR.GZ 压缩包", "zip" or "7z" or "tar" or "gz" => target.ToUpperInvariant() + " 压缩包",
        "mp4" or "mkv" or "mov" => target.ToUpperInvariant() + " 视频",
        "mp3" or "wav" or "flac" or "aac" or "m4a" => target.ToUpperInvariant() + " 音频",
        _ => target.ToUpperInvariant() + " 图片"
    };
}
