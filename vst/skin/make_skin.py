#!/usr/bin/env python3
"""Draw the MPC skin's artwork from upstream's panel art and write vst/layout.conf (one place for every coordinate).

    python3 vst/skin/make_skin.py          (Pillow; tools/build_skin.sh runs it in Docker)

Inputs: upstream/ui/*.png (upstream's artwork masters, unmodified; positions from upstream's plugin/ui-layout.csv and
src/ui/Design.h, copied below), Titillium Web Bold from the Open MPC checkout (tools/html_art/fonts, OFL).
Outputs: vst/art/*.png and vst/layout.conf (both generated: edit this script, not them).

MPC limits this follows (Open MPC docs/NOTES.md): knob filmstrips are 128 square frames, so a knob's 2r + 10 <= 128;
slider strips are frames of the slider's own size, at most 12288 px of strip; live text is Titillium Web or Roboto
only (the seven-segment program display is therefore pictures, and the value displays live text).

Pages (MPC's plugin area, 1280 x 628):
  REVERB  upstream's main panel and its MIX column: the program buttons and LEDs (a picture per program, clear taps
          on top), the program display (seven segments, a picture per program), the three value displays (live
          text), the six funnels (vertical sliders: each frame is the funnel with its lit segment), the six levers
          (top half steps up, bottom half down), MIX (the red fader), PAN (upstream's knob), OVL (the LED).
  SETUP   upstream's INPUT and OUTPUT faders, and the bottom bar's settings in upstream's gold frames: FLOAT / 16-BIT,
          XFMR, WIDTH and the ducker (CAL is not shown: it stays at its default).
The REVERB page leaves out upstream's LEVEL meter LEDs (no VU meter in the port); OVL is kept. MODE (upstream's
ORIGINAL / TRUE ST bottom-bar button) is on SETUP, in the CHARACTER row.
"""
import math
import os

from PIL import Image, ImageDraw, ImageFilter, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
VST = os.path.dirname(HERE)
ROOT = os.path.dirname(VST)
UI = os.path.join(ROOT, "upstream", "ui")
ART = os.path.join(VST, "art")
MV = os.environ.get("MPC_VST", os.path.join(os.path.dirname(ROOT), "mpc-vst-plugins"))
FONT = os.path.join(MV, "tools", "html_art", "fonts", "TitilliumWeb-Bold.ttf")

PAGE_W, PAGE_H, PAGE_Y = 1280, 628, 86   # MPC's plugin area; layout y = skin y + PAGE_Y
MAX_STRIP = 12288                         # slider strips (NOTES.md 2026-10-07)

# ---- upstream geometry (design pixels of main.png, 3495 x 1380), from plugin/ui-layout.csv and src/ui/Design.h ----
LEFT_END, MIX_FROM, MIX_TO = 2504, 2508, 2846   # the main panel; the right panel's left edge and MIX column
PROG_X, PROG_Y = 445, [586, 738, 889, 1041, 1193]
LED_X = 563
BUTTON, LED, JOY, KNOB = 125, 50, 193, 153
JOY_X, JOY_Y = [897, 1198, 1431, 1722, 2004, 2280], 1167
FUNNEL_EDGES = [(560, 817, 976), (600, 825, 968), (640, 831, 959), (680, 838, 953), (720, 844, 947), (760, 849, 941),
                (800, 854, 935), (840, 858, 930), (880, 862, 926), (920, 867, 921), (960, 872, 917), (1000, 876, 912)]
FUNNEL_DX = [0, 301, 534, 825, 1107, 1383]
FUNNEL_TOP, FUNNEL_BOTTOM, FUNNEL_SEGMENTS, FUNNEL_INSET, FUNNEL_GAP = 526.0, 1029.0, 10, 3.0, 3.0
DISPLAYS = {"program": (490.0, 235.5, 434, 151), "lowpass": (897.5, 235.5, 285, 151),
            "refl": (1317.5, 235.5, 461, 151), "decay": (2002.0, 235.5, 828, 151)}
OVL_BOX = (149.5, 525.0, 52, 75)
LEVEL_ERASE = (88, 576, 322, 1332)       # the LEVEL LEDs +3 .. -12 and "LEVEL dB", below OVL's row
LEVEL_PATCH_X, LEVEL_PATCH_Y = 368, 515  # clean panel inside the PROGRAM frame, as large as LEVEL_ERASE
PAN = (2687, 263)
MIX_X, MIX_TOP, MIX_BOTTOM = 2684, 545.0, 1185.0
GAIN_X, GAIN_TOP, GAIN_BOTTOM = [2996, 3307], 218.0, 1238.0
FADER_W, FADER_H = 100, 211
JOYS = ["lowpass", "delay", "ampl", "lf", "rt", "hf"]

