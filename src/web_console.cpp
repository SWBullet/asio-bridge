#include "web_console.h"
#include "device_scan.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <thread>
#include <vector>

// wstring → UTF-8 JSON 安全串(转义引号/反斜杠)
static std::string ws2json(const std::wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    std::string out;
    for (char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    return out;
}

// 内嵌网页：复古金属控制台（拉丝金属表头 + 机械按键 + LED 指示灯 + LCD 表头）
static const char* HTML = R"HTML(<!DOCTYPE html>
<html lang="zh"><head><meta charset="utf-8"><title>Bridge 控制台</title>
<style>
*{box-sizing:border-box}
body{background:radial-gradient(ellipse at 50% -10%,#23272e 0%,#13161b 55%,#0b0d10 100%);color:#d8dde3;font-family:'Segoe UI',system-ui,sans-serif;margin:0;padding:18px;min-height:100vh}
.wrap{max-width:1180px;margin:0 auto}

/* ===== 拉丝金属表头 ===== */
.topbar{position:relative;background:
  repeating-linear-gradient(0deg,rgba(0,0,0,.045) 0 1px,transparent 1px 3px),
  linear-gradient(180deg,#ececec 0%,#d2d2d2 35%,#9e9e9e 62%,#c9c9c9 100%);
  border:1px solid #6f6f6f;border-radius:10px;padding:16px 22px;margin-bottom:16px;
  box-shadow:0 5px 14px rgba(0,0,0,.55),inset 0 1px 0 rgba(255,255,255,.85);
  display:flex;align-items:center;justify-content:space-between;flex-wrap:wrap;gap:10px}
.topbar .brand{color:#2a2a2e;font-weight:400;font-size:30px;letter-spacing:5px;text-shadow:0 1px 0 rgba(255,255,255,.75);font-family:"方正瘦金书","汉仪瘦金体","华康瘦金体","STKaiti","KaiTi","楷体",serif}
.topbar .sub{color:#5a5a5e;font-size:10px;letter-spacing:4px;margin-top:2px}
.screw{position:absolute;width:11px;height:11px;border-radius:50%;
  background:radial-gradient(circle at 35% 30%,#f7f7f7,#8f8f8f 55%,#555);box-shadow:0 1px 2px rgba(0,0,0,.65)}
.screw::after{content:'';position:absolute;left:2px;top:5px;width:7px;height:1.4px;background:#4c4c4c;transform:rotate(45deg)}
.s1{top:7px;left:9px}.s2{top:7px;right:9px}.s3{bottom:7px;left:9px}.s4{bottom:7px;right:9px}

/* ===== 面板与表头（LCD） ===== */
.panel{position:relative;background:
  repeating-linear-gradient(0deg,rgba(255,255,255,.04) 0 1px,rgba(0,0,0,.05) 1px 2px,transparent 2px 3px),
  linear-gradient(180deg,#414850,#2c3138 70%,#272c33);
  border:1px solid #565c64;border-radius:9px;
  padding:12px 14px;margin-bottom:14px;
  box-shadow:0 4px 10px rgba(0,0,0,.5),inset 0 1px 0 rgba(255,255,255,.07)}
/* 机架安装：四角螺丝（金属钉头 + 高光） */
.panel::before{content:'';position:absolute;inset:0;pointer-events:none;border-radius:9px;
  background:
    radial-gradient(circle at 9px 9px,#14171b 0 3px,#6a717a 3.5px 5px,transparent 5.5px),
    radial-gradient(circle at calc(100% - 9px) 9px,#14171b 0 3px,#6a717a 3.5px 5px,transparent 5.5px),
    radial-gradient(circle at 9px calc(100% - 9px),#14171b 0 3px,#6a717a 3.5px 5px,transparent 5.5px),
    radial-gradient(circle at calc(100% - 9px) calc(100% - 9px),#14171b 0 3px,#6a717a 3.5px 5px,transparent 5.5px)}
/* 模块铭牌：与表头相同的拉丝金属材质 + 两侧螺丝 */
.mtitle{position:relative;display:inline-block;
  background:repeating-linear-gradient(0deg,rgba(0,0,0,.05) 0 1px,transparent 1px 3px),
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
/* ===== 复古正圆指针表头（2 列 × 2 排） ===== */
.gauge2{grid-column:span 2;grid-row:span 2;position:relative;display:flex;align-items:center;justify-content:center;padding:4px}
.gauge2 canvas{width:100%;height:auto;aspect-ratio:1/1;display:block}
/* 副栏：其余 9 项以细条小块展示 */
.substrip{display:flex;flex-wrap:wrap;gap:6px 14px;margin-top:10px;padding:6px 4px;border-top:1px solid #3a4048}
.substrip .chip{font-family:Consolas,monospace;font-size:11px;color:#9aa1aa;white-space:nowrap}
.substrip .chip b{color:#ffd27f;font-weight:600;text-shadow:0 0 5px rgba(255,190,80,.25)}

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
#wf{background:#06080a;border:2px solid #5b626a;border-radius:8px;width:100%;height:180px;display:block;
  box-shadow:inset 0 3px 8px rgba(0,0,0,.85)}
#histcv{background:#06080a;border:2px solid #5b626a;border-radius:8px;width:100%;height:200px;display:block;
  box-shadow:inset 0 3px 8px rgba(0,0,0,.85)}
.wflegend{font-size:11px;color:#8b939c;margin-top:6px;font-family:Consolas,monospace;letter-spacing:.5px}

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

/* ===== 复古导弹发射按键（正圆拇指肚大小） ===== */
.launchwrap{display:inline-flex;flex-direction:column;align-items:center;gap:6px;margin:0 8px}
.launchbtn{position:relative;width:60px;height:60px;border-radius:50%;cursor:pointer;padding:0;flex:none;
  border:2px solid #4a5056;
  background:radial-gradient(circle at 40% 30%,#eef0f2,#aeb3b9 55%,#6b727a 100%);
  box-shadow:0 6px 13px rgba(0,0,0,.65),inset 0 1px 1px rgba(255,255,255,.9),inset 0 -2px 4px rgba(0,0,0,.35);
  transition:transform .05s}
.launchbtn:active{transform:translateY(3px)}
.launchbtn .face{position:absolute;inset:10px;border-radius:50%;
  background:radial-gradient(circle at 38% 28%,#7d858e,#33383f 72%);
  box-shadow:inset 0 4px 9px rgba(0,0,0,.85),0 1px 0 rgba(255,255,255,.22)}
.launchbtn.on .face{background:radial-gradient(circle at 38% 28%,#ff9a8a,#d02020 55%,#7a0e0e 100%);
  box-shadow:inset 0 4px 9px rgba(0,0,0,.55),0 0 16px 4px rgba(248,81,73,.8)}
.launchbtn.amber .face{background:radial-gradient(circle at 38% 28%,#ffe9b0,#e9a23c 55%,#9a6a1a 100%);
  box-shadow:inset 0 4px 9px rgba(0,0,0,.55),0 0 12px 3px rgba(255,184,74,.6)}
.launchbtn .tag{position:absolute;left:0;right:0;top:50%;transform:translateY(-50%);text-align:center;
  font-family:Consolas,monospace;font-size:8px;font-weight:700;letter-spacing:1px;color:#111318;pointer-events:none}
.launchbtn.on .tag,.launchbtn.amber .tag{color:#fff7e8;text-shadow:0 1px 2px rgba(0,0,0,.6)}
.lblcap{font-family:Consolas,monospace;font-size:10px;letter-spacing:2px;color:#9aa2ac;font-weight:600;white-space:nowrap}

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
  <div><div class="brand">文超工作室</div><div class="sub">AUDIO RELAY CONSOLE</div></div>
  <div class="ledrow">
    <div class="leditem"><span class="tube on" id="h-pwr"><span class="glass"></span><span class="getter"></span><span class="fil f1"></span><span class="fil f2"></span><span class="pin p1"></span><span class="pin p2"></span></span>PWR</div>
    <div class="leditem"><span class="tube" id="h-asio"><span class="glass"></span><span class="getter"></span><span class="fil f1"></span><span class="fil f2"></span><span class="pin p1"></span><span class="pin p2"></span></span>ASIO</div>
    <div class="leditem"><span class="tube" id="h-cap"><span class="glass"></span><span class="getter"></span><span class="fil f1"></span><span class="fil f2"></span><span class="pin p1"></span><span class="pin p2"></span></span>CAPTURE</div>
  </div>
</div>

<div class="panel">
  <div class="mtitle">TELEMETRY</div>
  <div class="grid" id="meters">
    <div class="gauge2"><canvas id="g-peak"></canvas></div>
    <div class="gauge2"><canvas id="g-wm"></canvas></div>
    <div class="gauge2"><canvas id="g-lat"></canvas></div>
  </div>
  <div class="substrip" id="substrip"></div>
</div>

<div class="panel">
  <div class="mtitle">WATERMARK HISTORY · 水位历史</div>
  <div style="position:relative"><canvas id="histcv"></canvas></div>
  <div class="wflegend"><span style="color:#ffd27f">■ 水位</span> <span style="color:#ffb84a">┄ 目标水位</span> <span style="color:#6d747d">观察窗口 10 分钟 · Y 自动</span></div>
</div>

<div class="panel">
  <div class="mtitle">SYSTEM STATUS</div>
  <div class="ledrow">
    <div class="leditem"><span class="tube" id="l-chain"><span class="glass"></span><span class="getter"></span><span class="fil f1"></span><span class="fil f2"></span><span class="pin p1"></span><span class="pin p2"></span></span>采样率链一致</div>
    <div class="leditem"><span class="tube" id="l-lock"><span class="glass"></span><span class="getter"></span><span class="fil f1"></span><span class="fil f2"></span><span class="pin p1"></span><span class="pin p2"></span></span>锁频锁定</div>
    <div class="leditem"><span class="tube" id="l-under"><span class="glass"></span><span class="getter"></span><span class="fil f1"></span><span class="fil f2"></span><span class="pin p1"></span><span class="pin p2"></span></span>无欠载</div>
    <div class="leditem"><span class="tube" id="l-clip"><span class="glass"></span><span class="getter"></span><span class="fil f1"></span><span class="fil f2"></span><span class="pin p1"></span><span class="pin p2"></span></span>无削波</div>
    <div class="leditem"><span class="tube" id="l-drift"><span class="glass"></span><span class="getter"></span><span class="fil f1"></span><span class="fil f2"></span><span class="pin p1"></span><span class="pin p2"></span></span>时钟漂移 &lt;100ppm</div>
  </div>
</div>

<div class="panel">
  <div class="mtitle">300B SPECTRUM · 谐波叠加</div>
  <div style="position:relative"><canvas id="wf"></canvas></div>
  <div class="wflegend"><span style="color:#ffbe50">■ 音乐原始</span> <span style="color:#4dd8e6">■ 新增谐波</span> <span style="margin-left:14px">谐波增益</span> <input type="range" id="harmgain" min="0" max="60" value="0" style="width:140px"> <span id="harmgain-v" style="min-width:44px">0 dB</span></div>
</div>

<div class="panel">
  <div class="mtitle">CONTROL DECK</div>
  <div class="ctlrow">
    <span class="lbl">输出设备</span>
    <select id="bank-device" style="background:#1e2226;color:#d8dde3;border:1px solid #565c64;border-radius:5px;padding:6px 8px;font-size:12px;min-width:290px;font-family:inherit"></select>
    <button class="btn" id="devscan">刷新</button>
  </div>
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
  </div>
  <div class="ctlrow">
    <span class="lbl">重采样质量</span>
    <span id="bank-src">
      <button class="btn" data-v="0">线性低延迟</button>
      <button class="btn" data-v="32">sinc 高精度</button>
    </span>
  </div>
  <div class="ctlrow">
    <span class="lbl">300B 染色</span>
    <label class="switch"><input type="checkbox" id="tube"><span class="track"><span class="knob"></span></span><span class="led"></span></label>
    <span class="lbl" style="margin-left:12px">暖度</span>
    <input type="range" id="tubewarmth" min="0" max="100" value="30" style="width:150px">
    <span id="tubewarmth-v" style="min-width:34px">30%</span>
  </div>
  <div class="ctlrow">
    <span class="lbl">厚度与宽度</span>
    <label class="switch"><input type="checkbox" id="thickness"><span class="track"><span class="knob"></span></span><span class="led"></span></label>
    <span class="lbl" style="margin-left:12px">厚度</span>
    <input type="range" id="thickness-delay" min="2" max="100" value="20" style="width:170px">
  </div>
  <div class="ctlrow">
    <span class="lbl">火箭推进器</span>
    <label class="switch"><input type="checkbox" id="booster"><span class="track"><span class="knob"></span></span><span class="led"></span></label>
    <span class="lbl" style="margin-left:12px">增益</span>
    <input type="range" id="booster-db" min="0" max="100" value="67" style="width:140px">
    <span id="booster-db-v" style="min-width:44px">67%</span>
  </div>
  <div class="ctlrow">
    <span class="lbl">宽度</span>
    <span id="bank-width">
      <button class="btn" data-v="0">关闭</button>
      <button class="btn" data-v="1">俱乐部</button>
      <button class="btn" data-v="2">音乐厅</button>
      <button class="btn" data-v="3">太和殿</button>
    </span>
  </div>
  <div class="ctlrow">
    <div class="launchwrap">
      <button class="launchbtn on" id="bridge"><span class="face"></span><span class="tag">BRIDGE</span></button>
      <span class="lblcap">ASIO Bridge</span>
    </div>
    <div class="launchwrap">
      <button class="launchbtn amber" id="reset"><span class="face"></span><span class="tag">RESET</span></button>
      <span class="lblcap">重置统计</span>
    </div>
  </div>
</div>

<div id="updbar" style="display:none;margin:14px 0 8px;padding:9px 14px;background:linear-gradient(180deg,#262a2f,#171a1e);
  border:1px solid #565c64;border-radius:6px;font-size:12px;color:#aab2bc;align-items:center;gap:10px;flex-wrap:wrap">
  <span id="updmsg">检查更新</span>
  <span style="flex:1"></span>
  <button class="btn" id="updcheck" style="display:none;padding:4px 12px">检查更新</button>
  <button class="btn" id="updgo" style="display:none;padding:4px 12px;background:linear-gradient(180deg,#8a2b2b,#571717);border-color:#c05050">下载并升级</button>
</div>

<div class="foot">BRIDGE · PROCESS LOOPBACK RELAY</div>
</div>
<script>
var firstInit=true;
var CSRF='';   // CSRF Token（由服务端在页面尾部注入，POST 控制接口必须携带）
function fmt(n){return n.toLocaleString()}
function setLed(el,on,amber){el.className='tube '+(on?(amber?'on dim':'on'):'bad')}
function chip(l,v){return '<span class="chip">'+l+' <b>'+v+'</b></span>'}
/* ===== 三块复古正圆指针表头（麦景图式精工） ===== */
var gPeak={cur:0,tgt:0,min:0,max:1,danger:0.85};
var gWm={cur:0,tgt:0,min:0,max:4096,mark:4096,danger:1024};
var gLat={cur:0,tgt:0,min:0,max:300,danger:200};
function gAng(v,o){var t=(v-o.min)/(o.max-o.min);t=t<0?0:(t>1?1:t);return Math.PI*0.75+t*Math.PI*1.5}
function gTick(o,i){
  var v=o.min+(o.max-o.min)*i/5;
  if(o===gPeak)return v.toFixed(1);
  if(o===gLat)return Math.round(v);
  return Math.round(v);   // 水位刻度：整数表示（不用小数/小数 k）
}
// 分区色界：峰值 0–0.85 绿 / 0.85–1 红；延迟 0–100 绿 / 100–200 琥珀 / 200–300 红；
// 水位 低于危险值红 / 危险值–目标绿 / 高于目标琥珀
function zoneBounds(o){
  if(o===gPeak)return[{to:0.85,c:'#7dd07d'},{to:1,c:'#f85149'}];
  if(o===gLat)return[{to:100,c:'#7dd07d'},{to:200,c:'#ffb84a'},{to:300,c:'#f85149'}];
  var b=[];
  if(o.danger>o.min)b.push({to:o.danger,c:'#f85149'});
  if(o.mark>o.min&&o.mark>o.danger)b.push({to:o.mark,c:'#7dd07d'});
  if(o.max>o.mark)b.push({to:o.max,c:'#ffb84a'});
  return b;
}
function zoneColor(o,v){
  var b=zoneBounds(o);
  for(var i=0;i<b.length;i++)if(v<=b[i].to)return b[i].c;
  return b.length?b[b.length-1].c:'#ffd27f';
}
)HTML" R"HTML(function drawDial(id,o,label,valText){
  var cv=document.getElementById(id);if(!cv)return;
  var ctx=cv.getContext('2d');
  var dpr=window.devicePixelRatio||1;
  var w=cv.clientWidth, h=cv.clientHeight;   // 读 CSS 尺寸（无强制回流），缓存于 backing store
  if(w<50)return;
  var W=Math.round(w*dpr), H=Math.round(h*dpr);
  if(cv.width!==W||cv.height!==H){cv.width=W;cv.height=H;}
  ctx.setTransform(dpr,0,0,dpr,0,0);
  var S=Math.min(w,h), cx=w/2, cy=h/2, R=S/2-5;
  ctx.clearRect(0,0,w,h);
  // ===== 金属表圈：亮银拉丝（无黑框） + 滚花圈 + 抛光内圈 =====
  ctx.beginPath();ctx.arc(cx,cy,R+4,0,6.283);
  var bez=ctx.createRadialGradient(cx,cy,R*0.7,cx,cy,R+4);
  bez.addColorStop(0,'#b8bdc2');bez.addColorStop(0.55,'#e8eaec');bez.addColorStop(0.85,'#a6acb2');bez.addColorStop(1,'#6b727a');
  ctx.fillStyle=bez;ctx.fill();
  // 滚花（短放射纹模拟机加工滚花，浅色）
  for(var k=0;k<72;k++){
    var ka=k/72*6.283, c=Math.cos(ka), s=Math.sin(ka);
    ctx.beginPath();ctx.moveTo(cx+c*(R-1),cy+s*(R-1));ctx.lineTo(cx+c*(R+2),cy+s*(R+2));
    ctx.strokeStyle='rgba(80,86,94,.25)';ctx.lineWidth=1;ctx.stroke();
  }
  // 抛光内圈
  ctx.beginPath();ctx.arc(cx,cy,R-2,0,6.283);ctx.strokeStyle='#f2f4f5';ctx.lineWidth=1.5;ctx.stroke();
  // ===== 镜面暖色表盘（中心亮、边缘暖褐，非黑） =====
  var face=ctx.createRadialGradient(cx-R*0.3,cy-R*0.35,R*0.05,cx,cy,R);
  face.addColorStop(0,'#6a4420');face.addColorStop(0.55,'#3a2410');face.addColorStop(1,'#241505');
  ctx.beginPath();ctx.arc(cx,cy,R-4,0,6.283);ctx.fillStyle=face;ctx.fill();
  // 顶部高光弧（镜面反射）
  var sheen=ctx.createRadialGradient(cx-R*0.35,cy-R*0.4,R*0.05,cx-R*0.35,cy-R*0.4,R*0.9);
  sheen.addColorStop(0,'rgba(255,240,210,.16)');sheen.addColorStop(1,'rgba(255,240,210,0)');
  ctx.beginPath();ctx.arc(cx,cy,R-4,0,6.283);ctx.fillStyle=sheen;ctx.fill();
  // ===== 分区色带（不同颜色划分量程区域） =====
  var zb=zoneBounds(o), prev=o.min;
  for(var z=0;z<zb.length;z++){
    var afrom=gAng(prev,o), ato=gAng(zb[z].to,o);
    ctx.beginPath();ctx.arc(cx,cy,R-9,afrom,ato);
    ctx.strokeStyle=zb[z].c;ctx.globalAlpha=0.16;ctx.lineWidth=R*0.13;ctx.stroke();
    ctx.globalAlpha=1;prev=zb[z].to;
  }
  // ===== 刻度：按所在区域着色（主刻度 + 细分刻线） =====
  var fsz=S>150?10:(S>100?8:7);
  for(var i=0;i<=25;i++){
    var v=o.min+(o.max-o.min)*i/25;
    var a=gAng(v,o), major=(i%5===0);
    var zc=zoneColor(o,v);
    ctx.beginPath();ctx.moveTo(cx+Math.cos(a)*(R-19),cy+Math.sin(a)*(R-19));
    ctx.lineTo(cx+Math.cos(a)*(R-(major?10:14)),cy+Math.sin(a)*(R-(major?10:14)));
    ctx.strokeStyle=zc;ctx.globalAlpha=major?1:0.55;ctx.lineWidth=major?1.6:0.7;ctx.stroke();
    ctx.globalAlpha=1;
    if(major){
      ctx.fillStyle=zc;ctx.font=fsz+'px Consolas,monospace';ctx.textAlign='center';
      var lx=cx+Math.cos(a)*(R-31), ly=cy+Math.sin(a)*(R-31)+fsz*0.35;
      ctx.fillText(gTick(o,i/5),lx,ly);
    }
  }
  // ===== 单指针（细长针 + 柔和阴影，阻尼缓动） =====
  var an=gAng(o.cur,o);
  var nx=Math.cos(an), ny=Math.sin(an);
  ctx.save();
  ctx.shadowColor='rgba(0,0,0,.6)';ctx.shadowBlur=4;ctx.shadowOffsetY=2;
  ctx.strokeStyle='#ffd27f';ctx.lineWidth=2;ctx.lineCap='round';
  ctx.beginPath();ctx.moveTo(cx,cy);ctx.lineTo(cx+nx*(R-24),cy+ny*(R-24));   // 单指针：轴心→针尖
  ctx.stroke();
  ctx.restore();
  // 轴心（金属帽 + 内孔，放大 3 倍）
  ctx.beginPath();ctx.arc(cx,cy,19.5,0,6.283);
  var hub=ctx.createRadialGradient(cx-4.5,cy-4.5,1.5,cx,cy,19.5);
  hub.addColorStop(0,'#f0e6d0');hub.addColorStop(0.6,'#c8a86a');hub.addColorStop(1,'#5a4420');
  ctx.fillStyle=hub;ctx.fill();
  ctx.beginPath();ctx.arc(cx,cy,6.6,0,6.283);ctx.fillStyle='#241505';ctx.fill();
  // ===== 表名 + 数字读数 =====
  ctx.fillStyle='#d8b46a';ctx.font=(S>150?10:(S>100?8:7))+'px Consolas,monospace';ctx.fillText(label,cx,cy+R*0.36);
  ctx.fillStyle='#ffd27f';ctx.font=(S>150?13:(S>100?11:9))+'px Consolas,monospace';ctx.fillText(valText,cx,cy+R*0.54);
}
function stepG(g){var d=g.tgt-g.cur;if(Math.abs(d)<0.0005){g.cur=g.tgt;return}g.cur+=d*0.15}
setInterval(function(){
  stepG(gPeak);stepG(gWm);stepG(gLat);
  drawDial('g-peak',gPeak,'峰值 PEAK',gPeak.cur.toFixed(3));
  drawDial('g-wm',gWm,'水位 WATERMARK',fmt(Math.round(gWm.cur))+' 采样');
  drawDial('g-lat',gLat,'链路延迟 LATENCY',Math.round(gLat.cur)+' ms');
},40);
async function pollStatus(){
  try{
    var r=await fetch('/api/status');var s=await r.json();
    var upTxt=(s.uptime/60000).toFixed(1)+' 分钟';
    // 锁频读数：锁定后显示「采集k→ASIOk ±ppm」，未锁则「测量中…」
    var rlTxt='测量中…';
    if(s.inRate>0){
      var sign=(s.ratioBase-1)>0?'+':'-';
      var ppm=Math.abs((s.ratioBase-1)*1e6).toFixed(1);
      rlTxt=(s.capRate/1000).toFixed(1)+'k→'+(s.asioRate/1000).toFixed(1)+'k '+sign+ppm+' ppm';
    }
    var tgt=s.targetPid?('PID '+s.targetPid+(s.targetActive?' · 活跃':' · 静默')):'发现中…';
    var dr=(typeof s.drift==='number')?s.drift:0;
    document.getElementById('substrip').innerHTML=
      chip('目标',fmt(s.target))+
      chip('采集',s.capRate+' Hz')+
      chip('ASIO',s.asioRate+' Hz')+
      chip('缓冲',s.asioBuffer+' 帧')+
      chip('运行',upTxt)+
      chip('实测输入',s.inRate?s.inRate.toFixed(1)+' Hz':'—')+
      chip('实测输出',s.outRate?s.outRate.toFixed(1)+' Hz':'—')+
      chip('锁频',rlTxt)+
      chip('目标进程',tgt)+
      chip('欠载',fmt(s.underruns||0))+
      chip('漂移',dr.toFixed(1)+' ppm');
    // 三块指针的目标值
    gPeak.tgt=Math.min(1,Math.max(0,s.peak));
    gWm.max=Math.max(2048,s.target*2);
    gWm.mark=s.target;
    gWm.tgt=Math.min(gWm.max,Math.max(0,s.watermark));
    gLat.tgt=s.latencyMs>0?Math.min(300,s.latencyMs):0;
    setLed(document.getElementById('h-asio'),s.asioRate>0);
    setLed(document.getElementById('h-cap'),s.targetActive,s.targetPid&&!s.targetActive);
    // SYSTEM STATUS：静态 DOM 只改灯态（不再每 2s 重建 innerHTML）
    setLed(document.getElementById('l-chain'),s.capRate===s.asioRate);
    setLed(document.getElementById('l-lock'),s.inRate>0);
    setLed(document.getElementById('l-under'),!s.underRecent);
    setLed(document.getElementById('l-clip'),s.peak<=1.0);
    // 时钟漂移：0=测量中(琥珀) |<100ppm=正常(绿) |超差=红
    setLed(document.getElementById('l-drift'),Math.abs(dr)<100,dr===0);
    // 拨杆开关每次轮询同步实际状态（三态请求 2s 内生效，这里回读防漂移）
    document.getElementById('dither').checked=!!s.dither;
    if(firstInit){
      firstInit=false;
      // 桥开关只初始化一次，之后由用户点击驱动（避免每 2s 轮询与点击竞争导致关不掉）
      document.getElementById('bridge').classList.toggle('on',!!s.bridgeOn);
      document.getElementById('tube').checked=!!s.tubeOn;
      document.getElementById('tubewarmth').value=Math.round((s.tubeWarmth||0.3)*100);
      document.getElementById('tubewarmth-v').textContent=document.getElementById('tubewarmth').value+'%';
      document.getElementById('thickness').checked=!!s.thicknessOn;
      document.getElementById('thickness-delay').value=Math.round(s.thicknessDelay||20);
      document.getElementById('booster').checked=!!s.boosterOn;
      document.getElementById('booster-db').value=Math.round((s.boosterDb||12)/18*100);
      document.getElementById('booster-db-v').textContent=document.getElementById('booster-db').value+'%';
      setBank('bank-floor',String(s.floor));
      setBank('bank-src',String(s.srcTaps||0));
      setBank('bank-width',String(s.thicknessWidth||0));
      document.getElementById('bank-device').value=String(s.selectedDevice);
    }
    // ===== 在线升级（底部状态条）：检查更新（当前版本号）=====
    var ub=document.getElementById('updbar');
    if(s.appVer){
      var msgEl=document.getElementById('updmsg');
      // 常态文案：检查更新（版本号）；有动态状态时展示状态
      if(s.updateAvailable){
        msgEl.textContent='发现新版本 V'+s.updateVer+'（版本：V'+s.appVer+'）';
      } else if(s.updateChecking){
        msgEl.textContent='正在检查更新…';
      } else if(s.updateDownloading){
        msgEl.textContent='正在下载并升级…';
      } else if(s.updateError){
        // 检查失败不提示失败字样，静默回到常态文案
        msgEl.textContent='检查更新（版本：V'+s.appVer+'）';
      } else {
        msgEl.textContent='检查更新（版本：V'+s.appVer+'）';
      }
      document.getElementById('updcheck').style.display=s.updateChecking?'none':'inline-block';
      document.getElementById('updgo').style.display=s.updateAvailable?'inline-block':'none';
      ub.style.display='flex';
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
bindBank('bank-src',function(v){ctl('action=src&value='+v)});
bindBank('bank-width',function(v){ctl('action=thicknesswidth&value='+v)});
function ctl(body){if(!CSRF)return;fetch('/api/control',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:body+'&token='+encodeURIComponent(CSRF)})}
document.getElementById('dither').onchange=function(e){ctl('action=dither&value='+(e.target.checked?1:0))}
document.getElementById('tube').onchange=function(e){ctl('action=tube&value='+(e.target.checked?1:0))}
document.getElementById('tubewarmth').oninput=function(e){document.getElementById('tubewarmth-v').textContent=e.target.value+'%'}
document.getElementById('tubewarmth').onchange=function(e){ctl('action=tubewarmth&value='+e.target.value)}
document.getElementById('thickness').onchange=function(e){ctl('action=thickness&value='+(e.target.checked?1:0))}
document.getElementById('thickness-delay').onchange=function(e){ctl('action=thicknessdelay&value='+e.target.value)}
document.getElementById('booster').onchange=function(e){ctl('action=booster&value='+(e.target.checked?1:0))}
document.getElementById('booster-db').oninput=function(e){document.getElementById('booster-db-v').textContent=e.target.value+'%'}
document.getElementById('booster-db').onchange=function(e){ctl('action=boosterdb&value='+e.target.value)}
document.getElementById('bridge').onclick=function(){var on=!this.classList.contains('on');this.classList.toggle('on',on);ctl('action=bridge&value='+(on?1:0))}
document.getElementById('reset').onclick=function(){ctl('action=reset&value=1')}
document.getElementById('updcheck').onclick=function(){ctl('action=updatecheck&value=1')}
document.getElementById('updgo').onclick=function(){ctl('action=updatedownload&value=1')}
function loadDevices(){
  try{
    fetch('/api/devices').then(function(r){return r.json()}).then(function(s){
      var sel=document.getElementById('bank-device');
      var cur=sel.value;
      sel.innerHTML='<option value="-1">自动 (ASIO 优先, 无则 WASAPI)</option>';
      var devs=s.devices||[];
      for(var i=0;i<devs.length;i++){
        var opt=document.createElement('option');
        opt.value=String(i);
        opt.textContent=devs[i].name+' ['+(devs[i].asio?'ASIO':'WASAPI')+']';
        sel.appendChild(opt);
      }
      if(cur)sel.value=cur;
    });
  }catch(e){}
}
document.getElementById('bank-device').onchange=function(){ctl('action=device&value='+this.value)}
document.getElementById('devscan').onclick=function(){ctl('action=devscan&value=1');setTimeout(loadDevices,600)}
loadDevices();
// 设备热插拔自动刷新：每 3 秒轮询设备列表（服务端在设备事件时已自动重扫，
// 此处仅把新声卡/驱动反映到下拉框；与 /api/status 轮询节奏独立，避免频繁刷新列表）
setInterval(loadDevices,3000);
)HTML" R"HTML(
var harmGain=0;    // 谐波增益(dB):默认 0(正常比例),需要时放大残差频谱观测谐波覆盖范围
function fmtFreq(hz){
  if(hz>=1000){var v=hz/1000;v=v>=10?Math.round(v):Math.round(v*10)/10;return v+'k'}
  return String(Math.round(hz));
}
async function drawSpectrum(){
  try{
    var r=await fetch('/api/spectrum');var s=await r.json();
    if(!s['in']||!s['in'].length)return;
    var rate=s.rate||44100;
    var cv=document.getElementById('wf'),ctx=cv.getContext('2d');
    var dpr=window.devicePixelRatio||1;
    var rect=cv.getBoundingClientRect();
    if(cv.width!==Math.round(rect.width*dpr)){cv.width=Math.round(rect.width*dpr);cv.height=Math.round(rect.height*dpr);}
    var W=rect.width,H=rect.height;ctx.setTransform(dpr,0,0,dpr,0,0);
    ctx.clearRect(0,0,W,H);
    var padL=54,padR=12,padT=8,padB=20;
    var plotW=W-padL-padR, plotH=H-padT-padB;
    var fHi=rate/2;              // 线性频率轴 0 ~ Nyquist
    var bins=s['in'].length;     // 128 bin
    var ref=64;                  // 满刻度正弦(Hann 256)峰幅≈N/4 → 0 dBFS
    var X=function(k){return padL+k/bins*plotW};
    var Y=function(db){db=db<-120?-120:(db>0?0:db);return padT+(0-db)/120*plotH};
    function dbOf(m){var a=Math.abs(m)/ref;return a>0?20*Math.log10(a):-200}
    // 网格线:dB 横线 + 频率竖线(每 5kHz)
    ctx.strokeStyle='rgba(154,162,172,0.10)';ctx.lineWidth=1;
    for(var db=-120;db<=0;db+=20){ctx.beginPath();ctx.moveTo(padL,Y(db));ctx.lineTo(padL+plotW,Y(db));ctx.stroke();}
    for(var f=0;f<=fHi;f+=5000){var gx=padL+f/fHi*plotW;ctx.beginPath();ctx.moveTo(gx,padT);ctx.lineTo(gx,padT+plotH);ctx.stroke();}
    // 透明染色波浪:音乐原始(暖黄)+ 新增谐波(青),线性逐 bin 填充
    function wave(mags,gainDb,fill){
      ctx.fillStyle=fill;
      ctx.beginPath();
      ctx.moveTo(padL,padT+plotH);
      for(var k=0;k<bins;k++)ctx.lineTo(X(k),Y(dbOf(mags[k])+gainDb));
      ctx.lineTo(padL+plotW,padT+plotH);
      ctx.closePath();
      ctx.fill();
    }
    wave(s['in'],0,'rgba(255,190,80,0.40)');
    wave(s['res'],harmGain,'rgba(77,216,230,0.55)');
    // X 轴频率刻度(线性,每 5kHz)
    ctx.fillStyle='#6d747d';ctx.font='10px Consolas,monospace';ctx.textAlign='center';
    for(var f=0;f<=fHi;f+=5000){ctx.fillText(fmtFreq(f),padL+f/fHi*plotW,padT+plotH+14);}
    // Y 轴 dBFS 刻度(响度)
    ctx.textAlign='right';
    for(var db=-120;db<=0;db+=20){ctx.fillText(db+' dBFS',padL-5,Y(db)+3);}
    ctx.textAlign='left';
    // 谐波增益角标(仅放大时显示)
    if(harmGain>0){
      ctx.fillStyle='rgba(10,12,14,0.7)';ctx.fillRect(6,6,104,16);
      ctx.fillStyle='#4dd8e6';ctx.fillText('谐波 +'+harmGain+' dB',12,18);
    }
  }catch(e){}
}
document.getElementById('harmgain').oninput=function(e){harmGain=+e.target.value;document.getElementById('harmgain-v').textContent=(harmGain>0?'+':'')+harmGain+' dB'};
/* ===== 水位历史曲线（观察窗口 = X 轴时间范围，Y 刻度 = 0 自动 / 固定满刻度） ===== */
async function drawHist(){
  try{
    var r=await fetch('/api/history?range=600');var s=await r.json();
    var pts=s.points||[];
    var cv=document.getElementById('histcv'),ctx=cv.getContext('2d');
    var dpr=window.devicePixelRatio||1;
    var rect=cv.getBoundingClientRect();
    if(cv.width!==Math.round(rect.width*dpr)){cv.width=Math.round(rect.width*dpr);cv.height=Math.round(rect.height*dpr);}
    var W=rect.width,H=rect.height;ctx.setTransform(dpr,0,0,dpr,0,0);
    ctx.clearRect(0,0,W,H);
    // 背景横网格
    ctx.strokeStyle='rgba(154,162,172,0.10)';ctx.lineWidth=1;
    for(var gi=1;gi<5;gi++){ctx.beginPath();ctx.moveTo(0,H*gi/5);ctx.lineTo(W,H*gi/5);ctx.stroke();}
    if(pts.length<2){
      ctx.fillStyle='#6d747d';ctx.font='12px Consolas,monospace';ctx.textAlign='center';
      ctx.fillText('水位历史采集中…（每 2 秒一个采样点）',W/2,H/2);
      ctx.textAlign='left';return;
    }
    var t0=pts[0][0],t1=pts[pts.length-1][0];
    var tspan=Math.max(1,t1-t0);
    // Y 范围：自动（数据峰值上浮 15% 对齐 1024）
    var ymax=2048;
    for(var pi=0;pi<pts.length;pi++){
      if(pts[pi][1]>ymax)ymax=pts[pi][1];
      if(pts[pi][2]>ymax)ymax=pts[pi][2];
    }
    ymax=Math.ceil(ymax*1.15/1024)*1024;
    var padR=48,padB=16;
    var X=function(t){return 2+(t-t0)/tspan*(W-padR-4)};
    var Y=function(v){return H-padB-(Math.max(0,Math.min(v,ymax))/ymax)*(H-padB-6)};
    // 目标水位线（琥珀虚线）
    ctx.strokeStyle='rgba(255,184,74,0.8)';ctx.lineWidth=1.5;ctx.setLineDash([6,4]);
    ctx.beginPath();ctx.moveTo(X(t0),Y(pts[0][2]));ctx.lineTo(X(t1),Y(pts[pts.length-1][2]));ctx.stroke();
    ctx.setLineDash([]);
    // 水位曲线（暖黄描线 + 半透明填充）
    ctx.strokeStyle='#ffd27f';ctx.lineWidth=1.8;ctx.beginPath();
    for(var ci=0;ci<pts.length;ci++){
      var px=X(pts[ci][0]),py=Y(pts[ci][1]);
      ci?ctx.lineTo(px,py):ctx.moveTo(px,py);
    }
    ctx.stroke();
    ctx.lineTo(X(t1),H-padB);ctx.lineTo(X(t0),H-padB);ctx.closePath();
    ctx.fillStyle='rgba(255,210,127,0.10)';ctx.fill();
    // Y 轴刻度（右侧，5 档）
    ctx.fillStyle='#6d747d';ctx.font='10px Consolas,monospace';ctx.textAlign='left';
    for(var yi=0;yi<=5;yi++){
      var v=Math.round(ymax*yi/5);
      var vv=(v>=1024)?((v/1024)+'k'):String(v);
      ctx.fillText(vv,W-padR+4,H-padB-(H-padB-6)*yi/5+3);
    }
    // X 轴时间标签（起/止）
    ctx.fillText('-'+Math.round(tspan/60)+' 分',2,H-3);
    ctx.textAlign='right';ctx.fillText('现在',W-padR,H-3);ctx.textAlign='left';
  }catch(e){}
}
window.onresize=function(){drawSpectrum();drawHist()}
setInterval(pollStatus,2000);   // 状态轮询(表头/LED)
setInterval(drawSpectrum,100);  // 频谱律动(1/3 倍频程,快速刷新)
setInterval(drawHist,2000);     // 水位历史曲线(慢速刷新)
pollStatus();drawSpectrum();drawHist();
</script>
)HTML";

// HTML 尾部（与主体分离，便于在 </body> 前注入 CSRF Token 脚本）
static const char* HTML_TAIL = "</body></html>";

// UI 即时修改即时体现：优先读 exe 旁 web\index.html（每次首页请求都实时读盘，
// 改文件后刷新浏览器立即生效，无需重编译 C++）；文件不存在或读失败则回退内嵌
// HTML（保持单文件发布兼容）。返回 true 时 out 为完整页面内容。
static bool loadUiFromDisk(std::string& out) {
    wchar_t exe[MAX_PATH];
    if (!GetModuleFileNameW(nullptr, exe, MAX_PATH)) return false;
    wchar_t* slash = wcsrchr(exe, L'\\');
    if (!slash) return false;
    *slash = 0;
    wchar_t path[MAX_PATH];
    swprintf(path, MAX_PATH, L"%s\\web\\index.html", exe);
    FILE* f = _wfopen(path, L"rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 4 * 1024 * 1024) { fclose(f); return false; }  // 空/异常大视为无效
    out.resize((size_t)sz);
    size_t rd = fread(&out[0], 1, (size_t)sz, f);
    fclose(f);
    return rd == (size_t)sz;
}

static SOCKET g_listen = INVALID_SOCKET;
static std::thread g_thread;
static BridgeStatsPtrs g_p;
static ULONGLONG g_startTick = 0;
static std::atomic<bool> g_httpStop{false};
static unsigned short g_port = 3999;          // 实际监听端口（来自 startWebConsole 参数）
static char g_csrfToken[33] = {0};            // 每次进程启动随机生成，POST 控制接口必验

// 生成 128-bit 随机 CSRF Token（32 个十六进制字符）
static void genCsrfToken() {
    std::random_device rd;
    std::mt19937_64 gen(rd());
    gen.seed(rd() ^ (std::mt19937_64::result_type)GetTickCount64()
                 ^ ((std::mt19937_64::result_type)GetCurrentProcessId() << 32));
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 32; ++i) g_csrfToken[i] = hex[gen() & 0xF];
    g_csrfToken[32] = 0;
}

// Host 头白名单校验：只接受 127.0.0.1 / localhost / [::1]（防 DNS rebinding 与跨站请求）
static bool hostAllowed(const char* req) {
    char host[128] = {0};
    const char* h = req;
    while ((h = strstr(h, "\r\n")) != nullptr) {
        h += 2;
        if (_strnicmp(h, "Host:", 5) == 0) {
            const char* v = h + 5;
            while (*v == ' ' || *v == '\t') v++;
            const char* e;
            if (*v == '[') {                      // IPv6 字面量 [::1]:port
                e = strchr(v, ']');
                if (e) e++;
            } else {                              // 域名/IPv4，截到冒号（去端口）或行尾
                e = v;
                while (*e && *e != '\r' && *e != '\n' && *e != ':') e++;
            }
            size_t len = (size_t)(e - v);
            if (len >= sizeof(host)) len = sizeof(host) - 1;
            memcpy(host, v, len);
            break;
        }
    }
    return _stricmp(host, "127.0.0.1") == 0
        || _stricmp(host, "localhost") == 0
        || _stricmp(host, "[::1]") == 0;
}

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

    // 所有 /api/* 接口先过 Host 白名单（DNS rebinding / 跨站防护第一道）
    if (strncmp(path, "/api/", 5) == 0 && !hostAllowed(req)) {
        const char* denied = "{\"error\":\"forbidden host\"}";
        sendResponse(s, "403 Forbidden", "application/json", denied, (int)strlen(denied));
        return;
    }

    if (strcmp(path, "/api/status") == 0) {
        unsigned long long w = g_p.written->load(std::memory_order_relaxed);
        unsigned long long c = g_p.consumed->load(std::memory_order_relaxed);
        unsigned long long d = g_p.dropped->load(std::memory_order_relaxed);
        unsigned long long u = g_p.underruns->load(std::memory_order_relaxed);
        // 升级状态字符串快照（检查线程写，此处加锁防撕裂）
        std::string updMsg, updVer;
        if (g_p.update) {
            std::lock_guard<std::mutex> lk(*g_p.update->mutex);
            updMsg = g_p.update->message;
            updVer = g_p.update->latestVersion;
        }
        // 滑动窗口：最近 10 分钟内是否发生过真欠载（LED 用，不随历史累计永久变红）
        unsigned long long lastUnder = g_p.lastUnderrunAt->load(std::memory_order_relaxed);
        int underRecent = (lastUnder && (GetTickCount64() - lastUnder < 600000)) ? 1 : 0;
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
            "\"latencyMs\":%d,"
            "\"ratioBase\":%.7f,\"inRate\":%.1f,\"outRate\":%.1f,"
            "\"srcTaps\":%d,"
            "\"underRecent\":%d,"
            "\"tubeOn\":%d,\"tubeWarmth\":%.2f,"
            "\"thicknessOn\":%d,\"thicknessDelay\":%.1f,\"thicknessWidth\":%d,"
            "\"boosterOn\":%d,\"boosterDb\":%.1f,"
            "\"bridgeOn\":%d,"
            "\"selectedDevice\":%d,"
            "\"targetPid\":%u,\"targetActive\":%d,"
            "\"appVer\":\"%s\",\"updateAvailable\":%d,\"updateChecking\":%d,"
            "\"updateDownloading\":%d,\"updateError\":%d,\"updateMsg\":\"%s\",\"updateVer\":\"%s\"}",
            wm, target, (unsigned long long)floorM, (unsigned long long)wmMult,
            u, d, (double)g_p.peak->load(std::memory_order_relaxed),
            (double)g_p.driftPpm->load(std::memory_order_relaxed),
            w, c, (long)g_p.asioRate->load(std::memory_order_relaxed), buf,
            (long)g_p.asioType->load(std::memory_order_relaxed),
            (unsigned)g_p.capRate->load(std::memory_order_relaxed),
            (unsigned long long)(GetTickCount64() - g_startTick),
            g_p.ditherOn->load(std::memory_order_relaxed) ? 1 : 0,
            (int)g_p.latencyMs->load(std::memory_order_relaxed),
            (double)g_p.ratioBase->load(std::memory_order_relaxed),
            (double)g_p.inRate->load(std::memory_order_relaxed),
            (double)g_p.outRate->load(std::memory_order_relaxed),
            (int)g_p.srcTaps->load(std::memory_order_relaxed),
            underRecent,
            g_p.tubeOn->load(std::memory_order_relaxed) ? 1 : 0,
            (double)g_p.tubeWarmth->load(std::memory_order_relaxed),
            g_p.thicknessOn->load(std::memory_order_relaxed) ? 1 : 0,
            (double)g_p.thicknessDelayMs->load(std::memory_order_relaxed),
            (int)g_p.thicknessWidth->load(std::memory_order_relaxed),
            g_p.boosterOn->load(std::memory_order_relaxed) ? 1 : 0,
            (double)g_p.boosterDb->load(std::memory_order_relaxed),
            g_p.bridgeOn->load(std::memory_order_relaxed) ? 1 : 0,
            (int)g_p.selectedDevice->load(std::memory_order_relaxed),
            (unsigned)g_p.targetPid->load(std::memory_order_relaxed),
            g_p.targetActive->load(std::memory_order_relaxed) ? 1 : 0,
            kAppVersion,
            (g_p.update && g_p.update->available.load(std::memory_order_relaxed)) ? 1 : 0,
            (g_p.update && g_p.update->checking.load(std::memory_order_relaxed)) ? 1 : 0,
            (g_p.update && g_p.update->downloading.load(std::memory_order_relaxed)) ? 1 : 0,
            (g_p.update && g_p.update->error.load(std::memory_order_relaxed)) ? 1 : 0,
            updMsg.c_str(),
            updVer.c_str());
        sendResponse(s, "200 OK", "application/json; charset=utf-8", body, len);
    } else if (strcmp(path, "/api/control") == 0 && strcmp(method, "POST") == 0) {
        // CSRF Token 校验（跨站防护第二道：恶意页面猜不到本进程随机生成的 Token）
        char tokExpect[64];
        snprintf(tokExpect, sizeof(tokExpect), "token=%s", g_csrfToken);
        if (!strstr(req, tokExpect)) {
            const char* denied = "{\"error\":\"bad token\"}";
            sendResponse(s, "403 Forbidden", "application/json", denied, (int)strlen(denied));
            return;
        }
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
                // 三态请求：主循环 2s 节拍实时应用（无需重建链路）
                g_p.ditherReq->store(v != 0 ? 1 : 2, std::memory_order_relaxed);
            } else if (strstr(body, "action=src")) {
                g_p.srcTaps->store(v == 32 ? 32 : 0, std::memory_order_relaxed);
            } else if (strstr(body, "action=tubewarmth")) {
                // 必须排在 action=tube 之前：strstr 前缀匹配会把 action=tubewarmth
                // 误吞进 action=tube（暖度滑块失效且误开染色）。
                float w = (float)v / 100.0f;
                if (w < 0.0f) w = 0.0f;
                if (w > 1.0f) w = 1.0f;
                g_p.tubeWarmth->store(w, std::memory_order_relaxed);
            } else if (strstr(body, "action=tube")) {
                g_p.tubeOn->store(v != 0, std::memory_order_relaxed);
            } else if (strstr(body, "action=thicknessdelay")) {
                // 必须排在 action=thickness 之前（前缀匹配）
                float d = (float)v;
                if (d < 2.0f) d = 2.0f;
                if (d > 100.0f) d = 100.0f;
                g_p.thicknessDelayMs->store(d, std::memory_order_relaxed);
            } else if (strstr(body, "action=thicknesswidth")) {
                int w = v;
                if (w < 0) w = 0;
                if (w > 3) w = 3;
                g_p.thicknessWidth->store(w, std::memory_order_relaxed);
            } else if (strstr(body, "action=thickness")) {
                g_p.thicknessOn->store(v != 0, std::memory_order_relaxed);
            } else if (strstr(body, "action=boosterdb")) {
                // 必须排在 action=booster 之前（前缀匹配）；百分比(0~100) → dB(0~18)
                float d = (float)v * 18.0f / 100.0f;
                if (d < 0.0f) d = 0.0f;
                if (d > 18.0f) d = 18.0f;
                g_p.boosterDb->store(d, std::memory_order_relaxed);
            } else if (strstr(body, "action=booster")) {
                g_p.boosterOn->store(v != 0, std::memory_order_relaxed);
            } else if (strstr(body, "action=bridge")) {
                // ASIO Bridge 开关：开=桥接(静音目标端点), 关=停止(恢复系统音量)
                g_p.bridgeOn->store(v != 0, std::memory_order_relaxed);
                if (v == 0) g_p.needRestart->store(true);   // 关闭时立即断开会话
            } else if (strstr(body, "action=device")) {
                // 输出设备选择：-1=自动, 0..N-1=设备索引；切换需重建链路
                g_p.selectedDevice->store(v, std::memory_order_relaxed);
                g_p.needRestart->store(true);
            } else if (strstr(body, "action=devscan")) {
                std::string scanErr;
                auto devs = ScanOutputDevices(scanErr);
                {
                    std::lock_guard<std::mutex> lk(*g_p.devicesMutex);
                    *g_p.devices = std::move(devs);
                }
            } else if (strstr(body, "action=reset")) {
                g_p.resetReq->store(true);
            } else if (strstr(body, "action=updatecheck") && g_p.update) {
                requestUpdateCheck(g_p.update);
            } else if (strstr(body, "action=updatedownload") && g_p.update) {
                requestUpdateDownload(g_p.update);
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
        // 注意防下溢：进程运行时长不足观察窗口时从 0 起算（否则 uint64 回绕滤掉全部点）
        const uint64_t startSec = (nowSec > (uint64_t)rng) ? (nowSec - (uint64_t)rng) : 0;
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
    } else if (strcmp(path, "/api/spectrum") == 0) {
        // 频谱:输入频谱(音乐原始)+ 残差频谱(新增谐波),各 128 bin;rate=采样率(供 1/3 倍频程映射)
        char body[8192];
        int off = 0;
        off += snprintf(body + off, sizeof(body) - off, "{\"seq\":%llu,\"rate\":%ld,\"in\":[",
                        (unsigned long long)g_p.specSeq->load(std::memory_order_acquire),
                        (long)g_p.asioRate->load(std::memory_order_relaxed));
        for (int k = 0; k < 128; ++k)
            off += snprintf(body + off, sizeof(body) - off, "%s%.3f", k ? "," : "",
                            (*g_p.specIn)[k]);
        off += snprintf(body + off, sizeof(body) - off, "],\"res\":[");
        for (int k = 0; k < 128; ++k)
            off += snprintf(body + off, sizeof(body) - off, "%s%.4f", k ? "," : "",
                            (*g_p.specRes)[k]);
        off += snprintf(body + off, sizeof(body) - off, "]}");
        sendResponse(s, "200 OK", "application/json; charset=utf-8", body, off);
    } else if (strcmp(path, "/api/devices") == 0) {
        // 输出设备列表（名称 + 后端类型）
        std::string body = "{\"devices\":[";
        bool first = true;
        {
            std::lock_guard<std::mutex> lk(*g_p.devicesMutex);
            for (size_t i = 0; i < g_p.devices->size(); ++i) {
                const DeviceEntry& d = (*g_p.devices)[i];
                char tmp[640];
                snprintf(tmp, sizeof(tmp), "%s{\"name\":\"%s\",\"asio\":%d,\"driver\":\"%s\"}",
                         first ? "" : ",",
                         ws2json(d.name).c_str(),
                         d.asio ? 1 : 0,
                         d.asioDriver.c_str());
                body += tmp;
                first = false;
            }
        }
        body += "]}";
        sendResponse(s, "200 OK", "application/json; charset=utf-8", body.c_str(), (int)body.size());
    } else {
        // 首页：优先磁盘 UI（web\index.html，即时修改即时体现），回退内嵌。
        // CSRF Token 注入在最后一个 </body> 之前（主脚本之后，加载完成即可用）。
        std::string page;
        if (!loadUiFromDisk(page))
            page.assign(HTML).append(HTML_TAIL);          // 回退：内嵌主体 + 尾
        char inj[128];
        int ilen = snprintf(inj, sizeof(inj), "<script>CSRF=\"%s\";</script>", g_csrfToken);
        size_t pos = page.rfind("</body>");
        if (pos != std::string::npos) page.insert(pos, inj, (size_t)ilen);
        else page.append(inj, (size_t)ilen);
        sendResponse(s, "200 OK", "text/html; charset=utf-8", page.c_str(), (int)page.size());
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
    addr.sin_port = htons(g_port);
    if (bind(g_listen, (sockaddr*)&addr, sizeof(addr)) != 0) {
        printf("[控制台] 绑定 127.0.0.1:%u 失败（端口被占用？）\n", (unsigned)g_port);
        closesocket(g_listen);
        WSACleanup();
        return;
    }
    listen(g_listen, 4);
    printf("[控制台] 已启动: http://127.0.0.1:%u\n", (unsigned)g_port);

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
    if (port == 0) port = 3999;
    g_port = port;
    g_p = p;
    g_startTick = GetTickCount64();   // 进程内 uptime 起点
    genCsrfToken();                   // 每次启动生成新 CSRF Token
    g_httpStop.store(false);
    g_thread = std::thread(httpThread);
    return true;
}

void stopWebConsole() {
    g_httpStop.store(true);
    if (g_thread.joinable()) g_thread.join();
}
