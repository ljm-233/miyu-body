// 本地设置页实现 —— 2026-11-09 22:5x（NOTES 第十四节）。
//
// 为什么自己做 HTTP 而不是塞个库：只有三个路由（GET / 、GET /state 、POST /set），
// 一个进程、一条回环、单用户，塞库反而多一层依赖和一堆线程。这里全部在一个 fd 上
// 非阻塞收，每帧最多接两个连接，渲染永远不等它。
//
// 三条不能破的规矩：
//   ① bind 到回环（INADDR_LOOPBACK）—— 外面连不进来，也不需要任何鉴权
//   ② 每帧只 poll(fd, 0)，连接不完整就留着下一帧再读，超过 3 秒直接掐
//   ③ 设置改完立刻回调给 main/platform 去真生效；只存起来不改行为的字段一律不算数
//
// 落盘：state/settings.json（先写 .tmp 再 rename，半截文件不会污染下次启动）。
// 路径优先级：MIYU_STATE_DIR → <exe>/../state（build/miyu-wayland ↔ native3/state）

#include "web.h"
#include "partapi.h"   // 二十五：部位→表情的真源在 parts.tsv
#include "partpoke.h"  // 部位名单（Step 7 之后由 partapi 从 parts.tsv 提供）

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

#include "ctl.h"

