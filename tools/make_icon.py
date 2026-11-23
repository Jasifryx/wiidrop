"""Draws meta/icon.png (128x128, the Wii U Menu icon): WiiDrop's droplet on a dark background.
Pure Python, nothing to install:  python tools/make_icon.py"""
import math, os, struct, zlib

SIZE, SS = 128, 4  # output size, supersampling
N = SIZE * SS


def lerp(a, b, t):
    return tuple(a[i] + (b[i] - a[i]) * t for i in range(len(a)))


CYAN, VIOLET = (57, 198, 255), (167, 92, 255)
BG_TOP, BG_BOT = (16, 20, 52), (40, 20, 78)

# droplet geometry (same shape as the logo drawn in source/ui.c), in output pixels
s = 1.96
cx, cy = 64.0, 65.0
r = 17 * s
apex = (cx, cy - 26 * s)
baseY = cy + 1 * s
halfW = r * 0.93
discC = (cx, cy + 8 * s)
shineC, shineR = (cx - 6 * s, cy + 4 * s), 4.5 * s


def in_drop(x, y):
    if (x - discC[0]) ** 2 + (y - discC[1]) ** 2 <= r * r:
        return True
    if apex[1] <= y <= baseY:  # triangle from the apex down to the base
        t = (y - apex[1]) / (baseY - apex[1])
        return abs(x - cx) <= halfW * t
    return False


def pixel(x, y):
    """colour (r, g, b, a) at output-pixel coordinates x, y"""
    # rounded-square background with a soft cyan glow behind the drop
    m, rad = 2.0, 26.0
    qx, qy = max(abs(x - 64) - (64 - m - rad), 0), max(abs(y - 64) - (64 - m - rad), 0)
    if math.hypot(qx, qy) > rad:
        return (0, 0, 0, 0)
    c = lerp(BG_TOP, BG_BOT, y / SIZE)
    g = max(0.0, 1 - math.hypot(x - cx, y - (cy + 4)) / 62)
    c = lerp(c, CYAN, 0.35 * g * g)
    if in_drop(x, y):
        t = min(1.0, max(0.0, (y - apex[1]) / (discC[1] + r - apex[1])))
        c = lerp(CYAN, VIOLET, t * 0.9)
        d = math.hypot(x - shineC[0], y - shineC[1])
        if d <= shineR:
            c = lerp(c, (255, 255, 255), 0.6)
    return (c[0], c[1], c[2], 255)


rows = []
for oy in range(SIZE):
    row = bytearray([0])  # PNG filter type 0
    for ox in range(SIZE):
        acc = [0.0, 0.0, 0.0, 0.0]
        for sy in range(SS):
            for sx in range(SS):
                p = pixel(ox + (sx + 0.5) / SS, oy + (sy + 0.5) / SS)
                a = p[3] / 255
                acc[0] += p[0] * a
                acc[1] += p[1] * a
                acc[2] += p[2] * a
                acc[3] += a
        n = SS * SS
        alpha = acc[3] / n
        if alpha > 0:
            row += bytes(int(round(acc[i] / acc[3])) for i in range(3)) + bytes([int(round(alpha * 255))])
        else:
            row += bytes(4)
    rows.append(bytes(row))


def chunk(tag, data):
    return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)


png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", SIZE, SIZE, 8, 6, 0, 0, 0))
png += chunk(b"IDAT", zlib.compress(b"".join(rows), 9)) + chunk(b"IEND", b"")
out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "meta", "icon.png")
os.makedirs(os.path.dirname(out), exist_ok=True)
open(out, "wb").write(png)
print("wrote", os.path.normpath(out), len(png), "bytes")
