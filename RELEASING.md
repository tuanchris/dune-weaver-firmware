# Releasing

Dune Weaver Firmware ships a single ESP32 target — the **`sandtable`** PlatformIO
env (MKS-DLC32, 4M flash). Releases are published as **GitHub Release assets** on
`tuanchris/dune-weaver-firmware`: the flat `.bin` images plus a `manifest.json`
that a web installer consumes.

## Cut a release (automated)

Pushing a **`v*`** tag triggers the **Release** workflow
(`.github/workflows/release.yml`), which runs `build-dw-release.py` on a clean
ubuntu runner and publishes the staged assets as a GitHub Release.

**The release notes are the annotated tag's message body** — write the changelog
into the `git tag -a` annotation (subject line = title, everything after the
blank line = notes). Keep the house style: a one-line summary, bulleted changes
with enough detail to be useful, the torture-gate result, and the install line.

```sh
git tag -a v0.1.11 -F - <<'EOF'
Dune Weaver Firmware v0.1.11

Sand table firmware for MKS-DLC32 (ESP32 4M, `sandtable` build).

- **Headline change.** What it does and why it matters.
- ...

This release passed the 10-minute torture gate (… 0 reboots, 0 alerts).
EOF
git push origin v0.1.11
```

If the tag has only a subject line (no body), the workflow falls back to GitHub's
`--generate-notes` — which for direct-to-`main` commits is just a bare "Full
Changelog" compare link, so **always give the tag a body**. The manual steps
below are the equivalent local flow (useful for a dry run, or if the runner's
bundled mklittlefs ever breaks).

## Cut a release (manual)

1. **Tag the commit** (the firmware bakes its version from `git describe`, so tag
   first):

   ```sh
   git tag -a v0.1.0 -m "Dune Weaver Firmware v0.1.0"
   ```

2. **Build the artifacts** — compiles `sandtable` firmware + the littlefs image,
   then writes flat images, `manifest.json`, and a convenience zip to
   `release/<tag>/`:

   ```sh
   python3 build-dw-release.py        # add -v for full PlatformIO output
   ```

3. **Publish** the tag and the assets:

   ```sh
   git push origin main v0.1.0
   gh release create v0.1.0 \
     --title "v0.1.0" --notes-file <notes> \
     release/v0.1.0/manifest.json \
     release/v0.1.0/bootloader.bin release/v0.1.0/partitions.bin \
     release/v0.1.0/boot_app0.bin release/v0.1.0/firmware.bin \
     release/v0.1.0/littlefs.bin \
     release/v0.1.0/dune-weaver-firmware-v0.1.0-esp32.zip
   ```

## The manifest

`manifest.json` follows the [fluid-installer](https://github.com/breiler/fluid-installer)
schema but is trimmed to one MCU / one variant (`esp32` → `sandtable`) with three
install types: `fresh-install`, `firmware-update`, `filesystem-update`. Each image
`path` is a **bare filename** (not a nested path) so it maps 1:1 onto a GitHub
release asset, whose namespace is flat. The installer's asset base URL is therefore
`…/releases/download/<tag>/`.

Flash offsets (4M ESP32, see `min_littlefs.csv`): bootloader `0x1000`, partitions
`0x8000`, boot_app0 `0xe000`, firmware `0x10000`, littlefs `0x3d0000`.

## Toolchain note (Apple Silicon)

PlatformIO's bundled `tool-mklittlefs` is an x86_64 binary; on an arm64 Mac without
Rosetta, `buildfs` fails with *"Bad CPU type in executable."* Build a native
`mklittlefs` once and drop it over the bundled one:

```sh
git clone --recursive https://github.com/earlephilhower/mklittlefs && cd mklittlefs && make
cp mklittlefs ~/.platformio/packages/tool-mklittlefs/mklittlefs
```
