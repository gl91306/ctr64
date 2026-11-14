"""The N64 controller picture of the Controls screen.

Traced from a reference picture (tools/ui/ref/n64_ref2.png, 1500x1500 front view of the grey
controller on red):
- the outline is cut out of the red background;
- the body's soft shading (the shadow where the face overhangs the handles, the sides of the raised
  top plate, the shoulder buttons behind the lobes, the recesses round the D-pad, START, B/A and the
  C buttons) is the picture's own low-frequency shading: the buttons, D-pad cross, stick, logo and
  the C letter are filled in from their surroundings first (normalized convolution), then the result
  is smoothed so the plastic grain and fine print disappear;
- the buttons, D-pad cross and the stick are drawn on top as SVG (tools/ui/n64pad.svg, picture
  coordinates) at the positions measured on the picture, in its colours.
Z sits on the back of the middle handle, so it is shown as a tab in the gap to its left.

Output: ctr64/src/ui/n64pad_img.h (palettized run-length image + hotspots for the selection ring
and touch, via png2c.py), n64pad_big.png (3x on the backdrop colour) and n64pad_compare.png (the
reference next to the result, same size) to check against.
Needs Pillow, numpy and rsvg-convert (MSYS2 clang64).   usage: python tools/ui/n64pad.py"""
import json, math, os, subprocess, sys
import numpy as np
from PIL import Image, ImageDraw, ImageFilter

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
RSVG = r'C:\msys64\clang64\bin\rsvg-convert.exe'
REF = os.path.join(HERE, 'ref', 'n64_ref2.png')
X0, Y0, CW, CH = 150, 150, 1200, 1120      # crop of the picture
IMG_H = 176
IMG_W = round(IMG_H * CW / CH)
SS = 4
W4, H4 = IMG_W * SS, IMG_H * SS
FONT = 'font-family="Bahnschrift, Segoe UI, sans-serif" font-weight="bold" text-anchor="middle"'

# measured on the picture
DPAD = (393, 520)
START = (750, 555)
B_, A_ = (970, 548), (1050, 633)
CC = (1140, 482)
C_UP, C_DOWN, C_LEFT, C_RIGHT = (1140, 416), (1140, 549), (1072, 483), (1207, 483)
STICK = (750, 784)
CAP = (750, 780)
Z_ = (478, 905)
HOT = [
    ('L', 330, 290, 60, 'shoulder'),
    ('R', 1170, 290, 60, 'shoulder'),
    ('Z', Z_[0], Z_[1], 70, 'z'),
    ('START', START[0], START[1], 44, 'start'),
    ('B', B_[0], B_[1], 50, 'b'),
    ('A', A_[0], A_[1], 50, 'a'),
    ('C-Up', C_UP[0], C_UP[1], 36, 'c'),
    ('C-Down', C_DOWN[0], C_DOWN[1], 36, 'c'),
    ('C-Left', C_LEFT[0], C_LEFT[1], 36, 'c'),
    ('C-Right', C_RIGHT[0], C_RIGHT[1], 36, 'c'),
    ('Stick', STICK[0], STICK[1], 118, 'stick'),
    ('D-Pad', DPAD[0], DPAD[1], 110, 'dpad'),
]


def silhouette(a):
    """True where the controller is (not the red background or the floor shadow)"""
    r, g = a[..., 0].astype(int), a[..., 1].astype(int)
    return ~((r > 60) & (r - g > 60))


def holes(shape):
    """the parts that are drawn by us: filled in from their surroundings in the shading"""
    m = Image.new('L', (shape[1], shape[0]), 0)
    d = ImageDraw.Draw(m)
    x, y = DPAD
    d.rectangle((x - 38, y - 100, x + 38, y + 100), fill=255)
    d.rectangle((x - 100, y - 38, x + 100, y + 38), fill=255)
    d.ellipse((START[0] - 61, START[1] - 61, START[0] + 61, START[1] + 61), fill=255)
    for cx, cy in (B_, A_):
        d.ellipse((cx - 50, cy - 50, cx + 50, cy + 50), fill=255)
    for cx, cy in (C_UP, C_DOWN, C_LEFT, C_RIGHT):
        d.ellipse((cx - 36, cy - 36, cx + 36, cy + 36), fill=255)
    d.rectangle((CC[0] - 16, CC[1] - 26, CC[0] + 16, CC[1] + 26), fill=255)          # the C
    d.ellipse((STICK[0] - 138, STICK[1] - 134, STICK[0] + 138, STICK[1] + 130), fill=255)
    d.rounded_rectangle((612, 268, 888, 336), 30, fill=255)                       # the logo
    return np.array(m) > 0


