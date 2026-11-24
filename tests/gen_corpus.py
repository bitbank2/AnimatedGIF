#!/usr/bin/env python3
"""
Deterministic GIF test corpus generator for AnimatedGIF-LowMem.
Generates valid, edge-case, and malformed GIF vectors in tests/corpus/,
and validates all valid/edge GIFs independently with Pillow (PIL).
"""
import os
import struct
import random
from pathlib import Path
from PIL import Image

OUT = Path(__file__).resolve().parent / "corpus"
OUT.mkdir(parents=True, exist_ok=True)


class BitWriter:
    def __init__(self):
        self.buf = bytearray()
        self.acc = 0
        self.bits = 0

    def write(self, code: int, width: int):
        self.acc |= (code & ((1 << width) - 1)) << self.bits
        self.bits += width
        while self.bits >= 8:
            self.buf.append(self.acc & 0xFF)
            self.acc >>= 8
            self.bits -= 8

    def finish(self) -> bytes:
        if self.bits > 0:
            self.buf.append(self.acc & 0xFF)
            self.acc = 0
            self.bits = 0
        return bytes(self.buf)


def subblocks(data: bytes, chunk_sizes=None) -> bytes:
    out = bytearray()
    i = 0
    idx = 0
    while i < len(data):
        sz = 255 if chunk_sizes is None else chunk_sizes[idx % len(chunk_sizes)]
        sz = max(1, min(255, sz))
        part = data[i:i + sz]
        out.append(len(part))
        out.extend(part)
        i += len(part)
        idx += 1
    out.append(0)
    return bytes(out)


def lzw_encode(pixels: list[int], min_code_size: int, emit_multi_cc=False, deferred_cc=False,
               force_kwkwk_test=False) -> bytes:
    cc = 1 << min_code_size
    eoi = cc + 1
    max_dict = 4096

    bw = BitWriter()
    codesize = min_code_size + 1
    nextcode = cc + 2
    nextlim = 1 << codesize
    table = {(p,): p for p in range(cc)}

    def reset():
        nonlocal codesize, nextcode, nextlim, table
        bw.write(cc, codesize)
        codesize = min_code_size + 1
        nextcode = cc + 2
        nextlim = 1 << codesize
        table = {(p,): p for p in range(cc)}

    bw.write(cc, codesize)
    if not pixels:
        bw.write(eoi, codesize)
        return bw.finish()

    w = (pixels[0],)
    emitted = 0
    for k in pixels[1:]:
        wk = w + (k,)
        if wk in table:
            w = wk
        else:
            bw.write(table[w], codesize)
            emitted += 1
            if nextcode < max_dict:
                table[wk] = nextcode
                nextcode += 1
                if nextcode > nextlim and codesize < 12:
                    codesize += 1
                    nextlim <<= 1
            elif not deferred_cc:
                reset()
                w = (k,)
                continue
            if emit_multi_cc and emitted % 300 == 0 and nextcode > cc + 50:
                reset()
                w = (k,)
                continue
            w = (k,)
    bw.write(table[w], codesize)
    bw.write(eoi, codesize)
    return bw.finish()


def make_header(w: int, h: int, gct_colors: list[tuple[int, int, int]] | None, bg=0) -> bytes:
    out = bytearray(b"GIF89a")
    if gct_colors is not None:
        bits = 0
        while (2 << bits) < len(gct_colors):
            bits += 1
        packed = 0x80 | (7 << 4) | bits
        out.extend(struct.pack("<HHBBB", w, h, packed, bg, 0))
        pad = (2 << bits) - len(gct_colors)
        for r, g, b in gct_colors:
            out.extend((r, g, b))
        out.extend(b"\x00\x00\x00" * pad)
    else:
        out.extend(struct.pack("<HHBBB", w, h, 0, bg, 0))
    return bytes(out)


def make_gce(delay_cs=10, disposal=0, transparent=None) -> bytes:
    flags = (disposal & 7) << 2
    tidx = 0
    if transparent is not None:
        flags |= 1
        tidx = transparent
    return b"\x21\xF9\x04" + struct.pack("<BHB", flags, delay_cs, tidx) + b"\x00"


