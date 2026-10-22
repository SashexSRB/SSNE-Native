#!/usr/bin/env python3
"""Builds a GameCube test image (game_data files, junk-filled gaps) and encodes it as CISO, GCZ, WIA and RVZ
(written from the Dolphin format description) to test the launcher's readers.

usage: make_container_images.py <out_dir> [ciso gcz wia rvz]   (needs numpy, and libzstd.so.1 for RVZ/zstd)
Reads game files from $SSNE_GAME_DATA (default /home/claude/game_data). Then check each image with
`gctool verify <image>`: every SHA-1 must equal <out_dir>/plain.sha1 (CISO pads to its block size, so compare
`gctool extract` output for it instead). The images contain game data: never commit or share them."""
import bz2, ctypes, hashlib, lzma, os, random, struct, sys, zlib
import numpy as np

OUT = sys.argv[1] if len(sys.argv) > 1 else 'img'
os.makedirs(OUT, exist_ok=True)
GD = os.environ.get('SSNE_GAME_DATA', '/home/claude/game_data')
random.seed(7)

# ------------------------------------------------------------------ junk generator (LFG, as in Dolphin)
K, J = 521, 32
def lfg_block(seed_words):
    b = np.zeros(K, dtype=np.uint32)
    b[:17] = seed_words
    for i in range(17, K):
        b[i] = (int(b[i - 17]) << 23 & 0xFFFFFFFF) ^ (int(b[i - 16]) >> 9) ^ int(b[i - 1])
    b = (b & np.uint32(0xFF00FFFF)) | ((b >> np.uint32(2)) & np.uint32(0x00FF0000))
    def step(b):
        b[:J] ^= b[K - J:]
        for s in range(J, K, J):
            e = min(s + J, K)
            b[s:e] ^= b[s - J:e - J]
    for _ in range(4): step(b)
    out = bytearray()
    while len(out) < 0x8000:
        out += b.astype('>u4').tobytes()
        step(b)
    return bytes(out[:0x8000])

# ------------------------------------------------------------------ build the plain image
files = []
for root, dirs, fs in os.walk(GD + '/files'):
    dirs.sort()
    for f in sorted(fs):
        p = os.path.join(root, f)
        files.append(os.path.relpath(p, GD + '/files'))
files.sort(key=lambda s: s.lower())
boot = bytearray(open(GD + '/sys/boot.bin', 'rb').read())
bi2 = open(GD + '/sys/bi2.bin', 'rb').read()
app = open(GD + '/sys/apploader.img', 'rb').read()
dol = open(GD + '/sys/main.dol', 'rb').read()

# FST entries (directories + files in path order)
tree = {}
for f in files:
    parts = f.split('/')
    node = tree
    for d in parts[:-1]: node = node.setdefault(d + '/', {})
    node[parts[-1]] = f
entries, names = [], bytearray(b'\0')
def add(node, parent):
    for k in sorted(node, key=lambda s: s.rstrip('/').lower()):
        name_off = len(names); names.extend(k.rstrip('/').encode() + b'\0')
        if k.endswith('/'):
            idx = len(entries); entries.append(['d', name_off, parent, 0])
            add(node[k], idx)
            entries[idx][3] = len(entries)
        else:
            entries.append(['f', name_off, node[k], 0])
entries.append(['d', 0, 0, 0])
add(tree, 0)
entries[0][3] = len(entries)
fst_size = len(entries) * 12 + len(names)

dol_off = 0x10000
fst_off = (dol_off + len(dol) + 0x7FFF) & ~0x7FFF
pos = (fst_off + fst_size + 0x7FFF) & ~0x7FFF
layout = {}
junk_ranges = []  # (start, end) filled with junk
for i, e in enumerate(entries):
    if e[0] != 'f': continue
    gap = random.choice([0, 0, 1, 3, 17]) * 0x8000 + random.choice([0, 0x4000])
    pos += gap
    pos = (pos + 3) & ~3
    layout[e[2]] = pos
    pos += os.path.getsize(GD + '/files/' + e[2])
size = (pos + 0x3FFFFF) // 0x8000 * 0x8000 + 5 * 0x8000
img = bytearray(size)
struct.pack_into('>III', boot, 0x420, dol_off, fst_off, fst_size)
struct.pack_into('>I', boot, 0x42C, fst_size)
img[0:0x440] = boot
img[0x440:0x2440] = bi2
img[0x2440:0x2440 + len(app)] = app
img[dol_off:dol_off + len(dol)] = dol
fst = bytearray()
for e in entries:
    if e[0] == 'd': fst += struct.pack('>III', 0x01000000 | e[1], e[2], e[3])
    else: fst += struct.pack('>III', e[1], layout[e[2]], os.path.getsize(GD + '/files/' + e[2]))
