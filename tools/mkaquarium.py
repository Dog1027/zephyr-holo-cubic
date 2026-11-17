#!/usr/bin/env python3
"""Generate the Aquarium app's creatures (src/apps/fg/aquarium_sprites.c / .h).

Usage: uv run python tools/mkaquarium.py [--preview sprites.png]

Every creature is drawn here with a few shapes (supersampled, so the edges are smooth) as an
alpha mask: LVGL draws an A8 image in the object's image_recolor color, so one sprite takes
any color. Each one has AQUA_FRAMES frames of a loop; microbes also get mirrored frames for
swimming left. To change a creature edit its function, to add one write a function and add
it to CREATURES (the app has a matching row in its own table), then run this again.
"""
import argparse
import math
import os

from PIL import Image, ImageDraw

TOP = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

S=8                      # supersampling
FILL=95; LINE=255; SOFT=170
class Cv:
    def __init__(s,w,h): s.w,s.h=w,h; s.im=Image.new("L",(w*S,h*S),0); s.d=ImageDraw.Draw(s.im)
    def P(s,pts): return [(x*S,y*S) for x,y in pts]
    def ell(s,cx,cy,rx,ry,fill=None,outline=None,width=1.6):
        s.d.ellipse([(cx-rx)*S,(cy-ry)*S,(cx+rx)*S,(cy+ry)*S],fill=fill,outline=outline,width=int(width*S))
    def poly(s,pts,fill=None,outline=None,width=1.6):
        if fill is not None: s.d.polygon(s.P(pts),fill=fill)
        if outline is not None: s.line(pts+[pts[0]],outline,width)
    def line(s,pts,v=LINE,width=1.6):
        s.d.line(s.P(pts),fill=v,width=int(width*S),joint="curve")
        r=width/2
        for x,y in (pts[0],pts[-1]): s.d.ellipse([(x-r)*S,(y-r)*S,(x+r)*S,(y+r)*S],fill=v)
    def dot(s,x,y,r=1.0,v=LINE): s.d.ellipse([(x-r)*S,(y-r)*S,(x+r)*S,(y+r)*S],fill=v)
    def out(s):
        im=s.im.resize((s.w,s.h),Image.LANCZOS)
        return im
def wave(x0,y0,length,amp,ph,n=14,freq=2.2,grow=True):
    return [(x0+amp*(i/n if grow else 1)*math.sin(ph+freq*math.pi*i/n), y0+length*i/n) for i in range(n+1)]
def dome(cx,base,rx,ry,n=24): return [(cx+rx*math.cos(math.pi-math.pi*i/n), base-ry*math.sin(math.pi*i/n)) for i in range(n+1)]

def jelly(w,h,rx,ry,tent,tlen,arms,ph,rim=5,gon=True,tw=1.3):
    c=Cv(w,h); cx=w/2; p=math.sin(ph); rx=rx*(1+0.07*p); ry=ry*(1-0.09*p); base=ry+2.5
    for i in range(tent):
        x=cx-rx*0.82+2*rx*0.82*i/(tent-1)
        c.line(wave(x,base+1,tlen*(0.8+0.2*((i*7)%3)/2),2.2,ph+i*0.9),SOFT,tw)
    for i in range(arms):
        x=cx-rx*0.3+0.6*rx*i/max(arms-1,1)
        c.line(wave(x,base+1,tlen*0.55,1.6,ph*1.0+i*1.7,freq=1.4),LINE,2.4)
    d=dome(cx,base,rx,ry)
    scal=[(cx-rx+2*rx*(i+0.5)/rim, base+1.6) for i in range(rim)]
    edge=[]
    for i in range(rim):
        edge+= [(cx+rx-2*rx*i/rim,base),(cx+rx-2*rx*(i+0.5)/rim,base+1.8)]
    edge.append((cx-rx,base))
    c.poly(d+edge,fill=FILL,outline=LINE,width=1.7)
    if gon:
        for dx in (-0.38,0.38): c.ell(cx+dx*rx,base-ry*0.42,rx*0.2,ry*0.2,outline=LINE,width=1.2)
    c.line([(cx-rx*0.55,base-ry*0.72),(cx-rx*0.2,base-ry*0.9)],LINE,1.2)      # highlight
    return c.out()
