// Sand-table fork: the WiFi setup portal served by handle_root in AP mode
// (and at /wifi in any mode).  One self-contained page - no external assets,
// since in AP mode there is no internet.  It drives /wifi_status, /wifi_scan,
// /wifi_save and /wifi_standalone; SSIDs and failure reasons are injected via
// textContent only, never innerHTML, so hostile SSID names can't script the
// page.

#pragma once

namespace WebUI {
    const char PAGE_WIFI_PORTAL[] = R"HTML(<!DOCTYPE html>
<html><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Dune Weaver Setup</title>
<style>
:root{color-scheme:dark}
body{font-family:-apple-system,system-ui,Segoe UI,Roboto,sans-serif;background:#141210;color:#eee8dd;margin:0;display:flex;justify-content:center}
.card{width:100%;max-width:420px;padding:28px 20px}
h1{font-size:22px;margin:8px 0 4px}
.sub{color:#a89f90;margin:0 0 20px}
.btn{display:block;width:100%;box-sizing:border-box;padding:14px;margin:10px 0;border-radius:10px;border:1px solid #3a352c;background:#241f19;color:#eee8dd;font-size:16px;text-align:left;cursor:pointer}
.btn b{display:block;font-size:16px;margin-bottom:2px}
.btn span{font-size:13px;color:#a89f90}
.btn.primary{background:#c98f3d;border-color:#c98f3d;color:#1a140c;text-align:center;font-weight:600}
.fail{background:#3a1f1a;border:1px solid #7a3b2e;color:#f0c4b0;padding:10px 12px;border-radius:8px;font-size:14px;margin:0 0 14px;display:none}
input{width:100%;box-sizing:border-box;padding:12px;margin:6px 0;border-radius:8px;border:1px solid #3a352c;background:#1c1813;color:#eee8dd;font-size:16px}
#aps{list-style:none;padding:0;margin:0 0 8px;max-height:200px;overflow-y:auto;border:1px solid #3a352c;border-radius:8px}
#aps li{padding:11px 12px;border-bottom:1px solid #2a251e;cursor:pointer;display:flex;justify-content:space-between;font-size:15px}
#aps li:last-child{border-bottom:0}
#aps li.sel{background:#33291b}
.dim{color:#a89f90;font-size:13px}
a.back{color:#c98f3d;text-decoration:none;font-size:14px}
.view{display:none}.view.on{display:block}
.spin{margin:14px auto;width:22px;height:22px;border:3px solid #3a352c;border-top-color:#c98f3d;border-radius:50%;animation:r 1s linear infinite}
@keyframes r{to{transform:rotate(360deg)}}
label{font-size:14px;color:#a89f90}
label input{width:auto;margin-right:6px}
</style></head><body><div class="card">

<div id="v-choose" class="view on">
 <h1>Dune Weaver</h1>
 <p class="sub">How do you want to use your table?</p>
 <div id="fail" class="fail"></div>
 <button class="btn" onclick="show('wifi')"><b>Connect to home Wi-Fi</b><span>The table joins your network and the app controls it from anywhere in the house. 2.4&nbsp;GHz networks only.</span></button>
 <button class="btn" onclick="standalone()"><b>Use standalone hotspot</b><span>No home Wi-Fi needed. Join the table&rsquo;s own hotspot from your phone whenever you want to control it.</span></button>
 <p class="dim" id="stanote"></p>
</div>

<div id="v-wifi" class="view">
 <p><a class="back" href="#" onclick="show('choose');return false">&larr; Back</a></p>
 <h1>Choose your network</h1>
 <ul id="aps"><li class="dim">Scanning&hellip;</li></ul>
 <input id="ssid" maxlength="32" placeholder="Network name (SSID)">
 <input id="pw" type="password" maxlength="64" placeholder="Password (at least 8 characters)">
 <label><input type="checkbox" onclick="$('pw').type=this.checked?'text':'password'">Show password</label>
 <button class="btn primary" onclick="save()">Connect</button>
 <p class="dim">Open (passwordless) networks aren&rsquo;t supported.</p>
</div>

<div id="v-saving" class="view">
 <h1>Connecting&hellip;</h1>
 <div class="spin"></div>
 <p>The table is restarting and joining <b id="sv"></b>. This hotspot will shut down in a few seconds.</p>
 <p>Rejoin your home Wi-Fi and open the Dune Weaver app.</p>
 <p class="dim">If the <span id="apname1">DuneWeaver</span> hotspot comes back in about a minute, the join failed &mdash; reconnect to it to see why and try again.</p>
</div>

<div id="v-standalone" class="view">
 <h1>Hotspot mode is on</h1>
 <p>You can close this window. Keep your phone on the <b id="apname2">DuneWeaver</b> network and open the Dune Weaver app to control your table.</p>
 <p class="dim">To switch to home Wi-Fi later, open <b>http://192.168.0.1</b> while connected to the hotspot.</p>
</div>

<p class="dim" style="margin-top:24px">HTTP API map: <a class="back" href="/help">/help</a></p>

<script>
function $(i){return document.getElementById(i)}
var scanT=null;
function show(v){
 var a=document.querySelectorAll('.view');
 for(var i=0;i<a.length;i++)a[i].className='view';
 $('v-'+v).className='view on';
 if(v=='wifi')scan(1);
}
fetch('/wifi_status').then(function(r){return r.json()}).then(function(s){
 var ap=s.ap_ssid||'DuneWeaver';
 $('apname1').textContent=ap;$('apname2').textContent=ap;
 if(s.fail){
  $('fail').style.display='block';
  $('fail').textContent='Couldn’t join “'+s.sta_ssid+'”: '+s.fail;
 }
 if(s.mode=='standalone')$('stanote').textContent='Standalone hotspot mode is currently active.';
 if(s.mode=='sta')$('stanote').textContent='Currently connected to “'+s.sta_ssid+'”.';
}).catch(function(){});
function scan(fresh){
 if(scanT){clearTimeout(scanT);scanT=null}
 fetch('/wifi_scan'+(fresh?'?rescan=1':'')).then(function(r){return r.json()}).then(function(s){
  if(s.status!='ok'){scanT=setTimeout(function(){scan(0)},1500);return}
  // JSONencoder emits numbers as strings ("rssi":"-23","secure":"1") - coerce.
  s.aps.sort(function(a,b){return (+b.rssi)-(+a.rssi)});
  var ul=$('aps');ul.innerHTML='';
  if(!s.aps.length){
   var e=document.createElement('li');e.className='dim';e.textContent='No networks found';ul.appendChild(e);
   scanT=setTimeout(function(){scan(1)},4000);return;
  }
  s.aps.forEach(function(a){
   var li=document.createElement('li');
   var n=document.createElement('span');n.textContent=a.ssid;
   var d=document.createElement('span');d.className='dim';
   d.textContent=(a.secure==1?'🔒 ':'')+bars(+a.rssi);
   li.appendChild(n);li.appendChild(d);
   li.onclick=function(){
    $('ssid').value=a.ssid;
    var s=ul.querySelectorAll('li');for(var i=0;i<s.length;i++)s[i].className='';
    li.className='sel';$('pw').focus();
   };
   ul.appendChild(li);
  });
 }).catch(function(){scanT=setTimeout(function(){scan(0)},2000)});
}
function bars(r){return r>-55?'▮▮▮':r>-70?'▮▮▯':'▮▯▯'}
// The two writes are idle-gated on the board and the boot auto-home usually
// still runs while the user is in this sheet, so on {"status":"busy"} keep
// retrying until the table goes Idle (the server refuses cleanly each time).
function save(){
 var s=$('ssid').value.trim(),p=$('pw').value;
 if(!s){alert('Enter a network name');return}
 if(p.length<8){alert('The password must be at least 8 characters. Open networks aren’t supported.');return}
 $('sv').textContent=s;show('saving');postSave(s,p);
}
function postSave(s,p){
 var b=new URLSearchParams();b.append('ssid',s);b.append('password',p);
 fetch('/wifi_save',{method:'POST',body:b}).then(function(r){return r.json()}).then(function(j){
  if(j.status=='busy'){setTimeout(function(){postSave(s,p)},2000);return}
  if(j.status!='ok'){alert(j.message||'Failed to save');show('wifi')}
 }).catch(function(){/* board is rebooting; a dropped reply is the success path */});
}
function standalone(){
 fetch('/wifi_standalone',{method:'POST'}).then(function(r){return r.json()}).then(function(j){
  if(j.status=='busy'){$('stanote').textContent='Table is finishing homing…';setTimeout(standalone,2000);return}
  if(j.status=='ok')show('standalone');else alert(j.message||'Failed')
 }).catch(function(){show('standalone')/* STA case reboots mid-reply */});
}
</script>
</div></body></html>
)HTML";
}
