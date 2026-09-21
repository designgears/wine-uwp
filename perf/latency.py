#!/usr/bin/env python3
"""latency.py: measure input -> screen change latency on :98.
modes: keys (type digits, watch the result area), hover (move between two buttons, watch the button),
       switch (open menu and pick Graphing, watch the canvas area)."""
import sys, time, hashlib
from Xlib import display, X, XK
from Xlib.ext import xtest

d = display.Display(':98')
root = d.screen().root

def region_hash(x, y, w, h):
    img = root.get_image(x, y, w, h, X.ZPixmap, 0xffffffff)
    return hashlib.md5(img.data).digest()

def wait_change(x, y, w, h, before, timeout=3.0):
    t0 = time.perf_counter()
    while time.perf_counter() - t0 < timeout:
        if region_hash(x, y, w, h) != before:
            return time.perf_counter() - t0
        time.sleep(0.001)
    return None

def key(ch):
    code = d.keysym_to_keycode(XK.string_to_keysym(ch))
    xtest.fake_input(d, X.KeyPress, code); xtest.fake_input(d, X.KeyRelease, code); d.sync()

def move(x, y):
    xtest.fake_input(d, X.MotionNotify, x=x, y=y); d.sync()

def click(x, y):
    move(x, y); xtest.fake_input(d, X.ButtonPress, 1); xtest.fake_input(d, X.ButtonRelease, 1); d.sync()

def settle(x, y, w, h, quiet=0.3):
    last = region_hash(x, y, w, h); t = time.perf_counter()
    while time.perf_counter() - t < quiet:
        time.sleep(0.01)
        cur = region_hash(x, y, w, h)
        if cur != last: last = cur; t = time.perf_counter()
    return last

mode = sys.argv[1]
res = []
if mode == 'keys':
    area = (500, 100, 460, 100)     # the result display
    move(300, 700); time.sleep(0.2)
    for i in range(20):
        before = settle(*area)
        t0 = time.perf_counter(); key(str((i * 7) % 10))
        lat = wait_change(*area, before)
        if lat is not None: res.append(lat)
    key('Escape')
elif mode == 'hover':
    a, b = (120, 540), (360, 540)
    area = (40, 500, 400, 80)
    for i in range(20):
        before = settle(*area)
        move(*(a if i % 2 else b))
        lat = wait_change(*area, before)
        if lat is not None: res.append(lat)
elif mode == 'switch':
    area = (20, 100, 600, 400)
    before = settle(*area, quiet=0.5)
    click(26, 84); time.sleep(1.0)
    before = settle(*area, quiet=0.3)
    click(76, 246)
    lat = wait_change(*area, before, timeout=10)
    t_first = lat
    # until settled
    t0 = time.perf_counter(); settle(*area, quiet=0.5); res = [t_first, (t_first or 0) + time.perf_counter() - t0 - 0.5]
    print("switch: first change %.0f ms, settled %.0f ms" % (res[0] * 1000, res[1] * 1000)); sys.exit()
res.sort()
if res:
    print("%s: n=%d median %.1f ms, p90 %.1f ms, max %.1f ms" % (mode, len(res), res[len(res)//2]*1000, res[int(len(res)*0.9)]*1000, res[-1]*1000))
else:
    print(mode, "no changes seen")
