"""WarmVoice AI high-frequency reconstruction ("AI 补高频") - shared definitions.

Everything here mirrors the C++ side exactly (core/src/bwe.cpp):
  * STFT: 960-point Vorbis window, hop 480, 48 kHz (the AI denoiser's own transform)
  * 32 ERB bands (same layout as core/src/bands.cpp)
  * features -> small GRU -> per-band level of the missing high band
  * synthesis: spectral patching (copy the top octave of what the mic did capture upwards,
    with the phase rotation of a true frequency shift), scaled to the predicted levels

Numpy only (the training script uses torch for the same model).
SPDX-License-Identifier: MIT
"""
import numpy as np

FS = 48000
FFT = 960
HOP = 480
BINS = FFT // 2 + 1
BANDS = 32
BIN_HZ = FS / FFT  # 50 Hz

KC_MIN = 80    # 4 kHz: lowest cutoff the model handles
KC_MAX = 240   # 12 kHz
REF_TOP_BIN = 80   # bands entirely below 4 kHz form the level reference

N_IN = BANDS * 2 + 2
N_OUT = BANDS


def vorbis_window(n=FFT):
    k = np.arange(n)
    s = np.sin(np.pi * (k + 0.5) / n)
    return np.sin(0.5 * np.pi * s * s).astype(np.float64)


WIN = vorbis_window()
# power of a full-scale sine reads 0 dB (same as spectralNorm() in spectral.cpp)
POW_NORM = 4.0 / (WIN.sum() ** 2)


def band_layout():
    def f2e(f):
        return 9.265 * np.log(1.0 + f / (24.7 * 9.265))

    def e2f(e):
        return 24.7 * 9.265 * (np.exp(e / 9.265) - 1.0)

    lo, hi = f2e(0.0), f2e(FS / 2.0)
    step = (hi - lo) / BANDS
    widths, prev, over = [], 0, 0
    for b in range(BANDS):
        fb = int(np.floor(e2f(lo + (b + 1) * step) / BIN_HZ + 0.5))  # std::lround
        w = fb - prev - over
        if w < 2:
            over = 2 - w
            w = 2
        else:
            over = 0
        widths.append(w)
        prev = fb
    widths[-1] += BINS - sum(widths)
    edge = np.concatenate([[0], np.cumsum(widths)]).astype(int)
    centre_bin = 0.5 * (edge[:-1] + edge[1:] - 1)
    return edge, centre_bin


EDGE, CENTRE_BIN = band_layout()
REF_BANDS = np.array([b for b in range(BANDS) if EDGE[b + 1] <= REF_TOP_BIN and CENTRE_BIN[b] * BIN_HZ >= 200])


