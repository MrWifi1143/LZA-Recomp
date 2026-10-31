#!/usr/bin/env python3
# make_icon.py za|pla: the app icon (docs/icon/icon.svg, then icon_1024.png, icon.ico, icon.icns) drawn from the launcher's
# banner art (runtime/ui/launcher_lg.mm): Z-A's Prism Tower over the night city, Arceus's Mount Coronet under the
# space-time rift - the same shapes and colours as the launcher draws them, framed as a rounded square, lines thicker so
# they hold at small sizes. Needs rsvg-convert and ImageMagick's magick for the PNG / ICO. Run from the repository root.
import io, math, os, struct, subprocess, sys

S = 1024  # (the icon's size)
LW = 2.0  # (the launcher's line widths times this)


def rgba(r, g, b, a=255):
    return 'rgb(%d,%d,%d)' % (r, g, b), round(a / 255, 3)


class Svg:
    def __init__(self):
        self.out = []

    def add(self, s):
        self.out.append(s)

    def poly(self, pts, fill=None, stroke=None, width=1.0, closed=False):
        d = 'M' + ' L'.join('%.1f %.1f' % p for p in pts) + (' Z' if closed else '')
        a = ['d="%s"' % d]
        if fill:
            c, o = fill
            a.append('fill="%s" fill-opacity="%s"' % (c, o))
        else:
            a.append('fill="none"')
        if stroke:
            c, o = stroke
            a.append('stroke="%s" stroke-opacity="%s" stroke-width="%.2f" stroke-linejoin="round" stroke-linecap="round"' % (c, o, width * LW))
        self.add('<path %s/>' % ' '.join(a))

    def line(self, p, q, stroke, width):
        self.poly([p, q], stroke=stroke, width=width)

    def circle(self, c, r, fill):
        col, o = fill
        self.add('<circle cx="%.1f" cy="%.1f" r="%.1f" fill="%s" fill-opacity="%s"/>' % (c[0], c[1], r, col, o))

    def ellipse(self, c, rx, ry, fill):
        col, o = fill
        self.add('<ellipse cx="%.1f" cy="%.1f" rx="%.1f" ry="%.1f" fill="%s" fill-opacity="%s"/>' % (c[0], c[1], rx, ry, col, o))


def rnd(s):  # (launcher_lg.mm rnd: the same sequence)
    s[0] = (s[0] * 1664525 + 1013904223) & 0xffffffff
    return s[0] >> 8


def frame(svg, top, bottom, dots):
    # the rounded square: the banner's sky (top colour -> bottom colour) and its faint dot grid
    svg.add('<defs><linearGradient id="sky" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="%s"/>'
            '<stop offset="1" stop-color="%s"/></linearGradient>'
            '<clipPath id="tile"><rect x="0" y="0" width="%d" height="%d" rx="200"/></clipPath></defs>' % (top, bottom, S, S))
    svg.add('<g clip-path="url(#tile)">')
    svg.add('<rect x="0" y="0" width="%d" height="%d" fill="url(#sky)"/>' % (S, S))
    for y in range(24, S, 48):
        for x in range(24, S, 48):
            svg.circle((x, y), 2.6, rgba(*dots, int(34 * (1 - y / S) + 6)))


