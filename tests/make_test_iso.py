#!/usr/bin/env python3
"""Builds synthetic GameCube disc images for tests (no game data involved).

  test.iso   - boot title "Serious Sam (synthetic test image)", contains a hand-assembled
               PowerPC main.dol that exercises branches, loops, rotates, CR logic, floats,
               indirect calls, a switch jump table and paired-single loads.
  wrong.iso  - a valid disc for a different game (must be rejected).
  junk.bin   - not a disc at all.
"""
import struct, sys, os

TEXT, DATA, RESULT = 0x80003100, 0x80200000, 0x80100000

# ---------------------------------------------------------------- tiny PPC assembler
def D(op, d, a, imm): return (op << 26) | (d << 21) | (a << 16) | (imm & 0xFFFF)
def X(d, a, b, xo, rc=0): return (31 << 26) | (d << 21) | (a << 16) | (b << 11) | (xo << 1) | rc
def spr(n): return ((n & 31) << 5) | (n >> 5)
li    = lambda d, v: D(14, d, 0, v)
lis   = lambda d, v: D(15, d, 0, v)
addi  = lambda d, a, v: D(14, d, a, v)
ori   = lambda a, s, v: D(24, s, a, v)
cmpwi = lambda a, v, cr=0: (11 << 26) | (cr << 23) | (a << 16) | (v & 0xFFFF)
cmplwi= lambda a, v, cr=0: (10 << 26) | (cr << 23) | (a << 16) | (v & 0xFFFF)
stw   = lambda s, off, a: D(36, s, a, off)
lwz   = lambda d, off, a: D(32, d, a, off)
lfs   = lambda d, off, a: D(48, d, a, off)
stfs  = lambda s, off, a: D(52, s, a, off)
add   = lambda d, a, b: X(d, a, b, 266)
mr    = lambda a, s: X(s, a, s, 444)
lwzx  = lambda d, a, b: X(d, a, b, 23)
mtctr = lambda s: (31 << 26) | (s << 21) | (spr(9) << 11) | (467 << 1)
mflr  = lambda d: (31 << 26) | (d << 21) | (spr(8) << 11) | (339 << 1)
mtlr  = lambda s: (31 << 26) | (s << 21) | (spr(8) << 11) | (467 << 1)
mtgqr = lambda s, n: (31 << 26) | (s << 21) | (spr(912 + n) << 11) | (467 << 1)
rlwinm= lambda a, s, sh, mb, me: (21 << 26) | (s << 21) | (a << 16) | (sh << 11) | (mb << 6) | (me << 1)
fmuls = lambda d, a, c: (59 << 26) | (d << 21) | (a << 16) | (c << 6) | (25 << 1)
psq_l = lambda d, off, a, w, i: (56 << 26) | (d << 21) | (a << 16) | (w << 15) | (i << 12) | (off & 0xFFF)
ps_add= lambda d, a, b: (4 << 26) | (d << 21) | (a << 16) | (b << 11) | (21 << 1)
ps_merge10 = lambda d, a, b: (4 << 26) | (d << 21) | (a << 16) | (b << 11) | (592 << 1)
blr   = lambda: 0x4E800020
bctr  = lambda: 0x4E800420
bctrl = lambda: 0x4E800421

class Asm:
    def __init__(self, base): self.base, self.code, self.labels, self.fix = base, [], {}, []
    def pc(self): return self.base + 4 * len(self.code)
    def L(self, name): self.labels[name] = self.pc()
    def __call__(self, *ws): self.code.extend(ws)
    def b(self, name, lk=0): self.fix.append((len(self.code), 'b', name, lk)); self.code.append(0)
    def bc(self, bo, bi, name): self.fix.append((len(self.code), 'bc', name, (bo, bi))); self.code.append(0)
    def la(self, reg, name):  # lis/ori of a label address
        self.fix.append((len(self.code), 'la', name, reg)); self.code += [0, 0]
    def link(self):
        for idx, kind, name, extra in self.fix:
            t = self.labels[name]; here = self.base + 4 * idx
            if kind == 'b': self.code[idx] = (18 << 26) | ((t - here) & 0x03FFFFFC) | extra
            elif kind == 'bc': bo, bi = extra; self.code[idx] = (16 << 26) | (bo << 21) | (bi << 16) | ((t - here) & 0xFFFC)
            else: self.code[idx] = lis(extra, t >> 16); self.code[idx + 1] = ori(extra, extra, t & 0xFFFF)
        return b''.join(struct.pack('>I', w) for w in self.code)

