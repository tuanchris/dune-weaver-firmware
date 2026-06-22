#!/usr/bin/env python3

# Build a Dune Weaver Firmware release for the single `sandtable` (MKS-DLC32 /
# ESP32, 4M) target. Produces flat .bin images + a manifest.json whose image
# `path` fields are bare filenames, so they map 1:1 onto GitHub release assets
# (which live in a flat namespace). Also bundles everything into one .zip.
#
#   python3 build-dw-release.py          # build + assemble release/<tag>/
#   python3 build-dw-release.py -v       # verbose pio output
#
# The release tag is read from `git describe`; tag the commit (e.g. `git tag
# v0.1.0`) before running so the firmware bakes the matching version string.

import hashlib
import json
import os
import shutil
import subprocess
import sys
from zipfile import ZipFile, ZipInfo

VERBOSE = "-v" in sys.argv
ENV = "sandtable"
MCU = "esp32"
PIO = shutil.which("pio") or shutil.which("platformio") or "/opt/homebrew/bin/pio"

REPO = "https://github.com/tuanchris/dune-weaver-firmware"

# ESP32 4M flash layout (see min_littlefs.csv). Offsets are where esptool
# writes each image during a fresh install.
IMAGES = [
    # name,            offset,     filename
    ("bootloader",     "0x1000",   "bootloader.bin"),
    ("partitions",     "0x8000",   "partitions.bin"),
    ("bootapp",        "0xe000",   "boot_app0.bin"),
    ("firmware",       "0x10000",  "firmware.bin"),
    ("filesystem",     "0x3d0000", "littlefs.bin"),
]


def run(cmd):
    print("+", " ".join(cmd))
    if VERBOSE:
        rc = subprocess.run(cmd).returncode
    else:
        proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        for raw in proc.stdout:
            line = raw.decode("utf8", "replace")
            low = line.lower()
            if "took" in line or "uploading" in line or ("error" in low and "compiling" not in low):
                print(line, end="")
        proc.wait()
        rc = proc.returncode
    if rc != 0:
        sys.exit("Command failed (%d): %s" % (rc, " ".join(cmd)))


def git(*args):
    return subprocess.check_output(["git", *args]).strip().decode("utf-8")


def sha256(path):
    with open(path, "rb") as f:
        return hashlib.sha256(f.read()).hexdigest()


tag = git("describe", "--tags", "--abbrev=0")
try:
    git("describe", "--tags", "--exact-match", "HEAD")
except subprocess.CalledProcessError:
    print("WARNING: HEAD is not exactly on tag %s; firmware version string will\n"
          "         include a branch-commit suffix. Tag this commit first to ship\n"
          "         a clean release." % tag)

print("Building Dune Weaver Firmware release %s (env: %s)\n" % (tag, ENV))

# 1. Build firmware + filesystem image for the sandtable env.
run([PIO, "run", "--disable-auto-clean", "-e", ENV])
run([PIO, "run", "--disable-auto-clean", "-e", ENV, "-t", "buildfs"])

buildDir = os.path.join(".pio", "build", ENV)
bootapp_src = os.path.join(
    os.path.expanduser("~"), ".platformio", "packages",
    "framework-arduinoespressif32", "tools", "partitions", "boot_app0.bin")
shutil.copy(bootapp_src, os.path.join(buildDir, "boot_app0.bin"))

# 2. Stage flat images under release/<tag>/.
relPath = os.path.join("release", tag)
if os.path.exists(relPath):
    shutil.rmtree(relPath)
os.makedirs(relPath)

manifest = {
    "name": "Dune Weaver Firmware",
    "version": tag,
    "source_url": REPO + "/tree/" + tag,
    "release_url": REPO + "/releases/tag/" + tag,
    "images": {},
    "installable": {
        "name": "installable",
        "description": "Things you can install",
        "choice-name": "Processor type",
        "choices": [],
    },
}

for name, offset, filename in IMAGES:
    src = os.path.join(buildDir, filename)
    if not os.path.isfile(src):
        sys.exit("Missing build artifact: %s" % src)
    dst = os.path.join(relPath, filename)
    shutil.copy(src, dst)
    manifest["images"][MCU + "-" + name] = {
        "size": os.path.getsize(dst),
        "offset": offset,
        "path": filename,  # flat: matches GitHub release asset name
        "signature": {"algorithm": "SHA2-256", "value": sha256(dst)},
    }
    print("  image %-12s %8d bytes @ %s" % (name, os.path.getsize(dst), offset))

# 3. Installable tree: one MCU, one variant, three install types.
fresh = [MCU + "-bootloader", MCU + "-partitions", MCU + "-bootapp",
         MCU + "-firmware", MCU + "-filesystem"]
manifest["installable"]["choices"].append({
    "name": "esp32",
    "description": "ESP32-WROOM (MKS-DLC32)",
    "choice-name": "Installation type",
    "choices": [{
        "name": "sandtable",
        "description": "Dune Weaver sand table (WiFi + HTTP/JSON API)",
        "choice-name": "Installation type",
        "choices": [
            {"name": "fresh-install",
             "description": "Complete install, erasing all previous data. Upload your table's config.yaml afterward.",
             "erase": True, "images": fresh},
            {"name": "firmware-update",
             "description": "Update firmware only, preserving the filesystem (config.yaml, patterns).",
             "erase": False, "images": [MCU + "-firmware"]},
            {"name": "filesystem-update",
             "description": "Replace the filesystem only, erasing previous filesystem data.",
             "erase": False, "images": [MCU + "-filesystem"]},
        ],
    }],
})

with open(os.path.join(relPath, "manifest.json"), "w") as f:
    json.dump(manifest, f, indent=2)
print("  manifest.json")

# 4. One convenience bundle with the flat images + manifest at the zip root.
zipName = os.path.join(relPath, "dune-weaver-firmware-%s-esp32.zip" % tag)
with ZipFile(zipName, "w") as z:
    for entry in ["manifest.json"] + [fn for _, _, fn in IMAGES]:
        z.write(os.path.join(relPath, entry), entry)
print("\nWrote %s" % zipName)
print("Release staged in %s/" % relPath)
