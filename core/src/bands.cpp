// SPDX-License-Identifier: MIT
#include "wv/bands.h"

#include <cmath>

namespace wv {

static double freqToErb(double f) { return 9.265 * std::log(1.0 + f / (24.7 * 9.265)); }
static double erbToFreq(double e) { return 24.7 * 9.265 * (std::exp(e / 9.265) - 1.0); }

BandLayout::BandLayout() {
    const double binHz = (double)kFs / kFft;  // 50 Hz
    const double lo = freqToErb(0.0), hi = freqToErb(kFs / 2.0);
    const double step = (hi - lo) / kBands;
    const int minBins = 2;
    int prev = 0, over = 0;
    std::array<int, kBands> width{};
    for (int b = 0; b < kBands; ++b) {
        const int fb = (int)std::lround(erbToFreq(lo + (b + 1) * step) / binHz);
        int w = fb - prev - over;
        if (w < minBins) { over = minBins - w; w = minBins; } else { over = 0; }
        width[b] = w;
        prev = fb;
    }
    // Make the widths cover exactly kBins bins (Nyquist bin included in the last band).
    int sum = 0;
    for (int w : width) sum += w;
    width[kBands - 1] += kBins - sum;
    edge[0] = 0;
    for (int b = 0; b < kBands; ++b) edge[b + 1] = edge[b] + width[b];
    for (int b = 0; b < kBands; ++b) {
        centreBin[b] = 0.5f * (float)(edge[b] + edge[b + 1] - 1);
        centreHz[b] = centreBin[b] * (float)binHz;
    }
}

void BandLayout::bandPower(const cplx* spec, float* outPow) const {
    for (int b = 0; b < kBands; ++b) {
        float acc = 0.0f;
        for (int k = edge[b]; k < edge[b + 1]; ++k) acc += std::norm(spec[k]);
        outPow[b] = acc / (float)(edge[b + 1] - edge[b]);
    }
}

void BandLayout::expandGains(const float* g, float* binGain) const {
    int b = 0;
    for (int k = 0; k < kBins; ++k) {
        const float kf = (float)k;
        if (kf <= centreBin[0]) { binGain[k] = g[0]; continue; }
        if (kf >= centreBin[kBands - 1]) { binGain[k] = g[kBands - 1]; continue; }
        while (b < kBands - 2 && kf > centreBin[b + 1]) ++b;
        const float t = (kf - centreBin[b]) / (centreBin[b + 1] - centreBin[b]);
        binGain[k] = g[b] + t * (g[b + 1] - g[b]);
    }
}

int BandLayout::bandOfHz(float hz) const {
    const int bin = clampv((int)std::lround(hz / ((float)kFs / kFft)), 0, kBins - 1);
    for (int b = 0; b < kBands; ++b)
        if (bin < edge[b + 1]) return b;
    return kBands - 1;
}

const BandLayout& bandLayout() {
    static const BandLayout layout;
    return layout;
}

}  // namespace wv
