// Tests for the DSP stages and the full pipeline.
// SPDX-License-Identifier: MIT
#include <cmath>
#include <cstdio>
#include <random>
#include <cstdlib>
#include <string>
#include <vector>

#include "wv/dsp.h"
#include "wv/pipeline.h"
#include "wv/resolve.h"

using namespace wv;

#define CHECK(cond, ...)                                                    \
    do {                                                                    \
        if (cond) { ++pass; }                                               \
        else { ++fail; std::printf("FAIL %s:%d: ", __FILE__, __LINE__);     \
               std::printf(__VA_ARGS__); std::printf("\n"); }               \
    } while (0)

static float rms(const float* x, size_t n) {
    double s = 0;
    for (size_t i = 0; i < n; ++i) s += (double)x[i] * x[i];
    return (float)std::sqrt(s / std::max<size_t>(1, n));
}

static void testResampler(int& pass, int& fail) {
    for (int rate : {16000, 44100, 96000}) {
        Resampler up, dn;
        up.configure(rate, kFs);
        dn.configure(kFs, rate);
        const int n = rate;  // 1 s, 1 kHz sine
        std::vector<float> x(n), y(up.maxOutput(n)), z(n + 64);
        for (int i = 0; i < n; ++i) x[i] = 0.5f * std::sin(2 * kPi * 1000.0f * i / rate);
        const int m = up.process(x.data(), n, y.data(), (int)y.size());
        CHECK(std::abs(m - (long long)n * kFs / rate) <= 2, "resampler %d count %d", rate, m);
        const float r = rms(y.data() + 2000, m - 4000);
        CHECK(std::fabs(r - 0.5f / std::sqrt(2.0f)) < 0.005f, "resampler %d rms %f", rate, r);
        const int k = dn.process(y.data(), m, z.data(), (int)z.size());
        CHECK(std::abs(k - n) <= 2, "round trip count %d vs %d", k, n);
    }
}

static void testLimiter(int& pass, int& fail) {
    Dynamics d;
    Resolved r;
    r.levelOn = 0; r.compOn = 0; r.ceiling = dbToLin(-1.0f);
    d.setParams(r);
    std::vector<float> x(48000);
    for (size_t i = 0; i < x.size(); ++i) x[i] = 1.8f * std::sin(2 * kPi * 200.0f * i / kFs) * ((i / 4800) % 2 ? 1.0f : 0.1f);
    float peak = 0;
    for (size_t i = 0; i < x.size(); i += kHop) {
        d.process(&x[i], kHop, true);
        for (int k = 0; k < kHop; ++k) peak = std::max(peak, std::fabs(x[i + k]));
    }
    CHECK(peak <= dbToLin(-1.0f) * 1.0001f, "limiter peak %f > ceiling", peak);
}

static void testLeveller(int& pass, int& fail) {
    // A quiet talker (-40 dBFS) must be brought close to the target level.
    Dynamics d;
    Resolved r;
    r.levelOn = 1; r.compOn = 1; r.levelTargetDb = -19.0f; r.compThresholdDb = -20.0f;
    d.setParams(r);
    std::mt19937 rng(3);
    std::normal_distribution<float> nd(0, dbToLin(-40.0f));
    std::vector<float> x(kFs * 8);
    for (auto& v : x) v = nd(rng);
    for (size_t i = 0; i < x.size(); i += kHop) d.process(&x[i], kHop, true);
    const float outDb = linToDb(rms(&x[x.size() - kFs], kFs));
    CHECK(std::fabs(outDb - (-19.0f)) < 2.0f, "leveller output %f dB, want about -19", outDb);
}

static void testEqFit(int& pass, int& fail) {
    UserParams u;
    Analysis a;
    ResolveInfo info;
    u.tone = Tone::Clear;
    Resolved r = resolve(u, a, &info);
    CHECK(info.autoEqFitErrDb < 0.5f, "tone curve fit error %f dB", info.autoEqFitErrDb);
    // response of the bank at 3.5 kHz ~ tone curve
    float db = 0;
    for (int k = 0; k < kEqBank; ++k) db += r.eqBank[k].magDb(3500, kFs);
    CHECK(std::fabs(db - toneCurveDb(Tone::Clear, 3500)) < 0.7f, "bank at 3.5k %f vs %f", db, toneCurveDb(Tone::Clear, 3500));
}

