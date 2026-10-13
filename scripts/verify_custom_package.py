import hashlib
import json
from pathlib import Path, PurePosixPath
import sys
from zipfile import ZipFile

artifact, commit, destination = sys.argv[1:]
artifact = Path(artifact).resolve()
root = Path(destination).resolve()

def expand(archive, target):
    if target.exists():
        raise ValueError("Extraction target must be new")
    target.mkdir(parents=True)
    with ZipFile(archive) as source:
        entries = source.infolist()
        if len(entries) > 50000 or sum(e.file_size for e in entries) > 4 * 1024**3:
            raise ValueError("Archive exceeds package limits")
        names = set()
        for entry in entries:
            name = entry.filename.replace("\\", "/").rstrip("/")
            parts = PurePosixPath(name).parts
            if not parts or name.startswith("/") or any(p in (".", "..") or ":" in p for p in parts):
                raise ValueError("Invalid archive path")
            if name.casefold() in names or ((entry.external_attr >> 16) & 0xF000) == 0xA000:
                raise ValueError("Duplicate path or symbolic link")
            names.add(name.casefold())
            output = target.joinpath(*parts)
            if not output.resolve().is_relative_to(target):
                raise ValueError("Archive path escapes target")
            if entry.is_dir():
                output.mkdir(parents=True, exist_ok=True)
            else:
                output.parent.mkdir(parents=True, exist_ok=True)
                with source.open(entry) as reader, output.open("xb") as writer:
                    while data := reader.read(1024 * 1024):
                        writer.write(data)

outer = artifact.parent / "extracted-artifact"
expand(artifact, outer)
build = json.loads((outer / "custom-build.json").read_text(encoding="utf-8-sig"))
if build["commit"] != commit:
    raise ValueError("Artifact commit mismatch")
archive = outer / (build["package"] + ".zip")
record = Path(str(archive) + ".sha256").read_text(encoding="utf-8-sig").strip().split(maxsplit=1)
if len(record) != 2 or record[1] != archive.name:
    raise ValueError("Invalid package checksum record")
package_digest = hashlib.file_digest(archive.open("rb"), "sha256").hexdigest()
if package_digest != record[0].lower():
    raise ValueError("Package ZIP digest mismatch")
expand(archive, root)
package = root / build["package"]
manifest = json.loads((package / "package-manifest.json").read_text(encoding="utf-8-sig"))
if manifest["edition"] != "PulseCustom" or manifest["schema"] != 1 or manifest["commit"] != commit:
    raise ValueError("Package identity mismatch")
names = set()
for entry in manifest["files"]:
    path = package / entry["path"]
    if not path.resolve().is_relative_to(package) or path.is_symlink() or not path.is_file():
        raise ValueError("Unsafe manifest file")
    if entry["path"].casefold() in names:
        raise ValueError("Duplicate manifest entry")
    names.add(entry["path"].casefold())
    if path.stat().st_size != entry["bytes"] or hashlib.file_digest(path.open("rb"), "sha256").hexdigest() != entry["sha256"]:
        raise ValueError("Package file differs from manifest: " + entry["path"])
actual_names = {p.relative_to(package).as_posix().casefold() for p in package.rglob("*") if p.is_file()}
if actual_names != names | {"package-manifest.json"}:
    raise ValueError("Unexpected or missing package files")
summary = {"commit": commit, "directory": str(package), "files": len(names),
           "package_sha256": package_digest,
           "pulse_sha256": hashlib.file_digest((package / "pulse.exe").open("rb"), "sha256").hexdigest()}
(artifact.parent / "verified-package.json").write_text(json.dumps(summary, indent=2), encoding="utf-8")
print(json.dumps(summary, ensure_ascii=False, indent=2))
