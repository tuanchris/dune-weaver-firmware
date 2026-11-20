#!/usr/bin/env python3
"""Generate the $LED/Palette tables for the firmware AND the mobile app.

One source for both, because the app animates its LED previews with the same
tables (dune-weaver-mobile/src/lib/ledSim.ts): a palette that exists on one
side only, or differs by a byte, shows the user colors the table never draws.

Palettes: our original eight (ids 0..7, persisted in NVS, so ids never move),
then WLED's: FastLED's Rainbow + Rainbow Bands, and the 58 cpt-city gradients
in WLED's own order. Gradients are flattened to 16 entries exactly as WLED
loads them (FastLED's CRGBPalette16 = gradient), so sampleTable() in Leds.cpp
draws them the way WLED does.

WLED v0.15 data, not 0.16: 0.16 re-stored the cpt-city colors for a global
gamma 2.2 pass at output. 0.15 kept them pre-gamma'd for direct output, and
the firmware has no gamma pass.

Run (from the firmware repo root):
  curl -sL https://raw.githubusercontent.com/wled/WLED/v0.15.3/wled00/palettes.h -o /tmp/palettes.h
  python3 tools/gen_led_palettes.py /tmp/palettes.h [--app ../dune-weaver-mobile]
Writes FluidNC/src/LedPalettes.h and <app>/src/lib/ledPalettes.ts.
"""

import argparse
import re
from pathlib import Path

# Ids 0..7: the firmware's original palettes, verbatim (palParty keeps its
# historical last entry). Id 0 has no table: each effect's own colors.
ORIGINAL = [
    ("default", "Default", None),
    ("ocean", "Ocean", [0x191970, 0x00008B, 0x191970, 0x000080, 0x00008B, 0x0000CD, 0x2E8B57, 0x008080,
                        0x5F9EA0, 0x0000FF, 0x008B8B, 0x6495ED, 0x7FFFD4, 0x2E8B57, 0x00FFFF, 0x87CEFA]),
    ("lava", "Lava", [0x000000, 0x800000, 0x000000, 0x800000, 0x8B0000, 0x800000, 0x8B0000, 0x8B0000,
                      0x8B0000, 0x8B0000, 0xFF0000, 0xFFA500, 0xFFFFFF, 0xFFA500, 0xFF0000, 0x8B0000]),
    ("forest", "Forest", [0x006400, 0x006400, 0x556B2F, 0x006400, 0x008000, 0x228B22, 0x6B8E23, 0x008000,
                          0x2E8B57, 0x66CDAA, 0x32CD32, 0x9ACD32, 0x90EE90, 0x7CFC00, 0x66CDAA, 0x228B22]),
    ("party", "Party", [0x5500AB, 0x84007C, 0xB5004B, 0xE5001B, 0xE81700, 0xB84700, 0xAB7700, 0xABAB00,
                        0xAB5500, 0xDD2200, 0xF2000E, 0xC2003E, 0x8F0071, 0x5F00A1, 0x2F00D0, 0x0700E9]),
    ("cloud", "Cloud", [0x0000FF, 0x00008B, 0x00008B, 0x00008B, 0x00008B, 0x00008B, 0x00008B, 0x00008B,
                        0x0000FF, 0x00008B, 0x87CEEB, 0x87CEEB, 0xADD8E6, 0xFFFFFF, 0xADD8E6, 0x87CEEB]),
    ("heat", "Heat", [0x000000, 0x330000, 0x660000, 0x990000, 0xCC0000, 0xFF0000, 0xFF3300, 0xFF6600,
                      0xFF9900, 0xFFCC00, 0xFFFF00, 0xFFFF33, 0xFFFF66, 0xFFFF99, 0xFFFFCC, 0xFFFFFF]),
    ("sunset", "Sunset", [0x00008B, 0x4B0082, 0x800080, 0xC71585, 0xFF1493, 0xFF4500, 0xFF8C00, 0xFFD700,
                          0xFFA500, 0xFF4500, 0xC71585, 0x800080, 0x4B0082, 0x191970, 0x00008B, 0x000033]),
]

# FastLED 3.6.0 colorpalettes.cpp (WLED 11 "Rainbow", 12 "Rainbow Bands").
# "rainbow" is taken: it stays the old name of id 0, which old apps still send.
FASTLED = [
    ("spectrum", "Rainbow", [0xFF0000, 0xD52A00, 0xAB5500, 0xAB7F00, 0xABAB00, 0x56D500, 0x00FF00, 0x00D52A,
                             0x00AB55, 0x0056AA, 0x0000FF, 0x2A00D5, 0x5500AB, 0x7F0081, 0xAB0055, 0xD5002B]),
    ("rainbow_bands", "Rainbow Bands", [0xFF0000, 0, 0xAB5500, 0, 0xABAB00, 0, 0x00FF00, 0,
                                        0x00AB55, 0, 0x0000FF, 0, 0x5500AB, 0, 0xAB0055, 0]),
]

