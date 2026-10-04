#!/usr/bin/env python3
"""Restore exact vendored ScummVM bytes offline; never executes the binary."""
import argparse, gzip, hashlib, json, os, pathlib, tempfile
HERE = pathlib.Path(__file__).resolve().parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--output", type=pathlib.Path, default=HERE.parents[2]/"files/ROOTFS/opt/scummvm/scummvm")
args=parser.parse_args()
manifest=json.loads((HERE/"manifest.json").read_text())
archive=(HERE/"scummvm.aarch64.gz").read_bytes()
if len(archive)!=manifest["compressed_size"] or hashlib.sha256(archive).hexdigest()!=manifest["compressed_sha256"]:
    raise SystemExit("Compressed ScummVM artifact checksum/size mismatch")
raw=gzip.decompress(archive)
if len(raw)!=manifest["size"] or hashlib.sha256(raw).hexdigest()!=manifest["sha256"]:
    raise SystemExit("ScummVM reconstructed checksum/size mismatch")
target=args.output.absolute()
for parent in target.parents:
    if parent.is_symlink(): raise SystemExit("Refusing symlink ancestor: "+str(parent))
if target.is_symlink(): raise SystemExit("Refusing symlink destination")
if target.exists():
    if not target.is_file() or hashlib.sha256(target.read_bytes()).hexdigest()!=manifest["sha256"]:
        raise SystemExit("Existing ScummVM destination differs; preserve it for review")
    print("Exact ScummVM executable already materialized: "+str(target))
else:
    target.parent.mkdir(parents=True,exist_ok=True)
    fd,tmp=tempfile.mkstemp(prefix=".scummvm-materialize-",dir=target.parent)
    try:
        with os.fdopen(fd,"wb") as stream: stream.write(raw)
        os.chmod(tmp,0o755)
        os.replace(tmp,target)
    finally:
        if os.path.exists(tmp): os.unlink(tmp)
    print("Materialized verified ScummVM executable: "+str(target))