def amoeba(ph):
    c=Cv(30,26); cx,cy=15,13
    pts=[(cx+(8.5+2.6*math.sin(3*a+ph)+1.8*math.sin(5*a-1.7*ph))*math.cos(a)*1.25,cy+(8.5+2.6*math.sin(3*a+ph)+1.8*math.sin(5*a-1.7*ph))*math.sin(a)*0.95) for a in [2*math.pi*i/40 for i in range(40)]]
    c.poly(pts,fill=FILL,outline=LINE,width=1.5)
    c.ell(cx+2*math.cos(ph),cy+1.5*math.sin(ph),3,3,fill=SOFT,outline=LINE,width=1.1)
    c.dot(cx-5,cy-2,1.0); c.dot(cx-3,cy+4,0.8); c.dot(cx+6,cy-3,0.8)
    return c.out()
def paramecium(ph):
    c=Cv(40,20); cx,cy=20,10; rx,ry=15,5.5
    for i in range(14):
        a=2*math.pi*i/14; b=a+0.45*math.sin(ph+i*0.8)
        c.line([(cx+rx*math.cos(a),cy+ry*math.sin(a)),(cx+(rx+2.4)*math.cos(b),cy+(ry+2.4)*math.sin(b))],SOFT,1.1)
    c.ell(cx,cy,rx,ry,fill=FILL,outline=LINE,width=1.5)
    c.ell(cx-4,cy,3.4,2.2,fill=SOFT); c.dot(cx+5,cy-1,1.3); c.dot(cx+9,cy+1,1.0)
    c.line([(cx-10,cy+3.5),(cx-5,cy+1)],LINE,1.1)
    return c.out()
def euglena(ph):
    c=Cv(44,16); cy=8
    c.line([(26+16*i/12, cy+3*math.sin(ph+i*0.9)*(i/12)) for i in range(13)],LINE,1.2)
    body=[(4,cy)]+[(4+22*i/10, cy-4.2*math.sin(math.pi*(i/10)**0.7)) for i in range(1,11)]+[(4+22*i/10, cy+4.2*math.sin(math.pi*(i/10)**0.7)) for i in range(9,0,-1)]
    c.poly(body,fill=FILL,outline=LINE,width=1.5)
    c.dot(21,cy-1,1.2); c.ell(13,cy,3,2,fill=SOFT)
    return c.out()
def rotifer(ph):
    c=Cv(22,40); cx=11
    for sx in (-1,1):
        for i in range(5):
            a=math.pi*(0.15+0.7*i/4)+0.5*math.sin(ph+i)
            c.line([(cx+sx*4.5,7),(cx+sx*4.5+3.4*math.cos(a)*sx*0.9+0,7-3.4*math.sin(a))],SOFT,0.9)
        c.ell(cx+sx*4.5,7,2.6,2.2,fill=FILL,outline=LINE,width=1.2)
    body=[(cx-6.5,8),(cx+6.5,8),(cx+5.5,20),(cx+2.2,29),(cx-2.2,29),(cx-5.5,20)]
    c.poly(body,fill=FILL,outline=LINE,width=1.5)
    c.ell(cx,16,2.6,3.2,outline=LINE,width=1.1)
    s=1.6*math.sin(ph)
    c.line([(cx,29),(cx+s,34)],LINE,1.4); c.line([(cx+s,34),(cx+s-2,38)],LINE,1.1); c.line([(cx+s,34),(cx+s+2,38)],LINE,1.1)
    return c.out()