static void testPipelineBypassAndLatency(int& pass, int& fail) {
    // Disabled pipeline must pass audio through unchanged, delayed by exactly latencyFrames().
    for (int rate : {48000, 16000, 44100}) {
        Pipeline p;
        PipelineOptions o;
        o.aiLibraryPath = WV_MOCK_AI;
        o.watchdog = false;
        p.configure(rate, 2, o);
        UserParams u;
        u.enabled = 0;
        Analysis a;
        a.inputRate = rate;
        p.setParams(resolve(u, a));
        // Band-limited noise: unlike a sine, its delay is unambiguous.
        const int n = rate * 2;
        std::vector<float> x((size_t)n * 2), y;
        std::mt19937 rng(rate);
        std::normal_distribution<float> nd(0, 0.3f);
        Biquad lp1, lp2;
        lp1.c = BiquadCoefs::lowpass(1200.0f, 0.54f, (float)rate);
        lp2.c = BiquadCoefs::lowpass(1200.0f, 1.31f, (float)rate);
        for (int i = 0; i < n; ++i) x[2 * i] = x[2 * i + 1] = lp2.process(lp1.process(nd(rng)));
        const float xr = rms(x.data(), x.size());
        y = x;
        for (int i = 0; i < n; i += 441) p.process(&y[(size_t)i * 2], std::min(441, n - i));
        const int lat = p.latencyFrames();
        // find the best-matching delay
        int best = -1; double bestErr = 1e9;
        for (int d = std::max(0, lat - 40); d <= lat + 40; ++d) {
            double e = 0;
            for (int i = rate / 2; i < n; ++i) { const double v = y[2 * i] - x[2 * (i - d)]; e += v * v; }
            if (e < bestErr) { bestErr = e; best = d; }
        }
        CHECK(std::abs(best - lat) <= 1, "rate %d: measured delay %d, reported %d", rate, best, lat);
        // Other rates go through two resamplers and keep a fraction-of-a-sample delay, so
        // align with a fractional (linearly interpolated) shift before comparing.
        double bestFracErr = bestErr;
        for (double fr = -1.0; fr <= 1.0; fr += 0.02) {
            double e = 0;
            for (int i = rate / 2; i < n; ++i) {
                const double pos = i - best - fr;
                const int i0 = (int)std::floor(pos);
                const double t = pos - i0;
                const double ref = (1 - t) * x[2 * i0] + t * x[2 * (i0 + 1)];
                const double v = y[2 * i] - ref;
                e += v * v;
            }
            bestFracErr = std::min(bestFracErr, e);
        }
        const double relErr = std::sqrt(bestFracErr / (n - rate / 2)) / xr;
        CHECK(relErr < (rate == kFs ? 1e-4 : 0.03), "rate %d: bypass error %g", rate, relErr);
    }
}

// Speech-like bursts in steady noise, 0.5 s on / 1 s off.
static std::vector<float> burstSignal(int seconds) {
    std::mt19937 rng(7);
    std::normal_distribution<float> nd(0, 0.01f);
    std::vector<float> x((size_t)kFs * seconds);
    for (size_t i = 0; i < x.size(); ++i) {
        const bool on = (i / 24000) % 3 == 0;
        x[i] = nd(rng) + (on ? 0.2f * std::sin(2 * kPi * 220.0f * i / kFs) * (1 + 0.5f * std::sin(2 * kPi * 3.0f * i / kFs)) : 0.0f);
    }
    return x;
}

// The AI library used by the tests: the stand-in by default, the real one if WV_AI_LIB is set
// (CI runs the tests a second time with the real DeepFilterNet build).
static std::string aiLib() {
    const char* e = std::getenv("WV_AI_LIB");
    return e && *e ? std::string(e) : std::string(WV_MOCK_AI);
}
static bool realAi() { const char* e = std::getenv("WV_AI_LIB"); return e && *e; }

static void testPipelineWithAi(int& pass, int& fail) {
    // Full chain with the AI stand-in: AI active, finite output, peaks under the ceiling, and
    // the noise the AI removes stays removed (the leveller / compressor must not pull it back
    // up in the pauses). Compared against the same chain with denoising off.
    float pauseDb[2] = {0, 0};
    for (int denoise = 1; denoise >= 0; --denoise) {
        Pipeline p;
        PipelineOptions o;
        o.aiLibraryPath = aiLib();
        o.watchdog = false;
        const std::string st = p.configure(48000, 1, o);
        CHECK(p.aiActive(), "AI library not loaded: %s", st.c_str());
        UserParams u;
        u.stageOn[(int)Stage::Denoise] = denoise;
        Analysis a;
        p.setParams(resolve(u, a));
        std::vector<float> x = burstSignal(6);
        for (size_t i = 0; i < x.size(); i += 480) p.process(&x[i], 480);
        bool finite = true;
        float peak = 0;
        for (float v : x) { finite &= std::isfinite(v); peak = std::max(peak, std::fabs(v)); }
        CHECK(finite, "non-finite output");
        CHECK(peak <= dbToLin(-1.0f) * 1.001f, "peak %f above ceiling", peak);
        const int lat = p.latencyFrames();
        pauseDb[denoise] = linToDb(rms(&x[(size_t)(10 * 24000) + lat + 4000], 16000));  // 0.6..0.9 s into a pause
        Analysis out;
        p.getAnalysis(out);
        CHECK(out.aiActive == 1 && out.aiProblem == 0, "analysis says AI %d problem %d", out.aiActive, out.aiProblem);
        if (denoise) {
            // The real AI rightly refuses to call a sine tone speech, so this check is for the
            // stand-in only; the end-to-end run checks the profile with real speech.
            if (!realAi()) CHECK(out.speechFrames > 50, "voice profile did not learn (%u frames)", out.speechFrames);
            CHECK(out.rawFrames > 400, "input profile did not learn (%u frames)", out.rawFrames);
        }
    }
    // the stand-in removes about 11 dB, the real AI far more; most of it must reach the output
    std::printf("  pauses with denoising %.1f dB, without %.1f dB (%s)\n", pauseDb[1], pauseDb[0], realAi() ? "real AI" : "stand-in");
    CHECK(pauseDb[1] < pauseDb[0] - (realAi() ? 25.0f : 8.0f), "denoising lost in the chain: pause %.1f dB (on) vs %.1f dB (off)", pauseDb[1], pauseDb[0]);
}

