#!/usr/bin/env python3
"""Check that the ported WLED effects draw the same frames in firmware and app.

Each effect exists twice: C++ in FluidNC/src/WledFx/fx_*.cpp (what the strip
runs) and TypeScript in dune-weaver-mobile/src/lib/wledfx/fx_*.ts (what the
app's LED previews animate). This runs every effect in both through the same
frames -- same seed, clock, speed/intensity, colors, palettes, ring and
rectangle -- and diffs the strip's RGB pixel for pixel.

The C++ side is built for the host with ASan/UBSan, so it doubles as the
effects' memory-safety check (no firmware build needed).

  curl -sL https://raw.githubusercontent.com/wled/WLED/v0.15.3/wled00/FX.cpp -o /tmp/FX.cpp
  python3 tools/fx_parity.py --fx /tmp/FX.cpp [batch ...]   # e.g. fx_ref; default: all
  python3 tools/fx_parity.py --fx /tmp/FX.cpp --leds 400     # a long strip ($LED/Count's max)

Exit 0 = every checked effect matches exactly. Float-heavy effects may drift by
an LSB (C float math vs JS); those are reported, not hidden.
"""

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FXDIR = ROOT / "FluidNC/src/WledFx"
N_LEDS = 49
FRAMES = 90
# (speed, intensity, color1, color2, palette id, ring)
SCENARIOS = [
    (128, 128, 0xFFB060, 0x0040FF, 0, True),
    (220, 40, 0xFF0000, 0x000000, 1, True),
    (25, 235, 0x20FF80, 0x302000, 21, False),
]


def wled_meta(fx_cpp):
    """mode function name -> parsed WLED metadata."""
    s = Path(fx_cpp).read_text(encoding="utf-8", errors="replace")
    data = dict(re.findall(r'static const char (_data_FX_MODE_\w+)\[\] PROGMEM = "([^"]*)";', s))
    out = {}
    for fx, fn, d in re.findall(r"addEffect\((FX_MODE_\w+),\s*&(\w+),\s*(_data_FX_MODE_\w+)\)", s):
        out[fn] = parse_meta(data.get(d, ""))
    return out


def parse_meta(md):
    parts = md.split(";")
    head = parts[0].split("@")
    name = head[0]
    sliders = head[1].split(",") if len(head) > 1 else []
    flags = parts[3] if len(parts) > 3 else ""
    defaults = {}
    if len(parts) > 4:
        for kv in parts[4].split(","):
            if "=" in kv:
                k, v = kv.split("=", 1)
                try:
                    defaults[k.strip()] = int(v)
                except ValueError:
                    pass
    return {
        "name": name,
        "sliders": sliders,
        "colors": parts[1] if len(parts) > 1 else None,
        "palette": parts[2] if len(parts) > 2 else None,
        "flags": flags,
        "is2D": "2" in flags and "1" not in flags,
        "defaults": defaults,
        "raw": md,
    }


def batch_modes(cpp, ts):
    # registered effects are the non-static `uint16_t x(void)` / exported `x()` (helpers are static / unexported)
    c = re.findall(r"^uint16_t (\w+)\((?:void)?\)", cpp.read_text(), re.M)
    t = re.findall(r"^export function (\w+)\(\): number", ts.read_text(), re.M) if ts.exists() else []
    return c, t


def info(meta, fn):
    m = meta.get(fn)
    if m is None:
        raise SystemExit(f"{fn}: not registered in WLED's FX.cpp (wrong name?)")
    d = m["defaults"]
    checks = (1 if d.get("o1") else 0) | (2 if d.get("o2") else 0) | (4 if d.get("o3") else 0)
    return {"fn": fn, "is2D": m["is2D"], "c1": d.get("c1", 128), "c2": d.get("c2", 128), "c3": d.get("c3", 16), "checks": checks}


