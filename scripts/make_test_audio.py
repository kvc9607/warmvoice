#!/usr/bin/env python3
"""Build test recordings that imitate typical gaming-headset problems.

    make_test_audio.py clean.wav noise.wav outdir

Creates:
  headset.wav  quiet (-34 dBFS speech), boxy + dull mic colour, fan/room noise at 12 dB SNR
               (process with `wvproc --rate 16000` to also imitate a 16 kHz wireless link)
  usbmic.wav   normal level, slightly thin mic, noise at 20 dB SNR, full band
"""
import sys

import numpy as np
import scipy.io.wavfile as wavfile
import scipy.signal as sg

FS = 48000


def load(path):
    rate, d = wavfile.read(path)
    d = d.astype(np.float64)
    if d.ndim > 1:
        d = d.mean(axis=1)
    if np.issubdtype(np.asarray(d).dtype, np.floating) and np.abs(d).max() > 1.5:
        d /= 32768.0
    if rate != FS:
        d = sg.resample_poly(d, FS, rate)
    return d


def active_rms(x, frame=480):
    n = len(x) // frame
    e = (x[: n * frame].reshape(n, frame) ** 2).mean(axis=1)
    thr = e.max() * 10 ** (-40 / 10)
    return np.sqrt(e[e > thr].mean())


def peaking(f0, q, gain_db):
    a = 10 ** (gain_db / 40)
    w = 2 * np.pi * f0 / FS
    al = np.sin(w) / (2 * q)
    b = [1 + al * a, -2 * np.cos(w), 1 - al * a]
    aa = [1 + al / a, -2 * np.cos(w), 1 - al / a]
    return np.array(b) / aa[0], np.array(aa) / aa[0]


def high_shelf(f0, gain_db):
    a = 10 ** (gain_db / 40)
    w = 2 * np.pi * f0 / FS
    cw, sw = np.cos(w), np.sin(w)
    al = sw / 2 * np.sqrt(2)
    sa = 2 * np.sqrt(a) * al
    b = [a * ((a + 1) + (a - 1) * cw + sa), -2 * a * ((a - 1) + (a + 1) * cw), a * ((a + 1) + (a - 1) * cw - sa)]
    aa = [(a + 1) - (a - 1) * cw + sa, 2 * ((a - 1) - (a + 1) * cw), (a + 1) - (a - 1) * cw - sa]
    return np.array(b) / aa[0], np.array(aa) / aa[0]


def colour(x, filters):
    for b, a in filters:
        x = sg.lfilter(b, a, x)
    return x


def mix(speech, noise, snr_db, speech_dbfs):
    reps = int(np.ceil(len(speech) / len(noise)))
    noise = np.tile(noise, reps)[: len(speech)]
    s = speech / active_rms(speech) * 10 ** (speech_dbfs / 20)
    nr = np.sqrt((noise ** 2).mean())
    n = noise / nr * 10 ** ((speech_dbfs - snr_db) / 20)
    # 1 s of pure noise at the start: lets the noise estimate settle, like real use
    lead = n[:FS]
    return np.concatenate([lead, s + n])


def save(path, x):
    x = np.clip(x, -1, 1)
    wavfile.write(path, FS, (x * 32767).astype(np.int16))


def main():
    clean, noise, out = sys.argv[1], sys.argv[2], sys.argv[3]
    s = load(clean)
    n = load(noise)
    s = np.concatenate([s, s])  # ~21 s, enough for the voice profile to settle
    boxy_dull = [peaking(450, 1.4, 6.0), high_shelf(3500, -7.0)]
    thin = [peaking(150, 0.8, -5.0), peaking(3000, 1.5, 3.0)]
    save(f"{out}/headset.wav", mix(colour(s, boxy_dull), n, 12, -34))
    save(f"{out}/usbmic.wav", mix(colour(s, thin), n, 20, -24))
    save(f"{out}/clean_ref.wav", np.concatenate([np.zeros(FS), s / active_rms(s) * 10 ** (-19 / 20)]))


if __name__ == "__main__":
    main()
