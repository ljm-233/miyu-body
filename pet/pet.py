#!/usr/bin/env python3
"""
米鱼的桌面浮层（Wayland / niri）。

思路：用一个 layer-shell 全屏透明窗口，里面塞一个 WebKit 视图跑 live2d 页面。
背景全透明，所以屏幕上只看得见那条鱼；点击只在鱼身上生效（其余穿透到桌面要另外配，
niri 的 layer-shell 暂时不支持 input region 穿透，所以先做「整层接收，但只有鱼身上可拖」）。
"""
import fcntl
import os
import sys
import gi

gi.require_version('Gtk', '3.0')
gi.require_version('WebKit2', '4.1')
gi.require_version('GtkLayerShell', '0.1')
from gi.repository import Gtk, Gdk, GLib, WebKit2, GtkLayerShell  # noqa: E402
import cairo  # noqa: E402  （空的输入区域 = 整窗穿透）

URL = sys.argv[1] if len(sys.argv) > 1 else 'http://127.0.0.1:8100/body10.html?pet=1&v=' + str(int(__import__('time').time()))


LOCK_PATH = os.path.expanduser('~/.dsh/miyu/body/pet/.pet.lock')
CONF_PATH = os.path.expanduser('~/.dsh/miyu/body/pet/pet.conf')


def read_conf():
    """窗口怎么放由这个文件说了算。改完 `kill -USR1 <pid>` 立刻生效，不用重启。
    height: 占屏幕高度的比例（0.1~1.0）
    margin: 离屏幕边缘多少像素
    corner: 贴哪个角，br/bl/tr/tl
    """
    c = {'height': 0.9, 'margin': 24, 'corner': 'br'}
    try:
        for line in open(CONF_PATH, encoding='utf-8'):
            line = line.split('#', 1)[0].strip()
            if '=' not in line:
                continue
            k, v = (x.strip() for x in line.split('=', 1))
            if k == 'height':
                c['height'] = max(0.1, min(1.0, float(v)))
            elif k == 'margin':
                c['margin'] = max(0, int(v))
            elif k == 'corner' and v in ('br', 'bl', 'tr', 'tl'):
                c['corner'] = v
    except OSError:
        pass
    return c


