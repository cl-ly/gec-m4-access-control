import ctypes
import os
import sys
from ctypes import wintypes

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "chars.txt")
PROJ = r"D:\YUE QIAN tech\AccessControl"
OUT_C = os.path.join(PROJ, "Core", "Src", "oledfont.c")
OUT_H = os.path.join(PROJ, "Core", "Inc", "oledfont.h")

W = 16
H = 16

user32 = ctypes.WinDLL("user32", use_last_error=True)
gdi32 = ctypes.WinDLL("gdi32", use_last_error=True)

user32.GetDC.restype = wintypes.HDC
user32.GetDC.argtypes = [wintypes.HWND]
gdi32.CreateCompatibleDC.restype = wintypes.HDC
gdi32.CreateCompatibleDC.argtypes = [wintypes.HDC]
gdi32.CreateCompatibleBitmap.restype = wintypes.HBITMAP
gdi32.CreateCompatibleBitmap.argtypes = [wintypes.HDC, ctypes.c_int, ctypes.c_int]
gdi32.SelectObject.restype = wintypes.HGDIOBJ
gdi32.SelectObject.argtypes = [wintypes.HDC, wintypes.HGDIOBJ]
gdi32.CreateFontW.restype = wintypes.HFONT
gdi32.CreateFontW.argtypes = [ctypes.c_int] * 5 + [wintypes.DWORD] * 8 + [wintypes.LPCWSTR]
gdi32.PatBlt.restype = wintypes.BOOL
gdi32.PatBlt.argtypes = [wintypes.HDC, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int, wintypes.DWORD]
gdi32.TextOutW.restype = wintypes.BOOL
gdi32.TextOutW.argtypes = [wintypes.HDC, ctypes.c_int, ctypes.c_int, wintypes.LPCWSTR, ctypes.c_int]
gdi32.GetPixel.restype = wintypes.DWORD
gdi32.GetPixel.argtypes = [wintypes.HDC, ctypes.c_int, ctypes.c_int]
gdi32.SetBkMode.restype = ctypes.c_int
gdi32.SetBkMode.argtypes = [wintypes.HDC, ctypes.c_int]
gdi32.SetTextColor.restype = wintypes.DWORD
gdi32.SetTextColor.argtypes = [wintypes.HDC, wintypes.DWORD]
gdi32.DeleteObject.argtypes = [wintypes.HGDIOBJ]

screen = user32.GetDC(None)
if not screen:
    print("GetDC failed")
    sys.exit(2)
memdc = gdi32.CreateCompatibleDC(screen)
bmp = gdi32.CreateCompatibleBitmap(screen, W, H)
if not memdc or not bmp:
    print("dc/bmp failed")
    sys.exit(2)
gdi32.SelectObject(memdc, bmp)
gdi32.SetBkMode(memdc, 1)
gdi32.SetTextColor(memdc, 0x00FFFFFF)
font = gdi32.CreateFontW(16, 0, 0, 0, 400, 0, 0, 0, 134, 0, 0, 0, 0, u"SimSun")
if not font:
    print("CreateFontW failed")
    sys.exit(2)
gdi32.SelectObject(memdc, font)


def bitmap_of(ch):
    gdi32.PatBlt(memdc, 0, 0, W, H, 0x00000042)
    gdi32.TextOutW(memdc, 0, 0, ch, 1)
    px = [[0] * W for _ in range(H)]
    for y in range(H):
        for x in range(W):
            c = gdi32.GetPixel(memdc, x, y)
            if (c & 0xFF) >= 0x80:
                px[y][x] = 1
    return px


def bbox(px):
    minx, maxx, miny, maxy = 99, -1, 99, -1
    for y in range(H):
        for x in range(W):
            if px[y][x]:
                if x < minx:
                    minx = x
                if x > maxx:
                    maxx = x
                if y < miny:
                    miny = y
                if y > maxy:
                    maxy = y
    return minx, maxx, miny, maxy


def page_bytes(px, x0, ncols):
    out = []
    for p in (0, 1):
        for i in range(ncols):
            v = 0
            for b in range(8):
                if px[p * 8 + b][x0 + i]:
                    v |= (1 << b)
            out.append(v)
    return out


def preview(tag, px):
    print("---- %s ----" % tag)
    for y in range(H):
        print("".join("#" if px[y][x] else "." for x in range(W)))


with open(SRC, "r", encoding="utf-8") as f:
    txt = f.read()
cn = []
for ch in txt:
    if not ch.strip():
        continue
    if ch not in cn:
        cn.append(ch)

ascii_rows = []
clip = 0
shift = 0
for code in range(32, 127):
    ch = chr(code)
    px = bitmap_of(ch)
    minx, maxx, miny, maxy = bbox(px)
    if maxx > 7:
        clip += 1
    if minx > 0 and maxx <= 7:
        shift += 1
    if code in (35, 48, 65, 56):
        preview("ASCII %d %s bbox=%d,%d,%d,%d" % (code, ch, minx, maxx, miny, maxy), px)
    ascii_rows.append((code, page_bytes(px, 0, 8)))

cn_rows = []
for idx, ch in enumerate(cn):
    px = bitmap_of(ch)
    minx, maxx, miny, maxy = bbox(px)
    if idx < 2:
        preview("CN %d U+%04X bbox=%d,%d,%d,%d" % (idx, ord(ch), minx, maxx, miny, maxy), px)
    cn_rows.append((ord(ch), page_bytes(px, 0, 16)))


def hexline(vals):
    return ",".join("0x%02X" % v for v in vals)

h = []
h.append("#ifndef __OLEDFONT_H")
h.append("#define __OLEDFONT_H")
h.append("")
h.append("typedef struct")
h.append("{")
h.append("    unsigned short code;")
h.append("    unsigned char  dat[32];")
h.append("} CN_GLYPH;")
h.append("")
h.append("#define OLED_CN16_NUM  %d" % len(cn_rows))
h.append("")
h.append("extern const unsigned char OLED_F8x16[95][16];")
h.append("extern const CN_GLYPH OLED_CN16[OLED_CN16_NUM];")
h.append("")
h.append("#endif")
h.append("")

c = []
c.append('#include "oledfont.h"')
c.append("")
c.append("/* ASCII 32..126, 8x16, column major, 2 pages per glyph */")
c.append("const unsigned char OLED_F8x16[95][16] = {")
for code, vals in ascii_rows:
    c.append("    {%s}, /* %d */" % (hexline(vals), code))
c.append("};")
c.append("")
c.append("/* Chinese 16x16, column major, 2 pages per glyph */")
c.append("const CN_GLYPH OLED_CN16[OLED_CN16_NUM] = {")
for codec, vals in cn_rows:
    c.append("    {0x%04X, {%s}}," % (codec, hexline(vals)))
c.append("};")
c.append("")

try:
    with open(OUT_H, "w", encoding="ascii", newline="\n") as f:
        f.write("\n".join(h))
    with open(OUT_C, "w", encoding="ascii", newline="\n") as f:
        f.write("\n".join(c))
    ok = True
except Exception as e:
    ok = False
    print("WRITE FAILED: %r" % (e,))

print("ascii=%d cn=%d clip=%d shift=%d write_ok=%s" % (len(ascii_rows), len(cn_rows), clip, shift, ok))
print("H=%s" % OUT_H)
print("C=%s" % OUT_C)
