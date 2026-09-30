#!/usr/bin/env python3
"""Turn an rtl_sdr-format IQ recording into a WAV that SDR++'s File Source plays.

  sample_to_wav.py <in.cu8 | in.xz> <out dir> [--freq HZ] [--rate HZ]

nrsc5 ships one such recording, support/sample.xz: unsigned 8-bit I/Q at
1488375 Hz, centred on KUT (90.5 MHz, Austin TX), carrying HD1 and HD2. The
File Source wants 16-bit stereo WAV and reads the centre frequency from a
"<n>Hz" in the file name, so the output is named accordingly. With no HD Radio
in range, this is the way to try the module in SDR++ itself.
"""
import argparse
import lzma
import os
import struct

import numpy as np

ap = argparse.ArgumentParser()
ap.add_argument("input")
ap.add_argument("outdir")
ap.add_argument("--freq", type=int, default=90500000)
ap.add_argument("--rate", type=int, default=1488375)
a = ap.parse_args()

opener = lzma.open if a.input.endswith(".xz") else open
with opener(a.input, "rb") as f:
    raw = np.frombuffer(f.read(), dtype=np.uint8)
raw = raw[: len(raw) // 2 * 2]
# (u - 127.5) * 256, rounded: the full 16-bit range, no clipping.
s16 = np.round((raw.astype(np.float32) - 127.5) * 256).astype(np.int16)

path = os.path.join(a.outdir, f"nrsc5_sample_{a.freq}Hz.wav")
data = s16.tobytes()
with open(path, "wb") as f:
    f.write(b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVE")
    f.write(b"fmt " + struct.pack("<IHHIIHH", 16, 1, 2, a.rate, a.rate * 4, 4, 16))
    f.write(b"data" + struct.pack("<I", len(data)))
    f.write(data)
print(f"{path}: {len(s16) // 2 / a.rate:.1f} s at {a.rate} Hz, centre {a.freq} Hz")
