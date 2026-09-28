#!/usr/bin/env python3
"""Compile the generated source for AGA; draw editable original pixel sprites.
Pillow is only an authoring dependency. agk art/build itself stays stdlib-only.
"""
from pathlib import Path
from PIL import Image, ImageDraw
from font import letters
import math
ROOT=Path(__file__).resolve().parents[1]
# 176 scene colors + 16 reserved sprite slots + 64 carefully chosen armor, emissive and UI colors.
scene=Image.open(ROOT/'source/foundry.png').convert('RGB').resize((320,256),Image.Resampling.LANCZOS).quantize(colors=176,dither=Image.Dither.NONE)
original=scene.getpalette()
# Color 0 also paints the physical monitor border; keep it dark. Sprite
# transparency does not display color 16, so it can retain this scene color.
base=[8,14,24]+original[3:48]+original[:3]+[0]*45+original[48:528]
scene=scene.point([16 if i==0 else i if i<16 else i+16 for i in range(256)])
objects=[0x080E18,0x121E2C,0x223348,0x354B60,0x526D80,0x7796A4,0xACC6CC,0xE6F3EE,
 0x122F36,0x1B5058,0x277B82,0x43B3BA,0x7DE9E5,0xCFFFF1,0x163D51,0x2680A6,
 0x472228,0x733137,0xAC4140,0xD85C45,0xF38B56,0xFFBC77,0xFFE9AA,0xFFF9DA,
 0x302339,0x543A55,0x765872,0xA27C99,0xD1A8BF,0xFFE3EB,0xAF1537,0xFF3755,
 0x382618,0x614128,0x946139,0xC68B4C,0xECAD59,0xFFD07F,0xFFECBB,0xFCFFFF,
 0x153928,0x22603B,0x388C4C,0x60C86E,0x9DEB94,0xDDF7C1,0x41648F,0x7298C5,
 0x201C26,0x453A43,0x665A62,0x988781,0xBDAE9A,0xE2D1AD,0xF5DDAD,0xFFC149,
 0xD33A18,0xF66622,0xFF9B32,0xFFCE51,0xFFF49B,0xC9F8FF,0x87E2FF,0xFFFFFF]
pal=base+[v for c in objects for v in (c>>16,c>>8&255,c&255)]
scene.putpalette(pal);scene.convert('RGB').save(ROOT/'foundry.png')
(ROOT/'palette.txt').write_text(''.join(f'{i} 0x{pal[i*3]:02X}{pal[i*3+1]:02X}{pal[i*3+2]:02X}\n' for i in range(256)))
def rgb(i): return tuple(pal[i*3:i*3+3])+(255,)
def canvas(w,h): return Image.new('RGBA',(w,h),(0,0,0,0))
def sheet(name, frames):
    w,h=frames[0].size
    out=canvas(w*len(frames),h)
    for f,im in enumerate(frames):out.paste(im,(f*w,0))
    out.save(ROOT/(name+'.png'))
