#!/usr/bin/env python3
"""The game's own icon, from the player's ROM: the 32x32 picture the DS menu
shows for the cartridge (in the ROM's banner). Writes a Windows .ico for
dqix.exe (and optionally a .png), scaled up with sharp pixels.

    rom_icon.py <rom.nds> <out.ico> [out.png]

Nothing from the game is kept in this project: the icon is made from the ROM
when the game is built, like the rest of the game. Only the standard library
is used (the build's Python has no extra packages)."""
import struct
import sys
import zlib


def banner_icon(banner):
    """32x32 RGBA pixels from the ROM's banner (its first 0x240 bytes)"""
    bits = banner[0x20:0x220]                    # 4x4 tiles of 8x8, 4 bits a pixel
    pal = struct.unpack_from("<16H", banner, 0x220)
    colours = []
    for i, c in enumerate(pal):                  # BGR555; colour 0 is see-through
        r, g, b = c & 31, (c >> 5) & 31, (c >> 10) & 31
        colours.append(((r << 3) | (r >> 2), (g << 3) | (g >> 2), (b << 3) | (b >> 2), 0 if i == 0 else 255))
    px = [[(0, 0, 0, 0)] * 32 for _ in range(32)]
    for t in range(16):
        tx, ty = t % 4, t // 4
        for y in range(8):
            for x in range(8):
                b = bits[t * 32 + y * 4 + x // 2]
                px[ty * 8 + y][tx * 8 + x] = colours[(b >> 4) if x & 1 else (b & 15)]
    return px


def scale(px, n):
    """n x n, nearest pixel (the icon is pixel art)"""
    s = len(px)
    return [[px[y * s // n][x * s // n] for x in range(n)] for y in range(n)]


def png(px):
    n = len(px)
    raw = b"".join(b"\0" + bytes(v for p in row for v in p) for row in px)

    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", n, n, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def bmp_entry(px):
    """a classic icon image (32-bit BGRA, bottom-up, with the AND mask), which
    every Windows version and tool reads; used for the small sizes"""
    n = len(px)
    head = struct.pack("<IiiHHIIiiII", 40, n, n * 2, 1, 32, 0, 0, 0, 0, 0, 0)
    xor = b"".join(bytes((p[2], p[1], p[0], p[3])) for row in reversed(px) for p in row)
    stride = ((n + 31) // 32) * 4
    mask = b""
    for row in reversed(px):
        bits = bytearray(stride)
        for x, p in enumerate(row):
            if p[3] == 0:
                bits[x // 8] |= 0x80 >> (x % 8)
        mask += bytes(bits)
    return head + xor + mask


def ico(px):
    sizes = [16, 24, 32, 48, 64, 128, 256]
    images = [png(scale(px, n)) if n >= 128 else bmp_entry(scale(px, n)) for n in sizes]
    out = struct.pack("<HHH", 0, 1, len(sizes))
    at = 6 + 16 * len(sizes)
    for n, data in zip(sizes, images):
        out += struct.pack("<BBBBHHII", n % 256, n % 256, 0, 0, 1, 32, len(data), at)
        at += len(data)
    return out + b"".join(images)


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    with open(sys.argv[1], "rb") as f:
        head = f.read(0x200)
        off = struct.unpack_from("<I", head, 0x68)[0] if len(head) >= 0x6C else 0
        f.seek(off)
        banner = f.read(0x240)
    if off == 0 or len(banner) < 0x240:
        sys.exit("no icon in this ROM")
    px = banner_icon(banner)
    with open(sys.argv[2], "wb") as f:
        f.write(ico(px))
    if len(sys.argv) > 3:
        with open(sys.argv[3], "wb") as f:
            f.write(png(scale(px, 256)))


if __name__ == "__main__":
    main()
