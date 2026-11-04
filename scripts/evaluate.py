#!/usr/bin/env python3
"""Objective checks of a processed file against the clean reference.

    evaluate.py clean_ref.wav input.wav output.wav

Reports
  * speech level (active speech, dBFS) and peak
  * level in the 1 s noise-only lead-in (what team-mates hear while you are silent)
  * colour error: how far the long-term spectrum is from the real (clean) voice, in dB RMS
    over 1/3-octave bands 150 Hz .. 7 kHz, after removing the overall level. A good result
    lowers this number for the input -> output step (the mic colour got undone).
  * high-band energy above 8 kHz relative to 1-4 kHz (restoration check)
"""
import sys

import numpy as np
import scipy.io.wavfile as wavfile
import scipy.signal as sg

FS = 48000


def load(p):
    r, d = wavfile.read(p)
    d = d.astype(np.float64)
    if d.ndim > 1:
        d = d.mean(axis=1)
    d /= 32768.0
    if r != FS:
        d = sg.resample_poly(d, FS, r)
    return d


def speech_mask(x, frame=480):
    n = len(x) // frame
    e = (x[: n * frame].reshape(n, frame) ** 2).mean(axis=1)
    return e > e.max() * 10 ** (-35 / 10), e


def third_octave_ltas(x, mask, frame=480):
    f, t, Z = sg.stft(x, FS, nperseg=2048, noverlap=1536)
    P = np.abs(Z) ** 2
    hop = 512
    idx = np.clip((t * FS / frame).astype(int), 0, len(mask) - 1)
    P = P[:, mask[idx]].mean(axis=1)
    centres = 1000 * 2 ** (np.arange(-13, 13) / 3)
    out = []
    for c in centres:
        lo, hi = c / 2 ** (1 / 6), c * 2 ** (1 / 6)
        sel = (f >= lo) & (f < hi)
        out.append(10 * np.log10(P[sel].mean() + 1e-20) if sel.any() else np.nan)
    return centres, np.array(out)


def colour_error(ref_c, ref_l, l, lo=150, hi=7000):
    sel = (ref_c >= lo) & (ref_c <= hi)
    d = l[sel] - ref_l[sel]
    d -= d.mean()
    return np.sqrt((d ** 2).mean()), d


def main():
    ref, inp, out = (load(p) for p in sys.argv[1:4])
    n = min(len(ref), len(inp), len(out))
    ref, inp, out = ref[:n], inp[:n], out[:n]
    mask, e = speech_mask(ref)
    lead = slice(int(0.2 * FS), int(0.9 * FS))
    c, lr = third_octave_ltas(ref, mask)
    _, li = third_octave_ltas(inp, mask)
    _, lo = third_octave_ltas(out, mask)
    ei, di = colour_error(c, lr, li)
    eo, do = colour_error(c, lr, lo)

    def lvl(x):
        m = mask[: len(x) // 480]
        fr = x[: len(m) * 480].reshape(len(m), 480)
        return 10 * np.log10((fr[m] ** 2).mean() + 1e-20)

    def hb(l):
        return np.nanmean(l[(c > 8000) & (c < 14000)]) - np.nanmean(l[(c > 1000) & (c < 4000)])

    print(f"speech level  in {lvl(inp):6.1f} dBFS   out {lvl(out):6.1f} dBFS   (target about -19)")
    print(f"peak          in {20*np.log10(np.abs(inp).max()+1e-12):6.1f} dBFS   out {20*np.log10(np.abs(out).max()+1e-12):6.1f} dBFS")
    print(f"silence/noise in {10*np.log10((inp[lead]**2).mean()+1e-20):6.1f} dBFS   out {10*np.log10((out[lead]**2).mean()+1e-20):6.1f} dBFS")
    # pauses in the middle of the file (at least 0.3 s away from any speech)
    from scipy.ndimage import binary_dilation
    near = binary_dilation(mask, iterations=30)
    pz = ~near
    pz[: 2 * FS // 480] = False
    def pause_db(x):
        m = pz[: len(x) // 480]
        fr = x[: len(m) * 480].reshape(len(m), 480)
        return 10 * np.log10((fr[m] ** 2).mean() + 1e-20) if m.any() else float('nan')
    print(f"mid-file pauses in {pause_db(inp):6.1f} dBFS   out {pause_db(out):6.1f} dBFS   ({pz.sum()/100:.1f} s of pauses)")
    print(f"colour error vs real voice: in {ei:4.2f} dB  ->  out {eo:4.2f} dB")
    print(f"high band (8-14k vs 1-4k): ref {hb(lr):6.1f}  in {hb(li):6.1f}  out {hb(lo):6.1f} dB")
    sel = (c >= 150) & (c <= 7000)
    print("  band  Hz: " + " ".join(f"{int(v):>5d}" for v in c[sel]))
    print("  in  -ref: " + " ".join(f"{v:+5.1f}" for v in di))
    print("  out -ref: " + " ".join(f"{v:+5.1f}" for v in do))


if __name__ == "__main__":
    main()
