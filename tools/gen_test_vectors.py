#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Generate the decoder test vectors in test/data/.

Requirements: Python 3.8+, numpy, soundfile (libsndfile >= 1.1 for MP3):
    python -m pip install --user numpy soundfile

WAV, AIFF, DSF and DFF files are written by hand here so every header variant is under our
control. FLAC, MP3 and Ogg Vorbis go through libsndfile (libFLAC, LAME, libvorbis).

Besides the audio files the script writes vectors.txt, one line per file:

    name codec rate channels bits dop frames crc32 ref

crc32 is zlib's CRC-32 of the expected decoder output (interleaved Q31, int32 little-endian) for
lossless files, or "-" for lossy ones. ref names the lossless file a lossy file is compared with
(signal-to-noise after alignment), or "-". The host tests (test/test_decoders_*.c) read it.
Output is deterministic: running the script twice gives the same manifest.
"""

import argparse
import os
import struct
import sys
import zlib

import numpy as np
import soundfile as sf

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_OUT = os.path.join(ROOT, "test", "data")


# ----------------------------------------------------------------------------------- signals ----
def tone_sweep(n, rate, amp=0.5):
    """Stereo test signal: L = 997 Hz sine, R = log sweep 50 Hz -> 8 kHz (both at amp)."""
    t = np.arange(n) / rate
    left = amp * np.sin(2 * np.pi * 997.0 * t)
    f0, f1, dur = 50.0, 8000.0, n / rate
    k = np.log(f1 / f0) / dur
    right = amp * np.sin(2 * np.pi * f0 * (np.exp(k * t) - 1) / k)
    return np.stack([left, right], axis=1)


def quantize(x, bits):
    """float [-1, 1) -> signed integers of `bits` bits (rounded, clipped)."""
    scale = float(1 << (bits - 1))
    return np.clip(np.round(x * scale), -scale, scale - 1).astype(np.int64)


def q31_from_int(v, bits):
    return (v.astype(np.int64) << (32 - bits)).astype(np.int32)


def q31_from_float(x):
    """Our decoder's float rule: trunc(x * 2^31), saturated; NaN -> 0; -1.0 exact."""
    x = np.asarray(x, dtype=np.float64)
    out = np.zeros(x.shape, dtype=np.int64)
    nan = np.isnan(x)
    hi = ~nan & (x >= 1.0)
    lo = ~nan & (x <= -1.0)
    mid = ~nan & ~hi & ~lo
    out[hi] = 2**31 - 1
    out[lo] = -(2**31)
    out[mid] = np.trunc(x[mid] * 2.0**31).astype(np.int64)
    return out.astype(np.int32)


def crc_of(q31):
    return "%08x" % (zlib.crc32(np.ascontiguousarray(q31, dtype="<i4").tobytes()) & 0xFFFFFFFF)


# ------------------------------------------------------------------------------ RIFF / WAVE ----
def chunk_le(tag, body):
    pad = b"\0" if len(body) & 1 else b""
    return tag + struct.pack("<I", len(body)) + body + pad


def wav_fmt(tag, channels, rate, container_bytes, bits, valid_bits=None, extensible=False):
    block = channels * container_bytes
    if not extensible:
        return struct.pack("<HHIIHH", tag, channels, rate, rate * block, block, bits)
    guid = struct.pack("<H", tag) + bytes.fromhex("000000001000800000aa00389b71")
    mask = 0x4 if channels == 1 else 0x3
    return struct.pack("<HHIIHHHHI", 0xFFFE, channels, rate, rate * block, block, bits, 22,
                       valid_bits or bits, mask) + guid


def write_riff(path, chunks):
    body = b"WAVE" + b"".join(chunks)
    with open(path, "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", len(body)) + body)


def pcm_bytes_le(v, container_bytes):
    """Signed ints (already scaled to the container) -> little-endian bytes."""
    v = v.astype(np.int64).reshape(-1)
    u = v & ((1 << (8 * container_bytes)) - 1)
    out = np.zeros((len(u), container_bytes), dtype=np.uint8)
    for i in range(container_bytes):
        out[:, i] = (u >> (8 * i)) & 0xFF
    return out.tobytes()


