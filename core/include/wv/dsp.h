// Time-domain stages of the WarmVoice chain. All run at 48 kHz mono, allocate only on
// construction and process blocks in place.
// SPDX-License-Identifier: MIT
#pragma once
#include <array>
#include <vector>
#include "wv/biquad.h"
#include "wv/params.h"

namespace wv {

// ---- High-frequency restoration (blind bandwidth extension, DSP version) ----------------
// For microphones that lose everything above a cutoff fc (16/24/32 kHz links, Bluetooth,
// cheap USB codecs). The band [fc/2, fc] is full-wave rectified, which creates its
// harmonics in [fc, 2fc]; that new band is high-passed and its level is tied to the level
// of the source band, so it follows the voice instead of adding constant hiss.
class Restorer {
public:
    void setParams(float cutoffHz, float gain);
    void process(float* x, int n);
    void reset();

private:
    Biquad srcHp_, srcLp_, dcLp_, genHp1_, genHp2_, genLp_;
    float envSrc_ = 0, envGen_ = 0, g_ = 0, gain_ = 0, cut_ = 0;
    float envCoef_ = 0, gCoef_ = 0;
    bool on_ = false;
};

// ---- Texture: level-independent harmonic saturation with 2x oversampling -----------------
class Texture {
public:
    Texture();
    void setParams(float drive, float even, float odd, float mix);
    void process(float* x, int n);
    void reset();

private:
    float shape(float u) const;
    static constexpr int kTaps = 31;           // half-band FIR, odd length
    std::array<float, kTaps> h_{};
    std::array<float, kTaps + 1> upHist_{};       // 2 x 16 even-tap history
    std::array<float, 2 * kTaps> dnHist_{};
    std::array<float, 2 * kTaps> dryHist_{}, scaleHist_{};
    int upPos_ = 0, dnPos_ = 0, dryPos_ = 0;
    float env_ = 1e-4f, envCoef_ = 0;
    float drive_ = 0, a2_ = 0, a3_ = 0, mix_ = 0;
    Biquad dc_;
};

// ---- Dynamics: leveller (slow) + compressor (fast) + look-ahead limiter --------------------
// One detector feeds all three, and the combined gain is applied with a single multiply.
class Dynamics {
public:
    Dynamics();
    void setParams(const Resolved& r);
    // speech: whether this block contains speech (the leveller only learns from speech, so
    // it never pumps the background up during pauses).
    void process(float* x, int n, bool speech);
    void reset();
    float gainReductionDb() const { return grDb_; }
    float levelGainDb() const { return levGainDb_; }
    static constexpr int lookahead() { return kLook - 1; }

private:
    static constexpr int kLook = 64;   // 1.33 ms
    static constexpr int kBlock = 16;
    bool levelOn_ = true, compOn_ = true;
    float fixedGain_ = 0, target_ = -19, thr_ = -20, ratio_ = 2.5f, atk_ = 0, rel_ = 0, ceil_ = 0.891f;
    // leveller
    float levelEst_ = -120, levelPow_ = 0, levGainDb_ = 0;
    bool levInit_ = false;
    float initPow_ = 0;
    int initHops_ = 0;
    // compressor
    float compEnvDb_ = -120, compGainDb_ = 0, prevCompLin_ = 1, avgGrDb_ = 0, grDb_ = 0;
    // limiter
    std::array<float, kLook> delay_{}, boxBuf_{}, dqVal_{};
    std::array<long long, kLook> dqIdx_{};
    int dpos_ = 0, dqHead_ = 0, dqSize_ = 0;
    long long t_ = 0;
    float boxSum_ = kLook, limGain_ = 1, limRel_ = 0;
    std::vector<float> work_;
};

// ---- Rational polyphase resampler (any device rate <-> 48 kHz) ---------------------------
class Resampler {
public:
    void configure(int inRate, int outRate);
    bool passthrough() const { return L_ == 1 && M_ == 1; }
    // Consumes n input samples, appends output samples to out (returns count written).
    int process(const float* in, int n, float* out, int outCapacity);
    int maxOutput(int n) const { return (int)((long long)n * L_ / M_) + 2; }
    double latencyInputSamples() const { return passthrough() ? 0.0 : kTaps / 2.0; }
    void reset();

private:
    static constexpr int kTaps = 32;   // taps per phase
    int L_ = 1, M_ = 1;
    std::vector<float> bank_;          // L_ phases x kTaps
    std::array<float, 2 * kTaps> hist_{};
    int histPos_ = 0;
    long long phaseAcc_ = 0;           // position of the next output in units of 1/L input samples
};

}  // namespace wv