# Upstream's colours (Design.h, Joystick.cpp, SegmentDisplay.cpp) and the panel's gold / cream.
SEG_LIT, SEG_GLOW = (255, 138, 58), (255, 10, 0)
LIT_EDGE, LIT_MIDDLE, AMBIENT = (255, 46, 24), (255, 90, 48), (255, 36, 16)
GOLD, GOLD_DARK, CREAM, RED_TEXT = (226, 176, 64), (120, 84, 24), (239, 230, 207), (255, 40, 30)

# Composite of the main panel and the MIX column, then the scale to MPC's width.
COMP_W = LEFT_END + (MIX_TO - MIX_FROM)
S = PAGE_W / COMP_W
PAD_Y = (PAGE_H - round(1380 * S)) // 2


def cx_of(ox):
    """Main.png x -> composite x (the MIX column moves left by the gap between the two panels)."""
    return ox if ox < LEFT_END + 2 else ox - (MIX_FROM - LEFT_END)


def sx(ox):
    return cx_of(ox) * S


def sy(oy):
    return oy * S + PAD_Y


def ui(name, size=None):
    im = Image.open(os.path.join(UI, name)).convert("RGBA")
    return im.resize(size, Image.LANCZOS) if size else im


def paste_centred(canvas, im, cx, cy):
    canvas.alpha_composite(im, (round(cx - im.width / 2), round(cy - im.height / 2)))


def font(px):
    return ImageFont.truetype(FONT, px)


def clamp_box(b, size):
    """A resampling box kept inside the image (rounding at the page edges can overshoot by a fraction of a pixel)."""
    return (max(0.0, b[0]), max(0.0, b[1]), min(float(size[0]), b[2]), min(float(size[1]), b[3]))


