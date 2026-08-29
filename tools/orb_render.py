#!/usr/bin/env python3
"""
orb_render.py -- offline, faithful mockup renderer for ORB-TAMA screenshots.

Parses the SAME FreeSansBold bitmap fonts bundled in orb/Fonts/ and re-plays
the drawing calls from orb.ino (identical colors, coordinates, geometry) onto
a 240x240 RGB565 framebuffer, then writes PNGs -- both a raw square frame and
a "device view" (round GC9A01 screen on a dark bezel).

Usage:  python3 tools/orb_render.py [outdir]
"""

import math
import os
import re
import sys

from PIL import Image

W = H = 240

# ---------------------------------------------------------------- RGB565 ---
def rgb565(r, g, b):
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def rgb_of(v):
    r = (v >> 11) & 0x1F
    g = (v >> 5) & 0x3F
    b = v & 0x1F
    return (r << 3, g << 2, b << 3)


# palette -- copied verbatim from orb.ino
COL_BG = rgb565(5, 6, 10)
COL_BODY = rgb565(15, 17, 26)
COL_RIM = rgb565(36, 42, 58)
COL_SCLERA = rgb565(238, 242, 250)
COL_PUPIL = rgb565(8, 8, 12)
COL_TRACK = rgb565(28, 31, 42)
COL_ICON = rgb565(210, 216, 230)
COL_ICONDIM = rgb565(80, 86, 102)
COL_HEART = rgb565(255, 92, 128)
COL_POOP = rgb565(122, 82, 48)
COL_GOOD = rgb565(80, 220, 130)
COL_BAD = rgb565(255, 82, 82)

# -------------------------------------------------------------- utilities --
def hsv565(h, s, v):
    h = h % 360.0
    if h < 0:
        h += 360
    c = v * s
    x = c * (1 - abs((h / 60.0) % 2 - 1))
    m = v - c
    if h < 60:
        r, g, b = c, x, 0
    elif h < 120:
        r, g, b = x, c, 0
    elif h < 180:
        r, g, b = 0, c, x
    elif h < 240:
        r, g, b = 0, x, c
    elif h < 300:
        r, g, b = x, 0, c
    else:
        r, g, b = c, 0, x
    return rgb565(int((r + m) * 255), int((g + m) * 255), int((b + m) * 255))


def lerp_color(a, b, t):
    ar, ag, ab = (a >> 11) << 3, ((a >> 5) & 0x3F) << 2, (a & 0x1F) << 3
    br, bg, bl = (b >> 11) << 3, ((b >> 5) & 0x3F) << 2, (b & 0x1F) << 3
    r = ar + ((br - ar) * t >> 8)
    g = ag + ((bg - ag) * t >> 8)
    b = ab + ((bl - ab) * t >> 8)
    return rgb565(r, g, b)


def isqrt(n):
    return int(math.isqrt(n))


# fast sin/cos LUT (identical to firmware LUT)
SIN_LUT = [int(math.sin(i * 2 * math.pi / 64) * 1024) for i in range(64)]


