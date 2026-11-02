#!/usr/bin/env python3
"""
Prepares a launcher release for Settings -> Update: copies each board's built image
and writes latest.json next to them, then prints the gh command that publishes them.

    python tools/make_release.py [--out build/release]

Every board under boards/ whose image is built at the current version goes in:
guition_p4_43 from launcher/build/launcher.bin, any other <id> from
launcher/build_<id>/launcher_<id>.bin (idf.py -B build_<id> -DLX_BOARD=<id> build).

latest.json lists each board under "boards"; its top-level fields are guition_p4_43's,
which is where launchers from before per-board releases look. The board reads
releases/latest/download/latest.json, so the newest published release (not a draft
or pre-release) is the one devices install.
"""
import argparse
import hashlib
import json
import os
import re
import shutil

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REPO = "giltal/ESP32_P4_Launcher"
FIRST = "guition_p4_43"


def image_of(board):
    name = "launcher.bin" if board == FIRST else f"launcher_{board}.bin"
    folder = "build" if board == FIRST else f"build_{board}"
    return os.path.join(ROOT, "launcher", folder, name), name


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=os.path.join(ROOT, "build", "release"))
    args = ap.parse_args()

    header = open(os.path.join(ROOT, "components", "lx_common", "include", "lx_common.h")).read()
    version = re.search(r'LX_LAUNCHER_VERSION\s+"([^"]+)"', header).group(1)
    tag = f"v{version}"
    os.makedirs(args.out, exist_ok=True)

    boards, files = {}, []
    for board in sorted(os.listdir(os.path.join(ROOT, "boards"))):
        path, name = image_of(board)
        if not os.path.isfile(path):
            print(f"skip {board}: {os.path.relpath(path, ROOT)} not built")
            continue
        data = open(path, "rb").read()
        if version.encode() not in data:
            print(f"skip {board}: image is not version {version}, rebuild it")
            continue
        shutil.copyfile(path, os.path.join(args.out, name))
        files.append(name)
        boards[board] = {
            "url": f"https://github.com/{REPO}/releases/download/{tag}/{name}",
            "size": len(data),
            "sha256": hashlib.sha256(data).hexdigest(),
        }
    if not boards:
        raise SystemExit("no board image built at this version")
    if FIRST not in boards:
        print(f"warning: no {FIRST} image; launchers older than per-board releases will see no update")

    manifest = {"version": version, **boards.get(FIRST, {}), "boards": boards}
    with open(os.path.join(args.out, "latest.json"), "w") as f:
        json.dump(manifest, f, indent=2)
    files.append("latest.json")
    print(json.dumps(manifest, indent=2))
    assets = " ".join(f'"{args.out}/{n}"' for n in files)
    print(f"\ngh release create {tag} -R {REPO} --title \"Launcher {version}\" {assets}")


if __name__ == "__main__":
    main()
