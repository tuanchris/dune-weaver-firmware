#!/usr/bin/env python3
"""Local mock of the sand-table firmware API, to preview the UI without a board.
    python3 mock.py   then open http://localhost:8088
Simulates the command interface + new web routes (/sand_stop, /sand_feed),
SD file reads (pattern preview, playlist .txt), upload, settings round-trips."""
import http.server, urllib.parse, json, os, time, math

THUMBS="mock_thumbs"
have=[f[:-4] for f in os.listdir(THUMBS) if f.endswith(".png")]  # e.g. star.thr
EXTRA=["spiral-rose.thr","celtic-knot.thr","mandala-7.thr","wave-interference.thr","no-thumb.thr"]
PATTERNS=sorted(set(have+EXTRA))
PLAYLISTS=["evening.txt","all-spirals.txt","favorites.txt"]

sim={"state":"Idle","file":"","t0":0,"dur":80,"feed":800,
     "pl":{"active":False,"index":0,"total":0,"name":"","clearing":False,"quiet":False},
     "led":{"effect":"rainbow","brightness":40}}
settings={"Sands/Slots":"21:00-08:00@daily","Sands/Enabled":"0","Playlist/Mode":"loop",
  "Playlist/Shuffle":"0","Playlist/PauseTime":"30","Playlist/ClearPattern":"adaptive",
  "Playlist/AutoHome":"5","THR/Feed":"800","LED/Effect":"rainbow","LED/Brightness":"40",
  "LED/Speed":"50","LED/RunEffect":"rainbow","LED/IdleEffect":"static",
  "Sta/SSID":"The Bears’ Wi-Fi Network","WiFi/Mode":"STA>AP","Sta/IPMode":"DHCP"}

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
    return {"state":sim["state"],"theta":round(time.time()%6.283,4),"rho":round(0.5+0.45*math.sin(time.time()),4),
      "feed":int(settings.get("THR/Feed",800)) if sim["state"]!="Run" else sim["feed"],
      "running":sim["state"] in("Run","Hold") and bool(sim["file"]),
      "file":sim["file"],"progress":round(pr,1),"playlist":sim["pl"],
      "led":{"effect":settings["LED/Effect"],"brightness":int(settings["LED/Brightness"])}}

def spiral_thr():
    out=["# mock pattern"]
    for i in range(1200):
        t=i*0.18; r=min(1.0,i/1200.0)
        out.append(f"{t:.4f} {r:.4f}")
    return "\n".join(out)+"\n"

def run_cmd(c):
    if c=="[ESP410]": return json.dumps({"AP_LIST":[
        {"SSID":"The Bears’ Wi-Fi Network","SIGNAL":34,"IS_PROTECTED":1},
        {"SSID":"Neighbor 2.4","SIGNAL":58,"IS_PROTECTED":1},
        {"SSID":"CoffeeShop","SIGNAL":22,"IS_PROTECTED":0}]})
    if c=="$Sand/Status": return json.dumps(status())
    if c=="$Sand/Patterns": return json.dumps(PATTERNS)
    if c=="$Sand/Playlists": return json.dumps(PLAYLISTS)
    if c.startswith("$SD/Run="):
        sim.update(state="Run",file=c[8:],t0=time.time()); sim["pl"]["active"]=False; return "ok"
    if c.startswith("$Playlist/Run="):
        sim.update(state="Run",file="/patterns/"+PATTERNS[0],t0=time.time())
        sim["pl"].update(active=True,index=0,total=4,name=c[14:],clearing=False); return "ok"
    if c=="$Playlist/Skip":
        if sim["pl"]["active"]: sim["pl"]["index"]=min(sim["pl"]["total"]-1,sim["pl"]["index"]+1); sim["t0"]=time.time()
        return "ok"
    if c=="$H": sim.update(state="Idle"); return "ok"
    if c.startswith("$THR/Feed="): settings["THR/Feed"]=c[10:]; return "ok"
    if c.startswith("$") and "=" in c:
        k,v=c[1:].split("=",1); settings[k]=v; return "ok"
    if c.startswith("$"):
        k=c[1:]
        if k in settings: return f"{k}={settings[k]}"
        if k=="I": return "[VER:3.9 FluidNC v3.9.5 (sandtable):]"
    return "ok"

class H(http.server.BaseHTTPRequestHandler):
    def log_message(self,*a): pass
    def _send(self,code,body,ctype="text/plain"):
        b=body if isinstance(body,bytes) else body.encode()
        self.send_response(code);self.send_header("Content-Type",ctype)
        self.send_header("Content-Length",str(len(b)));self.send_header("Cache-Control","no-store")
        self.end_headers();self.wfile.write(b)
    def do_POST(self):
        u=urllib.parse.urlparse(self.path)
        if u.path=="/upload":
            try:self.rfile.read(int(self.headers.get("Content-Length",0)))
            except:pass
            return self._send(200,"ok")
        return self._send(404,"nf")
    def do_GET(self):
        u=urllib.parse.urlparse(self.path);q=urllib.parse.parse_qs(u.query)
        if u.path in("/","/index.html"): return self._send(200,open("index.html","rb").read(),"text/html")
        if u.path=="/command":
            c=(q.get("plain") or q.get("cmd") or [""])[0]
            return self._send(200,run_cmd(c)+"\nok\n")
        if u.path=="/sand_status":           # multi-client status poll (HTTP body)
            return self._send(200,json.dumps(status()),"application/json")
        if u.path in("/feedhold_reload","/cyclestart_reload","/restart_reload","/sand_stop","/sand_home","/sand_feed"):
            if u.path=="/feedhold_reload": sim["state"]="Hold" if sim["file"] else "Idle"
            elif u.path=="/cyclestart_reload":
                if sim["file"]: sim["state"]="Run"
            elif u.path in("/restart_reload","/sand_stop"): sim.update(state="Idle",file="");sim["pl"]["active"]=False
            elif u.path=="/sand_home": sim.update(state="Idle")
            return self._send(200,"ok")
        if u.path.startswith("/sd/thumbs/"):
            fp=os.path.join(THUMBS,urllib.parse.unquote(u.path[len("/sd/thumbs/"):]))
            if os.path.exists(fp): return self._send(200,open(fp,"rb").read(),"image/png")
            return self._send(404,"no thumb")
        if u.path.startswith("/sd/patterns/"): return self._send(200,spiral_thr())
        if u.path.startswith("/sd/playlists/"): return self._send(200,"\n".join("/patterns/"+p for p in PATTERNS[:4])+"\n")
        return self._send(404,"not found")

if __name__=="__main__":
    print("Sand-table UI mock → http://localhost:8088  (Ctrl-C to stop)")
    http.server.ThreadingHTTPServer(("127.0.0.1",8088),H).serve_forever()
