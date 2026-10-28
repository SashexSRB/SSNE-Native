#!/usr/bin/env python3
"""Generates runtime/src/gx/prompt_icons.inc: controller button glyphs (Xbox and PlayStation style) that replace the
GameCube button glyphs in the game's font textures (see gx_prompts.cpp). Drawn here with Pillow at 4x and
downsampled; letters use Outfit Bold (SIL OFL, launcher/fonts). Run from the repository root:
    python3 tools/make_prompt_icons.py
"""
from PIL import Image, ImageDraw, ImageFont
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FONT = os.path.join(ROOT, "launcher", "fonts", "Outfit-Bold.ttf")
S = 4          # supersampling
H = 48         # icon height

# Must match enum PromptGlyph in runtime/include/ssne/runtime.h (1-based; 0 = keep the GameCube glyph). The keyboard
# keycaps (KEYS) follow the named glyphs; the port finds them by SDL scancode name (ssne::prompt_key_glyph).
NAMES = ["XB_A", "XB_B", "XB_X", "XB_Y", "XB_LB", "XB_RB", "XB_LT", "XB_RT", "XB_LS", "XB_RS", "XB_VIEW",
         "XB_MENU", "DPAD_UP", "DPAD_DOWN", "DPAD_LEFT", "DPAD_RIGHT", "PS_CROSS", "PS_CIRCLE", "PS_SQUARE",
         "PS_TRIANGLE", "PS_L1", "PS_R1", "PS_L2", "PS_R2", "PS_L3", "PS_R3", "PS_SHARE", "PS_OPTIONS",
         "MS_LEFT", "MS_RIGHT", "MS_MIDDLE", "MS_WHEEL_UP", "MS_WHEEL_DOWN", "MS_BACK", "MS_FORWARD"]

# Keyboard keys with a keycap: (SDL scancode name, label). Labels starting with "@" are drawn symbols.
KEYS = [(c, c) for c in "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"] + [
    ("Space", "@space"), ("Return", "Enter"), ("Keypad Enter", "Enter"), ("Escape", "Esc"), ("Tab", "Tab"),
    ("Backspace", "@back"), ("Left Shift", "Shift"), ("Right Shift", "Shift"), ("Left Ctrl", "Ctrl"),
    ("Right Ctrl", "Ctrl"), ("Left Alt", "Alt"), ("Right Alt", "Alt"), ("CapsLock", "Caps"), ("Up", "@up"),
    ("Down", "@down"), ("Left", "@left"), ("Right", "@right"), ("Delete", "Del"), ("Insert", "Ins"),
    ("Home", "Home"), ("End", "End"), ("PageUp", "PgUp"), ("PageDown", "PgDn"), ("`", "`"), ("-", "-"),
    ("=", "="), ("[", "["), ("]", "]"), (";", ";"), ("'", "'"), (",", ","), (".", "."), ("/", "/"), ("\\", "\\"),
]

def font(px):
    return ImageFont.truetype(FONT, px)

def text_center(d, cx, cy, txt, px, fill, outline=None):
    f = font(px)
    b = d.textbbox((0, 0), txt, font=f)
    x = cx - (b[0] + b[2]) / 2
    y = cy - (b[1] + b[3]) / 2
    if outline:
        d.text((x, y), txt, font=f, fill=fill, stroke_width=int(px * 0.08), stroke_fill=outline)
    else:
        d.text((x, y), txt, font=f, fill=fill)

def canvas(w):
    im = Image.new("RGBA", (w * S, H * S), (0, 0, 0, 0))
    return im, ImageDraw.Draw(im)

