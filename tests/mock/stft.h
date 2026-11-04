// STFT used only by the test stand-in for the AI library (the real engine has its own).
// Same layout as DeepFilterNet 3: Vorbis window 960, hop 480.
// SPDX-License-Identifier: MIT
#pragma once
#include <vector>
#include "fft.h"
#include "wv/common.h"

namespace wv {

class Stft {
public:
    Stft();
    void analyse(const float* hopIn, cplx* specOut);
    void synthesise(const cplx* spec, float* hopOut);
    static constexpr int latencySamples() { return kFft - kHop; }
    void reset();

private:
    RealFft fft_;
    std::vector<float> win_, inBuf_, frame_, olaBuf_;
};

}  // namespace wv
