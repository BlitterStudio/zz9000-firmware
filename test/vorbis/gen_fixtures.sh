#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Regenerate the Ogg Vorbis fixtures under fixtures/ and vorbis_fixtures.h.
# The encoder is ffmpeg's libvorbis wrapper; the reference decoder is
# ffmpeg's native floating-point Vorbis decoder (independent of libvorbis
# and Tremor), converted to signed 16-bit big-endian. Input PCM is a
# deterministic synthetic signal. Re-paginated, chained and malformed
# streams are written by the Ogg muxer below from the encoded packets.
# The tool versions used are recorded in vorbis_fixtures.h.
set -euo pipefail
cd "$(dirname "$0")"
command -v ffmpeg >/dev/null || { echo "ffmpeg required" >&2; exit 1; }
mkdir -p fixtures
LIBVORBIS_VERSION="$(dpkg-query -W -f='${Version}' libvorbisenc2 2>/dev/null || echo unknown)"
python3 - "$(ffmpeg -hide_banner -version | head -1 | cut -d' ' -f1-3)" \
         "$LIBVORBIS_VERSION" <<'PY'
import math, os, struct, subprocess, sys, tempfile

ffmpeg_version, libvorbis_version = sys.argv[1], sys.argv[2]
tmp = tempfile.mkdtemp()
FX = 'fixtures'

def run(args, data=None):
    return subprocess.run(args, input=data, stdout=subprocess.PIPE,
                          stderr=subprocess.DEVNULL, check=True).stdout

def pcm(frames, ch, rate, seed):
    out = bytearray()
    x = seed
    for i in range(frames):
        t = i / rate
        for c in range(ch):
            x = (x * 1103515245 + 12345) & 0x7fffffff
            f = 220.0 * (c + 1) + 1800.0 * t
            v = 9000 * math.sin(2 * math.pi * f * t) + 4000 * math.sin(2 * math.pi * 3150.0 * t)
            v += ((x >> 8) % 1536) - 768
            if i == 1000:
                v = 32767
            out += struct.pack('<h', max(-32768, min(32767, int(v))))
    return bytes(out)

def raw_args(rate, ch):
    return ['-f', 's16le', '-ar', str(rate), '-ac', str(ch), '-i', '-']

# 50 ms Ogg pages, so every fixture has several audio pages.
BITEXACT = ['-fflags', '+bitexact', '-flags:a', '+bitexact', '-map_metadata', '-1',
            '-page_duration', '50000']

def encode(name, frames, ch, rate, q, codec='libvorbis', extra=()):
    path = os.path.join(tmp, name + '.ogg')
    run(['ffmpeg', '-v', 'error', '-y'] + raw_args(rate, ch) +
        ['-c:a', codec] + list(extra) + (['-q:a', str(q)] if q is not None else []) +
        BITEXACT + ['-f', 'ogg', path], pcm(frames, ch, rate, len(name)))
    return open(path, 'rb').read()

def reference(data, frames, ch):
    """ffmpeg's ogg demuxer does not apply Vorbis end trimming: decode, check
    the EOS granule equals the input length, keep exactly that many frames
    (the head is aligned: both decoders drop the first packet's output)."""
    path = os.path.join(tmp, 'ref.ogg')
    open(path, 'wb').write(data)
    ref = run(['ffmpeg', '-v', 'error', '-c:a', 'vorbis', '-i', path,
               '-f', 's16be', '-acodec', 'pcm_s16be', '-'])
    last = list(pages(data))[-1]
    assert last['granule'] == frames and last['flags'] & 4, (last['granule'], frames)
    assert len(ref) >= frames * ch * 2, (len(ref), frames * ch * 2)
    return ref[:frames * ch * 2]

# ---- Ogg pages -------------------------------------------------------------
CRC = []
for i in range(256):
    r = i << 24
    for _ in range(8):
        r = ((r << 1) ^ 0x04c11db7) if r & 0x80000000 else (r << 1)
    CRC.append(r & 0xffffffff)

