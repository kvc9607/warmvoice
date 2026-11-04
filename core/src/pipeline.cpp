// SPDX-License-Identifier: MIT
#include "wv/pipeline.h"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace wv {

namespace {
constexpr int kMaxChunk = 4096;     // device frames handled per inner iteration
using Clock = std::chrono::steady_clock;
inline double secs(Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration<double>(b - a).count();
}
}  // namespace

Pipeline::Pipeline() = default;
Pipeline::~Pipeline() = default;

std::string Pipeline::configure(int deviceRate, int channels, const PipelineOptions& opt) {
    rate_ = deviceRate;
    channels_ = std::max(1, channels);
    opt_ = opt;
    rsIn_.configure(rate_, kFs);
    rsOut_.configure(kFs, rate_);
    std::string err;
    ai_ = AiDenoiser::create(opt.aiLibraryPath, &err);
    aiProblem_ = ai_ ? 0 : 1;
    // Internal delay at 48 kHz. The bypass path and the no-AI path are delayed by the same
    // amount, so switching never shifts the audio in time.
    chainDelay_ = kAiDelay + 15 /*texture*/ + Dynamics::lookahead();
    const int up = (int)std::ceil((double)kFs / std::min(rate_, kFs)) + 1;
    mono_.assign(kMaxChunk, 0.0f);
    inFifo_.assign((size_t)kMaxChunk * up + 64 + kHop, 0.0f);
    outFifo_.assign((size_t)kMaxChunk * up + 4 * kHop, 0.0f);
    hopIn_.assign(kHop, 0.0f);
    hopOut_.assign(kHop, 0.0f);
    aiDelay_.assign(kAiDelay, 0.0f);
    bypass_.assign(chainDelay_, 0.0f);
    reset();
    return ai_ ? "AI denoiser loaded" : ("AI denoiser NOT available: " + err);
}

void Pipeline::reset() {
    if (ai_) ai_->reset();
    spectral_.reset();
    restore_.reset();
    hpf_.reset();
    for (auto& b : eqBank_) b.reset();
    for (auto& b : userEq_) b.reset();
    texture_.reset();
    dyn_.reset();
    rsIn_.reset();
    rsOut_.reset();
    std::fill(aiDelay_.begin(), aiDelay_.end(), 0.0f);
    std::fill(bypass_.begin(), bypass_.end(), 0.0f);
    aiDelayPos_ = bypassPos_ = 0;
    inCount_ = 0;
    // Pre-fill the device-rate output queue with one hop (+ resampler slack) so a call of
    // any size can always be answered: this is the only buffering delay of the block size.
    outRead_ = 0;
    outCount_ = (int)std::ceil((double)kHop * rate_ / kFs) + 4;
    std::fill(outFifo_.begin(), outFifo_.begin() + outCount_, 0.0f);
    aiFailed_ = aiSlow_ = overloaded_ = speech_ = false;
    aiProblem_ = ai_ ? 0 : 1;
    slowHops_ = 0;
    loadEma_ = 0;
    mixWet_ = 1.0f;
    timing_ = StageTiming();
}

void Pipeline::setParams(const Resolved& r) {
    p_ = r;
    spectral_.setParams(r);
    restore_.setParams(r.restoreOn ? r.restoreCutoffHz : 0.0f, r.restoreGain);
    hpf_.c = r.hpf;
    for (int k = 0; k < kEqBank; ++k) eqBank_[k].c = r.eqBank[k];
    for (int k = 0; k < kEqNodes; ++k) userEq_[k].c = r.userEq[k];
    texture_.setParams(r.texDrive, r.texEven, r.texOdd, r.textureOn ? r.texMix : 0.0f);
    dyn_.setParams(r);
}

int Pipeline::latencyFrames() const {
    const double r = (double)rate_ / kFs;
    const double prefill = std::ceil((double)kHop * rate_ / kFs) + 4;
    return (int)std::lround(prefill + chainDelay_ * r + rsIn_.latencyInputSamples() +
                            rsOut_.latencyInputSamples() * r);
}

void Pipeline::process(float* io, int frames) {
    for (int base = 0; base < frames; base += kMaxChunk) {
        const int n = std::min(kMaxChunk, frames - base);
        float* chunk = io + (size_t)base * channels_;
        const float inv = 1.0f / (float)channels_;
        for (int i = 0; i < n; ++i) {
            float s = 0;
            for (int c = 0; c < channels_; ++c) s += chunk[(size_t)i * channels_ + c];
            mono_[i] = s * inv;
        }
        const int m = rsIn_.process(mono_.data(), n, inFifo_.data() + inCount_, (int)inFifo_.size() - inCount_);
        inCount_ += m;
        int consumed = 0;
        while (inCount_ - consumed >= kHop) {
            std::copy(inFifo_.begin() + consumed, inFifo_.begin() + consumed + kHop, hopIn_.begin());
            consumed += kHop;
            processHop(hopIn_.data());
            if (outRead_ > 0) {
                std::copy(outFifo_.begin() + outRead_, outFifo_.begin() + outRead_ + outCount_, outFifo_.begin());
                outRead_ = 0;
            }
            outCount_ += rsOut_.process(hopOut_.data(), kHop, outFifo_.data() + outCount_,
                                        (int)outFifo_.size() - outCount_);
        }
        if (consumed) {
            std::copy(inFifo_.begin() + consumed, inFifo_.begin() + inCount_, inFifo_.begin());
            inCount_ -= consumed;
        }
        for (int i = 0; i < n; ++i) {
            float v = 0.0f;
            if (outCount_ > 0) { v = outFifo_[outRead_++]; --outCount_; }
            for (int c = 0; c < channels_; ++c) chunk[(size_t)i * channels_ + c] = v;
        }
    }
}