# ------------------------------------------------------------------ Z-A: Prism Tower (launcher_lg.mm prism_tower)
def prism_tower(svg, cx, ground, H):
    fill = rgba(12, 18, 30)
    c, dim = rgba(160, 220, 90, 235), rgba(120, 180, 70, 130)
    Y = lambda f: ground - f * H

    def legw(f):
        u = (f - 0.075) / 0.165
        return H * (0.075 + 0.105 * (1 - u) * (1 - u))

    def shw(f):
        u = f - 0.52
        return H * (0.036 + (0.40 if u > 0 else 0.52) * u * u)

    cup0 = shw(0.80)

    def part(f0, f1, n, hw, thick):
        L = [(cx - hw(f0 + (f1 - f0) * i / n), Y(f0 + (f1 - f0) * i / n)) for i in range(n + 1)]
        R = [(cx + hw(f0 + (f1 - f0) * i / n), Y(f0 + (f1 - f0) * i / n)) for i in range(n + 1)]
        svg.poly(L + R[::-1], fill=fill, closed=True)
        svg.poly(L, stroke=c, width=thick)
        svg.poly(R, stroke=c, width=thick)

    # the base: a stepped pedestal of two tiers with sloping sides (as the tower's own), each with a window band; the
    # upper tier's top is as wide as the legs' foot, so their curves come down onto its corners
    for f0, f1, w0, w1 in ((0.0, 0.035, 0.235, 0.215), (0.035, 0.075, 0.205, 0.180)):
        a0, a1 = w0 * H, w1 * H
        svg.poly([(cx - a0, Y(f0)), (cx - a1, Y(f1)), (cx + a1, Y(f1)), (cx + a0, Y(f0))], fill=fill, stroke=c, width=2.0, closed=True)
        fb = (f0 + f1) / 2
        hb = ((w0 + w1) / 2 - 0.028) * H  # (the band, inside the sloping sides)
        x = cx - hb
        while x <= cx + hb + 0.5:
            svg.line((x, Y(fb) - 6), (x, Y(fb) + 6), dim, 1.2)
            x += 2 * hb / max(1, round(2 * hb / 18))
    door = [(cx + 0.045 * H * math.cos(math.pi + math.pi * i / 24), Y(0.0) + 0.045 * H * math.sin(math.pi + math.pi * i / 24))
            for i in range(25)]
    svg.poly(door, fill=fill, stroke=c, width=2.0, closed=True)
    part(0.075, 0.240, 40, legw, 2.5)  # the legs, beams crossed between them
    for i in range(5):
        f0 = 0.10 + 0.028 * i
        f1 = f0 + 0.028
        l0, r0, l1, r1 = cx - legw(f0), cx + legw(f0), cx - legw(f1), cx + legw(f1)
        svg.line((l0, Y(f0)), (r1, Y(f1)), dim, 1.2)
        svg.line((r0, Y(f0)), (l1, Y(f1)), dim, 1.2)
        svg.line((l1, Y(f1)), (r1, Y(f1)), c, 1.3)
    arch = 0.045 * H
    A = [(cx - arch, Y(0.075))] + [(cx + arch * math.cos(math.pi + math.pi * i / 20), Y(0.120) + arch * math.sin(math.pi + math.pi * i / 20))
                                    for i in range(21)] + [(cx + arch, Y(0.075))]
    svg.poly(A, fill=rgba(20, 34, 40), stroke=c, width=2.0)
    svg.poly([(cx - 0.095 * H, Y(0.258)), (cx + 0.095 * H, Y(0.258)), (cx + 0.095 * H, Y(0.240)), (cx - 0.095 * H, Y(0.240))],
             fill=fill, stroke=c, width=2.0, closed=True)  # the deck
    # the crown: the back fins first, then the shaft and front fins / middle column as one outline
    o, fin = cup0, cup0 + 0.028 * H
    back = []
    for k in (-1, 1):
        q = [(cx + k * 0.020 * H, Y(0.84)), (cx + k * 0.074 * H, Y(0.84)), (cx + k * 0.074 * H, Y(0.980)), (cx + k * 0.034 * H, Y(0.962))]
        svg.poly(q, fill=fill, stroke=rgba(150, 210, 85, 190), width=1.8, closed=True)
        svg.line((cx + k * 0.046 * H, Y(0.86)), (cx + k * 0.048 * H, Y(0.962)), dim, 1.0)
        back.append((cx + k * 0.074 * H, Y(0.980)))
    P = [(cx - shw(0.258 + 0.542 * i / 80), Y(0.258 + 0.542 * i / 80)) for i in range(80)]

    def bez(p0, p1, p2, k, first):
        for i in range(0 if first else 1, k + 1):
            u = i / k
            v = 1 - u
            P.append((v * v * p0[0] + 2 * v * u * p1[0] + u * u * p2[0], v * v * p0[1] + 2 * v * u * p1[1] + u * u * p2[1]))

    bez((cx - o, Y(0.80)), (cx - o - 0.017 * H, Y(0.875)), (cx - fin, Y(0.970)), 24, True)
    P.append((cx - o + 0.004 * H, Y(0.958)))
    fi, col = o - 0.006 * H, 0.052 * H
    P.append((cx - fi, Y(0.910)))
    bez((cx - fi, Y(0.910)), (cx - fi, Y(0.896)), (cx - (fi + col) / 2, Y(0.896)), 5, False)
    bez((cx - (fi + col) / 2, Y(0.896)), (cx - col, Y(0.896)), (cx - col, Y(0.910)), 5, False)
    P += [(cx - col, Y(0.950)), (cx - 0.032 * H, Y(0.958)), (cx - 0.032 * H, Y(0.978)), (cx - 0.016 * H, Y(0.985)),
          (cx - 0.016 * H, Y(0.996)), (cx - 0.005 * H, Y(1.000)), (cx - 0.004 * H, Y(1.004))]
    P += [(2 * cx - x, y) for x, y in P[::-1]]
    svg.poly(P, fill=fill, closed=True)
    svg.poly(P, stroke=c, width=2.5)
    for k in (-1, 1):
        svg.line((cx + k * (o + 0.004 * H), Y(0.83)), (cx + k * (fin - 0.014 * H), Y(0.962)), dim, 1.2)
    tips = [(cx - fin, Y(0.970)), (cx + fin, Y(0.970))]
    f = 0.29
    while f < 0.80:  # rings
        svg.line((cx - shw(f), Y(f)), (cx + shw(f), Y(f)), dim, 1.0)
        f += 0.06
    svg.line((cx, Y(1.004)), (cx, Y(1.05)), c, 2.0)  # antenna
    svg.line((cx - 0.007 * H, Y(1.016)), (cx + 0.007 * H, Y(1.016)), c, 1.3)
    svg.line((cx - 0.004 * H, Y(1.030)), (cx + 0.004 * H, Y(1.030)), c, 1.3)
    w0, w1 = 0.30, 0.78  # the long window up the shaft and its X lattice

    def whw(f):
        u = (f - w0) / (w1 - w0)
        return shw(f) * 0.70 * math.sqrt(max(0.0, math.sin(u * math.pi)))

    L = [(cx - whw(w0 + (w1 - w0) * i / 60), Y(w0 + (w1 - w0) * i / 60)) for i in range(61)]
    R = [(cx + whw(w0 + (w1 - w0) * i / 60), Y(w0 + (w1 - w0) * i / 60)) for i in range(61)]
    svg.poly(L, stroke=c, width=1.8)
    svg.poly(R, stroke=c, width=1.8)
    for i in range(9):
        f0 = w0 + (w1 - w0) * (i + 0.5) / 9.5
        f1 = w0 + (w1 - w0) * (i + 1.5) / 9.5
        svg.line((cx - whw(f0), Y(f0)), (cx + whw(f1), Y(f1)), dim, 1.2)
        svg.line((cx + whw(f0), Y(f0)), (cx - whw(f1), Y(f1)), dim, 1.2)
        svg.line((cx - whw(f1), Y(f1)), (cx + whw(f1), Y(f1)), c, 1.3)
    for p in tips:  # red lights on the fin tips (lit), the back fins' (dimmer), the purple beacon
        svg.circle(p, 14, rgba(255, 60, 60, 70))
        svg.circle(p, 6, rgba(255, 90, 80, 255))
    for p in back:
        svg.circle(p, 12, rgba(255, 60, 60, 55))
        svg.circle(p, 5, rgba(255, 90, 80, 204))
    top = (cx, Y(1.05))
    svg.circle(top, 26, rgba(180, 110, 255, 90))
    svg.circle(top, 15, rgba(190, 120, 255, 240))
    svg.circle(top, 5, rgba(246, 228, 255, 255))


