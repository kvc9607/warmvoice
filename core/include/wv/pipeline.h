// The whole WarmVoice chain. One instance per microphone endpoint.
//
//   device rate, N ch --downmix--> resample to 48k --> [10 ms hops]
//     1 AI denoiser (DeepFilterNet 3); inside its STFT: spectral stage (dynamic tone fixes,
//       voice profile, input profile)
//     2 high-frequency restoration (only if the mic lost the highs)
//     3 EQ: rumble cut + 10-band auto bank (tone + smart correction) + user EQ nodes
//     4 texture (2x oversampled)
//     5 dynamics: leveller + compressor + limiter, one gain
//   --> resample back --> copy to all channels
//
// Every step does something the others do not; nothing is measured or transformed twice.
// If the AI is missing / fails / is too slow, step 1 becomes a plain delay (same latency)
// and the rest keeps working.
//
// configure() allocates; everything else is real-time safe (no locks, no allocation).
// SPDX-License-Identifier: MIT
#pragma once
#include <memory>
#include <string>
#include <vector>
#include "wv/ai_denoiser.h"
#include "wv/dsp.h"
#include "wv/params.h"
#include "wv/spectral.h"

namespace wv {

struct StageTiming {
    double ai = 0, restore = 0, eq = 0, texture = 0, dynamics = 0, total = 0;
    long long hops = 0;
};

struct PipelineOptions {
    std::string aiLibraryPath;     // empty = default name
    bool watchdog = true;          // stop the AI (or bypass) when the computer cannot keep up
};

class Pipeline {
public:
    Pipeline();
    ~Pipeline();
    // Not real-time safe. Returns a short status text.
    std::string configure(int deviceRate, int channels, const PipelineOptions& opt = PipelineOptions());
    void setParams(const Resolved& r);               // real-time safe
    void process(float* interleaved, int frames);    // real-time safe, in place
    int latencyFrames() const;                       // at the device rate
    void getAnalysis(Analysis& out) const;
    const StageTiming& timing() const { return timing_; }
    bool aiActive() const { return aiUsable(); }
    void resetProfile() { spectral_.resetProfile(); }
    void reset();
    bool lastSpeech() const { return speech_; }      // for traces
    float lastLevelGainDb() const { return dyn_.levelGainDb(); }

    // Delay of DeepFilterNet's transform + look-ahead: 480 + 960 samples at 48 kHz.
    static constexpr int kAiDelay = (kFft - kHop) + AiDenoiser::kExtraLatency;

private:
    void processHop(float* hop);
    bool aiUsable() const { return ai_ && !aiFailed_ && !aiSlow_; }

    int rate_ = kFs, channels_ = 1;
    PipelineOptions opt_;
    Resolved p_;
    std::unique_ptr<AiDenoiser> ai_;
    int aiProblem_ = 0;
    bool aiFailed_ = false, aiSlow_ = false, overloaded_ = false, speech_ = false;
    SpectralProcessor spectral_;
    Restorer restore_;
    Biquad hpf_, eqBank_[kEqBank], userEq_[kEqNodes];
    Texture texture_;
    Dynamics dyn_;
    Resampler rsIn_, rsOut_;
    // buffers
    std::vector<float> mono_, inFifo_, outFifo_, hopIn_, hopOut_, aiDelay_, bypass_;
    int inCount_ = 0, outRead_ = 0, outCount_ = 0, aiDelayPos_ = 0, bypassPos_ = 0, chainDelay_ = 0;
    float mixWet_ = 1.0f;
    // meters / watchdog
    float inPeak_ = 0, inSpeechDb_ = -120, noiseDb_ = -120, outDb_ = -120, loadEma_ = 0;
    int slowHops_ = 0, speechHold_ = 0;
    StageTiming timing_;
};

}  // namespace wv
