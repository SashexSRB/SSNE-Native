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

# Must match enum PromptGlyph in runtime/include/ssne/runtime.h (1-based; 0 = keep the GameCube glyph).
NAMES = ["XB_A", "XB_B", "XB_X", "XB_Y", "XB_LB", "XB_RB", "XB_LT", "XB_RT", "XB_LS", "XB_RS", "XB_VIEW",
         "XB_MENU", "DPAD_UP", "DPAD_DOWN", "DPAD_LEFT", "DPAD_RIGHT", "PS_CROSS", "PS_CIRCLE", "PS_SQUARE",
         "PS_TRIANGLE", "PS_L1", "PS_R1", "PS_L2", "PS_R2", "PS_L3", "PS_R3", "PS_SHARE", "PS_OPTIONS"]

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
    }
    return [icons[n] for n in NAMES]

def main():
    icons = build()
    out = os.path.join(ROOT, "runtime", "src", "gx", "prompt_icons.inc")
    with open(out, "w") as f:
        f.write("// Generated by tools/make_prompt_icons.py - do not edit. Straight (non-premultiplied) RGBA.\n")
        f.write("// Letters: Outfit Bold, SIL Open Font License 1.1.\n")
        for n, im in zip(NAMES, icons):
            data = im.tobytes()
            f.write("static const uint8_t k%s[%d] = {" % (n, len(data)))
            f.write(",".join(str(b) for b in data))
            f.write("};\n")
        f.write("static const PromptIcon kPromptIcons[] = {\n    {0, 0, nullptr},\n")
        for n, im in zip(NAMES, icons):
            f.write("    {%d, %d, k%s},\n" % (im.width, im.height, n))
        f.write("};\n")
    sheet = Image.new("RGBA", (sum(i.width + 4 for i in icons), H), (90, 70, 40, 255))
    x = 0
    for im in icons:
        sheet.alpha_composite(im, (x, 0)); x += im.width + 4
    sheet.save(os.environ.get("PROMPT_SHEET", "/tmp/prompt_icons.png"))
    print("wrote", out)

if __name__ == "__main__":
    main()
