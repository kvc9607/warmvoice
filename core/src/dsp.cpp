// SPDX-License-Identifier: MIT
#include "wv/dsp.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace wv {

static double besselI0(double x) {
    double s = 1, t = 1;
    for (int k = 1; k < 30; ++k) { t *= (x / (2 * k)) * (x / (2 * k)); s += t; }
    return s;
}

// =========================================================================== Restorer
void Restorer::setParams(float cutoffHz, float gain) {
    const bool on = cutoffHz > 2000.0f && cutoffHz < 20000.0f && gain > 0.0f;
    if (on && (std::fabs(cutoffHz - cut_) > 1.0f || !on_)) {
        const float fs = (float)kFs;
        srcHp_.c = BiquadCoefs::highpass(cutoffHz * 0.5f, 0.707f, fs);
        srcLp_.c = BiquadCoefs::lowpass(cutoffHz * 0.92f, 0.707f, fs);
        genHp1_.c = BiquadCoefs::highpass(cutoffHz, 0.5412f, fs);   // 4th-order Butterworth
        genHp2_.c = BiquadCoefs::highpass(cutoffHz, 1.3066f, fs);
        genLp_.c = BiquadCoefs::lowpass(std::min(2.0f * cutoffHz, 20000.0f) * 0.95f, 0.707f, fs);
        envCoef_ = onePoleCoef(0.005f, fs);
        gCoef_ = onePoleCoef(0.010f, fs);
        reset();
    }
    on_ = on;
    cut_ = cutoffHz;
    gain_ = gain;
}

void Restorer::reset() {
    srcHp_.reset(); srcLp_.reset(); genHp1_.reset(); genHp2_.reset(); genLp_.reset();
    envSrc_ = envGen_ = g_ = 0;
}

void Restorer::process(float* x, int n) {
    if (!on_) return;
    for (int i = 0; i < n; ++i) {
        const float s = srcLp_.process(srcHp_.process(x[i]));
        const float gen = genLp_.process(genHp2_.process(genHp1_.process(std::fabs(s))));
        envSrc_ = envCoef_ * envSrc_ + (1 - envCoef_) * s * s;
        envGen_ = envCoef_ * envGen_ + (1 - envCoef_) * gen * gen;
        const float target = std::min(8.0f, gain_ * std::sqrt(envSrc_ / (envGen_ + 1e-14f)));
        g_ = gCoef_ * g_ + (1 - gCoef_) * target;
        x[i] += g_ * gen;
    }
    if (envSrc_ < 1e-20f) envSrc_ = 0;
    if (envGen_ < 1e-20f) envGen_ = 0;
}

// =========================================================================== Texture
Texture::Texture() {
    // Kaiser-windowed half-band low-pass (cut-off at a quarter of the 96 kHz rate).
    const int c = kTaps / 2;
    const double beta = 7.0, i0b = besselI0(beta);
    for (int k = 0; k < kTaps; ++k) {
        const int d = k - c;
        double v;
        if (d == 0) v = 0.5;
        else if (d % 2 == 0) v = 0.0;
        else v = std::sin(3.14159265358979323846 * d / 2.0) / (3.14159265358979323846 * d);
        const double r = (double)d / c;
        h_[k] = (float)(v * besselI0(beta * std::sqrt(std::max(0.0, 1 - r * r))) / i0b);
    }
    dc_.c = BiquadCoefs::highpass(20.0f, 0.707f, (float)kFs);
    envCoef_ = onePoleCoef(0.03f, (float)kFs);
    reset();
}

void Texture::reset() {
    upHist_.fill(0); dnHist_.fill(0); dryHist_.fill(0); scaleHist_.fill(0);
    upPos_ = dnPos_ = dryPos_ = 0;
    env_ = 1e-4f;
    dc_.reset();
}