def za(svg):
    frame(svg, '#0c1028', '#22143a', (150, 190, 255))
    # the tower stands on the street (an icon has no status strip or front buildings over its foot, as the launcher's
    # banner does): the whole base and door above it, the beacon below the top edge
    street = 930
    cx, ground, H = 512, street, 836
    svg.add('<defs><radialGradient id="glow"><stop offset="0" stop-color="rgb(150,230,90)" stop-opacity="0.32"/>'
            '<stop offset="1" stop-color="rgb(150,230,90)" stop-opacity="0"/></radialGradient>'
            '<linearGradient id="street" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="rgb(14,16,30)"/>'
            '<stop offset="1" stop-color="rgb(8,9,20)"/></linearGradient></defs>')
    svg.add('<circle cx="%d" cy="%d" r="450" fill="url(#glow)"/>' % (cx, int(ground - 0.55 * H)))
    s = [7]
    for layer in range(2):  # the city: a back row, the street and the tower, a low front row beside its foot (lit windows)
        if layer == 1:
            svg.poly([(0, street), (S, street), (S, S), (0, S)], fill=rgba(10, 12, 24), closed=True)
            svg.add('<rect x="0" y="%d" width="%d" height="%d" fill="url(#street)"/>' % (street, S, S - street))
            svg.line((0, street), (S, street), rgba(120, 180, 70, 90), 1.2)
            prism_tower(svg, cx, ground, H)
        x = -20.0
        while x < S:
            bw = 52.0 + rnd(s) % 96
            bh = (0.05 + rnd(s) % 100 * 0.0010) * S if layer else (0.11 + rnd(s) % 100 * 0.0018) * S
            if layer == 1 and abs(x + bw / 2 - cx) < S * 0.30:
                x += bw
                continue
            p, q = (x, street - bh), (x + bw - 6, street)
            svg.poly([p, (q[0], p[1]), q, (p[0], q[1])], fill=rgba(8, 9, 20) if layer else rgba(20, 22, 44), closed=True)
            wy = p[1] + 16
            while wy < street - 12:
                wx = p[0] + 10
                while wx < q[0] - 12:
                    r = rnd(s)
                    if r % 4 == 0:
                        svg.poly([(wx, wy), (wx + 8, wy), (wx + 8, wy + 10), (wx, wy + 10)], fill=rgba(255, 214, 120, 60 + r % 150), closed=True)
                    wx += 18
                wy += 22
            x += bw


