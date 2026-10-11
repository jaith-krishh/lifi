// Web dashboard served by the ESP32 at "/". Self-contained: no internet needed.
// It polls /api/status every 250 ms and draws the live light signal.
#pragma once
#include <Arduino.h>

const char INDEX_HTML[] PROGMEM = R"rawliteral(<!doctype html>
<html lang="en"><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>LiFi Receiver</title>
<style>
:root{--bg:#f4f5f7;--card:#fff;--ink:#16181d;--mute:#667085;--line:#e4e7ec;
--accent:#2563eb;--ok:#16a34a;--warn:#d97706;--bad:#dc2626;--trace:#2563eb;--thr:#d97706}
@media (prefers-color-scheme:dark){:root{--bg:#0e1116;--card:#171b22;--ink:#e6e9ef;--mute:#8b94a5;
--line:#2a303b;--accent:#60a5fa;--ok:#4ade80;--warn:#fbbf24;--bad:#f87171;--trace:#60a5fa;--thr:#fbbf24}}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--ink);font:15px/1.45 system-ui,-apple-system,Segoe UI,Roboto,sans-serif}
.wrap{max-width:980px;margin:0 auto;padding:16px}
header{display:flex;align-items:center;justify-content:space-between;gap:12px;margin-bottom:14px}
h1{font-size:20px;margin:0;letter-spacing:-.01em}
h1 small{color:var(--mute);font-weight:500;font-size:13px;margin-left:6px}
.pill{display:inline-flex;align-items:center;gap:6px;padding:4px 10px;border-radius:999px;
background:var(--card);border:1px solid var(--line);font-size:13px;font-weight:600;white-space:nowrap}
.dot{width:8px;height:8px;border-radius:50%;background:var(--mute)}
.card{background:var(--card);border:1px solid var(--line);border-radius:14px;padding:16px;margin-bottom:14px}
.label{color:var(--mute);font-size:12px;text-transform:uppercase;letter-spacing:.06em;font-weight:600}
#msg{font:600 clamp(26px,7vw,46px)/1.15 ui-monospace,SFMono-Regular,Menlo,Consolas,monospace;
margin:6px 0 4px;word-break:break-all;min-height:1.15em}
#msg.flash{animation:fl .9s ease-out}
@keyframes fl{0%{color:var(--ok);transform:scale(1.03)}100%{color:var(--ink);transform:none}}
#rx{font-family:ui-monospace,Menlo,Consolas,monospace;color:var(--mute);min-height:1.4em}
.bar{height:4px;background:var(--line);border-radius:4px;overflow:hidden;margin-top:8px}
.bar i{display:block;height:100%;width:0;background:var(--accent);transition:width .2s}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(140px,1fr));gap:10px;margin-bottom:14px}
.tile{background:var(--card);border:1px solid var(--line);border-radius:12px;padding:12px}
.tile b{display:block;font-size:24px;font-variant-numeric:tabular-nums;margin-top:2px}
.tile span{color:var(--mute);font-size:12px}
canvas{width:100%;height:190px;display:block}
.legend{display:flex;flex-wrap:wrap;gap:14px;font-size:12px;color:var(--mute);margin-top:8px}
.legend i{display:inline-block;width:14px;height:3px;vertical-align:middle;margin-right:5px}
table{width:100%;border-collapse:collapse;font-size:14px}
td{padding:7px 4px;border-top:1px solid var(--line);vertical-align:top}
td.m{font-family:ui-monospace,Menlo,Consolas,monospace;word-break:break-all}
td.t{color:var(--mute);white-space:nowrap;width:1%}
.chip{font-size:11px;font-weight:700;padding:2px 7px;border-radius:6px;text-transform:uppercase}
.c-ok{background:color-mix(in srgb,var(--ok) 18%,transparent);color:var(--ok)}
.c-bad{background:color-mix(in srgb,var(--bad) 18%,transparent);color:var(--bad)}
.row{display:flex;justify-content:space-between;align-items:center;gap:10px;margin-bottom:8px}
button{font:inherit;font-size:13px;padding:6px 12px;border-radius:8px;border:1px solid var(--line);
background:var(--bg);color:var(--ink);cursor:pointer}
.empty{color:var(--mute);padding:10px 0}
</style></head><body><div class="wrap">

<header>
  <h1>LiFi Receiver<small id="prof"></small></h1>
  <span class="pill"><span class="dot" id="dot"></span><span id="state">Connecting…</span></span>
</header>

<section class="card">
  <div class="label">Last message received</div>
  <div id="msg">—</div>
  <div id="rx"></div>
  <div class="bar"><i id="prog"></i></div>
</section>

<div class="grid">
  <div class="tile"><span>Frames OK</span><b id="ok">0</b></div>
  <div class="tile"><span>Errors</span><b id="err">0</b><span id="errd"></span></div>
  <div class="tile"><span>Link quality</span><b id="q">—</b><span>last 32 frames</span></div>
  <div class="tile"><span>Data rate</span><b id="bps">—</b><span>bit/s, last frame</span></div>
  <div class="tile"><span>Threshold</span><b id="thr">—</b><span id="mode"></span></div>
  <div class="tile"><span>Sensor lag</span><b id="lag">—</b><span>auto-corrected</span></div>
</div>

<section class="card">
  <div class="row"><div class="label">Light signal (last 3 s)</div><div class="label" id="adc"></div></div>
  <canvas id="scope"></canvas>
  <div class="legend">
    <span><i style="background:var(--trace)"></i>LDR level</span>
    <span><i style="background:var(--thr)"></i>Threshold</span>
    <span><i style="background:var(--mute)"></i>Ambient / LED peak</span>
  </div>