def pcm_bytes_be(v, container_bytes):
    v = v.astype(np.int64).reshape(-1)
    u = v & ((1 << (8 * container_bytes)) - 1)
    out = np.zeros((len(u), container_bytes), dtype=np.uint8)
    for i in range(container_bytes):
        out[:, container_bytes - 1 - i] = (u >> (8 * i)) & 0xFF
    return out.tobytes()


# --------------------------------------------------------------------------------- AIFF ----
def ext80(rate):
    """Integer sample rate -> 80-bit IEEE 754 extended (big-endian)."""
    exp = rate.bit_length() - 1
    mant = rate << (63 - exp)
    return struct.pack(">HQ", 16383 + exp, mant)


def chunk_be(tag, body):
    pad = b"\0" if len(body) & 1 else b""
    return tag + struct.pack(">I", len(body)) + body + pad


def write_form(path, form, chunks):
    body = form + b"".join(chunks)
    with open(path, "wb") as f:
        f.write(b"FORM" + struct.pack(">I", len(body)) + body)


# ----------------------------------------------------------------------------------- Ogg ----
def _ogg_crc_table():
    table = []
    for i in range(256):
        r = i << 24
        for _ in range(8):
            r = ((r << 1) ^ 0x04C11DB7) if r & 0x80000000 else (r << 1)
        table.append(r & 0xFFFFFFFF)
    return table


OGG_CRC = _ogg_crc_table()


def ogg_crc(data):
    crc = 0
    for b in data:
        crc = ((crc << 8) & 0xFFFFFFFF) ^ OGG_CRC[((crc >> 24) & 0xFF) ^ b]
    return crc


def ogg_pages(data):
    """Split an Ogg file into (header_type, granule, serial, lacing, body) tuples."""
    pages, pos = [], 0
    while pos < len(data):
        assert data[pos:pos + 4] == b"OggS"
        flags, granule, serial = data[pos + 5], struct.unpack_from("<q", data, pos + 6)[0], \
            struct.unpack_from("<I", data, pos + 14)[0]
        nseg = data[pos + 26]
        lacing = list(data[pos + 27:pos + 27 + nseg])
        start = pos + 27 + nseg
        body = data[start:start + sum(lacing)]
        pages.append((flags, granule, serial, lacing, body))
        pos = start + sum(lacing)
    return pages


def ogg_page(flags, granule, serial, seq, lacing, body):
    hdr = b"OggS" + bytes([0, flags]) + struct.pack("<qII", granule, serial, seq) + b"\0\0\0\0" + \
        bytes([len(lacing)]) + bytes(lacing)
    page = bytearray(hdr + body)
    struct.pack_into("<I", page, 22, ogg_crc(page))
    return bytes(page)


