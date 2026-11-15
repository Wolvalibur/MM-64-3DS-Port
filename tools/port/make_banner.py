#!/usr/bin/env python3
"""PORT (2026-11-14): a HOME Menu banner from your own picture.

    make_banner.py <template banner.bin> <picture.png/.jpg> <out banner.bin> [preview.png]

The template (port/banner.bin) is a CBMD banner whose model shows one 256 x 128 RGBA4 texture. Only that texture's
pixels change: the picture is fitted whole into it (centred, see-through around it), dithered to 4 bits a channel,
and the banner is written again with the same model and the same sound.
"""
import struct
import sys

from PIL import Image


def lz11_decompress(d):
    if d[0] != 0x11:
        raise ValueError("not LZ11 data")
    size = d[1] | d[2] << 8 | d[3] << 16
    p = 4
    if size == 0:
        size = struct.unpack_from("<I", d, 4)[0]
        p = 8
    out = bytearray()
    while len(out) < size:
        flags = d[p]
        p += 1
        for bit in range(8):
            if len(out) >= size:
                break
            if flags & (0x80 >> bit):
                b0 = d[p]
                ind = b0 >> 4
                if ind == 0:
                    n = (((b0 & 0xF) << 4) | (d[p + 1] >> 4)) + 0x11
                    disp = ((d[p + 1] & 0xF) << 8 | d[p + 2]) + 1
                    p += 3
                elif ind == 1:
                    n = (((b0 & 0xF) << 12) | (d[p + 1] << 4) | (d[p + 2] >> 4)) + 0x111
                    disp = ((d[p + 2] & 0xF) << 8 | d[p + 3]) + 1
                    p += 4
                else:
                    n = ind + 1
                    disp = ((b0 & 0xF) << 8 | d[p + 1]) + 1
                    p += 2
                for _ in range(n):
                    out.append(out[-disp])
            else:
                out.append(d[p])
                p += 1
    return bytes(out)


def lz11_compress(src):
    """Greedy LZ11 (matches of 3..0x110 bytes, up to 0x1000 back)."""
    n = len(src)
    out = bytearray([0x11, n & 0xFF, (n >> 8) & 0xFF, (n >> 16) & 0xFF])
    heads = {}
    p = 0
    while p < n:
        flag_pos = len(out)
        out.append(0)
        flags = 0
        for bit in range(8):
            if p >= n:
                break
            best_len = best_disp = 0
            if p + 3 <= n:
                for cand in reversed(heads.get(src[p:p + 3], [])[-64:]):
                    disp = p - cand
                    if disp > 0x1000:
                        break
                    length = 0
                    while length < 0x110 and p + length < n and src[cand + length] == src[p + length]:
                        length += 1
                    if length > best_len:
                        best_len, best_disp = length, disp
                        if length == 0x110:
                            break
            step = best_len if best_len >= 3 else 1
            if best_len >= 3:
                flags |= 0x80 >> bit
                d = best_disp - 1
                if best_len <= 0x10:
                    out += bytes([((best_len - 1) << 4) | (d >> 8), d & 0xFF])
                else:
                    m = best_len - 0x11
                    out += bytes([m >> 4, ((m & 0xF) << 4) | (d >> 8), d & 0xFF])
            else:
                out.append(src[p])
            for q in range(p, p + step):
                if q + 3 <= n:
                    heads.setdefault(src[q:q + 3], []).append(q)
            p += step
        out[flag_pos] = flags
    return bytes(out)


def morton(x, y):
    m = 0
    for b in range(3):
        m |= ((x >> b) & 1) << (2 * b) | ((y >> b) & 1) << (2 * b + 1)
    return m