def cpp_main(effects):
    decl = "\n".join(f"uint16_t {e['fn']}();" for e in effects)
    rows = ",\n".join(
        f'    {{"{e["fn"]}", {e["fn"]}, {str(e["is2D"]).lower()}, {e["c1"]}, {e["c2"]}, {e["c3"]}, {e["checks"]}}}' for e in effects
    )
    scen = ",\n".join(f"    {{{sp}, {ix}, 0x{c1:06X}u, 0x{c2:06X}u, {pal}, {str(ring).lower()}}}" for sp, ix, c1, c2, pal, ring in SCENARIOS)
    return f"""#include "WledFxRunner.h"
#include <cstdio>
namespace wled {{
{decl}
}}
using namespace wled;
struct Sc {{ uint8_t sp, ix; uint32_t c1, c2; uint8_t pal; bool ring; }};
static const FxInfo fx[] = {{
{rows}
}};
static const Sc sc[] = {{
{scen}
}};
int main() {{
    static uint8_t rgb[{N_LEDS} * 3];
    for (const FxInfo& f : fx) {{
        for (unsigned s = 0; s < sizeof(sc) / sizeof(sc[0]); s++) {{
            Runner r;
            seed(4242 + s);
            for (int k = 0; k < {FRAMES}; k++) {{
                FxFrame fr{{sc[s].sp, sc[s].ix, sc[s].c1, sc[s].c2, sc[s].pal, 1000u + 33u * k, sc[s].ring}};
                for (auto& b : rgb) b = 0;
                r.render(f, fr, rgb, {N_LEDS});
                printf("%s %u %d ", f.name, s, k);
                for (int i = 0; i < {N_LEDS} * 3; i++) printf("%02x", rgb[i]);
                printf("\\n");
            }}
        }}
    }}
    return 0;
}}
"""


def ts_main(effects, batches):
    imports = "\n".join(f"import * as {b} from './lib/wledfx/{b}'" for b in batches)
    table = ",\n".join(
        f"  {{ name: '{e['fn']}', fn: ({e['batch']} as any).{e['fn']}, is2D: {str(e['is2D']).lower()}, c1: {e['c1']}, c2: {e['c2']}, c3: {e['c3']}, checks: {e['checks']} }}"
        for e in effects
    )
    scen = ",\n".join(f"  [{sp}, {ix}, 0x{c1:06x}, 0x{c2:06x}, {pal}, {str(ring).lower()}]" for sp, ix, c1, c2, pal, ring in SCENARIOS)
    return f"""import {{ Runner, seed, type FxInfo }} from './lib/wledfx/runner'
{imports}
const fx: FxInfo[] = [
{table}
]
const sc: Array<[number, number, number, number, number, boolean]> = [
{scen}
]
const out: string[] = []
const rgb = new Uint8Array({N_LEDS} * 3)
for (const f of fx) {{
  for (let s = 0; s < sc.length; s++) {{
    const r = new Runner()
    seed(4242 + s)
    for (let k = 0; k < {FRAMES}; k++) {{
      rgb.fill(0)
      r.render(f, {{ speed: sc[s][0], intensity: sc[s][1], color1: sc[s][2], color2: sc[s][3], palette: sc[s][4], now: 1000 + 33 * k, ring: sc[s][5] }}, rgb, {N_LEDS})
      out.push(`${{f.name}} ${{s}} ${{k}} ` + Array.from(rgb, (b) => b.toString(16).padStart(2, '0')).join(''))
    }}
  }}
}}
process.stdout.write(out.join('\\n') + '\\n')
"""


def transpile(app, work, files):
    ts = app / "node_modules/typescript"
    js = f"""
const ts = require({json.dumps(str(ts))}), fs = require('fs'), path = require('path')
for (const [src, dst] of {json.dumps(files)}) {{
  const out = ts.transpileModule(fs.readFileSync(src, 'utf8'), {{ compilerOptions: {{ module: ts.ModuleKind.CommonJS, target: ts.ScriptTarget.ES2020 }}, reportDiagnostics: true }})
  fs.mkdirSync(path.dirname(dst), {{ recursive: true }})
  fs.writeFileSync(dst, out.outputText)
}}
"""
    subprocess.run(["node", "-e", js], check=True)


