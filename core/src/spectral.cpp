// SPDX-License-Identifier: MIT
#include "wv/spectral.h"

#include <algorithm>
#include <cmath>

namespace wv {

namespace {
// Scale so that a full-scale sine reads about 0 dB in band power.
float spectralNorm() {
    static const float k = [] {
        double wsum = 0;
        for (int n = 0; n < kFft; ++n) {
            const double s = std::sin(3.14159265358979323846 * (n + 0.5) / kFft);
            wsum += std::sin(0.5 * 3.14159265358979323846 * s * s);
        }
        return (float)(4.0 / (wsum * wsum));
    }();
    return k;
}
constexpr float kCutRelease = 0.88f;     // ~80 ms
constexpr uint32_t kRefFrames = 2000;    // speaker reference adapts over ~20 s of speech
constexpr uint32_t kWarmup = 100;        // no dynamic cuts during the first second of speech
constexpr uint32_t kProfileFrames = 6000;
}  // namespace

SpectralProcessor::SpectralProcessor() : L_(bandLayout()) {
    sibLo_ = L_.bandOfHz(4500.0f);
    sibHi_ = L_.bandOfHz(10500.0f);
    bodyLo_ = L_.bandOfHz(300.0f);
    bodyHi_ = L_.bandOfHz(3000.0f);
    for (int b = 0; b < kBands; ++b) {
        const float hz = L_.centreHz[b];
        thr_[b] = (hz >= 2000.0f && hz <= 9500.0f) ? 4.5f : (hz < 150.0f ? 8.0f : 6.0f);
    }
    reset();
}

void SpectralProcessor::reset() {
    cutDb_.fill(0.0f);
    devRef_.fill(0.0f);
    devVar_.fill(9.0f);
    sibRef_ = 0.0f;
    sibVar_ = 9.0f;
    refFrames_ = 0;
    resetProfile();
}

void SpectralProcessor::resetProfile() {
    ltas_.fill(0.0f);
    rawLtas_.fill(0.0f);
    profileFrames_ = rawFrames_ = 0;
}

void SpectralProcessor::setParams(const Resolved& r) {
    optimizeOn_ = r.optimizeOn != 0;
    dynRatio_ = r.dynRatio;
    dynMaxCut_ = r.dynMaxCutDb;
    for (int b = 0; b < kBands; ++b) eqResp_[b] = r.eqRespDb[b];
}

void SpectralProcessor::process(cplx* spec, const cplx* noisy, bool speech, float powScale) {
    const float norm = spectralNorm() * powScale;
    float pow[kBands];
    L_.bandPower(spec, pow);
    for (int b = 0; b < kBands; ++b) pow[b] *= norm;

    // ---- input profile: raw spectrum, every frame with signal (used to find a mic that
    //      stops at some frequency; noise is included on purpose) ----
    if (noisy) {
        float raw[kBands];
        L_.bandPower(noisy, raw);
        float tot = 0;
        for (int b = 0; b < kBands; ++b) { raw[b] *= norm; tot += raw[b]; }
        if (tot > 1e-12f) {
            ++rawFrames_;
            const float a = 1.0f / (float)std::min(rawFrames_, kProfileFrames);
            for (int b = 0; b < kBands; ++b) rawLtas_[b] += a * (raw[b] - rawLtas_[b]);
        }
    }

    // ---- dynamic tone fixes (only while talking) ----
    bool anyCut = false;
    if (optimizeOn_ && speech) {
        float lv[kBands];
        for (int b = 0; b < kBands; ++b) lv[b] = powToDb(pow[b]) + eqResp_[b];
        ++refFrames_;
        const float a = 1.0f / (float)std::min(refFrames_, kRefFrames);
        float sibSum = 0, bodySum = 0;
        for (int b = sibLo_; b <= sibHi_; ++b) sibSum += lv[b];
        for (int b = bodyLo_; b <= bodyHi_; ++b) bodySum += lv[b];
        const float sib = sibSum / (float)(sibHi_ - sibLo_ + 1) - bodySum / (float)(bodyHi_ - bodyLo_ + 1);
        const float sd = sib - sibRef_;
        sibRef_ += a * sd;
        sibVar_ += a * (sd * sd - sibVar_);
        const float sibExcess = sd - std::max(3.0f, 1.5f * std::sqrt(sibVar_));
        for (int b = 0; b < kBands; ++b) {
            const int lo = std::max(0, b - 2), hi = std::min(kBands - 1, b + 2);
            float env = 0;
            for (int j = lo; j <= hi; ++j) env += lv[j];
            env /= (float)(hi - lo + 1);
            const float dev = lv[b] - env - devRef_[b];
            devRef_[b] += a * dev;
            devVar_[b] += a * (dev * dev - devVar_[b]);
            float tgt = 0.0f;
            if (refFrames_ > kWarmup) {
                const float excess = dev - std::max(thr_[b], 1.5f * std::sqrt(devVar_[b]));
                tgt = std::min(dynMaxCut_, dynRatio_ * std::max(0.0f, excess));
                if (b >= sibLo_ && b <= sibHi_)
                    tgt = std::min(dynMaxCut_ + 3.0f, tgt + dynRatio_ * std::max(0.0f, sibExcess));
            }
            cutDb_[b] = tgt > cutDb_[b] ? tgt : tgt + (cutDb_[b] - tgt) * kCutRelease;
            anyCut |= cutDb_[b] > 0.05f;
        }
        // ---- voice profile ----
        ++profileFrames_;
        const float ap = 1.0f / (float)std::min(profileFrames_, kProfileFrames);
        for (int b = 0; b < kBands; ++b) ltas_[b] += ap * (pow[b] - ltas_[b]);
    } else {
        for (int b = 0; b < kBands; ++b) {
            cutDb_[b] *= kCutRelease;
            anyCut |= cutDb_[b] > 0.05f;
        }
    }
    if (!anyCut) return;   // nothing to change: skip the per-bin multiply entirely
    for (int b = 0; b < kBands; ++b) bandGain_[b] = dbToLin(-cutDb_[b]);
    L_.expandGains(bandGain_.data(), binGain_.data());
    for (int k = 0; k < kBins; ++k) spec[k] *= binGain_[k];
}

}  // namespace wv
