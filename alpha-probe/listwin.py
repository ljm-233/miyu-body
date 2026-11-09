from Xlib import display, X, error
d = display.Display(':0')
root = d.screen().root
atom_name = d.get_atom('_NET_WM_NAME'); atom_utf8 = d.get_atom('UTF8_STRING')
def info(c):
    out = {'id': hex(c.id)}
    try:
        a = c.get_attributes(); out['depth'] = a.depth; out['map'] = a.map_state
    except Exception as e:
        out['depth'] = 'ERR:%s' % type(e).__name__
    try:
        g = c.get_geometry(); out['size'] = (g.width, g.height)
    except Exception as e:
        out['size'] = 'ERR'
    for prop in ('_NET_WM_NAME','WM_NAME','_NET_WM_WINDOW_OPACITY'):
        try:
            p = c.get_full_property(d.get_atom(prop), X.AnyPropertyType)
            if p is not None:
                v = p.value
                out[prop] = v.decode('utf-8','replace') if isinstance(v,bytes) else list(v)
        except Exception:
            pass
    return out
def walk(win, depth=0):
    for c in win.query_tree().children:
        print(' '*depth, info(c))
        walk(c, depth+2)
walk(root)