def copepod(ph):
    c=Cv(40,36); cx=20; a=0.45*math.sin(ph)
    for sx in (-1,1):
        c.line([(cx+sx*2,7),(cx+sx*10,5-6*a),(cx+sx*18,7-10*a)],LINE,1.2)
        for j,y in enumerate((14,18)): c.line([(cx+sx*4,y),(cx+sx*8.5,y+2+2*math.sin(ph+j))],SOFT,1.0)
    body=[(cx,3)]+[(cx+5.2*math.sin(math.pi*(i/10)**0.6),3+19*i/10) for i in range(1,10)]+[(cx,22)]+[(cx-5.2*math.sin(math.pi*(i/10)**0.6),3+19*i/10) for i in range(9,0,-1)]
    c.poly(body,fill=FILL,outline=LINE,width=1.5)
    c.dot(cx,7,1.3)
    c.line([(cx,22),(cx,29)],LINE,2.0); c.line([(cx,29),(cx-3,34)],LINE,1.1); c.line([(cx,29),(cx+3,34)],LINE,1.1)
    return c.out()
def volvox(ph):
    c=Cv(26,26); cx=cy=13
    c.ell(cx,cy,11,11,fill=FILL,outline=LINE,width=1.4)
    for i in range(5):
        a=ph*0.5+2*math.pi*i/5; c.ell(cx+5.5*math.cos(a),cy+5.5*math.sin(a),2.1,2.1,fill=SOFT,outline=LINE,width=0.9)
    for i in range(16):
        a=2*math.pi*i/16+ph*0.5; c.dot(cx+12.3*math.cos(a),cy+12.3*math.sin(a),0.55,SOFT)
    return c.out()
def diatom(ph):
    c=Cv(40,14); cy=7
    top=[(3+34*i/16,cy-4.6*math.sin(math.pi*i/16)) for i in range(17)]; bot=[(3+34*i/16,cy+4.6*math.sin(math.pi*i/16)) for i in range(16,-1,-1)]
    c.poly(top+bot,fill=FILL,outline=LINE,width=1.4)
    c.line([(6,cy),(34,cy)],LINE,1.0)
    for i in range(1,8):
        x=3+34*i/8; hh=4.6*math.sin(math.pi*i/8)-1.3; c.line([(x,cy-hh),(x,cy+hh)],SOFT,0.8)
    return c.out()
def bacteria(ph):
    c=Cv(28,20)
    def rod(x,y,a,l=7,r=2.6):
        dx,dy=l/2*math.cos(a),l/2*math.sin(a); c.line([(x-dx,y-dy),(x+dx,y+dy)],LINE,2*r); c.line([(x-dx,y-dy),(x+dx,y+dy)],FILL,2*r-2.4)
    rod(9,7,0.3+0.5*math.sin(ph)); rod(19,13,-0.5+0.5*math.cos(ph)); rod(8,15,1.2+0.4*math.sin(ph+1),5)
    return c.out()
def spirillum(ph):
    c=Cv(38,12); c.line([(3+32*i/40,6+3.3*math.sin(ph+i*0.55)) for i in range(41)],LINE,2.0)
    c.line([(35,6+3.3*math.sin(ph+22)),(37.5,6+3.3*math.sin(ph+22)+1.5*math.sin(ph*2))],SOFT,0.9)
    return c.out()
def waterbear(ph):
    c=Cv(40,24)
    for i,x in enumerate((9,16,23,30)):
        s=1.8*math.sin(ph+i*math.pi/2)
        c.line([(x,15),(x+s,20.5)],LINE,2.8); c.line([(x+s-1.2,21.5),(x+s+1.4,21.5)],SOFT,0.9)
    c.ell(19,10.5,14,7,fill=FILL,outline=LINE,width=1.5)
    for x in (12,19,26): c.line([(x,5.2+abs(x-19)*0.12),(x,15.6-abs(x-19)*0.12)],SOFT,0.8)
    c.ell(34,11,4.6,4.6,fill=FILL,outline=LINE,width=1.5); c.dot(35.5,9.8,0.9); c.line([(38,12),(39.3,12.4)],LINE,1.6)
    return c.out()


FRAMES = 4
PHASES = [2 * math.pi * i / FRAMES for i in range(FRAMES)]