namespace web {
namespace {

void logw(const char* fmt, ...) {
    std::fputs("[web] ", stderr);
    va_list ap; va_start(ap, fmt);
    std::vfprintf(stderr, fmt, ap); va_end(ap);
    std::fputc('\n', stderr);
}

// ── 单页 HTML（内嵌，不另建静态文件、无外链、无 CDN）────────────────────
// 深色 + 蓝紫（#0d1117 底 / #7aa2f7 主色，跟她身上那套一样），窄栏，手机上也能开。
const char* kHtml = R"HTML(<!doctype html>
<html lang="zh"><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<title>米鱼 · 设置</title>
<style>
  :root{
    --bg:#16181d; --card:#1e2128; --line:rgba(255,255,255,.06);
    --fg:#d7dce6; --dim:#8b93a1; --accent:#7aa2f7;
    --track:#2c313a; --ctl:#262a33; --knob:#8b93a1; --gh:#9aa4b8;
  }
  html[data-theme="light"]{
    --bg:#f5f6f8; --card:#ffffff; --line:rgba(0,0,0,.08);
    --fg:#1f2328; --dim:#6b7280; --accent:#3b6fd4;
    --track:#d8dbe1; --ctl:#f0f2f5; --knob:#9aa1ad; --gh:#5b6472;
  }
  *{box-sizing:border-box}
  html,body{margin:0;background:var(--bg);color:var(--fg);
    font:15px/1.5 -apple-system,BlinkMacSystemFont,"Noto Sans SC","Segoe UI",sans-serif}
  .wrap{max-width:1400px;margin:0 auto;padding:20px 18px 36px}
  .head{display:flex;align-items:center;justify-content:space-between;gap:12px}
  h1{font-size:15px;font-weight:600;margin:0;letter-spacing:.08em;color:var(--accent)}
  .themebtn{font:inherit;font-size:15px;line-height:1;color:var(--dim);background:var(--card);
    border:1px solid var(--line);border-radius:9px;padding:6px 10px;cursor:pointer}
  .themebtn:hover{color:var(--accent)}
  .status{display:flex;flex-wrap:wrap;gap:3px 14px;font-size:12px;color:var(--dim);margin:9px 0 16px}
  .status b{color:var(--gh);font-weight:600}
  .status i{color:var(--accent);font-style:normal;font-variant-numeric:tabular-nums}
  .grid{display:grid;grid-template-columns:minmax(0,1fr);gap:14px;align-items:start}
  @media (min-width:900px){ .grid{grid-template-columns:repeat(2,minmax(0,1fr));gap:18px} }
  @media (min-width:1400px){ .grid{grid-template-columns:repeat(3,minmax(0,1fr))} }
  .card{background:var(--card);border:1px solid var(--line);border-radius:12px;
        padding:3px 16px 8px;min-width:0}
  .gh{font-size:11px;letter-spacing:.18em;color:var(--gh);padding:12px 0 5px}
  .row{display:grid;grid-template-columns:104px minmax(0,1fr) 62px 52px;column-gap:10px;
       align-items:center;padding:9px 0;border-top:1px solid var(--line)}
  .gh + .row{border-top:0}
  .k{font-size:13px;white-space:nowrap}
  .c{min-width:0;display:flex;align-items:center}
  .v{font-size:12px;text-align:right;color:var(--accent);
     font-variant-numeric:tabular-nums;white-space:nowrap}
  .d{grid-column:1/-1;font-size:11px;color:var(--dim);margin-top:2px}
  input[type=range]{-webkit-appearance:none;appearance:none;width:100%;height:20px;
    background:transparent;margin:0}
  input[type=range]::-webkit-slider-runnable-track{height:4px;border-radius:2px;background:var(--track)}
  input[type=range]::-webkit-slider-thumb{-webkit-appearance:none;appearance:none;width:15px;
    height:15px;margin-top:-5.5px;border-radius:50%;background:var(--accent);border:0;
    box-shadow:0 0 0 3px rgba(122,162,247,.16)}
  input[type=range]::-moz-range-track{height:4px;border-radius:2px;background:var(--track)}
  input[type=range]::-moz-range-thumb{width:15px;height:15px;border:0;border-radius:50%;
    background:var(--accent)}
  select{width:100%;background:var(--ctl);color:var(--fg);border:1px solid var(--line);
    border-radius:8px;padding:4px 6px;font-size:12px;outline:none}
  .sw{position:relative;display:inline-block;width:38px;height:21px}
  .sw input{opacity:0;width:0;height:0}
  .sl{position:absolute;inset:0;background:var(--track);border-radius:22px;cursor:pointer;
      transition:background .15s}
  .sl:before{content:"";position:absolute;width:15px;height:15px;left:3px;top:3px;
      background:var(--knob);border-radius:50%;transition:transform .15s,background .15s}
  .sw input:checked + .sl{background:rgba(122,162,247,.35)}
  .sw input:checked + .sl:before{transform:translateX(17px);background:var(--accent)}
  /* 2026-11-10 23:1x：底部这几块跟着 .main 一起被拉到 1700px 宽，按钮条横贯整屏很难看。
     统一限宽居中，和上面两栏内容区的视觉宽度对齐。 */
  .tools, .btns, .hint, .foot{max-width:760px;margin-left:auto;margin-right:auto}
  .tools{display:flex;justify-content:center;gap:8px;margin:18px auto 10px}
  .tools button{font:inherit;font-size:11px;color:var(--dim);background:none;border:0;
    padding:4px 6px;cursor:pointer;border-radius:6px}
  .tools button:hover{color:var(--accent);background:var(--card)}
  .btns{margin-top:2px;max-width:760px}
  button.main{width:100%;font:inherit;font-size:13px;color:var(--fg);background:var(--card);
    border:1px solid var(--line);border-radius:10px;padding:11px 8px;cursor:pointer;
    transition:border-color .15s,color .15s}
  button.main:hover{border-color:#f7768e;color:#f7768e}
  .hint{margin-top:12px;font-size:11px;color:var(--dim);min-height:15px;max-width:760px;
        text-align:center;transition:color .2s}
  .hint.ok{color:#2f9e44}
  html[data-theme="light"] .hint.ok{color:#2b8a3e}
  .foot{margin-top:14px;font-size:11px;color:var(--dim);text-align:center;opacity:.8;max-width:760px}

  /* ── 二十五：重排版。左边一条固定导航，右边内容区，窄屏自动收成顶部横条 ── */
  .app{display:flex;gap:22px;align-items:flex-start;width:100%;max-width:1720px;margin:0 auto;
       padding:18px 22px 40px}
  .side{position:sticky;top:18px;flex:0 0 158px;display:flex;flex-direction:column;gap:2px;
        position:sticky;z-index:5}
  .brand{font-size:13px;font-weight:600;color:var(--accent);letter-spacing:.1em;
         padding:2px 10px 12px}
  .side a{display:block;font-size:13px;color:var(--dim);text-decoration:none;
          padding:7px 10px;border-radius:8px;cursor:pointer;border-left:2px solid transparent}
  .side a:hover{color:var(--fg);background:var(--card)}
  .side a.on{color:var(--accent);background:var(--card);border-left-color:var(--accent)}
  .main{flex:1 1 auto;min-width:0}
  /* 宽屏时内容区分两栏，别让一行拉到 1600px 那么长 */
  @media (min-width:1180px){
    #groups{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:0 22px;align-items:start}
  }
  section{display:none}
  section.on{display:block}
  @media (max-width:760px){
    .app{flex-direction:column;gap:10px;padding:14px 12px 30px}
    .side{position:static;flex:none;flex-direction:row;flex-wrap:wrap;gap:6px;width:100%;
          overflow-x:auto;padding-bottom:4px}
    .brand{display:none}
    .side a{border-left:0;border-bottom:2px solid transparent;padding:6px 9px}
    .side a.on{border-bottom-color:var(--accent)}
  }
  /* 表情墙 / 动作按钮墙 */
  .wall{display:flex;flex-wrap:wrap;gap:7px;padding:6px 0 10px}
  .wall button{font:inherit;font-size:12px;color:var(--fg);background:var(--ctl);
    border:1px solid var(--line);border-radius:8px;padding:6px 11px;cursor:pointer;
    transition:border-color .12s,color .12s,background .12s}
  .wall button:hover{border-color:var(--accent);color:var(--accent)}
  .wall button:active{background:rgba(122,162,247,.18)}
  .wall button.dead{opacity:.45}
  /* 行尾的「试」小按钮 */
  .try{font:inherit;font-size:11px;color:var(--dim);background:var(--ctl);
    border:1px solid var(--line);border-radius:7px;padding:3px 8px;cursor:pointer;
    margin-left:6px;white-space:nowrap}
  .try:hover{color:var(--accent);border-color:var(--accent)}
</style></style></head>
<body><script>
(function(){
  var t = null;
  try { t = localStorage.getItem('miyu-theme'); } catch(e) {}
  // ?theme=light / dark：只对这一页生效，不写 localStorage（截图/调试用）
  var q = location.search.indexOf('theme=light') >= 0 ? 'light'
        : (location.search.indexOf('theme=dark') >= 0 ? 'dark' : null);
  document.documentElement.setAttribute('data-theme', q || (t === 'light' ? 'light' : 'dark'));
})();
</script><div class="app">
  <nav class="side" id="nav"><div class="brand">米鱼 · 设置</div></nav>
  <main class="main">
    <div class="head"><p class="status" id="status">连接中…</p>
      <button class="themebtn" id="theme" title="切换明暗">☀</button></div>
    <div id="groups"></div>
    <div class="tools">
      <button id="call">叫她一下</button>
      <button id="exprnext">切个表情</button>
      <button id="restart">重启服务</button>
    </div>
    <div class="btns"><button class="main" id="reset">恢复默认</button></div>
    <div class="hint" id="hint"></div>
    <div class="foot">改完即时生效 · 设置存在 state/settings.json</div>
  </main>
</div>
<script>
(function(){
  var themeBtn = document.getElementById('theme');
  function paintTheme(){
    var light = document.documentElement.getAttribute('data-theme') === 'light';
    themeBtn.textContent = light ? '☾' : '☀';
    themeBtn.title = light ? '切到暗色' : '切到亮色';
  }
  themeBtn.addEventListener('click', function(){
    var light = document.documentElement.getAttribute('data-theme') === 'light';
    var next = light ? 'dark' : 'light';
    document.documentElement.setAttribute('data-theme', next);
    try { localStorage.setItem('miyu-theme', next); } catch(e) {}
    paintTheme();
  });
  paintTheme();

  var hintEl = document.getElementById('hint');
  function hint(t, ok){
    hintEl.textContent = t;
    hintEl.className = ok ? 'hint ok' : 'hint';
    if (ok) setTimeout(function(){ if (hintEl.textContent === t) hintEl.textContent = ''; }, 1600);
  }
  function post(obj){
    return fetch('/set', {method:'POST', headers:{'Content-Type':'application/json'},
      body: JSON.stringify(obj)}).then(function(r){ return r.json(); });
  }
  var timers = {};
  function push(k, v, wait){
    clearTimeout(timers[k]);
    var o = {}; o[k] = v;
    timers[k] = setTimeout(function(){
      post(o).then(function(d){
        if (d && d.ok) hint('已应用：' + d.applied.join(', '), true);
        else hint('没生效', false);
      }).catch(function(){ hint('连不上（服务在重启？）', false); });
    }, wait);
  }

  var CORNER = {free:'自由', tl:'左上', tr:'右上', bl:'左下', br:'右下'};
  var AA = {off:'关', fxaa:'fxaa', smaa:'smaa', ssaa2x:'2×', ssaa3x:'3×'};
  var IDLE = {quiet:'安静', natural:'自然', lively:'活泼'};
  function pick(m){ return function(v){ return m[v] || String(v); }; }

  // 顺序 = 使用频率：常用 → 外观 → 行为 → 性能 → 声音
  var SCHEMA = [
    {g:'常用', items:[
      {k:'hide', t:'bool', label:'显示', invert:true,
       d:'关掉后她整个不见，进程还在，点哪儿都直接穿到桌面'},
      {k:'scale', t:'range', min:0.4, max:1.6, step:0.05, label:'大小',
       d:'整体放大缩小，位置不动'},
      {k:'alpha', t:'range', min:0.3, max:1.0, step:0.05, label:'透明度',
       d:'越低桌面从她身上透出来越多'},
      {k:'corner', t:'select', label:'吸角',
       opts:[['center','正中'],['free','自由'],['tl','左上'],['tr','右上'],['bl','左下'],['br','右下']],
       d:'吸到副屏哪个角，选完马上挪过去', show:pick(CORNER)}
    ]},
    {g:'外观', items:[
      {k:'aa', t:'select', label:'抗锯齿',
       opts:[['off','关'],['ssaa2x','ssaa 2 倍'],['ssaa3x','ssaa 3 倍'],['fxaa','fxaa'],
             ['smaa','smaa（只沿边缘混，比 fxaa 清楚）']],
       d:'ssaa2x 最锐（贴图不走 mipmap），代价是帧率；fxaa 最快但糊——它按亮度整块混合；smaa 只混边缘、比 fxaa 清楚，代价是 1x 上多跑三遍全屏；off 最省',
       show:pick(AA)},
      {k:'rim', t:'range', min:0, max:1, step:0.05, label:'轮廓光',
       d:'她身边一圈淡蓝光，深色壁纸上更好看'},
      {k:'particles', t:'range', min:0, max:1, step:0.05, label:'粒子',
       d:'身后往上飘的小气泡，0 关掉'},
      {k:'shadow', t:'range', min:0, max:1, step:0.05, label:'阴影',
       d:'脚下的软阴影。深色壁纸上基本看不见，只在浅色背景才有用，所以默认关'}
    ]},
    {g:'行为', items:[
      {k:'idleStyle', t:'select', label:'待机风格',
       opts:[['quiet','安静'],['natural','自然'],['lively','活泼']],
       d:'安静几乎不动、只剩呼吸；活泼会明显晃', show:pick(IDLE)},
      {k:'idle', t:'range', min:0, max:3, step:0.1, label:'待机幅度',
       d:'在风格之上再乘一个倍数，0 = 定住只剩呼吸'},
      {k:'lookOn', t:'bool', label:'视线',
       d:'关掉她就不跟鼠标了，眼睛回正'},
      {k:'look', t:'range', min:0, max:2, step:0.1, label:'视线强度',
       d:'看鼠标的幅度，眼珠和身体一起倾'},
      {k:'blink', t:'range', min:0, max:2, step:0.1, label:'眨眼',
       d:'2 = 眨得很勤，0 = 一直睁着'},
      {k:'tap', t:'bool', label:'点击',
       d:'戳她一下有反应；关掉只剩拖动'},
            {k:'dblTap', t:'bool', label:'双击反应',
       d:'同一处 0.9 秒内点两下，改说一声「啊」。默认关（这句的音频还没做好）'},
      {k:'combo', t:'bool', label:'连点脾气',
       d:'连着点同一个地方：第 3 下动作变大，第 5 下起换成黑脸。关掉就每一下反应一样'},
      {k:'tapPart', t:'bool', label:'点哪动哪',
       d:'戳哪个部位只让那一块动；关掉就回到整套反应。点空白处仍然播整套'},
      {k:'expr', t:'bool', label:'表情',
       d:'所有表情切换的总开关'},
      {k:'exprHold', t:'range', min:0.2, max:5, step:0.1, label:'表情停留',
       d:'表情挂多久自己淡回默认'},
      {k:'mouth', t:'range', min:0.5, max:3, step:0.1, label:'嘴动',
       d:'说完一句嘴还动多久（开合快慢不变）'}
    ]},
    {g:'手感', items:[
      {k:'grab', t:'bool', label:'抓起反馈',
       d:'按住开始拖的那一下她会颠一下（0.42 秒，上身微提 + 一点后仰，头发裙摆跟着晃）'},
      {k:'inertia', t:'bool', label:'松手惯性',
       d:'松手后按你最后那一下的速度再滑一小段才停；再按住她立刻刹住'},
      {k:'dragSway', t:'range', min:0, max:2, step:0.1, label:'拖动摆动',
       d:'拖她的时候整个人会真的歪过去（动的是投影，不是参数——参数她身上看不见）。1 = 拖得越快歪得越多，松手回弹'},
      {k:'swayIdle', t:'bool', label:'静止轻摆',
       d:'不动的时候极慢地歪一点点（15 秒一个来回，0.3°）；只在「活泼」待机风格下生效'},
      {k:'legSway', t:'range', min:0, max:3, step:0.1, label:'腿晃',
       d:'点腿时晃多大。0 最轻（干脆利落），3 最大（软、晃得久）'},
      {k:'chestSway', t:'range', min:0, max:3, step:0.1, label:'胸晃',
       d:'点胸口时晃多大。0 最轻（干脆利落），2 最大（软、晃得久）'},
      {k:'dragSpring', t:'range', min:0, max:2, step:0.1, label:'拖动弹簧',
       d:'拖动不是刚性跟随：她有一点点「重」，弹簧追你，快速拖能看到落后十几到几十像素。0.5 更软更沉，2 更跟手'},
      {k:'swayTilt', t:'range', min:0, max:4.5, step:0.5, label:'整体倾斜',
       d:'拖动时整张图绕中心歪一点（度）。默认 0 = 关——整体歪看着像「一张画在转」，他要的是各部位在甩，那个由「拖动摆动」给'},
      {k:'edgeSoft', t:'bool', label:'贴边软阻力',
       d:'拖到屏幕边外面越拖越沉（那一段位移只按 55% 生效），不再硬卡在边上'},
      {k:'timeAware', t:'bool', label:'时间感',
       d:'按本地钟点换状态：深夜幅度打六折、眨眼变少、每 40–90 秒低头一次；清晨比白天精神一点'}
    ]},
    {g:'性能', items:[
      {k:'fps', t:'select', label:'帧率', num:true,
       opts:[['30','30 最省电'],['60','60 默认'],['120','120'],['200','200 副屏上限'],
             ['240','240 主屏上限']],
       d:'越低越省 CPU；副屏 200 就封顶，240 只有挪到主屏才跑得满'}
    ]},
    {g:'声音', items:[
      {k:'voice', t:'bool', label:'出声',
       d:'说话时放预生成的短句，没生成的那句就静默'},
      {k:'vol', t:'range', min:0, max:1, step:0.05, label:'音量',
       d:'0 = 有声音但不响，要静音用上面那个开关'}
    ]}
  ];

  var els = {};
  function addRow(card, it){
    var row = document.createElement('div'); row.className = 'row';
    var k = document.createElement('span'); k.className = 'k'; k.textContent = it.label;
    var c = document.createElement('span'); c.className = 'c';
    var v = document.createElement('span'); v.className = 'v'; v.textContent = '—';
    var show = it.show || function(x){
      return it.t === 'range' ? Number(x).toFixed(2) : (it.t === 'bool' ? (x ? '开' : '关') : String(x));
    };

    if (it.t === 'range') {
      var r = document.createElement('input');
      r.type = 'range'; r.min = it.min; r.max = it.max; r.step = it.step;
      r.addEventListener('input', function(){
        v.textContent = Number(r.value).toFixed(2);
        push(it.k, parseFloat(r.value), 300);      // 300ms 防抖，别把服务端刷爆
      });
      c.appendChild(r);
      els[it.k] = {el: r, out: v, type: 'range', show: show};
    } else if (it.t === 'bool') {
      var lab = document.createElement('label'); lab.className = 'sw';
      var cb = document.createElement('input'); cb.type = 'checkbox';
      var sl = document.createElement('span'); sl.className = 'sl';
      lab.appendChild(cb); lab.appendChild(sl);
      c.appendChild(lab);
      cb.addEventListener('change', function(){
        var o = {}; o[it.k] = it.invert ? !cb.checked : cb.checked;
        v.textContent = cb.checked ? '开' : '关';
        post(o).then(function(d){
          if (d && d.ok) hint('已应用：' + it.k, true); else hint('没生效', false);
        }).catch(function(){});
      });
      els[it.k] = {el: cb, out: v, type: 'bool', show: show};
    } else {
      var sel = document.createElement('select');
      it.opts.forEach(function(o2){
        var op = document.createElement('option'); op.value = o2[0]; op.textContent = o2[1];
        sel.appendChild(op);
      });
      c.appendChild(sel);
      sel.addEventListener('change', function(){
        var o = {}; o[it.k] = it.num ? parseInt(sel.value, 10) : sel.value;
        v.textContent = show(it.num ? parseInt(sel.value, 10) : sel.value);
        post(o).then(function(d){
          if (d && d.ok) hint('已应用：' + it.k, true); else hint('没生效', false);
        }).catch(function(){});
      });
      els[it.k] = {el: sel, out: v, type: 'select', show: show, num: !!it.num};
    }

    row.appendChild(k); row.appendChild(c); row.appendChild(v);
    var d = document.createElement('div'); d.className = 'd'; d.textContent = it.d;
    row.appendChild(d);
    card.appendChild(row);
  }

  var root = document.getElementById('groups');
  // 二十五：重排版。每个分组渲染成一个 <section>，左边导航切显示；
  // 宽屏是左侧固定条，窄屏自动收成顶部横条（CSS 里那段 media query）。
  var navEl = document.getElementById('nav');
  var sections = [];
  function addSection(title){
    var sec = document.createElement('section');
    sec.style.display = 'none';   // 显式隐藏，不靠 CSS 类
    var card = document.createElement('div'); card.className = 'card';
    var h = document.createElement('div'); h.className = 'gh'; h.textContent = title;
    card.appendChild(h); sec.appendChild(card);
    root.appendChild(sec);
    var idx = sections.length;               // 先记下自己的下标
    sections.push({sec: sec, card: card});
    var a = document.createElement('a'); a.textContent = title;
    // 2026-11-10 22:2x：这里原来写的是 `sections.length - 1`，闭包在点击那一刻求值，
    // 于是**点任何一个标签都跳到最后一页**——他说的「其他标签点不了」就是这个。
    a.addEventListener('click', function(){ showSection(idx); });
    navEl.appendChild(a);
    return card;
  }
  function showSection(i){
    if (!(i >= 0 && i < sections.length)) i = 0;
    sections.forEach(function(s, j){ s.sec.style.display = (j === i) ? 'block' : 'none'; });
    var links = navEl.querySelectorAll('a');
    for (var k = 0; k < links.length; k++) links[k].classList.toggle('on', k === i);
    try { localStorage.setItem('miyu-tab', String(i)); } catch(e) {}
  }
  SCHEMA.forEach(function(sec){
    var card = addSection(sec.g);
    sec.items.forEach(function(it){ addRow(card, it); });
  });

  // 二十五：诊断组 —— 显示「最近一次点击判成了哪个部位、在哪个 u/v」。
  // 今晚所有串位（耳朵判成头、飘带判成腿）都是靠这两个数定位的，
  // 放进页面他就不用每次问我「这里算哪块」。
  var diagCard = null, diagLine = null;
  function buildDiag(){
    if (diagCard) return;
    diagCard = addSection('诊断');
    diagLine = document.createElement('div');
    diagLine.className = 'd';
    diagLine.style.fontSize = '13px';
    diagLine.style.color = 'var(--fg)';
    diagLine.textContent = '还没点过她';
    diagCard.appendChild(diagLine);
    var tip = document.createElement('div'); tip.className = 'd';
    tip.textContent = '点她身上任意一处，这里会显示判成了哪个部位、点在哪（u/v 是窗口归一化坐标）';
    diagCard.appendChild(tip);
  }

  // 二十五：两组新东西 —— 表情墙、动作墙。
  // 都是「点一下立刻发生」，用的是 POST /set 的 cmd 通道（cmd + arg），
  // 不写设置、不落盘，和改滑块是两回事。
  var exprCard = null, planCard = null;
  function cmd(name, arg){
    var o = {}; o.cmd = name; o.arg = arg;
    post(o).then(function(d){
      if (d && d.ok) hint(name === 'expr' ? ('表情：' + arg) : (name === 'plan' ? ('播：' + arg) : 'ok'), true);
      else hint('没生效', false);
    }).catch(function(){ hint('连不上', false); });
  }
  function buildExprWall(names){
    if (exprCard) return;
    exprCard = addSection('表情');
    var wall = document.createElement('div'); wall.className = 'wall';
    names.forEach(function(n){
      var b = document.createElement('button'); b.textContent = n;
      b.addEventListener('click', function(){ cmd('expr', n); });
      wall.appendChild(b);
    });
    var off = document.createElement('button'); off.textContent = '回默认';
    off.addEventListener('click', function(){ cmd('expr', '默认'); });
    wall.appendChild(off);
    exprCard.appendChild(wall);
    var tip = document.createElement('div'); tip.className = 'd';
    tip.textContent = '点一个立刻切过去，5 秒后淡回默认（和点她身上部位用的是同一套）';
    exprCard.appendChild(tip);
  }
  function buildPlanWall(names){
    if (planCard) return;
    planCard = addSection('动作');
    var wall = document.createElement('div'); wall.className = 'wall';
    names.forEach(function(n){
      var b = document.createElement('button'); b.textContent = n;
      b.addEventListener('click', function(){ cmd('plan', n); });
      wall.appendChild(b);
    });
    planCard.appendChild(wall);
    var tip = document.createElement('div'); tip.className = 'd';
    tip.textContent = 'plans/ 目录里的动作计划，点一下播一遍';
    planCard.appendChild(tip);
  }

  // 二十五 → Step 7：点击反应 —— 每个部位点下去播哪个表情。
  // 部位列表**不再写死**：由 /state 的 partNames（后端从 state/parts.tsv 取，顺序 = id）
  // 填进来。加一个部位只要改表，这里一行代码都不用动。
  // 表情列表还是来自 exprNames（加了新 exp3 不用改页面）。空值 = 只放自身动作。
  var PARTS = [];
  var partCard = null;
  var tabRestored = false;
  function buildPartCard(names){
    if (partCard) return;
    // 2026-11-10 23:0x：这里原来自己 createElement('div') 造了一张裸 card，
    // 然后 `root.appendChild(partCard)` 直接塞进 #groups —— 它**不属于任何 section**，
    // 于是切页时它永远显示。他说的「每一页都有点击反应」就是这个。
    // 改成和其它组一样走 addSection，才会跟着标签页一起切换。
    partCard = addSection('点击反应');
    // 二十五：这两个在我们这个模型里**参数没绑顶点**（509 个参数里只有 162 个
    // 会真的动画面，它们用的 key9 / key13 不在里面），选下去脸一点变化都没有。
    // 名字直接标出来，省得他以为设置没生效。
    //   来源：state/param-map.json（param-probe 逐个按死 min/max 量的）
    var DEAD = {'兔牙':1, '流冷汗':1};
    var opts = [['','无（只放自身动作）']];
    names.forEach(function(n){
      opts.push([n, DEAD[n] ? (n + '（模型里没绑，选了没反应）') : n]);
    });
    PARTS.forEach(function(p){
      var row = addRow(partCard, {k:'pexpr.'+p[0], t:'select', label:p[1], opts:opts,
        d:'点这个部位时播哪个表情。选「无」就只放它自己的动作。'});
      // 行尾加一个「试」：不用真的去点她，直接让身体演一遍这个部位的反应
      var vb = els['pexpr.'+p[0]] && els['pexpr.'+p[0]].out;
      if (vb && vb.parentNode) {
        // 放在 .v 后面当 row 的第 4 列（CSS 里 .row 已经是 4 列）
        var t = document.createElement('button'); t.className = 'try'; t.textContent = '试';
        t.title = '让身体演一遍「点' + p[1] + '」的反应';
        t.addEventListener('click', function(){ cmd('part', p[0]); });
        vb.parentNode.insertBefore(t, vb.nextSibling);
      }
    });
    // （addSection 已经把这张卡挂到自己的 section 上了）
  }

  function statusLine(s){
    var f = (s.fps_now === null || s.fps_now === undefined) ? '—' : Number(s.fps_now).toFixed(1);
    document.getElementById('status').innerHTML =
      '<b>运行中</b>' +
      '<span>PID <i>' + s.pid + '</i></span>' +
      '<span>输出 <i>' + s.output + '</i></span>' +
      '<span>窗口 <i>' + s.window + '</i></span>' +
      '<span>帧率 <i>' + f + '</i> fps</span>';
  }
  function fill(s){
    statusLine(s);
    // Step 7：部位列表从后端来（顺序 = parts.tsv 里 part 行的顺序 = id）
    if (s.partNames && s.partNames.length) PARTS = s.partNames;
    // 十九：查找表没读到 / shader 编不过 → 下拉里不留 smaa（选了也不会生效）
    var aaSel = els.aa && els.aa.el;
    if (aaSel && s.smaa_ok === false) {
      for (var i = aaSel.options.length - 1; i >= 0; i--)
        if (aaSel.options[i].value === 'smaa') aaSel.remove(i);
    }
    if (s.exprNames && s.exprNames.length) {
      buildPartCard(s.exprNames);
      buildExprWall(s.exprNames);
    }
    if (s.planNames && s.planNames.length) buildPlanWall(s.planNames);
    buildDiag();
    if (diagLine && s.lastHit) {
      var h = s.lastHit;
      diagLine.textContent = h.id > 0
        ? ('最近一次：' + h.name + '（id ' + h.id + '） u=' + Number(h.u).toFixed(3) + '  v=' + Number(h.v).toFixed(3)
           + (h.say ? '　她说：' + h.say : '　（这一块没配台词）'))
        : '还没点过她';
    }
    if (!tabRestored) {
      tabRestored = true;
      var t0 = 0;
      try { t0 = parseInt(localStorage.getItem('miyu-tab') || '0', 10) || 0; } catch(e) {}
      // 索引越界就回到第一页 —— 兜底，省得以后加了组又变成「点了没反应」
      if (!(t0 >= 0 && t0 < sections.length)) t0 = 0;
      showSection(t0);
    }
    if (s.partExpr) PARTS.forEach(function(p){
      var e = els['pexpr.'+p[0]];
      if (e && e.el) { var val = s.partExpr[p[0]] || ''; e.el.value = val; }
    });
    SCHEMA.forEach(function(sec){
      sec.items.forEach(function(it){
        var e = els[it.k]; if (!e) return;
        var val = s[it.k];
        if (e.type === 'range') {
          e.el.value = val; e.out.textContent = Number(val).toFixed(2);
        } else if (e.type === 'bool') {
          var shown = it.invert ? !val : !!val;
          e.el.checked = shown; e.out.textContent = shown ? '开' : '关';
        } else {
          e.el.value = String(val); e.out.textContent = e.show(e.num ? parseInt(val, 10) : val);
        }
      });
    });
  }
  function load(){
    fetch('/state').then(function(r){ return r.json(); }).then(fill).catch(function(){
      document.getElementById('status').textContent = '连不上服务';
    });
  }
  load();
  // 每秒只刷顶栏（帧率是 main 里 5 秒一次的实测值，刷得再勤也只会变一次；
  // 不动控件值 —— 不然正在拖的滑块会被服务端值拽回去）
  setInterval(function(){
    fetch('/state').then(function(r){ return r.json(); }).then(statusLine).catch(function(){});
  }, 1000);

  document.getElementById('call').addEventListener('click', function(){
    post({say:'在', mood:60}).then(function(){ hint('叫了：在', true); })
      .catch(function(){ hint('连不上', false); });
  });
  document.getElementById('exprnext').addEventListener('click', function(){
    post({action:'expr_next'}).then(function(d){
      if (d && d.ok) hint('换了个表情', true); else hint('没换成', false);
    }).catch(function(){ hint('连不上', false); });
  });
  document.getElementById('reset').addEventListener('click', function(){
    post({action:'reset'}).then(function(d){
      if (d && d.ok) { hint('已恢复默认', true); load(); } else hint('没恢复', false);
    }).catch(function(){ hint('连不上', false); });
  });
  document.getElementById('restart').addEventListener('click', function(){
    hint('重启中…', false);
    fetch('/set', {method:'POST', body: JSON.stringify({action:'restart'})})
      .catch(function(){}).then(function(){
        setTimeout(function(){ hint('服务重启中，几秒后刷新', false); }, 400);
      });
  });
})();
</script>
</body></html>
)HTML";

// ── 一份极简的 JSON：只认「一个扁平对象」────────────────────────────
// settings.json 和 POST /set 的 body 都是这个形状，不需要递归下降那套（plan.cpp
// 那份要嵌套数组，才写得长）。字符串支持 \\ \" \n \t \u（\u 只按字符塞回去）。
enum VType { VNull, VNum, VBool, VStr };
struct Val {
    VType       t = VNull;
    double      n = 0.0;
    bool        b = false;
    std::string s;
};

struct Scan {
    const char* p;
    const char* e;
    void ws() { while (p < e && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) ++p; }
    bool str(std::string& out) {
        ws();
        if (p >= e || *p != '"') return false;
        ++p;
        out.clear();
        while (p < e) {
            const char c = *p++;
            if (c == '"') return true;
            if (c != '\\') { out.push_back(c); continue; }
            if (p >= e) return false;
            const char esc = *p++;
            switch (esc) {
                case 'n': out.push_back('\n'); break;
                case 't': out.push_back('\t'); break;
                case 'r': out.push_back('\r'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'u': {
                    // 只处理 \uXXXX 里能落进 UTF-8 的 BMP 字符；代理对按原样跳过
                    unsigned v = 0;
                    for (int i = 0; i < 4 && p < e; i++) {
                        const char h = *p++;
                        v <<= 4;
                        if (h >= '0' && h <= '9') v |= unsigned(h - '0');
                        else if (h >= 'a' && h <= 'f') v |= unsigned(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') v |= unsigned(h - 'A' + 10);
                    }
                    if (v < 0x80) out.push_back(char(v));
                    else if (v < 0x800) {
                        out.push_back(char(0xC0 | (v >> 6)));
                        out.push_back(char(0x80 | (v & 0x3F)));
                    } else {
                        out.push_back(char(0xE0 | (v >> 12)));
                        out.push_back(char(0x80 | ((v >> 6) & 0x3F)));
                        out.push_back(char(0x80 | (v & 0x3F)));
                    }
                    break;
                }
                default: out.push_back(esc); break;
            }
        }
        return false;
    }
    bool value(Val& v) {
        ws();
        if (p >= e) return false;
        if (*p == '"') { v.t = VStr; return str(v.s); }
        if (*p == 't') { if (e - p < 4) return false; p += 4; v.t = VBool; v.b = true;  return true; }
        if (*p == 'f') { if (e - p < 5) return false; p += 5; v.t = VBool; v.b = false; return true; }
        if (*p == 'n') { if (e - p < 4) return false; p += 4; v.t = VNull; return true; }
        {
            char* end = nullptr;
            const double d = std::strtod(p, &end);
            if (end == p) return false;
            p = end;
            v.t = VNum;
            v.n = d;
            return true;
        }
    }
};

// 解析一个扁平对象；失败返回 false（键值对可能已经被填了一半，调用方不看）。
bool parseFlat(const std::string& text, std::vector<std::pair<std::string, Val>>& out) {
    Scan s{text.data(), text.data() + text.size()};
    s.ws();
    if (s.p >= s.e || *s.p != '{') return false;
    ++s.p;
    s.ws();
    if (s.p < s.e && *s.p == '}') return true;
    while (s.p < s.e) {
        std::string k;
        if (!s.str(k)) return false;
        s.ws();
        if (s.p >= s.e || *s.p != ':') return false;
        ++s.p;
        Val v;
        if (!s.value(v)) return false;
        out.emplace_back(k, v);
        s.ws();
        if (s.p < s.e && *s.p == ',') { ++s.p; continue; }
        if (s.p < s.e && *s.p == '}') return true;
        return false;
    }
    return false;
}

std::string jsonEsc(const std::string& in) {
    std::string o;
    o.reserve(in.size() + 8);
    for (char c : in) {
        switch (c) {
            case '"':  o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n";  break;
            case '\r': o += "\\r";  break;
            case '\t': o += "\\t";  break;
            default:   o.push_back(c); break;
        }
    }
    return o;
}

float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

// ── 十八：aa 的取值归一 ─────────────────────────────────────────────
// 只认五档：off / fxaa / smaa / ssaa2x / ssaa3x。
// 老 key `ssaa` 的写法（off / 2x / 3x）和直接写 "2x"/"3x" 一起收 —— 旧的 settings.json、
// 旧脚本、旧书签里的 POST 都不会炸。返回 false = 不认识，调用方忽略（不写坏值）。
bool normAa(const std::string& in, std::string& out) {
    if (in == "off")  { out = "off";    return true; }
    if (in == "fxaa") { out = "fxaa";   return true; }
    if (in == "smaa") { out = "smaa";   return true; }
    if (in == "ssaa2x" || in == "2x") { out = "ssaa2x"; return true; }
    if (in == "ssaa3x" || in == "3x") { out = "ssaa3x"; return true; }
    return false;
}

// ── 状态 ────────────────────────────────────────────────────────────
int         gFd       = -1;
int         gPort     = 0;
Settings    gSet;
ApplyFn     gApply    = nullptr;
CmdFn       gCmd      = nullptr;
std::string gOutput   = "?";
int         gWinW     = 0, gWinH = 0;
double      gFps      = -1.0;    // 十八：main 的 framePace 每 5 秒喂一次实测帧率
std::string gPath     = "settings.json";
bool        gSmaaOk   = true;    // 十九：postfx 报告 smaa 能不能用（main 启动时喂一次）
std::vector<std::string> gExprs;
static std::vector<std::string> gPlans;
static std::string gLastSay;   // 最近一次点击她说的那句话（诊断用）   // 二十五：plans/*.json 名单
size_t      gExprIdx  = 0;

struct Conn {
    int         fd = -1;
    std::string in;
    double      t0 = 0.0;
    bool        inSet = false;   // 这个连接是不是 POST /set（日志里要打原文）
};
constexpr int kMaxConn = 4;
Conn gConn[kMaxConn];

// ── 落盘 ────────────────────────────────────────────────────────────
// ── 二十五 → Step 5：部位 → 表情 ──────────────────────────────────
// 数据在 **state/parts.tsv 的 expr 段**（唯一真源）。原来还有一个
// state/part-react.tsv，2026-11-10 挪去 state/legacy/ 退休了 —— 两份数据
// 就是今晚所有串位的病因。
//
// 这个 map 不再是数据源，只是**设置页的镜像**：启动时从 partapi 取初值，
// handleSet 改的时候同时调 partapi::setExprOverride（内存立刻生效）+ saveExprRows（落盘）。
void loadPartReact() {
    // 二十五：真源换成 state/parts.tsv 的 expr 段（partapi 在启动时读进来）。
    // 这里只把初值填进内存 map 给设置页显示用；**真正生效的是 partapi**。
    // part-react.tsv 从 2026-11-10 22:2x 起退休（挪到 state/legacy/），
    // 留着它就是「两处各有一份数据」，今晚所有串位都是这个病因。
    int n = 0;
    for (int id = 1; id <= partapi::count(); ++id) {
        const std::string v = partapi::exprValueOf(id);
        if (!v.empty()) {
            gSet.partExpr[partapi::nameOf(id)] = v;
            ++n;
        }
    }
    logw("部位→表情：从 parts.tsv 读进 %d 条", n);
}

void savePartReact() {
    // 二十五：写回 state/parts.tsv 的 expr 段。saveExprRows 内部是
    // 「非 expr 行原样保留 → 写 .tmp → 重解析校验 → 通过才 rename」，
    // 校验不过不落盘（子代理那边单独测过：非 expr 的 98 行 sha256 一字节不差）。
    std::string err;
    if (!partapi::saveExprRows("state/parts.tsv", &err))
        logw("部位→表情存不下来：%s", err.c_str());
}

void saveSettings() {
    const std::string tmp = gPath + ".tmp";
    {
        std::ofstream f(tmp, std::ios::trunc);
        if (!f) { logw("设置存不下来：%s", tmp.c_str()); return; }
        f.setf(std::ios::fixed);
        f.precision(3);
        f << "{\"scale\":" << gSet.scale
          << ",\"alpha\":" << gSet.alpha
          << ",\"tap\":" << (gSet.tap ? "true" : "false")
          << ",\"expr\":" << (gSet.expr ? "true" : "false")
          << ",\"idle\":" << gSet.idle
          << ",\"idleStyle\":\"" << gSet.idleStyle << "\""
          << ",\"look\":" << gSet.look
          << ",\"lookOn\":" << (gSet.lookOn ? "true" : "false")
          << ",\"blink\":" << gSet.blink
          << ",\"mouth\":" << gSet.mouth
          << ",\"exprHold\":" << gSet.exprHold
          << ",\"fps\":" << gSet.fps
          << ",\"voice\":" << (gSet.voice ? "true" : "false")
          << ",\"vol\":" << gSet.vol
          << ",\"corner\":\"" << gSet.corner << "\""
          << ",\"hide\":" << (gSet.hide ? "true" : "false")
          << ",\"aa\":\"" << gSet.aa << "\""
          << ",\"shadow\":" << gSet.shadow
          << ",\"rim\":" << gSet.rim
          << ",\"particles\":" << gSet.particles
          // 二十：拖动体感三件 + 时间感
          << ",\"grab\":" << (gSet.grab ? "true" : "false")
          << ",\"inertia\":" << (gSet.inertia ? "true" : "false")
          << ",\"edgeSoft\":" << (gSet.edgeSoft ? "true" : "false")
          << ",\"timeAware\":" << (gSet.timeAware ? "true" : "false")
          << ",\"dragSway\":" << gSet.dragSway
          << ",\"swayIdle\":" << (gSet.swayIdle ? "true" : "false")
          << ",\"swayTilt\":" << gSet.swayTilt
          << ",\"chestSway\":" << gSet.chestSway
          // 2026-11-10 22:3x：legSway 一直漏在这儿 —— web.h 有字段、页面有滑块、
          // handleSet 也认，就是没落盘，重启回 1.0。补上。
          << ",\"legSway\":" << gSet.legSway
          << ",\"dragSpring\":" << gSet.dragSpring
          << ",\"tapPart\":" << (gSet.tapPart ? "true" : "false")
          << ",\"combo\":" << (gSet.combo ? "true" : "false")
          << ",\"dblTap\":" << (gSet.dblTap ? "true" : "false") << "}\n";
    }
    if (std::rename(tmp.c_str(), gPath.c_str()) != 0)
        logw("设置存不下来（rename 失败）：%s", gPath.c_str());
}

void loadSettings() {
    std::ifstream f(gPath);
    if (!f) { logw("没有 %s，用默认值", gPath.c_str()); return; }
    std::stringstream ss;
    ss << f.rdbuf();
    std::vector<std::pair<std::string, Val>> kv;
    if (!parseFlat(ss.str(), kv)) { logw("%s 不是合法 JSON，用默认值", gPath.c_str()); return; }
    bool sawAa = false;          // 十八：见上面 aa / ssaa 的先后规矩
    for (auto& [k, v] : kv) {
        if (k == "scale" && v.t == VNum) gSet.scale = clampf(float(v.n), 0.4f, 1.6f);
        else if (k == "alpha" && v.t == VNum) gSet.alpha = clampf(float(v.n), 0.3f, 1.0f);
        else if (k == "idle" && v.t == VNum) gSet.idle = clampf(float(v.n), 0.0f, 3.0f);
        else if (k == "tap" && v.t == VBool) gSet.tap = v.b;
        else if (k == "expr" && v.t == VBool) gSet.expr = v.b;
        else if (k == "hide" && v.t == VBool) gSet.hide = v.b;
        else if (k == "lookOn" && v.t == VBool) gSet.lookOn = v.b;
        else if (k == "voice" && v.t == VBool) gSet.voice = v.b;
        else if (k == "look" && v.t == VNum) gSet.look = clampf(float(v.n), 0.0f, 2.0f);
        else if (k == "blink" && v.t == VNum) gSet.blink = clampf(float(v.n), 0.0f, 2.0f);
        else if (k == "mouth" && v.t == VNum) gSet.mouth = clampf(float(v.n), 0.5f, 3.0f);
        else if (k == "exprHold" && v.t == VNum) gSet.exprHold = clampf(float(v.n), 0.2f, 5.0f);
        else if (k == "vol" && v.t == VNum) gSet.vol = clampf(float(v.n), 0.0f, 1.0f);
        else if (k == "fps" && v.t == VNum) {
            const int n = int(v.n);
            if (n == 30 || n == 60 || n == 120 || n == 200 || n == 240) gSet.fps = n;
        }
        else if (k == "idleStyle" && v.t == VStr) {
            const std::string c = v.s;
            if (c == "quiet" || c == "natural" || c == "lively") gSet.idleStyle = c;
        }
        else if (k == "corner" && v.t == VStr) {
            const std::string c = v.s;
            if (c == "tl" || c == "tr" || c == "bl" || c == "br" || c == "center" || c == "free") gSet.corner = c;
        }
        // ── 第十七节：抗锯齿 + 后处理三件 ──────────────────────────
        else if (k == "shadow" && v.t == VNum) gSet.shadow = clampf(float(v.n), 0.0f, 1.0f);
        else if (k == "rim" && v.t == VNum) gSet.rim = clampf(float(v.n), 0.0f, 1.0f);
        else if (k == "particles" && v.t == VNum) gSet.particles = clampf(float(v.n), 0.0f, 1.0f);
        // ── 二十：拖动体感三件 + 时间感（四个都是 bool，缺了就用默认 true）──
        else if (k == "grab" && v.t == VBool) gSet.grab = v.b;
        else if (k == "inertia" && v.t == VBool) gSet.inertia = v.b;
        else if (k == "edgeSoft" && v.t == VBool) gSet.edgeSoft = v.b;
        else if (k == "timeAware" && v.t == VBool) gSet.timeAware = v.b;
        // 二十之二：摆动两件
        else if (k == "dragSway" && v.t == VNum) gSet.dragSway = clampf(float(v.n), 0.0f, 2.0f);
        else if (k == "swayIdle" && v.t == VBool) gSet.swayIdle = v.b;
        else if (k == "swayTilt" && v.t == VNum) gSet.swayTilt = clampf(float(v.n), 0.0f, 4.5f);
        else if (k == "chestSway" && v.t == VNum) gSet.chestSway = clampf(float(v.n), 0.0f, 3.0f);
        else if (k == "legSway" && v.t == VNum) gSet.legSway = clampf(float(v.n), 0.0f, 3.0f);
        else if (k == "dragSpring" && v.t == VNum) gSet.dragSpring = clampf(float(v.n), 0.0f, 2.0f);
        // 二十一：点哪动哪
        else if (k == "tapPart" && v.t == VBool) gSet.tapPart = v.b;
        else if (k == "combo" && v.t == VBool) gSet.combo = v.b;
        else if (k == "dblTap" && v.t == VBool) gSet.dblTap = v.b;
        // 十八：aa 是新主键；ssaa 是旧键（映射过来，读了不炸）。
        // 两个都出现时 aa 赢 —— 所以先记「见过 aa」，旧键只在没见 aa 时生效。
        else if (k == "aa" && v.t == VStr) {
            std::string c;
            if (normAa(v.s, c)) { gSet.aa = c; sawAa = true; }
            else logw("%s 里 aa=%s 不认识，用默认 %s", gPath.c_str(), v.s.c_str(), gSet.aa.c_str());
        }
        else if (k == "ssaa" && v.t == VStr) {
            std::string c;
            if (normAa(v.s, c)) { if (!sawAa) gSet.aa = c; }
            else logw("%s 里 ssaa=%s 不认识，忽略", gPath.c_str(), v.s.c_str());
        }
    }
    logw("读回 %s：scale=%.2f alpha=%.2f tap=%d expr=%d idle=%.2f(%s) corner=%s hide=%d "
         "look=%.2f/%d blink=%.2f mouth=%.2f exprHold=%.2f fps=%d voice=%d vol=%.2f "
         "aa=%s shadow=%.2f rim=%.2f particles=%.2f legSway=%.2f",
         gPath.c_str(), gSet.scale, gSet.alpha, int(gSet.tap), int(gSet.expr),
         gSet.idle, gSet.idleStyle.c_str(), gSet.corner.c_str(), int(gSet.hide),
         gSet.look, int(gSet.lookOn), gSet.blink, gSet.mouth, gSet.exprHold,
         gSet.fps, int(gSet.voice), gSet.vol,
         gSet.aa.c_str(), gSet.shadow, gSet.rim, gSet.particles, gSet.legSway);
}

// ── 往 ctl 的 socket 推一条（说一句话 / 切个表情）────────────────────
// 不新建协议：自己当一次客户端，连 $XDG_RUNTIME_DIR/miyu-body.sock 写一行。
void sendToCtl(const std::string& line) {
    const char* path = ctl::sockPath();
    if (!path || std::strcmp(path, "(未建)") == 0) { logw("ctl socket 没建，丢掉：%s", line.c_str()); return; }
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return;
    struct sockaddr_un sa;
    std::memset(&sa, 0, sizeof sa);
    sa.sun_family = AF_UNIX;
    std::snprintf(sa.sun_path, sizeof sa.sun_path, "%s", path);
    if (::connect(fd, (struct sockaddr*)&sa, sizeof sa) != 0) {
        logw("连不上 %s（%s），丢掉：%s", path, std::strerror(errno), line.c_str());
        ::close(fd);
        return;
    }
    const std::string msg = line + "\n";
    ssize_t off = 0;
    while (off < (ssize_t)msg.size()) {
        const ssize_t n = ::send(fd, msg.data() + off, msg.size() - size_t(off), MSG_NOSIGNAL);
        if (n <= 0) break;
        off += n;
    }
    ::close(fd);
}

// ── HTTP 应答 ───────────────────────────────────────────────────────
void sendAll(int fd, const std::string& s) {
    size_t off = 0;
    while (off < s.size()) {
        const ssize_t n = ::send(fd, s.data() + off, s.size() - off, MSG_NOSIGNAL);
        if (n <= 0) return;
        off += size_t(n);
    }
}

std::string resp(int code, const char* ctype, const std::string& body) {
    const char* reason = code == 200 ? "OK" : (code == 404 ? "Not Found" : "Bad Request");
    std::string h = "HTTP/1.1 ";
    h += std::to_string(code);
    h += " ";
    h += reason;
    h += "\r\nContent-Type: ";
    h += ctype;
    h += "\r\nContent-Length: ";
    h += std::to_string(body.size());
    h += "\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n";
    h += body;
    return h;
}

std::string stateJson() {
    std::string s;
    char win[32];
    std::snprintf(win, sizeof win, "%dx%d", gWinW, gWinH);
    s  = "{\"ok\":true,\"pid\":" + std::to_string(int(getpid()));
    s += ",\"output\":\"" + jsonEsc(gOutput) + "\"";
    s += ",\"window\":\"" + std::string(win) + "\"";
    s += ",\"scale\":" + std::to_string(gSet.scale);
    s += ",\"alpha\":" + std::to_string(gSet.alpha);
    s += ",\"tap\":" + std::string(gSet.tap ? "true" : "false");
    s += ",\"expr\":" + std::string(gSet.expr ? "true" : "false");
    s += ",\"idle\":" + std::to_string(gSet.idle);
    s += ",\"idleStyle\":\"" + gSet.idleStyle + "\"";
    s += ",\"look\":" + std::to_string(gSet.look);
    s += ",\"lookOn\":" + std::string(gSet.lookOn ? "true" : "false");
    s += ",\"blink\":" + std::to_string(gSet.blink);
    s += ",\"mouth\":" + std::to_string(gSet.mouth);
    s += ",\"exprHold\":" + std::to_string(gSet.exprHold);
    s += ",\"fps\":" + std::to_string(gSet.fps);
    s += ",\"voice\":" + std::string(gSet.voice ? "true" : "false");
    s += ",\"vol\":" + std::to_string(gSet.vol);
    s += ",\"corner\":\"" + gSet.corner + "\"";
    // Step 7：部位列表（name + 中文标签，顺序 = id），前端拿它生成「点击反应」那组。
    // 加一个部位 = state/parts.tsv 加几行，这个数组自动变长。
    s += ",\"partNames\":[";
    bool firstPart = true;
    // 顺序：有 display 记录就按它（设置页从上到下），否则按 id。
    // 过滤：只下发「点得出反应」的部位 —— `other` 这种纯占位（没参数没表情）不该出现在页面上。
    auto emitPart = [&](int i) {
        if (!partapi::hasAction(i)) return;
        if (!firstPart) s += ",";
        firstPart = false;
        s += "[\"" + std::string(partapi::nameOf(i)) + "\",\"" +
             std::string(partapi::labelOf(i)) + "\"]";
    };
    std::vector<char> shown(static_cast<size_t>(partapi::count()) + 1, 0);
    for (unsigned char d : partapi::reg().display) { emitPart(static_cast<int>(d)); shown[d] = 1; }
    // display 里没写到的部位（新加的部位）追加到末尾 —— 加部位不用改这条 display 行
    for (int i = 1; i <= partapi::count(); ++i) if (!shown[static_cast<size_t>(i)]) emitPart(i);
    s += "]";
    // 二十五：最近一次判定的部位（设置页「诊断」显示，定位串位用）
    {
        const int id = partpoke::gLastHitId;
        s += ",\"lastHit\":{\"id\":" + std::to_string(id);
        s += ",\"name\":\"";
        s += (id > 0 && id < partpoke::kZoneMax) ? partapi::nameOf(id) : "";
        s += "\",\"u\":" + std::to_string(float(partpoke::gLastHitU));
        s += ",\"v\":" + std::to_string(float(partpoke::gLastHitV));
        s += ",\"say\":\"" + jsonEsc(gLastSay) + "\"}";
    }
    // 二十五：部位→表情（设置页那一组用它渲染下拉）
    {
        s += ",\"partExpr\":{";
        bool first = true;
        for (const auto& [part, ex] : gSet.partExpr) {
            if (!first) s += ",";
            first = false;
            s += "\"" + part + "\":\"" + ex + "\"";
        }
        s += "}";
        s += ",\"planNames\":[";
        for (size_t i = 0; i < gPlans.size(); ++i) {
            if (i) s += ",";
            s += "\"" + gPlans[i] + "\"";
        }
        s += "]";
        s += ",\"exprNames\":[";
        for (size_t i = 0; i < gExprs.size(); ++i) {
            if (i) s += ",";
            s += "\"" + gExprs[i] + "\"";
        }
        s += "]";
    }
    s += ",\"hide\":" + std::string(gSet.hide ? "true" : "false");
    s += ",\"aa\":\"" + gSet.aa + "\"";
    // 十九：页面靠它决定「下拉里留不留 smaa 这一项」
    s += ",\"smaa_ok\":" + std::string(gSmaaOk ? "true" : "false");
    s += ",\"shadow\":" + std::to_string(gSet.shadow);
    s += ",\"rim\":" + std::to_string(gSet.rim);
    s += ",\"particles\":" + std::to_string(gSet.particles);
    // 二十：拖动体感三件 + 时间感
    s += ",\"grab\":" + std::string(gSet.grab ? "true" : "false");
    s += ",\"inertia\":" + std::string(gSet.inertia ? "true" : "false");
    s += ",\"edgeSoft\":" + std::string(gSet.edgeSoft ? "true" : "false");
    s += ",\"timeAware\":" + std::string(gSet.timeAware ? "true" : "false");
    // 二十之二：摆动两件
    s += ",\"dragSway\":" + std::to_string(gSet.dragSway);
    s += ",\"swayIdle\":" + std::string(gSet.swayIdle ? "true" : "false");
    s += ",\"swayTilt\":" + std::to_string(gSet.swayTilt);
    s += ",\"legSway\":" + std::to_string(gSet.legSway);
    s += ",\"chestSway\":" + std::to_string(gSet.chestSway);
    s += ",\"dragSpring\":" + std::to_string(gSet.dragSpring);
    s += ",\"tapPart\":" + std::string(gSet.tapPart ? "true" : "false");
    s += ",\"combo\":" + std::string(gSet.combo ? "true" : "false");
    s += ",\"dblTap\":" + std::string(gSet.dblTap ? "true" : "false");
    s += ",\"fps_now\":" + (gFps < 0.0 ? std::string("null") : std::to_string(gFps));
    s += ",\"port\":" + std::to_string(gPort);
    s += ",\"expr_next\":\"" + jsonEsc(gExprs.empty() ? "" : gExprs[gExprIdx % gExprs.size()]) + "\"";
    s += "}";
    return s;
}

// POST /set：改设置 → 立刻回调生效 → 落盘 → 回 {"ok":true,"applied":[...]}
std::string handleSet(const std::string& body, bool logIt) {
    std::vector<std::pair<std::string, Val>> kv;
    if (!parseFlat(body, kv)) {
        if (logIt) logw("POST /set 解析失败：%s", body.c_str());
        return "{\"ok\":false,\"error\":\"bad json\"}";
    }
    std::vector<std::string> changed;      // 需要回调生效的字段
    std::vector<std::string> applied;      // 回给页面的 applied
    bool persist = false;
    std::string say;
    bool haveSay = false;
    bool haveCmd = false;
    std::string cmdName, cmdArg;
    double mood = -1.0;
    bool wantRestart = false, wantExprNext = false, wantReset = false;

    for (auto& [k, v] : kv) {
        if (k == "scale" && v.t == VNum) {
            const float nv = clampf(float(v.n), 0.4f, 1.6f);
            if (nv != gSet.scale) { gSet.scale = nv; changed.push_back("scale"); }
            continue;
        }
        if (k == "alpha" && v.t == VNum) {
            const float nv = clampf(float(v.n), 0.3f, 1.0f);
            if (nv != gSet.alpha) { gSet.alpha = nv; changed.push_back("alpha"); }
            continue;
        }
        if (k == "idle" && v.t == VNum) {
            const float nv = clampf(float(v.n), 0.0f, 3.0f);
            if (nv != gSet.idle) { gSet.idle = nv; changed.push_back("idle"); }
            continue;
        }
        // ── 第十六节新增的九项 ──────────────────────────────────────
        if (k == "look" && v.t == VNum) {
            const float nv = clampf(float(v.n), 0.0f, 2.0f);
            if (nv != gSet.look) { gSet.look = nv; changed.push_back("look"); }
            continue;
        }
        if (k == "blink" && v.t == VNum) {
            const float nv = clampf(float(v.n), 0.0f, 2.0f);
            if (nv != gSet.blink) { gSet.blink = nv; changed.push_back("blink"); }
            continue;
        }
        if (k == "mouth" && v.t == VNum) {
            const float nv = clampf(float(v.n), 0.5f, 3.0f);
            if (nv != gSet.mouth) { gSet.mouth = nv; changed.push_back("mouth"); }
            continue;
        }
        if (k == "exprHold" && v.t == VNum) {
            const float nv = clampf(float(v.n), 0.2f, 5.0f);
            if (nv != gSet.exprHold) { gSet.exprHold = nv; changed.push_back("exprHold"); }
            continue;
        }
        if (k == "vol" && v.t == VNum) {
            const float nv = clampf(float(v.n), 0.0f, 1.0f);
            if (nv != gSet.vol) { gSet.vol = nv; changed.push_back("vol"); }
            continue;
        }
        if (k == "fps" && v.t == VNum) {
            const int nv = int(v.n + (v.n < 0 ? -0.5 : 0.5));
            // 2026-11-09 23:4x：他要五档。副屏 HDMI-A-3 是 200Hz、主屏 eDP-1 是 240Hz，
            // 所以 240 只在主屏有意义（副屏会被 vsync 卡在 200）。
            if (nv != 30 && nv != 60 && nv != 120 && nv != 200 && nv != 240) {
                if (logIt) logw("POST /set fps=%d 只认 30/60/120/200/240，忽略", nv);
            } else if (nv != gSet.fps) {
                gSet.fps = nv; changed.push_back("fps");
            }
            continue;
        }
        if (k == "idleStyle" && v.t == VStr) {
            const std::string c = v.s;
            if (c != "quiet" && c != "natural" && c != "lively") {
                if (logIt) logw("POST /set idleStyle=%s 不认识，忽略", c.c_str());
            } else if (c != gSet.idleStyle) {
                gSet.idleStyle = c; changed.push_back("idleStyle");
            }
            continue;
        }
        if (k == "lookOn" && v.t == VBool) {
            if (v.b != gSet.lookOn) { gSet.lookOn = v.b; changed.push_back("lookOn"); }
            continue;
        }
        if (k == "voice" && v.t == VBool) {
            if (v.b != gSet.voice) { gSet.voice = v.b; changed.push_back("voice"); }
            continue;
        }
        if (k == "tap" && v.t == VBool) {
            if (v.b != gSet.tap) { gSet.tap = v.b; changed.push_back("tap"); }
            continue;
        }
        if (k == "expr" && v.t == VBool) {
            if (v.b != gSet.expr) { gSet.expr = v.b; changed.push_back("expr"); }
            continue;
        }
        // 二十五：部位→表情。key 形如 "pexpr.chest"，值是表情名（空串 = 不放表情）
        if (k.rfind("pexpr.", 0) == 0 && v.t == VStr) {
            const std::string part = k.substr(6);
            if (!part.empty() && gSet.partExpr[part] != v.s) {
                gSet.partExpr[part] = v.s;
                // 二十五：立刻让人生效（覆盖层），再落盘。少了这行，设置页改表情
                // 就只是「改了一张没人读的表」——今晚 22:2x 验出来就是这个。
                partapi::setExprOverride(partapi::indexOf(part), v.s);
                savePartReact();
                changed.push_back(k);
                logw("部位→表情：%s = 「%s」", part.c_str(), v.s.empty() ? "(无)" : v.s.c_str());
            }
            continue;
        }
        if (k == "hide" && v.t == VBool) {
            if (v.b != gSet.hide) { gSet.hide = v.b; changed.push_back("hide"); }
            continue;
        }
        if (k == "corner" && v.t == VStr) {
            const std::string c = v.s;
            if (c == "tl" || c == "tr" || c == "bl" || c == "br" || c == "center" || c == "free") {
                if (c != gSet.corner) { gSet.corner = c; changed.push_back("corner"); }
            } else {
                if (logIt) logw("POST /set corner=%s 不认识，忽略", c.c_str());
            }
            continue;
        }
        // ── 第十七节新增四项 ────────────────────────────────────
        if (k == "shadow" && v.t == VNum) {
            const float nv = clampf(float(v.n), 0.0f, 1.0f);
            if (nv != gSet.shadow) { gSet.shadow = nv; changed.push_back("shadow"); }
            continue;
        }
        if (k == "rim" && v.t == VNum) {
            const float nv = clampf(float(v.n), 0.0f, 1.0f);
            if (nv != gSet.rim) { gSet.rim = nv; changed.push_back("rim"); }
            continue;
        }
        if (k == "particles" && v.t == VNum) {
            const float nv = clampf(float(v.n), 0.0f, 1.0f);
            if (nv != gSet.particles) { gSet.particles = nv; changed.push_back("particles"); }
            continue;
        }
        // 十八：aa 主键 + ssaa 旧键（旧页面/旧脚本照旧能改）
        // ── 二十：拖动体感三件 + 时间感（四个 bool，改完立刻生效）──────
        if (k == "grab" && v.t == VBool) {
            if (v.b != gSet.grab) { gSet.grab = v.b; changed.push_back("grab"); }
            continue;
        }
        if (k == "inertia" && v.t == VBool) {
            if (v.b != gSet.inertia) { gSet.inertia = v.b; changed.push_back("inertia"); }
            continue;
        }
        if (k == "edgeSoft" && v.t == VBool) {
            if (v.b != gSet.edgeSoft) { gSet.edgeSoft = v.b; changed.push_back("edgeSoft"); }
            continue;
        }
        if (k == "timeAware" && v.t == VBool) {
            if (v.b != gSet.timeAware) { gSet.timeAware = v.b; changed.push_back("timeAware"); }
            continue;
        }
        if (k == "dblTap" && v.t == VBool) {
            if (v.b != gSet.dblTap) { gSet.dblTap = v.b; changed.push_back("dblTap"); }
            continue;
        }
        if (k == "combo" && v.t == VBool) {
            if (v.b != gSet.combo) { gSet.combo = v.b; changed.push_back("combo"); }
            continue;
        }
        if (k == "tapPart" && v.t == VBool) {
            if (v.b != gSet.tapPart) { gSet.tapPart = v.b; changed.push_back("tapPart"); }
            continue;
        }
        // ── 二十之二：摆动两件（每帧读，不需要额外动作）──────────────
        if (k == "dragSway" && v.t == VNum) {
            const float nv = clampf(float(v.n), 0.0f, 2.0f);
            if (nv != gSet.dragSway) { gSet.dragSway = nv; changed.push_back("dragSway"); }
            continue;
        }
        if (k == "swayIdle" && v.t == VBool) {
            if (v.b != gSet.swayIdle) { gSet.swayIdle = v.b; changed.push_back("swayIdle"); }
            continue;
        }
        if (k == "swayTilt" && v.t == VNum) {
            const float nv = clampf(float(v.n), 0.0f, 4.5f);
            if (nv != gSet.swayTilt) { gSet.swayTilt = nv; changed.push_back("swayTilt"); }
            continue;
        }
        if (k == "legSway" && v.t == VNum) {
            const float nv = clampf(float(v.n), 0.0f, 3.0f);
            if (nv != gSet.legSway) { gSet.legSway = nv; changed.push_back("legSway"); }
            continue;
        }
        if (k == "chestSway" && v.t == VNum) {
            const float nv = clampf(float(v.n), 0.0f, 3.0f);
            if (nv != gSet.chestSway) { gSet.chestSway = nv; changed.push_back("chestSway"); }
            continue;
        }
        if (k == "dragSpring" && v.t == VNum) {
            const float nv = clampf(float(v.n), 0.0f, 2.0f);
            if (nv != gSet.dragSpring) { gSet.dragSpring = nv; changed.push_back("dragSpring"); }
            continue;
        }
        if ((k == "aa" || k == "ssaa") && v.t == VStr) {
            std::string c;
            if (!normAa(v.s, c)) {
                if (logIt) logw("POST /set %s=%s 只认 off/fxaa/smaa/ssaa2x/ssaa3x"
                                "（旧写法 off/2x/3x 也认），忽略",
                                k.c_str(), v.s.c_str());
            } else if (c == "smaa" && !gSmaaOk) {
                // 查找表没读到：不能留一个「选了但什么也不发生」的档
                if (logIt) logw("POST /set %s=smaa 但 smaa 不可用（查找表没读到），按 ssaa2x 落",
                                k.c_str());
                c = "ssaa2x";
                if (c != gSet.aa) { gSet.aa = c; changed.push_back("aa"); }
            } else if (c != gSet.aa) {
                gSet.aa = c; changed.push_back("aa");
            }
            continue;
        }
        // 二十五：动作按钮。{"cmd":"part","arg":"chest"} → 让主体「试一下」这个部位
        // （和真的点它走同一条 partPokeStart），不动任何设置、不落盘。
        if (k == "cmd" && v.t == VStr) { cmdName = v.s; haveCmd = true; continue; }
        if (k == "arg" && v.t == VStr) { cmdArg = v.s; continue; }
        if (k == "say" && v.t == VStr) { say = v.s; haveSay = true; continue; }
        if (k == "mood" && v.t == VNum) { mood = v.n; continue; }
        if (k == "action" && v.t == VStr) {
            if (v.s == "restart") wantRestart = true;
            else if (v.s == "expr_next") wantExprNext = true;
            else if (v.s == "reset") wantReset = true;
            continue;
        }
        // 不认识的键：记一行，别把整包丢掉（页面/脚本将来多传字段不该报错）
        if (logIt) logw("POST /set 不认识的键：%s", k.c_str());
    }

    // ── reset：全部回默认，并且**每一项都回调一遍**（否则页面显示对、行为还是旧的）──
    if (wantReset) {
        const Settings d;               // 结构体默认值就是出厂设置
        gSet = d;
        // MIYU_NO_TAP 是命令行上的一次性覆盖，reset 也得听它的（和启动时同一套规矩）
        if (getenv("MIYU_NO_TAP")) gSet.tap = false;
        static const char* kAll[] = {
            "scale", "alpha", "tap", "expr", "idle", "corner", "hide",
            "idleStyle", "look", "lookOn", "blink", "mouth", "exprHold", "fps",
            "voice", "vol", "aa", "shadow", "rim", "particles",
            "grab", "inertia", "edgeSoft", "timeAware", "dragSway", "swayIdle",
            "swayTilt", "dragSpring", "tapPart",
        };
        changed.clear();
        for (const char* k : kAll) changed.push_back(k);
        logw("reset：全部设置恢复默认（%zu 项）", changed.size());
    }

    if (haveCmd && gCmd) {
        gCmd(cmdName.c_str(), cmdArg.c_str());
        applied.push_back(cmdName);
    }

    if (!changed.empty()) persist = true;

    // 先让每一层真的生效，再落盘：落盘失败不影响这一轮的生效。
    for (const auto& c : changed) {
        if (gApply) gApply(c.c_str());
        applied.push_back(c);
    }
    if (persist) saveSettings();

    if (haveSay || mood >= 0.0) {
        std::string m = "{";
        if (haveSay) m += "\"say\":\"" + jsonEsc(say) + "\"";
        if (mood >= 0.0) { if (haveSay) m += ","; m += "\"mood\":" + std::to_string(int(mood)); }
        m += "}";
        sendToCtl(m);
        applied.push_back("say");
    }
    if (wantExprNext) {
        if (gExprs.empty()) {
            logw("没有表情可切（main 没扫到 Exp3）");
        } else {
            gExprIdx = (gExprIdx + 1) % gExprs.size();
            sendToCtl("{\"expr\":\"" + jsonEsc(gExprs[gExprIdx]) + "\"}");
            logw("切表情 → %s", gExprs[gExprIdx].c_str());
            applied.push_back("expr_next");
        }
    }
    if (wantRestart) {
        // system() 会阻塞到命令返回，所以把 systemctl 挂到后台再交出去：
        // 重启要好几秒，不能让她在这几秒里不动。
        logw("重启服务：systemctl --user restart miyu-body（后台）");
        const int rc = system("( systemctl --user restart miyu-body >/dev/null 2>&1 & )");
        (void)rc;
        applied.push_back("restart");
    }

    if (logIt) {
        if (applied.empty()) logw("POST /set %s → 没有可应用的键", body.c_str());
        else logw("POST /set %s → 已应用", body.c_str());
    }

    std::string out = "{\"ok\":true,\"applied\":[";
    for (size_t i = 0; i < applied.size(); i++) {
        if (i) out += ",";
        out += "\"" + applied[i] + "\"";
    }
    out += "]}";
    return out;
}

// 一个连接的完整请求到了没：0 还没读完，1 读完了（resp 已填），-1 该掐
int tryHandle(Conn& c, std::string& respOut) {
    const size_t hdrEnd = c.in.find("\r\n\r\n");
    if (hdrEnd == std::string::npos) return 0;

    // 请求行
    const size_t lineEnd = c.in.find("\r\n");
    if (lineEnd == std::string::npos) return -1;
    const std::string line = c.in.substr(0, lineEnd);
    const size_t sp1 = line.find(' ');
    const size_t sp2 = sp1 == std::string::npos ? std::string::npos : line.find(' ', sp1 + 1);
    if (sp1 == std::string::npos || sp2 == std::string::npos) return -1;
    const std::string method = line.substr(0, sp1);
    std::string path = line.substr(sp1 + 1, sp2 - sp1 - 1);
    const size_t q = path.find('?');
    if (q != std::string::npos) path = path.substr(0, q);

    // Content-Length（头部一律大小写不敏感地找）
    size_t clen = 0;
    {
        std::string head = c.in.substr(0, hdrEnd);
        for (char& ch : head) ch = char(std::tolower(static_cast<unsigned char>(ch)));
        const size_t p = head.find("content-length:");
        if (p != std::string::npos) clen = size_t(std::strtoul(head.c_str() + p + 15, nullptr, 10));
    }
    if (c.in.size() < hdrEnd + 4 + clen) return 0;
    const std::string body = c.in.substr(hdrEnd + 4, clen);
    const bool isSet = (method == "POST" && path == "/set");
    c.inSet = isSet;

    if (method == "GET" && path == "/") {
        respOut = resp(200, "text/html; charset=utf-8", kHtml);
        return 1;
    }
    if (method == "GET" && path == "/state") {
        respOut = resp(200, "application/json", stateJson());
        return 1;
    }
    if (isSet) {
        respOut = resp(200, "application/json", handleSet(body, /*logIt=*/true));
        return 1;
    }
    respOut = resp(404, "text/plain; charset=utf-8", "404\n");
    return 1;
}

void closeConn(Conn& c) {
    if (c.fd >= 0) ::close(c.fd);
    c.fd = -1;
    c.in.clear();
    c.inSet = false;
}

std::string resolveStatePath() {
    const char* env = std::getenv("MIYU_STATE_DIR");
    std::string dir;
    if (env && *env) {
        dir = env;
    } else {
        char buf[4096];
        const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof buf - 1);
        if (n > 0) {
            buf[n] = '\0';
            std::string exe(buf);
            const size_t slash = exe.rfind('/');
            if (slash != std::string::npos) dir = exe.substr(0, slash) + "/../state";
        }
        if (dir.empty()) dir = "state";
    }
    ::mkdir(dir.c_str(), 0755);      // 已经有就 EEXIST，不管
    return dir + "/settings.json";
}

}  // namespace

// ── 对外 ────────────────────────────────────────────────────────────
Settings&       settings()  { return gSet; }
const Settings& settingsC() { return gSet; }

// ── 十九：smaa 可用性 + 回落 ────────────────────────────────────────
void setSmaaOk(bool ok) {
    if (gSmaaOk == ok) return;
    gSmaaOk = ok;
    if (!ok) logw("smaa 不可用：抗锯齿下拉里不留这一档，请求 smaa 的一律按 ssaa2x 走");
}

void aaFallback() {
    if (gSet.aa != "smaa") return;
    gSet.aa = "ssaa2x";
    saveSettings();
    logw("smaa 不可用 → aa 回落到 ssaa2x（已写回 %s）", gPath.c_str());
}

void setStatus(const char* output, int w, int h) {
    if (output) gOutput = output;
    gWinW = w;
    gWinH = h;
}

void setFps(double fps) { gFps = fps; }

void setExprNames(std::vector<std::string> names) { gExprs = std::move(names); }

const std::vector<std::string>& exprNames() { return gExprs; }

void setLastSay(const std::string& t) { gLastSay = t; }

void setPlanNames(std::vector<std::string> names) { gPlans = std::move(names); }

const char* statePath() { return gPath.c_str(); }
int port() { return gPort; }

void init(ApplyFn apply, CmdFn cmd) {
    gApply = apply;
    gCmd   = cmd;
    gPath  = resolveStatePath();
    loadSettings();
    loadPartReact();          // 二十五：部位→表情（从 parts.tsv 的 expr 段取初值）

    // 端口：MIYU_WEB_PORT 覆盖，默认 8200
    int want = 8200;
    if (const char* p = std::getenv("MIYU_WEB_PORT")) {
        const int n = std::atoi(p);
        if (n > 0 && n < 65536) want = n;
        else logw("MIYU_WEB_PORT=%s 不合法，用 %d", p, want);
    }

    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { logw("socket 建不起来（%s），设置页关掉", std::strerror(errno)); return; }
    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);

    struct sockaddr_in sa;
    std::memset(&sa, 0, sizeof sa);
    sa.sin_family      = AF_INET;
    sa.sin_port        = htons(uint16_t(want));
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);   // 只认 127.0.0.1

    if (::bind(fd, (struct sockaddr*)&sa, sizeof sa) != 0) {
        logw("端口 %d 被占用（%s）—— 设置页不启动，身体照常跑", want, std::strerror(errno));
        ::close(fd);
        return;
    }
    if (::listen(fd, 8) != 0) {
        logw("listen 失败（%s），设置页关掉", std::strerror(errno));
        ::close(fd);
        return;
    }
    const int fl = ::fcntl(fd, F_GETFL, 0);
    ::fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    gFd   = fd;
    gPort = want;
    for (auto& c : gConn) c.fd = -1;
    logw("设置页 http://127.0.0.1:%d （%zu 个表情可切，落盘 %s）",
         gPort, gExprs.size(), gPath.c_str());
}

void poll(double now) {
    if (gFd < 0) return;

    // 每帧最多接两个连接：再多就留给下几帧，别让 accept 循环吃掉这一帧的时间。
    for (int i = 0; i < 2; i++) {
        const int cfd = ::accept(gFd, nullptr, nullptr);
        if (cfd < 0) break;
        int slot = -1;
        for (int j = 0; j < kMaxConn; j++) if (gConn[j].fd < 0) { slot = j; break; }
        if (slot < 0) { ::close(cfd); continue; }
        const int fl = ::fcntl(cfd, F_GETFL, 0);
        ::fcntl(cfd, F_SETFL, fl | O_NONBLOCK);
        gConn[slot].fd = cfd;
        gConn[slot].in.clear();
        gConn[slot].inSet = false;
        gConn[slot].t0 = now;
    }

    for (auto& c : gConn) {
        if (c.fd < 0) continue;

        // 读：读到 EAGAIN 为止，一次最多 64KB（设置页的请求都不到 1KB）
        if (c.in.size() < 65536) {
            char buf[8192];
            for (int round = 0; round < 4; round++) {
                const ssize_t n = ::recv(c.fd, buf, sizeof buf, 0);
                if (n > 0) { c.in.append(buf, size_t(n)); continue; }
                if (n == 0) break;                        // 对端关了，下面按「读完」处理
                if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                closeConn(c);
                break;
            }
        }
        if (c.fd < 0) continue;

        std::string out;
        const int st = tryHandle(c, out);
        if (st == 1) {
            sendAll(c.fd, out);
            closeConn(c);
            continue;
        }
        // 3 秒还没收全（半截请求 / 客户端不写了）：掐掉，别攒着
        if (now - c.t0 > 3.0) {
            if (!c.in.empty())
                logw("请求 3 秒没收全，丢掉（%zu 字节）", c.in.size());
            closeConn(c);
        }
    }
}

void shutdown() {
    for (auto& c : gConn) closeConn(c);
    if (gFd >= 0) { ::close(gFd); gFd = -1; }
}

}  // namespace web
