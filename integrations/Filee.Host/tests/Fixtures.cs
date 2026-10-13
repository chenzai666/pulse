using System.Diagnostics;
using System.IO.Compression;
using System.Text;

internal static class Fixtures
{
    private const string Types = "http://schemas.openxmlformats.org/package/2006/content-types";
    private const string Relations = "http://schemas.openxmlformats.org/package/2006/relationships";
    private const string Office = "http://schemas.openxmlformats.org/officeDocument/2006/relationships";

    public static string Zip(string path, Dictionary<string, string> entries)
    {
        using var archive = ZipFile.Open(path, ZipArchiveMode.Create);
        foreach (var item in entries)
        {
            var entry = archive.CreateEntry(item.Key);
            if (item.Key.EndsWith('/')) continue;
            using var output = new StreamWriter(entry.Open(), new UTF8Encoding(false));
            output.Write(item.Value);
        }
        return path;
    }

    public static string Xlsx(string path) => Zip(path, new()
    {
        ["[Content_Types].xml"] = $"""
            <Types xmlns="{Types}"><Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/><Default Extension="xml" ContentType="application/xml"/><Override PartName="/xl/workbook.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml"/><Override PartName="/xl/worksheets/sheet1.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml"/></Types>
            """,
        ["_rels/.rels"] = $"""<Relationships xmlns="{Relations}"><Relationship Id="rId1" Type="{Office}/officeDocument" Target="xl/workbook.xml"/></Relationships>""",
        ["xl/workbook.xml"] = $"""<workbook xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main" xmlns:r="{Office}"><sheets><sheet name="Pulse" sheetId="1" r:id="rId1"/></sheets></workbook>""",
        ["xl/_rels/workbook.xml.rels"] = $"""<Relationships xmlns="{Relations}"><Relationship Id="rId1" Type="{Office}/worksheet" Target="worksheets/sheet1.xml"/></Relationships>""",
        ["xl/worksheets/sheet1.xml"] = """<worksheet xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main"><dimension ref="A1:B2"/><sheetData><row r="1"><c r="A1" t="inlineStr"><is><t>Pulse 工作表</t></is></c><c r="B1"><v>42</v></c></row><row r="2"><c r="A2" t="inlineStr"><is><t>转换测试</t></is></c><c r="B2"><v>7</v></c></row></sheetData></worksheet>"""
    });

    public static string Pptx(string path) => Zip(path, new()
    {
        ["[Content_Types].xml"] = $"""
            <Types xmlns="{Types}"><Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/><Default Extension="xml" ContentType="application/xml"/><Override PartName="/ppt/presentation.xml" ContentType="application/vnd.openxmlformats-officedocument.presentationml.presentation.main+xml"/><Override PartName="/ppt/slides/slide1.xml" ContentType="application/vnd.openxmlformats-officedocument.presentationml.slide+xml"/></Types>
            """,
        ["_rels/.rels"] = $"""<Relationships xmlns="{Relations}"><Relationship Id="rId1" Type="{Office}/officeDocument" Target="ppt/presentation.xml"/></Relationships>""",
        ["ppt/presentation.xml"] = $"""<p:presentation xmlns:p="http://schemas.openxmlformats.org/presentationml/2006/main" xmlns:r="{Office}"><p:sldIdLst><p:sldId id="256" r:id="rId1"/></p:sldIdLst><p:sldSz cx="9144000" cy="6858000"/></p:presentation>""",
        ["ppt/_rels/presentation.xml.rels"] = $"""<Relationships xmlns="{Relations}"><Relationship Id="rId1" Type="{Office}/slide" Target="slides/slide1.xml"/></Relationships>""",
        ["ppt/slides/slide1.xml"] = """
            <p:sld xmlns:p="http://schemas.openxmlformats.org/presentationml/2006/main" xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main"><p:cSld><p:spTree><p:nvGrpSpPr><p:cNvPr id="1" name=""/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr><p:grpSpPr/><p:sp><p:nvSpPr><p:cNvPr id="2" name="Pulse text"/><p:cNvSpPr/><p:nvPr/></p:nvSpPr><p:spPr><a:xfrm><a:off x="914400" y="914400"/><a:ext cx="7315200" cy="2743200"/></a:xfrm><a:prstGeom prst="rect"><a:avLst/></a:prstGeom></p:spPr><p:txBody><a:bodyPr/><a:lstStyle/><a:p><a:r><a:rPr lang="zh-CN" sz="2400"/><a:t>Pulse 幻灯片转换测试</a:t></a:r></a:p></p:txBody></p:sp></p:spTree></p:cSld></p:sld>
            """
    });

    public static string Bmp(string path)
    {
        using var output = new BinaryWriter(File.Create(path));
        output.Write((ushort)0x4d42); output.Write(70); output.Write(0); output.Write(54);
        output.Write(40); output.Write(2); output.Write(2); output.Write((ushort)1); output.Write((ushort)24);
        output.Write(0); output.Write(16); output.Write(2835); output.Write(2835); output.Write(0); output.Write(0);
        output.Write(new byte[] { 0, 0, 255, 0, 255, 0, 0, 0, 255, 0, 0, 255, 255, 255, 0, 0 });
        return path;
    }

    public static string Wav(string path)
    {
        const int samples = 8000;
        using var output = new BinaryWriter(File.Create(path), Encoding.ASCII);
        output.Write("RIFF"u8); output.Write(36 + samples * 2); output.Write("WAVEfmt "u8);
        output.Write(16); output.Write((ushort)1); output.Write((ushort)1); output.Write(samples);
        output.Write(samples * 2); output.Write((ushort)2); output.Write((ushort)16); output.Write("data"u8); output.Write(samples * 2);
        for (var i = 0; i < samples; i++) output.Write((short)(10000 * Math.Sin(2 * Math.PI * 440 * i / samples)));
        return path;
    }

    public static async Task Run(string executable, string[] arguments, int timeout = 60)
    {
        var start = new ProcessStartInfo(executable) { UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true, RedirectStandardError = true };
        foreach (var argument in arguments) start.ArgumentList.Add(argument);
        using var process = Process.Start(start) ?? throw new IOException("测试数据引擎无法启动。");
        var stdout = process.StandardOutput.ReadToEndAsync();
        var stderr = process.StandardError.ReadToEndAsync();
        try { await process.WaitForExitAsync().WaitAsync(TimeSpan.FromSeconds(timeout)); }
        catch { if (!process.HasExited) process.Kill(entireProcessTree: true); throw; }
        var error = await stderr;
        await stdout;
        if (process.ExitCode != 0) throw new IOException("测试数据生成失败：" + error[..Math.Min(error.Length, 3000)]);
    }

    public static OwnedProcess Owner()
    {
        var executable = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.Windows), "System32", "WindowsPowerShell", "v1.0", "powershell.exe");
        var start = new ProcessStartInfo(executable) { UseShellExecute = false, CreateNoWindow = true };
        foreach (var argument in new[] { "-NoProfile", "-NonInteractive", "-Command", "Start-Sleep -Seconds 120" }) start.ArgumentList.Add(argument);
        return new OwnedProcess(Process.Start(start) ?? throw new IOException("测试父进程无法启动。"));
    }

    internal sealed class OwnedProcess(Process process) : IDisposable
    {
        public int Id => process.Id;
        public void Kill() => process.Kill();
        public Task WaitForExitAsync() => process.WaitForExitAsync();
        public void Dispose() { try { if (!process.HasExited) process.Kill(); } finally { process.Dispose(); } }
    }
}
