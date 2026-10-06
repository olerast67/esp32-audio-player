#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Render docs/demo.gif: a terminal session of the host CLI, typed out line by line.

The session text is the real output of player_cli and tools/gapless_check.py on the test vectors
(see the ctest `gapless_cli`). Re-run this script after changing the CLI output format:

    python tools/render_demo_gif.py            # needs Pillow and a monospace TTF font
"""
import os
import sys

from PIL import Image, ImageDraw, ImageFont

SESSION = [
    ("$ ", "ls album"),
    ("", "01 gap_a.flac  02 gap_b.flac"),
    ("", ""),
    ("$ ", "player_cli album out.wav --gapless-folder"),
    ("", "[1/2] 01 gap_a  FLAC 44100 Hz 16 bit mono, 0:00"),
    ("", "output: WAV 24 bit, 44100 Hz 24 bit, processing: none"),
    ("", "[2/2] 02 gap_b  FLAC 44100 Hz 16 bit mono, 0:00"),
    ("", "out.wav: 44100 frames, 1.00 s, 44100 Hz, 24 bit"),
    ("", ""),
    ("$ ", "python tools/gapless_check.py out.wav test/data/gap_ref.wav"),
    ("", "44100 samples played, 44100 in the reference, 44100 identical"),
    ("", ""),
    ("$ ", "player_cli flac_s24_96k.flac bt.wav --rate 44100 --crossfeed"),
    ("", "[1/1] flac_s24_96k  FLAC 96000 Hz 24 bit stereo, 0:00"),
    ("", "output: WAV 16 bit, 44100 Hz 16 bit, processing: crossfeed+limiter+resample"),
    ("", "bt.wav: 22286 frames, 0.51 s, 44100 Hz, 16 bit"),
]
TITLE = "gap_a.flac + gap_b.flac: one sine cut at sample 22383, inside a FLAC frame"

W, H = 960, 470
BG, FG, PROMPT, DIM, BAR = (13, 17, 23), (230, 237, 243), (88, 166, 255), (139, 148, 158), (33, 38, 45)
FONT_CANDIDATES = ["C:/Windows/Fonts/consola.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
                   "/System/Library/Fonts/Menlo.ttc"]


def font(size):
    for path in FONT_CANDIDATES:
        if os.path.exists(path):
            return ImageFont.truetype(path, size)
    sys.exit("no monospace font found")


def frame(lines, f, small):
    img = Image.new("RGB", (W, H), BG)
    d = ImageDraw.Draw(img)
    d.rectangle([0, 0, W, 34], fill=BAR)
    for i, c in enumerate([(255, 95, 86), (255, 189, 46), (39, 201, 63)]):
        d.ellipse([16 + i * 22, 11, 28 + i * 22, 23], fill=c)
    d.text((90, 9), TITLE, font=small, fill=DIM)
    y = 50
    for prompt, text in lines:
        x = 18
        if prompt:
            d.text((x, y), prompt, font=f, fill=PROMPT)
            x += d.textlength(prompt, font=f)
        d.text((x, y), text, font=f, fill=FG if prompt or "identical" in text else (201, 209, 217))
        y += 25
    return img


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    out = os.path.join(root, "docs", "demo.gif")
    f, small = font(16), font(13)
    frames, durations, shown = [], [], []
    for prompt, text in SESSION:
        if prompt:  # type the command
            for n in range(0, len(text) + 1, 3):
                frames.append(frame(shown + [(prompt, text[:n])], f, small))
                durations.append(40)
            shown.append((prompt, text))
            frames.append(frame(shown, f, small))
            durations.append(500)
        else:
            shown.append((prompt, text))
            frames.append(frame(shown, f, small))
            durations.append(260 if text else 120)
    durations[-1] = 4000
    os.makedirs(os.path.dirname(out), exist_ok=True)
    frames[0].save(out, save_all=True, append_images=frames[1:], duration=durations, loop=0, optimize=True)
    print("%s: %d frames, %d bytes" % (out, len(frames), os.path.getsize(out)))


if __name__ == "__main__":
    main()
