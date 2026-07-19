#!/usr/bin/env python3
"""Tiny local dashboard for watching a running soak (realistic_soak.py or
soak.py).  Serves a self-refreshing page AND fetches the board server-side, so
there's no CORS/file:// wall - open the printed URL in a browser.

    python3 soak/dashboard.py                                  # defaults below
    python3 soak/dashboard.py --outdir soak/logs/realistic_20260718_1229 \
        --base http://192.168.68.128 --port 8787

It reads the soak's telemetry.jsonl (history / heap sparkline) + events.log
(alerts), and adds one light ~5 s /sand_status poll of its own for a live top
line.  That's a negligible add on top of the soak's own pollers; it never
touches the SD (no file probing).
"""

import argparse
import glob
import json
import os
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ARGS = None


def latest_outdir():
    here = os.path.dirname(os.path.abspath(__file__))
    runs = sorted(glob.glob(os.path.join(here, "logs", "realistic_*"))
                  + glob.glob(os.path.join(here, "logs", "soak_*")))
    return runs[-1] if runs else os.path.join(here, "logs")


def tail_lines(path, n):
    try:
        with open(path, "r", errors="replace") as f:
            return f.readlines()[-n:]
    except OSError:
        return []


def read_pid_alive(outdir):
    try:
        pid = int(open(os.path.join(outdir, "pid")).read().strip())
    except (OSError, ValueError):
        return None
    try:
        os.kill(pid, 0)
        return True
    except OSError:
        return False


def build_state():
    outdir = ARGS.outdir
    # history from telemetry.jsonl
    recs = []
    for ln in tail_lines(os.path.join(outdir, "telemetry.jsonl"), 240):
        ln = ln.strip()
        if ln:
            try:
                recs.append(json.loads(ln))
            except ValueError:
                pass
    # alerts/warns from events.log
    events = [ln.rstrip("\n") for ln in tail_lines(os.path.join(outdir, "events.log"), 400)
              if " ALERT " in ln or " WARN " in ln]
    # live poll (server-side, no CORS)
    live, live_err = None, None
    try:
        body = urllib.request.urlopen(ARGS.base.rstrip("/") + "/sand_status", timeout=5).read()
        live = json.loads(body)
    except Exception as e:  # board busy/rebooting/unreachable - show it, don't crash
        live_err = str(e)
    return {
        "outdir": os.path.basename(outdir),
        "base": ARGS.base,
        "proc_alive": read_pid_alive(outdir),
        "records": recs,
        "events": events[-40:],
        "alert_count": sum(1 for e in events if " ALERT " in e),
        "warn_count": sum(1 for e in events if " WARN " in e),
        "live": live,
        "live_err": live_err,
    }