void Pipeline::processHop(float* x) {
    const auto t0 = Clock::now();
    float* y = hopOut_.data();

    // raw input delayed by the chain latency, for bypass / on-off cross-fades
    float raw[kHop];
    float peak = 0, ms = 0;
    for (int i = 0; i < kHop; ++i) {
        const float in = x[i];
        peak = std::max(peak, std::fabs(in));
        ms += in * in;
        raw[i] = bypass_[bypassPos_];
        bypass_[bypassPos_] = in;
        bypassPos_ = (bypassPos_ + 1) % chainDelay_;
    }
    inPeak_ = std::max(peak, inPeak_ * 0.995f);
    const float hopDb = powToDb(ms / kHop);

    const float wetTarget = (p_.enabled && !overloaded_) ? 1.0f : 0.0f;
    if (wetTarget == 0.0f && mixWet_ == 0.0f) {   // fully bypassed: skip the work
        std::copy(raw, raw + kHop, y);
        timing_.total += secs(t0, Clock::now());
        ++timing_.hops;
        return;
    }

    // 1 AI denoiser (+ spectral stage inside it)
    bool done = false;
    if (aiUsable()) {
        // "denoise off" keeps the transform (for the tone fixes) but lets all noise through
        const float atten = p_.denoiseOn ? p_.attenLimitDb : 0.5f;
        if (ai_->process(x, y, spectral_, atten)) { done = true; speech_ = ai_->speech(); }
        else { aiFailed_ = true; aiProblem_ = 2; }
    }
    if (!done) {
        // No AI: same delay, no denoising; a plain level test stands in for speech detection
        // so the leveller still works.
        for (int i = 0; i < kHop; ++i) {
            y[i] = aiDelay_[aiDelayPos_];
            aiDelay_[aiDelayPos_] = x[i];
            aiDelayPos_ = (aiDelayPos_ + 1) % kAiDelay;
        }
        speech_ = hopDb > -50.0f;
    }
    if (speech_) inSpeechDb_ += 0.02f * (hopDb - inSpeechDb_);
    else noiseDb_ += 0.02f * (hopDb - noiseDb_);
    const auto t1 = Clock::now();

    // 2 restoration
    restore_.process(y, kHop);
    const auto t2 = Clock::now();
    // 3 EQ
    hpf_.processBlock(y, kHop);
    if (p_.autoEqOn) for (auto& b : eqBank_) b.processBlock(y, kHop);
    if (p_.userEqOn) for (auto& b : userEq_) b.processBlock(y, kHop);
    const auto t3 = Clock::now();
    // 4 texture
    texture_.process(y, kHop);
    const auto t4 = Clock::now();
    // 5 dynamics
    dyn_.process(y, kHop, speech_);
    const auto t5 = Clock::now();

    // on/off cross-fade with the equally delayed raw signal
    if (mixWet_ != wetTarget || wetTarget < 1.0f) {
        const float a = mixWet_, b = wetTarget;
        for (int i = 0; i < kHop; ++i) {
            const float w = a + (b - a) * (float)(i + 1) / kHop;
            y[i] = raw[i] + w * (y[i] - raw[i]);
        }
        mixWet_ = wetTarget;
    }
    float oms = 0;
    for (int i = 0; i < kHop; ++i) oms += y[i] * y[i];
    outDb_ = powToDb(oms / kHop);

    timing_.ai += secs(t0, t1);
    timing_.restore += secs(t1, t2);
    timing_.eq += secs(t2, t3);
    timing_.texture += secs(t3, t4);
    timing_.dynamics += secs(t4, t5);
    const double hopTime = secs(t0, Clock::now());
    timing_.total += hopTime;
    ++timing_.hops;

    // watchdog: a hop must finish well inside its 10 ms
    const float load = (float)(hopTime / 0.010);
    loadEma_ = 0.98f * loadEma_ + 0.02f * load;
    if (opt_.watchdog) {
        if (loadEma_ > 0.8f) {
            if (++slowHops_ > 200) {          // 2 s too slow
                if (aiUsable()) { aiSlow_ = true; aiProblem_ = 3; }
                else overloaded_ = true;
                slowHops_ = 0;
            }
        } else {
            slowHops_ = 0;
        }
    }
}

void Pipeline::getAnalysis(Analysis& a) const {
    const auto& lt = spectral_.ltasPow();
    const auto& rl = spectral_.rawLtasPow();
    for (int b = 0; b < kBands; ++b) {
        a.ltasDb[b] = powToDb(lt[b]);
        a.rawLtasDb[b] = powToDb(rl[b]);
    }
    a.speechFrames = spectral_.profileFrames();
    a.rawFrames = spectral_.rawFrames();
    a.inPeakDb = linToDb(inPeak_);
    a.inSpeechDb = inSpeechDb_;
    a.noiseFloorDb = noiseDb_;
    a.outLevelDb = outDb_;
    a.gainReductionDb = dyn_.gainReductionDb();
    a.load = loadEma_;
    a.aiActive = aiUsable() ? 1 : 0;
    a.aiProblem = aiProblem_;
    a.bypassed = overloaded_ ? 1 : 0;
    a.inputRate = rate_;
    ++a.seq;
}

}  // namespace wv
