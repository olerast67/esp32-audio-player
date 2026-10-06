#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Compare a WAV written by player_cli with a reference WAV, sample by sample.

test/data/gap_ref.wav is one continuous 1 kHz sine; gap_a.flac and gap_b.flac are the same
signal cut at sample 22383, which is not a FLAC frame boundary. Played back to back through the
engine, the output must equal the reference exactly:

    player_cli <folder with 01 gap_a.flac and 02 gap_b.flac> out.wav --gapless-folder
    python tools/gapless_check.py out.wav test/data/gap_ref.wav

Only the first channel is compared; 24-bit output is reduced to 16 bits by dropping the low byte
(the source is 16-bit, so the low byte is zero when nothing changed the samples).
"""
import struct
import sys
import wave


def read_mono16(path):
    with wave.open(path) as w:
        n, width, ch = w.getnframes(), w.getsampwidth(), w.getnchannels()
        data = w.readframes(n)
    if width == 2:
        samples = list(struct.unpack("<%dh" % (n * ch), data))
    elif width == 3:
        samples = [int.from_bytes(data[i:i + 3], "little", signed=True) >> 8 for i in range(0, len(data), 3)]
    else:
        sys.exit("%s: %d-byte samples are not handled" % (path, width))
    return samples[::ch]


def main():
    if len(sys.argv) != 3:
        sys.exit("usage: gapless_check.py played.wav reference.wav")
    played, ref = read_mono16(sys.argv[1]), read_mono16(sys.argv[2])
    same = sum(1 for a, b in zip(played, ref) if a == b)
    print("%d samples played, %d in the reference, %d identical" % (len(played), len(ref), same))
    sys.exit(0 if len(played) == len(ref) == same else 1)


if __name__ == "__main__":
    main()
