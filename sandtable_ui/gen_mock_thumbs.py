#!/usr/bin/env python3
"""Render a few real .thr patterns to 256px thumbnails for the mock UI.
Thumbnail convention matches the firmware: /sd/thumbs/<pattern>.png"""
import os, glob, numpy as np
import matplotlib; matplotlib.use("Agg")
import matplotlib.pyplot as plt

PAT="/Volumes/4TB SSD/projects/dune-weaver/patterns"
picks=["star.thr","custom_patterns/sand-patterns/patterns/tara-1.thr",
  "custom_patterns/sand-patterns/patterns/anime-girl-8.thr",
  "custom_patterns/sand-patterns/patterns/fathers-day-1.thr",
  "custom_patterns/sand-patterns/patterns/hibiscus-flower.thr",
  "custom_patterns/sand-patterns/patterns/bodhi-tree.thr",
  "0-1-hubcap.thr","1-1-ibex.thr","1-1-pizza-slice-swirl.thr"]
def parse(p):
    th,rh=[],[]
    for ln in open(p,errors="replace"):
        s=ln.strip()
        if not s or s[0]=="#": continue
        q=s.split()
        if len(q)<2: continue
        try: th.append(float(q[0])); rh.append(float(q[1]))
        except: pass
    th=np.array(th); rh=np.clip(np.array(rh),0,1)
    return rh*np.cos(th), rh*np.sin(th)
n=0
for rel in picks:
    p=os.path.join(PAT,rel)
    if not os.path.exists(p): continue
    x,y=parse(p)
    fig=plt.figure(figsize=(2.56,2.56),dpi=100)
    ax=fig.add_axes([0,0,1,1]); ax.plot(x,y,color="#e0aa4e",lw=0.5,solid_capstyle="round")
    ax.set_aspect("equal"); ax.set_xlim(-1.04,1.04); ax.set_ylim(-1.04,1.04); ax.axis("off")
    out=os.path.join("mock_thumbs", os.path.basename(rel)+".png")
    fig.savefig(out,transparent=True,dpi=100); plt.close(fig); n+=1
print(f"generated {n} thumbnails -> mock_thumbs/")
