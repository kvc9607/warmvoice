// Biquad filters (RBJ cookbook designs, transposed direct form II).
// SPDX-License-Identifier: MIT
#pragma once
#include <cmath>
#include "wv/common.h"

namespace wv {

struct BiquadCoefs {
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;  // a0 normalised to 1

    // Magnitude response in dB at frequency f (Hz) for sample rate fs.
    float magDb(float f, float fs) const {
        const double w = 2.0 * 3.14159265358979323846 * f / fs;
        const std::complex<double> z1 = std::polar(1.0, -w), z2 = z1 * z1;
        const std::complex<double> num = (double)b0 + (double)b1 * z1 + (double)b2 * z2;
        const std::complex<double> den = 1.0 + (double)a1 * z1 + (double)a2 * z2;
        return (float)(20.0 * std::log10(std::abs(num) / std::abs(den) + 1e-30));
    }

    static BiquadCoefs identity() { return {}; }

    static BiquadCoefs fromRaw(double b0, double b1, double b2, double a0, double a1, double a2) {
        BiquadCoefs c;
        c.b0 = (float)(b0 / a0); c.b1 = (float)(b1 / a0); c.b2 = (float)(b2 / a0);
        c.a1 = (float)(a1 / a0); c.a2 = (float)(a2 / a0);
        return c;
    }

    static BiquadCoefs highpass(float f0, float q, float fs) {
        const double w = 2.0 * 3.14159265358979323846 * f0 / fs, cw = std::cos(w), al = std::sin(w) / (2.0 * q);
        return fromRaw((1 + cw) / 2, -(1 + cw), (1 + cw) / 2, 1 + al, -2 * cw, 1 - al);
    }
    static BiquadCoefs lowpass(float f0, float q, float fs) {
        const double w = 2.0 * 3.14159265358979323846 * f0 / fs, cw = std::cos(w), al = std::sin(w) / (2.0 * q);
        return fromRaw((1 - cw) / 2, 1 - cw, (1 - cw) / 2, 1 + al, -2 * cw, 1 - al);
    }
    static BiquadCoefs bandpass(float f0, float q, float fs) {  // 0 dB peak gain
        const double w = 2.0 * 3.14159265358979323846 * f0 / fs, cw = std::cos(w), al = std::sin(w) / (2.0 * q);
        return fromRaw(al, 0, -al, 1 + al, -2 * cw, 1 - al);
    }
    static BiquadCoefs peaking(float f0, float q, float gainDb, float fs) {
        const double A = std::pow(10.0, gainDb / 40.0);
        const double w = 2.0 * 3.14159265358979323846 * f0 / fs, cw = std::cos(w), al = std::sin(w) / (2.0 * q);
        return fromRaw(1 + al * A, -2 * cw, 1 - al * A, 1 + al / A, -2 * cw, 1 - al / A);
    }
    static BiquadCoefs lowShelf(float f0, float gainDb, float fs, float slope = 1.0f) {
        const double A = std::pow(10.0, gainDb / 40.0);
        const double w = 2.0 * 3.14159265358979323846 * f0 / fs, cw = std::cos(w), sw = std::sin(w);
        const double al = sw / 2.0 * std::sqrt((A + 1 / A) * (1 / slope - 1) + 2), sA = 2 * std::sqrt(A) * al;
        return fromRaw(A * ((A + 1) - (A - 1) * cw + sA), 2 * A * ((A - 1) - (A + 1) * cw),
                       A * ((A + 1) - (A - 1) * cw - sA), (A + 1) + (A - 1) * cw + sA,
                       -2 * ((A - 1) + (A + 1) * cw), (A + 1) + (A - 1) * cw - sA);
    }
    static BiquadCoefs highShelf(float f0, float gainDb, float fs, float slope = 1.0f) {
        const double A = std::pow(10.0, gainDb / 40.0);
        const double w = 2.0 * 3.14159265358979323846 * f0 / fs, cw = std::cos(w), sw = std::sin(w);
        const double al = sw / 2.0 * std::sqrt((A + 1 / A) * (1 / slope - 1) + 2), sA = 2 * std::sqrt(A) * al;
        return fromRaw(A * ((A + 1) + (A - 1) * cw + sA), -2 * A * ((A - 1) + (A + 1) * cw),
                       A * ((A + 1) + (A - 1) * cw - sA), (A + 1) - (A - 1) * cw + sA,
                       2 * ((A - 1) - (A + 1) * cw), (A + 1) - (A - 1) * cw - sA);
    }
};

struct Biquad {
    BiquadCoefs c;
    float z1 = 0, z2 = 0;

    inline float process(float x) {
        const float y = c.b0 * x + z1;
        z1 = c.b1 * x - c.a1 * y + z2;
        z2 = c.b2 * x - c.a2 * y;
        return y;
    }
    void processBlock(float* x, int n) {
        float s1 = z1, s2 = z2;
        const float b0 = c.b0, b1 = c.b1, b2 = c.b2, a1 = c.a1, a2 = c.a2;
        for (int i = 0; i < n; ++i) {
            const float in = x[i];
            const float y = b0 * in + s1;
            s1 = b1 * in - a1 * y + s2;
            s2 = b2 * in - a2 * y;
            x[i] = y;
        }
        // Flush denormals so a silent input never slows the audio thread down.
        z1 = std::fabs(s1) < 1e-25f ? 0.0f : s1;
        z2 = std::fabs(s2) < 1e-25f ? 0.0f : s2;
    }
    void reset() { z1 = z2 = 0; }
};

}  // namespace wv