class Box:
    """An integer box on the page (skin pixels) and the same area in composite pixels, so a picture or a strip frame
    drawn at full resolution lands exactly on the background."""

    def __init__(self, cx, cy, w, h):
        self.x, self.y = round(cx - w / 2), round(cy - h / 2)
        self.w, self.h = round(w), round(h)

    def comp(self):
        return (self.x / S, (self.y - PAD_Y) / S, (self.x + self.w) / S, (self.y + self.h - PAD_Y) / S)

    def render(self, comp_canvas):
        """This box of a full-resolution canvas, downsampled to the page."""
        return comp_canvas.resize((self.w, self.h), Image.LANCZOS, box=clamp_box(self.comp(), comp_canvas.size))

    def layout(self):
        return "x=%d y=%d w=%d h=%d" % (self.x, self.y + PAGE_Y, self.w, self.h)

    def centre(self):
        return "cx=%d cy=%d" % (self.x + self.w // 2, self.y + self.h // 2 + PAGE_Y)


def glow(layer, radius, colour, alpha):
    """A blurred, tinted copy of `layer`'s alpha, for the segments' and funnels' red glow."""
    a = layer.split()[3].filter(ImageFilter.GaussianBlur(radius)).point(lambda v: int(v * alpha))
    out = Image.new("RGBA", layer.size, colour + (0,))
    out.putalpha(a)
    return out


# ---------------------------------------------------------------------------------------------------------------
def composite_reverb():
    main = ui("main.png")
    comp = Image.new("RGBA", (COMP_W, 1380), (13, 20, 24, 255))
    comp.alpha_composite(main.crop((0, 0, LEFT_END, 1380)), (0, 0))
    comp.alpha_composite(main.crop((MIX_FROM, 0, MIX_TO, 1380)), (LEFT_END, 0))
    # The LEVEL meter's LEDs (+3 .. -12) and their caption are not drawn (the port has no VU meter, user 2026-11-22):
    # covered with a clean patch of panel from inside the PROGRAM frame (hidden by the buttons), feathered at its
    # edges. OVL, its own row above, stays.
    x0, y0, x1, y1 = LEVEL_ERASE
    patch = main.crop((LEVEL_PATCH_X, LEVEL_PATCH_Y, LEVEL_PATCH_X + x1 - x0, LEVEL_PATCH_Y + y1 - y0))
    mask = Image.new("L", patch.size, 0)
    ImageDraw.Draw(mask).rectangle((8, -20, patch.width - 9, patch.height - 9), fill=255)  # hard top edge, under OVL
    comp.paste(patch, (x0, y0), mask.filter(ImageFilter.GaussianBlur(4)))
    # The levers at rest are part of the panel; the program buttons up, their LEDs off (the pictures draw the rest).
    for x in JOY_X:
        paste_centred(comp, ui("joy-mid.png", (JOY, JOY)), cx_of(x), JOY_Y)
    return comp


def program_picture(comp, p):
    c = comp.copy()
    for i, (y, letter) in enumerate(zip(PROG_Y, "SMLEN")):
        state = "In" if i == p else "Out"
        paste_centred(c, ui("%s %s.png" % (letter, state), (BUTTON, BUTTON)), PROG_X, y)
        led = ("yellow led on.png" if i == p else "Yellow led off.png") if letter == "N" else \
              ("red led on.png" if i == p else "red led off.png")
        paste_centred(c, ui(led, (LED, LED)), LED_X, y)
    return c


# Seven segments, as upstream's SegmentDisplay.
SA, SB, SC, SD, SE, SF, SG = 1, 2, 4, 8, 16, 32, 64
GLYPHS = {"0": SA | SB | SC | SD | SE | SF, "1": SB | SC, "2": SA | SB | SD | SE | SG, "5": SA | SC | SD | SF | SG,
          "S": SA | SC | SD | SF | SG, "H": SB | SC | SE | SF | SG, "T": SD | SE | SF | SG, "R": SE | SG,
          "M": SA | SB | SC | SE | SF, "E": SA | SD | SE | SF | SG, "D": SB | SC | SD | SE | SG, "L": SD | SE | SF,
          "N": SC | SE | SG, "G": SA | SC | SD | SE | SF, "I": SE | SF}
PROGRAM_NAMES = ["SHT.RT", "MED.RT", "LNG.RT", "E.250", "N.LIN"]   # upstream's params::programName


def segment_polys(w, h):
    t = w * 0.15
    gap = t * 0.7
    half = t * 0.5
    mid = h * 0.5
    hx0, hx1 = t + gap * 0.2, w - t - gap * 0.2

    def hseg(x0, x1, yc):
        return [(x0, yc), (x0 + half, yc - half), (x1 - half, yc - half), (x1, yc), (x1 - half, yc + half), (x0 + half, yc + half)]

    def vseg(xc, y0, y1):
        return [(xc, y0), (xc + half, y0 + half), (xc + half, y1 - half), (xc, y1), (xc - half, y1 - half), (xc - half, y0 + half)]

    return [hseg(hx0, hx1, half), vseg(w - half, 0, mid - gap * 0.35), vseg(w - half, mid + gap * 0.35, h),
            hseg(hx0, hx1, h - half), vseg(half, mid + gap * 0.35, h), vseg(half, 0, mid - gap * 0.35), hseg(hx0, hx1, mid)]


def draw_segments(comp, box_centre, box_size, text, digits=5):
    """Upstream's display: ghost segments, then the lit ones with three red halos (SegmentDisplay::paint)."""
    cx, cy = box_centre
    bw, bh = box_size
    x0, y0 = cx - bw / 2 + 26, cy - bh / 2 + 24
    aw, ah = bw - 52, bh - 48
    pitch = aw / digits
    w, h = pitch * 0.66, ah
    shear = math.tan(math.radians(4.0))
    polys = segment_polys(w, h)
    cells, run = [], []
    for ch in text:
        if ch == ".":
            if run:
                run[-1][1] = True
        else:
            run.append([GLYPHS.get(ch, 0), False])
    cells = [[0, False]] * max(0, digits - len(run)) + run

    def place(i, pts):
        ox = x0 + pitch * i + shear * h + pitch * 0.06
        return [(ox + px - shear * py, y0 + py) for px, py in pts]

    pr = w * 0.09

    def point(i):
        px = x0 + pitch * i + w + pitch * 0.15
        py = y0 + h - pr
        return (px - pr, py - pr, px + pr, py + pr)

    ghost = Image.new("RGBA", comp.size, (0, 0, 0, 0))
    gd = ImageDraw.Draw(ghost)
    lit = Image.new("RGBA", comp.size, (0, 0, 0, 0))
    ld = ImageDraw.Draw(lit)
    for i in range(digits):
        for s, poly in enumerate(polys):
            gd.polygon(place(i, poly), fill=SEG_GLOW + (int(255 * 0.2 * 1.3),))
            if cells[i][0] & (1 << s):
                ld.polygon(place(i, poly), fill=SEG_LIT + (255,))
        gd.ellipse(point(i), fill=SEG_GLOW + (int(255 * 0.2 * 1.3),))
        if cells[i][1]:
            ld.ellipse(point(i), fill=SEG_LIT + (255,))
    comp.alpha_composite(ghost)
    for radius, alpha in ((64 * 2.5 * 0.4, 0.5), (30 * 2.5 * 0.4, 0.7), (8 * 2.5 * 0.4, 0.8)):
        comp.alpha_composite(glow(lit, radius, SEG_GLOW, alpha))
    comp.alpha_composite(lit)


def funnel_edges(y):
    e = FUNNEL_EDGES
    i = 1
    while i < len(e) - 1 and e[i][0] < y:
        i += 1
    (py, pl, pr), (qy, ql, qr) = e[i - 1], e[i]
    t = (y - py) / (qy - py)
    return pl + (ql - pl) * t, pr + (qr - pr) * t


def wedge(col, ya, yb, steps=8):
    dx = FUNNEL_DX[col]
    left, right = [], []
    for i in range(steps + 1):
        y = ya + (yb - ya) * i / steps
        l, r = funnel_edges(y)
        left.append((cx_of(l + dx + FUNNEL_INSET), y))
        right.append((cx_of(r + dx - FUNNEL_INSET), y))
    return left + right[::-1]


def draw_funnel(comp, col, segment, ox=0, oy=0):
    """Upstream's Funnel::paint: faint lines between the segments, the lit one (counted from the bottom) with a red
    ambient tint around it inside the wedge and a glow spilling past its edges. `comp` is a crop of the composite at
    (ox, oy)."""
    seg_h = (FUNNEL_BOTTOM - FUNNEL_TOP) / FUNNEL_SEGMENTS

    def wedge_at(ya, yb):
        return [(x - ox, y - oy) for x, y in wedge(col, ya, yb)]

    inside = Image.new("L", comp.size, 0)
    ImageDraw.Draw(inside).polygon(wedge_at(FUNNEL_TOP, FUNNEL_BOTTOM), fill=255)
    y_bottom = FUNNEL_BOTTOM - seg_h * segment
    y_top = y_bottom - seg_h
    lit = Image.new("RGBA", comp.size, (0, 0, 0, 0))
    ImageDraw.Draw(lit).polygon(wedge_at(y_top + FUNNEL_GAP / 2, y_bottom - FUNNEL_GAP / 2), fill=LIT_EDGE + (255,))
    # spill: the glow outside the wedge too
    comp.alpha_composite(glow(lit, 34 * 0.4 * 2.5, (255, 26, 8), 0.55))
    comp.alpha_composite(glow(lit, 14 * 0.4 * 2.5, (255, 42, 20), 0.8))
    # ambient tint and lines, clipped to the wedge
    amb = Image.new("RGBA", comp.size, (0, 0, 0, 0))
    ad = ImageDraw.Draw(amb)
    reach = 0.24 * (FUNNEL_BOTTOM - FUNNEL_TOP)
    centre = (y_top + y_bottom) / 2
    for k in range(40):
        y0 = FUNNEL_TOP + (FUNNEL_BOTTOM - FUNNEL_TOP) * k / 40
        y1 = FUNNEL_TOP + (FUNNEL_BOTTOM - FUNNEL_TOP) * (k + 1) / 40
        d = abs((y0 + y1) / 2 - centre)
        a = max(0.0, 1.0 - d / reach) * 0.30
        if a > 0:
            ad.rectangle((0, y0 - oy, comp.width, y1 - oy), fill=AMBIENT + (int(255 * a),))
    for k in range(1, FUNNEL_SEGMENTS):
        y = FUNNEL_BOTTOM - seg_h * k - oy
        ad.rectangle((0, y - 1.75, comp.width, y + 1.75), fill=(255, 40, 20, int(255 * 0.45 * 0.35)))
    clipped = Image.new("RGBA", comp.size, (0, 0, 0, 0))
    clipped.paste(amb, (0, 0), inside)
    comp.alpha_composite(clipped)
    # the lit segment: edge colour, a brighter middle
    core = Image.new("RGBA", comp.size, (0, 0, 0, 0))
    ImageDraw.Draw(core).polygon(wedge_at(y_top + FUNNEL_GAP / 2 + seg_h * 0.22, y_bottom - FUNNEL_GAP / 2 - seg_h * 0.22),
                                 fill=LIT_MIDDLE + (255,))
    comp.alpha_composite(lit)
    comp.alpha_composite(core.filter(ImageFilter.GaussianBlur(4)))


def strip(frames_imgs):
    w, h = frames_imgs[0].size
    out = Image.new("RGBA", (w, h * len(frames_imgs)))
    for i, f in enumerate(frames_imgs):
        out.paste(f, (0, i * h))
    return out


def local(comp, box, margin=80):
    """A copy of just the composite area around `box` (to draw one frame cheaply), and its offset."""
    x0, y0, x1, y1 = box.comp()
    ox, oy = max(0, int(x0) - margin), max(0, int(y0) - margin)
    ex, ey = min(comp.width, int(math.ceil(x1)) + margin), min(comp.height, int(math.ceil(y1)) + margin)
    return comp.crop((ox, oy, ex, ey)), ox, oy


def render_local(region, ox, oy, box):
    x0, y0, x1, y1 = box.comp()
    return region.resize((box.w, box.h), Image.LANCZOS, box=clamp_box((x0 - ox, y0 - oy, x1 - ox, y1 - oy), region.size))


def fader_frames(base_region, ox, oy, box, cap, cap_x, y_at, n):
    frames = []
    for k in range(n):
        f = base_region.copy()
        cy = y_at(k / (n - 1))
        f.alpha_composite(cap, (round(cap_x - ox - cap.width / 2), round(cy - oy - cap.height / 2)))
        frames.append(render_local(f, ox, oy, box))
    return frames


def save(im, name):
    im.save(os.path.join(ART, name))
    return "art/" + name


# ---------------------------------------------------------------------------------------------------------------
def page_reverb(lines):
    comp = composite_reverb()
    bg = Image.new("RGBA", (PAGE_W, PAGE_H), (13, 20, 24, 255))
    full = Box(PAGE_W / 2, PAD_Y + 1380 * S / 2, PAGE_W, 1380 * S)
    bg.alpha_composite(full.render(comp), (full.x, full.y))
    lines.append("[tab REVERB]")
    lines.append("art file=%s fit=stretch" % save(bg.convert("RGB"), "bg_reverb.png"))

    # Program buttons and LEDs: a picture per program; clear taps on top.
    pbox = Box(sx(504), sy(889.5), (LED_X + LED / 2 + 8 - (PROG_X - BUTTON / 2 - 8)) * S, (PROG_Y[-1] - PROG_Y[0] + BUTTON + 16) * S)
    files = []
    for p in range(5):
        files.append(save(pbox.render(program_picture(comp, p)), "prog_%d.png" % p))
    lines.append('picture %s key=program files="%s"' % (pbox.layout(), ",".join(files)))
    pitch = (PROG_Y[1] - PROG_Y[0]) * S
    clear = save(Image.new("RGBA", (8, 8), (0, 0, 0, 0)), "clear.png")
    lines.append('enum_v cx=%d cy=%d sw=%d sh=%d gap=0 label="" key=program img=%s img_on=%s'
                 % (round(sx(PROG_X)), round(sy(889.5) + PAGE_Y), round(BUTTON * S + 10), round(pitch), clear, clear))

    # Program display: seven segments, a picture per program.
    dx, dy, dw, dh = DISPLAYS["program"]
    dbox = Box(sx(dx), sy(dy), dw * S, dh * S)
    files = []
    for p, name in enumerate(PROGRAM_NAMES):
        c = comp.copy()
        draw_segments(c, (cx_of(dx), dy), (dw, dh), name)
        files.append(save(dbox.render(c), "lcd_prog_%d.png" % p))
    lines.append('picture %s key=program files="%s"' % (dbox.layout(), ",".join(files)))

    # Value displays: live text (upstream's segment colour), the units are printed under them.
    for key, name in (("lcd_lp", "lowpass"), ("lcd_refl", "refl"), ("lcd_decay", "decay")):
        x, y, w, h = DISPLAYS[name]
        b = Box(sx(x), sy(y), (w - 40) * S, (h - 30) * S)
        lines.append('readout %s w=%d h=%d label="" key=%s tsize=46 tcolor=ff7a32 tweight=700 talign=center'
                     % (b.centre(), b.w, b.h, key))

    # OVL: upstream's red LED box, off and lit.
    ox_, oy_, ow, oh = OVL_BOX
    obox = Box(sx(ox_), sy(oy_), ow * S + 8, oh * S + 8)
    off = obox.render(comp)
    c = comp.copy()
    led = Image.new("RGBA", c.size, (0, 0, 0, 0))
    ImageDraw.Draw(led).rounded_rectangle((ox_ - ow / 2 + 5, oy_ - oh / 2 + 5, ox_ + ow / 2 - 5, oy_ + oh / 2 - 5), 8,
                                          fill=(255, 52, 36, 255))
    c.alpha_composite(glow(led, 22, (255, 20, 0), 0.9))
    c.alpha_composite(led)
    hi = Image.new("RGBA", c.size, (0, 0, 0, 0))
    ImageDraw.Draw(hi).rounded_rectangle((ox_ - ow / 2 + 12, oy_ - oh / 2 + 10, ox_ + ow / 2 - 12, oy_ - 6), 6,
                                         fill=(255, 170, 140, 120))
    c.alpha_composite(hi.filter(ImageFilter.GaussianBlur(4)))
    lines.append('picture %s key=ovl files="%s,%s"' % (obox.layout(), save(off, "ovl_0.png"), save(obox.render(c), "ovl_1.png")))


    # Funnels: vertical sliders whose frames are the funnel with its lit segment.
    for col, key in enumerate(JOYS):
        l0, r0 = FUNNEL_EDGES[0][1] + FUNNEL_DX[col], FUNNEL_EDGES[0][2] + FUNNEL_DX[col]
        fx = (l0 + r0) / 2
        fbox = Box(sx(fx), sy((FUNNEL_TOP + FUNNEL_BOTTOM) / 2), (r0 - l0 + 60) * S, (FUNNEL_BOTTOM - FUNNEL_TOP + 40) * S)
        n = max(2, min(64, MAX_STRIP // fbox.h))
        region, rox, roy = local(comp, fbox)
        by_segment = {}
        frames = []
        for k in range(n):
            seg = min(FUNNEL_SEGMENTS - 1, round(k / (n - 1) * (FUNNEL_SEGMENTS - 1)))
            if seg not in by_segment:
                canvas = region.copy()
                draw_funnel(canvas, col, seg, rox, roy)
                by_segment[seg] = render_local(canvas, rox, roy, fbox)
            frames.append(by_segment[seg])
        name = save(strip(frames), "funnel_%s.png" % key)
        lines.append('slider_v %s w=%d h=%d label="" key=%s strip=%s frames=%d ns=0 vs=0 bw=%d'
                     % (fbox.centre(), fbox.w, fbox.h, key, name, n, fbox.w))

    # Levers: the top half steps up, the bottom half down; the pressed half shows upstream's lever moved.
    for col, key in enumerate(JOYS):
        jx = JOY_X[col]
        for half, (pkey, moved) in enumerate(((key + "_up", "joy-up.png"), (key + "_dn", "joy-down.png"))):
            # the top half starts below the funnel slider's own (invisible) value label
            top_trim = 34 if half == 0 else 0
            hb = Box(sx(jx), sy(JOY_Y - JOY / 4 + top_trim / 2 if half == 0 else JOY_Y + JOY / 4), JOY * S,
                     (JOY / 2 - top_trim) * S)
            rest = hb.render(comp)
            c = comp.copy()
            # the lever moved, over the panel without the resting lever: repaint the panel there first
            region = ui("main.png").crop((int(jx - JOY), int(JOY_Y - JOY), int(jx + JOY), int(JOY_Y + JOY)))
            c.alpha_composite(region, (int(cx_of(jx) - JOY), int(JOY_Y - JOY)))
            paste_centred(c, ui(moved, (JOY, JOY)), cx_of(jx), JOY_Y)
            pressed = hb.render(c)
            lines.append('button %s w=%d h=%d label="" key=%s img=%s img_on=%s'
                         % (hb.centre(), hb.w, hb.h, pkey, save(rest, "joy_%s_%d.png" % (key, half)),
                            save(pressed, "joy_%s_%d_on.png" % (key, half))))

    # MIX: the red fader on its track.
    mbox = Box(sx(MIX_X), sy((MIX_TOP + MIX_BOTTOM) / 2), FADER_W * S + 14, (MIX_BOTTOM - MIX_TOP + FADER_H) * S)
    n = max(2, min(64, MAX_STRIP // mbox.h))
    region, rox, roy = local(comp, mbox, margin=120)
    cap = ui("red fader.png", (FADER_W, FADER_H))
    frames = fader_frames(region, rox, roy, mbox, cap, cx_of(MIX_X), lambda f: MIX_BOTTOM - f * (MIX_BOTTOM - MIX_TOP), n)
    lines.append('slider_v %s w=%d h=%d label="" key=mix strip=%s frames=%d ns=0 vs=0 bw=%d'
                 % (mbox.centre(), mbox.w, mbox.h, save(strip(frames), "fader_mix.png"), n, mbox.w + 20))

    # PAN: upstream's knob (the builder turns it; 2r + 10 <= 128).
    r = min(58, round(KNOB * S / 2))
    lines.append('knob cx=%d cy=%d r=%d label="" key=pan img=%s ns=0 vs=0'
                 % (round(sx(PAN[0])), round(sy(PAN[1]) + PAGE_Y), r, save(ui("Knob.png", (4 * r, 4 * r)), "knob.png")))
    lines.append('qlinks "REVERB" = lowpass,delay,ampl,lf,rt,hf,mix,pan')


# ---------------------------------------------------------------------------------------------------------------
def gold_frame(d, box, title=None, f=None):
    x0, y0, x1, y1 = box
    d.rounded_rectangle((x0 + 2, y0 + 2, x1 + 2, y1 + 2), 9, outline=(0, 0, 0, 140), width=4)
    d.rounded_rectangle(box, 9, outline=GOLD_DARK + (255,), width=5)
    d.rounded_rectangle((x0 + 1, y0 + 1, x1 - 1, y1 - 1), 8, outline=GOLD + (255,), width=3)
    if title:
        label(d, ((x0 + x1) / 2, y0 - 22), title, f)


def label(d, centre, text, f, colour=CREAM):
    w = d.textlength(text, font=f)
    x, y = centre[0] - w / 2, centre[1] - f.size / 2 - 4
    d.text((x + 2, y + 2), text, font=f, fill=(0, 0, 0, 170))
    d.text((x, y), text, font=f, fill=colour + (255,))


def page_setup(lines):
    main = ui("main.png")
    # The plate: upstream's panel texture (a clean strip of it, mirrored to fill), with its screws.
    # a clean area of the main panel (left of the program display, above the LEVEL LEDs), at the page's scale
    tex = main.crop((75, 80, 255, 430))
    tex = tex.resize((round(tex.width * S * 1.6), round(tex.height * S * 1.6)), Image.LANCZOS)
    plate = Image.new("RGBA", (PAGE_W, PAGE_H))
    for j, yy in enumerate(range(0, PAGE_H, tex.height)):
        for i, xx in enumerate(range(0, PAGE_W, tex.width)):
            t = tex.transpose(Image.FLIP_LEFT_RIGHT) if i % 2 else tex
            t = t.transpose(Image.FLIP_TOP_BOTTOM) if j % 2 else t
            plate.alpha_composite(t, (xx, yy))
    # INPUT and OUTPUT: upstream's right panel columns, at the REVERB page's scale.
    col_from, col_to = 2850, 3495
    cols = main.crop((col_from, 0, col_to, 1380)).resize((round((col_to - col_from) * S), round(1380 * S)), Image.LANCZOS)
    cols_x = PAGE_W - cols.width
    plate.alpha_composite(cols, (cols_x, PAD_Y))
    screw = main.crop((8, 10, 66, 68)).resize((round(58 * S), round(58 * S)), Image.LANCZOS)
    for sxp, syp in ((6, 6), (cols_x - screw.width - 6, 6), (6, PAGE_H - screw.height - 6),
                     (cols_x - screw.width - 6, PAGE_H - screw.height - 6)):
        plate.alpha_composite(screw, (sxp, syp))
    d = ImageDraw.Draw(plate)
    ft, fl = font(28), font(24)

    # Frames (skin pixels) and their labels, then the controls in them.
    x0, x1 = 30, cols_x - 24
    row1, row2 = (52, 292), (340, 604)
    # No CAL frame (the VU calibration table is not shown, user 2026-11-22; CAL stays at its default, the A/D's full
    # scale at 0 dBFS): CHARACTER takes the whole first row, STORAGE, TRANSFORMER and MODE side by side.
    char_box = (x0, row1[0], x1, row1[1])
    width_box = (x0, row2[0], x0 + 220, row2[1])
    duck_box = (width_box[2] + 24, row2[0], x1, row2[1])
    for b, t in ((char_box, "CHARACTER"), (width_box, "WIDTH"), (duck_box, "DUCKER")):
        gold_frame(d, b, t, ft)
    # captions inside the frames
    char_cx = [char_box[0] + off for off in (165, 450, 765)]
    char_cap_y, char_ctl_y = row1[0] + 70, row1[0] + 150
    label(d, (char_cx[0], char_cap_y), "STORAGE", fl)
    label(d, (char_cx[1], char_cap_y), "TRANSFORMER", fl)
    label(d, (char_cx[2], char_cap_y - 22), "MODE", fl)
    dk = [("RANGE", "duck_range"), ("THRESHOLD", "duck_thresh"), ("ATTACK", "duck_attack"), ("RELEASE", "duck_release")]
    dx0 = duck_box[0] + 150
    pitch = (duck_box[2] - 30 - dx0) / len(dk)
    knob_y = row2[0] + 132
    for i, (t, _) in enumerate(dk):
        label(d, (dx0 + pitch * (i + 0.5), row2[0] + 34), t, fl)
    label(d, ((duck_box[0] + dx0) / 2 + 10, row2[0] + 34), "ON", fl)

    lines.append("[tab SETUP]")
    lines.append("art file=%s fit=stretch" % save(plate.convert("RGB"), "bg_setup.png"))
    y = lambda v: round(v + PAGE_Y)  # noqa: E731
    lines.append('enum_h cx=%d cy=%d sw=130 sh=56 label="" key=bits options="FLOAT,16-BIT"' % (char_cx[0], y(char_ctl_y)))
    lines.append('enum_h cx=%d cy=%d sw=130 sh=56 label="" key=xfmr options="OFF,XFMR"' % (char_cx[1], y(char_ctl_y)))
    lines.append('enum_v cx=%d cy=%d sw=240 sh=52 label="" key=mode options="TRUE ST,ORIGINAL  L+R > ST"'
                 % (char_cx[2], y(char_ctl_y)))
    wcx = (width_box[0] + width_box[2]) / 2
    lines.append('knob cx=%d cy=%d r=54 label="" key=width img=art/knob.png ns=0 vs=30' % (wcx, y(row2[0] + 120)))
    lines.append('enum_v cx=%d cy=%d sw=96 sh=48 label="" key=duck options="OFF,ON"' % ((duck_box[0] + dx0) / 2 + 10, y(knob_y)))
    for i, (_, k) in enumerate(dk):
        lines.append('knob cx=%d cy=%d r=48 label="" key=%s img=art/knob.png ns=0 vs=30' % (dx0 + pitch * (i + 0.5), y(knob_y), k))

    # INPUT and OUTPUT: upstream's white faders on their tracks; the frames follow the printed scale (the parameter
    # is the fader position, as upstream's).
    for key, gx in zip(("in_gain", "out_gain"), GAIN_X):
        cxp = cols_x + (gx - col_from) * S
        gbox = Box(cxp, sy((GAIN_TOP + GAIN_BOTTOM) / 2), FADER_W * S + 14, (GAIN_BOTTOM - GAIN_TOP + FADER_H) * S)
        n = max(2, min(64, MAX_STRIP // gbox.h))
        cap = ui("White Fader.png", (round(FADER_W * S), round(FADER_H * S)))
        frames = []
        for k in range(n):
            f = plate.crop((gbox.x, gbox.y, gbox.x + gbox.w, gbox.y + gbox.h))
            cyp = sy(GAIN_BOTTOM - k / (n - 1) * (GAIN_BOTTOM - GAIN_TOP)) - gbox.y
            f.alpha_composite(cap, (round(gbox.w / 2 - cap.width / 2), round(cyp - cap.height / 2)))
            frames.append(f)
        lines.append('slider_v %s w=%d h=%d label="" key=%s strip=%s frames=%d ns=0 vs=0 bw=%d'
                     % (gbox.centre(), gbox.w, gbox.h, key, save(strip(frames), "fader_%s.png" % key), n, gbox.w + 30))
    lines.append('qlinks "SETUP" = in_gain,out_gain,width,bits,xfmr,mode,duck,duck_range,duck_thresh,duck_attack,duck_release')


THEME = """# REV-246 (MPC port). GENERATED by vst/skin/make_skin.py from upstream's panel art: edit the script, not this file.
art_css=skin.css
theme_bg=0d1418
theme_panel=17242b
theme_line=e2b040
theme_ink=efe6cf
theme_ink_dim=c9b98f
theme_ink_faint=8a7d5c
theme_accent=e2b040
theme_accent_hi=ffd27a
theme_knob_face=2c3036
theme_knob_ring=4a5059
theme_knob_dot=efe6cf
theme_bar=0b1013
theme_btn_text=efe6cf
theme_tab_on=e2b040
theme_tab_on_tx=0d1418
theme_lcd=000000
theme_seg_active=e2b040
theme_seg_inactive=1b2a33
theme_seg_active_tx=120c04
"""

CSS = """/* REV-246 (MPC port), GENERATED by vst/skin/make_skin.py. Over upstream's panel art: the value displays' boxes and
   the program taps are invisible; option segments look like upstream's bottom-bar buttons. */
.box { fill: none; stroke: none; }
.look-image .seg-tx, .look-image .seg-bg, .look-image .seg-sheen { display: none; }
.seg-bg { stroke: #e2b040; stroke-width: 2.5; rx: 7; }
.seg-sheen { display: none; }
.seg-tx { font-family: 'Titillium Web'; font-weight: 700; font-size: 24px; letter-spacing: 0.04em; }
"""


def main():
    os.makedirs(ART, exist_ok=True)
    lines = [THEME]
    page_reverb(lines)
    lines.append("")
    page_setup(lines)
    open(os.path.join(VST, "layout.conf"), "w").write("\n".join(lines) + "\n")
    open(os.path.join(VST, "skin.css"), "w").write(CSS)
    print("vst/layout.conf, vst/skin.css and %d images in vst/art" % len(os.listdir(ART)))


if __name__ == "__main__":
    main()
