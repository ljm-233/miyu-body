from Xlib import display, X
d = display.Display(':0'); root = d.screen().root
found = []
def walk(w):
    for c in w.query_tree().children:
        try:
            p = c.get_full_property(d.get_atom('_NET_WM_NAME'), X.AnyPropertyType)
            nm = p.value.decode() if p else None
        except Exception:
            nm = None
        if nm == 'alpha-probe':
            found.append(c)
            return
        try: walk(c)
        except Exception: pass
walk(root)
w = found[0]
g = w.get_geometry()
print('xid', hex(w.id), 'size', g.width, g.height)
raw = w.get_image(0, 0, g.width, g.height, X.ZPixmap, 0xffffffff)
data = raw.data
stride = len(data) // g.height
print('stride', stride, 'bpp', stride / g.width)
def px(x, y):
    i = y*stride + x*4
    b, gg, r, a = data[i], data[i+1], data[i+2], data[i+3]
    return (r, gg, b, a)
pts = {'green_left': (g.width//4, g.height//2),
       'red_right': (g.width*3//4, g.height//2),
       'red_right_low': (g.width-30, g.height-30),
       'red_right_top': (g.width-30, 30)}
for k,(x,y) in pts.items():
    print(k, '(',x,y,') RGBA=', px(x,y))