def _box(a, r, axis):
    """running mean of width 2r+1 along axis (edges padded with the edge value)"""
    pad = [(0, 0), (0, 0)]
    pad[axis] = (r + 1, r)
    c = np.cumsum(np.pad(a, pad, mode='edge'), axis=axis, dtype=np.float64)
    n = a.shape[axis]
    hi = np.take(c, np.arange(2 * r + 1, 2 * r + 1 + n), axis=axis)
    lo = np.take(c, np.arange(0, n), axis=axis)
    return ((hi - lo) / (2 * r + 1)).astype(np.float32)


def blur(a, r):
    """about a Gaussian of radius r: three box passes each way"""
    b = max(1, int(round(r * 0.58)))
    a = a.astype(np.float32)
    for _ in range(3):
        a = _box(_box(a, b, 0), b, 1)
    return a


def shading():
    img = np.array(Image.open(REF).convert('RGB')).astype(np.float32)
    inside = silhouette(img.astype(np.uint8))
    hole = holes(inside.shape)
    valid = (inside & ~hole).astype(np.uint8) * 255
    # keep the edge band out of the estimate (it is mixed with red)
    w = (np.array(Image.fromarray(valid).filter(ImageFilter.MinFilter(9))) > 0).astype(np.float32)
    out = np.empty_like(img)
    for c in range(3):
        ch = img[..., c] * w
        filled = np.zeros_like(ch)
        done = w > 0
        for r in (6, 14, 30, 60, 120):  # fill from ever wider surroundings
            wb = blur(w, r)
            est = blur(ch, r) / np.maximum(wb, 1e-6)
            take = (~done) & (wb > 1e-3)
            filled[take] = est[take]
            done = done | take
        out[..., c] = blur(np.where(w > 0, img[..., c], filled), 3)  # plastic grain off
    return out, inside


def tri(cx, cy, d, s):
    pts = {'u': [(0, -s), (-s, s * 0.6), (s, s * 0.6)], 'd': [(0, s), (-s, -s * 0.6), (s, -s * 0.6)],
           'l': [(-s, 0), (s * 0.6, -s), (s * 0.6, s)], 'r': [(s, 0), (-s * 0.6, -s), (-s * 0.6, s)]}[d]
    return ' '.join(f'{cx + x:.1f},{cy + y:.1f}' for x, y in pts)