fst += names
img[fst_off:fst_off + len(fst)] = fst
used = bytearray(size // 0x8000 + 1)  # 1 = block has real data somewhere
def mark(a, b):
    for blk in range(a // 0x8000, (b + 0x7FFF) // 0x8000): used[blk] = 1
mark(0, 0x2440 + len(app)); mark(dol_off, dol_off + len(dol)); mark(fst_off, fst_off + len(fst))
for f, off in layout.items():
    data = open(GD + '/files/' + f, 'rb').read()
    img[off:off + len(data)] = data
    mark(off, off + len(data))
# Junk: after the FST, every byte not covered by file data gets junk from its block's seed, except for a
# zero-filled stretch at the end (like trimmed/zeroed regions) and a few blocks left as zeros.
seeds = {}
cov = np.zeros(size, dtype=bool)
cov[:fst_off + len(fst)] = True
for f, off in layout.items(): cov[off:off + os.path.getsize(GD + '/files/' + f)] = True
zero_tail = size - 3 * 0x8000
nblk = 0
for blk in range((fst_off + len(fst)) // 0x8000, zero_tail // 0x8000):
    a = blk * 0x8000
    c = cov[a:a + 0x8000]
    if c.all(): continue
    if random.random() < 0.05 and not used[blk]: continue  # zero block
    seed = [random.getrandbits(32) for _ in range(17)]
    seeds[blk] = seed
    j = np.frombuffer(lfg_block(seed), dtype=np.uint8)
    blkv = np.frombuffer(bytes(img[a:a + 0x8000]), dtype=np.uint8).copy()
    blkv[~c] = j[~c]
    img[a:a + 0x8000] = blkv.tobytes()
    nblk += 1
open(OUT + '/plain.iso', 'wb').write(img)
print('plain image %d bytes, %d files, %d junk blocks, sha1 %s' % (size, len(layout), nblk, hashlib.sha1(img).hexdigest()))
open(OUT + '/plain.sha1', 'w').write(hashlib.sha1(img).hexdigest())

# ------------------------------------------------------------------ compressors
zs = ctypes.CDLL('libzstd.so.1')
zs.ZSTD_compressBound.restype = ctypes.c_size_t
zs.ZSTD_compress.restype = ctypes.c_size_t
def zstd(d, lvl=5):
    cap = zs.ZSTD_compressBound(ctypes.c_size_t(len(d)))
    buf = ctypes.create_string_buffer(cap)
    n = zs.ZSTD_compress(buf, ctypes.c_size_t(cap), bytes(d), ctypes.c_size_t(len(d)), ctypes.c_int(lvl))
    return buf.raw[:n]
LZMA_PROPS = bytes([(2 * 5 + 0) * 9 + 3]) + (1 << 24).to_bytes(4, 'little')
def comp(method, d):
    d = bytes(d)
    if method == 0: return d
    if method == 1:  # purge
        out = bytearray(); i = 0; n = len(d)
        arr = np.frombuffer(d, dtype=np.uint8)
        nz = np.flatnonzero(arr)
        # segments of non-zero data, merging gaps < 8 bytes
        if len(nz):
            start = nz[0]; prev = nz[0]
            for x in list(nz[1:]) + [None]:
                if x is None or x - prev > 8:
                    out += struct.pack('>II', start, prev + 1 - start) + d[start:prev + 1]
                    if x is not None: start = x
                if x is not None: prev = x
        return bytes(out) + hashlib.sha1(out).digest()
    if method == 2: return bz2.compress(d, 9)
    if method == 3: return lzma.compress(d, format=lzma.FORMAT_RAW, filters=[{'id': lzma.FILTER_LZMA1, 'lc': 3, 'lp': 0, 'pb': 2, 'dict_size': 1 << 24}])
    if method == 4: return lzma.compress(d, format=lzma.FORMAT_RAW, filters=[{'id': lzma.FILTER_LZMA2, 'preset': 6, 'dict_size': 1 << 24}])
    if method == 5: return zstd(d, 5)
def cdata(method):
    if method == 3: return LZMA_PROPS
    if method == 4: return bytes([24])  # 2^24 dictionary -> prop 24
    return b''

# ------------------------------------------------------------------ CISO
def write_ciso(path, bs):
    nb = (size + bs - 1) // bs
    m = bytearray(0x8000 - 8)
    body = bytearray()
    for b in range(nb):
        blk = img[b * bs:(b + 1) * bs]
        if any(blk):
            m[b] = 1; body += blk.ljust(bs, b'\0')
    open(path, 'wb').write(b'CISO' + struct.pack('<I', bs) + m + body)

# ------------------------------------------------------------------ GCZ
def write_gcz(path, bs):
    nb = (size + bs - 1) // bs
    ptrs, hashes, data = [], [], bytearray()
    for b in range(nb):
        blk = bytes(img[b * bs:(b + 1) * bs]).ljust(bs, b'\0')
        c = zlib.compress(blk, 9)
        if len(c) >= bs:
            ptrs.append(len(data) | (1 << 63)); data += blk; hashes.append(zlib.adler32(blk))
        else:
            ptrs.append(len(data)); data += c; hashes.append(zlib.adler32(c))
    hdr = struct.pack('<IIQQII', 0xB10BC001, 0, len(data), size, bs, nb)
    open(path, 'wb').write(hdr + b''.join(struct.pack('<Q', p) for p in ptrs) + b''.join(struct.pack('<I', h) for h in hashes) + data)

# ------------------------------------------------------------------ WIA / RVZ
def rvz_pack(start, length):
    """Packed representation of img[start:start+length]: runs of data and junk seeds."""
    out = bytearray(); p = start; end = start + length; any_junk = False
    while p < end:
        blk = p // 0x8000
        bend = min(end, (blk + 1) * 0x8000)
        if blk in seeds:
            # split the block into covered (data) / uncovered (junk) runs
            c = cov[p:bend]
            q = p
            while q < bend:
                v = c[q - p]
                r = q
                idx = np.flatnonzero(c[q - p:] != v)
                r = bend if len(idx) == 0 else q + int(idx[0])
                if v:
                    out += struct.pack('>I', r - q) + img[q:r]
                else:
                    any_junk = True
                    out += struct.pack('>I', 0x80000000 | (r - q)) + b''.join(struct.pack('>I', w) for w in seeds[blk])
                q = r
        else:
            out += struct.pack('>I', bend - p) + img[p:bend]
        p = bend
    return bytes(out), any_junk

def write_wia(path, rvz, method, chunk, raw_uncompressed_if_bigger=True):
    data_start = 0x80
    raw_off, raw_size = data_start, size - data_start
    groups_n = (size + chunk - 1) // chunk
    header_space = 0x48 + 0xDC
    body = bytearray()
    body_base = 0x1000  # tables go after the groups; body starts at 0x1000
    groups = []
    dedup = {}
    for g in range(groups_n):
        a = g * chunk
        raw = bytes(img[a:min(a + chunk, size)])
        if not any(raw):
            groups.append((0, 0, 0)); continue
        packed_size = 0
        payload = raw
        if rvz:
            pk, junk = rvz_pack(a, len(raw))
            if junk: payload, packed_size = pk, len(pk)
        c = comp(method, payload)
        compressed = method != 0
        if rvz and method != 0 and len(c) >= len(payload) and raw_uncompressed_if_bigger:
            c, compressed = payload, False
        key = c
        if key in dedup:
            off = dedup[key]
        else:
            while (body_base + len(body)) % 4: body += b'\0'
            off = body_base + len(body); body += c; dedup[key] = off
        sz = len(c) | (0x80000000 if (rvz and compressed) else 0)
        groups.append((off >> 2, sz, packed_size))
    while (body_base + len(body)) % 4: body += b'\0'
    raw_tab = struct.pack('>QQII', raw_off, raw_size, 0, groups_n)
    grp_tab = b''.join(struct.pack('>III', *g) if rvz else struct.pack('>II', *g[:2]) for g in groups)
    raw_c, grp_c = comp(method, raw_tab), comp(method, grp_tab)
    raw_tab_off = body_base + len(body); body += raw_c
    while (body_base + len(body)) % 4: body += b'\0'
    grp_tab_off = body_base + len(body); body += grp_c
    cd = cdata(method)
    level = {0: 0, 1: 0, 2: 9, 3: 6, 4: 6, 5: 5}[method]
    disc = struct.pack('>IIiI', 1, method, level, chunk) + bytes(img[:0x80]) + struct.pack('>IIQ', 0, 0, 0) + bytes(20)
    disc += struct.pack('>IQI', 1, raw_tab_off, len(raw_c)) + struct.pack('>IQI', groups_n, grp_tab_off, len(grp_c))
    disc += bytes([len(cd)]) + cd.ljust(7, b'\0')
    assert len(disc) == 0xDC, hex(len(disc))
    total = body_base + len(body)
    head = (b'RVZ\x01' if rvz else b'WIA\x01') + struct.pack('>III', 0x01000000, 0x00030000 if rvz else 0x00090000, len(disc))
    head += hashlib.sha1(disc).digest() + struct.pack('>QQ', size, total)
    head += hashlib.sha1(head).digest()
    assert len(head) == 0x48
    f = bytearray(head + disc)
    f = f.ljust(body_base, b'\0') + body
    open(path, 'wb').write(f)

which = sys.argv[2:] or ['ciso', 'gcz', 'wia', 'rvz']
if 'ciso' in which: write_ciso(OUT + '/t.ciso', 2 << 20)
if 'gcz' in which: write_gcz(OUT + '/t.gcz', 32 << 10)
names_m = {0: 'none', 1: 'purge', 2: 'bzip2', 3: 'lzma', 4: 'lzma2', 5: 'zstd'}
if 'wia' in which:
    for m in (0, 1, 2, 3, 4):
        write_wia(OUT + '/t-%s.wia' % names_m[m], False, m, 2 << 20)
if 'rvz' in which:
    for m, ch in ((5, 128 << 10), (5, 2 << 20), (0, 128 << 10), (2, 512 << 10), (3, 128 << 10), (4, 256 << 10), (5, 32 << 10)):
        write_wia(OUT + '/t-%s-%dk.rvz' % (names_m[m], ch >> 10), True, m, ch)
for f in sorted(os.listdir(OUT)): print('%-28s %11d' % (f, os.path.getsize(OUT + '/' + f)))
