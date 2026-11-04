// AI noise suppression (DeepFilterNet 3) loaded at run time from a separate library. If it
// is missing, fails, or the computer is too slow, the chain keeps running without denoising
// (everything else still works) and reports why, so audio never stops.
//
// The library is DeepFilterNet's libDF built with our small patch (dfn/libdf-hook.patch),
// which adds wv_df_* functions: our spectral stage is called back *inside* DeepFilterNet's
// own STFT, so both share one transform.
// SPDX-License-Identifier: MIT
#pragma once
#include <memory>
#include <string>
#include "wv/spectral.h"

namespace wv {

class AiDenoiser {
public:
    // Returns nullptr when the library cannot be loaded. `path` empty = default name next to
    // the executable / in the library search path.
    static std::unique_ptr<AiDenoiser> create(const std::string& path = std::string(), std::string* err = nullptr);
    ~AiDenoiser();

    // One 10 ms hop. Returns false if the engine reported an internal error.
    bool process(const float* in, float* out, SpectralProcessor& sp, float attenLimitDb);
    // Whether the last hop contained speech (from the AI's own SNR estimate).
    bool speech() const { return speech_; }
    void reset();

    // DeepFilterNet 3 looks two hops ahead.
    static constexpr int kExtraLatency = 2 * kHop;

private:
    AiDenoiser() = default;
    struct Api;
    std::unique_ptr<Api> api_;
    void* state_ = nullptr;
    float atten_ = -1.0f;
    bool speech_ = false;
};

}  // namespace wv