def overlay_svg():
    o = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{W4}" height="{H4}" viewBox="{X0} {Y0} {CW} {CH}">',
         '<defs>'
         '<radialGradient id="cap" cx="0.45" cy="0.4" r="0.7">'
         '<stop offset="0" stop-color="#E6E6EE"/><stop offset="1" stop-color="#B8B8C6"/></radialGradient>'
         '<linearGradient id="house" x1="0" y1="0" x2="0" y2="1">'
         '<stop offset="0" stop-color="#70727B"/><stop offset="1" stop-color="#60626A"/></linearGradient>'
         '<linearGradient id="cross" x1="0" y1="0" x2="0" y2="1">'
         '<stop offset="0" stop-color="#6A6D76"/><stop offset="1" stop-color="#5A5D66"/></linearGradient>'
         '<radialGradient id="red" cx="0.4" cy="0.35" r="0.7"><stop offset="0" stop-color="#EE5A60"/>'
         '<stop offset="1" stop-color="#D23A40"/></radialGradient>'
         '<radialGradient id="grn" cx="0.4" cy="0.35" r="0.7"><stop offset="0" stop-color="#1FA46A"/>'
         '<stop offset="1" stop-color="#0E8650"/></radialGradient>'
         '<radialGradient id="blu" cx="0.4" cy="0.35" r="0.7"><stop offset="0" stop-color="#2858BC"/>'
         '<stop offset="1" stop-color="#163C98"/></radialGradient>'
         '<radialGradient id="yel" cx="0.4" cy="0.35" r="0.7"><stop offset="0" stop-color="#FFD43A"/>'
         '<stop offset="1" stop-color="#EAB50E"/></radialGradient>'
         '<linearGradient id="rec" x1="0" y1="0" x2="0" y2="1">'
         '<stop offset="0" stop-color="#8C8E98"/><stop offset="0.5" stop-color="#B4B6C0"/>'
         '<stop offset="1" stop-color="#D6D8E0"/></linearGradient>'
         '</defs>']
    # D-pad cross: dark rim, face, sunken centre, embossed arrows
    x, y = DPAD
    o.append(f'<rect x="{x - 33}" y="{y - 95}" width="66" height="190" rx="9" fill="#33363E"/>')
    o.append(f'<rect x="{x - 95}" y="{y - 33}" width="190" height="66" rx="9" fill="#33363E"/>')
    o.append(f'<rect x="{x - 28}" y="{y - 90}" width="56" height="180" rx="6" fill="url(#cross)"/>')
    o.append(f'<rect x="{x - 90}" y="{y - 28}" width="180" height="56" rx="6" fill="url(#cross)"/>')
    o.append(f'<circle cx="{x}" cy="{y}" r="16" fill="#5C5F68" stroke="#4E5159" stroke-width="2"/>')
    for d, dx, dy in (('u', 0, -62), ('d', 0, 62), ('l', -62, 0), ('r', 62, 0)):
        o.append(f'<polygon points="{tri(x + dx, y + dy, d, 13)}" fill="#545760"/>')
    # START in its recess (shadowed at the top, lit at the bottom), B, A, C: matte colours, thin
    # dark edge, embossed marks
    x, y = START
    o.append(f'<circle cx="{x}" cy="{y}" r="58" fill="url(#rec)"/>')
    o.append(f'<circle cx="{x}" cy="{y}" r="44" fill="#8E2026"/>')
    o.append(f'<circle cx="{x}" cy="{y}" r="42" fill="url(#red)"/>')
    o.append(f'<text x="{x}" y="{y + 10}" font-size="28" fill="#BC2C32" {FONT}>START</text>')
    for (cx, cy, grad, mark, t) in ((B_[0], B_[1], 'grn', '#0B7646', 'B'), (A_[0], A_[1], 'blu', '#163C92', 'A')):
        o.append(f'<circle cx="{cx}" cy="{cy}" r="46" fill="#14161C"/>')
        o.append(f'<circle cx="{cx}" cy="{cy}" r="43" fill="url(#{grad})"/>')
        o.append(f'<text x="{cx}" y="{cy + 19}" font-size="54" fill="{mark}" {FONT}>{t}</text>')
    for (cx, cy), d in ((C_UP, 'u'), (C_DOWN, 'd'), (C_LEFT, 'l'), (C_RIGHT, 'r')):
        o.append(f'<circle cx="{cx}" cy="{cy}" r="33" fill="#4A3A08"/>')
        o.append(f'<circle cx="{cx}" cy="{cy}" r="30.5" fill="url(#yel)"/>')
        o.append(f'<polygon points="{tri(cx, cy, d, 12)}" fill="#E0AA08"/>')
    o.append(f'<text x="{CC[0]}" y="{CC[1] + 16}" font-size="44" fill="#A9ACB8" {FONT}>C</text>')
    # analog stick: dark grey housing (lighter rim, darker towards the bottom), octagonal gate,
    # light ridged cap on its shaft
    x, y = STICK
    o.append(f'<circle cx="{x}" cy="{y}" r="126" fill="#A8AAB4"/>')
    o.append(f'<circle cx="{x}" cy="{y + 1}" r="119" fill="url(#house)"/>')
    oct_ = ' '.join(f'{x + 76 * math.cos(-math.pi / 2 + k * math.pi / 4):.1f},'
                    f'{y - 8 + 76 * math.sin(-math.pi / 2 + k * math.pi / 4):.1f}' for k in range(8))
    o.append(f'<polygon points="{oct_}" fill="#7A7C85" stroke="#5C5E66" stroke-width="3" stroke-linejoin="round"/>')
    x, y = CAP
    o.append(f'<ellipse cx="{x}" cy="{y + 4}" rx="56" ry="50" fill="#A6A8B6"/>')
    o.append(f'<circle cx="{x}" cy="{y}" r="47" fill="url(#cap)"/>')
    for r in (36, 25, 14):
        o.append(f'<circle cx="{x}" cy="{y}" r="{r}" fill="none" stroke="#ADADBD" stroke-width="3.5"/>')
    o.append(f'<circle cx="{x}" cy="{y}" r="5" fill="#ADADBD"/>')
    # Z (behind the middle handle)
    x, y = Z_
    o.append(f'<rect x="{x - 44}" y="{y - 60}" width="88" height="120" rx="18" fill="#A0A2AC" stroke="#D8DAE2" stroke-width="5"/>')
    o.append(f'<text x="{x}" y="{y + 22}" font-size="66" fill="#F2F2F6" {FONT}>Z</text>')
    o.append('</svg>')
    return '\n'.join(o)


