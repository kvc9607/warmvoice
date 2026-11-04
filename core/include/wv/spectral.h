// Spectral stage. It runs inside the AI denoiser's own STFT (DeepFilterNet calls it back
// for every 10 ms hop with the enhanced and the noisy spectrum), so it costs no extra
// transform and no extra delay. The AI already removes the noise - including during
// pauses - so there is no separate gate or voice detector here; this stage only does what
// the AI does not:
//   * dynamic tone fixes: harshness / boxiness / sibilance, cut only when they stick out
//     compared with this speaker's own long-term profile
//   * voice profile (long-term spectrum of the cleaned voice) for the smart optimisation
//   * input profile (long-term spectrum of the raw input) to detect a mic that loses highs
// SPDX-License-Identifier: MIT
#pragma once
#include <array>
#include "wv/bands.h"
#include "wv/params.h"

namespace wv {

class SpectralProcessor {
public:
    SpectralProcessor();
    void setParams(const Resolved& r);
    // enh: modified in place. noisy: time-aligned input spectrum. speech: the AI's decision.
    // powScale converts the engine's spectrum scaling to "0 dB = full-scale sine".
    void process(cplx* enh, const cplx* noisy, bool speech, float powScale);
    void reset();
    void resetProfile();

    const std::array<float, kBands>& ltasPow() const { return ltas_; }
    const std::array<float, kBands>& rawLtasPow() const { return rawLtas_; }
    uint32_t profileFrames() const { return profileFrames_; }
    uint32_t rawFrames() const { return rawFrames_; }

private:
    const BandLayout& L_;
    bool optimizeOn_ = true;
    float dynRatio_ = 0.8f, dynMaxCut_ = 7.5f;
    std::array<float, kBands> eqResp_{}, thr_{};
    std::array<float, kBands> devRef_{}, devVar_{}, cutDb_{}, ltas_{}, rawLtas_{};
    float sibRef_ = 0.0f, sibVar_ = 9.0f;
    uint32_t refFrames_ = 0, profileFrames_ = 0, rawFrames_ = 0;
    std::array<float, kBins> binGain_{};
    std::array<float, kBands> bandGain_{};
    int sibLo_, sibHi_, bodyLo_, bodyHi_;
};

}  // namespace wv