void Texture::setParams(float drive, float even, float odd, float mix) {
    drive_ = 0.5f + 1.5f * clampv(drive, 0.0f, 1.0f);
    a2_ = 0.2f * even;
    a3_ = -0.08f * odd;
    mix_ = clampv(mix, 0.0f, 1.0f);
}

inline float Texture::shape(float u) const {
    u = clampv(u, -2.0f, 2.0f);
    return u + a2_ * u * u + a3_ * u * u * u;
}

void Texture::process(float* x, int n) {
    if (mix_ <= 0.0f) return;
    // Half-band polyphase form: half of the taps are zero and half of the up-sampled input is
    // zero, so only the non-zero products are computed (33 multiplies per sample instead of 93).
    //   up:   y[2n]   = 2 * sum_j h[2j] * u[n-j]        (16 taps)
    //         y[2n+1] = 2 * h[c] * u[n-7] = u[n-7]       (centre tap only)
    //   down: v[n]    = sum_j h[2j] * w[2n-2j] + h[c] * w[2n-c]
    constexpr int C = kTaps / 2;          // 15
    constexpr int NE = C + 1;             // 16 even taps
    for (int i = 0; i < n; ++i) {
        const float in = x[i];
        env_ = envCoef_ * env_ + (1 - envCoef_) * in * in;
        const float scale = 3.0f * std::sqrt(env_) + 1e-5f;     // speech RMS -> about 1/3
        const float u = in / scale * drive_;
        // history of u (duplicated ring, newest at upPos_ + NE)
        upHist_[upPos_] = u;
        upHist_[upPos_ + NE] = u;
        const float* un = &upHist_[upPos_ + NE];
        upPos_ = (upPos_ + 1) % NE;
        float e = 0;
        for (int j = 0; j < NE; ++j) e += h_[2 * j] * un[-j];
        const float yEven = 2.0f * e;
        const float yOdd = un[-(C / 2)];   // u[n-7]
        // shape both 96 kHz samples. Decimate on the EVEN sample: output n then sits at
        // 96 kHz position 2n - 30, i.e. a whole 15 samples behind at 48 kHz (decimating on the
        // odd sample would leave a half-sample offset against the dry signal).
        dnHist_[dnPos_] = shape(yEven);
        dnHist_[dnPos_ + kTaps] = dnHist_[dnPos_];
        const float* wn = &dnHist_[dnPos_ + kTaps];   // newest = w[2n]
        dnPos_ = (dnPos_ + 1) % kTaps;
        // taps k on w[2n-k]; non-zero for even k and for the centre tap
        float v = h_[C] * wn[-C];
        for (int j = 0; j < NE; ++j) v += h_[2 * j] * wn[-2 * j];
        dnHist_[dnPos_] = shape(yOdd);
        dnHist_[dnPos_ + kTaps] = dnHist_[dnPos_];
        dnPos_ = (dnPos_ + 1) % kTaps;
        // dry signal and the level scale, both delayed to match (15 samples at 48 kHz)
        dryHist_[dryPos_] = in;
        dryHist_[dryPos_ + kTaps] = in;
        scaleHist_[dryPos_] = scale;
        scaleHist_[dryPos_ + kTaps] = scale;
        const float dry = dryHist_[dryPos_ + kTaps - C];
        const float sc = scaleHist_[dryPos_ + kTaps - C];
        dryPos_ = (dryPos_ + 1) % kTaps;
        // only the added harmonics are DC-blocked (the u^2 term makes DC); the voice itself
        // passes untouched
        const float added = dc_.process(v * sc / drive_ - dry);
        x[i] = dry + mix_ * added;
    }
}

// =========================================================================== Dynamics
Dynamics::Dynamics() { reset(); }

void Dynamics::reset() {
    levelEst_ = -120; levelPow_ = 0; levGainDb_ = 0; levInit_ = false; initPow_ = 0; initHops_ = 0;
    compEnvDb_ = -120; compGainDb_ = 0; prevCompLin_ = 1; avgGrDb_ = 0; grDb_ = 0;
    delay_.fill(0); boxBuf_.fill(1);
    boxSum_ = (float)kLook;
    dpos_ = dqHead_ = dqSize_ = 0;
    t_ = 0;
    limGain_ = 1;
}