def main():
    shade, inside = shading()
    rgb = Image.fromarray(np.clip(shade, 0, 255).astype(np.uint8)).crop((X0, Y0, X0 + CW, Y0 + CH))
    alpha = Image.fromarray((inside * 255).astype(np.uint8)).crop((X0, Y0, X0 + CW, Y0 + CH))
    alpha = alpha.filter(ImageFilter.GaussianBlur(3.0)).point(lambda v: 255 if v >= 128 else 0)
    body = rgb.resize((W4, H4), Image.LANCZOS).convert('RGBA')
    body.putalpha(alpha.resize((W4, H4), Image.LANCZOS))
    out = Image.new('RGBA', (W4, H4), (0, 0, 0, 0))
    sh = Image.new('L', (W4, H4), 0)
    sh.paste(body.getchannel('A'), (0, 3 * SS))
    shadow = Image.new('RGBA', (W4, H4), (0, 0, 0, 0))
    shadow.putalpha(sh.filter(ImageFilter.GaussianBlur(3 * SS)).point(lambda v: v * 40 // 100))
    out.alpha_composite(shadow)
    out.alpha_composite(body)
    svg = os.path.join(HERE, 'n64pad.svg')
    with open(svg, 'w') as f:
        f.write(overlay_svg())
    over = os.path.join(HERE, 'n64pad_over.png')
    subprocess.run([RSVG, '-w', str(W4), '-h', str(H4), '-o', over, svg], check=True)
    out.alpha_composite(Image.open(over).convert('RGBA'))
    os.remove(over)
    final = out.resize((IMG_W, IMG_H), Image.LANCZOS)
    png = os.path.join(HERE, 'n64pad.png')
    final.save(png)
    big = Image.new('RGBA', (IMG_W * 3, IMG_H * 3), (0x45, 0x45, 0x4B, 255))
    big.alpha_composite(out.resize((IMG_W * 3, IMG_H * 3), Image.LANCZOS))
    big.save(os.path.join(HERE, 'n64pad_big.png'))
    ref = Image.open(REF).convert('RGB').crop((X0, Y0, X0 + CW, Y0 + CH)).resize((IMG_W * 3, IMG_H * 3), Image.LANCZOS)
    cmp_ = Image.new('RGB', (IMG_W * 6 + 10, IMG_H * 3), (255, 255, 255))
    cmp_.paste(ref, (0, 0))
    cmp_.paste(big.convert('RGB'), (IMG_W * 3 + 10, 0))
    cmp_.save(os.path.join(HERE, 'n64pad_compare.png'))
    # close-up of the middle (the stick and START), reference above, result below
    box = (560, 470, 940, 930)
    z = 1.2
    ref2 = Image.open(REF).convert('RGB').crop(box)
    ours = out.crop(tuple(round((v - o_) * W4 / CW) for v, o_ in zip(box, (X0, Y0, X0, Y0)))).resize(ref2.size, Image.LANCZOS)
    bg = Image.new('RGBA', ref2.size, (0x45, 0x45, 0x4B, 255))
    bg.alpha_composite(ours)
    zoom = Image.new('RGB', (ref2.width * 2 + 10, ref2.height), (255, 255, 255))
    zoom.paste(ref2, (0, 0))
    zoom.paste(bg.convert('RGB'), (ref2.width + 10, 0))
    zoom.save(os.path.join(HERE, 'n64pad_zoom.png'))
    s = IMG_W / CW
    hot = [{'name': n, 'x': round((x - X0) * s), 'y': round((y - Y0) * s), 'r': round(r * s), 'kind': k}
           for n, x, y, r, k in HOT]
    out_h = os.path.join(ROOT, 'ctr64', 'src', 'ui', 'n64pad_img.h')
    subprocess.run([sys.executable, os.path.join(HERE, 'png2c.py'), png, 'k_n64pad', out_h, json.dumps(hot)], check=True)
    print('wrote', out_h, IMG_W, 'x', IMG_H)


if __name__ == '__main__':
    main()