def crc32(data):
    c = 0
    for b in data:
        c = ((c << 8) & 0xffffffff) ^ CRC[((c >> 24) ^ b) & 0xff]
    return c

def page(serial, seq, granule, flags, lacing, body):
    hdr = struct.pack('<4sBBqIIIB', b'OggS', 0, flags, granule, serial, seq, 0,
                      len(lacing)) + bytes(lacing)
    p = bytearray(hdr + body)
    struct.pack_into('<I', p, 22, crc32(p))
    return bytes(p)

def pages(data):
    off = 0
    while off < len(data):
        assert data[off:off + 4] == b'OggS'
        n = data[off + 26]
        lac = data[off + 27:off + 27 + n]
        size = 27 + n + sum(lac)
        yield dict(raw=data[off:off + size], flags=data[off + 5],
                   granule=struct.unpack_from('<q', data, off + 6)[0],
                   serial=struct.unpack_from('<I', data, off + 14)[0],
                   lacing=lac, body=data[off + 27 + n:off + size])
        off += size

def packets(data):
    out, cur = [], b''
    for pg in pages(data):
        p = 0
        for l in pg['lacing']:
            cur += pg['body'][p:p + l]
            p += l
            if l < 255:
                out.append(cur)
                cur = b''
    return out, list(pages(data))

# ---- Vorbis setup parsing (mode block flags for granule positions) --------
class Bits:
    def __init__(self, data):
        self.d, self.pos = data, 0
    def read(self, n):
        v = 0
        for i in range(n):
            byte = self.d[self.pos >> 3]
            v |= ((byte >> (self.pos & 7)) & 1) << i
            self.pos += 1
        return v

def ilog(v):
    r = 0
    while v:
        r += 1
        v >>= 1
    return r

def lookup1(entries, dim):
    r = int(entries ** (1.0 / dim))
    while (r + 1) ** dim <= entries:
        r += 1
    while r ** dim > entries:
        r -= 1
    return r

def setup_modes(setup, channels):
    b = Bits(setup)
    assert b.read(8) == 5 and bytes(b.read(8) for _ in range(6)) == b'vorbis'
    for _ in range(b.read(8) + 1):
        assert b.read(24) == 0x564342
        dim, entries = b.read(16), b.read(24)
        if b.read(1):
            i = 0
            b.read(5)
            while i < entries:
                i += b.read(ilog(entries - i))
        else:
            sparse = b.read(1)
            for _ in range(entries):
                if not sparse or b.read(1):
                    b.read(5)
        maptype = b.read(4)
        if maptype:
            b.read(32); b.read(32)
            qbits = b.read(4) + 1
            b.read(1)
            vals = lookup1(entries, dim) if maptype == 1 else entries * dim
            b.read(vals * qbits)
    for _ in range(b.read(6) + 1):
        b.read(16)
    for _ in range(b.read(6) + 1):
        ftype = b.read(16)
        if ftype == 0:
            b.read(8); b.read(16); b.read(16); b.read(6); b.read(8)
            for _ in range(b.read(4) + 1):
                b.read(8)
        else:
            parts = [b.read(4) for _ in range(b.read(5))]
            dims = []
            for _ in range(max(parts) + 1 if parts else 0):
                dims.append(b.read(3) + 1)
                subs = b.read(2)
                if subs:
                    b.read(8)
                for _ in range(1 << subs):
                    b.read(8)
            b.read(2)
            rangebits = b.read(4)
            for p in parts:
                b.read(dims[p] * rangebits)
    for _ in range(b.read(6) + 1):
        b.read(16); b.read(24); b.read(24); b.read(24)
        classes = b.read(6) + 1
        b.read(8)
        cascades = []
        for _ in range(classes):
            low = b.read(3)
            cascades.append(low | (b.read(5) << 3 if b.read(1) else 0))
        for c in cascades:
            b.read(8 * bin(c).count('1'))
    for _ in range(b.read(6) + 1):
        assert b.read(16) == 0
        submaps = b.read(4) + 1 if b.read(1) else 1
        if b.read(1):
            for _ in range(b.read(8) + 1):
                b.read(2 * ilog(channels - 1))
        assert b.read(2) == 0
        if submaps > 1:
            b.read(4 * channels)
        b.read(24 * submaps)
    modes = []
    for _ in range(b.read(6) + 1):
        modes.append(b.read(1))
        b.read(16); b.read(16); b.read(8)
    assert b.read(1) == 1
    return modes

