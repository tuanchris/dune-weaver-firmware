#!/usr/bin/env python3

# Build a Dune Weaver Firmware release for every shipping board target.
# Produces flat .bin images + a manifest.json whose image `path` fields are bare
# filenames, so they map 1:1 onto GitHub release assets (which live in a flat
# namespace). Also bundles each target into its own .zip.
#
#   python3 build-dw-release.py          # build + assemble release/<tag>/
#   python3 build-dw-release.py -v       # verbose pio output
#
# The release tag is read from `git describe`; tag the commit (e.g. `git tag
# v0.1.0`) before running so the firmware bakes the matching version string.
#
# Adding a board: append to TARGETS below. Filenames are MCU-prefixed because
# the release namespace is flat and every env emits the same `firmware.bin`.

import hashlib
import json
import os
import shutil
import subprocess
import sys
from zipfile import ZipFile

VERBOSE = "-v" in sys.argv
PIO = shutil.which("pio") or shutil.which("platformio") or "/opt/homebrew/bin/pio"

REPO = "https://github.com/tuanchris/dune-weaver-firmware"

# One entry per board we ship. `images` are the offsets esptool writes each
# image to during a fresh install, and they are NOT shared between MCUs: the
# ESP32-S3 boots its bootloader from 0x0 rather than 0x1000, and its 8MB
# partition table puts the filesystem somewhere else entirely. Getting this
# wrong produces a board that flashes cleanly and then will not boot.
TARGETS = [
    {
        "env": "sandtable",
        "mcu": "esp32",
        "description": "ESP32-WROOM (MKS DLC32)",
        # 4M flash layout — see min_littlefs.csv.
        "images": [
            # name,          offset,     source filename
            ("bootloader", "0x1000", "bootloader.bin"),
            ("partitions", "0x8000", "partitions.bin"),
            ("bootapp", "0xe000", "boot_app0.bin"),
            ("firmware", "0x10000", "firmware.bin"),
            ("filesystem", "0x3d0000", "littlefs.bin"),
        ],
    },
    {
        "env": "sandtable_s3",
        "mcu": "esp32s3",
        "description": "ESP32-S3 (MKS DLC32 MAX)",
        # 8M flash layout — see sandtable_8MB.csv. The S3 bootloader lives at
        # 0x0; there is no 0x1000 offset on this chip.
        "images": [
            ("bootloader", "0x0", "bootloader.bin"),
            ("partitions", "0x8000", "partitions.bin"),
            ("bootapp", "0xe000", "boot_app0.bin"),
            ("firmware", "0x10000", "firmware.bin"),
            ("filesystem", "0x610000", "littlefs.bin"),
        ],
    },
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


# CI exports the tag that triggered the release as DW_RELEASE_TAG. `git describe`
# tie-breaks arbitrarily when a commit carries more than one tag -- cutting a
# final release at the same commit as its last RC staged the whole release under
# the RC's name -- so an explicit tag always wins. Unset (a local build) keeps
# the original describe behaviour.
tag = os.environ.get("DW_RELEASE_TAG", "").strip() or git("describe", "--tags", "--abbrev=0")
try:
    if git("rev-parse", tag + "^{commit}") != git("rev-parse", "HEAD"):
        raise subprocess.CalledProcessError(1, "rev-parse")
except subprocess.CalledProcessError:
    print("WARNING: HEAD is not exactly on tag %s; firmware version string will\n"
          "         include a branch-commit suffix. Tag this commit first to ship\n"
          "         a clean release." % tag)

envs = ", ".join(t["env"] for t in TARGETS)
print("Building Dune Weaver Firmware release %s (envs: %s)\n" % (tag, envs))

relPath = os.path.join("release", tag)
if os.path.exists(relPath):
    shutil.rmtree(relPath)
os.makedirs(relPath)

bootapp_src = os.path.join(
    os.path.expanduser("~"), ".platformio", "packages",
    "framework-arduinoespressif32", "tools", "partitions", "boot_app0.bin")

# Per-MCU staged filenames, so the zip and manifest builders agree.
staged = {}


def install_types(mcu):
    """The three install flavours offered for one MCU."""
    fresh = [mcu + "-" + n for n in
             ("bootloader", "partitions", "bootapp", "firmware", "filesystem")]
    return [
        {"name": "fresh-install",
         "description": "Complete install, erasing all previous data. Upload your table's config.yaml afterward.",
         "erase": True, "images": fresh},
        {"name": "firmware-update",
         "description": "Update firmware only, preserving the filesystem (config.yaml, patterns).",
         # Also rewrite otadata (boot_app0.bin @ 0xe000) so the bootloader
         # selects ota_0/app0, where the firmware below is written. The
         # partition table is dual-slot OTA; a board that previously took
         # an OTA update (FluidNC OTA.cpp writes the inactive slot) has
         # otadata pointing at app1, so a firmware-only write to app0 would
         # otherwise leave it booting the stale slot.
         "erase": False, "images": [mcu + "-bootapp", mcu + "-firmware"]},
        {"name": "filesystem-update",
         "description": "Replace the filesystem only, erasing previous filesystem data.",
         "erase": False, "images": [mcu + "-filesystem"]},
    ]


def build_manifest(targets):
    """A manifest describing exactly `targets` — the whole release, or one zip."""
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
    for target in targets:
        mcu = target["mcu"]
        for name, offset, _ in target["images"]:
            key = mcu + "-" + name
            path = staged[key]
            full = os.path.join(relPath, path)
            manifest["images"][key] = {
                "size": os.path.getsize(full),
                "offset": offset,
                "path": path,  # flat: matches GitHub release asset name
                "signature": {"algorithm": "SHA2-256", "value": sha256(full)},
            }
        manifest["installable"]["choices"].append({
            "name": mcu,
            "description": target["description"],
            "choice-name": "Installation type",
            "choices": [{
                "name": "sandtable",
                "description": "Dune Weaver sand table (WiFi + HTTP/JSON API)",
                "choice-name": "Installation type",
                "choices": install_types(mcu),
            }],
        })
    return manifest


# 1. Build firmware + filesystem image for every target and stage flat,
#    MCU-prefixed images under release/<tag>/.
for target in TARGETS:
    env, mcu = target["env"], target["mcu"]
    print("\n== %s (%s) ==" % (env, mcu))
    run([PIO, "run", "--disable-auto-clean", "-e", env])
    run([PIO, "run", "--disable-auto-clean", "-e", env, "-t", "buildfs"])

    buildDir = os.path.join(".pio", "build", env)
    shutil.copy(bootapp_src, os.path.join(buildDir, "boot_app0.bin"))

    for name, offset, filename in target["images"]:
        src = os.path.join(buildDir, filename)
        if not os.path.isfile(src):
            sys.exit("Missing build artifact: %s" % src)
        dstName = mcu + "-" + filename
        shutil.copy(src, os.path.join(relPath, dstName))
        staged[mcu + "-" + name] = dstName
        print("  image %-12s %8d bytes @ %-9s -> %s"
              % (name, os.path.getsize(src), offset, dstName))

# 2. One manifest covering every target — this is what the installer reads.
with open(os.path.join(relPath, "manifest.json"), "w") as f:
    json.dump(build_manifest(TARGETS), f, indent=2)
print("\n  manifest.json (%d processors)" % len(TARGETS))

# 3. A convenience bundle per target: that board's images plus a manifest
#    describing only them, so an offline flash from the zip cannot reference a
#    file the zip does not contain.
for target in TARGETS:
    mcu = target["mcu"]
    zipName = os.path.join(
        relPath, "dune-weaver-firmware-%s-%s.zip" % (tag, mcu))
    with ZipFile(zipName, "w") as z:
        z.writestr("manifest.json",
                   json.dumps(build_manifest([target]), indent=2))
        for name, _, _ in target["images"]:
            entry = staged[mcu + "-" + name]
            z.write(os.path.join(relPath, entry), entry)
    print("  %s" % os.path.basename(zipName))

print("\nRelease staged in %s/" % relPath)
