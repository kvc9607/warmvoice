// SPDX-License-Identifier: MIT
#include "stft.h"

#include <algorithm>
#include <cmath>

namespace wv {

Stft::Stft() : fft_(kFft), win_(kFft), inBuf_(kFft, 0.0f), frame_(kFft), olaBuf_(kFft, 0.0f) {
    // Vorbis window: w(n)^2 + w(n + N/2)^2 = 1, so analysis+synthesis with the same window
    // and 50 % overlap reconstructs the input exactly.
    for (int n = 0; n < kFft; ++n) {
        const double s = std::sin(3.14159265358979323846 * (n + 0.5) / kFft);
        win_[n] = (float)std::sin(0.5 * 3.14159265358979323846 * s * s);
    }
}

void Stft::reset() {
    std::fill(inBuf_.begin(), inBuf_.end(), 0.0f);
    std::fill(olaBuf_.begin(), olaBuf_.end(), 0.0f);
}

void Stft::analyse(const float* hopIn, cplx* specOut) {
    std::copy(inBuf_.begin() + kHop, inBuf_.end(), inBuf_.begin());
    std::copy(hopIn, hopIn + kHop, inBuf_.begin() + (kFft - kHop));
    for (int n = 0; n < kFft; ++n) frame_[n] = inBuf_[n] * win_[n];
    fft_.forward(frame_.data(), specOut);
}

void Stft::synthesise(const cplx* spec, float* hopOut) {
    fft_.inverse(spec, frame_.data());
    for (int n = 0; n < kFft; ++n) olaBuf_[n] += frame_[n] * win_[n];
    std::copy(olaBuf_.begin(), olaBuf_.begin() + kHop, hopOut);
    std::copy(olaBuf_.begin() + kHop, olaBuf_.end(), olaBuf_.begin());
    std::fill(olaBuf_.begin() + (kFft - kHop), olaBuf_.end(), 0.0f);
}

}  // namespace wv