def make_netscape(loops=0) -> bytes:
    return b"\x21\xFF\x0BNETSCAPE2.0\x03\x01" + struct.pack("<H", loops) + b"\x00"


def make_image_desc(x, y, w, h, lct_colors=None, interlaced=False) -> bytes:
    packed = 0x40 if interlaced else 0
    pal_bytes = b""
    if lct_colors is not None:
        bits = 0
        while (2 << bits) < len(lct_colors):
            bits += 1
        packed |= 0x80 | bits
        pad = (2 << bits) - len(lct_colors)
        pb = bytearray()
        for r, g, b in lct_colors:
            pb.extend((r, g, b))
        pb.extend(b"\x00\x00\x00" * pad)
        pal_bytes = bytes(pb)
    return b"\x2C" + struct.pack("<HHHHB", x, y, w, h, packed) + pal_bytes


def write_gif(name: str, data: bytes, verify_pil=True):
    p = OUT / name
    p.write_bytes(data)
    if verify_pil:
        with Image.open(p) as im:
            n = getattr(im, "n_frames", 1)
            for i in range(n):
                im.seek(i)
                im.convert("RGB").load()


def build_valid_and_edge():
    # 1. edge_small_1x1_2bit.gif (41 bytes; exposes upstream bug rejecting <267B 2-bit+ GIFs)
    g = make_header(1, 1, [(12, 34, 56), (255, 0, 0), (0, 255, 0), (0, 0, 255)])
    g += make_image_desc(0, 0, 1, 1) + b"\x02" + subblocks(lzw_encode([0], 2)) + b"\x3B"
    write_gif("edge_small_1x1_2bit.gif", g)

    # 2. valid_8bit_256colors.gif (32x32 using all 256 colors)
    pal256 = [((i * 37) % 256, (i * 73) % 256, (i * 151) % 256) for i in range(256)]
    px256 = [(x + y * 32) & 0xFF for y in range(32) for x in range(32)]
    g = make_header(32, 32, pal256)
    g += make_image_desc(0, 0, 32, 32) + b"\x08" + subblocks(lzw_encode(px256, 8)) + b"\x3B"
    write_gif("valid_8bit_256colors.gif", g)

    # 3. valid_local_palette.gif (global -> local -> global, 32x32 so >300B)
    gpal = [(10, 20, 30), (200, 100, 50), (30, 200, 80), (240, 240, 240)]
    lpal = [(250, 10, 10), (10, 250, 10), (10, 10, 250), (255, 255, 0)]
    px_a = [((i * 3 + (i // 32) * 5 + (i % 7)) % 4) for i in range(32 * 32)]
    px_b = [((i * 5 + (i // 32) * 3 + 1) % 4) for i in range(32 * 32)]
    g = make_header(32, 32, gpal) + make_netscape(0)
    g += make_gce(10, 0) + make_image_desc(0, 0, 32, 32) + b"\x02" + subblocks(lzw_encode(px_a, 2))
    g += make_gce(12, 0) + make_image_desc(0, 0, 32, 32, lct_colors=lpal) + b"\x02" + subblocks(lzw_encode(px_b, 2))
    g += make_gce(15, 0) + make_image_desc(0, 0, 32, 32) + b"\x02" + subblocks(lzw_encode(px_a[::-1], 2))
    g += b"\x3B"
    write_gif("valid_local_palette.gif", g)

    # 4. valid_transparency_disposal.gif
    g = make_header(32, 32, gpal, bg=1) + make_netscape(2)
    g += make_gce(8, disposal=1, transparent=0) + make_image_desc(0, 0, 32, 32) + b"\x02" + subblocks(lzw_encode([(x // 4 + y // 4) % 4 for y in range(32) for x in range(32)], 2))
    g += make_gce(8, disposal=2, transparent=2) + make_image_desc(4, 5, 20, 18) + b"\x02" + subblocks(lzw_encode([(x + y) % 4 for y in range(18) for x in range(20)], 2))
    g += make_gce(8, disposal=2, transparent=3) + make_image_desc(8, 2, 16, 16) + b"\x02" + subblocks(lzw_encode([3 if (x + y) % 2 == 0 else 1 for y in range(16) for x in range(16)], 2))
    g += make_gce(8, disposal=0, transparent=1) + make_image_desc(0, 0, 32, 32) + b"\x02" + subblocks(lzw_encode([(x ^ y) % 4 for y in range(32) for x in range(32)], 2))
    g += b"\x3B"
    write_gif("valid_transparency_disposal.gif", g)

    # 5. valid_interlaced.gif (97x79 odd non-multiple-of-8 size, >300B)
    w, h = 97, 79
    rng_int = random.Random(99)
    px_interlaced = [rng_int.randrange(4) for _ in range(w * h)]
    g = make_header(w, h, gpal)
    g += make_image_desc(0, 0, w, h, interlaced=True) + b"\x02" + subblocks(lzw_encode(px_interlaced, 2)) + b"\x3B"
    write_gif("valid_interlaced.gif", g)

    # 6. valid_kwkwk.gif (ABABA & runs triggering repeated KwKwK across multiple widths, >300B)
    px_kw = []
    for i in range(1, 80):
        px_kw.extend([0] * i)
    for k in range(1, 40):
        px_kw.extend([1, 2] * k + [3] * (k % 7 + 1))
    rng_kw = random.Random(7)
    px_kw.extend(rng_kw.randrange(4) for _ in range(1200))
    w = 64
    h = (len(px_kw) + w - 1) // w
    px_kw.extend([2] * (w * h - len(px_kw)))
    g = make_header(w, h, gpal)
    g += make_image_desc(0, 0, w, h) + b"\x02" + subblocks(lzw_encode(px_kw, 2)) + b"\x3B"
    write_gif("valid_kwkwk.gif", g)

    # 7. valid_deferred_cc.gif (fill 12-bit dict to 4096 and keep emitting without CC)
    rng = random.Random(42)
    w, h = 200, 140
    # Mix pseudo-random pairs and runs so dictionary hits 4096 well before end of frame
    px_def = [rng.randrange(256) if (i % 7) else (i // 7) % 256 for i in range(w * h)]
    g = make_header(w, h, pal256)
    g += make_image_desc(0, 0, w, h) + b"\x08" + subblocks(lzw_encode(px_def, 8, deferred_cc=True)) + b"\x3B"
    write_gif("valid_deferred_cc.gif", g)

    # 8. valid_multi_cc.gif (multiple Clear Codes mid-stream)
    w, h = 80, 60
    px_mc = [(x * 3 + y * 5 + (x // 8) * (y // 8)) % 16 for y in range(h) for x in range(w)]
    pal16 = [((i * 17) % 256, (i * 53) % 256, (i * 97) % 256) for i in range(16)]
    g = make_header(w, h, pal16)
    g += make_image_desc(0, 0, w, h) + b"\x04" + subblocks(lzw_encode(px_mc, 4, emit_multi_cc=True)) + b"\x3B"
    write_gif("valid_multi_cc.gif", g)

    # 9. valid_subblock_boundary.gif (1, 2, 3, 255 byte chunks)
    w, h = 64, 64
    px_sb = [((x * x + y * 3) % 16) for y in range(h) for x in range(w)]
    g = make_header(w, h, pal16)
    g += make_image_desc(0, 0, w, h) + b"\x04" + subblocks(lzw_encode(px_sb, 4), chunk_sizes=[1, 2, 3, 255, 7, 13]) + b"\x3B"
    write_gif("valid_subblock_boundary.gif", g)

    # 10. valid_long_chain.gif (480x1600 all-zeros -> max chain length > 1200, every chain triggers KwKwK!)
    # Exceeds FILE_BUF_SIZE=768 to test chunked pixel expansion fallback!
    w, h = 480, 1600
    px_long = [0] * (w * h)
    g = make_header(w, h, gpal)
    g += make_image_desc(0, 0, w, h) + b"\x02" + subblocks(lzw_encode(px_long, 2)) + b"\x3B"
    write_gif("valid_long_chain.gif", g)

    # 11. edge_cc_eoi.gif (consecutive CC, CC, CC before pixel, and frame 2 with CC -> EOI)
    # Note: Pillow accepts consecutive CC and CC->EOI.
    bw = BitWriter()
    # minCodeSize=2: cc=4, eoi=5, width=3
    bw.write(4, 3)
    bw.write(4, 3)
    bw.write(4, 3)
    bw.write(1, 3)
    bw.write(2, 3)
    bw.write(4, 3)  # CC right before EOI
    bw.write(5, 3)  # EOI
    g = make_header(2, 1, gpal)
    g += make_image_desc(0, 0, 2, 1) + b"\x02" + subblocks(bw.finish()) + b"\x3B"
    write_gif("edge_cc_eoi.gif", g, verify_pil=True)

    # 12. edge_comment.gif (comment extension for getComment() test)
    comment = b"AnimatedGIF-LowMem test comment 1234567890!"
    g = make_header(4, 4, gpal)
    g += b"\x21\xFE" + bytes([len(comment)]) + comment + b"\x00"
    g += make_image_desc(0, 0, 4, 4) + b"\x02" + subblocks(lzw_encode([0, 1, 2, 3] * 4, 2)) + b"\x3B"
    write_gif("edge_comment.gif", g)

    # 13. edge_huge_comment.gif (multi-sub-block 510-byte comment extension)
    g = make_header(8, 8, gpal) + b"\x21\xFE\xFF" + (b"A" * 255) + b"\xFF" + (b"B" * 255) + b"\x00"
    g += make_image_desc(0, 0, 8, 8) + b"\x02" + subblocks(lzw_encode([0] * 64, 2)) + b"\x3B"
    write_gif("edge_huge_comment.gif", g)


def build_malformed():
    # Use a 2-color palette (iColorTableBits == 1) or >300B comment so upstream GIFParseInfo
    # reaches the actual malformed section instead of tripping its <267B small-file bug.
    gpal = [(0, 0, 0), (255, 255, 255)]
    base_valid = make_header(8, 8, gpal) + make_image_desc(0, 0, 8, 8) + b"\x02" + subblocks(lzw_encode([0] * 64, 2)) + b"\x3B"

    cases = {}
    cases["malformed_truncated_hdr.gif"] = b"GIF8"
    cases["malformed_zero_dims.gif"] = make_header(0, 10, gpal) + b"\x3B"
    cases["malformed_too_wide.gif"] = make_header(2049, 10, gpal) + b"\x3B"
    cases["malformed_oob_frame.gif"] = make_header(16, 16, gpal) + make_image_desc(10, 10, 10, 10) + b"\x02" + subblocks(lzw_encode([0] * 100, 2)) + b"\x3B"
    cases["malformed_no_palette.gif"] = make_header(8, 8, None) + make_image_desc(0, 0, 8, 8) + b"\x02" + subblocks(lzw_encode([0] * 64, 2)) + b"\x3B"

    for bad_mcs in (0, 1, 9, 255):
        cases[f"malformed_bad_mincodesize_{bad_mcs}.gif"] = (
            make_header(8, 8, gpal) + make_image_desc(0, 0, 8, 8) + bytes([bad_mcs]) + b"\x02\x44\x01\x00\x3B"
        )

    # First LZW code after CC is >= cc (e.g. code=7 when cc=4), triggering OOB/invalid code
    bw = BitWriter()
    bw.write(4, 3)
    bw.write(7, 3)  # invalid first code (> nextcode)
    bw.write(5, 3)
    cases["malformed_bad_first_code.gif"] = (
        make_header(8, 8, gpal) + make_image_desc(0, 0, 8, 8) + b"\x02" + subblocks(bw.finish()) + b"\x3B"
    )

    # Mid-stream code > nextcode (specifically triggers code in 4096..5910 in 12-bit or invalid code)
    bw = BitWriter()
    bw.write(4, 3)
    bw.write(0, 3)
    bw.write(7, 3)  # nextcode is 6; 7 > nextcode
    bw.write(5, 3)
    cases["malformed_code_gt_nextcode.gif"] = (
        make_header(8, 8, gpal) + make_image_desc(0, 0, 8, 8) + b"\x02" + subblocks(bw.finish()) + b"\x3B"
    )

    # Trigger code >= 4096 (< LINK_UNUSED=5911) if possible? Actually 12-bit code max is 4095!
    # Wait: what triggers LINK_UNUSED OOB in upstream?
    # In upstream, if code=0..4095 has giftabs[code] == LINK_UNUSED (0x1717 = 5911) or garbage (because memset sets bytes to 0x17 -> 0x1717 = 5911):
    # Wait: if code == nextcode on FIRST code after CC (code = cc + 2), or if code > nextcode (code = 4095 < LINK_UNUSED),
    # upstream reads giftabs[4095] = 0x1717 (if CC happened), OR if code > nextcode without CC and old stack garbage or if minCodeSize=8 sets codesize=9..12:
    # Even better: when ucCodeStart = 8 (cc = 256), nextcode = 258; if stream emits code = 4095 right away (width=12 after fake codes or width=9 code=511):
    # In upstream, giftabs[511] == 0x1717 == LINK_UNUSED, EXCEPT wait: what if ucCodeStart = 8 and code = 511, giftabs[511] IS 0x1717, so while(code < LINK_UNUSED) stops,
    # Wait: what if `ucCodeStart = 8` and `code = 256..511` was NOT reset? Wait: upstream resets `giftabs[cc..4095]` to `0x1717`, EXCEPT `giftabs[0..cc-1]` is `LINK_END=5912`!
    # What if an encoder sends `ucCodeStart = 9` (corrupt)? In upstream GIFParseInfo, ucCodeStart is NOT checked to be <= 8!
    # When ucCodeStart = 12 (`cc = 4096`!), `nextcode = 4098`, `memset(&giftabs[4096], ...)` writes OUT OF BOUNDS OF usGIFTable[4096]!
    # Let's include `malformed_ucCodeStart_12.gif` AND `malformed_truncated_subblock.gif` AND `malformed_getinfo_oob.gif`!
    cases["malformed_ucCodeStart_12.gif"] = (
        make_header(8, 8, gpal) + make_image_desc(0, 0, 8, 8) + b"\x0C\x02\x44\x01\x00\x3B"
    )

    # Sub-block length 255 but file ends after 5 bytes
    cases["malformed_truncated_subblock.gif"] = (
        make_header(8, 8, gpal) + make_image_desc(0, 0, 8, 8) + b"\x02\xFF\x84\x8F\xA9\xCB"
    )

    # Truncated mid-LZW (valid 4-byte subblock, no 0x00 terminator, no EOI)
    cases["malformed_truncated_mid_lzw.gif"] = (
        make_header(8, 8, gpal) + make_image_desc(0, 0, 8, 8) + b"\x02\x02\x04\x00"
    )

    # Bad GCE length
    cases["malformed_bad_gce.gif"] = (
        make_header(8, 8, gpal) + b"\x21\xF9\x03\x00\x0A\x00\x00" + make_image_desc(0, 0, 8, 8) + b"\x02\x02\x44\x01\x00\x3B"
    )

    # Trigger OOB in GIF_getInfo(): truncated inside extension / palette / subblock > remaining
    cases["malformed_getinfo_oob.gif"] = (
        b"GIF89a\x08\x00\x08\x00\xF7\x00\x00" + b"\x01\x02\x03"  # claims 256-color palette (768 bytes) but only 3 bytes exist!
    )

    # Unterminated comment / sub-block before image
    cases["malformed_unterminated_comment.gif"] = (
        make_header(8, 8, gpal) + b"\x21\xFE\x10" + b"short"
    )

    for name, data in cases.items():
        (OUT / name).write_bytes(data)

    # Sanity check: ensure base_valid isn't accidentally unused
    assert len(base_valid) > 20


if __name__ == "__main__":
    build_valid_and_edge()
    build_malformed()
    files = sorted(p.name for p in OUT.glob("*.gif"))
    print(f"Generated {len(files)} corpus GIFs in {OUT}:")
    for f in files:
        print(f"  {f} ({(OUT / f).stat().st_size} bytes)")