# 48x48 assault armor: asymmetrical cannon, faceted pauldrons, piston legs.
frames=[]
for f in range(4):
 im=canvas(48,48);d=ImageDraw.Draw(im)
 def poly(p,c):d.polygon(p,fill=rgb(c))
 def box(p,c):d.rectangle(p,fill=rgb(c))
 def line(p,c):d.line(p,fill=rgb(c))
 # luminous thrusters, varying actual silhouette
 for x,y in [(10,27),(20,39),(31,39)]:
  poly([(x-3,y),(x+3,y),(x+2,y+4),(x,y+7+(f&1)*2),(x-2,y+4)],207)
  poly([(x-1,y),(x+1,y),(x,y+6-(f&1))],253)
 # rear pack
 box((6,13,15,29),192);box((7,14,13,25),194);box((8,15,10,24),196)
 for y in (17,20,23):line((8,y,12,y),193)
 # legs: joint and upper edge, hip pistons
 for x,y in [(18,29+(f&1)),(29,28+((f>>1)&1))]:
  box((x-3,y-2,x+4,y+5),192);box((x-2,y,x+3,y+4),195)
  box((x,y+4,x+2,y+9),197);box((x-3,y+7,x+5,y+12),192)
  poly([(x-3,y+7),(x+3,y+6),(x+5,y+10),(x+4,y+12),(x-4,y+12)],201)
  line((x-2,y+7,x+2,y+7,x+3,y+10),204);box((x-4,y+12,x+5,y+14),193)
  line((x-3,y+13,x+3,y+13),196)
 # body angular plate
 poly([(15,12),(27,11),(33,18),(30,29),(22,33),(14,28),(12,19)],192)
 poly([(16,13),(26,12),(31,18),(28,28),(22,30),(15,26),(14,19)],201)
 poly([(16,13),(26,12),(29,16),(20,19),(14,19)],204)
 poly([(21,20),(29,17),(27,27),(22,29)],202)
 line((15,21,18,27,22,29),200);box((20,20,25,22),192);line((21,20,25,20),219)
 box((15,27,18,29),196);box((23,28,28,30),195)
 # head helmet, glowing slit, antenna
 line((21,3,21,9),196)
 poly([(21,5),(28,5),(31,9),(30,15),(21,16),(18,11)],192)
 poly([(22,6),(27,6),(29,9),(20,9)],197)
 poly([(20,10),(30,9),(29,14),(22,15)],195)
 line((22,10,29,10),255);line((22,11,28,11),223);box((26,13,29,14),193)
 # forward heavy shoulder
 poly([(28,14),(36,14),(40,19),(37,25),(29,24),(26,20)],192)
 poly([(29,15),(35,15),(38,19),(35,22),(29,21)],196)
 line((29,15,35,15,37,17),199)
 for x in (30,33,36):line((x,18,x+1,20),193)
 # gun with long barrel and glowing heatsink
 box((28,23,44,29),192);box((29,24,43,27),195);line((30,24,43,24),197)
 box((39,22,44,29),193);line((40,23,43,23),196);box((45,24,47,28),192)
 line((45,25,47,25),220)
 for x in (31,34,37):box((x,25,x+1,26),218)
 # rivets
 for x,y in [(16,17),(28,24),(31,16),(19,31)]:d.point((x,y),fill=rgb(199))
 frames.append(im)
sheet('mech',frames)
# Hunter drones, red optics, counter-rotating engine rings.
frames=[]
for f in range(4):
 im=canvas(32,28);d=ImageDraw.Draw(im)
 for cx in (6,25):
  d.ellipse((cx-5,9,cx+5,21),fill=rgb(192),outline=rgb(195))
  d.ellipse((cx-3,11,cx+3,19),fill=rgb(210),outline=rgb(218))
  d.line((cx-2,14+(f&1)*2,cx+2,14+(f&1)*2),fill=rgb(222))
 d.polygon([(9,7),(22,7),(27,13),(24,20),(17,23),(7,19),(5,13)],fill=rgb(192))
 d.polygon([(10,8),(21,8),(25,13),(22,17),(9,17),(7,13)],fill=rgb(211))
 d.line((10,8,21,8,23,10),fill=rgb(214));d.polygon([(10,11),(21,11),(19,15),(12,15)],fill=rgb(208))
 d.rectangle((11,12,21,13),fill=rgb(223));d.line((13,12,19,12),fill=rgb(255))
 d.rectangle((12,19,17,23),fill=rgb(194));d.rectangle((5,17,11,21),fill=rgb(195))
 d.rectangle((2,19,7,21),fill=rgb(197));d.line((12,4,12,8),fill=rgb(196))
 d.point((12,4),fill=rgb(223));frames.append(im)