static void testPipelineWithoutAi(int& pass, int& fail) {
    // AI library missing: the rest of the chain still works, with the same latency, and the
    // problem is reported.
    Pipeline pa, pn;
    PipelineOptions o;
    o.watchdog = false;
    o.aiLibraryPath = WV_MOCK_AI;
    pa.configure(48000, 1, o);
    o.aiLibraryPath = "does-not-exist";
    pn.configure(48000, 1, o);
    CHECK(!pn.aiActive(), "AI unexpectedly active");
    CHECK(pa.latencyFrames() == pn.latencyFrames(), "latency differs: %d vs %d", pa.latencyFrames(), pn.latencyFrames());
    UserParams u;
    Analysis a;
    pn.setParams(resolve(u, a));
    std::vector<float> x = burstSignal(6);
    for (size_t i = 0; i < x.size(); i += 480) pn.process(&x[i], 480);
    float peak = 0;
    for (float v : x) peak = std::max(peak, std::fabs(v));
    CHECK(peak > 0.1f && peak <= dbToLin(-1.0f) * 1.001f, "no-AI output peak %f", peak);
    Analysis out;
    pn.getAnalysis(out);
    CHECK(out.aiActive == 0 && out.aiProblem == 1, "no-AI analysis: active %d problem %d", out.aiActive, out.aiProblem);
}

static void testAiFailure(int& pass, int& fail) {
    // The engine reports an error mid-stream: audio continues, problem is reported.
    Pipeline p;
    PipelineOptions o;
    o.aiLibraryPath = WV_MOCK_AI;
    o.watchdog = false;
    p.configure(48000, 1, o);
    UserParams u;
    Analysis a;
    p.setParams(resolve(u, a));
    std::vector<float> x = burstSignal(4);
    x[kFs * 2] = 12345.0f;   // the stand-in fails on this hop (it starts a hop: 96000 % 480 == 0)
    for (size_t i = 0; i < x.size(); i += 480) p.process(&x[i], 480);
    Analysis out;
    p.getAnalysis(out);
    CHECK(out.aiActive == 0 && out.aiProblem == 2, "after failure: active %d problem %d", out.aiActive, out.aiProblem);
    float tail = rms(&x[x.size() - kFs], kFs);
    CHECK(std::isfinite(tail) && tail > 1e-4f, "audio stopped after AI failure (%g)", tail);
}

void testTextureTransparent(int& pass, int& fail);
void runPipelineTests(int& pass, int& fail) {
    testTextureTransparent(pass, fail);
    testResampler(pass, fail);
    testLimiter(pass, fail);
    testLeveller(pass, fail);
    testEqFit(pass, fail);
    testPipelineBypassAndLatency(pass, fail);
    testPipelineWithAi(pass, fail);
    testPipelineWithoutAi(pass, fail);
    if (!realAi()) testAiFailure(pass, fail);   // needs the stand-in's error trigger
}

// Texture with no harmonics must be a pure delay (checks the polyphase filters and the
// dry-path alignment).
void testTextureTransparent(int& pass, int& fail) {
    for (float hz : {200.0f, 1000.0f, 8000.0f}) {
        Texture t;
        t.setParams(0.5f, 0.0f, 0.0f, 1.0f);
        // band-limited noise around `hz` (a sine's delay would be ambiguous)
        std::vector<float> x(kFs), y;
        std::mt19937 rng((unsigned)hz);
        std::normal_distribution<float> nd(0, 1);
        Biquad bp;
        bp.c = BiquadCoefs::bandpass(hz, 0.7f, kFs);
        for (int i = 0; i < kFs; ++i) x[i] = 0.2f * bp.process(nd(rng));
        const float xr = rms(&x[kFs / 2], kFs / 2);
        y = x;
        for (int i = 0; i < kFs; i += kHop) t.process(&y[i], kHop);
        int best = 0; double bestE = 1e9;
        for (int d = 0; d < 40; ++d) {
            double e = 0;
            for (int i = kFs / 2; i < kFs; ++i) { const double v = y[i] - x[i - d]; e += v * v; }
            if (e < bestE) { bestE = e; best = d; }
        }
        const double rel = std::sqrt(bestE / (kFs / 2)) / xr;
        CHECK(best == 15, "texture delay %d at %.0f Hz", best, hz);
        // the 8 kHz test band reaches past 15 kHz, where the short half-band filter starts to roll off
        CHECK(rel < (hz > 5000 ? 0.03 : 0.01), "texture not transparent at %.0f Hz: %g", hz, rel);
    }
}
