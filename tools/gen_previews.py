#!/usr/bin/env python3
"""Generate Dune-Weaver-style pattern previews + a manifest for the sand table.

Point it at a patterns folder; it walks the tree, renders a preview image for
every .thr file (matching the Dune Weaver renderer: black path on a transparent
background, 2048px render downscaled to a WEBP, rotated 180), and writes:

  <out>/index.json                 - JSON array of .thr paths relative to the
                                     patterns root (exactly what the firmware's
                                     GET /sand_patterns serves as /patterns/index.json)
  <out>/cached_images/<rel>.thr.webp   - one preview per pattern (same naming/layout
                                     as Dune Weaver, so it drops onto the SD /patterns)
  <out>/previews.zip               - zip of cached_images/ (use --shards N to split)

Run:  python3 tools/gen_previews.py /path/to/patterns [--out DIR] [options]
Needs Pillow with WEBP support:  pip install Pillow
"""

import argparse
import json
import math
import os
import sys
import zipfile
from concurrent.futures import ProcessPoolExecutor, as_completed

# ---- rendering (mirrors dune-weaver modules/core/preview.py) ----------------

RENDER_SIZE = 2048   # high-quality render size
DISPLAY_SIZE = 512   # final preview size
STROKE_WIDTH = 2
LINE_COLOR = "black"


def parse_thr(path):
    """Return [(theta, rho), ...]; skips blank lines and '#' comments."""
    coords = []
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split()
            if len(parts) != 2:
                continue
            try:
                coords.append((float(parts[0]), float(parts[1])))
            except ValueError:
                continue
    return coords


def render_preview(coords, render_size, display_size):
    """Render coords to a PIL RGBA preview image (transparent background)."""
    from PIL import Image, ImageDraw

    img = Image.new("RGBA", (render_size, render_size), (255, 255, 255, 0))
    draw = ImageDraw.Draw(img)
    if not coords:
        return img.resize((display_size, display_size), Image.Resampling.LANCZOS)

    center = render_size / 2.0
    scale = (render_size / 2.0) - 10.0
    pts = [
        (center - rho * scale * math.cos(theta), center - rho * scale * math.sin(theta))
        for theta, rho in coords
    ]
    if len(pts) > 1:
        draw.line(pts, fill=LINE_COLOR, width=STROKE_WIDTH, joint="curve")
    elif len(pts) == 1:
        x, y = pts[0]
        draw.ellipse([(x - 4, y - 4), (x + 4, y + 4)], fill=LINE_COLOR)

    img = img.resize((display_size, display_size), Image.Resampling.LANCZOS)
    return img.rotate(180)


# ---- per-file worker (runs in a subprocess) --------------------------------

def _worker(job):
    rel, src, dst, render_size, display_size, quality = job
    try:
        if os.path.dirname(dst):
            os.makedirs(os.path.dirname(dst), exist_ok=True)
        img = render_preview(parse_thr(src), render_size, display_size)
        img.save(dst, "WEBP", lossless=False, quality=quality, alpha_quality=20, method=4)
        return (rel, True, None)
    except Exception as e:  # keep going; report at the end
        return (rel, False, str(e))


# ---- tree walk -------------------------------------------------------------

def find_thr(root):
    """Relative .thr paths under root, skipping cached_images and dotfiles."""
    out = []
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames if d != "cached_images" and not d.startswith(".")]
        for fn in filenames:
            if fn.lower().endswith(".thr") and not fn.startswith("."):
                rel = os.path.relpath(os.path.join(dirpath, fn), root)
                out.append(rel.replace(os.sep, "/"))
    out.sort(key=str.lower)
    return out


def preview_rel(thr_rel):
    """cached_images/<dir>/<name>.thr.webp  (Dune Weaver naming)."""
    return f"cached_images/{thr_rel}.webp"