# ------------------------------------------------------------------ Arceus: Mount Coronet under the rift (launcher_lg.mm)
def rift(svg, cx, cy, rx):
    ry = rx * 0.30
    s = [21]
    for i in range(18, 0, -1):
        svg.ellipse((cx, cy), rx * (0.95 + 0.03 * i), ry * (0.95 + 0.07 * i), rgba(170, 175, 215, 5))
    rim = []
    for i in range(120):
        th = i * 2 * math.pi / 120
        r = 1 + 0.05 * math.sin(7 * th + 0.4) + 0.035 * math.sin(17 * th + 1.3) + 0.02 * math.sin(29 * th)
        if rnd(s) % 7 == 0:
            r -= 0.06 + 0.06 * (rnd(s) % 100) / 100
        rim.append((cx + math.cos(th) * rx * r, cy + math.sin(th) * ry * r))
    svg.poly(rim, fill=rgba(44, 46, 56, 240), stroke=rgba(150, 155, 175, 110), width=1.2, closed=True)
    for i in range(6):
        svg.ellipse((cx, cy), rx * (0.85 - 0.12 * i), ry * (0.85 - 0.12 * i), rgba(20, 20, 28, 40))
    for k in range(28):  # grey wisps
        a0 = k * 2.39996
        r0 = 0.22 + 0.70 * ((k * 53) % 100) / 100
        m = 6 + (k * 7) % 8
        pts = []
        for i in range(m):
            th = a0 + i * 0.06
            rr = r0 * (1 - 0.12 * i / 13.0) + 0.010 * math.sin(i * 0.6 + k)
            pts.append((cx + math.cos(th) * rx * rr, cy + math.sin(th) * ry * rr))
        svg.poly(pts, stroke=rgba(160, 164, 180, int(40 + 70 * (1 - r0))), width=1.1 + 0.6 * (k % 2))
    for k in range(34):  # shards breaking off the rim
        th = (rnd(s) % 1000) / 1000 * 2 * math.pi
        out = 1.02 + 0.16 * (rnd(s) % 100) / 100 + 0.10 * abs(math.cos(th))
        p = (cx + math.cos(th) * rx * out, cy + math.sin(th) * ry * out)
        sz = (2.5 + 6.0 * (rnd(s) % 100) / 100) * 2.2
        an = (rnd(s) % 628) / 100
        svg.poly([(p[0] + math.cos(an) * sz, p[1] + math.sin(an) * sz * 0.6),
                  (p[0] + math.cos(an + 2.3) * sz * 0.7, p[1] + math.sin(an + 2.3) * sz * 0.5),
                  (p[0] + math.cos(an + 4.1) * sz * 0.8, p[1] + math.sin(an + 4.1) * sz * 0.5)], fill=rgba(78, 82, 98, 235), closed=True)
    for i in range(10, 0, -1):  # the starburst
        svg.ellipse((cx, cy), rx * 0.022 * i, ry * 0.05 * i, rgba(220, 230, 255, 9))
    for i in range(12):
        an = i * 0.5235988 + 0.15 * math.sin(i)
        l = rx * (0.22 if i % 3 == 0 else 0.11) * 1.15 * (0.6 + 0.4 * abs(math.cos(an)))
        svg.line((cx, cy), (cx + math.cos(an) * l, cy + math.sin(an) * l * 0.55), rgba(255, 255, 255, 210), 1.6 if i % 3 == 0 else 1.0)
    svg.circle((cx, cy), 9, rgba(255, 255, 255))