def find_texture(cg):
    """The banner's image texture: (pixel offset, width, height). Only a 256 x 128 RGBA4 one is accepted."""
    i = cg.find(b"TXOB")
    while i >= 0:
        if i + 0x48 <= len(cg):
            height, width = struct.unpack_from("<II", cg, i + 0x14)
            fmt = struct.unpack_from("<I", cg, i + 0x30)[0]
            length = struct.unpack_from("<I", cg, i + 0x40)[0]
            data = i + 0x44 + struct.unpack_from("<I", cg, i + 0x44)[0]
            if (width, height, fmt) == (256, 128, 4) and length == width * height * 2 and data + length <= len(cg):
                return data, width, height
        i = cg.find(b"TXOB", i + 1)
    raise ValueError("the template banner has no 256 x 128 RGBA4 texture")


def fit(picture, width, height):
    im = Image.open(picture).convert("RGBA")
    im.thumbnail((width, height), Image.LANCZOS)
    canvas = Image.new("RGBA", (width, height), (0, 0, 0, 0))
    canvas.paste(im, ((width - im.width) // 2, (height - im.height) // 2), im)
    return canvas


def to_rgba4(img):
    """Floyd-Steinberg dithering to 4 bits a channel (smooth gradients instead of bands). Returns rows of 4-tuples."""
    w, h = img.size
    px = [[list(img.getpixel((x, y))) for x in range(w)] for y in range(h)]
    for y in range(h):
        for x in range(w):
            old = px[y][x]
            new = [min(15, max(0, int(round(c / 17.0)))) for c in old]
            for c in range(3):  # colour only: the edges' see-through stays exact
                err = old[c] - new[c] * 17
                if x + 1 < w:
                    px[y][x + 1][c] += err * 7 / 16
                if y + 1 < h:
                    if x > 0:
                        px[y + 1][x - 1][c] += err * 3 / 16
                    px[y + 1][x][c] += err * 5 / 16
                    if x + 1 < w:
                        px[y + 1][x + 1][c] += err * 1 / 16
            px[y][x] = new
    return px


def encode(px, width, height):
    out = bytearray(width * height * 2)
    for ty in range(height // 8):
        for tx in range(width // 8):
            base = (ty * (width // 8) + tx) * 64
            for y in range(8):
                for x in range(8):
                    r, g, b, a = px[height - 1 - (ty * 8 + y)][tx * 8 + x]  # stored bottom row first
                    struct.pack_into("<H", out, (base + morton(x, y)) * 2, r << 12 | g << 8 | b << 4 | a)
    return bytes(out)


def main():
    if len(sys.argv) not in (4, 5):
        sys.exit(__doc__)
    template, picture, out_path = sys.argv[1:4]
    raw = open(template, "rb").read()
    if raw[:4] != b"CBMD":
        sys.exit("the template is not a CBMD banner")
    cgfx_off = struct.unpack_from("<I", raw, 0x08)[0]
    cwav_off = struct.unpack_from("<I", raw, 0x84)[0]
    if cgfx_off != 0x88 or any(struct.unpack_from("<12I", raw, 0x0C)) or cwav_off <= cgfx_off:
        sys.exit("unexpected banner layout (one model, then the sound)")
    cg = bytearray(lz11_decompress(raw[cgfx_off:cwav_off]))
    data, width, height = find_texture(cg)
    px = to_rgba4(fit(picture, width, height))
    cg[data:data + width * height * 2] = encode(px, width, height)

    packed = lz11_compress(bytes(cg))
    if lz11_decompress(packed) != bytes(cg):
        sys.exit("internal error: the compressed model does not read back")
    header = bytearray(raw[:0x88])
    new_cwav = (0x88 + len(packed) + 0x1F) & ~0x1F
    struct.pack_into("<I", header, 0x84, new_cwav)
    body = bytes(header) + packed
    body += b"\0" * (new_cwav - len(body)) + raw[cwav_off:]
    open(out_path, "wb").write(body)

    if len(sys.argv) == 5:  # what the HOME Menu will show, at 4 bits a channel
        prev = Image.new("RGBA", (width, height))
        prev.putdata([tuple(c * 17 for c in p) for row in px for p in row])
        prev.save(sys.argv[4])
    print("banner: %s (%d bytes, was %d)" % (out_path, len(body), len(raw)))


if __name__ == "__main__":
    main()