void Dynamics::setParams(const Resolved& r) {
    levelOn_ = r.levelOn != 0;
    compOn_ = r.compOn != 0;
    target_ = r.levelTargetDb;
    fixedGain_ = r.fixedGainDb;
    thr_ = r.compThresholdDb;
    ratio_ = std::max(1.0f, r.compRatio);
    const float blockRate = (float)kFs / kBlock;
    atk_ = onePoleCoef(r.compAttack, blockRate);
    rel_ = onePoleCoef(r.compRelease, blockRate);
    ceil_ = r.ceiling;
    limRel_ = onePoleCoef(0.05f, (float)kFs);
}

void Dynamics::process(float* x, int n, bool speech) {
    // ---- leveller: feedback on what the listener actually gets ----
    // It measures the output (after the compressor, before the limiter) during speech and
    // nudges its gain toward the target, so compression never needs a separate make-up gain.
    float ms = 0;
    for (int i = 0; i < n; ++i) ms += x[i] * x[i];
    const float inPw = ms / (float)std::max(1, n);
    const float prevLev = levGainDb_;
    // Start only after 0.3 s of accumulated speech, so a single key click or cough that
    // was taken for speech cannot set a huge starting gain.
    if (levelOn_ && speech && powToDb(inPw) > -70.0f && !levInit_) {
        initPow_ += inPw;
        if (++initHops_ >= 30) {
            levGainDb_ = clampv(target_ - powToDb(initPow_ / (float)initHops_), -12.0f, 12.0f);  // first guess
            levelPow_ = dbToLin(target_) * dbToLin(target_);
            levInit_ = true;
        }
    }
    if (!levelOn_) levGainDb_ = fixedGain_;
    const float levA = levInit_ || !levelOn_ ? dbToLin(prevLev) : 1.0f;
    const float levB = levInit_ || !levelOn_ ? dbToLin(levGainDb_) : 1.0f;

    float minLim = 1.0f, outMs = 0;
    for (int b0 = 0; b0 < n; b0 += kBlock) {
        const int bn = std::min(kBlock, n - b0);
        float bms = 0;
        for (int i = 0; i < bn; ++i) {
            const float t = (float)(b0 + i) / (float)n;
            const float v = x[b0 + i] * (levA + (levB - levA) * t);
            x[b0 + i] = v;
            bms += v * v;
        }
        float compLin = 1.0f;
        if (compOn_) {
            const float db = powToDb(bms / (float)bn);
            const float c = db > compEnvDb_ ? atk_ : rel_;
            compEnvDb_ = c * compEnvDb_ + (1 - c) * db;
            const float over = compEnvDb_ - thr_;
            const float slope = 1.0f / ratio_ - 1.0f;
            float gr = 0;
            if (over >= 3.0f) gr = slope * over;
            else if (over > -3.0f) gr = slope * (over + 3.0f) * (over + 3.0f) / 12.0f;
            compGainDb_ = gr;
            compLin = dbToLin(gr);
        }
        for (int i = 0; i < bn; ++i) {
            const float t = (float)(i + 1) / (float)bn;
            const float v = x[b0 + i] * (prevCompLin_ + (compLin - prevCompLin_) * t);
            outMs += v * v;
            const float a = std::fabs(v);
            const float r = a > ceil_ ? ceil_ / a : 1.0f;
            // sliding minimum over the last kLook samples (monotonic queue)
            while (dqSize_ > 0 && dqVal_[(dqHead_ + dqSize_ - 1) % kLook] >= r) --dqSize_;
            dqVal_[(dqHead_ + dqSize_) % kLook] = r;
            dqIdx_[(dqHead_ + dqSize_) % kLook] = t_;
            ++dqSize_;
            while (dqIdx_[dqHead_] <= t_ - kLook) { dqHead_ = (dqHead_ + 1) % kLook; --dqSize_; }
            const float hold = dqVal_[dqHead_];
            // box-average of the held minimum: smooth, and guaranteed low enough exactly when
            // the peak leaves the delay line
            boxSum_ += hold - boxBuf_[dpos_];
            boxBuf_[dpos_] = hold;
            const float smooth = boxSum_ / (float)kLook;
            limGain_ = smooth < limGain_ ? smooth : limRel_ * limGain_ + (1 - limRel_) * smooth;
            delay_[dpos_] = v;
            dpos_ = (dpos_ + 1) % kLook;
            x[b0 + i] = delay_[dpos_] * limGain_;   // sample written kLook-1 samples ago
            ++t_;
            minLim = std::min(minLim, limGain_);
        }
        prevCompLin_ = compLin;
    }
    // feedback update for the next hop: +6 dB/s up, -15 dB/s down at most
    if (levelOn_ && levInit_ && speech) {
        levelPow_ += 0.03f * (outMs / (float)n - levelPow_);
        const float err = target_ - powToDb(levelPow_);
        levGainDb_ = clampv(levGainDb_ + clampv(0.05f * err, -0.15f, 0.06f), -12.0f, 30.0f);
    }
    levelEst_ = powToDb(levelPow_);
    grDb_ = -compGainDb_ - linToDb(minLim);
}

