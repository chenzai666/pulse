#requires -Version 7.2
param(
    [Parameter(Mandatory)][ValidatePattern('^[0-9]+$')][string]$SourceRunId,
    [Parameter(Mandatory)][ValidatePattern('^[0-9]+$')][string]$VerificationRunId,
    [Parameter(Mandatory)][ValidatePattern('^[0-9a-f]{40}$')][string]$SourceCommit
)
$ErrorActionPreference = 'Stop'
$taskRepo = Split-Path -Parent $PSScriptRoot
Set-Location $taskRepo
$work = Join-Path $taskRepo 'build-custom-installer'
New-Item -ItemType Directory -Path $work -Force | Out-Null
$env:PULSE_INSTALL_SOURCE_RUN = $SourceRunId
$env:PULSE_INSTALL_VERIFY_RUN = $VerificationRunId
$env:PULSE_INSTALL_SOURCE_COMMIT = $SourceCommit
$env:PULSE_INSTALL_WORK = $work
# 仅接受同仓库、同源码的成功构建，并逐个验证 GitHub artifact 摘要。
@'
import hashlib, json, os, re, urllib.error, urllib.request, zipfile
from pathlib import Path
root = Path(os.environ['PULSE_INSTALL_WORK'])
token = os.environ['GH_TOKEN']
base = 'https://api.github.com/repos/chenzai666/pulse'
def api(path):
    with urllib.request.urlopen(urllib.request.Request(base+path, headers={'Authorization':'Bearer '+token,'Accept':'application/vnd.github+json'}),timeout=30) as r:
        return json.load(r)
class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self,*args): return None
records=[]
for key, name, output in [('PULSE_INSTALL_SOURCE_RUN','Pulse-Custom-complete','complete.zip'),('PULSE_INSTALL_VERIFY_RUN','Pulse-Custom-build-diagnostics','verification.zip')]:
    run_id=os.environ[key]
    run=api('/actions/runs/'+run_id)
    if run['conclusion']!='success' or run['head_sha']!=os.environ['PULSE_INSTALL_SOURCE_COMMIT'] or run['path']!='.github/workflows/custom-build.yml':
        raise RuntimeError('来源或定向验证运行的结论、源码或工作流不匹配')
    matches=[a for a in api('/actions/runs/'+run_id+'/artifacts')['artifacts'] if a['name']==name and not a['expired']]
    if len(matches)!=1: raise RuntimeError('缺少唯一完整构建产物: '+name)
    item=matches[0]; expected=item.get('digest','').removeprefix('sha256:')
    if not re.fullmatch('[0-9a-f]{64}',expected): raise RuntimeError('构建产物缺少可信摘要')
    request=urllib.request.Request(base+'/actions/artifacts/'+str(item['id'])+'/zip',headers={'Authorization':'Bearer '+token})
    try: reply=urllib.request.build_opener(NoRedirect).open(request,timeout=30)
    except urllib.error.HTTPError as error: reply=error
    if reply.code!=302: raise RuntimeError('构建产物下载重定向失败')
    location=reply.headers['Location']; reply.close()
    digest=hashlib.sha256()
    with urllib.request.urlopen(location,timeout=120) as reader, (root/output).open('xb') as writer:
        while block:=reader.read(1024*1024): digest.update(block);writer.write(block)
    if digest.hexdigest()!=expected: raise RuntimeError('构建产物摘要不匹配')
    records.append({'run':run_id,'artifact':item['id'],'sha256':expected})
with zipfile.ZipFile(root/'verification.zip') as archive:
    required=['bench_data/native-dialogs-wire.log','bench_data/native-dialogs-takeover.log','bench_data/native-dialogs-adapter.log','bench_data/native-dialogs-save-model.log','bench_data/native-dialogs-save-picker.log','bench_data/native-dialogs-settings-integration/results.log','bench_data/native-dialogs-settings-navigation/results.log']
    for name in required:
        text=archive.read(name).decode('utf-8-sig',errors='strict')
        if '[PASS]' not in text or '[FAIL]' in text: raise RuntimeError('定向验证日志未通过: '+name)
    takeover=archive.read(required[1]).decode('utf-8-sig')
    if 'SUMMARY failures=0' not in takeover: raise RuntimeError('缺少完整接管与恢复结果')
(root/'provenance.json').write_text(json.dumps({'source_commit':os.environ['PULSE_INSTALL_SOURCE_COMMIT'],'packaging_commit':os.environ.get('GITHUB_SHA',''),'artifacts':records},indent=2),encoding='utf-8')
print('来源与接管定向验证均通过，产物摘要已验证')
'@ | python -
if ($LASTEXITCODE) { throw '安装载荷来源或验证证据校验失败' }
& python scripts/verify_custom_package.py (Join-Path $work 'complete.zip') $SourceCommit (Join-Path $work 'verified') |
    Tee-Object -FilePath (Join-Path $work 'payload-verification.log')
if ($LASTEXITCODE) { throw '完整载荷逐文件校验失败' }
$stage = Join-Path $work ('verified/Pulse-Custom-' + $SourceCommit.Substring(0,9))
$iscc = Join-Path ${env:ProgramFiles(x86)} 'Inno Setup 6/ISCC.exe'
if (-not (Test-Path -LiteralPath $iscc)) {
    & choco install innosetup --version=6.4.3 --yes --no-progress --limit-output
    if ($LASTEXITCODE -or -not (Test-Path -LiteralPath $iscc)) { throw '无法安装固定版本 Inno Setup 编译器' }
}
& "$PSScriptRoot/package_custom_installer.ps1" -PackageDirectory $stage -OutputDirectory (Join-Path $taskRepo 'dist-installer') -IsccPath $iscc
& "$PSScriptRoot/test_custom_installer.ps1" -PackageDirectory $stage -InstallerDirectory (Join-Path $taskRepo 'dist-installer') -OutputDirectory $work
Copy-Item -LiteralPath (Join-Path $work 'provenance.json') -Destination (Join-Path $taskRepo 'dist-installer/provenance.json')