def granules(pkts, src_pages):
    """End sample of every audio packet (libvorbis lapping rule)."""
    ident = pkts[0]
    ch = ident[11]
    bs = (1 << (ident[28] & 15), 1 << (ident[28] >> 4))
    modes = setup_modes(pkts[2], ch)
    mbits = ilog(len(modes) - 1)
    out, prev, pos = [0, 0, 0], None, 0
    for p in pkts[3:]:
        cur = bs[modes[Bits(p).read(1 + mbits) >> 1]] if p else 0
        if prev is not None:
            pos += prev // 4 + cur // 4
        prev = cur
        out.append(pos)
    out[-1] = src_pages[-1]['granule']    # end trimming
    return out

def mux(pkts, grans, serial, max_body=4096, max_segs=255, first_seq=0):
    """Paginate packets: ID header alone on the BOS page, audio from a fresh
    page, packets split across pages wherever the limits fall."""
    out, seq = [], first_seq
    lacing, body, gran = [], b'', -1
    flags_cont = 0
    def flush(eos=False):
        nonlocal lacing, body, gran, seq, flags_cont
        flags = flags_cont | (2 if seq == first_seq else 0) | (4 if eos else 0)
        out.append(page(serial, seq, gran, flags, lacing, body))
        flags_cont = 1 if lacing and lacing[-1] == 255 else 0
        seq += 1
        lacing, body, gran = [], b'', -1
    for i, p in enumerate(pkts):
        segs = [255] * (len(p) // 255) + [len(p) % 255]
        off = 0
        for s in segs:
            if len(lacing) == max_segs or len(body) + s > max_body:
                flush()
            lacing.append(s)
            body += p[off:off + s]
            off += s
        gran = grans[i]
        if i == 0 or i == 2 or i == len(pkts) - 1:
            flush(eos=i == len(pkts) - 1)
    return b''.join(out)

def write(name, data):
    open(os.path.join(FX, name), 'wb').write(data)

# ---- Fixtures --------------------------------------------------------------
positive = []
encoded = {}
for name, frames, ch, rate, q in [
        ('mono8k', 4000, 1, 8000, 0),
        ('mono22k', 8820, 1, 22050, 3),
        ('stereo44k', 22050, 2, 44100, 3),
        ('stereo48k_q10', 14400, 2, 48000, 10)]:
    data = encode(name, frames, ch, rate, q)
    pk, pg = packets(data)
    g = granules(pk, pg)
    # The granule rule must reproduce the encoder's page granules.
    for p_ in pg[:-1]:
        assert p_['granule'] in g or p_['granule'] == -1, (name, p_['granule'])
    ref = reference(data, frames, ch)
    write(name + '.ogg', data)
    write(name + '.s16', ref)
    encoded[name] = (data, pk, pg, g, ref)
    ident = pk[0]
    positive.append((name, name, frames, ch, rate,
                     1 << (ident[28] & 15), 1 << (ident[28] >> 4), len(pk[2])))

s_data, s_pk, s_pg, s_g, s_ref = encoded['stereo44k']
S = s_pg[0]['serial']

# Same packets, tiny pages: every setup/audio packet spans pages.
split = mux(s_pk, s_g, S, max_body=61, max_segs=3)
assert reference(split, 22050, 2) == s_ref
write('stereo44k_split.ogg', split)
positive.append(('stereo44k_split', 'stereo44k', 22050, 2, 44100, 256, 2048, len(s_pk[2])))

# A 192 KiB comment header (cover-art sized) on maximum-size pages.
art = b'METADATA_BLOCK_PICTURE=' + b'QUFB' * 49152
comment = (b'\x03vorbis' + struct.pack('<I', 6) + b'zz9000' + struct.pack('<I', 2) +
           struct.pack('<I', 11) + b'TITLE=Cover' + struct.pack('<I', len(art)) + art + b'\x01')
big = mux([s_pk[0], comment] + s_pk[2:], s_g, S, max_body=255 * 255)
assert reference(big, 22050, 2) == s_ref
write('stereo44k_bigcomment.ogg', big)
positive.append(('stereo44k_bigcomment', 'stereo44k', 22050, 2, 44100, 256, 2048, len(s_pk[2])))

# Trailing non-Ogg bytes after EOS (an ID3v1-style tag) are ignored.
write('stereo44k_trailing.ogg', s_data + b'TAG' + bytes(125))
positive.append(('stereo44k_trailing', 'stereo44k', 22050, 2, 44100, 256, 2048, len(s_pk[2])))

# Negative streams.
def flip(data, offset):
    d = bytearray(data)
    d[offset] ^= 0x40
    return bytes(d)

mid = s_pg[len(s_pg) // 2]
mid_off = s_data.index(mid['raw'])
write('neg_badcrc.ogg', flip(s_data, mid_off + len(mid['raw']) - 10))
write('neg_nocomment.ogg', mux([s_pk[0], s_pk[2]] + s_pk[3:], [0, 0] + s_g[3:], S))
write('neg_swapped.ogg', mux([s_pk[0], s_pk[2], s_pk[1]] + s_pk[3:], s_g, S))
write('neg_setupfirst.ogg', mux([s_pk[2], s_pk[1], s_pk[0]] + s_pk[3:], s_g, S))
write('neg_bigsetup.ogg', mux([s_pk[0], s_pk[1], s_pk[2] + bytes(70000)] + s_pk[3:], s_g, S))
bad_comment = b'\x03vorbis' + struct.pack('<I', 1000) + b'short' + b'\x01'
write('neg_badcomment.ogg', mux([s_pk[0], bad_comment] + s_pk[2:], s_g, S))
write('neg_badpacket.ogg', mux(s_pk[:20] + [s_pk[0]] + s_pk[20:], s_g[:20] + [s_g[19]] + s_g[20:], S))
write('neg_truncated_mid.ogg', s_data[:-100])
write('neg_truncated_page.ogg', s_data[:s_data.index(s_pg[-1]['raw'])])
gap = b''.join(p['raw'] for i, p in enumerate(s_pg) if i != len(s_pg) // 2)
write('neg_gap.ogg', gap)
ident = bytearray(s_pk[0])
struct.pack_into('<I', ident, 12, 7999)
write('neg_rate7999.ogg', mux([bytes(ident)] + s_pk[1:], s_g, S))

# A setup header whose single codebook has 16384 used entries (ordered,
# all 14 bits long): legal, but beyond SDK_VORBIS_MAX_BOOK_USED.
class W:
    def __init__(self):
        self.bits = []
    def put(self, v, n):
        self.bits += [(v >> i) & 1 for i in range(n)]
    def bytes(self):
        b = self.bits + [0] * (-len(self.bits) % 8)
        return bytes(sum(b[i + j] << j for j in range(8)) for i in range(0, len(b), 8))
w = W()
for c in b'\x05vorbis':
    w.put(c, 8)
w.put(0, 8)                                   # 1 codebook
w.put(0x564342, 24); w.put(1, 16); w.put(16384, 24)
w.put(1, 1); w.put(13, 5); w.put(16384, 15)   # ordered: 16384 x 14 bits
w.put(0, 4)                                   # no value mapping
w.put(0, 6); w.put(0, 16)                     # 1 time, type 0
w.put(0, 6); w.put(1, 16)                     # 1 floor, type 1
w.put(0, 5); w.put(0, 2); w.put(4, 4)         # no partitions, mult 1, rangebits 4
w.put(0, 6); w.put(0, 16)                     # 1 residue, type 0
w.put(0, 24); w.put(0, 24); w.put(0, 24); w.put(0, 6); w.put(0, 8)
w.put(0, 3); w.put(0, 1)                      # cascade 0
w.put(0, 6); w.put(0, 16)                     # 1 mapping, type 0
w.put(0, 1); w.put(0, 1); w.put(0, 2)         # 1 submap, no coupling
w.put(0, 8); w.put(0, 8); w.put(0, 8)
w.put(0, 6); w.put(0, 1); w.put(0, 16); w.put(0, 16); w.put(0, 8)  # 1 mode
w.put(1, 1)                                   # framing
write('neg_bigbook.ogg', mux([s_pk[0], s_pk[1], w.bytes()] + s_pk[3:], s_g, S))

# Chained: stereo44k, then a second link (mono22k) with another serial.
m_data = encoded['mono22k'][0]
assert encoded['mono22k'][2][0]['serial'] == S
m_pk, m_pg, m_g = encoded['mono22k'][1], encoded['mono22k'][2], encoded['mono22k'][3]
write('neg_chained.ogg', s_data + mux(m_pk, m_g, S + 1))

# Multiplexed: two Vorbis streams in one physical stream.
a, b = os.path.join(tmp, 'a.raw'), os.path.join(tmp, 'b.raw')
open(a, 'wb').write(pcm(8000, 2, 44100, 1))
open(b, 'wb').write(pcm(8000, 1, 44100, 2))
mux_path = os.path.join(tmp, 'mux.ogg')
run(['ffmpeg', '-v', 'error', '-y'] + raw_args(44100, 2)[:-1] + [a] + raw_args(44100, 1)[:-1] + [b] +
    ['-map', '0', '-map', '1', '-c:a', 'libvorbis', '-q:a', '3'] + BITEXACT + ['-f', 'ogg', mux_path])
write('neg_multiplexed.ogg', open(mux_path, 'rb').read())

write('neg_opus.ogg', encode('opus', 4800, 2, 48000, None, codec='libopus'))
write('neg_oggflac.ogg', encode('oggflac', 4000, 2, 44100, None, codec='flac'))
write('neg_3ch.ogg', encode('ch3', 4000, 3, 44100, 3))

lines = ['/* Generated by gen_fixtures.sh. Do not edit.',
         ' * Encoder: %s with libvorbis %s (-c:a libvorbis).' % (ffmpeg_version, libvorbis_version),
         ' * Reference: the same ffmpeg\'s native floating-point Vorbis decoder,',
         ' * converted to s16be by libswresample.',
         ' * SPDX-License-Identifier: GPL-3.0-or-later */',
         '#ifndef VORBIS_FIXTURES_H', '#define VORBIS_FIXTURES_H', '#include <stdint.h>',
         'struct vorbis_fixture { const char *name; const char *ref; uint32_t frames, channels,'
         ' rate, block0, block1, setup_bytes; };',
         'static const struct vorbis_fixture vorbis_fixtures[] = {']
for name, ref, frames, ch, rate, b0, b1, setup in positive:
    lines.append('  { "%s", "%s", %d, %d, %d, %d, %d, %d },' % (name, ref, frames, ch, rate, b0, b1, setup))
lines += ['};', '#endif']
open('vorbis_fixtures.h', 'w').write('\n'.join(lines) + '\n')
PY
echo "wrote fixtures/ and vorbis_fixtures.h"
