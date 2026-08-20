#include "web_console.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

// 内嵌网页：复古金属控制台（拉丝金属表头 + 机械按键 + LED 指示灯 + LCD 表头）
static const char* HTML = R"HTML(<!DOCTYPE html>
<html lang="zh"><head><meta charset="utf-8"><title>Bridge 控制台</title>
<style>
*{box-sizing:border-box}
body{background:radial-gradient(ellipse at 50% -10%,#23272e 0%,#13161b 55%,#0b0d10 100%);color:#d8dde3;font-family:'Segoe UI',system-ui,sans-serif;margin:0;padding:18px;min-height:100vh}
.wrap{max-width:1180px;margin:0 auto}

/* ===== 拉丝金属表头 ===== */
.topbar{position:relative;background:
  repeating-linear-gradient(90deg,rgba(0,0,0,.045) 0 1px,transparent 1px 3px),
  linear-gradient(180deg,#ececec 0%,#d2d2d2 35%,#9e9e9e 62%,#c9c9c9 100%);
  border:1px solid #6f6f6f;border-radius:10px;padding:16px 22px;margin-bottom:16px;
  box-shadow:0 5px 14px rgba(0,0,0,.55),inset 0 1px 0 rgba(255,255,255,.85);
  display:flex;align-items:center;justify-content:space-between;flex-wrap:wrap;gap:10px}
.topbar .brand{color:#26262a;font-weight:800;font-size:26px;letter-spacing:7px;text-shadow:0 1px 0 rgba(255,255,255,.7);font-family:Consolas,monospace}
.topbar .sub{color:#5a5a5e;font-size:10px;letter-spacing:4px;margin-top:2px}
.screw{position:absolute;width:11px;height:11px;border-radius:50%;
  background:radial-gradient(circle at 35% 30%,#f7f7f7,#8f8f8f 55%,#555);box-shadow:0 1px 2px rgba(0,0,0,.65)}
.screw::after{content:'';position:absolute;left:2px;top:5px;width:7px;height:1.4px;background:#4c4c4c;transform:rotate(45deg)}
.s1{top:7px;left:9px}.s2{top:7px;right:9px}.s3{bottom:7px;left:9px}.s4{bottom:7px;right:9px}

/* ===== 面板与表头（LCD） ===== */
.panel{background:linear-gradient(180deg,#3b4148,#2a2f35 70%,#262b31);border:1px solid #565c64;border-radius:9px;
  padding:12px 14px;margin-bottom:14px;box-shadow:0 4px 10px rgba(0,0,0,.5),inset 0 1px 0 rgba(255,255,255,.07)}
/* 模块铭牌：与表头相同的拉丝金属材质 + 两侧螺丝 */
.mtitle{position:relative;display:inline-block;
  background:repeating-linear-gradient(90deg,rgba(0,0,0,.05) 0 1px,transparent 1px 3px),
  linear-gradient(180deg,#ececec 0%,#d2d2d2 35%,#9e9e9e 62%,#c9c9c9 100%);
  border:1px solid #6f6f6f;border-radius:6px;padding:6px 30px 6px 26px;margin-bottom:10px;
  color:#26262a;font-weight:700;font-size:11px;letter-spacing:3px;font-family:Consolas,monospace;
  text-shadow:0 1px 0 rgba(255,255,255,.6);
  box-shadow:0 3px 8px rgba(0,0,0,.5),inset 0 1px 0 rgba(255,255,255,.8)}
.mtitle::before,.mtitle::after{content:'';position:absolute;top:50%;margin-top:-4px;width:8px;height:8px;border-radius:50%;
  background:radial-gradient(circle at 35% 30%,#f7f7f7,#8f8f8f 55%,#555);box-shadow:0 1px 2px rgba(0,0,0,.6)}
.mtitle::before{left:9px}
.mtitle::after{right:9px}
.grid{display:grid;grid-template-columns:repeat(6,1fr);gap:10px}
@media(max-width:1000px){.grid{grid-template-columns:repeat(4,1fr)}}
@media(max-width:640px){.grid{grid-template-columns:repeat(2,1fr)}}
.meter{background:linear-gradient(180deg,#454b52,#2c3138);border:2px solid #5b626a;border-radius:8px;
  padding:7px 12px 8px;box-shadow:inset 0 2px 6px rgba(0,0,0,.7)}
.meter .l{font-size:10px;letter-spacing:2px;color:#9aa1aa}
.meter .v{font-family:Consolas,monospace;font-size:19px;color:#ffd27f;text-shadow:0 0 9px rgba(255,190,80,.3);
  font-variant-numeric:tabular-nums;margin-top:3px;line-height:1.25}
.meter .s{font-size:10px;color:#6d747d;margin-top:1px}
.meter .v.warn{color:#ff8a5c}.meter .v.bad{color:#ff5c54}

/* ===== LED 状态条 ===== */
.ledrow{display:flex;gap:24px;flex-wrap:wrap;align-items:center}
.leditem{display:flex;align-items:center;gap:8px;font-size:12px;color:#b9c0c8;letter-spacing:1px;font-family:Consolas,monospace}
/* 金属表头上的指示灯标字：金属雕刻黑色（刻字浮雕：深黑字 + 底部高光） */
.topbar .leditem{font-size:11px;letter-spacing:2px;font-weight:800;color:#1b1b1f;
  text-shadow:0 1px 0 rgba(255,255,255,.65)}
.led{width:9px;height:9px;border-radius:50%;background:#353a41;box-shadow:inset 0 1px 2px #000;flex:none}
/* ===== 12AX7 复古电子管指示灯 ===== */
.tube{position:relative;display:inline-block;width:10px;height:26px;flex:none}
.tube .glass{position:absolute;inset:0;border-radius:5px 5px 3px 3px;border:1.5px solid rgba(190,205,220,.4);
  background:linear-gradient(180deg,rgba(255,255,255,.09),rgba(255,255,255,.02) 45%,rgba(255,255,255,.06));
  box-shadow:inset 0 0 5px rgba(0,0,0,.55)}
.tube .getter{position:absolute;top:2px;left:2px;right:2px;height:6px;border-radius:2px;
  background:linear-gradient(180deg,#a8b0b8,#565e66)}
.tube .fil{position:absolute;top:11px;width:2px;height:11px;border-radius:1px;
  background:linear-gradient(180deg,#8a5a28,#5a3a18);opacity:.55}
.tube .f1{left:2.5px}.tube .f2{right:2.5px}
.tube .pin{position:absolute;bottom:-4px;width:1.5px;height:4px;background:#5d646d}
.tube .p1{left:2.5px}.tube .p2{right:2.5px}
.tube.on .fil{opacity:1;background:linear-gradient(180deg,#ffe9b0,#ff7a2a);
  box-shadow:0 0 7px 1px rgba(255,140,40,.9)}
.tube.on .glass{box-shadow:inset 0 0 5px rgba(0,0,0,.55),0 0 9px rgba(255,150,50,.4)}
.tube.on.dim .fil{background:linear-gradient(180deg,#e8c98a,#b06a28);box-shadow:0 0 5px 1px rgba(255,150,60,.45)}
.tube.bad .fil{opacity:1;background:linear-gradient(180deg,#ff9a9a,#d02020);box-shadow:0 0 7px 1px rgba(220,50,50,.85)}
.tube.bad .glass{box-shadow:inset 0 0 5px rgba(0,0,0,.55),0 0 9px rgba(220,60,60,.35)}
.topbar .tube{width:8px;height:20px}
.topbar .tube .fil{top:9px;height:8px}
.topbar .tube .getter{height:5px}
.led.g{background:#3fb950;box-shadow:0 0 8px rgba(63,185,80,.85)}
.led.r{background:#f85149;box-shadow:0 0 8px rgba(248,81,73,.85)}
.led.a{background:#ffb84a;box-shadow:0 0 8px rgba(255,184,74,.85)}

/* ===== 示波窗 ===== */
canvas{background:#06080a;border:2px solid #5b626a;border-radius:8px;width:100%;height:220px;display:block;
  box-shadow:inset 0 3px 8px rgba(0,0,0,.85)}
#tip{position:absolute;background:rgba(20,22,26,.95);border:1px solid #565c64;border-radius:6px;padding:6px 10px;
  font-size:12px;display:none;pointer-events:none;z-index:5;line-height:1.7;font-family:Consolas,monospace}

/* ===== 机械按键 ===== */
.btn{font-family:inherit;font-size:12px;letter-spacing:1px;font-weight:700;color:#2c2c2c;
  background:linear-gradient(180deg,#f4f4f4 0%,#d0d0d0 45%,#a6a6a6 100%);
  border:1px solid #7a7a7a;border-radius:5px;padding:7px 13px;cursor:pointer;
  box-shadow:0 3px 0 #606060,inset 0 1px 0 rgba(255,255,255,.8);text-shadow:0 1px 0 rgba(255,255,255,.5);
  transition:transform .05s}
.btn:hover{filter:brightness(1.05)}
.btn:active{transform:translateY(3px);box-shadow:0 0 0 #606060,inset 0 2px 5px rgba(0,0,0,.38)}
.btn.on{color:#3a2a05;background:linear-gradient(180deg,#ffedb0 0%,#e9b74c 45%,#b9851f 100%);
  box-shadow:0 3px 0 #6d4d0d,inset 0 1px 0 rgba(255,255,255,.7);text-shadow:0 1px 0 rgba(255,255,255,.45)}
.btn.on::after{content:'';display:inline-block;width:6px;height:6px;border-radius:50%;background:#2ea043;
  margin-left:6px;box-shadow:0 0 5px #3fb950;vertical-align:1px}
.btn.big{padding:11px 22px;font-size:14px;letter-spacing:2px}
.btn.danger{color:#3d1a12;background:linear-gradient(180deg,#f8d7c8 0%,#d98a66 45%,#a8532f 100%);
  border-color:#7a4530;box-shadow:0 3px 0 #5e301d,inset 0 1px 0 rgba(255,255,255,.7);text-shadow:0 1px 0 rgba(255,255,255,.4)}
.btn.flash::after{content:'';display:inline-block;width:7px;height:7px;border-radius:50%;background:#2ea043;
  margin-left:7px;box-shadow:0 0 6px #3fb950;animation:blink .5s 3}
@keyframes blink{0%,100%{opacity:1}50%{opacity:.15}}

/* ===== 机械拨杆开关 ===== */
.ctlrow{display:flex;gap:18px;align-items:center;flex-wrap:wrap;margin-bottom:10px}
.ctlrow .lbl{font-size:12px;letter-spacing:2px;color:#9aa2ac;font-weight:600}
.switch{display:inline-flex;align-items:center;gap:8px;cursor:pointer;user-select:none}
.switch input{display:none}
.switch .track{width:46px;height:20px;background:linear-gradient(180deg,#23272c,#0e1114);border:1px solid #565c64;
  border-radius:4px;box-shadow:inset 0 2px 5px rgba(0,0,0,.85);position:relative}
.switch .knob{position:absolute;top:1px;left:2px;width:18px;height:16px;border-radius:3px;
  background:linear-gradient(180deg,#f2f2f2,#b7b7b7);box-shadow:0 1px 3px rgba(0,0,0,.7);transition:left .12s}
.switch input:checked + .track{background:linear-gradient(180deg,#33250c,#171006);border-color:#8a6d2b}
.switch input:checked + .track .knob{left:25px}
.switch .led{width:8px;height:8px;border-radius:50%;background:#3a4047;box-shadow:inset 0 1px 2px #000}
.switch input:checked + .track + .led{background:#3fb950;box-shadow:0 0 7px rgba(63,185,80,.9)}
.foot{margin-top:14px;text-align:center;font-size:10px;letter-spacing:3px;color:#5d646d}
</style></head><body>)HTML" R"HTML(
<div class="wrap">
<div class="topbar">
  <span class="screw s1"></span><span class="screw s2"></span><span class="screw s3"></span><span class="screw s4"></span>
  <div><div class="brand">BRIDGE</div><div class="sub">AUDIO RELAY CONSOLE</div></div>
  <div class="ledrow">
    <div class="leditem"><span class="tube on" id="h-pwr"><span class="glass"></span><span class="getter"></span><span class="fil f1"></span><span class="fil f2"></span><span class="pin p1"></span><span class="pin p2"></span></span>PWR</div>
    <div class="leditem"><span class="tube" id="h-asio"><span class="glass"></span><span class="getter"></span><span class="fil f1"></span><span class="fil f2"></span><span class="pin p1"></span><span class="pin p2"></span></span>ASIO</div>
    <div class="leditem"><span class="tube" id="h-cap"><span class="glass"></span><span class="getter"></span><span class="fil f1"></span><span class="fil f2"></span><span class="pin p1"></span><span class="pin p2"></span></span>CAPTURE</div>
  </div>
</div>

<div class="panel">
  <div class="mtitle">TELEMETRY</div>
  <div class="grid" id="meters"></div>
</div>

<div class="panel">
  <div class="mtitle">SYSTEM STATUS</div>
  <div class="ledrow" id="leds"></div>
</div>

<div class="panel">
  <div class="mtitle">WATERMARK SCOPE</div>
  <div style="position:relative"><canvas id="spark"></canvas><div id="tip"></div></div>
</div>

<div class="panel">
  <div class="mtitle">CONTROL DECK</div>
  <div class="ctlrow">
    <span class="lbl">水位下限</span>
    <span id="bank-floor">
      <button class="btn" data-v="4">4×</button><button class="btn" data-v="6">6×</button>
      <button class="btn" data-v="8">8×</button><button class="btn" data-v="12">12×</button>
      <button class="btn" data-v="16">16×</button>
    </span>
  </div>
  <div class="ctlrow">
    <span class="lbl">TPDF 抖动</span>
    <label class="switch"><input type="checkbox" id="dither"><span class="track"><span class="knob"></span></span><span class="led"></span></label>
    <span class="lbl" style="margin-left:14px">直通模式</span>
    <label class="switch"><input type="checkbox" id="passthrough"><span class="track"><span class="knob"></span></span><span class="led"></span></label>
  </div>
  <div class="ctlrow">
    <span class="lbl">观察窗口</span>
    <span id="bank-range">
      <button class="btn" data-v="60">1分</button><button class="btn" data-v="180">3分</button>
      <button class="btn" data-v="600">10分</button><button class="btn" data-v="1800">30分</button>
      <button class="btn" data-v="3600">1时</button><button class="btn" data-v="7200">2时</button>
    </span>
  </div>
  <div class="ctlrow">
    <span class="lbl">Y 刻度</span>
    <span id="bank-scale">
      <button class="btn" data-v="0">自动</button><button class="btn" data-v="2048">2048</button>
      <button class="btn" data-v="4096">4096</button><button class="btn" data-v="8192">8192</button>
      <button class="btn" data-v="16384">16384</button><button class="btn" data-v="32768">32768</button>
    </span>
  </div>
  <div class="ctlrow">
    <button class="btn big danger" id="rebuild">重 建 链 路</button>
    <button class="btn big" id="reset">重 置 统 计</button>
  </div>
</div>

<div class="foot">BRIDGE · PROCESS LOOPBACK RELAY · RME ADI-2 PRO</div>
</div>
<script>
var firstInit=true;
var range=600, scale=0;
function fmt(n){return n.toLocaleString()}
function setLed(el,on,amber){el.className='tube '+(on?(amber?'on dim':'on'):'bad')}
function tubeSpan(id){return '<span class="tube" id="'+id+'"><span class="glass"></span><span class="getter"></span><span class="fil f1"></span><span class="fil f2"></span><span class="pin p1"></span><span class="pin p2"></span></span>'}
function mtr(l,v,s,cls){return '<div class="meter"><div class="l">'+l+'</div><div class="v'+(cls?' '+cls:'')+'">'+v+'</div><div class="s">'+s+'</div></div>'}
async function pollStatus(){
  try{
    var r=await fetch('/api/status');var s=await r.json();
    var upTxt=(s.uptime/60000).toFixed(1)+' 分钟';
    var rlTxt=s.inRate?(s.ratioBase-1>0?'+':'-')+Math.abs((s.ratioBase-1)*1e6).toFixed(1)+' ppm':'—';
    var tgt=s.targetPid?('PID '+s.targetPid+(s.targetActive?' · 活跃':' · 静默')):'发现中…';
    var wmCls=s.watermark<1024?'bad':(s.watermark>s.target*1.5?'warn':'');
    document.getElementById('meters').innerHTML=
      mtr('水位',fmt(s.watermark),'采样',wmCls)+
      mtr('目标',fmt(s.target),'采样')+
      mtr('下限',s.floor+'×','倍数')+
      mtr('欠载',fmt(s.underruns),'累计',s.underruns>0?'warn':'')+
      mtr('峰值',s.peak.toFixed(3),'FS',s.peak>0.999?'bad':'')+
      mtr('采集',s.capRate+' Hz','Bridge')+
      mtr('ASIO',s.asioRate+' Hz',s.asioBuffer+' 帧')+
      mtr('运行',upTxt,'')+
      mtr('实测输入',s.inRate?s.inRate.toFixed(1)+' Hz':'—','')+
      mtr('实测输出',s.outRate?s.outRate.toFixed(1)+' Hz':'—','')+
      mtr('速率锁',rlTxt,'')+
      mtr('目标进程',tgt,'');
    setLed(document.getElementById('h-asio'),s.asioRate>0);
    setLed(document.getElementById('h-cap'),s.targetActive,s.targetPid&&!s.targetActive);
    var ledTpl='<div class="leditem"><span class="led" id="ledN"></span>LABEL</div>';
    document.getElementById('leds').innerHTML=
      '<div class="leditem">'+tubeSpan('l-chain')+'采样率链一致</div>'+
      '<div class="leditem">'+tubeSpan('l-lock')+'速率锁有效</div>'+
      '<div class="leditem">'+tubeSpan('l-under')+'无欠载</div>'+
      '<div class="leditem">'+tubeSpan('l-clip')+'无削波</div>'+
      '<div class="leditem">'+tubeSpan('l-active')+'目标活跃</div>';
    setLed(document.getElementById('l-chain'),s.capRate===s.asioRate);
    setLed(document.getElementById('l-lock'),s.inRate>0);
    setLed(document.getElementById('l-under'),s.underruns===0);
    setLed(document.getElementById('l-clip'),s.peak<0.999);
    setLed(document.getElementById('l-active'),s.targetActive);
    if(firstInit){
      firstInit=false;
      document.getElementById('dither').checked=!!s.dither;
      document.getElementById('passthrough').checked=!!s.passthrough;
      setBank('bank-floor',String(s.floor));
      setBank('bank-range',String(range));
      setBank('bank-scale',String(scale));
    }
  }catch(e){}
}
function setBank(id,val){
  var b=document.getElementById(id);
  for(var i=0;i<b.children.length;i++){
    var c=b.children[i];
    if(c.getAttribute('data-v')===val)c.classList.add('on');else c.classList.remove('on');
  }
}
function bindBank(id,fn){
  var b=document.getElementById(id);
  for(var i=0;i<b.children.length;i++)(function(btn){
    btn.onclick=function(){
      setBank(id,btn.getAttribute('data-v'));
      fn(+btn.getAttribute('data-v'));
    };
  })(b.children[i]);
}
bindBank('bank-floor',function(v){ctl('action=floor&value='+v)});
bindBank('bank-range',function(v){range=v;draw()});
bindBank('bank-scale',function(v){scale=v;draw()});
function ctl(body){fetch('/api/control',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:body})}
document.getElementById('dither').onchange=function(e){ctl('action=dither&value='+(e.target.checked?1:0))}
document.getElementById('passthrough').onchange=function(e){ctl('action=passthrough&value='+(e.target.checked?1:0))}
document.getElementById('rebuild').onclick=function(){this.classList.add('flash');setTimeout(function(){document.getElementById('rebuild').classList.remove('flash')},1600);ctl('action=rebuild&value=1')}
document.getElementById('reset').onclick=function(){this.classList.add('flash');setTimeout(function(){document.getElementById('reset').classList.remove('flash')},1600);ctl('action=reset&value=1')}

async function draw(){
  try{
    var r=await fetch('/api/history?range='+range);var s=await r.json();
    var pts=s.points;
    var cv=document.getElementById('spark'),ctx=cv.getContext('2d');
    var dpr=window.devicePixelRatio||1;
    var rect=cv.getBoundingClientRect();
    if(cv.width!==Math.round(rect.width*dpr)){cv.width=Math.round(rect.width*dpr);cv.height=Math.round(rect.height*dpr);}
    var W=rect.width,H=rect.height;ctx.setTransform(dpr,0,0,dpr,0,0);
    ctx.clearRect(0,0,W,H);
    var mx=scale;
    if(!mx){ mx=1024; for(var i=0;i<pts.length;i++){ if(pts[i][1]>mx)mx=pts[i][1]; if(pts[i][2]>mx)mx=pts[i][2]; } }
    var Y=function(v){return H-(v/mx)*H}
    var px=function(i){return pts.length<2?W:i/(pts.length-1)*W}
    var tg=pts.length?pts[pts.length-1][2]:0;
    var danger=1024;
    if(danger<mx){ctx.fillStyle='rgba(248,81,73,0.08)';ctx.fillRect(0,Y(danger),W,H-Y(danger));}
    var hi=Math.min(tg*1.5,mx), lo=Math.min(danger,mx);
    if(hi>lo){ctx.fillStyle='rgba(63,185,80,0.08)';ctx.fillRect(0,Y(hi),W,Y(lo)-Y(hi));}
    ctx.strokeStyle='rgba(255,184,74,0.75)';ctx.setLineDash([5,5]);ctx.lineWidth=1;
    ctx.beginPath();ctx.moveTo(0,Y(tg));ctx.lineTo(W,Y(tg));ctx.stroke();ctx.setLineDash([]);
    if(pts.length>1){
      ctx.beginPath();ctx.moveTo(px(0),Y(pts[0][1]));
      for(var i=1;i<pts.length;i++)ctx.lineTo(px(i),Y(pts[i][1]));
      var g=ctx.createLinearGradient(0,0,0,H);
      g.addColorStop(0,'rgba(255,190,80,0.22)');g.addColorStop(1,'rgba(255,190,80,0)');
      ctx.strokeStyle='#ffbe50';ctx.lineWidth=1.6;ctx.stroke();
      ctx.lineTo(W,H);ctx.lineTo(px(0),H);ctx.closePath();ctx.fillStyle=g;ctx.fill();
      var lv=pts[pts.length-1][1];
      ctx.beginPath();ctx.arc(W,Y(lv),6,0,6.283);ctx.fillStyle='rgba(255,190,80,0.25)';ctx.fill();
      ctx.beginPath();ctx.arc(W,Y(lv),3,0,6.283);ctx.fillStyle='#ffd27f';ctx.fill();
      ctx.fillStyle='#b9c0c8';ctx.font='12px Consolas,monospace';
      ctx.fillText(lv.toLocaleString(),W-64,Math.max(12,Y(lv)-8));
    }
    if(hoverX>=0 && pts.length>1){
      var i=Math.round(hoverX/W*(pts.length-1));i=Math.max(0,Math.min(pts.length-1,i));
      var x=px(i),p=pts[i];
      ctx.strokeStyle='rgba(154,162,172,0.6)';ctx.setLineDash([3,3]);
      ctx.beginPath();ctx.moveTo(x,0);ctx.lineTo(x,H);ctx.stroke();ctx.setLineDash([]);
      var tip=document.getElementById('tip');
      tip.style.display='block';
      tip.style.left=Math.min(W-178,x+10)+'px';
      tip.style.top='10px';
      tip.innerHTML='<b>'+fmtAge(pts[pts.length-1][0]-p[0])+'</b><br>水位 '+p[1].toLocaleString()+'<br>目标 '+p[2].toLocaleString();
    }else{var t2=document.getElementById('tip');if(t2)t2.style.display='none';}
    var last=pts.length?pts[pts.length-1][1]:0;
    var st,sc;
    if(last<danger){st='水位过低';sc='#f85149'}
    else if(last>tg*1.5){st='水位偏高';sc='#ffb84a'}
    else{st='水位健康';sc='#3fb950'}
    ctx.fillStyle='rgba(10,12,14,0.8)';
    roundRect(ctx,8,8,st.length*14+46,24,12);ctx.fill();
    ctx.fillStyle=sc;ctx.beginPath();ctx.arc(20,20,4,0,6.283);ctx.fill();
    ctx.fillStyle='#d8dde3';ctx.font='12px Segoe UI';ctx.fillText(st,32,24);
    ctx.fillStyle='#6d747d';ctx.font='11px Consolas,monospace';
    ctx.fillText('-'+(range/60).toFixed(0)+' 分钟',8,H-8);
    ctx.fillText('现在',W-36,H-8);
    ctx.fillText('0',W-14,12);
    ctx.fillText(mx.toLocaleString(),W-62,12);
  }catch(e){}
}
function roundRect(ctx,x,y,w,h,r){ctx.beginPath();ctx.moveTo(x+r,y);ctx.arcTo(x+w,y,x+w,y+h,r);ctx.arcTo(x+w,y+h,x,y+h,r);ctx.arcTo(x,y+h,x,y,r);ctx.arcTo(x,y,x+w,y,r);ctx.closePath();}
function fmtAge(sec){if(sec<60)return sec+' 秒前';if(sec<3600)return Math.floor(sec/60)+' 分钟前';return (sec/3600).toFixed(1)+' 小时前';}
var hoverX=-1;
document.getElementById('spark').onmousemove=function(e){var r=e.target.getBoundingClientRect();hoverX=e.clientX-r.left;draw()}
document.getElementById('spark').onmouseleave=function(){hoverX=-1;draw()}
window.onresize=function(){draw()}
setInterval(function(){pollStatus();draw()},2000);
pollStatus();draw();
</script></body></html>
)HTML";

static SOCKET g_listen = INVALID_SOCKET;
static std::thread g_thread;
static BridgeStatsPtrs g_p;
static ULONGLONG g_startTick = 0;
static std::atomic<bool> g_httpStop{false};

static void sendResponse(SOCKET s, const char* status, const char* contentType, const char* body, int len) {
    char hdr[512];
    int hlen = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %d\r\nConnection: close\r\nCache-Control: no-store\r\n\r\n",
        status, contentType, len);
    send(s, hdr, hlen, 0);
    send(s, body, len, 0);
}

static void handleRequest(SOCKET s, char* req, int n) {
    (void)n;
    char method[8] = {0}, path[256] = {0};
    sscanf(req, "%7s %255s", method, path);

    if (strcmp(path, "/api/status") == 0) {
        unsigned long long w = g_p.written->load(std::memory_order_relaxed);
        unsigned long long c = g_p.consumed->load(std::memory_order_relaxed);
        unsigned long long d = g_p.dropped->load(std::memory_order_relaxed);
        unsigned long long u = g_p.underruns->load(std::memory_order_relaxed);
        unsigned long long wm = g_p.wmNow->load(std::memory_order_relaxed);   // 真实水位
        size_t wmMult = g_p.wMult->load(std::memory_order_relaxed);
        size_t floorM = g_p.floorMult->load(std::memory_order_relaxed);
        long buf = g_p.asioBuffer->load(std::memory_order_relaxed);
        unsigned long long target = (unsigned long long)wmMult * (unsigned long long)buf * 2ull;
        char body[1024];
        int len = snprintf(body, sizeof(body),
            "{\"watermark\":%llu,\"target\":%llu,\"floor\":%llu,\"wmult\":%llu,"
            "\"underruns\":%llu,\"dropped\":%llu,\"peak\":%.3f,\"drift\":%.2f,"
            "\"written\":%llu,\"consumed\":%llu,"
            "\"asioRate\":%ld,\"asioBuffer\":%ld,\"asioType\":%ld,\"capRate\":%u,\"uptime\":%llu,"
            "\"dither\":%d,"
            "\"ratioBase\":%.7f,\"inRate\":%.1f,\"outRate\":%.1f,\"passthrough\":%d,"
            "\"targetPid\":%u,\"targetActive\":%d}",
            wm, target, (unsigned long long)floorM, (unsigned long long)wmMult,
            u, d, (double)g_p.peak->load(std::memory_order_relaxed),
            (double)g_p.driftPpm->load(std::memory_order_relaxed),
            w, c, (long)g_p.asioRate->load(std::memory_order_relaxed), buf,
            (long)g_p.asioType->load(std::memory_order_relaxed),
            (unsigned)g_p.capRate->load(std::memory_order_relaxed),
            (unsigned long long)(GetTickCount64() - g_startTick),
            g_p.ditherOn->load(std::memory_order_relaxed) ? 1 : 0,
            (double)g_p.ratioBase->load(std::memory_order_relaxed),
            (double)g_p.inRate->load(std::memory_order_relaxed),
            (double)g_p.outRate->load(std::memory_order_relaxed),
            g_p.passthrough->load(std::memory_order_relaxed) ? 1 : 0,
            (unsigned)g_p.targetPid->load(std::memory_order_relaxed),
            g_p.targetActive->load(std::memory_order_relaxed) ? 1 : 0);
        sendResponse(s, "200 OK", "application/json; charset=utf-8", body, len);
    } else if (strcmp(path, "/api/control") == 0 && strcmp(method, "POST") == 0) {
        char* body = strstr(req, "\r\n\r\n");
        if (body) {
            body += 4;
            char* vp = strstr(body, "value=");
            int v = vp ? atoi(vp + 6) : 0;
            if (strstr(body, "action=floor")) {
                if (v < 2) v = 2;
                if (v > 32) v = 32;
                g_p.floorMult->store((size_t)v, std::memory_order_relaxed);
            } else if (strstr(body, "action=dither")) {
                g_p.ditherReq->store(v != 0, std::memory_order_relaxed);
                g_p.needRestart->store(true);   // 重建后生效
            } else if (strstr(body, "action=passthrough")) {
                g_p.passthroughReq->store(v != 0 ? 1 : 2, std::memory_order_relaxed);
            } else if (strstr(body, "action=rebuild")) {
                g_p.needRestart->store(true);
            } else if (strstr(body, "action=reset")) {
                g_p.resetReq->store(true);
            }
        }
        const char* okBody = "{\"ok\":true}";
        sendResponse(s, "200 OK", "application/json", okBody, (int)strlen(okBody));
    } else if (strncmp(path, "/api/history", 12) == 0) {
        int rng = 600;
        char* q = strstr(path, "range=");
        if (q) rng = atoi(q + 6);
        if (rng < 30) rng = 30;
        if (rng > 7200) rng = 7200;
        const uint64_t nowSec = GetTickCount64() / 1000;
        const uint64_t startSec = nowSec - (uint64_t)rng;
        const uint64_t w = g_p.histWrite->load(std::memory_order_acquire);
        const uint64_t oldest = (w >= kHistCap) ? (w - kHistCap) : 0;
        std::vector<HistPoint> sel;
        sel.reserve(720);
        for (uint64_t i = oldest; i < w; ++i) {
            const HistPoint& p = (*g_p.histBuf)[i % kHistCap];
            if (p.t < startSec || p.t > nowSec) continue;
            sel.push_back(p);
        }
        // 抽稀到 ≤600 点
        size_t stride = sel.size() > 600 ? (sel.size() + 599) / 600 : 1;
        std::string body = "{\"points\":[";
        size_t cnt = 0;
        for (size_t i = 0; i < sel.size(); i += stride) {
            char tmp[64];
            snprintf(tmp, sizeof(tmp), "%s[%u,%u,%u]", cnt ? "," : "",
                     sel[i].t, sel[i].wm, sel[i].target);
            body += tmp;
            ++cnt;
        }
        body += "]}";
        sendResponse(s, "200 OK", "application/json; charset=utf-8", body.c_str(), (int)body.size());
    } else {
        sendResponse(s, "200 OK", "text/html; charset=utf-8", HTML, (int)strlen(HTML));
    }
}

static void httpThread() {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        printf("[控制台] WSAStartup 失败\n");
        return;
    }
    g_listen = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (g_listen == INVALID_SOCKET) { WSACleanup(); return; }
    BOOL reuse = 1;
    setsockopt(g_listen, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    addr.sin_port = htons(3999);
    if (bind(g_listen, (sockaddr*)&addr, sizeof(addr)) != 0) {
        printf("[控制台] 绑定 127.0.0.1:3999 失败（端口被占用？）\n");
        closesocket(g_listen);
        WSACleanup();
        return;
    }
    listen(g_listen, 4);
    printf("[控制台] 已启动: http://127.0.0.1:3999\n");

    while (!g_httpStop.load()) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(g_listen, &fds);
        timeval tv = {0, 200000};
        int sel = select(0, &fds, nullptr, nullptr, &tv);
        if (sel <= 0) continue;
        SOCKET c = accept(g_listen, nullptr, nullptr);
        if (c == INVALID_SOCKET) continue;
        // 循环读取完整请求（headers + body），避免体包落在关闭后触发 RST
        char req[4096];
        int total = 0;
        bool done = false;
        while (!done && total < (int)sizeof(req) - 1) {
            int n = recv(c, req + total, sizeof(req) - 1 - total, 0);
            if (n <= 0) break;
            total += n;
            req[total] = 0;
            const char* hdrEnd = strstr(req, "\r\n\r\n");
            if (!hdrEnd) continue;
            if (strncmp(req, "POST", 4) == 0) {
                const char* cl = strstr(req, "Content-Length:");
                int bodyLen = cl ? atoi(cl + 15) : 0;
                done = (int)((hdrEnd + 4 - req) + bodyLen) <= total;
            } else {
                done = true;
            }
        }
        if (total > 0) handleRequest(c, req, total);
        closesocket(c);
    }
    closesocket(g_listen);
    WSACleanup();
}

bool startWebConsole(const BridgeStatsPtrs& p, unsigned short port) {
    (void)port;
    g_p = p;
    g_startTick = GetTickCount64();   // 进程内 uptime 起点
    g_httpStop.store(false);
    g_thread = std::thread(httpThread);
    return true;
}

void stopWebConsole() {
    g_httpStop.store(true);
    if (g_thread.joinable()) g_thread.join();
}
