#!/usr/bin/env python3
"""Gzip index.html -> FluidNC/data/index.html.gz (served from littlefs)."""
import gzip, os, sys
src=os.path.join(os.path.dirname(__file__),"index.html")
dst=os.path.join(os.path.dirname(__file__),"..","FluidNC","data","index.html.gz")
raw=open(src,"rb").read()
data=gzip.compress(raw,9)
open(dst,"wb").write(data)
LITTLEFS=192*1024
other=3156+1150  # config.yaml + favicon.ico already in data/
print(f"index.html      {len(raw):>7,} B")
print(f"index.html.gz   {len(data):>7,} B  ({len(data)/len(raw)*100:.0f}% of raw)")
print(f"littlefs budget {LITTLEFS:>7,} B  | gz+config+favicon = {len(data)+other:,} B  ({(len(data)+other)/LITTLEFS*100:.0f}% used)")
print("OK" if len(data)+other < LITTLEFS else "OVER BUDGET")