# WLED's gGradientPalettes order (palettes 13..70) -> (array, label). Keys are
# the label lowercased, "&" dropped, spaces -> "_". WLED's "Sunset" gets
# sunset_real: our id 7 already owns "sunset".
WLED_GRADIENTS = [
    ("Sunset_Real_gp", "Sunset Real"), ("es_rivendell_15_gp", "Rivendell"), ("es_ocean_breeze_036_gp", "Breeze"),
    ("rgi_15_gp", "Red & Blue"), ("retro2_16_gp", "Yellowout"), ("Analogous_1_gp", "Analogous"),
    ("es_pinksplash_08_gp", "Splash"), ("Sunset_Yellow_gp", "Pastel"), ("Another_Sunset_gp", "Sunset 2"),
    ("Beech_gp", "Beech"), ("es_vintage_01_gp", "Vintage"), ("departure_gp", "Departure"),
    ("es_landscape_64_gp", "Landscape"), ("es_landscape_33_gp", "Beach"), ("rainbowsherbet_gp", "Sherbet"),
    ("gr65_hult_gp", "Hult"), ("gr64_hult_gp", "Hult 64"), ("GMT_drywet_gp", "Drywet"), ("ib_jul01_gp", "Jul"),
    ("es_vintage_57_gp", "Grintage"), ("ib15_gp", "Rewhi"), ("Tertiary_01_gp", "Tertiary"), ("lava_gp", "Fire"),
    ("fierce_ice_gp", "Icefire"), ("Colorfull_gp", "Cyane"), ("Pink_Purple_gp", "Light Pink"),
    ("es_autumn_19_gp", "Autumn"), ("BlacK_Blue_Magenta_White_gp", "Magenta"), ("BlacK_Magenta_Red_gp", "Magred"),
    ("BlacK_Red_Magenta_Yellow_gp", "Yelmag"), ("Blue_Cyan_Yellow_gp", "Yelblu"), ("Orange_Teal_gp", "Orange & Teal"),
    ("Tiamat_gp", "Tiamat"), ("April_Night_gp", "April Night"), ("Orangery_gp", "Orangery"), ("C9_gp", "C9"),
    ("Sakura_gp", "Sakura"), ("Aurora_gp", "Aurora"), ("Atlantica_gp", "Atlantica"), ("C9_2_gp", "C9 2"),
    ("C9_new_gp", "C9 New"), ("temperature_gp", "Temperature"), ("Aurora2_gp", "Aurora 2"),
    ("retro_clown_gp", "Retro Clown"), ("candy_gp", "Candy"), ("toxy_reaf_gp", "Toxy Reaf"),
    ("fairy_reaf_gp", "Fairy Reaf"), ("semi_blue_gp", "Semi Blue"), ("pink_candy_gp", "Pink Candy"),
    ("red_reaf_gp", "Red Reaf"), ("aqua_flash_gp", "Aqua Flash"), ("yelblu_hot_gp", "Yelblu Hot"),
    ("lite_light_gp", "Lite Light"), ("red_flash_gp", "Red Flash"), ("blink_red_gp", "Blink Red"),
    ("red_shift_gp", "Red Shift"), ("red_tide_gp", "Red Tide"), ("candy2_gp", "Candy2"),
]


def key_of(label):
    return re.sub(r"\s+", "_", label.lower().replace("&", "").strip()).replace("__", "_")


def parse_gradients(text):
    text = re.sub(r"//[^\n]*|/\*.*?\*/", "", text, flags=re.S)
    out = {}
    for name, body in re.findall(r"(\w+_gp)\[\]\s*PROGMEM\s*=\s*\{(.*?)\}", text, re.S):
        out[name] = [int(v, 0) for v in re.findall(r"0x[0-9a-fA-F]+|\d+", body)]
    return out


def fill_gradient_rgb(e, start, c0, end, c1):
    """FastLED fill_gradient_RGB (8.7 fixed point, 16-bit wrapping accumulators)."""
    if end < start:
        start, end, c0, c1 = end, start, c1, c0
    div = (end - start) or 1
    delta = [int(((b - a) << 7) / div) * 2 for a, b in zip(c0, c1)]  # C truncates toward 0
    acc = [a << 8 for a in c0]
    for i in range(start, end + 1):
        e[i] = tuple((v & 0xFFFF) >> 8 for v in acc)
        acc = [(v + d) & 0xFFFF for v, d in zip(acc, delta)]


def to_palette16(gp):
    """FastLED CRGBPalette16::operator=(TProgmemRGBGradientPalette_bytes)."""
    ents = [tuple(gp[i:i + 4]) for i in range(0, len(gp), 4)]
    count = next(i for i, u in enumerate(ents) if u[0] == 255) + 1
    e = [(0, 0, 0)] * 16
    last = -1
    rgb0, index0, k = ents[0][1:], 0, 0
    while index0 < 255:
        k += 1
        index1, rgb1 = ents[k][0], ents[k][1:]
        s, t = index0 // 16, index1 // 16
        if count < 16:
            if s <= last < 15:
                s = last + 1
                t = max(t, s)
            last = t
        fill_gradient_rgb(e, s, rgb0, t, rgb1)
        index0, rgb0 = index1, rgb1
    return [(r << 16) | (g << 8) | b for r, g, b in e]


