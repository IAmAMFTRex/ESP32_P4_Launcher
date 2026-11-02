#!/usr/bin/env python3
"""
Prepares a launcher release for Settings -> Update: copies the built launcher.bin
and writes latest.json next to it, then prints the gh command that publishes both.

    python tools/make_release.py [--out build/release]

The board reads releases/latest/download/latest.json, so the newest published
release (not a draft or pre-release) is the one devices install.
"""
import argparse
import hashlib
import json
import os
import re
import shutil

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REPO = "giltal/ESP32_P4_Launcher"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=os.path.join(ROOT, "build", "release"))
    args = ap.parse_args()

    header = open(os.path.join(ROOT, "components", "lx_common", "include", "lx_common.h")).read()
    version = re.search(r'LX_LAUNCHER_VERSION\s+"([^"]+)"', header).group(1)
    binary = os.path.join(ROOT, "launcher", "build", "launcher.bin")
    data = open(binary, "rb").read()
    if f"{version}".encode() not in data:
        raise SystemExit(f"launcher.bin is not version {version}: build the launcher first")

    os.makedirs(args.out, exist_ok=True)
    shutil.copyfile(binary, os.path.join(args.out, "launcher.bin"))
    tag = f"v{version}"
    manifest = {
        "version": version,
        "url": f"https://github.com/{REPO}/releases/download/{tag}/launcher.bin",
        "size": len(data),
        "sha256": hashlib.sha256(data).hexdigest(),
    }
    with open(os.path.join(args.out, "latest.json"), "w") as f:
        json.dump(manifest, f, indent=2)
    print(json.dumps(manifest, indent=2))
    print(f"\ngh release create {tag} -R {REPO} --title \"Launcher {version}\" "
          f"\"{args.out}/launcher.bin\" \"{args.out}/latest.json\"")


if __name__ == "__main__":
    main()
