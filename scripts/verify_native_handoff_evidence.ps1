#requires -Version 7.2
param([Parameter(Mandatory)][string]$OutputDirectory)
$ErrorActionPreference='Stop'
$verifiedCommit='6748a95b27c105fcbb4f528f34605f085c55c10a'
$runId='39431806316'
$artifactId='12269931381'
$digest='211ce443cbc343e7317570403302720d4fdcf0985030137a0f421e98466b3090'
& git fetch --no-tags --depth=1 origin $verifiedCommit
if($LASTEXITCODE){throw '无法读取已验证源码'}
$changed=@(& git diff --name-only $verifiedCommit HEAD -- src CMakeLists.txt cmake)
if($LASTEXITCODE -or $changed.Count){throw '原生源码或构建规则已经变化，必须重新执行接管回归'}
$artifact=& gh api "repos/chenzai666/pulse/actions/artifacts/$artifactId" | ConvertFrom-Json
if($LASTEXITCODE -or $artifact.expired -or $artifact.digest -cne ('sha256:'+$digest) -or
   $artifact.workflow_run.head_sha -cne $verifiedCommit -or [string]$artifact.workflow_run.id -cne $runId){throw '接管回归证据身份不匹配'}
$output=[IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Path $output -Force | Out-Null
$env:PULSE_HANDOFF_EVIDENCE_DIR=$output
$env:PULSE_HANDOFF_EVIDENCE_DIGEST=$digest
$env:PULSE_HANDOFF_EVIDENCE_ARTIFACT=$artifactId
@'
import hashlib, io, os, pathlib, urllib.error, urllib.request, zipfile
class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, *args): return None
request=urllib.request.Request('https://api.github.com/repos/chenzai666/pulse/actions/artifacts/'+os.environ['PULSE_HANDOFF_EVIDENCE_ARTIFACT']+'/zip',headers={'Authorization':'Bearer '+os.environ['GH_TOKEN']})
try: reply=urllib.request.build_opener(NoRedirect).open(request,timeout=30)
except urllib.error.HTTPError as error: reply=error
if reply.code != 302: raise RuntimeError('Evidence download did not redirect')
location=reply.headers['Location']; reply.close()
with urllib.request.urlopen(location,timeout=60) as stream: data=stream.read(1024*1024+1)
if len(data)>1024*1024 or hashlib.sha256(data).hexdigest()!=os.environ['PULSE_HANDOFF_EVIDENCE_DIGEST']:
    raise RuntimeError('Evidence digest or size mismatch')
output=pathlib.Path(os.environ['PULSE_HANDOFF_EVIDENCE_DIR'])
with zipfile.ZipFile(io.BytesIO(data)) as archive:
    for name,count in [('adapter',32),('handoff',84),('history',8),('save',10)]:
        info=archive.getinfo('picker-verification/'+name+'.log')
        if info.file_size>1024*1024: raise RuntimeError('Evidence log too large')
        text=archive.read(info).decode('utf-8-sig')
        lines=text.splitlines()
        if any(line.startswith('[FAIL]') for line in lines) or sum(line.startswith('[PASS]') for line in lines)!=count:
            raise RuntimeError('Evidence cases did not all pass: '+name)
        if name=='handoff' and ('SUMMARY failures=0' not in lines or
           'save-confirm: one Pulse confirmation reaches native OnFileOk exactly once after delayed host validation' not in text or
           'save-confirm: native save remains concealed throughout delayed OnFileOk validation' not in text):
            raise RuntimeError('Missing single-save/opacity evidence')
        (output/('reused-'+name+'.log')).write_text(text,encoding='utf-8')
        print('[PASS] Verified prior '+name+' evidence: '+str(count)+' cases')
'@ | python -
if($LASTEXITCODE){throw '接管回归证据校验失败'}
[ordered]@{source_commit=$verifiedCommit;run_id=$runId;artifact_id=$artifactId;sha256=$digest;source_unchanged=$true;passed=134;failed=0} |
    ConvertTo-Json | Set-Content -LiteralPath (Join-Path $output 'native-handoff-provenance.json') -Encoding utf8
