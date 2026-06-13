#!/usr/bin/env python3
"""Local mock of the sand-table firmware API, to preview the UI without a board.
    python3 mock.py   then open http://localhost:8088
Simulates $Sand/Status (with advancing progress), pattern/playlist lists,
settings round-trips, LED, and the realtime pause/resume/stop routes."""
import http.server, urllib.parse, json, os, time, re

THUMBS="mock_thumbs"
have_thumbs=[f[:-4] for f in os.listdir(THUMBS) if f.endswith(".png")]  # e.g. star.thr
EXTRA=["spiral-rose.thr","celtic-knot.thr","mandala-7.thr","wave-interference.thr","no-thumb-demo.thr"]
PATTERNS=sorted(set(have_thumbs+EXTRA))
PLAYLISTS=["evening.txt","all-spirals.txt","favorites.txt"]

sim={"state":"Idle","file":"","t0":0,"dur":90,"feed":800,
     "pl":{"active":False,"index":0,"total":0,"name":"","clearing":False,"quiet":False},
     "led":{"effect":"rainbow","brightness":40}}
settings={"Sands/Slots":"21:00-08:00@daily","Sands/Enabled":"0","Playlist/Mode":"loop",
  "Playlist/Shuffle":"0","Playlist/PauseTime":"30","Playlist/ClearPattern":"adaptive",
  "Playlist/AutoHome":"5","LED/RunEffect":"rainbow","LED/IdleEffect":"static","LED/Speed":"50"}

def progress():
    if sim["state"]!="Run" or not sim["file"]: return -1.0
    p=min(100.0,(time.time()-sim["t0"])/sim["dur"]*100)
    if p>=100:
        if sim["pl"]["active"] and sim["pl"]["index"]<sim["pl"]["total"]-1:
            sim["pl"]["index"]+=1; sim["t0"]=time.time(); return 0.0
        sim["state"]="Idle"; sim["file"]=""; sim["pl"]["active"]=False
    return p

def status():
    pr=progress()
    return {"state":sim["state"],"theta":round((time.time()%6.283),4),"rho":round(0.5+0.4*((time.time()%4)/4),4),
      "feed":sim["feed"],"running":sim["state"] in("Run","Hold") and bool(sim["file"]),
      "file":sim["file"],"progress":round(pr,1),"playlist":sim["pl"],"led":sim["led"]}

def run_cmd(c):
    if c=="$Sand/Status": return json.dumps(status())
    if c=="$Sand/Patterns": return json.dumps(PATTERNS)
    if c=="$Sand/Playlists": return json.dumps(PLAYLISTS)
    if c.startswith("$SD/Run="):
        sim.update(state="Run",file=c[8:],t0=time.time()); sim["pl"]["active"]=False; return "ok"
    if c.startswith("$Playlist/Run="):
        nm=c[14:]; sim.update(state="Run",file="/patterns/"+(PATTERNS[0]),t0=time.time())
        sim["pl"].update(active=True,index=0,total=4,name=nm,clearing=False); return "ok"
    if c=="$Playlist/Skip":
        if sim["pl"]["active"]: sim["pl"]["index"]=min(sim["pl"]["total"]-1,sim["pl"]["index"]+1); sim["t0"]=time.time()
        return "ok"
    if c=="$Playlist/Stop": sim.update(state="Idle",file=""); sim["pl"]["active"]=False; return "ok"
    if c=="$H": sim.update(state="Idle"); return "ok"
    if c.startswith("$THR/Feed="): sim["feed"]=int(c[10:]); return "ok"
    if c.startswith("$LED/Effect="): sim["led"]["effect"]=c[12:]; return "ok"
    if c.startswith("$LED/Brightness="): sim["led"]["brightness"]=int(c[16:]); return "ok"
    if c.startswith("$") and "=" in c:
        k,v=c[1:].split("=",1); settings[k]=v; return "ok"
    if c.startswith("$"):  # bare read
        k=c[1:]; 
        if k in settings: return f"{k}={settings[k]}"
    return "ok"

class H(http.server.BaseHTTPRequestHandler):
    def log_message(self,*a): pass
    def _send(self,code,body,ctype="text/plain",enc=None):
        b=body if isinstance(body,bytes) else body.encode()
        self.send_response(code); self.send_header("Content-Type",ctype)
        if enc: self.send_header("Content-Encoding",enc)
        self.send_header("Content-Length",str(len(b))); self.send_header("Cache-Control","no-store")
        self.end_headers(); self.wfile.write(b)
    def do_GET(self):
        u=urllib.parse.urlparse(self.path); q=urllib.parse.parse_qs(u.query)
        if u.path=="/" or u.path=="/index.html":
            return self._send(200,open("index.html","rb").read(),"text/html")
        if u.path=="/command":
            c=(q.get("plain") or q.get("cmd") or [""])[0]
            return self._send(200,run_cmd(c)+"\nok\n")
        if u.path in("/feedhold_reload","/cyclestart_reload","/restart_reload"):
            if u.path=="/feedhold_reload": sim["state"]="Hold" if sim["file"] else "Idle"
            elif u.path=="/cyclestart_reload":
                if sim["file"]: sim["state"]="Run"
            else: sim.update(state="Idle",file=""); sim["pl"]["active"]=False
            return self._send(200,"ok")
        if u.path.startswith("/sd/thumbs/"):
            name=urllib.parse.unquote(u.path[len("/sd/thumbs/"):])
            fp=os.path.join(THUMBS,name)
            if os.path.exists(fp): return self._send(200,open(fp,"rb").read(),"image/png")
            return self._send(404,"no thumb")
        return self._send(404,"not found")

if __name__=="__main__":
    print("Sand-table UI mock → http://localhost:8088  (Ctrl-C to stop)")
    http.server.ThreadingHTTPServer(("127.0.0.1",8088),H).serve_forever()
