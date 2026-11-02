#!/usr/bin/env python3
"""
Prepares the online app catalogue for Settings -> Apps: copies every built app that
is not a developer app (PAPP_MF_HIDDEN), its icon and the folder icons, and writes
apps.json next to them. --publish uploads them to the "apps" release on GitHub.

    python apps/build_all.py                       # build the apps first
    python tools/make_apps_release.py [--out build/apps_release] [--publish]

The "apps" release is a pre-release, so it never becomes releases/latest (where the
launcher looks for its own update); its files are replaced in place on each publish.
An app's version is its manifest's (the app's version.txt); the launcher offers an
update when the catalogue's is newer than the installed one.

apps.json:
  {"format": 1,
   "folders": {"Emulators": {"icon": url}, ...},
   "apps": [{"id", "name", "version", "folder", "dir", "file", "url", "size", "sha256", "icon"}]}
folder: where a new install goes (/sd/apps/<folder>/<dir>/<file>; "" = the main page).
An installed app is updated where it is, wherever the user moved it.
"""
import argparse
import hashlib
import json
import os
import shutil
import struct
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REPO = "giltal/ESP32_P4_Launcher"
TAG = "apps"
BASE = f"https://github.com/{REPO}/releases/download/{TAG}/"
ICONS = os.path.join(ROOT, "apps", "common", "icons")
FOLDERS = {"emu.": "Emulators", "game.": "Games"}          # by app id prefix; others: main page
FOLDER_ICONS = {"Emulators": "folder_emulators.png", "Games": "folder_games.png"}
PAPP_MAGIC = 0x32504150          # "PAP2"
MANIFEST_MAGIC = 0x464E414D      # "MANF"
MF_HIDDEN = 1 << 1


def manifest(path):
    """id, name, version, flags of a .papp, or None."""
    data = open(path, "rb").read()
    magic, = struct.unpack_from("<I", data, 0)
    header_size, = struct.unpack_from("<H", data, 6)
    manifest_off, = struct.unpack_from("<I", data, 32)
    if magic != PAPP_MAGIC or not manifest_off:
        return None
    m = data[header_size + manifest_off:header_size + manifest_off + 96]
    if struct.unpack_from("<I", m, 0)[0] != MANIFEST_MAGIC:
        return None
    cstr = lambda b: b.split(b"\0")[0].decode("utf-8", "replace")
    return dict(id=cstr(m[8:40]), name=cstr(m[40:72]), version=cstr(m[72:88]),
                flags=struct.unpack_from("<I", m, 92)[0])


def sha256(path):
    return hashlib.sha256(open(path, "rb").read()).hexdigest()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=os.path.join(ROOT, "build", "apps_release"))
    ap.add_argument("--publish", action="store_true", help=f"upload to the '{TAG}' release with gh")
    ap.add_argument("--base", default=BASE, help="where the files will be (to test from another server, "
                    "named in /sd/update/apps_url.txt on the card)")
    args = ap.parse_args()
    base = args.base if args.base.endswith("/") else args.base + "/"
    os.makedirs(args.out, exist_ok=True)
    for old in os.listdir(args.out):                # emptied, not removed: a test server may serve it
        if os.path.isfile(os.path.join(args.out, old)):
            os.remove(os.path.join(args.out, old))

    apps, files, folders = [], [], {}
    built = os.path.join(ROOT, "build", "apps")
    for name in sorted(os.listdir(built)):
        papp = os.path.join(built, name, name + ".papp")
        if not os.path.isfile(papp):
            continue
        m = manifest(papp)
        if not m:
            print(f"skip {name}: no manifest")
            continue
        if m["flags"] & MF_HIDDEN:
            continue
        folder = next((f for p, f in FOLDERS.items() if m["id"].startswith(p)), "")
        entry = dict(id=m["id"], name=m["name"], version=m["version"], folder=folder, dir=name,
                     file=name + ".papp", url=base + name + ".papp", size=os.path.getsize(papp),
                     sha256=sha256(papp))
        shutil.copyfile(papp, os.path.join(args.out, name + ".papp"))
        files.append(name + ".papp")
        icon = os.path.join(built, name, "icon.png")
        if os.path.isfile(icon):
            shutil.copyfile(icon, os.path.join(args.out, name + ".png"))
            files.append(name + ".png")
            entry["icon"] = base + name + ".png"
        apps.append(entry)
        if folder and folder not in folders:
            folders[folder] = {}
            fi = FOLDER_ICONS.get(folder)
            if fi and os.path.isfile(os.path.join(ICONS, fi)):
                shutil.copyfile(os.path.join(ICONS, fi), os.path.join(args.out, fi))
                files.append(fi)
                folders[folder]["icon"] = base + fi
        print(f"{m['name']:16s} {m['version']:8s} {folder or '(main page)'}")
    if not apps:
        raise SystemExit("no app built: run python apps/build_all.py")

    with open(os.path.join(args.out, "apps.json"), "w") as f:
        json.dump(dict(format=1, folders=folders, apps=apps), f, indent=1)
    files.append("apps.json")
    print(f"{len(apps)} apps in {args.out}")

    paths = [os.path.join(args.out, n) for n in files]
    if not args.publish:
        print(f"\npublish with: python tools/make_apps_release.py --publish")
        return
    exists = subprocess.run(["gh", "release", "view", TAG, "-R", REPO], capture_output=True).returncode == 0
    if not exists:
        subprocess.run(["gh", "release", "create", TAG, "-R", REPO, "--prerelease", "--title", "Apps",
                        "--notes", "Apps for Settings -> Apps on the launcher. Replaced in place on each "
                        "publish; the launcher compares versions."], check=True)
    subprocess.run(["gh", "release", "upload", TAG, "-R", REPO, "--clobber"] + paths, check=True)
    print(f"published {len(apps)} apps to {BASE}apps.json")


if __name__ == "__main__":
    sys.exit(main())
