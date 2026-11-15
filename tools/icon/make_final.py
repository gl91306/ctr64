# Final HOME Menu art from the renders (render_marks.py designs 40 and 32):
#   ctr64/cia/icon.png    48x48, the menu render on a light square (the SMDH icon has no alpha)
#   ctr64/cia/banner.png  256x128, the low-poly model, transparent
#   ctr64/cia/banner.bnr  the banner for makerom (needs bannertool, 3ds-bannertool 1.2.3): the 3D
#                         model if CTR64_PYCGFX is set (see below), else banner.png
#   python tools/icon/make_final.py
import os
from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, 'out')
CIA = os.path.join(HERE, '..', '..', 'ctr64', 'cia')


def crop_alpha(im):
    return im.crop(im.getchannel('A').point(lambda a: 255 if a > 8 else 0).getbbox())


def icon(src, size=48, fill=0.96):
    s = size * 8
    bg = Image.new('RGBA', (s, s))
    d = ImageDraw.Draw(bg)
    for y in range(s):  # soft light gradient, a touch warm at the bottom
        t = y / (s - 1)
        d.line([(0, y), (s, y)], fill=(int(250 - 18 * t), int(250 - 22 * t), int(252 - 30 * t), 255))
    m = crop_alpha(Image.open(src).convert('RGBA'))
    k = fill * s / max(m.size)
    m = m.resize((round(m.width * k), round(m.height * k)), Image.LANCZOS)
    bg.alpha_composite(m, ((s - m.width) // 2, (s - m.height) // 2))
    return bg.convert('RGB').resize((size, size), Image.LANCZOS)


def banner(src, w=256, h=128, fill=0.96):
    m = crop_alpha(Image.open(src).convert('RGBA'))
    k = fill * h / m.height
    m = m.resize((round(m.width * k), round(m.height * k)), Image.LANCZOS)
    b = Image.new('RGBA', (w, h), (0, 0, 0, 0))
    b.alpha_composite(m, ((w - m.width) // 2, (h - m.height) // 2))
    return b


icon(os.path.join(OUT, 'mark_40.png')).save(os.path.join(CIA, 'icon.png'))
icon(os.path.join(OUT, 'mark_40.png'), 24).save(os.path.join(OUT, 'icon_final_24.png'))
banner(os.path.join(OUT, 'mark_32.png')).save(os.path.join(CIA, 'banner.png'))
# banner.bnr (what makerom takes): bannertool, silent banner sound
import shutil, subprocess, wave
bt = shutil.which('bannertool') or r'C:\devkitPro\tools\bin\bannertool.exe'
wav = os.path.join(CIA, 'silence.wav')
w = wave.open(wav, 'wb')
w.setnchannels(1)
w.setsampwidth(2)
w.setframerate(22050)
w.writeframes(b'\0\0' * 11025)
w.close()
# the 3D banner: banner_gltf.py (Blender) -> pycgfx -> CGFX; set CTR64_PYCGFX to a pycgfx checkout
# (github.com/skyfloogle/pycgfx, needs gltflib + pillow). Without it: the flat image banner.
pyc = os.environ.get('CTR64_PYCGFX')
if pyc:
    glb, cgfx = os.path.join(OUT, 'banner.glb'), os.path.join(OUT, 'banner.cgfx')
    blender = os.environ.get('BLENDER', r'C:\Program Files\Blender Foundation\Blender 5.0\blender.exe')
    subprocess.run([blender, '-b', '--factory-startup', '-P', os.path.join(HERE, 'banner_gltf.py'), '--', glb],
                   check=True)
    subprocess.run([os.environ.get('CTR64_PYCGFX_PYTHON', 'python'), '-E', 'main.py', glb, cgfx], cwd=pyc,
                   check=True)
    src = ['-ci', cgfx]
else:
    src = ['-i', os.path.join(CIA, 'banner.png')]
subprocess.run([bt, 'makebanner', *src, '-a', wav, '-o', os.path.join(CIA, 'banner.bnr')], check=True)
print('ok')
