// ERB-spaced band layout over the 481 bins of DeepFilterNet's STFT (960-point window,
// 480 hop at 48 kHz). All of WarmVoice's spectral work happens inside that one transform.
// SPDX-License-Identifier: MIT
#pragma once
#include <array>
#include "wv/common.h"

namespace wv {

struct BandLayout {
    std::array<int, kBands + 1> edge{};     // band b covers bins [edge[b], edge[b+1])
    std::array<float, kBands> centreHz{};
    std::array<float, kBands> centreBin{};

    BandLayout();
    // Average power per band.
    void bandPower(const cplx* spec, float* outPow) const;
    // Expand per-band linear gains into per-bin gains with linear interpolation between
    // band centres (avoids "staircase" artefacts at band borders).
    void expandGains(const float* bandGain, float* binGain) const;
    int bandOfHz(float hz) const;
};

const BandLayout& bandLayout();

}  // namespace wv