def make_zip(cache_root, files, out_dir, shards):
    """Zip the preview files (paths relative to out_dir) into 1 or N shards."""
    paths = [p for p in files if os.path.exists(os.path.join(out_dir, p))]
    written = []
    shards = max(1, shards)
    per = (len(paths) + shards - 1) // shards if paths else 0
    for s in range(shards):
        chunk = paths[s * per:(s + 1) * per] if per else []
        if not chunk and s > 0:
            continue
        name = "previews.zip" if shards == 1 else f"previews_{s + 1}of{shards}.zip"
        zpath = os.path.join(out_dir, name)
        with zipfile.ZipFile(zpath, "w", zipfile.ZIP_DEFLATED) as z:
            for p in chunk:
                z.write(os.path.join(out_dir, p), p)
        written.append((name, len(chunk), os.path.getsize(zpath)))
    return written


def main():
    ap = argparse.ArgumentParser(description="Generate sand-table pattern previews + manifest.")
    ap.add_argument("patterns", help="patterns root folder (walked recursively)")
    ap.add_argument("--out", default=None, help="output dir (default: <patterns>/_export)")
    ap.add_argument("--render-size", type=int, default=RENDER_SIZE)
    ap.add_argument("--display-size", type=int, default=DISPLAY_SIZE)
    ap.add_argument("--quality", type=int, default=80, help="WEBP quality 0..100")
    ap.add_argument("--jobs", type=int, default=os.cpu_count(), help="parallel workers")
    ap.add_argument("--shards", type=int, default=1, help="split previews.zip into N files")
    ap.add_argument("--force", action="store_true", help="regenerate previews that already exist")
    ap.add_argument("--no-zip", action="store_true", help="skip building the zip(s)")
    args = ap.parse_args()

    root = os.path.abspath(args.patterns)
    if not os.path.isdir(root):
        sys.exit(f"not a folder: {root}")
    try:
        import PIL  # noqa: F401
    except ImportError:
        sys.exit("Pillow is required:  pip install Pillow")

    out_dir = os.path.abspath(args.out) if args.out else os.path.join(root, "_export")
    os.makedirs(out_dir, exist_ok=True)

    thr = find_thr(root)
    print(f"found {len(thr)} .thr files under {root}")

    # manifest (firmware /sand_patterns format: array of relative .thr paths)
    index_path = os.path.join(out_dir, "index.json")
    with open(index_path, "w", encoding="utf-8") as f:
        json.dump(thr, f, ensure_ascii=False, separators=(",", ":"))
    print(f"wrote {index_path} ({len(thr)} entries)")

    # build the preview render jobs
    previews = [preview_rel(t) for t in thr]
    jobs = []
    for t, pv in zip(thr, previews):
        dst = os.path.join(out_dir, pv)
        if not args.force and os.path.exists(dst):
            continue
        jobs.append((t, os.path.join(root, t), dst, args.render_size, args.display_size, args.quality))

    skipped = len(thr) - len(jobs)
    print(f"rendering {len(jobs)} previews ({skipped} up-to-date) with {args.jobs} workers...")
    ok = fail = 0
    failures = []
    with ProcessPoolExecutor(max_workers=args.jobs) as ex:
        futs = [ex.submit(_worker, j) for j in jobs]
        for i, fut in enumerate(as_completed(futs), 1):
            rel, good, err = fut.result()
            if good:
                ok += 1
            else:
                fail += 1
                failures.append((rel, err))
            if i % 100 == 0 or i == len(futs):
                print(f"  {i}/{len(futs)}  ok={ok} fail={fail}", flush=True)

    if failures:
        print(f"\n{len(failures)} failed (first 10):")
        for rel, err in failures[:10]:
            print(f"  {rel}: {err}")

    if not args.no_zip:
        for name, n, size in make_zip(os.path.join(out_dir, "cached_images"), previews, out_dir, args.shards):
            print(f"wrote {os.path.join(out_dir, name)}  ({n} previews, {size/1e6:.1f} MB)")

    print("\ndone.")
    print(f"  manifest : {index_path}")
    print(f"  previews : {os.path.join(out_dir, 'cached_images')}/  (<pattern>.thr.webp)")
    print("  upload index.json to the card as /patterns/index.json for GET /sand_patterns.")


if __name__ == "__main__":
    main()