PAGE = r"""<!doctype html><html lang=en><head><meta charset=utf-8>
<meta name=viewport content="width=device-width,initial-scale=1">
<title>Soak monitor</title><style>
:root{--bg:#0d1117;--card:#161b22;--line:#30363d;--fg:#e6edf3;--dim:#8b949e;
--ok:#3fb950;--warn:#d29922;--bad:#f85149;--accent:#58a6ff}
@media(prefers-color-scheme:light){:root{--bg:#f6f8fa;--card:#fff;--line:#d0d7de;
--fg:#1f2328;--dim:#636c76;--ok:#1a7f37;--warn:#9a6700;--bad:#cf222e;--accent:#0969da}}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--fg);
font:14px/1.45 ui-monospace,SFMono-Regular,Menlo,monospace;padding:16px}
h1{font-size:15px;margin:0 0 2px;font-weight:600}
.sub{color:var(--dim);font-size:12px;margin-bottom:14px}
.banner{padding:12px 16px;border-radius:8px;font-weight:700;font-size:16px;
margin-bottom:14px;border:1px solid var(--line)}
.b-ok{background:color-mix(in srgb,var(--ok) 15%,transparent);color:var(--ok)}
.b-bad{background:color-mix(in srgb,var(--bad) 15%,transparent);color:var(--bad)}
.b-dead{background:color-mix(in srgb,var(--dim) 15%,transparent);color:var(--dim)}
.grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(150px,1fr));gap:10px}
.card{background:var(--card);border:1px solid var(--line);border-radius:8px;padding:10px 12px}
.k{color:var(--dim);font-size:11px;text-transform:uppercase;letter-spacing:.04em}
.v{font-size:19px;font-weight:600;margin-top:3px;word-break:break-word}
.v small{font-size:12px;color:var(--dim);font-weight:400}
.ok{color:var(--ok)}.warn{color:var(--warn)}.bad{color:var(--bad)}
.wide{grid-column:1/-1}
svg{display:block;width:100%;height:60px}
.charts{display:grid;grid-template-columns:repeat(auto-fill,minmax(300px,1fr));gap:10px;margin-top:10px}
.chart{background:var(--card);border:1px solid var(--line);border-radius:8px;padding:10px 12px}
.chart h3{margin:0 0 4px;font-size:11px;font-weight:600;color:var(--dim);
text-transform:uppercase;letter-spacing:.04em;display:flex;justify-content:space-between;gap:8px}
.chart h3 .cur{color:var(--fg);font-weight:700}
.chart svg{height:120px}
.axis{font-size:9px}.axis text{fill:var(--dim)}
.leg{font-size:10px;color:var(--dim);margin-top:5px;display:flex;gap:12px;flex-wrap:wrap}
.leg i{display:inline-block;width:10px;height:2px;vertical-align:middle;margin-right:4px;border-radius:2px}
.evt{font-size:12px;white-space:pre-wrap;max-height:220px;overflow:auto}
.evt .a{color:var(--bad)}.evt .w{color:var(--warn)}
.foot{color:var(--dim);font-size:11px;margin-top:12px}
</style></head><body>
<h1>Soak monitor <span id=dot></span></h1>
<div class=sub id=meta>connecting…</div>
<div class=banner id=banner>…</div>
<div class=grid id=cards></div>
<div class=charts id=charts></div>
<div class="card wide" style=margin-top:10px>
  <div class=k>alerts / warnings</div>
  <div class=evt id=events>—</div>
</div>
<div class=foot id=foot></div>
<script>
const $=id=>document.getElementById(id);
const REFRESH=%%REFRESH%%;
function fmtUp(s){if(s==null)return'—';s=+s;const h=Math.floor(s/3600),m=Math.floor(s%3600/60);
return h?`${h}h ${m}m`:`${m}m ${s%60|0}s`;}
function card(k,v,cls){return`<div class=card><div class=k>${k}</div><div class="v ${cls||''}">${v}</div></div>`;}
function tsSec(s){return s?Date.parse(s.replace(' ','T'))/1000:0;}
function rateFn(key){return (r,i,recs)=>{if(i===0)return null;const p=recs[i-1];
  const dv=r[key]-p[key],dt=(tsSec(r.ts)-tsSec(p.ts))||60;return dv>=0?dv/dt*60:null;};}
const CHARTS=[
  {id:'heap',title:'heap (bytes)',fmt:v=>Math.round(v/1000)+'k',ymin:0,
   thresholds:[{v:20000,color:'#d29922'},{v:12000,color:'#f85149'}],
   series:[{key:'heap_largest',color:'#58a6ff',label:'largest'},
           {key:'heap',color:'#8b949e',label:'free'},
           {key:'heap_min',color:'#6e7681',label:'min'}]},
  {id:'thru',title:'client load (/min)',fmt:v=>Math.round(v),ymin:0,
   series:[{fn:rateFn('requests'),color:'#3fb950',label:'req/min'},
           {fn:rateFn('failures'),color:'#f85149',label:'fail/min'}]},
  {id:'prog',title:'pattern progress (%)',fmt:v=>Math.round(v)+'%',ymin:0,
   series:[{fn:r=>typeof r.progress==='number'&&r.progress>=0?r.progress*100:null,
            color:'#58a6ff',label:'progress'}]},
  {id:'up',title:'uptime (min) — a drop = reboot',fmt:v=>Math.round(v/60)+'m',ymin:0,
   series:[{key:'uptime',color:'#a371f7',label:'uptime'}]},
];
function lineChart(recs,cfg){
  const W=600,H=120,PL=4,PR=4,PT=10,PB=12,n=recs.length;
  const vals=cfg.series.map(s=>recs.map((r,i)=>{
    const v=typeof s.fn==='function'?s.fn(r,i,recs):r[s.key];
    return (typeof v==='number'&&isFinite(v))?v:null;}));
  let all=[];vals.forEach(a=>a.forEach(v=>{if(v!=null)all.push(v);}));
  (cfg.thresholds||[]).forEach(t=>all.push(t.v));
  if(cfg.ymin!=null)all.push(cfg.ymin);
  if(!all.length)return{svg:'',cur:'—'};
  let mx=Math.max(...all),mn=Math.min(...all);if(mx===mn)mx+=1;
  const X=i=>PL+(n<=1?0:i/(n-1))*(W-PL-PR),Y=v=>PT+(1-(v-mn)/(mx-mn))*(H-PT-PB);
  let svg='';
  (cfg.thresholds||[]).forEach(t=>{const y=Y(t.v).toFixed(1);
    svg+=`<line x1=0 y1=${y} x2=${W} y2=${y} stroke="${t.color}" stroke-width=.7 stroke-dasharray=3,3 opacity=.55/>`;});
  vals.forEach((a,si)=>{let d='',pen=false;
    a.forEach((v,i)=>{if(v==null){pen=false;return;}
      d+=(pen?'L':'M')+X(i).toFixed(1)+' '+Y(v).toFixed(1)+' ';pen=true;});
    if(d)svg+=`<path d="${d}" fill=none stroke="${cfg.series[si].color}" stroke-width=1.4 stroke-linejoin=round/>`;});
  const f=cfg.fmt||(v=>Math.round(v));
  svg+=`<g class=axis><text x=3 y=9>${f(mx)}</text><text x=3 y=${H-3}>${f(mn)}</text></g>`;
  let cur='—';for(let i=vals[0].length-1;i>=0;i--){if(vals[0][i]!=null){cur=f(vals[0][i]);break;}}
  return{svg,cur};
}
function renderCharts(recs){
  const host=$('charts');
  if(!host.dataset.built){
    host.innerHTML=CHARTS.map(c=>`<div class=chart><h3><span>${c.title}</span>`+
      `<span class=cur id=cur-${c.id}></span></h3>`+
      `<svg id=svg-${c.id} viewBox="0 0 600 120" preserveAspectRatio=none></svg>`+
      `<div class=leg>${c.series.map(s=>`<span><i style=background:${s.color}></i>${s.label}</span>`).join('')}</div></div>`).join('');
    host.dataset.built='1';
  }
  CHARTS.forEach(c=>{const r=lineChart(recs,c);
    document.getElementById('svg-'+c.id).innerHTML=r.svg;
    document.getElementById('cur-'+c.id).textContent=r.cur;});
}
async function tick(){
  let s;try{s=await(await fetch('/api',{cache:'no-store'})).json();}catch(e){
    $('dot').textContent='● offline';$('dot').style.color='var(--bad)';return;}
  const L=s.live||{},pl=(L.playlist||{});
  const dead=s.proc_alive===false;
  const bad=s.alert_count>0;
  $('banner').className='banner '+(dead?'b-dead':bad?'b-bad':'b-ok');
  $('banner').textContent=dead?'⏹  SOAK PROCESS NOT RUNNING':
    bad?`✗  ${s.alert_count} ALERT(s) — investigate`:'✓  CLEAN — no alerts';
  $('dot').textContent='● live';$('dot').style.color='var(--ok)';
  $('meta').textContent=`${s.base}  ·  ${s.outdir}  ·  ${s.records.length} samples  ·  proc ${dead?'dead':'alive'}`;
  const last=s.records[s.records.length-1]||{};
  const state=L.state||last.state||'?';
  const sdok=(L.sd_ok!=null?L.sd_ok:last.sd_ok);
  const reset=L.last_reset||last.last_reset||'?';
  const resetBad=['panic','task_wdt','int_wdt','brownout'].includes(reset);
  const hl=L.heap_largest!=null?L.heap_largest:last.heap_largest;
  const hf=L.heap!=null?L.heap:last.heap;
  const hm=L.heap_min!=null?L.heap_min:last.heap_min;
  const file=(L.file||last.file||'').replace('/patterns/','')||'—';
  const cards=[
    card('heap free',(hf??'—'),hf!=null&&hf<12000?'bad':hf!=null&&hf<20000?'warn':'ok'),
    card('heap_largest',(hl??'—'),hl!=null&&hl<12000?'bad':hl!=null&&hl<20000?'warn':'ok'),
    card('heap_min',(hm??'—')+' <small>low-water</small>',hm!=null&&hm<12000?'bad':''),
    card('state',state,state==='Run'?'ok':state==='Alarm'?'bad':''),
    card('pattern',`${file}`,''),
    card('playlist',`${pl.name?('#'+ (pl.index??'?')+' / '+(pl.total??'?')):(last.pl_index!=null?('#'+last.pl_index):'—')}`,pl.active===false?'warn':''),
    card('sd_ok',sdok===false?'FALSE':sdok===true?'true':'?',sdok===false?'bad':'ok'),
    card('uptime',fmtUp(L.uptime!=null?L.uptime:last.uptime)+(last.reboots?` <small>(${last.reboots} reboot)</small>`:''),last.reboots?'bad':''),
    card('last_reset',reset,resetBad?'bad':''),
    card('requests',(last.requests??'—')+' <small>'+(last.failures??0)+' fail</small>',last.failures?'warn':''),
    card('led / pause',(last.led??0)+' / '+(last.pauses??0),''),
  ];
  $('cards').innerHTML=cards.join('');
  renderCharts(s.records);
  $('events').innerHTML=s.events.length?s.events.map(e=>{
    const c=e.includes(' ALERT ')?'a':'w';return`<span class=${c}>${e.replace(/</g,'&lt;')}</span>`;
  }).join('\n'):'<span style=color:var(--dim)>none 🎉</span>';
  if(s.live_err)$('foot').textContent='live poll error: '+s.live_err+'  (board busy/rebooting — history still valid)';
  else $('foot').textContent='refresh '+(REFRESH/1000)+'s · '+new Date().toLocaleTimeString();
}
tick();setInterval(tick,REFRESH);
</script></body></html>"""


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def _send(self, code, body, ctype):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path.startswith("/api"):
            body = json.dumps(build_state()).encode()
            self._send(200, body, "application/json")
        elif self.path == "/" or self.path.startswith("/?"):
            html = PAGE.replace("%%REFRESH%%", str(int(ARGS.refresh * 1000))).encode()
            self._send(200, html, "text/html; charset=utf-8")
        else:
            self._send(404, b"not found", "text/plain")


def main():
    global ARGS
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--base", default="http://192.168.68.128")
    ap.add_argument("--outdir", default=None, help="soak run dir (default: newest under soak/logs)")
    ap.add_argument("--port", type=int, default=8787)
    ap.add_argument("--refresh", type=float, default=5.0, help="page refresh seconds")
    ARGS = ap.parse_args()
    ARGS.outdir = os.path.abspath(ARGS.outdir) if ARGS.outdir else latest_outdir()
    srv = ThreadingHTTPServer(("127.0.0.1", ARGS.port), Handler)
    print(f"soak dashboard: http://localhost:{ARGS.port}")
    print(f"  board: {ARGS.base}")
    print(f"  outdir: {ARGS.outdir}")
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
