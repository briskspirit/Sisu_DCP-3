#!/usr/bin/env python3
"""Extract gzip members from old Freeman/TipTec Nokia installers.

The Nokia NSE-8 v6.00 installer is a Windows 3.x Freeman Installer wrapper
with gzip-compressed payload members appended in sequence. 7-Zip can only see
the first member, so this script scans all valid gzip streams and names the
payloads using the embedded setup manifest.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import shutil
import zlib
from pathlib import Path


PRELUDE_NAMES = [
    "install.exe",
    "setup.inf",
    "fienu.dll",
    "fienu.hlp",
    "3210.bmp",
]

PAYLOAD_RE = re.compile(
    rb"[A-Za-z0-9_&.-]+\.(?:olg|ui|wug|mbx|dwn|pp|gms|000|00a|00b|00c|00d|00e|00f|00y)",
    re.IGNORECASE,
)


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def valid_gzip_members(blob: bytes) -> list[dict[str, object]]:
    members: list[dict[str, object]] = []
    start = 0
    while True:
        offset = blob.find(b"\x1f\x8b\x08", start)
        if offset < 0:
            break

        try:
            decompressor = zlib.decompressobj(16 + zlib.MAX_WBITS)
            payload = decompressor.decompress(blob[offset:]) + decompressor.flush()
            consumed = len(blob[offset:]) - len(decompressor.unused_data)
        except zlib.error:
            start = offset + 1
            continue

        if consumed > 18 and payload:
            members.append(
                {
                    "offset": offset,
                    "compressed_size": consumed,
                    "payload": payload,
                }
            )
            start = offset + consumed
        else:
            start = offset + 1

    return members


def payload_names(setup_payload: bytes, count: int) -> list[str]:
    seen: list[str] = []
    for match in PAYLOAD_RE.finditer(setup_payload):
        name = match.group(0).decode("latin1").lower()
        if name not in seen:
            seen.append(name)

    names = PRELUDE_NAMES + seen
    return names[:count]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("installer", type=Path)
    parser.add_argument("outdir", type=Path)
    parser.add_argument("--clean", action="store_true")
    args = parser.parse_args()

    if args.clean and args.outdir.exists():
        shutil.rmtree(args.outdir)
    args.outdir.mkdir(parents=True, exist_ok=True)

    blob = args.installer.read_bytes()
    members = valid_gzip_members(blob)
    if len(members) < len(PRELUDE_NAMES):
        raise SystemExit(f"Only found {len(members)} gzip members; expected at least {len(PRELUDE_NAMES)}")

    names = payload_names(members[1]["payload"], len(members))  # setup.inf is stream 1
    if len(names) < len(members):
        names.extend(f"stream_{i:03d}.bin" for i in range(len(names), len(members)))

    manifest = {
        "source": str(args.installer),
        "source_sha256": sha256(blob),
        "member_count": len(members),
        "members": [],
    }

    for idx, (member, name) in enumerate(zip(members, names)):
        payload = member["payload"]
        path = args.outdir / name
        path.write_bytes(payload)
        manifest["members"].append(
            {
                "index": idx,
                "name": name,
                "offset": member["offset"],
                "compressed_size": member["compressed_size"],
                "size": len(payload),
                "sha256": sha256(payload),
            }
        )

    (args.outdir / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"extracted {len(members)} members to {args.outdir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