def build_program():
    a = Asm(TEXT)
    JT = DATA + 0x100   # jump table lives in data
    a.L('entry')
    a(mflr(31))
    a(lis(3, RESULT >> 16))                       # r3 = result base
    a(li(4, 10)); a.b('fib', 1); a(stw(5, 0, 3))  # [0] fib(10) = 55
    a(li(6, 100), mtctr(6), li(7, 0), li(8, 1))
    a.L('loop'); a(add(7, 7, 8), addi(8, 8, 1)); a.bc(16, 0, 'loop')   # bdnz
    a(stw(7, 4, 3))                               # [1] sum 1..100 = 5050
    a(lis(9, 0x1234), ori(9, 9, 0x5678), rlwinm(10, 9, 8, 24, 31), stw(10, 8, 3))  # [2] 0x12
    a(rlwinm(10, 9, 0, 28, 3), stw(10, 12, 3))    # [3] wrap mask 0xF000000F -> 0x10000008
    a(cmpwi(7, 5000), li(11, 2)); a.bc(4, 1, 'skip'); a(li(11, 1))      # ble
    a.L('skip'); a(stw(11, 16, 3))                # [4] 1
    a(lis(12, DATA >> 16), lfs(1, 0, 12), lfs(2, 4, 12), fmuls(3, 1, 2), stfs(3, 20, 3))  # [5] 6.0f
    a.la(12, 'double_it'); a(mtctr(12), li(4, 21), bctrl(), stw(4, 24, 3))            # [6] 42
    # switch(2) via jump table -> 300
    a(li(0, 2), cmplwi(0, 3)); a.bc(12, 1, 'sw_def')                    # bgt default
    a(lis(12, JT >> 16), addi(12, 12, JT & 0xFFFF), rlwinm(0, 0, 2, 0, 29), lwzx(0, 12, 0), mtctr(0), bctr())
    a.L('c0'); a(li(13, 100)); a.b('sw_end')
    a.L('c1'); a(li(13, 200)); a.b('sw_end')
    a.L('c2'); a(li(13, 300)); a.b('sw_end')
    a.L('c3'); a(li(13, 400)); a.b('sw_end')
    a.L('sw_def'); a(li(13, 999))
    a.L('sw_end'); a(stw(13, 28, 3))              # [7] 300
    # paired singles: GQR0=0 (float). load (1.5, 4.0), add to itself -> (3.0, 8.0), merge10 -> ps0=8.0
    a(lis(12, DATA >> 16), li(0, 0), mtgqr(0, 0), psq_l(4, 0, 12, 0, 0), ps_add(5, 4, 4), ps_merge10(6, 5, 5), stfs(6, 32, 3))  # [8] 8.0f
    a(mtlr(31), blr())
    a.L('fib'); a(li(5, 0), li(6, 1), mtctr(4))
    a.L('fl'); a(add(7, 5, 6), mr(5, 6), mr(6, 7)); a.bc(16, 0, 'fl'); a(blr())
    a.L('double_it'); a(add(4, 4, 4), blr())
    text = a.link()
    data = bytearray(0x200)
    data[0:8] = struct.pack('>ff', 1.5, 4.0)
    for k, lab in enumerate(['c0', 'c1', 'c2', 'c3']):
        struct.pack_into('>I', data, 0x100 + 4 * k, a.labels[lab])
    return text, bytes(data)

def build_dol(text, data):
    hdr = bytearray(0x100)
    t_off = 0x100; d_off = t_off + len(text)
    struct.pack_into('>I', hdr, 0x00, t_off); struct.pack_into('>I', hdr, 0x48, TEXT); struct.pack_into('>I', hdr, 0x90, len(text))
    struct.pack_into('>I', hdr, 0x1C, d_off); struct.pack_into('>I', hdr, 0x64, DATA); struct.pack_into('>I', hdr, 0xAC, len(data))
    struct.pack_into('>I', hdr, 0xD8, 0x80300000); struct.pack_into('>I', hdr, 0xDC, 0x1000)
    struct.pack_into('>I', hdr, 0xE0, TEXT)
    return bytes(hdr) + text + data

def build_iso(game_id, title, dol, files):
    img = bytearray(0x80000)
    img[0:6] = game_id.encode(); img[7] = 1
    struct.pack_into('>I', img, 0x1C, 0xC2339F3D)
    t = title.encode(); img[0x20:0x20 + len(t)] = t
    struct.pack_into('>I', img, 0x2440 + 0x14, 0x20)      # apploader body size
    dol_off, fst_off = 0x10000, 0x8000
    img[dol_off:dol_off + len(dol)] = dol
    # FST: root, dir "Levels" containing a.txt, then readme.txt at root
    names = b''; entries = []
    def name(n):
        nonlocal names; o = len(names); names += n.encode() + b'\0'; return o
    file_off = 0x20000; blobs = []
    entries.append((1, 0, 0, None))                       # root placeholder
    for path, content in files:
        if '/' in path:
            d, f = path.split('/')
            entries.append((1, name(d), 0, 'DIR'))        # parent 0, next fixed below
            di = len(entries) - 1
            entries.append((0, name(f), file_off, len(content))); blobs.append((file_off, content)); file_off += (len(content) + 31) & ~31
            entries[di] = (1, entries[di][1], 0, len(entries))
        else:
            entries.append((0, name(path), file_off, len(content))); blobs.append((file_off, content)); file_off += (len(content) + 31) & ~31
    entries[0] = (1, 0, 0, len(entries))
    fst = b''.join(struct.pack('>III', (t << 24) | n, o, s) for t, n, o, s in entries) + names
    img[fst_off:fst_off + len(fst)] = fst
    struct.pack_into('>III', img, 0x420, dol_off, fst_off, len(fst))
    for off, content in blobs: img[off:off + len(content)] = content
    return bytes(img)

if __name__ == '__main__':
    out = sys.argv[1] if len(sys.argv) > 1 else '.'
    os.makedirs(out, exist_ok=True)
    dol = build_dol(*build_program())
    files = [('Levels/test.ssw', b'synthetic level'), ('readme.txt', b'hello from a synthetic disc\n')]
    open(os.path.join(out, 'test.iso'), 'wb').write(build_iso('G3BZ00', 'Serious Sam (synthetic test image)', dol, files))
    open(os.path.join(out, 'wrong.iso'), 'wb').write(build_iso('GXXE01', 'Some Other Game', dol, files))
    open(os.path.join(out, 'junk.bin'), 'wb').write(b'\x00' * 4096)
    open(os.path.join(out, 'test.dol'), 'wb').write(dol)
    print('wrote test images to', out)
