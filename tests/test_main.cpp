// WarmVoice unit tests (no external framework).
// SPDX-License-Identifier: MIT
#include <cmath>
#include <cstdio>
#include <functional>
#include <random>
#include <string>
#include <vector>

#include "wv/biquad.h"
#include "fft.h"
#include "stft.h"
#include "wv/bands.h"

using namespace wv;

static int gFail = 0, gPass = 0;
#define CHECK(cond, ...)                                              \
    do {                                                              \
        if (cond) { ++gPass; }                                        \
        else { ++gFail; std::printf("FAIL %s:%d: ", __FILE__, __LINE__); \
               std::printf(__VA_ARGS__); std::printf("\n"); }         \
    } while (0)

static void testFft() {
    for (int n : {8, 30, 64, 120, 480, 960}) {
        RealFft f(n);
        std::mt19937 rng(n);
        std::uniform_real_distribution<float> d(-1, 1);
        std::vector<float> x(n), y(n);
        for (auto& v : x) v = d(rng);
        std::vector<cplx> X(n / 2 + 1);
        f.forward(x.data(), X.data());
        // Compare with a naive DFT.
        double maxErr = 0;
        for (int k = 0; k <= n / 2; ++k) {
            std::complex<double> acc = 0;
            for (int t = 0; t < n; ++t) acc += (double)x[t] * std::polar(1.0, -2 * M_PI * k * t / n);
            maxErr = std::max(maxErr, std::abs(acc - std::complex<double>(X[k].real(), X[k].imag())));
        }
        CHECK(maxErr < 1e-3 * n, "fft n=%d err=%g", n, maxErr);
        f.inverse(X.data(), y.data());
        double e2 = 0;
        for (int t = 0; t < n; ++t) e2 = std::max(e2, (double)std::fabs(x[t] - y[t]));
        CHECK(e2 < 1e-5, "ifft n=%d err=%g", n, e2);
    }
}

static void testStftReconstruction() {
    Stft s;
    std::mt19937 rng(1);
    std::normal_distribution<float> d(0, 0.3f);
    const int hops = 50;
    std::vector<float> in(hops * kHop), out(hops * kHop);
    for (auto& v : in) v = d(rng);
    std::vector<cplx> spec(kBins);
    for (int h = 0; h < hops; ++h) {
        s.analyse(&in[h * kHop], spec.data());
        s.synthesise(spec.data(), &out[h * kHop]);
    }
    const int lat = Stft::latencySamples();
    double maxErr = 0;
    for (int i = kFft; i < hops * kHop; ++i) maxErr = std::max(maxErr, (double)std::fabs(out[i] - in[i - lat]));
    CHECK(maxErr < 1e-5, "stft reconstruction err=%g", maxErr);
}

static void testBands() {
    const BandLayout& L = bandLayout();
    CHECK(L.edge[0] == 0 && L.edge[kBands] == kBins, "band edges cover all bins");
    bool mono = true;
    for (int b = 0; b < kBands; ++b) mono &= L.edge[b + 1] - L.edge[b] >= 2;
    CHECK(mono, "every band has >= 2 bins");
    std::printf("  bands Hz:");
    for (int b = 0; b < kBands; ++b) std::printf(" %.0f", L.centreHz[b]);
    std::printf("\n");
}

static void testBiquad() {
    auto c = BiquadCoefs::peaking(1000, 1.0f, 6.0f, 48000);
    CHECK(std::fabs(c.magDb(1000, 48000) - 6.0f) < 0.01f, "peaking centre gain %f", c.magDb(1000, 48000));
    auto hp = BiquadCoefs::highpass(80, 0.707f, 48000);
    CHECK(std::fabs(hp.magDb(80, 48000) + 3.01f) < 0.05f, "hp -3dB at fc: %f", hp.magDb(80, 48000));
}

extern void runPipelineTests(int& pass, int& fail);  // tests_pipeline.cpp

int main() {
    testFft();
    testStftReconstruction();
    testBands();
    testBiquad();
    runPipelineTests(gPass, gFail);
    std::printf("%d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
