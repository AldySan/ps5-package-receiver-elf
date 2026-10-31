#!/usr/bin/env python3
"""Generate the launcher icon (icon0.png) and web logo (logo.png) for the
PS5 package receiver payload.

Design: one shared mark = "wireless package receive"
    two signal arcs (incoming from the PC)
    a bold down arrow (the install)
    a rounded package frame (the PKG)
The icon is a full-bleed navy/blue gradient square for the PS5 home screen.
The logo is a dark rounded tile on transparency that blends with the WebUI
header (#171717 page, #202020 cards, #4F8EF7 accent).

Everything is drawn 4x oversampled and downscaled with Lanczos for crisp
anti-aliased edges. Faint fills are alpha-composited on their own layer
(ImageDraw replaces pixels instead of blending, so never pass alpha fills
straight to it). No text: the mark must read at 44 px.
"""
from PIL import Image, ImageDraw, ImageFilter

SS = 4                      # supersample factor
SIZE = 1024
W = SIZE * SS               # working canvas 4096


def sc(*vals):
    """Scale logical px to supersampled px (scalar for one arg)."""
    if len(vals) == 1:
        return vals[0] * SS
    return [v * SS for v in vals]


# mark layout, logical 1024 space (shared by icon and logo)
BOX = (300, 492, 724, 878)          # package frame
BOX_R = 60
SHAFT = (468, 375, 556, 600)        # arrow shaft (overlaps head neck)
HEAD = ((347, 585), (677, 585), (512, 740))   # arrow head triangle
ARC_C = (512, 740)                  # arcs concentric with the arrow tip
ARCS = ((495, 56), (590, 60))       # (radius, stroke) inner, outer


def draw_mark(img, box_c, arrow_c, arcs_c, fill_alpha=20,
              stroke_mul=1.0):
    """Draw the shared mark onto RGBA img with proper alpha blending."""
    # -- faint package fill on its own layer (alpha-blended) --
    fill = Image.new("RGBA", (W, W), (0, 0, 0, 0))
    fd = ImageDraw.Draw(fill)
    fd.rounded_rectangle(sc(*BOX), radius=sc(BOX_R), fill=(255, 255, 255,
                                                           fill_alpha))
    img.alpha_composite(fill)

    d = ImageDraw.Draw(img)

    # -- package frame --
    d.rounded_rectangle(sc(*BOX), radius=sc(BOX_R), outline=box_c,
                        width=int(56 * stroke_mul) * SS)

    # -- arrow: shaft + head (shaft drawn long to overlap the neck) --
    d.rectangle(sc(*SHAFT), fill=arrow_c)
    d.polygon([tuple(sc(*p)) for p in HEAD], fill=arrow_c)

    # -- two signal arcs opening downward, centered on the arrow tip --
    cx, cy = ARC_C
    for r, w in ARCS:
        bbox = sc(cx - r, cy - r, cx + r, cy + r)
        d.arc(bbox, start=236, end=304, fill=arcs_c,
              width=int(w * stroke_mul) * SS)


def vertical_gradient(size, top, bottom):
    """Linear vertical gradient as an RGB image."""
    w, h = size
    grad = Image.new("RGB", (1, h))
    for y in range(h):
        t = y / max(h - 1, 1)
        grad.putpixel((0, y), tuple(int(top[i] + (bottom[i] - top[i]) * t)
                                    for i in range(3)))
    return grad.resize((w, h))


def make_icon():
    """Full-bleed PS5 launcher icon, 1024x1024."""
    img = vertical_gradient((W, W), (0x08, 0x15, 0x2E), (0x12, 0x38, 0x6E)
                            ).convert("RGBA")

    # soft radial glow behind the mark
    glow = Image.new("L", (W, W), 0)
    gd = ImageDraw.Draw(glow)
    gd.ellipse(sc(512 - 480, 40, 512 + 480, 900), fill=46)
    glow = glow.filter(ImageFilter.GaussianBlur(190 * SS))
    img = Image.composite(Image.new("RGBA", (W, W), (140, 185, 255, 255)),
                          img, glow)

    draw_mark(img, box_c=(0xA9, 0xCC, 0xFF), arrow_c=(255, 255, 255),
              arcs_c=(0xA9, 0xCC, 0xFF), fill_alpha=48)
    return img.resize((SIZE, SIZE), Image.LANCZOS).convert("RGB")


def make_logo():
    """Rounded dark tile on transparency for the WebUI header / favicon."""
    img = Image.new("RGBA", (W, W), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)

    # tile: matches the WebUI cards (#202020) with a hairline border
    d.rounded_rectangle(sc(36, 36, 988, 988), radius=sc(192),
                        fill=(0x23, 0x25, 0x2B, 255),
                        outline=(0x3D, 0x40, 0x49, 255), width=6 * SS)
    # faint inner glow so the tile is not dead flat
    glow = Image.new("RGBA", (W, W), (0, 0, 0, 0))
    gd = ImageDraw.Draw(glow)
    gd.ellipse(sc(512 - 400, 512 - 400, 512 + 400, 512 + 400),
               fill=(79, 142, 247, 22))
    glow = glow.filter(ImageFilter.GaussianBlur(120 * SS))
    img = Image.alpha_composite(img, glow)

    # slightly thicker strokes: must stay readable at 44 px
    draw_mark(img, box_c=(0x7F, 0xB5, 0xFF), arrow_c=(255, 255, 255),
              arcs_c=(0x7F, 0xB5, 0xFF), fill_alpha=52, stroke_mul=1.12)
    return img.resize((SIZE, SIZE), Image.LANCZOS)


if __name__ == "__main__":
    import os
    # payload/ sits one level above this script
    out = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    icon = make_icon()
    # flat art + smooth gradient: a 256-color palette is visually identical
    # and keeps the embedded ELF small (the payload is sent over the LAN)
    icon.quantize(colors=256, method=Image.MEDIANCUT,
                  dither=Image.FLOYDSTEINBERG).save(
        os.path.join(out, "icon0.png"), optimize=True)
    # shown at 44 px in the header + favicon: 512 px source is plenty
    logo = make_logo().resize((512, 512), Image.LANCZOS)
    logo.quantize(colors=128, method=Image.FASTOCTREE).save(
        os.path.join(out, "logo.png"), optimize=True)
    print("written:", out)