def stft(x):
    """x: float mono at 48 kHz -> complex (frames, BINS). Frame n covers x[n*HOP : n*HOP+FFT]."""
    n = 1 + max(0, (len(x) - FFT) // HOP)
    idx = np.arange(FFT)[None, :] + HOP * np.arange(n)[:, None]
    return np.fft.rfft(x[idx] * WIN[None, :], axis=1)


def istft(X, length):
    """Overlap-add with the same window (Vorbis is power complementary at 50 % overlap)."""
    frames = np.fft.irfft(X, n=FFT, axis=1) * WIN[None, :]
    y = np.zeros(max(length, HOP * (len(X) - 1) + FFT))
    for n in range(len(X)):
        y[n * HOP:n * HOP + FFT] += frames[n]
    return y[:length]


def power_db(X):
    return 10.0 * np.log10(np.abs(X) ** 2 * POW_NORM + 1e-20)


def band_density_db(pdb, lo_bin=None):
    """pdb: (frames, BINS) dB. Mean power per bin in each band, optionally only bins >= lo_bin.
    Returns (frames, BANDS) dB; bands with no bins left get -200."""
    p = 10.0 ** (pdb / 10.0)
    out = np.full((p.shape[0], BANDS), -200.0)
    for b in range(BANDS):
        a, e = EDGE[b], EDGE[b + 1]
        if lo_bin is not None:
            a = max(a, lo_bin)
        if a < e:
            out[:, b] = 10.0 * np.log10(p[:, a:e].mean(axis=1) + 1e-20)
    return out


def features(band_db, kc):
    """band_db: (frames, BANDS) dB of the band-limited input (full bands). kc: cutoff bin.
    Returns x (frames, N_IN), ref (frames,), target mask (BANDS,) of bands to predict."""
    avail = (EDGE[1:] <= kc).astype(np.float64)
    ref = band_db[:, REF_BANDS].mean(axis=1)
    rel = np.clip((band_db - ref[:, None]) / 20.0, -4.0, 2.0) * avail[None, :]
    lvl = np.clip((ref + 60.0) / 40.0, -2.0, 2.0)
    x = np.concatenate([rel, np.repeat(avail[None, :], len(ref), 0),
                        np.full((len(ref), 1), kc / BINS), lvl[:, None]], axis=1)
    target = (EDGE[1:] > kc)
    return x.astype(np.float32), ref, target


def sigmoid(v):
    return 1.0 / (1.0 + np.exp(-v))


class NumpyModel:
    """Same math as the torch model and the C++ code: Linear+ReLU -> GRU -> Linear."""

    def __init__(self, w):
        self.w = w

    def run(self, x):
        w = self.w
        h = np.zeros(w['gru_w_hh'].shape[1], dtype=np.float32)
        nh = h.shape[0]
        out = np.zeros((len(x), N_OUT), dtype=np.float32)
        a = np.maximum(0.0, x @ w['fc1_w'].T + w['fc1_b'])
        gi_all = a @ w['gru_w_ih'].T + w['gru_b_ih']
        for t in range(len(x)):
            gi = gi_all[t]
            gh = w['gru_w_hh'] @ h + w['gru_b_hh']
            r = sigmoid(gi[:nh] + gh[:nh])
            z = sigmoid(gi[nh:2 * nh] + gh[nh:2 * nh])
            n = np.tanh(gi[2 * nh:] + r * gh[2 * nh:])
            h = (1.0 - z) * n + z * h
            out[t] = w['fc2_w'] @ h + w['fc2_b']
        return out


def patch(X, kc, frame0=0):
    """Fill bins >= kc by copying the top part of the captured band upwards (a frequency
    shift by s bins rotates the phase by pi*s per hop)."""
    Y = X.copy()
    P = kc // 2
    k = np.arange(kc, BINS)
    j = (k - kc) % P
    src = kc - P + j
    s = k - src
    n = np.arange(frame0, frame0 + len(X))
    sign = np.where(((s[None, :] * n[:, None]) & 1) == 1, -1.0, 1.0)
    Y[:, kc:] = X[:, src] * sign
    return Y


def gains_to_bins(gdb, kc):
    """Per-band gains (dB) for the predicted bands -> per-bin gains for bins >= kc,
    interpolated between band centres (like BandLayout::expandGains)."""
    bands = [b for b in range(BANDS) if EDGE[b + 1] > kc]
    cb = np.array([max(CENTRE_BIN[b], kc) for b in bands], dtype=np.float64)
    k = np.arange(kc, BINS, dtype=np.float64)
    out = np.zeros((gdb.shape[0], len(k)))
    for t in range(gdb.shape[0]):
        out[t] = np.interp(k, cb, gdb[t, bands])
    return out


def reconstruct(Xlim, kc, pred_rel, ref, amount_db=0.0):
    """Xlim: STFT of the band-limited signal. pred_rel: model output (frames, BANDS) in
    units of 20 dB relative to ref. Returns the STFT with the high band filled in."""
    Y = patch(Xlim, kc)
    pdb = power_db(Y)
    have = band_density_db(pdb, lo_bin=kc)           # what the raw patch has per band
    want = pred_rel * 20.0 + ref[:, None] + amount_db
    gdb = np.clip(want - have, -60.0, 40.0)
    gb = gains_to_bins(gdb, kc)
    Y[:, kc:] *= 10.0 ** (gb / 20.0)
    return Y