def build(palettes_h):
    grads = parse_gradients(palettes_h)
    pals = ORIGINAL + FASTLED
    for arr, label in WLED_GRADIENTS:
        pals.append((key_of(label), label, to_palette16(grads[arr])))
    return pals


def self_check(pals):
    # A black->white two-stop gradient spreads evenly: entry i = i * 17.
    assert to_palette16([0, 0, 0, 0, 255, 255, 255, 255]) == [(i * 17) * 0x010101 for i in range(16)]
    names = [p[0] for p in pals]
    assert names[:8] == ["default", "ocean", "lava", "forest", "party", "cloud", "heat", "sunset"], "ids 0..7 moved"
    assert len(set(names)) == len(names) and "rainbow" not in names, "duplicate palette key"
    assert len(pals) <= 127, "EnumSetting ids are int8_t"
    assert all(re.fullmatch(r"[a-z0-9_]+", n) for n in names)
    assert all(t is None or len(t) == 16 for _, _, t in pals[1:]) and pals[0][2] is None


def hexrow(t, fmt):
    return ", ".join(fmt % v for v in t)


def emit_cpp(pals):
    lines = [
        "// GENERATED by tools/gen_led_palettes.py from WLED v0.15.3 + FastLED 3.6.0.",
        "// Edit the script, not this file; it also writes the app's ledPalettes.ts.",
        "//",
        "// $LED/Palette tables: 16 packed-RGB entries each, linearly blended by",
        "// Leds::sampleTable() (WLED's CRGBPalette16). Ids are persisted in NVS:",
        "// append only, never reorder. Std-only, so [env:tests] can include it.",
        "#pragma once",
        "",
        "#include <cstdint>",
        "#include <string_view>",
        "",
        "namespace LedPalettes {",
        f"    constexpr int kCount = {len(pals)};",
        "",
        "    // Id 0 (\"default\") has no table: each effect keeps its own colors,",
        "    // the hue wheel for the hue-cycling ones. kTables[id - 1] backs id >= 1.",
        "    constexpr const char* kNames[kCount] = {",
    ]
    lines += [f'        "{n}",' for n, _, _ in pals]
    lines += ["    };", "", "    constexpr uint32_t kTables[kCount - 1][16] = {"]
    for n, _, t in pals[1:]:
        lines.append(f"        {{ {hexrow(t, '0x%06X')} }},  // {n}")
    lines += [
        "    };",
        "",
        "    // Name -> id, -1 if unknown. \"rainbow\" was id 0's name before the WLED",
        "    // palettes, and older apps still send it.",
        "    inline int find(std::string_view name) {",
        "        if (name == \"rainbow\") {",
        "            return 0;",
        "        }",
        "        for (int i = 0; i < kCount; i++) {",
        "            if (name == kNames[i]) {",
        "                return i;",
        "            }",
        "        }",
        "        return -1;",
        "    }",
        "}",
        "",
    ]
    return "\n".join(lines)


def emit_ts(pals):
    lines = [
        "// GENERATED by dune-weaver-firmware/tools/gen_led_palettes.py (WLED v0.15.3 +",
        "// FastLED 3.6.0). Edit the script, not this file: it also writes the firmware's",
        "// LedPalettes.h, and the two must stay byte-identical (the previews animate",
        "// the same tables the strip draws).",
        "",
        "export interface LedPalette {",
        "  /** $LED/Palette value. The array index is the firmware's palette id. */",
        "  name: string",
        "  label: string",
        "  /** 16 packed-RGB entries; null for 'default' (each effect's own colors). */",
        "  colors: readonly number[] | null",
        "}",
        "",
        "export const LED_PALETTE_LIST: readonly LedPalette[] = [",
    ]
    for n, label, t in pals:
        colors = "null" if t is None else f"[{hexrow(t, '0x%06x')}]"
        lines.append(f"  {{ name: '{n}', label: '{label}', colors: {colors} }},")
    lines += ["]", ""]
    return "\n".join(lines)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("palettes_h", help="WLED v0.15.3 wled00/palettes.h")
    ap.add_argument("--app", default="../dune-weaver-mobile", help="mobile app repo (default: sibling dir)")
    args = ap.parse_args()
    pals = build(Path(args.palettes_h).read_text(encoding="utf-8"))
    self_check(pals)
    root = Path(__file__).resolve().parent.parent
    (root / "FluidNC/src/LedPalettes.h").write_text(emit_cpp(pals))
    app = (root / args.app).resolve()
    (app / "src/lib/ledPalettes.ts").write_text(emit_ts(pals))
    print(f"{len(pals)} palettes -> FluidNC/src/LedPalettes.h, {app}/src/lib/ledPalettes.ts")


if __name__ == "__main__":
    main()