def main():
    global N_LEDS
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--fx", required=True, help="WLED v0.15.3 wled00/FX.cpp (for each effect's metadata)")
    ap.add_argument("--app", default=str(ROOT.parent / "dune-weaver-mobile"))
    ap.add_argument("--keep", action="store_true", help="keep the work dir")
    ap.add_argument("--leds", type=int, default=N_LEDS, help=f"strip length (default {N_LEDS}; past 255 catches 8-bit index math)")
    ap.add_argument("batches", nargs="*", help="fx_* batch names (default: every fx_*.cpp)")
    a = ap.parse_args()
    N_LEDS = a.leds
    app = Path(a.app).resolve()
    meta = wled_meta(a.fx)
    batches = a.batches or sorted(p.stem for p in FXDIR.glob("fx_*.cpp"))

    effects, problems = [], []
    for b in batches:
        cpp, ts = FXDIR / f"{b}.cpp", app / f"src/lib/wledfx/{b}.ts"
        c, t = batch_modes(cpp, ts)
        for fn in sorted(set(c) ^ set(t)):
            problems.append(f"{b}: {fn} is only in the {'C++' if fn in c else 'TS'} file")
        for fn in c:
            if fn in t:
                e = info(meta, fn)
                e["batch"] = b
                effects.append(e)
    if not effects:
        raise SystemExit("no effects found")

    work = Path(tempfile.mkdtemp(prefix="fxparity-"))
    try:
        (work / "main.cpp").write_text(cpp_main(effects))
        srcs = [str(FXDIR / "WledFx.cpp"), str(FXDIR / "WledFxRunner.cpp")] + [str(FXDIR / f"{b}.cpp") for b in batches]
        cxx = os.environ.get("CXX", "c++")
        build = [cxx, "-std=c++17", "-O1", "-g", "-ffp-contract=off", "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-w",
                 f"-I{FXDIR}", str(work / "main.cpp"), *srcs, "-o", str(work / "fxdump")]
        subprocess.run(build, check=True)
        crun = subprocess.run([str(work / "fxdump")], capture_output=True, text=True)
        if crun.returncode != 0 or "runtime error" in crun.stderr:
            print(crun.stderr[-4000:], file=sys.stderr)
            problems.append("C++ side: sanitizer/runtime error (above)")

        files = [(str(app / "src/lib/ledPalettes.ts"), str(work / "js/lib/ledPalettes.js")),
                 (str(app / "src/lib/wledfx/shim.ts"), str(work / "js/lib/wledfx/shim.js")),
                 (str(app / "src/lib/wledfx/runner.ts"), str(work / "js/lib/wledfx/runner.js"))]
        files += [(str(app / f"src/lib/wledfx/{b}.ts"), str(work / f"js/lib/wledfx/{b}.js")) for b in batches]
        (work / "main.ts").write_text(ts_main(effects, batches))
        files.append((str(work / "main.ts"), str(work / "js/main.js")))
        transpile(app, work, files)
        trun = subprocess.run(["node", str(work / "js/main.js")], capture_output=True, text=True)
        if trun.returncode != 0:
            print(trun.stderr[-4000:], file=sys.stderr)
            problems.append("TS side: crashed (above)")

        def frames(text):
            d = {}
            for line in text.splitlines():
                name, s, k, hexs = line.split(" ")
                d[(name, int(s), int(k))] = bytes.fromhex(hexs)
            return d

        cf, tf = frames(crun.stdout), frames(trun.stdout)
        ok = 0
        for e in effects:
            worst, first = 0, None
            for s in range(len(SCENARIOS)):
                for k in range(FRAMES):
                    a_, b_ = cf.get((e["fn"], s, k)), tf.get((e["fn"], s, k))
                    if a_ is None or b_ is None:
                        first = first or (s, k, "missing frame")
                        continue
                    if a_ != b_:
                        diff = [(i, a_[i], b_[i]) for i in range(len(a_)) if a_[i] != b_[i]]
                        worst = max(worst, max(abs(x - y) for _, x, y in diff))
                        if first is None:
                            i, x, y = diff[0]
                            first = (s, k, f"LED {i // 3} ch {i % 3}: C++ {x} vs TS {y} ({len(diff)} bytes differ)")
            lit = sum(1 for s in range(len(SCENARIOS)) for k in range(FRAMES) if any(cf.get((e["fn"], s, k), b"")))
            if first is None:
                ok += 1
                print(f"  ok    {e['fn']}" + ("" if lit else "   (never lit: check it draws)"))
            else:
                s, k, msg = first
                print(f"  DIFF  {e['fn']}: scenario {s} frame {k}: {msg}; max diff {worst}")
                problems.append(f"{e['fn']} differs")
        print(f"{ok}/{len(effects)} effects match")
        for p in problems:
            print("  !", p)
        return 0 if not problems else 1
    finally:
        if a.keep:
            print("work dir:", work)
        else:
            shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