def take_lock():
    """单实例锁。2026-11-09：反复起进程没杀干净，四个桌宠同时加载模型把内存吃了几个 G。
    锁住之后第二个启动会直接退出，不会再叠。"""
    os.makedirs(os.path.dirname(LOCK_PATH), exist_ok=True)
    fh = open(LOCK_PATH, 'w')
    try:
        fcntl.flock(fh, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError:
        print('[pet] 已经有一个在跑了，这次不启动（要换的话先 pkill -f body/pet/pet.py）', flush=True)
        sys.exit(0)
    fh.write(str(os.getpid()))
    fh.flush()
    return fh


# ── 状态桥：让 DSH 那边能告诉她「我刚才说了什么」 ────────────────────────
# 2026-11-09：加一个极小的 HTTP 端点。
#   GET /state  → {"say": "...", "at": 时间戳, "mood": 数字}
#   POST /say   → 写入一段话，页面下次轮询就做口型/动作
import json
import threading
from http.server import BaseHTTPRequestHandler, HTTPServer

STATE = {"say": "", "at": 0, "mood": 0}
STATE_LOCK = threading.Lock()


class Bridge(BaseHTTPRequestHandler):
    def log_message(self, *a):   # 别刷屏
        pass

    def end_headers(self):
        # 2026-11-09：页面在 8100、桥在 8101，跨源。没有这几个头 fetch 直接被拦，
        # 而页面那边是 catch{} 吞掉的，表现就是「怎么都不动」。
        self.send_header('Access-Control-Allow-Origin', '*')
        self.send_header('Access-Control-Allow-Headers', '*')
        self.send_header('Access-Control-Allow-Methods', 'GET, POST, OPTIONS')
        super().end_headers()

    def do_OPTIONS(self):
        self.send_response(204)
        self.end_headers()

    def do_GET(self):
        if self.path.startswith('/state'):
            with STATE_LOCK:
                body = json.dumps(STATE, ensure_ascii=False).encode()
            self.send_response(200)
            self.send_header('Content-Type', 'application/json; charset=utf-8')
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        else:
            self.send_response(404)
            self.end_headers()

    def do_POST(self):
        if self.path.startswith('/log'):
            n = int(self.headers.get('Content-Length') or 0)
            raw = self.rfile.read(n) if n else b''
            print('[page] ' + raw.decode('utf-8', 'replace')[:400], flush=True)
            self.send_response(204)
            self.end_headers()
            return
        if self.path.startswith('/say'):
            n = int(self.headers.get('Content-Length') or 0)
            raw = self.rfile.read(n) if n else b'{}'
            try:
                d = json.loads(raw or b'{}')
            except Exception:
                d = {}
            import time as _t
            with STATE_LOCK:
                STATE['say'] = str(d.get('text') or '')[:200]
                STATE['mood'] = int(d.get('mood') or 0)
                STATE['at'] = int(_t.time() * 1000)
            self.send_response(204)
            self.end_headers()
        else:
            self.send_response(404)
            self.end_headers()


def start_bridge(port=8101):
    def run():
        try:
            HTTPServer(('127.0.0.1', port), Bridge).serve_forever()
        except OSError as e:
            print(f'[pet] 状态桥起不来（{e}），只影响联动', flush=True)
    threading.Thread(target=run, daemon=True).start()
    print(f'[pet] 状态桥 http://127.0.0.1:{port}/state', flush=True)


def main():
    _lock = take_lock()
    start_bridge()  # noqa: F841  留着引用，进程活着锁就不放
    win = Gtk.Window()

    # 透明：拿到 rgba visual，再把窗口背景设成完全透明
    screen = win.get_screen()
    visual = screen.get_rgba_visual()
    if visual is not None:
        win.set_visual(visual)
    win.set_app_paintable(True)
    win.connect('draw', lambda w, cr: cr.set_source_rgba(0, 0, 0, 0))

    # layer-shell：浮在最上层，铺满整个输出
    GtkLayerShell.init_for_window(win)
    GtkLayerShell.set_layer(win, GtkLayerShell.Layer.OVERLAY)
    GtkLayerShell.set_namespace(win, 'miyu-body')
    # 2026-11-09：原来四面锚定 = 全屏覆盖层，整层吃输入，鼠标点哪都没反应。
    # 改成只锚右下、给定尺寸：窗口就只有那一块，其他地方是真的穿透。
    # 2026-11-09：这机器有两块屏（内屏 1706x1066 / 外接 1920x1080）。
    # 原来只取主屏尺寸、却让 layer-shell 自己挑输出，结果窗口落到内屏上还按大屏算高度，
    # 人就被撑爆、头被切掉。现在显式把窗口绑到主显示器。
    disp = Gdk.Display.get_default()
    # 选面积最大的那块（外接 1920x1080 比内屏 1706x1066 大），她要站在大屏上
    mon = None
    try:
        n = disp.get_n_monitors() if disp else 0
        best = 0
        for i in range(n):
            m = disp.get_monitor(i)
            g = m.get_geometry()
            if g.width * g.height > best:
                best = g.width * g.height
                mon = m
        if mon is not None:
            g = mon.get_geometry()
            print(f'[pet] 选中显示器 #{mon.get_index()} 逻辑 {g.width}x{g.height} 位置 {g.x},{g.y}', flush=True)
    except Exception as e:
        print(f'[pet] 选显示器失败（{e}）', flush=True)
    if mon is None:
        mon = disp.get_primary_monitor() if disp else None
    try:
        if mon is not None:
            GtkLayerShell.set_monitor(win, mon)
            print(f'[pet] 绑到显示器 {mon.get_model() or mon.get_manufacturer()} 逻辑 {mon.get_geometry().width}x{mon.get_geometry().height}', flush=True)
    except Exception as e:
        print(f'[pet] 绑显示器失败（{e}），用默认', flush=True)
    geo = mon.get_geometry() if mon else None
    sw = geo.width if geo else 1920
    sh = geo.height if geo else 1080
    # 2026-11-09：原来 42%×94% 太大，问「为什么要这么大」——对，没必要。
    # 改成按屏高的比例算，宽按模型本身的长宽比（3749:7697 ≈ 0.487）留一点余量。
    conf = read_conf()
    frac = float(os.environ.get('PET_HEIGHT', conf['height']))
    margin = conf['margin']
    corner = conf['corner']
    win_h = int(sh * frac)
    win_w = int(win_h * 0.52)
    win.set_size_request(win_w, win_h)
    print(f'[pet] 窗口 {win_w}x{win_h}（屏 {sw}x{sh}，高度占 {frac:.0%}）', flush=True)
    if corner in ('br', 'bl'):
        GtkLayerShell.set_anchor(win, GtkLayerShell.Edge.BOTTOM, True)
    if corner in ('tr', 'tl'):
        GtkLayerShell.set_anchor(win, GtkLayerShell.Edge.TOP, True)
    if corner in ('br', 'tr'):
        GtkLayerShell.set_anchor(win, GtkLayerShell.Edge.RIGHT, True)
    if corner in ('bl', 'tl'):
        GtkLayerShell.set_anchor(win, GtkLayerShell.Edge.LEFT, True)
    for edge in (GtkLayerShell.Edge.BOTTOM, GtkLayerShell.Edge.TOP,
                 GtkLayerShell.Edge.RIGHT, GtkLayerShell.Edge.LEFT):
        GtkLayerShell.set_margin(win, edge, margin)
    # 键盘不抢焦点，免得打字被它吃掉
    GtkLayerShell.set_keyboard_mode(win, GtkLayerShell.KeyboardMode.NONE)

    web = WebKit2.WebView()
    web.set_background_color(Gdk.RGBA(0, 0, 0, 0))
    settings = web.get_settings()
    settings.set_enable_developer_extras(True)
    settings.set_property('allow-file-access-from-file-urls', True)
    settings.set_property('allow-universal-access-from-file-urls', True)
    web.load_uri(URL)
    # 页面自己带深色渐变底，这里要它透明：注入一段 CSS
    web.run_javascript("""
      document.documentElement.style.background='transparent';
      document.body.style.background='transparent';
      [].forEach.call(document.querySelectorAll('#tip,#err'), e => e.style.display='none');
    """, None, None, None)

    # 2026-11-09：输入穿透。给窗口一个空的 input region，它就不再吃任何点击，
    # 鼠标事件全部落到下面的桌面/浏览器上。设 PET_CLICK=1 可以关掉穿透（方便拖它）。
    # 2026-11-09：三件事要一起成立——穿透、能拖、窗口可调。
    # 做法：整窗把输入区域收成「她身体那一块竖矩形」，其余全部穿透。
    # 这样鼠标点别处照常落到桌面上，点她身上才能拖。
    if os.environ.get('PET_CLICK') == '1':
        print('[pet] 整窗可交互（PET_CLICK=1）', flush=True)
    else:
        W, H = win_w, win_h
        body = cairo.RectangleInt(int(W * 0.22), int(H * 0.03), int(W * 0.56), int(H * 0.94))
        win.input_shape_combine_region(cairo.Region(body))
        print(f'[pet] 只有中间 {int(W*0.56)}x{int(H*0.94)} 接收点击，其余穿透', flush=True)

    # 2026-11-09：改完 pet.conf 不用重启，`kill -USR1 <pid>` 就重新算一遍尺寸和位置。
    def on_usr1():
        try:
            c = read_conf()
            new_h = int(sh * float(c['height']))
            new_w = int(new_h * 0.52)
            win.resize(new_w, new_h)
            for edge in (GtkLayerShell.Edge.BOTTOM, GtkLayerShell.Edge.TOP,
                         GtkLayerShell.Edge.RIGHT, GtkLayerShell.Edge.LEFT):
                GtkLayerShell.set_margin(win, edge, c['margin'])
            if os.environ.get('PET_CLICK') != '1':
                body = cairo.RectangleInt(int(new_w * 0.22), int(new_h * 0.03),
                                          int(new_w * 0.56), int(new_h * 0.94))
                win.input_shape_combine_region(cairo.Region(body))
            print(f'[pet] 按新配置重排：{new_w}x{new_h} margin={c["margin"]} corner={c["corner"]}', flush=True)
        except Exception as e:
            print(f'[pet] 重排失败：{e}', flush=True)
        return True
    import signal as _sig
    GLib.unix_signal_add(GLib.PRIORITY_DEFAULT, _sig.SIGUSR1, on_usr1)

    win.add(web)
    win.connect('destroy', Gtk.main_quit)
    win.show_all()
    Gtk.main()


if __name__ == '__main__':
    main()