sheet('drone',frames)
# The Moloch siege engine: 80x80 walking reactor, broad shoulders and cannon.
frames=[]
for f in range(2):
 im=canvas(80,80);d=ImageDraw.Draw(im)
 def b(p,c):d.rectangle(p,fill=rgb(c))
 def p(points,c):d.polygon(points,fill=rgb(c))
 def l(points,c):d.line(points,fill=rgb(c))
 for x in (29,59):
  b((x-6,49,x+7,63),192);b((x-4,51,x+5,66),194)
  for xx in (x-3,x+3):b((xx,56,xx+1,69),197)
  p([(x-8,65),(x+8,63),(x+11,75),(x+7,78),(x-13,78),(x-12,72)],192)
  p([(x-7,66),(x+6,65),(x+8,73),(x-11,74),(x-10,71)],211)
  l((x-7,66,x+6,65,x+7,67),214)
  for xx in range(x-10,x+7,4):l((xx,75,xx+1,77),196)
 # reactor back and pipes
 b((58,13,74,49),192);b((60,15,71,45),194)
 for yy in range(17,43,5):b((61,yy,73,yy+1),196)
 b((66,6,72,17),193);b((67,5,74,8),196)
 p([(23,24),(37,16),(61,19),(70,34),(62,53),(43,61),(24,51),(19,36)],192)
 p([(26,25),(38,18),(60,21),(65,32),(60,49),(43,56),(26,48),(22,36)],210)
 p([(26,25),(39,18),(59,21),(54,28),(33,29)],214)
 p([(45,31),(61,27),(64,34),(58,48),(44,53)],209)
 # enormous hot circular core
 d.ellipse((32,30,55,52),fill=rgb(192),outline=rgb(196))
 d.ellipse((35,33,52,49),fill=rgb(248),outline=rgb(214))
 d.ellipse((38,35,49,46),fill=rgb(250+f),outline=rgb(252))
 b((36,39,51,41),219);b((40,34,42,48),219)
 # aggressive face with sensor hood
 p([(30,10),(47,8),(54,13),(52,24),(35,28),(27,21)],192)
 p([(32,11),(46,10),(51,14),(31,17)],196)
 p([(31,18),(51,15),(49,22),(35,25)],193)
 l((32,19,49,17),223);l((35,20,46,19),255)
 b((37,23,44,26),195)
 # shoulder cannon aiming left
 p([(13,24),(29,21),(34,29),(30,38),(16,39),(9,32)],192)
 p([(14,25),(27,23),(31,28),(26,33),(13,33)],195)
 l((14,25,27,23,30,26),198)
 b((4,32,28,40),192);b((4,33,24,38),196);b((0,32,7,41),193)
 l((0,33,6,33),214);b((0,35,2,39),250)
 for x in (10,15,20):b((x,35,x+2,37),193)
 # armored shoulder right
 p([(59,21),(71,21),(77,27),(76,40),(67,44),(60,37)],192)
 p([(60,23),(70,23),(75,27),(74,36),(67,40),(62,35)],211)
 l((60,23,70,23,74,26),214)
 for y in (28,31,34):l((66,y,73,y),209)
 for x,y in [(25,29),(26,45),(59,45),(65,26),(33,13)]:b((x,y,x+1,y+1),198)
 frames.append(im)