def fast_sin10(ang_deg):
    a = ((ang_deg % 360) + 360) % 360
    return SIN_LUT[(a * 64) // 360]


def fast_cos10(ang_deg):
    return fast_sin10(ang_deg + 90)


# ----------------------------------------------------------------- canvas --
class Canvas:
    """240x240 RGB565 framebuffer with the GFX primitives the firmware uses."""

    def __init__(self):
        self.fb = [COL_BG] * (W * H)

    def pixel(self, x, y, c):
        if 0 <= x < W and 0 <= y < H:
            self.fb[y * W + x] = c

    def fill_rect(self, x, y, w, h, c):
        x0 = max(0, x); y0 = max(0, y)
        x1 = min(W, x + w); y1 = min(H, y + h)
        for yy in range(y0, y1):
            start = yy * W + x0
            self.fb[start:start + (x1 - x0)] = [c] * (x1 - x0)

    def fill_circle(self, cx, cy, r, c):
        r = int(r)
        for dy in range(-r, r + 1):
            hw = isqrt(r * r - dy * dy)
            self.fill_rect(cx - hw, cy + dy, 2 * hw + 1, 1, c)

    def draw_circle(self, cx, cy, r, c):
        r = int(r)
        x, y, err = r, 0, 1 - r
        while x >= y:
            for px, py in ((x, y), (y, x), (-y, x), (-x, y),
                           (-x, -y), (-y, -x), (y, -x), (x, -y)):
                self.pixel(cx + px, cy + py, c)
            y += 1
            if err < 0:
                err += 2 * y + 1
            else:
                x -= 1
                err += 2 * (y - x) + 1

    def fill_ellipse(self, cx, cy, rx, ry, c):
        for dy in range(-ry, ry + 1):
            t = 1 - (dy * dy) / (ry * ry) if ry else 0
            hw = int(rx * math.sqrt(max(t, 0)))
            self.fill_rect(cx - hw, cy + dy, 2 * hw + 1, 1, c)

    def fill_round_rect(self, x, y, w, h, r, c):
        r = min(r, w // 2, h // 2)
        self.fill_rect(x + r, y, w - 2 * r, h, c)
        self.fill_rect(x, y + r, w, h - 2 * r, c)
        for (cx, cy) in ((x + r, y + r), (x + w - 1 - r, y + r),
                         (x + r, y + h - 1 - r), (x + w - 1 - r, y + h - 1 - r)):
            self.fill_circle(cx, cy, r, c)

    def draw_round_rect(self, x, y, w, h, r, c):
        self.draw_line(x + r, y, x + w - 1 - r, y, c)
        self.draw_line(x + r, y + h - 1, x + w - 1 - r, y + h - 1, c)
        self.draw_line(x, y + r, x, y + h - 1 - r, c)
        self.draw_line(x + w - 1, y + r, x + w - 1, y + h - 1 - r, c)
        for (cx, cy) in ((x + r, y + r), (x + w - 1 - r, y + r),
                         (x + r, y + h - 1 - r), (x + w - 1 - r, y + h - 1 - r)):
            self.draw_circle(cx, cy, r, c)
    def draw_line(self, x0, y0, x1, y1, c):
        dx = abs(x1 - x0); dy = -abs(y1 - y0)
        sx = 1 if x0 < x1 else -1
        sy = 1 if y0 < y1 else -1
        err = dx + dy
        while True:
            self.pixel(x0, y0, c)
            if x0 == x1 and y0 == y1:
                break
            e2 = 2 * err
            if e2 >= dy:
                err += dy; x0 += sx
            if e2 <= dx:
                err += dx; y0 += sy

    def fill_triangle(self, x0, y0, x1, y1, x2, y2, c):
        pts = [(x0, y0), (x1, y1), (x2, y2)]
        pts.sort(key=lambda p: p[1])
        (ax, ay), (bx, by), (cx, cy) = pts
        if ay == cy:
            return
        for y in range(ay, cy + 1):
            xs, xe = None, None
            for (p1, p2) in (((ax, ay), (bx, by)),
                            ((ax, ay), (cx, cy)),
                            ((bx, by), (cx, cy))):
                if p1[1] == p2[1]:
                    continue
                if min(p1[1], p2[1]) <= y <= max(p1[1], p2[1]):
                    t = (y - p1[1]) / (p2[1] - p1[1])
                    x = p1[0] + (p2[0] - p1[0]) * t
                    xs = x if xs is None else min(xs, x)
                    xe = x if xe is None else max(xe, x)
            if xs is not None:
                self.fill_rect(int(round(xs)), y, int(round(xe)) - int(round(xs)) + 1, 1, c)

    def draw_arc(self, x, y, r1, r2, start, end, c):
        for a in range(start, end + 1, 2):
            rad = math.radians(a)
            for r in range(int(r1), int(r2) + 1):
                self.pixel(int(x + r * math.cos(rad)), int(y + r * math.sin(rad)), c)

    def draw_rect_border(self, c):
        self.draw_line(1, 1, 238, 1, c)
        self.draw_line(1, 238, 238, 238, c)
        self.draw_line(1, 1, 1, 238, c)
        self.draw_line(238, 1, 238, 238, c)

# ------------------------------------------------------------- fonts -------
class Font:
    def __init__(self, path):
        with open(path) as f:
            text = f.read()
        m = re.search(r'Bitmaps\[\] PROGMEM = \{(.*?)\};', text, re.S)
        nums = re.findall(r'0x[0-9A-Fa-f]+|\d+', m.group(1))
        self.bitmaps = [int(n, 16) if n.startswith('0x') else int(n) for n in nums]
        m = re.search(r'Glyphs\[\] PROGMEM = \{(.*?)\};', text, re.S)
        glyphs = []
        pat = re.compile(r'\{\s*(-?\d+)\s*,\s*(-?\d+)\s*,\s*(-?\d+)\s*,\s*(-?\d+)\s*,\s*(-?\d+)\s*,\s*(-?\d+)\s*\}')
        for g in pat.finditer(m.group(1)):
            glyphs.append(tuple(int(x) for x in g.groups()))
        m = re.search(r'0x([0-9A-Fa-f]+)\s*,\s*0x([0-9A-Fa-f]+)\s*,\s*(\d+)\s*\}', text)
        self.first = int(m.group(1), 16)
        self.last = int(m.group(2), 16)
        self.y_adv = int(m.group(3))
        self.glyph = glyphs
        assert len(self.glyph) == self.last - self.first + 1, (path, len(self.glyph), self.last - self.first + 1)

    def glyph_for(self, ch):
        idx = ord(ch) - self.first
        if idx < 0 or idx >= len(self.glyph):
            idx = ord('?') - self.first
        return self.glyph[idx]

    def draw_char(self, cv, ch, x, y, color):
        off, w, h, xa, xo, yo = self.glyph_for(ch)
        for yy in range(h):
            for xx in range(w):
                bit_index = yy * w + xx
                byte = self.bitmaps[off + bit_index // 8]
                mask = 0x80 >> (bit_index % 8)
                if byte & mask:
                    cv.pixel(x + xx, y + yy, color)

    def text_bounds(self, s):
        cursor_x = 0
        x1 = y1 = x2 = y2 = 0
        for ch in s:
            off, w, h, xa, xo, yo = self.glyph_for(ch)
            xa_ = cursor_x + xo
            ya_ = yo
            x1 = min(x1, xa_); y1 = min(y1, ya_)
            x2 = max(x2, xa_ + w); y2 = max(y2, ya_ + h)
            cursor_x += xa
        return x1, y1, x2 - x1, y2 - y1

    def draw_centered(self, cv, s, cx, cy, color):
        x1, y1, w, h = self.text_bounds(s)
        ox = cx - w // 2 - x1
        oy = cy - (h >> 1) - y1
        x = 0
        for ch in s:
            off, gw, gh, xa, xo, yo = self.glyph_for(ch)
            self.draw_char(cv, ch, ox + x + xo, oy + yo, color)
            x += xa

    def draw_centered_left(self, cv, s, x, cy, color):
        """left-aligned at cursor x, vertically centered on cy (like setCursor)"""
        _, y1, _, h = self.text_bounds(s)
        oy = cy - (h >> 1) - y1
        cx = x
        for ch in s:
            off, gw, gh, xa, xo, yo = self.glyph_for(ch)
            self.draw_char(cv, ch, cx + xo, oy + yo, color)
            cx += xa


_HERE = os.path.dirname(os.path.abspath(__file__))
_FONTS = os.path.join(_HERE, '..', 'orb', 'Fonts')
FONT9 = Font(os.path.join(_FONTS, 'FreeSansBold9pt7b.h'))
FONT12 = Font(os.path.join(_FONTS, 'FreeSansBold12pt7b.h'))
FONT18 = Font(os.path.join(_FONTS, 'FreeSansBold18pt7b.h'))
FONT24 = Font(os.path.join(_FONTS, 'FreeSansBold24pt7b.h'))

# ============================================================== STATE ======
class Tama:
    """Mirror of the firmware's TamaData + mood timers for a still frame."""

    def __init__(self, stage=3, pers=3, form=1, accessory=0, hunger=80, fun=80,
                 energy=80, age_sec=60000, fav_food=0, state=0):
        self.stage = stage          # 0 egg,1 baby,2 teen,3 adult
        self.pers = pers            # 0 lazy,1 hyper,2 picky,3 cuddly
        self.form = form            # 0 balanced,1 chubby,2 athletic,3 sparkly
        self.accessory = accessory  # 0 none,1 hat,2 glasses,3 bow,4 crown
        self.hunger = float(hunger)
        self.fun = float(fun)
        self.energy = float(energy)
        self.age_sec = age_sec
        self.fav_food = fav_food
        self.state = state          # 0 awake,1 eating,2 playing,3 sleeping,4 dead

    def evolve_stage(self):
        if self.age_sec < 3600:
            return 1
        if self.age_sec < 43200:
            return 2
        return 3


P_HUE = (210, 25, 110, 330)                    # lazy hyper picky cuddly
FORM_SCALE = (1.0, 1.06, 0.94, 1.0)            # balanced chubby athletic sparkly
STAGE_NAME = ("EGG", "BABY", "TEEN", "ADULT")
FORM_NAME = ("", "CHUBBY", "ATHLETIC", "SPARKLY")
P_NAME = ("LAZY", "HYPER", "PICKY", "CUDDLY")
FD_NAME = ("burger", "apple", "cake", "fish", "candy")
ACC_NAME = ("none", "hat", "glasses", "bow", "crown")
ACC_COL = (0, rgb565(200, 60, 60), rgb565(60, 60, 200), rgb565(230, 120, 200), rgb565(240, 200, 60))
ICON = ((58, 186), (120, 198), (182, 186))
CHOICE = ((70, 118), (170, 118))
FOOD_CHOICE = ((30, 118), (78, 118), (126, 118), (174, 118), (222, 118))
POOP_SPOTS = ((44, 146), (196, 146), (86, 174), (154, 174))


class Moods:
    """which mood timers are 'active' for the still frame."""

    def __init__(self, happy=False, celebrating=False, grumpy=False, curious=False,
                 zooming=False, angry=False, excited=False, sleepy=False, shy=False,
                 stretching=False, waving=False, yawning=False, dizzy=False):
        self.happy = happy
        self.celebrating = celebrating
        self.grumpy = grumpy
        self.curious = curious
        self.zooming = zooming
        self.angry = angry
        self.excited = excited
        self.sleepy = sleepy
        self.shy = shy
        self.stretching = stretching
        self.waving = waving
        self.yawning = yawning
        self.dizzy = dizzy


# ======================================================== RENDER SCENES ====
def render_scene(cv, tms, tama, moods, particles=(), bubble=0,
                 info=False, chooser=False, food_chooser=False, game=None,
                 poops=(), refuse=False, night=False, clock_h=None):
    # fast clear
    cv.fb[:] = [COL_BG] * (W * H)
    if tama.state == 4:                                   # dead
        draw_death(cv, tms)
        return
    if night and clock_h is not None and tama.state == 3:
        draw_night_clock(cv, clock_h)
        return
    draw_gauge_ring(cv, tama)
    cv.fill_circle(120, 108, 84, COL_BODY)
    # rim dots
    for a in range(100, 176, 4):
        cs = fast_cos10(a); sn = fast_sin10(a)
        rx = 120 + (82 * cs >> 10)
        ry = 108 - (82 * sn >> 10)
        cv.fill_circle(rx, ry, 1, COL_RIM)
    draw_poops(cv, poops)
    draw_pet(cv, tms, tama, moods, particles, refuse)
    draw_icons(cv, sleeping=(tama.state == 3), playing=(tama.state == 2 and game is not None))
    if chooser:
        draw_chooser(cv)
    if food_chooser:
        draw_food_chooser(cv)
    if info:
        draw_info(cv, tama, tms)
    if game is not None:
        draw_game_overlay(cv, tms, game)


def draw_gauge_ring(cv, tama):
    hq = int(tama.hunger * 2.55)
    fq = int(tama.fun * 2.55)
    eq = int(tama.energy * 2.55)

    def arc_track(center_deg, span, c):
        half = span // 2
        for a in range(center_deg - half, center_deg + half + 1, 5):
            arc_dot(113, a, c)

    def arc_value(center_deg, q, c):
        if q < 3:
            return
        span = int(100.0 * (q / 255.0))
        half = span // 2
        for a in range(center_deg - half, center_deg + half + 1, 4):
            arc_dot(113, a, c)

    def arc_dot(r, ang_deg, c):
        cs = fast_cos10(int(ang_deg))
        sn = fast_sin10(int(ang_deg))
        x = 120 + (r * cs >> 10)
        y = 108 - (r * sn >> 10)
        cv.fill_circle(x, y, 2, c)

    arc_track(150, 100, COL_TRACK)
    arc_track(30, 100, COL_TRACK)
    arc_track(270, 100, COL_TRACK)
    arc_track(90, 100, COL_TRACK)
    arc_value(150, hq, lerp_color(COL_BAD, COL_GOOD, hq))
    arc_value(30, fq, lerp_color(rgb565(120, 120, 140), COL_HEART, fq))
    arc_value(270, eq, lerp_color(COL_BAD, rgb565(90, 190, 255), eq))


def draw_poops(cv, active):
    for i in range(4):
        if not active[i]:
            continue
        x, y = POOP_SPOTS[i]
        cv.fill_circle(x, y + 3, 7, COL_POOP)
        cv.fill_circle(x - 3, y - 2, 5, COL_POOP)
        cv.fill_circle(x + 3, y - 3, 4, COL_POOP)
        cv.fill_circle(x, y - 7, 3, COL_POOP)


def draw_pet(cv, tms, tama, m, particles, refuse=False):
    scale = (0.72 if tama.stage == 1 else 0.88 if tama.stage == 2 else 1.0) * FORM_SCALE[tama.form]
    hue = (tms * 0.006 + tama.stage * 90.0 + P_HUE[tama.pers]) % 360.0
    iris_col = hsv565(hue, 0.72, 0.85)
    iris_dark = hsv565(hue, 0.85, 0.45)
    sleeping = tama.state == 3
    sad = (not sleeping and not m.happy and not m.grumpy and not m.zooming and
           not m.angry and not m.excited and not m.sleepy and not m.dizzy and
           (tama.hunger < 25 or tama.fun < 20 or tama.energy < 15))
    if m.angry:
        iris_col = rgb565(220, 50, 50)
        iris_dark = rgb565(160, 30, 30)

    squash = 1.0
    breathe = math.sin(tms * (2 * math.pi / 4200.0))
    if sleeping:
        squash = 0.12
    elif m.happy or m.celebrating:
        squash = 0.62 + 0.06 * math.sin(tms * 0.02)
    elif m.angry:
        squash = 0.92 + 0.03 * math.sin(tms * 0.03)
    elif m.excited:
        squash = 0.58 + 0.10 * math.sin(tms * 0.03)
    elif m.sleepy:
        squash = 0.88 + 0.04 * math.sin(tms * 0.005)
    elif m.dizzy:
        squash = 0.85 + 0.12 * math.sin(tms * 0.015)
    elif sad:
        squash = 0.82

    droop = 0.88 if sad else 1.0
    if m.stretching:
        droop = 0.70 + 0.30 * math.sin((tms % 1500) * math.pi / 1500.0)
    blink_phase = 1.0
    ry = int((42.0 * squash * droop) * scale *
             (1.0 if sleeping else (1.0 - 0.96 * (1.0 - blink_phase))))
    rx = int(44.0 * scale * (1.0 + 0.02 * breathe))
    if ry < 2:
        ry = 2
    iris_r = int(21 * scale)
    pup_r = int(11 * scale)
    if m.curious:
        iris_r = int(iris_r * 1.05)
        pup_r = int(pup_r * 0.8)

    shift = int(math.sin(tms * 0.045) * 72) if m.zooming else 0
    if m.waving:
        shift += int(math.sin(tms * 0.012) * 8)
    if m.dizzy:
        shift += int(math.sin(tms * 0.008) * 18)
    eye_cy = 104 + int(3.0 * breathe) + (4 if sleeping else 0)
    gx = int(math.sin(tms * 0.01) * 12) if m.dizzy else 0
    gy = int(math.cos(tms * 0.013) * 8) if m.dizzy else 0

    draw_antenna(cv, tms, iris_col, scale, shift, tama)
    if tama.accessory != 0:
        draw_accessory(cv, tama.accessory, shift, top_y=56)

    if sleeping:
        draw_shut_eye(cv, 76 + shift, eye_cy, int(40 * scale))
        draw_shut_eye(cv, 164 + shift, eye_cy, int(40 * scale))
    elif m.dizzy:
        sey = eye_cy
        for eye in range(2):
            ecx = (76 if eye == 0 else 164) + shift
            for ai in range(0, int(6.0 * math.pi / 0.25)):
                a = ai * 0.25
                r = a * 2.8 * scale
                rot = a + tms * 0.004 * (1 if eye == 0 else -1)
                sx = ecx + int(math.cos(rot) * r)
                sy = sey + int(math.sin(rot) * r)
                cv.pixel(sx, sy, iris_col)
                cv.pixel(sx + 1, sy, iris_col)
            cv.fill_circle(ecx, sey, int(5 * scale), COL_SCLERA)
            cv.fill_circle(ecx, sey, int(3 * scale), iris_col)
    else:
        if m.shy:
            ry = ry // 2
        draw_eye_int(cv, 76 + shift, eye_cy, rx, ry, gx, gy, iris_r, pup_r,
                     iris_col, iris_dark, sad)
        draw_eye_int(cv, 164 + shift, eye_cy, rx, ry, gx, gy, iris_r, pup_r,
                     iris_col, iris_dark, sad)

    draw_mouth(cv, tms, sleeping, m.happy or m.celebrating, sad, m.grumpy,
               m.angry, m.excited, m.sleepy, m.shy, m.yawning, m.waving, m.dizzy)

    if refuse:
        cv.fill_circle(120, 52, 13, COL_SCLERA)
        FONT12.draw_centered(cv, "!", 120, 52, COL_BAD)
    if m.angry:
        cv.draw_line(98, 62, 106, 56, rgb565(220, 50, 50))
        cv.draw_line(106, 56, 114, 62, rgb565(220, 50, 50))
        cv.draw_line(126, 62, 134, 56, rgb565(220, 50, 50))
        cv.draw_line(134, 56, 142, 62, rgb565(220, 50, 50))

    for (px, py, pt) in particles:
        if pt == 0:    # heart
            draw_heart(cv, px, py, 1, COL_HEART)
        elif pt == 3:  # sparkle
            draw_sparkle(cv, px, py, COL_GOOD)


def draw_antenna(cv, tms, c, scale, shift, tama):
    if tama.stage == 1:
        cv.fill_circle(120 + shift, 54, 3, c)
        return
    sway = math.sin(tms * 0.004) * 0.35
    # firmware: sway = fastSin10(tms*0.004*180/PI % 360) * 0.35 / 1024
    sway_deg = int(tms * 0.004 * (180.0 / math.pi)) % 360
    sway = fast_sin10(sway_deg) * 0.35 / 1024.0
    swayc = fast_cos10(sway_deg) * 0.35 / 1024.0
    length = 24.0 if tama.form == 2 else 20.0 if tama.stage == 3 else 15.0
    bx = 120 + shift
    by = 58
    tx = bx + sway * length
    ty = by - swayc * length
    cv.draw_line(int(bx), int(by), int(tx), int(ty), COL_RIM)
    cv.draw_line(int(bx) + 1, int(by), int(tx), int(ty), COL_RIM)
    cv.fill_circle(int(tx), int(ty), 4, rgb565(255, 255, 255) if tama.form == 3 else c)
    cv.fill_circle(int(tx) - 1, int(ty) - 1, 2, COL_SCLERA)


def draw_accessory(cv, acc, shift, top_y=56):
    if acc == 1:  # hat
        cv.fill_ellipse(120 + shift, top_y - 14, 22, 10, ACC_COL[1])
        cv.fill_round_rect(112 + shift, top_y - 18, 16, 8, 3, ACC_COL[1])
    elif acc == 2:  # glasses
        cv.draw_circle(80 + shift, 104, 12, ACC_COL[2])
        cv.draw_circle(160 + shift, 104, 12, ACC_COL[2])
        cv.draw_line(92 + shift, 104, 148 + shift, 104, ACC_COL[2])
        cv.draw_line(68 + shift, 104, 60 + shift, 100, ACC_COL[2])
        cv.draw_line(172 + shift, 104, 180 + shift, 100, ACC_COL[2])
    elif acc == 3:  # bow
        cv.fill_triangle(120 + shift, top_y - 6, 104 + shift, top_y - 16,
                         104 + shift, top_y + 4, ACC_COL[3])
        cv.fill_triangle(120 + shift, top_y - 6, 136 + shift, top_y - 16,
                         136 + shift, top_y + 4, ACC_COL[3])
        cv.fill_circle(120 + shift, top_y - 6, 3, rgb565(255, 200, 230))
    elif acc == 4:  # crown
        c = ACC_COL[4]
        cv.fill_round_rect(104 + shift, top_y - 20, 32, 10, 2, c)
        cv.fill_triangle(108 + shift, top_y - 20, 112 + shift, top_y - 30,
                         116 + shift, top_y - 20, c)
        cv.fill_triangle(120 + shift, top_y - 20, 120 + shift, top_y - 34,
                         120 + shift, top_y - 20, c)
        cv.fill_triangle(124 + shift, top_y - 20, 128 + shift, top_y - 30,
                         132 + shift, top_y - 20, c)


def draw_shut_eye(cv, cx, cy, rxw):
    rr = rxw * rxw
    for dx in range(-rxw, rxw + 1):
        yy = cy + int(3 * (rr - dx * dx) / rr)
        cv.fill_circle(cx + dx, yy, 2, rgb565(60, 66, 84))


def draw_eye_int(cv, cx, cy, rx, ry, gx, gy, iris_r, pup_r,
                 iris_col, iris_dark, sad):
    rx2 = rx * rx
    ry2 = ry * ry
    iris_r2 = iris_r * iris_r
    pup_r2 = pup_r * pup_r
    ix = cx + gx
    iy = cy + gy
    hx = ix - (iris_r * 9) // 20
    hy = iy - (iris_r * 9) // 20
    hl_r2 = 18
    blush_y = cy + (ry * 55) // 100 + 6
    sad_cutoff = (cy - ry // 3) if sad else -1
    y0 = max(cy - ry - 1, 0)
    y1 = min(cy + ry + 1, 239)
    for py in range(y0, y1 + 1):
        dy = py - cy
        dy2 = dy * dy
        if dy2 > ry2:
            continue
        q = ((ry2 - dy2) << 10) // ry2
        hw = (rx * isqrt(q << 10)) >> 10
        if hw < 1:
            continue
        x0 = max(cx - hw, 0)
        x1 = min(cx + hw, 239)
        sad_row = sad and py < sad_cutoff
        for px in range(x0, x1 + 1):
            col = rgb565(205, 212, 224) if sad_row else COL_SCLERA
            di = (px - ix) * (px - ix) + (py - iy) * (py - iy)
            if di < iris_r2:
                col = iris_dark if (py - iy) > 2 else iris_col
                if di < pup_r2:
                    col = COL_PUPIL
                else:
                    dhx = px - hx
                    dhy = py - hy
                    if dhx * dhx + dhy * dhy < hl_r2:
                        col = rgb565(255, 255, 255)
            cv.pixel(px, py, col)
    if not sad:
        cv.fill_circle(cx, blush_y, 4, rgb565(255, 130, 150))


def draw_mouth(cv, tms, sleeping, happy, sad, grumpy, angry, excited, sleepy,
               shy, yawning, waving, dizzy):
    if sleeping or grumpy:
        return
    if dizzy:
        moff = int(math.sin(tms * 0.012) * 3)
        cv.draw_circle(120 + moff, 148, 8, COL_RIM)
        cv.draw_circle(120 + moff, 148, 7, COL_RIM)
        return
    if yawning:
        open_ = int(12.0 * math.sin((tms % 1200) * math.pi / 1200.0))
        cv.fill_ellipse(120, 166, 8 + open_, 6 + open_, rgb565(26, 14, 20))
        cv.fill_ellipse(120, 167, 4 + open_ // 2, 3 + open_ // 2, rgb565(200, 100, 120))
        return
    if angry:
        for i in range(13):
            x = 108 + i * 2
            y = 178 + (i * 2 if i < 6 else (12 - i) * 2)
            cv.fill_circle(x, y, 2, rgb565(200, 60, 60))
        return
    if sad:
        for a in range(35, 146, 3):
            cs = fast_cos10(a); sn = fast_sin10(a)
            x = 120 + (30 * cs >> 10)
            y = 184 - (30 * sn >> 10)
            cv.fill_circle(x, y, 3, rgb565(120, 126, 142))
        return
    if excited:
        for a in range(35, 146, 2):
            cs = fast_cos10(a); sn = fast_sin10(a)
            x = 120 + (48 * cs >> 10)
            y = 128 + (48 * sn >> 10)
            if y > 210:
                continue
            cv.fill_circle(x, y, 5, rgb565(255, 255, 255))
        for a in range(45, 136, 3):
            cs = fast_cos10(a); sn = fast_sin10(a)
            x = 120 + (38 * cs >> 10)
            y = 132 + (38 * sn >> 10)
            if y > 205:
                continue
            cv.fill_circle(x, y, 3, rgb565(235, 240, 248))
        return
    if shy:
        wx = 120 + int(math.sin(tms * 0.01) * 2)
        cv.fill_round_rect(wx - 6, 162, 12, 3, 1, rgb565(180, 120, 140))
        return
    if happy:
        for a in range(35, 146, 2):
            cs = fast_cos10(a); sn = fast_sin10(a)
            x = 120 + (42 * cs >> 10)
            y = 132 + (42 * sn >> 10)
            if y > 205:
                continue
            cv.fill_circle(x, y, 4, rgb565(235, 240, 248))
        return
    cv.fill_round_rect(112, 160, 16, 3, 1, rgb565(90, 96, 112))


def draw_heart(cv, x, y, s, c):
    cv.fill_circle(x - 3 * s, y - 2 * s, 3 * s, c)
    cv.fill_circle(x + 3 * s, y - 2 * s, 3 * s, c)
    cv.fill_triangle(x - 6 * s, y, x + 6 * s, y, x, y + 7 * s, c)


def draw_sparkle(cv, x, y, c):
    cv.draw_line(x - 5, y, x + 5, y, c)
    cv.draw_line(x, y - 5, x, y + 5, c)
    cv.fill_circle(x, y, 2, c)


def draw_icons(cv, sleeping=False, playing=False):
    c = COL_ICONDIM if sleeping else COL_ICON
    # burger
    x, y = ICON[0]
    cv.fill_round_rect(x - 11, y - 9, 22, 7, 3, c)
    cv.fill_rect(x - 11, y - 1, 22, 3, c)
    cv.fill_round_rect(x - 11, y + 3, 22, 6, 3, c)
    # ball
    x, y = ICON[1]
    cv.fill_circle(x, y - 2, 9, COL_GOOD if playing else c)
    cv.draw_line(x - 9, y - 2, x + 9, y - 2, COL_BG)
    cv.draw_arc(x, y - 2, 9, 12, 200, 340, c)
    # drop
    x, y = ICON[2]
    cv.fill_triangle(x, y - 12, x - 8, y + 2, x + 8, y + 2, c)
    cv.fill_circle(x, y + 2, 8, c)
    cv.fill_circle(x - 3, y, 3, COL_BG)


def draw_chooser(cv):
    cv.fill_circle(120, 116, 100, rgb565(8, 10, 16))
    cv.draw_circle(120, 116, 100, COL_RIM)
    for g in range(2):
        cx, cy = CHOICE[g]
        cv.fill_circle(cx, cy, 28, rgb565(24, 28, 40))
        cv.draw_circle(cx, cy, 28, COL_RIM)
        if g == 0:   # pop: pulsing dot
            cv.fill_circle(cx, cy, 10, rgb565(90, 190, 255))
            cv.fill_circle(cx - 3, cy - 3, 4, rgb565(210, 240, 255))
            FONT9.draw_centered(cv, "pop", cx, cy + 40, rgb565(150, 160, 185))
        else:        # catch: falling burger
            cv.fill_round_rect(cx - 8, cy - 6, 16, 5, 2, rgb565(120, 80, 40))
            cv.fill_rect(cx - 8, cy - 1, 16, 2, rgb565(235, 220, 120))
            cv.fill_round_rect(cx - 8, cy + 1, 16, 4, 2, rgb565(120, 80, 40))
            cv.fill_circle(cx, cy + 14, 2, rgb565(240, 200, 120))
            FONT9.draw_centered(cv, "catch", cx, cy + 40, rgb565(150, 160, 185))
    FONT9.draw_centered(cv, "pick a game", 120, 190, rgb565(130, 138, 158))


def draw_food_chooser(cv):
    cv.fill_circle(120, 116, 100, rgb565(8, 10, 16))
    cv.draw_circle(120, 116, 100, COL_RIM)
    for f in range(5):
        cx, cy = FOOD_CHOICE[f]
        cv.fill_circle(cx, cy, 20, rgb565(24, 28, 40))
        cv.draw_circle(cx, cy, 20, COL_RIM)
        if f == 0:  # burger
            cv.fill_round_rect(cx - 10, cy - 6, 20, 4, 2, rgb565(180, 120, 60))
            cv.fill_rect(cx - 10, cy - 2, 20, 2, rgb565(100, 180, 60))
            cv.fill_round_rect(cx - 10, cy, 20, 3, 2, rgb565(180, 120, 60))
            cv.fill_round_rect(cx - 10, cy + 3, 20, 4, 2, rgb565(200, 150, 80))
        elif f == 1:  # apple
            cv.fill_circle(cx, cy + 2, 10, rgb565(200, 60, 60))
            cv.fill_circle(cx - 3, cy - 1, 10, rgb565(180, 40, 40))
            cv.fill_rect(cx - 1, cy - 12, 2, 6, rgb565(100, 70, 30))
            cv.fill_circle(cx + 3, cy - 10, 3, rgb565(80, 160, 60))
        elif f == 2:  # cake
            cv.fill_round_rect(cx - 10, cy - 2, 20, 10, 3, rgb565(230, 180, 220))
            cv.fill_round_rect(cx - 10, cy + 2, 20, 6, 3, rgb565(200, 140, 190))
            cv.fill_circle(cx, cy - 4, 3, rgb565(255, 80, 80))
        elif f == 3:  # fish
            cv.fill_ellipse(cx + 2, cy, 12, 7, rgb565(100, 160, 220))
            cv.fill_triangle(cx - 10, cy, cx - 16, cy - 6, cx - 16, cy + 6, rgb565(80, 140, 200))
            cv.fill_circle(cx + 6, cy - 2, 2, rgb565(40, 40, 80))
        else:        # candy
            cv.fill_ellipse(cx, cy, 8, 6, rgb565(220, 180, 255))
            cv.fill_triangle(cx - 8, cy, cx - 14, cy - 4, cx - 14, cy + 4, rgb565(180, 140, 220))
            cv.fill_triangle(cx + 8, cy, cx + 14, cy - 4, cx + 14, cy + 4, rgb565(180, 140, 220))
        FONT9.draw_centered(cv, FD_NAME[f], cx, cy + 28, rgb565(150, 160, 185))
    FONT9.draw_centered(cv, "pick a snack", 120, 190, rgb565(130, 138, 158))


def draw_info(cv, tama, tms):
    # flashing border (pick steady frame: border ON)
    cv.draw_rect_border(rgb565(90, 170, 255))
    cv.fill_circle(120, 116, 98, rgb565(30, 34, 48))
    cv.draw_circle(120, 116, 98, rgb565(90, 110, 150))
    cv.fill_circle(120, 116, 93, rgb565(22, 25, 36))
    cv.fill_round_rect(120 - 88, 16, 176, 16, 6, rgb565(60, 80, 120))
    FONT9.draw_centered(cv, "PET STATS", 120, 24, rgb565(230, 238, 250))
    if tama.form != 0:
        line = "%s %s" % (STAGE_NAME[tama.stage], FORM_NAME[tama.form])
    else:
        line = STAGE_NAME[tama.stage]
    FONT12.draw_centered(cv, line, 120, 46, rgb565(240, 244, 252))
    FONT9.draw_centered(cv, P_NAME[tama.pers], 120, 68, rgb565(170, 180, 200))
    bar(cv, 88, "FULL", tama.hunger, COL_GOOD, COL_BAD)
    bar(cv, 114, "FUN ", tama.fun, COL_HEART, rgb565(120, 120, 140))
    bar(cv, 140, "Zzz ", tama.energy, rgb565(90, 190, 255), COL_BAD)
    days = tama.age_sec // 86400
    if tama.stage == 1:
        hours_in_stage = tama.age_sec // 3600
        milestone = "TEEN"
    elif tama.stage == 2:
        hours_in_stage = (tama.age_sec - 3600) // 3600
        milestone = "ADULT"
    else:
        hours_in_stage = (tama.age_sec - 43200) // 3600
        milestone = "mature"
    sp = stage_pct(tama)
    FONT9.draw_centered(cv, "day %d  (%dh to %s)" % (days, hours_in_stage, milestone),
                        120, 156, rgb565(180, 188, 205))
    cv.fill_rect(120 - 100, 166, 200, 4, rgb565(30, 34, 44))
    cv.fill_rect(120 - 100, 166, int(200 * sp), 4,
                 lerp_color(COL_BAD, COL_GOOD, int(sp * 255)))
    FONT9.draw_centered(cv, "OFFLINE (no wifi)", 120, 186, rgb565(150, 158, 175))
    FONT9.draw_centered(cv, "USB power", 120, 196, rgb565(180, 188, 205))
    if tama.accessory != 0:
        FONT9.draw_centered(cv, "hat: %s" % ACC_NAME[tama.accessory], 120, 210,
                            ACC_COL[tama.accessory])
    FONT9.draw_centered(cv, "fav: %s" % FD_NAME[tama.fav_food], 120, 224,
                        rgb565(150, 180, 200))
    FONT9.draw_centered(cv, "tap to close", 120, 238, rgb565(140, 148, 165))


def stage_pct(tama):
    if tama.stage == 1:
        return tama.age_sec / 3600.0
    if tama.stage == 2:
        return (tama.age_sec - 3600) / 39600.0
    return 1.0


def bar(cv, y, label, v, hi, lo):
    FONT9.draw_centered_left(cv, label, 38, y + 6, rgb565(140, 148, 165))
    cv.draw_round_rect(86, y - 4, 116, 12, 5, COL_RIM)
    w = int(112 * (v / 100.0))
    if w > 0:
        cv.fill_round_rect(88, y - 2, w, 8, 4, lerp_color(lo, hi, int(v * 2.55)))


def draw_game_overlay(cv, tms, game):
    if game['type'] == 0:   # pop
        pulse = 20 + int(math.sin(tms * 0.02) * 1.6)
        tx, ty = game['tx'], game['ty']
        cv.fill_circle(tx, ty, pulse, rgb565(90, 190, 255))
        cv.fill_circle(tx - pulse // 3, ty - pulse // 3, pulse // 3, rgb565(210, 240, 255))
        frac = 1.0 - (game['el'] / 3200.0)
        arc_deg = int(360.0 * frac)
        for a in range(-90, -90 + arc_deg, 6):
            cs = fast_cos10(a); sn = fast_sin10(a)
            dx = ((pulse + 5) * cs) >> 10
            dy = ((pulse + 5) * sn) >> 10
            cv.fill_circle(tx + dx, ty - dy, 1, COL_GOOD)
        FONT9.draw_centered(cv, "%d/5" % game['hits'], 120, 40, rgb565(150, 160, 185))
    else:                    # catch
        for i in range(3):
            if not game['drops'][i]['on']:
                continue
            x, y = game['drops'][i]['x'], game['drops'][i]['y']
            cv.fill_round_rect(x - 8, y - 6, 16, 5, 2, rgb565(120, 80, 40))
            cv.fill_rect(x - 8, y - 1, 16, 2, rgb565(235, 220, 120))
            cv.fill_round_rect(x - 8, y + 1, 16, 4, 2, rgb565(120, 80, 40))
        FONT9.draw_centered(cv, "%d/5" % game['score'], 120, 40, rgb565(150, 160, 185))


def draw_egg_intro(cv, tms, el):
    cv.fill_circle(120, 116, 96, rgb565(8, 10, 16))
    wobble = min(el / 60000.0, 1.0) * 10.0
    ex = 120 + int(math.sin(el * 0.008) * wobble)
    cv.fill_ellipse(ex, 118, 34, 44, rgb565(225, 228, 238))
    cv.fill_ellipse(ex - 8, 104, 10, 14, rgb565(245, 246, 252))
    if el > 120000:
        cv.draw_line(ex - 14, 106, ex - 4, 114, rgb565(90, 96, 112))
    if el > 180000:
        cv.draw_line(ex - 4, 114, ex + 6, 108, rgb565(90, 96, 112))
        cv.draw_line(ex + 6, 108, ex + 2, 124, rgb565(90, 96, 112))
    if el > 240000:
        cv.draw_line(ex + 2, 124, ex - 10, 130, rgb565(90, 96, 112))
        cv.draw_line(ex - 10, 130, ex + 4, 138, rgb565(90, 96, 112))
    if el < 180000:
        FONT9.draw_centered(cv, "something is moving", 120, 186, rgb565(130, 138, 158))
    else:
        FONT9.draw_centered(cv, "almost...", 120, 186, rgb565(130, 138, 158))


def draw_death(cv, tms, tama):
    cv.fb[:] = [rgb565(8, 6, 12)] * (W * H)
    cv.fill_rect(0, 180, 240, 60, rgb565(28, 20, 14))
    for x in range(0, 240, 3):
        h = 2 + ((x * 7 + 13) % 5)
        cv.fill_rect(x, 180 - h, 2, h, rgb565(22, 16, 10))
    cv.fill_round_rect(80, 90, 80, 90, 6, rgb565(90, 95, 110))
    cv.fill_round_rect(84, 94, 72, 82, 4, rgb565(70, 75, 90))
    cv.fill_circle(120, 94, 38, rgb565(90, 95, 110))
    cv.fill_circle(120, 94, 34, rgb565(70, 75, 90))
    cv.fill_rect(117, 100, 6, 30, rgb565(110, 115, 130))
    cv.fill_rect(108, 110, 24, 6, rgb565(110, 115, 130))
    FONT18.draw_centered(cv, "R.I.P.", 120, 82, rgb565(140, 145, 165))
    days = tama.age_sec // 86400
    FONT9.draw_centered(cv, "%s %s" % (STAGE_NAME[tama.stage], P_NAME[tama.pers]),
                        120, 200, rgb565(100, 105, 120))
    FONT9.draw_centered(cv, "lived %d day%s" % (days, "" if days == 1 else "s"),
                        120, 214, rgb565(80, 85, 100))
    # ghost wisps (animate with tms)
    for i in range(3):
        wx = 90 + i * 30 + int(math.sin(tms * 0.001 + i * 2.1) * 12)
        phase = (tms * 0.02 + i * 40.0) % 100.0
        wy = 60 - int(phase)
        alpha = int(100 - phase)
        gc = lerp_color(rgb565(60, 60, 80), rgb565(8, 6, 12), 255 - alpha)
        cv.fill_circle(wx, wy, 3 + (alpha >> 6), gc)
    FONT9.draw_centered(cv, "tap to hatching", 120, 232, rgb565(70, 75, 90))


def draw_night_clock(cv, t):
    hh, mm, ss, dow, day = t
    cv.draw_circle(120, 120, 116, rgb565(20, 22, 30))
    cv.draw_circle(120, 120, 114, rgb565(14, 15, 20))
    for i in range(60):
        deg = i * 6
        ca = fast_cos10(deg); sa = fast_sin10(deg)
        major = (i % 5 == 0)
        r_out = 106 if major else 109
        c = rgb565(120, 128, 148) if major else rgb565(50, 55, 70)
        x0 = 120 + (101 * ca >> 10)
        y0 = 120 + (101 * sa >> 10)
        x1 = 120 + (r_out * ca >> 10)
        y1 = 120 + (r_out * sa >> 10)
        cv.draw_line(x0, y0, x1, y1, c)
        if major:
            x2 = 120 + (102 * ca >> 10)
            y2 = 120 + (102 * sa >> 10)
            cv.draw_line(x2, y2, x1, y1, c)
    sec = ss + 0.0
    minv = mm + sec / 60.0
    hrsv = (hh % 12) + minv / 60.0
    clock_hand(cv, hrsv * 30 * math.pi / 180.0, 50, 4, rgb565(150, 158, 178))
    clock_hand(cv, minv * 6 * math.pi / 180.0, 78, 3, rgb565(120, 128, 148))
    sdeg = (int(sec * 6)) % 360
    cs = fast_cos10(sdeg); ss_ = fast_sin10(sdeg)
    sx0 = 120 - (16 * cs >> 10)
    sy0 = 120 - (16 * ss_ >> 10)
    sx1 = 120 + (90 * cs >> 10)
    sy1 = 120 + (90 * ss_ >> 10)
    cv.draw_line(sx0, sy0, sx1, sy1, rgb565(150, 60, 70))
    cv.fill_circle(120, 120, 5, rgb565(150, 60, 70))
    cv.fill_circle(120, 120, 2, rgb565(200, 205, 220))
    DOW = ("SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT")
    FONT9.draw_centered(cv, "%s %d" % (DOW[dow], day), 120, 176, rgb565(70, 76, 94))


def clock_hand(cv, rad, length, width, color):
    deg = int(rad * (180.0 / math.pi)) % 360
    ca = fast_cos10(deg); sa = fast_sin10(deg)
    x0 = 120 - ca * 10.0 / 1024.0
    y0 = 120 - sa * 10.0 / 1024.0
    x1 = 120 + ca * length / 1024.0
    y1 = 120 + sa * length / 1024.0
    cv.draw_line(int(x0), int(y0), int(x1), int(y1), color)
    dx = x1 - x0
    dy = y1 - y0
    l = math.sqrt(dx * dx + dy * dy)
    if l < 1:
        return
    ox = -dy / l * width * 0.5
    oy = dx / l * width * 0.5
    cv.draw_line(int(x0 + ox), int(y0 + oy), int(x1 + ox), int(y1 + oy), color)
    cv.draw_line(int(x0 - ox), int(y0 - oy), int(x1 - ox), int(y1 - oy), color)


# ------------------------------------------------------------- PNG output --
def framebuffer_to_image(cv):
    img = Image.new('RGB', (W, H))
    px = img.load()
    for y in range(H):
        base = y * W
        row = [rgb_of(cv.fb[base + x]) for x in range(W)]
        for x, rgb in enumerate(row):
            px[x, y] = rgb
    return img


def device_view(img, scale=3):
    """Round GC9A01 screen (r=120) centered on a dark bezel, upscaled."""
    S = 240 * scale
    out = Image.new('RGB', (S, S), (12, 12, 16))
    # bezel ring shape = dark grey ring around the glass
    mask = Image.new('L', (S, S), 0)
    dm = Image.new('L', (240 * scale, 240 * scale), 0)
    from PIL import ImageDraw
    d = ImageDraw.Draw(dm)
    d.ellipse([0, 0, S - 1, S - 1], fill=255)
    big = img.resize((S, S), Image.NEAREST)
    out.paste(big, (0, 0), dm)
    # subtle rim highlight
    d2 = ImageDraw.Draw(out)
    d2.ellipse([2, 2, S - 3, S - 3], outline=(70, 74, 92), width=2)
    return out

# ============================================================ SCENE DRIVER ======
SCENES = []


def scene(name, tms, tama, moods=None, **kw):
    moods = moods or Moods()
    SCENES.append((name, tms, tama, moods, kw))


# --- main happy adult (cuddly, chubby, bow) ---
scene("adult-happy", 1800,
      Tama(stage=3, pers=3, form=1, accessory=3, hunger=85, fun=80, energy=75,
           age_sec=61200, fav_food=1),
      Moods(happy=True), particles=[(85, 40, 0), (160, 34, 0)])

# --- baby laughing ---
scene("baby-happy", 6000,
      Tama(stage=1, pers=1, form=0, accessory=0, hunger=80, fun=95, energy=90,
           age_sec=3000, fav_food=0, state=0),
      Moods(excited=True), particles=[(70, 44, 0), (172, 36, 3), (100, 28, 3)])

# --- teen curious ---
scene("teen-curious", 9000,
      Tama(stage=2, pers=0, form=2, accessory=2, hunger=70, fun=65, energy=60,
           age_sec=20000, fav_food=2),
      Moods(curious=True))

# --- sleepy ---
scene("sleepy", 3200,
      Tama(stage=3, pers=2, form=1, accessory=4, hunger=55, fun=60, energy=18,
           age_sec=80000, fav_food=4),
      Moods(sleepy=True))

# --- sleeping (shut eyes, dim) ---
scene("sleeping", 5000,
      Tama(stage=3, pers=3, form=1, accessory=0, hunger=70, fun=70, energy=0,
           age_sec=70000, fav_food=1, state=3),
      Moods())

# --- zoomies ---
scene("zoomies", 2600,
      Tama(stage=3, pers=1, form=3, accessory=0, hunger=90, fun=99, energy=95,
           age_sec=90000, fav_food=3),
      Moods(zooming=True), particles=[(60, 90, 2), (180, 110, 2), (120, 60, 3)])

# --- angry ---
scene("angry", 4200,
      Tama(stage=3, pers=0, form=1, accessory=0, hunger=60, fun=3, energy=80,
           age_sec=65000, fav_food=1),
      Moods(angry=True), refuse=True)

# --- sad ---
scene("sad", 10000,
      Tama(stage=2, pers=2, form=0, accessory=0, hunger=20, fun=15, energy=40,
           age_sec=26000, fav_food=2),
      Moods(), poops=(True, False, True, False))

# --- stats panel (adult) ---
scene("stats-adult", 7000,
      Tama(stage=3, pers=3, form=1, accessory=3, hunger=82, fun=64, energy=48,
           age_sec=54000, fav_food=1),
      info=True)

# --- stats panel (teen, progress bar halfway) ---
scene("stats-teen", 7000,
      Tama(stage=2, pers=0, form=2, accessory=2, hunger=70, fun=70, energy=70,
           age_sec=22000, fav_food=2),
      info=True)

# --- food chooser ---
scene("food-chooser", 6000,
      Tama(stage=3, pers=2, form=1, accessory=4, hunger=50, fun=60, energy=70,
           age_sec=61000, fav_food=4),
      food_chooser=True)

# --- game chooser ---
scene("game-chooser", 6000,
      Tama(stage=3, pers=1, form=3, accessory=0, hunger=70, fun=65, energy=80,
           age_sec=61000, fav_food=3),
      chooser=True)

# --- pop game ---
scene("game-pop", 4000,
      Tama(stage=3, pers=1, form=0, accessory=0, hunger=70, fun=60, energy=80,
           age_sec=61000, fav_food=0, state=2),
      game={'type': 0, 'tx': 70, 'ty': 116, 'hits': 3, 'el': 1200, 'drops': []})

# --- catch game ---
scene("game-catch", 4000,
      Tama(stage=3, pers=1, form=0, accessory=0, hunger=70, fun=60, energy=80,
           age_sec=61000, fav_food=0, state=2),
      game={'type': 1, 'tx': 0, 'ty': 0, 'score': 3, 'el': 0,
            'drops': [{'on': 1, 'x': 60, 'y': 90}, {'on': 1, 'x': 185, 'y': 60},
                      {'on': 1, 'x': 120, 'y': 130}]})

# --- night clock ---
scene("night-clock", 3000,
      Tama(stage=3, pers=3, form=1, accessory=0, hunger=80, fun=80, energy=0,
           age_sec=70000, fav_food=1, state=3),
      night=True, clock_h=(1, 23, 45, 1, 3))

# --- egg intro (cracking) ---
def _egg():
    cv = Canvas()
    draw_egg_intro(cv, 0, 270000)
    return cv
SCENES.append(("egg", 0, None, None, {'custom': _egg}))

# --- death ---
def _death():
    cv = Canvas()
    draw_death(cv, 5000, Tama(stage=3, pers=2, form=1, accessory=0,
                              hunger=0, fun=0, energy=0, age_sec=120000, fav_food=1))
    return cv
SCENES.append(("death", 0, None, None, {'custom': _death}))


def main():
    outdir = sys.argv[1] if len(sys.argv) > 1 else 'screenshots'
    os.makedirs(outdir, exist_ok=True)
    for (name, tms, tama, moods, kw) in SCENES:
        cv = kw.get('custom', lambda: None)()
        if cv is None:
            cv = Canvas()
            render_scene(cv, tms, tama, moods,
                         particles=kw.get('particles', ()),
                         info=kw.get('info', False),
                         chooser=kw.get('chooser', False),
                         food_chooser=kw.get('food_chooser', False),
                         game=kw.get('game'),
                         poops=kw.get('poops', (False, False, False, False)),
                         refuse=kw.get('refuse', False),
                         night=kw.get('night', False),
                         clock_h=kw.get('clock_h'))
        img = framebuffer_to_image(cv)
        img.save(os.path.join(outdir, name + '.png'))
        dv = device_view(img)
        dv.save(os.path.join(outdir, name + '-device.png'))
        print('wrote', name)
    print('all done ->', outdir)


if __name__ == '__main__':
    main()
