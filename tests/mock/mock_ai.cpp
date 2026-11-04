// Test stand-in for the AI library (warmvoice_ai). It exports the same wv_df_* functions as
// DeepFilterNet built with dfn/libdf-hook.patch and behaves the same way on the outside:
// same STFT layout and spectrum scaling (1/960), same two-hop look-ahead (so the same
// delay), a local-SNR value per hop, and the spectral hook with enhanced + noisy spectra.
// Its "denoising" is a plain Wiener filter - it exists to test the plumbing of the chain,
// not the sound. The sound of the real AI is measured in CI with the real library.
//
// Test hook: an input hop whose first sample is exactly 12345.0 makes it report an error.
// SPDX-License-Identifier: MIT
#include <algorithm>
#include <cmath>
#include <vector>

#include "stft.h"

#if defined(_WIN32)
#define WV_EXPORT extern "C" __declspec(dllexport)
#else
#define WV_EXPORT extern "C" __attribute__((visibility("default")))
#endif

using namespace wv;

typedef void (*wv_df_spec_hook)(void* user, float* enh, const float* noisy, size_t nFreqs, float lsnr);

namespace {
struct Mock {
    Stft stft;
    std::vector<cplx> ring[3];     // newest .. 2 hops old
    std::vector<float> noise;
    float lim = 1e-5f;
    int frame = 0;
    Mock() : noise(kBins, 0.0f) { for (auto& r : ring) r.assign(kBins, cplx()); }
};
}  // namespace

WV_EXPORT void* wv_df_create(float attenLimDb) {
    auto* m = new Mock();
    m->lim = std::pow(10.0f, -attenLimDb / 20.0f);
    return m;
}

WV_EXPORT void wv_df_set_atten_lim(void* st, float db) {
    if (st) static_cast<Mock*>(st)->lim = std::pow(10.0f, -db / 20.0f);
}

WV_EXPORT void wv_df_free(void* st) { delete static_cast<Mock*>(st); }

WV_EXPORT float wv_df_process(void* st, const float* in, float* out, wv_df_spec_hook hook, void* user) {
    if (!st || !in || !out) return NAN;
    if (in[0] == 12345.0f) return NAN;
    Mock& m = *static_cast<Mock*>(st);
    // newest frame, DeepFilterNet scaling
    std::rotate(std::begin(m.ring), std::begin(m.ring) + 2, std::end(m.ring));   // [2]->[0]
    std::vector<cplx>& newest = m.ring[0];
    m.stft.analyse(in, newest.data());
    float sp = 0, sn = 0;
    for (int k = 0; k < kBins; ++k) {
        newest[k] *= 1.0f / kFft;
        const float P = std::norm(newest[k]) + 1e-20f;
        if (m.frame < 10) m.noise[k] = m.frame == 0 ? P : m.noise[k] + (P - m.noise[k]) / (m.frame + 1);
        // running mean of the noise, ignoring bins that clearly hold speech
        else m.noise[k] = P < 3.0f * m.noise[k] ? 0.98f * m.noise[k] + 0.02f * P : m.noise[k] * 1.001f;
        if (k > 6 && k < 160) { sp += P; sn += m.noise[k]; }
    }
    ++m.frame;
    const float lsnr = std::clamp(10.0f * std::log10(std::max(sp - sn, 1e-20f) / (sn + 1e-20f)), -15.0f, 35.0f);
    // the frame leaving now is two hops old (look-ahead)
    const std::vector<cplx>& noisy = m.ring[2];
    std::vector<cplx> enh(kBins);
    for (int k = 0; k < kBins; ++k) {
        const float P = std::norm(noisy[k]) + 1e-20f;
        const float snr = std::max(P / (2.0f * m.noise[k] + 1e-20f) - 1.0f, 0.0f);
        enh[k] = noisy[k] * std::max(snr / (1.0f + snr), m.lim);
    }
    if (hook) hook(user, reinterpret_cast<float*>(enh.data()), reinterpret_cast<const float*>(noisy.data()), kBins, lsnr);
    for (auto& v : enh) v *= (float)kFft;
    m.stft.synthesise(enh.data(), out);
    return lsnr;
}