# (C name, function, mirrored frames for swimming left)
CREATURES = [
    ("MOON_JELLY", lambda p: jelly(44, 50, 18, 11, 7, 22, 3, p), False),
    ("LONG_JELLY", lambda p: jelly(34, 62, 12, 9, 5, 44, 2, p, rim=4, gon=False), False),
    ("BABY_JELLY", lambda p: jelly(28, 34, 10, 7.5, 4, 17, 1, p, rim=3, gon=False, tw=1.2), False),
    ("AMOEBA", amoeba, True),
    ("PARAMECIUM", paramecium, True),
    ("EUGLENA", euglena, True),
    ("ROTIFER", rotifer, True),
    ("COPEPOD", copepod, True),
    ("VOLVOX", volvox, False),
    ("DIATOM", diatom, False),
    ("BACTERIA", bacteria, False),
    ("SPIRILLUM", spirillum, True),
    ("WATER_BEAR", waterbear, True),
]


def c_bytes(data):
    return "\n".join("    " + ", ".join(f"0x{b:02x}" for b in data[i:i + 20]) + "," for i in range(0, len(data), 20))


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--preview", help="also write a PNG with every frame")
    args = p.parse_args()

    images, total = [], 0
    out = ["/*", " * Aquarium creatures, A8 alpha masks (the app colors them). Generated by tools/mkaquarium.py,",
           " * do not edit: change the drawing there and run it again.", " */",
           "#include \"aquarium_sprites.h\"", ""]
    table = []
    for name, fn, mirror in CREATURES:
        frames = [fn(ph) for ph in PHASES]
        images.append(frames)
        sides = [("R", frames)] + ([("L", [f.transpose(Image.FLIP_LEFT_RIGHT) for f in frames])] if mirror else [])
        refs = {}
        for side, imgs in sides:
            refs[side] = []
            for k, im in enumerate(imgs):
                var = f"{name.lower()}_{side.lower()}{k}"
                data = im.tobytes()
                total += len(data)
                out += [f"static const uint8_t {var}_map[] = {{", c_bytes(data), "};",
                        f"static const lv_image_dsc_t {var} = {{",
                        f"    .header = {{.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_A8, "
                        f".w = {im.width}, .h = {im.height}, .stride = {im.width}}},",
                        f"    .data_size = sizeof({var}_map),", f"    .data = {var}_map,", "};", ""]
                refs[side].append("&" + var)
        right = ", ".join(refs["R"])
        left = ", ".join(refs.get("L", refs["R"]))
        table.append(f"    [AQUA_{name}] = {{.w = {frames[0].width}, .h = {frames[0].height}, "
                     f".right = {{{right}}}, .left = {{{left}}}}},")
    out += ["const aqua_sprite_t aqua_sprites[AQUA_SPECIES_COUNT] = {"] + table + ["};", ""]

    enum = "\n".join(f"    AQUA_{n}," for n, _, _ in CREATURES)
    hdr = f"""#pragma once

#include <lvgl.h>

/* Aquarium creatures. Generated by tools/mkaquarium.py, do not edit. */

#define AQUA_FRAMES {FRAMES}

typedef enum {{
{enum}
    AQUA_SPECIES_COUNT
}} aqua_species_t;

typedef struct {{
    uint8_t w;
    uint8_t h;
    const lv_image_dsc_t *right[AQUA_FRAMES];   // facing right
    const lv_image_dsc_t *left[AQUA_FRAMES];    // facing left (the same frames when symmetric)
}} aqua_sprite_t;

extern const aqua_sprite_t aqua_sprites[AQUA_SPECIES_COUNT];
"""
    base = os.path.join(TOP, "src", "apps", "fg", "aquarium_sprites")
    with open(base + ".c", "w") as f:
        f.write("\n".join(out))
    with open(base + ".h", "w") as f:
        f.write(hdr)
    print(f"{len(CREATURES)} creatures, {total} bytes of masks")

    if args.preview:
        cell_w, cell_h = 56, 70
        sheet = Image.new("RGB", (cell_w * FRAMES, cell_h * len(images)), (11, 15, 23))
        for r, frames in enumerate(images):
            for k, im in enumerate(frames):
                x, y = k * cell_w + (cell_w - im.width) // 2, r * cell_h + (cell_h - im.height) // 2
                sheet.paste((46, 196, 214), (x, y), im)
        sheet.resize((sheet.width * 2, sheet.height * 2), Image.NEAREST).save(args.preview)


if __name__ == "__main__":
    main()
