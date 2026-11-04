// WarmVoice (暖声) - shared constants and helpers.
// SPDX-License-Identifier: MIT
#pragma once
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>

namespace wv {

using cplx = std::complex<float>;

// Internal processing format. Everything inside the pipeline runs at 48 kHz mono.
// The STFT layout matches DeepFilterNet 3 (960-point Vorbis window, 480 hop) so the
// AI engine and our own spectral stage share one transform (no extra FFT, no extra latency).
constexpr int kFs = 48000;
constexpr int kHop = 480;            // 10 ms
constexpr int kFft = 960;            // 20 ms window
constexpr int kBins = kFft / 2 + 1;  // 481
constexpr int kBands = 32;           // ERB-spaced bands used by the spectral stage

constexpr float kPi = 3.14159265358979323846f;

inline float dbToLin(float db) { return std::pow(10.0f, db * 0.05f); }
inline float linToDb(float x) { return 20.0f * std::log10(x > 1e-12f ? x : 1e-12f); }
inline float powToDb(float p) { return 10.0f * std::log10(p > 1e-20f ? p : 1e-20f); }

// One-pole smoothing coefficient for a time constant in seconds at rate `rate` (Hz).
inline float onePoleCoef(float seconds, float rate) {
    if (seconds <= 0.0f) return 0.0f;
    return std::exp(-1.0f / (seconds * rate));
}

template <typename T>
inline T clampv(T v, T lo, T hi) { return v < lo ? lo : (v > hi ? hi : v); }

}  // namespace wv
