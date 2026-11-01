#!/usr/bin/env python3
# steam_art.py za|pla: Steam library images for the game added as a non-Steam game, drawn like the app icon
# (make_icon.py: the launcher banner's art) at Steam's sizes, the title as the launcher's banner writes it (Baloo 2):
#   docs/icon/steam/capsule.png  600x900   library grid       (Steam: grid/<appid>p.png)
#   docs/icon/steam/wide.png     920x430   recent games strip (grid/<appid>.png)
#   docs/icon/steam/hero.png     1920x620  game page banner   (grid/<appid>_hero.png, no title: the logo goes over it)
#   docs/icon/steam/logo.png     1000x360  transparent title  (grid/<appid>_logo.png)
# Needs rsvg-convert. Run from the repository root.
import os, subprocess, sys, tempfile

import make_icon as mi

HERE = os.path.dirname(os.path.abspath(__file__))
FONT_DIR = os.path.join(HERE, '..', '..', 'third_party', 'fonts', 'Baloo2')
GAMES = {  # title, the launcher's sub-line colour, and the line under it
    'za': ('PL:Z-A', 'rgb(200,236,72)', 'Pokémon Legends: Z-A, recompiled for PC'),
    'pla': ('PL:A', 'rgb(240,196,92)', 'Pokémon Legends: Arceus, recompiled for PC'),
}


def title(svg, game, x, y, size, anchor='start', sub=True):
    # the launcher banner's lettering: the title, "R  E  C  O  M  P" in the accent colour, the line under it; y = the
    # title's baseline, size = its font size
    name, accent, line = GAMES[game]
    f = 'font-family="\'Baloo 2\'" font-weight="600"'  # (quoted: an unquoted "Baloo 2" isn't valid CSS)
    sh = 'stroke="rgb(6,8,18)" stroke-opacity="0.55" stroke-width="%.1f" paint-order="stroke"' % (size * 0.06)
    svg.add('<text x="%.1f" y="%.1f" %s font-size="%.1f" fill="rgb(245,248,255)" text-anchor="%s" %s>%s</text>'
            % (x, y, f, size, anchor, sh, name))
    svg.add('<text x="%.1f" y="%.1f" %s font-size="%.1f" fill="%s" text-anchor="%s" letter-spacing="%.1f" %s>RECOMP</text>'
            % (x + (size * 0.05 if anchor == 'start' else 0), y + size * 0.42, f, size * 0.30, accent, anchor, size * 0.14, sh))
    if sub:
        svg.add('<text x="%.1f" y="%.1f" %s font-size="%.1f" fill="rgb(170,180,205)" text-anchor="%s" %s>%s</text>'
                % (x + (size * 0.05 if anchor == 'start' else 0), y + size * 0.74, f, size * 0.16, anchor, sh, line))


def scene(svg, game, **at):
    (mi.za if game == 'za' else mi.pla)(svg, rx=0, **at)


def render(name, w, h, draw):
    mi.W, mi.HH = w, h
    mi.LW = max(1.2, 2.0 * h / mi.S)
    svg = mi.Svg()
    draw(svg)
    body = '\n'.join(svg.out)
    text = ('<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" viewBox="0 0 %d %d">\n%s\n</svg>\n' % (w, h, w, h, body))
    out = os.path.join(HERE, 'steam')
    os.makedirs(out, exist_ok=True)
    with tempfile.TemporaryDirectory() as tmp:
        src = os.path.join(tmp, name + '.svg')
        open(src, 'w').write(text)
        conf = os.path.join(tmp, 'fonts.conf')  # (Baloo 2 from third_party/, without installing it)
        open(conf, 'w').write('<?xml version="1.0"?><fontconfig><include ignore_missing="yes">/etc/fonts/fonts.conf</include>'
                              '<dir>%s</dir><cachedir>%s</cachedir></fontconfig>' % (os.path.abspath(FONT_DIR), tmp))
        env = dict(os.environ, FONTCONFIG_FILE=conf)
        subprocess.run(['rsvg-convert', '-w', str(w), '-h', str(h), '-o', os.path.join(out, name + '.png'), src], check=True, env=env)


def main():
    game = sys.argv[1] if len(sys.argv) > 1 else 'za'
    za = game == 'za'

    def capsule(svg):  # the tower / mountain under the title
        if za:
            scene(svg, game, cx=300, street=850, H=560)
        else:
            scene(svg, game, u=760)
        svg.add('</g>')
        title(svg, game, 300, 150 if za else 790, 118, 'middle', sub=False)

    def wide(svg):  # the title on the left, the tower / mountain on the right
        if za:
            scene(svg, game, cx=700, street=404, H=316)
        else:
            scene(svg, game, cx=690, u=430)
        svg.add('</g>')
        title(svg, game, 44, 190, 120)

    def hero(svg):
        if za:
            scene(svg, game, cx=1400, street=596, H=480)
        else:
            scene(svg, game, cx=1380, u=640)
        svg.add('</g>')

    def logo(svg):
        title(svg, game, 30, 230, 240, sub=False)

    render('capsule', 600, 900, capsule)
    render('wide', 920, 430, wide)
    render('hero', 1920, 620, hero)
    render('logo', 1000, 360, logo)
    print('steam/capsule.png, wide.png, hero.png, logo.png written (%s)' % game)


if __name__ == '__main__':
    main()
