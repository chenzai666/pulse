"""核实已通过的设置回归与当前界面源码一致，再复用对应记录。"""
import hashlib
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import urllib.error
import urllib.request
import zipfile

baseline = "18f8755e17305c84f4c17362ee757eb450ec98ad"
artifact_id = 12554659030
expected_digest = "26500528e99a51225faa6495dde41f2f67c4f25a84e1439475a7de4211bbc44a"
subprocess.run(["git", "fetch", "--no-tags", "--depth=1", "origin", baseline], check=True)
changed = subprocess.check_output(
    ["git", "diff", "--name-only", baseline, "HEAD", "--", "src", "CMakeLists.txt", "cmake"],
    text=True, encoding="utf-8").splitlines()
if changed != ["src/app/app_main.cpp"]:
    raise RuntimeError("设置或选择器源码已改变，必须重新验证")
old_main = subprocess.check_output(["git", "show", baseline + ":src/app/app_main.cpp"])
removed = b'            wcscmp(__wargv[i], L"--native-dialog-picker") == 0 ||\n'
new_main = Path("src/app/app_main.cpp").read_bytes().replace(b"\r\n", b"\n")
if old_main.count(removed) != 1 or old_main.replace(removed, b"") != new_main:
    raise RuntimeError("主入口变化超出删除旧命令行名称的范围")

class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, *args):
        return None

url = f"https://api.github.com/repos/chenzai666/pulse/actions/artifacts/{artifact_id}/zip"
request = urllib.request.Request(url, headers={"Authorization": "Bearer " + os.environ["GH_TOKEN"]})
try:
    reply = urllib.request.build_opener(NoRedirect).open(request, timeout=30)
except urllib.error.HTTPError as error:
    reply = error
if reply.code != 302:
    raise RuntimeError("无法读取固定验证记录")
location = reply.headers["Location"]
reply.close()
with urllib.request.urlopen(location, timeout=60) as stream:
    content = stream.read(8 * 1024 * 1024 + 1)
if len(content) > 8 * 1024 * 1024 or hashlib.sha256(content).hexdigest() != expected_digest:
    raise RuntimeError("验证记录摘要或大小不符")
output = Path(sys.argv[1]).resolve()
output.mkdir(parents=True, exist_ok=True)
with zipfile.ZipFile(io.BytesIO(content)) as archive:
    for filename in ("settings-integration.log", "save.log", "columns.log"):
        matches = [item for item in archive.infolist() if item.filename.endswith("/" + filename)]
        if len(matches) != 1 or matches[0].file_size > 1024 * 1024:
            raise RuntimeError("验证日志不完整")
        data = archive.read(matches[0])
        if b"[FAIL]" in data or b"[PASS]" not in data:
            raise RuntimeError("历史回归存在失败")
        if filename == "settings-integration.log" and b"failures=0" not in data:
            raise RuntimeError("设置回归未正常结束")
        (output / ("verified-" + filename)).write_bytes(data)
record = {"commit": baseline, "run_id": 40078968483, "artifact_id": artifact_id,
          "sha256": expected_digest, "source_match": True}
(output / "settings-evidence.json").write_text(json.dumps(record, indent=2), encoding="utf-8")
print("设置回归记录与当前界面源码一致，证据摘要通过")