def pla(svg):
    frame(svg, '#16143a', '#6e3c3a', (255, 226, 170))
    fill, c, dim, snow = rgba(26, 20, 40), rgba(240, 196, 92, 235), rgba(200, 160, 80, 110), rgba(236, 228, 255, 210)
    w = h = S
    cx, peak, foot = w * 0.5, h * 0.40, h * 0.97
    rift(svg, cx, h * 0.205, w * 0.40)
    sw, spread = w * 0.075, w * 0.80
    jag = [0.0, -0.12, 0.06, -0.22, -0.08, -0.18, 0.04, -0.10, 0.0]
    rim_y = lambda i: peak + jag[i] * sw * 0.5

    def hw(y):
        u = (y - peak) / (foot - peak)
        return sw + spread * (max(u, 0) ** 1.25)

    P = [(cx - hw(foot - (foot - peak) * i / 50), foot - (foot - peak) * i / 50) for i in range(51)]
    P += [(cx - sw + 2 * sw * i / 8.0, rim_y(i)) for i in range(1, 8)]
    P += [(cx + hw(foot - (foot - peak) * i / 50), foot - (foot - peak) * i / 50) for i in range(50, -1, -1)]
    svg.poly(P, fill=fill, closed=True)
    s = [5]
    tpos, td, tw = [], [], []
    for k in range(9):
        tpos.append((k + 0.5) / 9 + 0.03 * ((rnd(s) % 100) / 50 - 1))
        td.append(0.04 + 0.16 * (rnd(s) % 100) / 100)
        tw.append(0.025 + 0.03 * (rnd(s) % 100) / 100)
    ns, yl = 72, peak + (foot - peak) * 0.28
    Sn = []
    for i in range(ns):
        f = i / (ns - 1)
        dep = 0.012 * math.sin(f * 23.0)
        for k in range(9):
            q = 1 - abs(f - tpos[k]) / tw[k]
            if q > 0:
                dep = max(dep, td[k] * q * q)
        if i in (0, ns - 1):
            dep = 0
        Sn.append((cx + (2 * f - 1) * hw(yl), yl + (foot - peak) * dep))

    def top(x):
        dx = abs(x - cx)
        if dx > sw:
            return peak + (foot - peak) * ((dx - sw) / spread) ** (1 / 1.25)
        f = (x - (cx - sw)) / (2 * sw) * 8
        i = 0 if f < 0 else 7 if f >= 8 else int(f)
        return rim_y(i) + (rim_y(i + 1) - rim_y(i)) * (f - i)

    cap = [(x, top(x)) for x, _ in Sn] + Sn[::-1]
    svg.poly(cap, fill=rgba(150, 140, 205, 90), closed=True)
    svg.poly(Sn, stroke=snow, width=1.5)
    for k in range(9):  # streaks below the longer tongues
        if td[k] < 0.11:
            continue
        f, y = tpos[k], yl + (foot - peak) * td[k]
        x, ln = cx + (2 * f - 1) * hw(yl), (foot - peak) * 0.08
        svg.line((x, y + 8), (x + (2 * f - 1) * ln * 0.35, y + ln), rgba(236, 228, 255, 110), 1.2)
    for i in (-3, -2, -1, 1, 2, 3):  # the ridges
        f = i / 3.5
        R = []
        for j in range(12):
            y = peak + (foot - peak) * (0.03 + 0.80 * j / 11.0)
            R.append((cx + f * hw(y) * (0.92 - 0.10 * abs(f)), y))
        svg.poly(R, stroke=dim, width=1.2)
    svg.poly(P, stroke=c, width=2.5)
    for layer in range(2):  # the foothills in front
        base, amp = h - h * (0.16 - 0.07 * layer), h * (0.11 - 0.03 * layer)
        pts = []
        for i in range(31):
            x = -10 + (w + 20) * i / 30
            pts.append((x, base - amp * (0.35 + 0.65 * (rnd(s) % 100) / 100 * (0.6 + 0.4 * math.sin(i * 0.7 + layer)))))
        svg.poly(pts + [(w + 10, h + 10), (-10, h + 10)], fill=rgba(34, 26, 46) if layer == 0 else rgba(20, 16, 30), closed=True)
        svg.poly(pts, stroke=dim if layer else rgba(220, 176, 86, 160), width=1.5)