// =========================================================================== Resampler
void Resampler::configure(int inRate, int outRate) {
    const int g = std::gcd(inRate, outRate);
    L_ = outRate / g;
    M_ = inRate / g;
    bank_.assign((size_t)L_ * kTaps, 0.0f);
    if (!passthrough()) {
        const double fc = std::min(1.0, (double)L_ / M_) * 0.92;   // relative to input Nyquist
        const double D = kTaps / 2.0;
        for (int ph = 0; ph < L_; ++ph) {
            double sum = 0;
            for (int k = 0; k < kTaps; ++k) {
                const double t = k + (double)ph / L_ - D;
                const double x = 3.14159265358979323846 * fc * t;
                const double sinc = std::fabs(t) < 1e-9 ? 1.0 : std::sin(x) / x;
                const double u = (k + (double)ph / L_) / kTaps;  // 0..1
                const double w = 0.42 - 0.5 * std::cos(2 * 3.14159265358979323846 * u) +
                                 0.08 * std::cos(4 * 3.14159265358979323846 * u);
                bank_[(size_t)ph * kTaps + k] = (float)(fc * sinc * w);
                sum += fc * sinc * w;
            }
            for (int k = 0; k < kTaps; ++k) bank_[(size_t)ph * kTaps + k] = (float)(bank_[(size_t)ph * kTaps + k] / sum);
        }
    }
    reset();
}

void Resampler::reset() {
    hist_.fill(0);
    histPos_ = 0;
    phaseAcc_ = 0;
}

int Resampler::process(const float* in, int n, float* out, int cap) {
    if (passthrough()) {
        const int m = std::min(n, cap);
        std::copy(in, in + m, out);
        return m;
    }
    int written = 0;
    for (int i = 0; i < n; ++i) {
        hist_[histPos_] = in[i];
        hist_[histPos_ + kTaps] = in[i];
        const float* newest = &hist_[histPos_ + kTaps];
        histPos_ = (histPos_ + 1) % kTaps;
        // outputs whose position falls on this newest input sample
        while (phaseAcc_ < L_ && written < cap) {
            const float* h = &bank_[(size_t)phaseAcc_ * kTaps];
            float acc = 0;
            for (int k = 0; k < kTaps; ++k) acc += h[k] * newest[-k];
            out[written++] = acc;
            phaseAcc_ += M_;
        }
        phaseAcc_ -= L_;
    }
    return written;
}

}  // namespace wv