</section>

<section class="card">
  <div class="row"><div class="label">Frame log</div><button id="reset">Reset stats</button></div>
  <table><tbody id="log"></tbody></table>
  <div class="empty" id="empty">No frames yet. Point the transmitter LED at the LDR.</div>
</section>

</div><script>
const $=id=>document.getElementById(id);
const css=n=>getComputedStyle(document.documentElement).getPropertyValue(n).trim();
let lastSeq=-1,fails=0;

function ago(ms){const s=Math.max(0,Math.round(ms/1000));
  return s<60?s+'s ago':s<3600?Math.floor(s/60)+'m ago':Math.floor(s/3600)+'h ago';}

function setState(text,color){$('state').textContent=text;$('dot').style.background=css(color);}

function draw(d){
  const c=$('scope'),dpr=window.devicePixelRatio||1,w=c.clientWidth,h=c.clientHeight;
  if(c.width!==w*dpr||c.height!==h*dpr){c.width=w*dpr;c.height=h*dpr;}
  const g=c.getContext('2d');g.setTransform(dpr,0,0,dpr,0,0);g.clearRect(0,0,w,h);
  const tr=d.trace,vals=tr.concat([d.thr,d.floor,d.peak]);
  let lo=Math.min(...vals),hi=Math.max(...vals);const pad=Math.max(40,(hi-lo)*.12);lo-=pad;hi+=pad;
  const y=v=>h-(v-lo)/(hi-lo)*h,x=i=>i/(tr.length-1)*w;
  g.strokeStyle=css('--line');g.lineWidth=1;
  for(let i=1;i<3;i++){g.beginPath();g.moveTo(0,h*i/3);g.lineTo(w,h*i/3);g.stroke();}
  g.setLineDash([3,4]);g.strokeStyle=css('--mute');
  [d.floor,d.peak].forEach(v=>{g.beginPath();g.moveTo(0,y(v));g.lineTo(w,y(v));g.stroke();});
  g.setLineDash([6,4]);g.strokeStyle=css('--thr');g.lineWidth=2;
  g.beginPath();g.moveTo(0,y(d.thr));g.lineTo(w,y(d.thr));g.stroke();g.setLineDash([]);
  g.strokeStyle=css('--trace');g.lineWidth=2;g.lineJoin='round';g.beginPath();
  tr.forEach((v,i)=>i?g.lineTo(x(i),y(v)):g.moveTo(x(i),y(v)));g.stroke();
}

function render(d){
  $('prof').textContent=d.profile+' timing';
  if(d.rx)setState('Receiving','--accent');
  else if(!d.sig)setState(d.auto?'No signal':'Idle','--mute');
  else if(d.on)setState('Light detected','--warn');
  else setState('Listening','--ok');

  const latest=d.hist.find(h=>h.s==='ok');
  if(latest){$('msg').textContent=latest.m;}
  if(d.seq!==lastSeq){
    if(lastSeq>=0&&d.hist.length&&d.hist[0].s==='ok'){const m=$('msg');m.classList.remove('flash');void m.offsetWidth;m.classList.add('flash');}
    lastSeq=d.seq;
  }
  if(d.rx){
    $('rx').textContent='Receiving: '+d.partial+'▌  ('+d.got+(d.exp?'/'+d.exp:'')+' bytes)';
    $('prog').style.width=(d.exp?Math.min(100,d.got/d.exp*100):3)+'%';
  }else{
    $('rx').textContent=latest?'Received '+ago(d.up-latest.t)+' · '+latest.m.length+' chars in '+(latest.d/1000).toFixed(1)+' s':'';
    $('prog').style.width='0';
  }

  const err=d.chk+d.len+d.trunc;
  $('ok').textContent=d.ok;$('err').textContent=err;
  $('errd').textContent=err?`checksum ${d.chk} · length ${d.len} · cut ${d.trunc}`:'';
  $('q').textContent=d.q<0?'—':d.q+'%';
  $('q').style.color=d.q<0?'':css(d.q>=90?'--ok':d.q>=60?'--warn':'--bad');
  $('bps').textContent=d.bps>0?d.bps.toFixed(1):'—';
  $('thr').textContent=d.thr;
  $('mode').textContent=(d.auto?'auto':'manual (pot)')+' ± '+d.hyst;
  $('lag').textContent=d.stretch+' ms';
  $('adc').textContent='ADC '+d.adc+' · ambient '+d.floor+' · peak '+d.peak;

  const log=$('log');log.textContent='';
  d.hist.forEach(h=>{const tr=document.createElement('tr');
    const ok=h.s==='ok';
    tr.innerHTML='<td class="t"></td><td><span class="chip '+(ok?'c-ok':'c-bad')+'"></span></td><td class="m"></td>';
    tr.children[0].textContent=ago(d.up-h.t);tr.querySelector('.chip').textContent=h.s;
    tr.children[2].textContent=h.m||'(no data)';log.appendChild(tr);});
  $('empty').style.display=d.hist.length?'none':'block';
  draw(d);
}

async function poll(){
  try{const r=await fetch('/api/status',{cache:'no-store'});render(await r.json());fails=0;}
  catch(e){if(++fails>3)setState('Offline','--bad');}
  setTimeout(poll,250);
}
$('reset').onclick=()=>fetch('/api/reset',{method:'POST'}).then(()=>{lastSeq=-1;});
poll();
</script></body></html>
)rawliteral";
