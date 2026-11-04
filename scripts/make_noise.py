#!/usr/bin/env python3
"""Synthesise typical gaming-room noises (40 s each, 48 kHz):  make_noise.py outdir
   noise_fan.wav, noise_keyboard.wav, noise_fan_keyboard.wav"""
import sys

import numpy as np
import scipy.io.wavfile as w
import scipy.signal as sg

FS = 48000
out = sys.argv[1]
rng = np.random.default_rng(1)
n = FS * 40
t = np.arange(n) / FS
# PC fan: pink-ish noise + blade tone + 100 Hz mains hum
pink = sg.lfilter(*sg.butter(1, 800 / (FS / 2)), rng.standard_normal(n))
fan = pink / np.std(pink) + 0.3 * np.sin(2 * np.pi * 100 * t) + 0.15 * np.sin(2 * np.pi * 317 * t)
# mechanical keyboard: short bright clicks, about 5 per second, random level
kb = np.zeros(n)
b, a = sg.butter(2, [1500 / (FS / 2), 9000 / (FS / 2)], "band")
for pos in np.sort(rng.integers(0, n - 2000, size=40 * 5)):
    L = int(FS * 0.012)
    click = sg.lfilter(b, a, rng.standard_normal(L)) * np.exp(-np.arange(L) / (FS * 0.002))
    kb[pos:pos + L] += click * rng.uniform(0.5, 1.5)
kb /= np.sqrt((kb ** 2).mean())
for name, x in [("fan", fan), ("keyboard", kb), ("fan_keyboard", fan / np.std(fan) + 0.7 * kb)]:
    w.write(f"{out}/noise_{name}.wav", FS, (x / np.abs(x).max() * 0.5 * 32767).astype(np.int16))