def main():
    game = sys.argv[1] if len(sys.argv) > 1 else 'za'
    svg = Svg()
    (za if game == 'za' else pla)(svg)
    svg.add('</g>')
    title = "Pokémon Legends: Z-A Recomp - Prism Tower" if game == 'za' else 'Pokémon Legends: Arceus Recomp - Mount Coronet'
    body = '\n'.join(svg.out)
    text = ('<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" viewBox="0 0 %d %d">\n'
            '<!-- %s (docs/icon/make_icon.py %s: the launcher banner\'s art, runtime/ui/launcher_lg.mm) -->\n%s\n</svg>\n'
            % (S, S, S, S, title, game, body))
    here = os.path.dirname(os.path.abspath(__file__))
    open(os.path.join(here, 'icon.svg'), 'w').write(text)
    png = os.path.join(here, 'icon_1024.png')
    subprocess.run(['rsvg-convert', '-w', '1024', '-h', '1024', '-o', png, os.path.join(here, 'icon.svg')], check=True)
    sizes = [16, 24, 32, 48, 64, 128, 256]
    subprocess.run(['magick', png, '-define', 'icon:auto-resize=' + ','.join(map(str, sizes)), os.path.join(here, 'icon.ico')], check=True)
    # icon.icns: PNG entries (ic07 128, ic08 256, ic09 512, ic10 1024)
    chunks = b''
    for kind, n in ((b'ic07', 128), (b'ic08', 256), (b'ic09', 512), (b'ic10', 1024)):
        data = subprocess.run(['magick', png, '-resize', '%dx%d' % (n, n), 'png:-'], check=True, capture_output=True).stdout
        chunks += kind + struct.pack('>I', len(data) + 8) + data
    open(os.path.join(here, 'icon.icns'), 'wb').write(b'icns' + struct.pack('>I', len(chunks) + 8) + chunks)
    print('icon.svg, icon_1024.png, icon.ico, icon.icns written (%s)' % game)


if __name__ == '__main__':
    main()