def ogg_with_big_comment(src, comment_bytes):
    """Rewrite an Ogg Vorbis file with a large comment packet (cover art sized) that spans
    several pages and shares its last page with the start of the setup packet."""
    pages = ogg_pages(src)
    # Reassemble the three header packets.
    packets, cur, first_audio_page = [], b"", None
    for i, (flags, granule, serial, lacing, body) in enumerate(pages):
        off = 0
        for lv in lacing:
            cur += body[off:off + lv]
            off += lv
            if lv < 255:
                packets.append(cur)
                cur = b""
        if len(packets) >= 3:
            first_audio_page = i + 1
            break
    serial = pages[0][2]
    ident, _, setup = packets[:3]
    vendor = b"gen_test_vectors"
    filler = (b"METADATA_BLOCK_PICTURE=" + b"QUJD" * (comment_bytes // 4))
    comments = [b"TITLE=Big comment", filler]
    comment = b"\x03vorbis" + struct.pack("<I", len(vendor)) + vendor + struct.pack("<I", len(comments))
    for c in comments:
        comment += struct.pack("<I", len(c)) + c
    comment += b"\x01"

    out, seq = [], 0
    out.append(ogg_page(0x02, 0, serial, seq, [len(ident)], ident))
    seq += 1
    # Pack comment and setup back to back; a page ends when 255 lacing values are used.
    segs = []  # (lacing value, bytes)
    for pkt in (comment, setup):
        for i in range(0, len(pkt) // 255):
            segs.append((255, pkt[i * 255:(i + 1) * 255]))
        rest = len(pkt) % 255
        segs.append((rest, pkt[len(pkt) - rest:]))
    continued = False
    while segs:
        chunk, segs = segs[:255], segs[255:]
        lacing = [lv for lv, _ in chunk]
        body = b"".join(b for _, b in chunk)
        granule = 0 if lacing[-1] < 255 else -1
        out.append(ogg_page(0x01 if continued else 0x00, granule, serial, seq, lacing, body))
        continued = lacing[-1] == 255
        seq += 1
    for flags, granule, ser, lacing, body in pages[first_audio_page:]:
        out.append(ogg_page(flags, granule, ser, seq, lacing, body))
        seq += 1
    return b"".join(out)


# ----------------------------------------------------------------------------------- DSD ----
def sigma_delta2(x):
    """Second-order sigma-delta modulator: float in [-0.5, 0.5] -> bits (0/1), oldest first."""
    bits = np.empty(len(x), dtype=np.uint8)
    i1 = i2 = 0.0
    y = 1.0
    for n, v in enumerate(x.tolist()):
        i1 += v - y
        i2 += i1 - y
        y = 1.0 if i2 >= 0.0 else -1.0
        bits[n] = 1 if y > 0 else 0
    return bits


def dsd_signal(n_bits, rate, channels):
    t = np.arange(n_bits) / rate
    chans = [0.5 * np.sin(2 * np.pi * 1000.0 * t)]
    if channels == 2:
        chans.append(-0.5 * np.sin(2 * np.pi * 1000.0 * t))
    return [sigma_delta2(c) for c in chans]


def dop_expected(bits_per_channel):
    """Expected DoP stream (Q31) for per-channel bit arrays (oldest bit first)."""
    msb = [np.packbits(b, bitorder="big") for b in bits_per_channel]
    frames = len(bits_per_channel[0]) // 16
    out = np.zeros((frames, len(msb)), dtype=np.int64)
    marker = np.where(np.arange(frames) % 2 == 0, 0x05, 0xFA).astype(np.int64)
    for c, m in enumerate(msb):
        b0 = m[0:2 * frames:2].astype(np.int64)
        b1 = m[1:2 * frames:2].astype(np.int64)
        out[:, c] = (marker << 24) | (b0 << 16) | (b1 << 8)
    return out.astype(np.uint32).view(np.int32).reshape(-1)


def write_dsf(path, bits_per_channel, rate, lsb_first=True, block=4096):
    ch = len(bits_per_channel)
    order = "little" if lsb_first else "big"
    per_ch = [np.packbits(b, bitorder=order).tobytes() for b in bits_per_channel]
    nbytes = len(per_ch[0])
    nblocks = (nbytes + block - 1) // block
    data = bytearray()
    for k in range(nblocks):
        for c in range(ch):
            part = per_ch[c][k * block:(k + 1) * block]
            data += part + b"\0" * (block - len(part))
    fmt = b"fmt " + struct.pack("<QIIIIIIQII", 52, 1, 0, ch, ch, rate, 1 if lsb_first else 8,
                                len(bits_per_channel[0]), block, 0)
    data_chunk = b"data" + struct.pack("<Q", 12 + len(data)) + bytes(data)
    total = 28 + len(fmt) + len(data_chunk)
    head = b"DSD " + struct.pack("<QQQ", 28, total, 0)
    with open(path, "wb") as f:
        f.write(head + fmt + data_chunk)


def write_dff(path, bits_per_channel, rate):
    ch = len(bits_per_channel)
    msb = [np.packbits(b, bitorder="big") for b in bits_per_channel]
    inter = np.stack(msb, axis=1).reshape(-1).tobytes()

    def ck(tag, body):
        return tag + struct.pack(">Q", len(body)) + body + (b"\0" if len(body) & 1 else b"")

    ids = [b"SLFT", b"SRGT"] if ch == 2 else [b"C   "]
    name = b"not compressed"
    prop = b"SND " + ck(b"FS  ", struct.pack(">I", rate)) + \
        ck(b"CHNL", struct.pack(">H", ch) + b"".join(ids)) + \
        ck(b"CMPR", b"DSD " + bytes([len(name)]) + name)
    body = b"DSD " + ck(b"FVER", struct.pack(">I", 0x01050000)) + ck(b"PROP", prop) + ck(b"DSD ", inter)
    with open(path, "wb") as f:
        f.write(b"FRM8" + struct.pack(">Q", len(body)) + body)


# ----------------------------------------------------------------------------------- tags ----
def id3v2_tag(title, padding=512):
    frame_data = b"\x03" + title.encode("utf-8")
    frame = b"TIT2" + struct.pack(">I", len(frame_data)) + b"\0\0" + frame_data
    body = frame + b"\0" * padding
    size = len(body)
    syncsafe = bytes([(size >> 21) & 0x7F, (size >> 14) & 0x7F, (size >> 7) & 0x7F, size & 0x7F])
    return b"ID3\x03\x00\x00" + syncsafe + body


def id3v1_tag(title):
    return b"TAG" + title.encode("latin-1")[:30].ljust(30, b"\0") + b"\0" * 94 + b"\xff"


def apev2_tag(key, value):
    item = struct.pack("<II", len(value), 0) + key + b"\0" + value
    size = len(item) + 32
    header = b"APETAGEX" + struct.pack("<IIII", 2000, size, 1, 0xA0000000) + b"\0" * 8
    footer = b"APETAGEX" + struct.pack("<IIII", 2000, size, 1, 0x80000000) + b"\0" * 8
    return header + item + footer


# ---------------------------------------------------------------------------------- main ----
class Manifest:
    def __init__(self):
        self.lines = []

    def add(self, name, codec, rate, ch, bits, frames, crc="-", ref="-", dop=0):
        self.lines.append(f"{name} {codec} {rate} {ch} {bits} {dop} {frames} {crc} {ref}")

    def write(self, path):
        with open(path, "w", newline="\n") as f:
            f.write("# Generated by tools/gen_test_vectors.py - do not edit.\n")
            f.write("# name codec rate channels bits dop frames crc32(q31 le) ref\n")
            f.write("\n".join(self.lines) + "\n")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--out", default=DEFAULT_OUT, help="output directory (default: test/data)")
    args = ap.parse_args()
    out = args.out
    os.makedirs(out, exist_ok=True)
    m = Manifest()

    def p(name):
        return os.path.join(out, name)

    # 16/44.1 stereo reference: 1 s, with an unknown LIST chunk and an odd-sized chunk before data.
    n16 = 44100
    s16 = quantize(tone_sweep(n16, 44100), 16)
    info = chunk_le(b"LIST", b"INFOINAM" + struct.pack("<I", 8) + b"vectors\0")
    odd = chunk_le(b"odd ", b"xyz")  # 3 bytes + pad byte
    write_riff(p("wav_s16_44k.wav"), [chunk_le(b"fmt ", wav_fmt(1, 2, 44100, 2, 16)), info, odd,
                                     chunk_le(b"data", pcm_bytes_le(s16, 2))])
    m.add("wav_s16_44k.wav", "WAV", 44100, 2, 16, n16, crc_of(q31_from_int(s16, 16)))

    # 24/96 stereo, WAVE_FORMAT_EXTENSIBLE.
    n24 = 48000
    s24 = quantize(tone_sweep(n24, 96000), 24)
    write_riff(p("wav_s24_96k_ext.wav"), [chunk_le(b"fmt ", wav_fmt(1, 2, 96000, 3, 24, 24, True)),
                                         chunk_le(b"data", pcm_bytes_le(s24, 3))])
    m.add("wav_s24_96k_ext.wav", "WAV", 96000, 2, 24, n24, crc_of(q31_from_int(s24, 24)))

    # 20-bit samples in a 24-bit container (extensible, valid bits 20).
    n20 = 4800
    s20 = quantize(tone_sweep(n20, 48000), 20)
    write_riff(p("wav_s20in24_48k_ext.wav"), [chunk_le(b"fmt ", wav_fmt(1, 2, 48000, 3, 24, 20, True)),
                                             chunk_le(b"data", pcm_bytes_le(s20 << 4, 3))])
    m.add("wav_s20in24_48k_ext.wav", "WAV", 48000, 2, 24, n20, crc_of(q31_from_int(s20 << 4, 24)))

    # float32 stereo with out-of-range values, infinities and a NaN first, then the signal.
    special = np.array([[1.0, -1.0], [1.5, -1.5], [0.5, -0.5], [np.nan, np.inf], [-np.inf, 2.0**-40],
                        [0.999999, -0.999999], [3.0e-10, -3.0e-10], [0.25, -0.75]], dtype=np.float32)
    nf = 11025
    f32 = np.concatenate([special, (s16[:nf - len(special)] / 32768.0).astype(np.float32)])
    write_riff(p("wav_f32_44k.wav"), [chunk_le(b"fmt ", wav_fmt(3, 2, 44100, 4, 32)),
                                     chunk_le(b"fact", struct.pack("<I", nf)),
                                     chunk_le(b"data", f32.astype("<f4").tobytes())])
    m.add("wav_f32_44k.wav", "WAV", 44100, 2, 32, nf, crc_of(q31_from_float(f32)))

    # float64 mono 48 kHz.
    n64 = 4800
    f64 = tone_sweep(n64, 48000)[:, :1] * 1.2  # includes values beyond full scale
    write_riff(p("wav_f64_48k_mono.wav"), [chunk_le(b"fmt ", wav_fmt(3, 1, 48000, 8, 64)),
                                          chunk_le(b"data", f64.astype("<f8").tobytes())])
    m.add("wav_f64_48k_mono.wav", "WAV", 48000, 1, 32, n64, crc_of(q31_from_float(f64)))

    # 8-bit unsigned mono 22.05 kHz.
    n8 = 5512
    s8 = quantize(tone_sweep(n8, 22050)[:, :1], 8)
    write_riff(p("wav_u8_22k_mono.wav"), [chunk_le(b"fmt ", wav_fmt(1, 1, 22050, 1, 8)),
                                         chunk_le(b"data", (s8 + 128).astype(np.uint8).tobytes())])
    m.add("wav_u8_22k_mono.wav", "WAV", 22050, 1, 16, n8, crc_of(q31_from_int(s8, 8)))

    # RF64: ds64 carries the sizes, the 32-bit fields hold 0xFFFFFFFF.
    n32 = 12000
    s32 = quantize(tone_sweep(n32, 48000), 32)
    data32 = pcm_bytes_le(s32, 4)
    fmt32 = chunk_le(b"fmt ", wav_fmt(1, 2, 48000, 4, 32))
    riff_size = 4 + (8 + 28) + len(fmt32) + 8 + len(data32)
    ds64 = b"ds64" + struct.pack("<IQQQI", 28, riff_size, len(data32), n32, 0)
    with open(p("wav_s32_48k_rf64.wav"), "wb") as f:
        f.write(b"RF64" + struct.pack("<I", 0xFFFFFFFF) + b"WAVE" + ds64 + fmt32 +
                b"data" + struct.pack("<I", 0xFFFFFFFF) + data32)
        assert f.tell() == riff_size + 8
    m.add("wav_s32_48k_rf64.wav", "WAV", 48000, 2, 32, n32, crc_of(q31_from_int(s32, 32)))

    # AIFF 16-bit stereo (first half second of the WAV reference).
    na = 22050
    comm = struct.pack(">HIH", 2, na, 16) + ext80(44100)
    write_form(p("aiff_s16_44k.aiff"), b"AIFF", [chunk_be(b"COMM", comm),
                                                chunk_be(b"SSND", struct.pack(">II", 0, 0) +
                                                         pcm_bytes_be(s16[:na], 2))])
    m.add("aiff_s16_44k.aiff", "AIFF", 44100, 2, 16, na, crc_of(q31_from_int(s16[:na], 16)))

    # AIFC "sowt" (little-endian) 16-bit stereo.
    ns = 11025
    name = b"\x0bnot swapped"  # Pascal string incl. length byte, 12 bytes -> even
    comm = struct.pack(">HIH", 2, ns, 16) + ext80(44100) + b"sowt" + name
    write_form(p("aifc_sowt_s16_44k.aifc"), b"AIFC", [chunk_be(b"FVER", struct.pack(">I", 0xA2805140)),
                                                     chunk_be(b"COMM", comm),
                                                     chunk_be(b"SSND", struct.pack(">II", 0, 0) +
                                                              pcm_bytes_le(s16[:ns], 2))])
    m.add("aifc_sowt_s16_44k.aifc", "AIFF", 44100, 2, 16, ns, crc_of(q31_from_int(s16[:ns], 16)))

    # AIFF 24-bit mono 48 kHz: SSND before COMM, a non-zero SSND offset and an odd-sized chunk.
    nm = 12000
    m24 = quantize(tone_sweep(nm, 48000)[:, :1], 24)
    comm = struct.pack(">HIH", 1, nm, 24) + ext80(48000)
    write_form(p("aiff_s24_48k_mono.aiff"), b"AIFF", [chunk_be(b"ANNO", b"odd"),
                                                     chunk_be(b"SSND", struct.pack(">II", 4, 0) + b"\0" * 4 +
                                                              pcm_bytes_be(m24, 3)),
                                                     chunk_be(b"COMM", comm)])
    m.add("aiff_s24_48k_mono.aiff", "AIFF", 48000, 1, 24, nm, crc_of(q31_from_int(m24, 24)))

    # AIFC float32 big-endian stereo.
    nfl = 4410
    fl = (s16[:nfl] / 32768.0).astype(np.float32)
    comm = struct.pack(">HIH", 2, nfl, 32) + ext80(44100) + b"fl32" + b"\x00\x00"
    write_form(p("aifc_fl32_44k.aifc"), b"AIFC", [chunk_be(b"COMM", comm),
                                                 chunk_be(b"SSND", struct.pack(">II", 0, 0) +
                                                          fl.astype(">f4").tobytes())])
    m.add("aifc_fl32_44k.aifc", "AIFF", 44100, 2, 32, nfl, crc_of(q31_from_float(fl)))

    # FLAC 16/44.1 and 24/96 (same samples as the WAV files), and an ID3v2-prefixed FLAC.
    sf.write(p("flac_s16_44k.flac"), s16.astype(np.int16), 44100, format="FLAC", subtype="PCM_16")
    m.add("flac_s16_44k.flac", "FLAC", 44100, 2, 16, n16, crc_of(q31_from_int(s16, 16)))
    sf.write(p("flac_s24_96k.flac"), (s24 << 8).astype(np.int32), 96000, format="FLAC", subtype="PCM_24")
    m.add("flac_s24_96k.flac", "FLAC", 96000, 2, 24, n24, crc_of(q31_from_int(s24, 24)))
    tmp = p("_tmp.flac")
    nid = 11025
    sf.write(tmp, s16[:nid].astype(np.int16), 44100, format="FLAC", subtype="PCM_16")
    with open(tmp, "rb") as f:
        flac_small = f.read()
    os.remove(tmp)
    with open(p("flac_id3_s16_44k.flac"), "wb") as f:
        f.write(id3v2_tag("ID3 in front of FLAC") + flac_small)
    m.add("flac_id3_s16_44k.flac", "FLAC", 44100, 2, 16, nid, crc_of(q31_from_int(s16[:nid], 16)))

    # MP3 (LAME through libsndfile): VBR with Xing+LAME, CBR with Info+LAME, a tagged copy.
    lossy_src = (s16 / 32768.0).astype(np.float32)
    sf.write(p("mp3_vbr_44k.mp3"), lossy_src, 44100, format="MP3", subtype="MPEG_LAYER_III",
             bitrate_mode="VARIABLE", compression_level=0.1)
    m.add("mp3_vbr_44k.mp3", "MP3", 44100, 2, 24, n16, "-", "wav_s16_44k.wav")
    sf.write(p("mp3_cbr_44k.mp3"), lossy_src, 44100, format="MP3", subtype="MPEG_LAYER_III",
             bitrate_mode="CONSTANT", compression_level=0.2)
    m.add("mp3_cbr_44k.mp3", "MP3", 44100, 2, 24, n16, "-", "wav_s16_44k.wav")
    # MPEG-2 Layer III (LSF: 576 samples per frame) mono at 22.05 kHz, with its 16-bit reference.
    # Tone + sweep in one channel keeps the alignment search unambiguous.
    nl = 11025
    lsf = quantize(tone_sweep(nl, 22050, amp=0.35).sum(axis=1, keepdims=True), 16)
    write_riff(p("wav_s16_22k_mono.wav"), [chunk_le(b"fmt ", wav_fmt(1, 1, 22050, 2, 16)),
                                          chunk_le(b"data", pcm_bytes_le(lsf, 2))])
    m.add("wav_s16_22k_mono.wav", "WAV", 22050, 1, 16, nl, crc_of(q31_from_int(lsf, 16)))
    sf.write(p("mp3_lsf_22k_mono.mp3"), (lsf / 32768.0).astype(np.float32), 22050, format="MP3",
             subtype="MPEG_LAYER_III", bitrate_mode="VARIABLE", compression_level=0.1)
    m.add("mp3_lsf_22k_mono.mp3", "MP3", 22050, 1, 24, nl, "-", "wav_s16_22k_mono.wav")

    with open(p("mp3_vbr_44k.mp3"), "rb") as f:
        mp3 = f.read()
    with open(p("mp3_tagged_44k.mp3"), "wb") as f:
        f.write(id3v2_tag("Tagged MP3", padding=1000) + mp3 + apev2_tag(b"Title", b"APE tag") +
                id3v1_tag("ID3v1 title"))
    m.add("mp3_tagged_44k.mp3", "MP3", 44100, 2, 24, n16, "-", "wav_s16_44k.wav")

    # Ogg Vorbis, and the same stream with a cover-art sized comment packet.
    sf.write(p("ogg_44k.ogg"), lossy_src, 44100, format="OGG", subtype="VORBIS", compression_level=0.3)
    m.add("ogg_44k.ogg", "Vorbis", 44100, 2, 24, n16, "-", "wav_s16_44k.wav")
    with open(p("ogg_44k.ogg"), "rb") as f:
        ogg = f.read()
    with open(p("ogg_bigcomment_44k.ogg"), "wb") as f:
        f.write(ogg_with_big_comment(ogg, 150_000))
    m.add("ogg_bigcomment_44k.ogg", "Vorbis", 44100, 2, 24, n16, "-", "ogg_44k.ogg")

    # Gapless pair: one continuous 1 kHz sine split at a point that is not a frame boundary
    # for FLAC (4096) or MP3 (1152). gap_ref.wav is the continuous signal.
    ng, split = 44100, 22050 + 333
    g = quantize(0.7 * np.sin(2 * np.pi * 1000.0 * np.arange(ng) / 44100)[:, None], 16)
    write_riff(p("gap_ref.wav"), [chunk_le(b"fmt ", wav_fmt(1, 1, 44100, 2, 16)),
                                 chunk_le(b"data", pcm_bytes_le(g, 2))])
    m.add("gap_ref.wav", "WAV", 44100, 1, 16, ng, crc_of(q31_from_int(g, 16)))
    for part, seg in (("a", g[:split]), ("b", g[split:])):
        sf.write(p(f"gap_{part}.flac"), seg.astype(np.int16), 44100, format="FLAC", subtype="PCM_16")
        m.add(f"gap_{part}.flac", "FLAC", 44100, 1, 16, len(seg), crc_of(q31_from_int(seg, 16)))
        sf.write(p(f"gap_{part}.mp3"), (seg / 32768.0).astype(np.float32), 44100, format="MP3",
                 subtype="MPEG_LAYER_III", bitrate_mode="VARIABLE", compression_level=0.1)
        m.add(f"gap_{part}.mp3", "MP3", 44100, 1, 24, len(seg), "-", "gap_ref.wav")

    # DSD64: a 1 kHz tone through a 2nd-order sigma-delta modulator, as DSF (LSB first, 4096-byte
    # blocks, zero-padded last block) and as DFF; plus a mono MSB-first DSF.
    rate = 2822400
    nbits = rate // 4
    dsd = dsd_signal(nbits, rate, 2)
    exp = dop_expected(dsd)
    write_dsf(p("dsd64_1k.dsf"), dsd, rate)
    m.add("dsd64_1k.dsf", "DSF", rate // 16, 2, 24, nbits // 16, crc_of(exp), dop=1)
    write_dff(p("dsd64_1k.dff"), dsd, rate)
    m.add("dsd64_1k.dff", "DFF", rate // 16, 2, 24, nbits // 16, crc_of(exp), dop=1)
    mono = dsd_signal(rate // 20 + 40, rate, 1)  # not a multiple of 16 bits: the tail is dropped
    write_dsf(p("dsd64_mono_msb.dsf"), mono, rate, lsb_first=False)
    m.add("dsd64_mono_msb.dsf", "DSF", rate // 16, 1, 24, len(mono[0]) // 16, crc_of(dop_expected(mono)), dop=1)

    m.write(p("vectors.txt"))
    total = sum(os.path.getsize(p(f)) for f in os.listdir(out) if not f.startswith("_") and
                os.path.isfile(p(f)) and f != "README.md")
    print(f"wrote {len(m.lines)} vectors to {out} ({total / 1024:.0f} KiB)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
