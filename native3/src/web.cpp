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
    --bg:#0d1117; --fg:#c9d1d9; --dim:#6e7681; --line:#1f2430;
    --accent:#7aa2f7; --accent2:#bb9af7; --ok:#9ece6a;
  }
  *{box-sizing:border-box}
  html,body{margin:0;background:var(--bg);color:var(--fg);
    font:15px/1.6 -apple-system,BlinkMacSystemFont,"Noto Sans SC","Segoe UI",sans-serif}
  .wrap{max-width:420px;margin:0 auto;padding:22px 16px 40px}
  h1{font-size:16px;font-weight:600;margin:0 0 4px;letter-spacing:.06em;
     color:var(--accent)}
  .status{font-size:12px;color:var(--dim);margin:0 0 18px;word-break:break-all}
  .status b{color:var(--accent2);font-weight:500}
  .card{background:#10151d;border:1px solid var(--line);border-radius:12px;
        padding:6px 14px;margin-bottom:14px}
  .gh{font-size:11px;letter-spacing:.18em;color:var(--accent2);
      padding:13px 0 3px;text-transform:uppercase}
  .row{padding:13px 0;border-bottom:1px solid var(--line)}
  .row:last-child{border-bottom:0}
  .top{display:flex;justify-content:space-between;align-items:baseline;gap:10px}
  .k{font-size:13px;color:var(--fg)}
  .v{font-size:13px;color:var(--accent);font-variant-numeric:tabular-nums}
  .d{font-size:11px;color:var(--dim);margin-top:3px}
  input[type=range]{-webkit-appearance:none;appearance:none;width:100%;height:22px;
    background:transparent;margin:6px 0 0}
  input[type=range]::-webkit-slider-runnable-track{height:4px;border-radius:2px;
    background:#1f2430}
  input[type=range]::-webkit-slider-thumb{-webkit-appearance:none;appearance:none;
    width:16px;height:16px;margin-top:-6px;border-radius:50%;background:var(--accent);
    border:0;box-shadow:0 0 0 3px rgba(122,162,247,.18)}
  input[type=range]::-moz-range-track{height:4px;border-radius:2px;background:#1f2430}
  input[type=range]::-moz-range-thumb{width:16px;height:16px;border:0;border-radius:50%;
    background:var(--accent)}
  select{background:#161b26;color:var(--fg);border:1px solid var(--line);
    border-radius:8px;padding:5px 8px;font-size:13px;outline:none}
  .sw{position:relative;display:inline-block;width:40px;height:22px;flex:0 0 auto}
  .sw input{opacity:0;width:0;height:0}
  .sl{position:absolute;inset:0;background:#1f2430;border-radius:22px;
      transition:background .15s;cursor:pointer}
  .sl:before{content:"";position:absolute;width:16px;height:16px;left:3px;top:3px;
      background:#8b949e;border-radius:50%;transition:transform .15s,background .15s}
  .sw input:checked + .sl{background:rgba(122,162,247,.35)}
  .sw input:checked + .sl:before{transform:translateX(18px);background:var(--accent)}
  .btns{display:grid;grid-template-columns:1fr 1fr;gap:9px;margin-top:16px}
  button{font:inherit;font-size:13px;color:var(--fg);background:#161b26;
    border:1px solid var(--line);border-radius:10px;padding:11px 8px;cursor:pointer;
    transition:border-color .15s,color .15s,background .15s}
  button:hover{border-color:var(--accent);color:var(--accent)}
  button:active{background:#1b2230}
  button.wide{grid-column:1 / -1}
  button.warn:hover{border-color:#f7768e;color:#f7768e}
  .hint{margin-top:14px;font-size:11px;color:var(--dim);min-height:16px;
        text-align:center;transition:color .2s}
  .hint.ok{color:var(--ok)}
</style></head>
<body><div class="wrap">
  <h1>米鱼 · 设置</h1>
  <p class="status" id="status">连接中…</p>
  <div id="groups"></div>

  <div class="btns">
    <button id="call">叫她一下</button>
    <button id="exprnext">切个表情</button>
    <button id="reset">恢复默认</button>
    <button id="restart" class="warn">重启服务</button>
  </div>
  <div class="hint" id="hint"></div>
</div>
<script>
(function(){
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

  // 每项：k=键名 t=控件类型 label=名字 d=这一项到底改了什么
  var SCHEMA = [
    {g:'外观', items:[
      {k:'scale', t:'range', min:0.4, max:1.6, step:0.05, label:'大小',
       d:'整块窗口放大缩小（重建 buffer），位置不动'},
      {k:'alpha', t:'range', min:0.3, max:1.0, step:0.05, label:'不透明度',
       d:'乘到最终颜色上，越低桌面从她身上透出来越多'},
      {k:'idleStyle', t:'select', label:'待机风格',
       opts:[['quiet','安静（几乎不动）'],['natural','自然'],['lively','活泼（明显在晃）']],
       d:'quiet 几乎不动、只剩呼吸；lively 会明显晃，换姿势也更快'},
      {k:'idle', t:'range', min:0, max:3, step:0.1, label:'待机幅度',
       d:'在风格之上再乘一个倍数，0 = 完全定住只剩呼吸'},
      // ── 第十七节：抗锯齿 + 后处理三件 ──────────────────────────
      {k:'ssaa', t:'select', label:'抗锯齿',
       opts:[['off','关（最省，边缘有锯齿）'],['2x','2 倍（默认）'],['3x','3 倍（最细）']],
       d:'离屏按 N 倍画再缩回来，头发丝和裙边不再糊成锯齿；代价是显存 ×N²'},
      {k:'shadow', t:'range', min:0, max:1, step:0.05, label:'地面阴影',
       d:'脚下的软阴影，0 关掉她又像贴纸了'},
      {k:'rim', t:'range', min:0, max:1, step:0.05, label:'轮廓光',
       d:'边缘那一圈淡光，深色壁纸上尤其有用'},
      {k:'particles', t:'range', min:0, max:1, step:0.05, label:'气泡',
       d:'身后飘的气泡，0 关掉'}
    ]},
    {g:'行为', items:[
      {k:'look', t:'range', min:0, max:2, step:0.1, label:'视线跟随强度',
       d:'她看鼠标的幅度（眼珠 + 身体微倾），0 = 不看'},
      {k:'lookOn', t:'bool', label:'视线总开关',
       d:'关掉她完全不跟鼠标，眼睛回正'},
      {k:'blink', t:'range', min:0, max:2, step:0.1, label:'眨眼频率',
       d:'2 = 眨得很勤，1 = 默认，0 = 一直睁着不眨'},
      {k:'mouth', t:'range', min:0.5, max:3, step:0.1, label:'嘴动时长',
       d:'说完一句话嘴还在动的时长倍数（张合快慢不变）'},
      {k:'exprHold', t:'range', min:0.2, max:5, step:0.1, label:'表情停留',
       d:'表情挂多久后自己淡回默认（动作计划自带的表情不受它管）'},
      {k:'tap', t:'bool', label:'点击反应', d:'关掉后戳她没反应（拖动还照旧）'},
      {k:'expr', t:'bool', label:'表情切换', d:'关掉所有表情切换（含动作计划里的）'},
      {k:'corner', t:'select', label:'贴角',
       opts:[['free','自由（不动）'],['tl','左上'],['tr','右上'],['bl','左下'],['br','右下']],
       d:'吸到副屏四角之一'},
      {k:'hide', t:'bool', label:'隐藏', d:'整个隐藏，进程还在，点击直接穿过去'}
    ]},
    {g:'性能', items:[
      {k:'fps', t:'select', label:'帧率上限',
       opts:[['30','30（最省电）'],['60','60（默认）'],['120','120'],['200','200（副屏上限）'],['240','240（主屏上限）']],
       d:'每帧睡到该来的时刻，数字越低越省 CPU；副屏是 200Hz，240 档只有挪到主屏才跑得满'}
    ]},
    {g:'声音', items:[
      {k:'voice', t:'bool', label:'说话出声',
       d:'说话时播 voice/*.wav 里预生成的短句，没有这句就静默'},
      {k:'vol', t:'range', min:0, max:1, step:0.05, label:'音量',
       d:'pw-play 的 --volume，0 = 有声音但不响'}
    ]}
  ];

  var els = {};
  function addRow(card, it){
    var row = document.createElement('div'); row.className = 'row';
    var top = document.createElement('div'); top.className = 'top';
    var k = document.createElement('span'); k.className = 'k'; k.textContent = it.label;
    var v = null;
    if (it.t === 'range') { v = document.createElement('span'); v.className = 'v'; v.textContent = '—'; }
    top.appendChild(k);
    if (v) top.appendChild(v); else {
      var box = document.createElement('span'); box.style.flex = '0 0 auto'; top.appendChild(box);
    }
    row.appendChild(top);

    if (it.t === 'range') {
      var r = document.createElement('input');
      r.type = 'range'; r.min = it.min; r.max = it.max; r.step = it.step;
      r.addEventListener('input', function(){
        v.textContent = Number(r.value).toFixed(2);
        push(it.k, parseFloat(r.value), 300);     // 300ms 防抖
      });
      row.appendChild(r);
      els[it.k] = {el: r, out: v, type: 'range'};
    } else if (it.t === 'bool') {
      var lab = document.createElement('label'); lab.className = 'sw';
      var cb = document.createElement('input'); cb.type = 'checkbox';
      var sl = document.createElement('span'); sl.className = 'sl';
      lab.appendChild(cb); lab.appendChild(sl);
      top.removeChild(top.lastChild); top.appendChild(lab);
      cb.addEventListener('change', function(){
        var o = {}; o[it.k] = cb.checked;
        post(o).then(function(d){
          if (d && d.ok) hint('已应用：' + it.k, true); else hint('没生效', false);
        }).catch(function(){});
      });
      els[it.k] = {el: cb, type: 'bool'};
    } else {
      var sel = document.createElement('select');
      it.opts.forEach(function(o2){
        var op = document.createElement('option'); op.value = o2[0]; op.textContent = o2[1];
        sel.appendChild(op);
      });
      top.removeChild(top.lastChild); top.appendChild(sel);
      sel.addEventListener('change', function(){
        var o = {}; o[it.k] = (it.k === 'fps') ? parseInt(sel.value, 10) : sel.value;
        post(o).then(function(d){
          if (d && d.ok) hint('已应用：' + it.k, true); else hint('没生效', false);
        }).catch(function(){});
      });
      els[it.k] = {el: sel, type: 'select'};
    }

    var d = document.createElement('div'); d.className = 'd'; d.textContent = it.d;
    row.appendChild(d);
    card.appendChild(row);
  }

  var root = document.getElementById('groups');
  SCHEMA.forEach(function(sec){
    var card = document.createElement('div'); card.className = 'card';
    var h = document.createElement('div'); h.className = 'gh'; h.textContent = sec.g;
    card.appendChild(h);
    sec.items.forEach(function(it){ addRow(card, it); });
    root.appendChild(card);
  });

  function fill(s){
    document.getElementById('status').innerHTML =
      '<b>运行中</b> · PID ' + s.pid + ' · 输出 ' + s.output + ' · 窗口 ' + s.window;
    SCHEMA.forEach(function(sec){
      sec.items.forEach(function(it){
        var e = els[it.k]; if (!e) return;
        var val = s[it.k];
        if (e.type === 'range') {
          e.el.value = val;
          e.out.textContent = Number(val).toFixed(2);
        } else if (e.type === 'bool') {
          e.el.checked = !!val;
        } else {
          e.el.value = String(val);
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
  // 5 秒只刷状态行：不动控件值（不然正拖滑块会被服务端值拽回去）
  setInterval(function(){
    fetch('/state').then(function(r){ return r.json(); }).then(function(s){
      document.getElementById('status').innerHTML =
        '<b>运行中</b> · PID ' + s.pid + ' · 输出 ' + s.output + ' · 窗口 ' + s.window;
    }).catch(function(){});
  }, 5000);

  document.getElementById('call').addEventListener('click', function(){
    post({say:'在', mood:60}).then(function(){ hint('叫了：在', true); }).catch(function(){
      hint('连不上', false); });
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

// ── 状态 ────────────────────────────────────────────────────────────
int         gFd       = -1;
int         gPort     = 0;
Settings    gSet;
ApplyFn     gApply    = nullptr;
std::string gOutput   = "?";
int         gWinW     = 0, gWinH = 0;
std::string gPath     = "settings.json";
std::vector<std::string> gExprs;
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
          << ",\"ssaa\":\"" << gSet.ssaa << "\""
          << ",\"shadow\":" << gSet.shadow
          << ",\"rim\":" << gSet.rim
          << ",\"particles\":" << gSet.particles << "}\n";
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
            if (c == "tl" || c == "tr" || c == "bl" || c == "br" || c == "free") gSet.corner = c;
        }
        // ── 第十七节：抗锯齿 + 后处理三件 ──────────────────────────
        else if (k == "shadow" && v.t == VNum) gSet.shadow = clampf(float(v.n), 0.0f, 1.0f);
        else if (k == "rim" && v.t == VNum) gSet.rim = clampf(float(v.n), 0.0f, 1.0f);
        else if (k == "particles" && v.t == VNum) gSet.particles = clampf(float(v.n), 0.0f, 1.0f);
        else if (k == "ssaa" && v.t == VStr) {
            const std::string c = v.s;
            if (c == "off" || c == "2x" || c == "3x") gSet.ssaa = c;
        }
    }
    logw("读回 %s：scale=%.2f alpha=%.2f tap=%d expr=%d idle=%.2f(%s) corner=%s hide=%d "
         "look=%.2f/%d blink=%.2f mouth=%.2f exprHold=%.2f fps=%d voice=%d vol=%.2f "
         "ssaa=%s shadow=%.2f rim=%.2f particles=%.2f",
         gPath.c_str(), gSet.scale, gSet.alpha, int(gSet.tap), int(gSet.expr),
         gSet.idle, gSet.idleStyle.c_str(), gSet.corner.c_str(), int(gSet.hide),
         gSet.look, int(gSet.lookOn), gSet.blink, gSet.mouth, gSet.exprHold,
         gSet.fps, int(gSet.voice), gSet.vol,
         gSet.ssaa.c_str(), gSet.shadow, gSet.rim, gSet.particles);
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
    s += ",\"hide\":" + std::string(gSet.hide ? "true" : "false");
    s += ",\"ssaa\":\"" + gSet.ssaa + "\"";
    s += ",\"shadow\":" + std::to_string(gSet.shadow);
    s += ",\"rim\":" + std::to_string(gSet.rim);
    s += ",\"particles\":" + std::to_string(gSet.particles);
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
        if (k == "hide" && v.t == VBool) {
            if (v.b != gSet.hide) { gSet.hide = v.b; changed.push_back("hide"); }
            continue;
        }
        if (k == "corner" && v.t == VStr) {
            const std::string c = v.s;
            if (c == "tl" || c == "tr" || c == "bl" || c == "br" || c == "free") {
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
        if (k == "ssaa" && v.t == VStr) {
            const std::string c = v.s;
            if (c != "off" && c != "2x" && c != "3x") {
                if (logIt) logw("POST /set ssaa=%s 只认 off/2x/3x，忽略", c.c_str());
            } else if (c != gSet.ssaa) {
                gSet.ssaa = c; changed.push_back("ssaa");
            }
            continue;
        }
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
            "voice", "vol", "ssaa", "shadow", "rim", "particles",
        };
        changed.clear();
        for (const char* k : kAll) changed.push_back(k);
        logw("reset：全部设置恢复默认（%zu 项）", changed.size());
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

void setStatus(const char* output, int w, int h) {
    if (output) gOutput = output;
    gWinW = w;
    gWinH = h;
}

void setExprNames(std::vector<std::string> names) { gExprs = std::move(names); }

const char* statePath() { return gPath.c_str(); }
int port() { return gPort; }

void init(ApplyFn apply) {
    gApply = apply;
    gPath  = resolveStatePath();
    loadSettings();

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