def finish(im):
    return im.resize((im.width // S, im.height // S), Image.LANCZOS)

def xbox_face(letter, color):
    im, d = canvas(H)
    r = H * S / 2
    d.ellipse((1 * S, 1 * S, H * S - 1 * S, H * S - 1 * S), fill=(25, 25, 25, 255))
    d.ellipse((4 * S, 4 * S, H * S - 4 * S, H * S - 4 * S), fill=color)
    text_center(d, r, r - 1 * S, letter, int(H * S * 0.6), (255, 255, 255, 255), (20, 20, 20, 255))
    return finish(im)

def pill(label, w, fg=(30, 30, 30, 255), bg=(225, 225, 225, 255), radius=0.45, top_round=False):
    im, d = canvas(w)
    box = (1 * S, 4 * S, w * S - 1 * S, H * S - 4 * S)
    d.rounded_rectangle(box, radius=int((box[3] - box[1]) * radius), fill=(25, 25, 25, 255))
    inner = (box[0] + 3 * S, box[1] + 3 * S, box[2] - 3 * S, box[3] - 3 * S)
    d.rounded_rectangle(inner, radius=int((inner[3] - inner[1]) * radius), fill=bg)
    text_center(d, w * S / 2, H * S / 2 - 1 * S, label, int(H * S * 0.5), fg)
    return finish(im)

def trigger(label, w, bg=(225, 225, 225, 255)):
    im, d = canvas(w)
    # rounded on top, flatter bottom: like a trigger seen from the front
    d.rounded_rectangle((1 * S, 1 * S, w * S - 1 * S, H * S - 3 * S), radius=int(H * S * 0.42), fill=(25, 25, 25, 255))
    d.rectangle((1 * S, H * S * 0.55, w * S - 1 * S, H * S - 3 * S), fill=(25, 25, 25, 255))
    d.rounded_rectangle((4 * S, 4 * S, w * S - 4 * S, H * S - 6 * S), radius=int(H * S * 0.36), fill=bg)
    d.rectangle((4 * S, H * S * 0.55, w * S - 4 * S, H * S - 6 * S), fill=bg)
    text_center(d, w * S / 2, H * S / 2, label, int(H * S * 0.48), (30, 30, 30, 255))
    return finish(im)

def round_icon(draw_fn, bg=(225, 225, 225, 255)):
    im, d = canvas(H)
    d.ellipse((1 * S, 1 * S, H * S - 1 * S, H * S - 1 * S), fill=(25, 25, 25, 255))
    d.ellipse((4 * S, 4 * S, H * S - 4 * S, H * S - 4 * S), fill=bg)
    draw_fn(d)
    return finish(im)

def stick(label):
    return round_icon(lambda d: text_center(d, H * S / 2, H * S / 2 - 1 * S, label, int(H * S * 0.42), (30, 30, 30, 255)))

def menu_lines(d):
    for k in (-1, 0, 1):
        y = H * S / 2 + k * 8 * S
        d.rounded_rectangle((14 * S, y - 2 * S, 34 * S, y + 2 * S), radius=2 * S, fill=(30, 30, 30, 255))

def view_squares(d):
    d.rectangle((13 * S, 13 * S, 29 * S, 29 * S), outline=(30, 30, 30, 255), width=3 * S)
    d.rectangle((19 * S, 19 * S, 35 * S, 35 * S), fill=(225, 225, 225, 255), outline=(30, 30, 30, 255), width=3 * S)

def dpad(direction):
    im, d = canvas(H)
    c = H * S / 2
    a = 8 * S   # half arm width
    o = 22 * S  # arm length from center
    for (x0, y0, x1, y1) in ((c - a, c - o, c + a, c + o), (c - o, c - a, c + o, c + a)):
        d.rounded_rectangle((x0 - 3 * S, y0 - 3 * S, x1 + 3 * S, y1 + 3 * S), radius=4 * S, fill=(25, 25, 25, 255))
    for (x0, y0, x1, y1) in ((c - a, c - o, c + a, c + o), (c - o, c - a, c + o, c + a)):
        d.rounded_rectangle((x0, y0, x1, y1), radius=3 * S, fill=(150, 150, 150, 255))
    arm = {"up": (c - a, c - o, c + a, c - a), "down": (c - a, c + a, c + a, c + o),
           "left": (c - o, c - a, c - a, c + a), "right": (c + a, c - a, c + o, c + a)}[direction]
    d.rounded_rectangle(arm, radius=3 * S, fill=(255, 255, 255, 255))
    return finish(im)

def ps_face(shape, color):
    def draw(d):
        c = H * S / 2
        r = 11 * S
        w = 4 * S
        if shape == "cross":
            d.line((c - r, c - r, c + r, c + r), fill=color, width=w)
            d.line((c - r, c + r, c + r, c - r), fill=color, width=w)
        elif shape == "circle":
            d.ellipse((c - r, c - r, c + r, c + r), outline=color, width=w)
        elif shape == "square":
            d.rectangle((c - r + S, c - r + S, c + r - S, c + r - S), outline=color, width=w)
        else:
            d.polygon((c, c - r - 2 * S, c + r + 1 * S, c + r - 3 * S, c - r - 1 * S, c + r - 3 * S), outline=color, width=w)
    return round_icon(draw, bg=(30, 30, 30, 255))

DARK = (25, 25, 25, 255)
INK = (30, 30, 30, 255)
FACE = (235, 235, 235, 255)
LIP = (165, 165, 165, 255)
ACCENT = (255, 176, 32, 255)

def keycap(label):
    """A key seen from the front: dark outline, light face, darker lip at the bottom, the label in ink."""
    sym = label.startswith("@")
    px = int(H * S * (0.56 if len(label) == 1 else 0.40))
    tw = 0 if sym else (lambda b: b[2] - b[0])(ImageDraw.Draw(Image.new("RGBA", (1, 1))).textbbox((0, 0), label, font=font(px)))
    w = max(H, int(tw / S) + 22)
    im, d = canvas(w)
    W = w * S
    d.rounded_rectangle((1 * S, 2 * S, W - 1 * S, H * S - 2 * S), radius=10 * S, fill=DARK)
    d.rounded_rectangle((4 * S, 5 * S, W - 4 * S, H * S - 5 * S), radius=8 * S, fill=LIP)
    d.rounded_rectangle((4 * S, 5 * S, W - 4 * S, H * S - 10 * S), radius=8 * S, fill=FACE)
    cx, cy = W / 2, (5 * S + H * S - 10 * S) / 2
    if not sym:
        text_center(d, cx, cy, label, px, INK)
        return finish(im)
    a = 9 * S  # symbol half size
    t = 4 * S  # stroke
    if label in ("@up", "@down", "@left", "@right"):
        pts = {"@up": [(cx, cy - a), (cx + a, cy + a * 0.6), (cx - a, cy + a * 0.6)],
               "@down": [(cx, cy + a), (cx + a, cy - a * 0.6), (cx - a, cy - a * 0.6)],
               "@left": [(cx - a, cy), (cx + a * 0.6, cy - a), (cx + a * 0.6, cy + a)],
               "@right": [(cx + a, cy), (cx - a * 0.6, cy - a), (cx - a * 0.6, cy + a)]}[label]
        d.polygon(pts, fill=INK)
    elif label == "@back":  # long arrow to the left
        d.line((cx - a * 0.6, cy, cx + a * 1.2, cy), fill=INK, width=t)
        d.polygon([(cx - a * 1.3, cy), (cx - a * 0.4, cy - a * 0.8), (cx - a * 0.4, cy + a * 0.8)], fill=INK)
    elif label == "@space":  # the open-box space symbol
        d.line((cx - a * 1.2, cy - a * 0.3, cx - a * 1.2, cy + a * 0.5), fill=INK, width=t)
        d.line((cx + a * 1.2, cy - a * 0.3, cx + a * 1.2, cy + a * 0.5), fill=INK, width=t)
        d.line((cx - a * 1.2, cy + a * 0.5, cx + a * 1.2, cy + a * 0.5), fill=INK, width=t)
    return finish(im)

def mouse(part):
    """A mouse seen from above with the used part in the accent color; wheel icons add an arrow."""
    w = 44 if part in ("wheel_up", "wheel_down") else 34
    im, d = canvas(w)
    x0, x1 = 3 * S, 31 * S
    y0, y1 = 2 * S, H * S - 2 * S
    split = y0 + (y1 - y0) * 0.42
    d.rounded_rectangle((x0, y0, x1, y1), radius=14 * S, fill=DARK)
    ix0, ix1, iy0, iy1 = x0 + 3 * S, x1 - 3 * S, y0 + 3 * S, y1 - 3 * S
    d.rounded_rectangle((ix0, iy0, ix1, iy1), radius=11 * S, fill=FACE)
    mid = (x0 + x1) / 2
    if part in ("left", "right"):  # fill the button half, clipped to the body by redrawing the outline after
        hl = Image.new("RGBA", im.size, (0, 0, 0, 0))
        hd = ImageDraw.Draw(hl)
        hd.rounded_rectangle((ix0, iy0, ix1, iy1), radius=11 * S, fill=ACCENT)
        mask = Image.new("L", im.size, 0)
        md = ImageDraw.Draw(mask)
        md.rectangle((ix0, iy0, mid, split) if part == "left" else (mid, iy0, ix1, split), fill=255)
        im.paste(hl, (0, 0), mask)
    d.line((mid, iy0, mid, split), fill=DARK, width=3 * S)
    d.line((ix0, split, ix1, split), fill=DARK, width=3 * S)
    wheel_hl = part in ("middle", "wheel_up", "wheel_down")
    d.rounded_rectangle((mid - 4 * S, y0 + 8 * S, mid + 4 * S, y0 + 19 * S), radius=3 * S,
                        fill=ACCENT if wheel_hl else LIP, outline=DARK, width=2 * S)
    if part in ("back", "forward"):  # side buttons on the left edge
        yb = split + (5 * S if part == "forward" else 14 * S)
        d.rounded_rectangle((x0 - 1 * S, yb, x0 + 5 * S, yb + 7 * S), radius=2 * S, fill=ACCENT, outline=DARK, width=2 * S)
    if part in ("wheel_up", "wheel_down"):
        ax, ay, a = 38 * S, H * S / 2, 6 * S
        pts = [(ax, ay - a), (ax + a, ay + a * 0.5), (ax - a, ay + a * 0.5)] if part == "wheel_up" else \
              [(ax, ay + a), (ax + a, ay - a * 0.5), (ax - a, ay - a * 0.5)]
        d.polygon(pts, fill=DARK)
    return finish(im)

def build():
    icons = {
        "XB_A": xbox_face("A", (78, 176, 46, 255)), "XB_B": xbox_face("B", (214, 40, 40, 255)),
        "XB_X": xbox_face("X", (35, 110, 220, 255)), "XB_Y": xbox_face("Y", (240, 190, 20, 255)),
        "XB_LB": pill("LB", 60), "XB_RB": pill("RB", 60), "XB_LT": trigger("LT", 54), "XB_RT": trigger("RT", 54),
        "XB_LS": stick("LS"), "XB_RS": stick("RS"), "XB_VIEW": round_icon(view_squares), "XB_MENU": round_icon(menu_lines),
        "DPAD_UP": dpad("up"), "DPAD_DOWN": dpad("down"), "DPAD_LEFT": dpad("left"), "DPAD_RIGHT": dpad("right"),
        "PS_CROSS": ps_face("cross", (120, 160, 255, 255)), "PS_CIRCLE": ps_face("circle", (255, 90, 90, 255)),
        "PS_SQUARE": ps_face("square", (240, 130, 200, 255)), "PS_TRIANGLE": ps_face("triangle", (60, 210, 160, 255)),
        "PS_L1": pill("L1", 60), "PS_R1": pill("R1", 60), "PS_L2": trigger("L2", 54), "PS_R2": trigger("R2", 54),
        "PS_L3": stick("L3"), "PS_R3": stick("R3"),
        "PS_SHARE": pill("SHARE", 96, radius=0.5), "PS_OPTIONS": round_icon(menu_lines),
        "MS_LEFT": mouse("left"), "MS_RIGHT": mouse("right"), "MS_MIDDLE": mouse("middle"),
        "MS_WHEEL_UP": mouse("wheel_up"), "MS_WHEEL_DOWN": mouse("wheel_down"), "MS_BACK": mouse("back"),
        "MS_FORWARD": mouse("forward"),
    }
    named = [icons[n] for n in NAMES]
    caps = {}
    for _, label in KEYS:
        if label not in caps: caps[label] = keycap(label)
    labels = list(caps)
    return named, [caps[l] for l in labels], labels

def main():
    named, caps, labels = build()
    icons = named + caps
    names = NAMES + ["KEY_%d" % i for i in range(len(caps))]
    assert len(icons) < 256
    out = os.path.join(ROOT, "runtime", "src", "gx", "prompt_icons.inc")
    with open(out, "w") as f:
        f.write("// Generated by tools/make_prompt_icons.py - do not edit. Straight (non-premultiplied) RGBA.\n")
        f.write("// Letters: Outfit Bold, SIL Open Font License 1.1.\n")
        for n, im in zip(names, icons):
            data = im.tobytes()
            f.write("static const uint8_t k%s[%d] = {" % (n, len(data)))
            f.write(",".join(str(b) for b in data))
            f.write("};\n")
        f.write("static const PromptIcon kPromptIcons[] = {\n    {0, 0, nullptr},\n")
        for n, im in zip(names, icons):
            f.write("    {%d, %d, k%s},\n" % (im.width, im.height, n))
        f.write("};\n")
        f.write("// Keyboard keycaps by SDL scancode name (glyph ids follow the named glyphs).\n")
        f.write("static const struct { const char* name; uint8_t glyph; } kKeyGlyphs[] = {\n")
        for sdl_name, label in KEYS:
            esc = sdl_name.replace("\\", "\\\\").replace('"', '\\"')
            f.write('    {"%s", %d},\n' % (esc, len(NAMES) + 1 + labels.index(label)))
        f.write("};\n")
    sheet = Image.new("RGBA", (sum(i.width + 4 for i in icons), H), (90, 70, 40, 255))
    x = 0
    for im in icons:
        sheet.alpha_composite(im, (x, 0)); x += im.width + 4
    sheet.save(os.environ.get("PROMPT_SHEET", "/tmp/prompt_icons.png"))
    print("wrote", out)

if __name__ == "__main__":
    main()