sheet('boss',frames)
frames=[]
for f in range(6):
 im=canvas(32,32);d=ImageDraw.Draw(im);r=[5,11,15,14,12,8][f]
 for j in range(9):
  a=j*2*math.pi/9+f*.35;cx=16+int(math.cos(a)*r*.55);cy=16+int(math.sin(a)*r*.55)
  rr=max(2,r//2-(f//3));d.ellipse((cx-rr,cy-rr,cx+rr,cy+rr),fill=rgb(208 if f>3 else 248))
  d.ellipse((cx-rr+2,cy-rr+2,cx+rr-1,cy+rr-1),fill=rgb(211 if f>3 else 250))
 if f<4:d.ellipse((16-r//2,16-r//2,16+r//2,16+r//2),fill=rgb(252 if f>1 else 255))
 for j in range(6):
  a=j*1.05+f;cx=16+int(math.cos(a)*min(15,r+3));cy=16+int(math.sin(a)*min(15,r+3));d.point((cx,cy),fill=rgb(251))
 frames.append(im)
sheet('blast',frames)
for name,color in [('shot',252),('bolt',223)]:
 frames=[]
 for f in range(2):
  im=canvas(16,8);d=ImageDraw.Draw(im)
  d.polygon([(0,3),(8,1),(13,2),(15,4),(12,6),(7,6),(0,4)],fill=rgb(218 if name=='shot' else 210))
  d.line((3,3,12,3),fill=rgb(color),width=2);d.line((7,3,13,3),fill=rgb(255 if f else color));frames.append(im)
 sheet(name,frames)
# Pickups use high palette indices too, providing an in-game eight-plane probe.
im=canvas(16,16);d=ImageDraw.Draw(im);d.rectangle((2,2,13,13),fill=rgb(192),outline=rgb(235));d.rectangle((4,4,11,11),fill=rgb(233));d.line((5,8,10,8),fill=rgb(237),width=2);d.line((8,5,8,10),fill=rgb(237),width=2);sheet('repair',[im])
config='[art]\nchipset = "aga"\ndepth = 8\n[foundry]\nsource = "foundry.png"\nkind = "bitmap"\n'
for name,w,h in [('mech',48,48),('drone',32,28),('boss',80,80),('blast',32,32),('shot',16,8),('bolt',16,8),('repair',16,16)]:
 kind='sprite' if name=='mech' else 'bob'
 config+=f'[{name}]\nsource = "{name}.png"\nkind = "{kind}"\nframe_width = {w}\nframe_height = {h}\n'
 if name=='mech':config+='attached = true\nchannel = 0\n'
(ROOT/'art.toml').write_text(config)
# Glyph rows, generated independently of the game's screen depth.
rows=[]
for c in range(128):rows.append('{'+','.join(str(int(row,2)<<1) for row in letters.get(chr(c),letters[' ']))+'}')
(ROOT.parent/'src/font.h').write_text('/* Generated by art/tools/build_art.py */\nstatic const unsigned char glyphs[128][7] = {\n'+',\n'.join(rows)+'\n};\n')
print('Compiled foundry: 176 scene colors, 16 sprite slots, 64 combat/UI colors; original sprite sheets ready.')
# Keep the native hand-drawn candidates for comparison. Selected RD assets are
# compiled to hardware format; alpha and the original source files are preserved.
import shutil
hand=ROOT/'source/handcrafted';hand.mkdir(exist_ok=True)
for name in ['mech','boss']:shutil.copyfile(ROOT/(name+'.png'),hand/(name+'.png'))
rd=ROOT/'source/rd'
mech_src=rd/'mech-idle.png' if (rd/'mech-idle.png').exists() else rd/'mech-1.png'
if mech_src.exists():
 im=Image.open(mech_src).convert('RGBA')
 if im.size==(48,48):sheet('mech',[im]*4)
 elif im.size==(192,48):im.save(ROOT/'mech.png')
 elif im.size==(192,96):
  # Retain four poses with the cannon muzzle at the right edge. Middle poses
  # retract the gun too far for a continuous-fire sprite; keep the source intact.
  selected=[0,1,6,7]
  sheet('mech',[im.crop((f%4*48,f//4*48,f%4*48+48,f//4*48+48)) for f in selected])
 else:raise ValueError(f'Inspect animation sheet dimensions before using it: {im.size}')
# Four stride poses, eight cannon poses, six destruction poses. All source
# sheets remain untouched; this only packs frames and maps the AGA palette.
if (rd/'boss-walking.png').exists():
 target=Image.new('P',(1,1));target.putpalette(pal)
 frames=[]
 for action,selected in [('walking',[0,2,4,6]),('attack',list(range(8))),('destroy',[0,1,2,3,4,7])]:
  im=Image.open(rd/f'boss-{action}.png').convert('RGBA')
  assert im.size==(320,160), (action,im.size)
  for f in selected:
   pose=im.crop((f%4*80,f//4*80,f%4*80+80,f//4*80+80))
   colors=pose.convert('RGB').quantize(palette=target,dither=Image.Dither.NONE).convert('RGBA')
   colors.putalpha(pose.getchannel('A'));frames.append(colors)
 sheet('boss',frames)
