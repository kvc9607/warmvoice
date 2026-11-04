// wvproc: run the WarmVoice chain on a WAV file (for listening tests and benchmarks).
//
//   wvproc in.wav out.wav [options]
//     --tone warm|clear|natural   --volume 0..30      --ai path/to/warmvoice_ai library
//     --off gain,denoise,restore,optimize,texture,eq,compress,level
//     --one-pass                  (skip the learning pass; simulates a brand-new install)
//     --rate N                    (resample the input to N Hz first: simulates such a mic)
//     --quiet
// SPDX-License-Identifier: MIT
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>

#include "wav.h"
#include "wv/pipeline.h"
#include "wv/resolve.h"

using namespace wv;

static int stageIndex(const std::string& s) {
    const char* names[] = {"gain", "denoise", "restore", "optimize", "texture", "eq", "compress", "level"};
    for (int i = 0; i < (int)Stage::Count; ++i)
        if (s == names[i]) return i;
    return -1;
}

static FILE* gTrace = nullptr;
static void runOnce(Pipeline& p, Wav& io) {
    // Feed the device-rate audio in APO-sized calls (10 ms).
    const int block = std::max(1, io.rate / 100);
    for (size_t f = 0; f < io.frames(); f += block) {
        const int n = (int)std::min<size_t>(block, io.frames() - f);
        p.process(&io.data[f * io.channels], n);
        if (gTrace) std::fprintf(gTrace, "%.2f,%d,%.2f\n", (double)f / io.rate, p.lastSpeech() ? 1 : 0, p.lastLevelGainDb());
    }
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: wvproc in.wav out.wav [--tone warm|clear|natural] [--volume V] [--ai PATH] "
                             "[--off a,b] [--one-pass] [--rate N] [--quiet]\n");
        return 2;
    }
    UserParams u;
    PipelineOptions opt;
    opt.watchdog = false;   // offline: never fall back because the machine is busy
    bool twoPass = true, quiet = false;
    int simRate = 0, bandLimit = 0;
    std::string traceFile, bandLimitOut;
    for (int i = 3; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
        if (a == "--tone") {
            std::string t = next();
            u.tone = t == "clear" ? Tone::Clear : (t == "natural" ? Tone::Natural : Tone::Warm);
        } else if (a == "--volume") u.volume = (float)std::atof(next().c_str());
        else if (a == "--ai") opt.aiLibraryPath = next();
        else if (a == "--one-pass") twoPass = false;
        else if (a == "--quiet") quiet = true;
        else if (a == "--rate") simRate = std::atoi(next().c_str());
        else if (a == "--bandlimit") bandLimit = std::atoi(next().c_str());
        else if (a == "--save-input") bandLimitOut = next();
        else if (a == "--trace") traceFile = next();
        else if (a == "--off") {
            std::stringstream ss(next());
            std::string s;
            while (std::getline(ss, s, ',')) {
                const int k = stageIndex(s);
                if (k < 0) { std::fprintf(stderr, "unknown stage %s\n", s.c_str()); return 2; }
                u.stageOn[k] = 0;
            }
        } else { std::fprintf(stderr, "unknown option %s\n", a.c_str()); return 2; }
    }

    Wav in;
    std::string err;
    if (!readWav(argv[1], in, &err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 1; }
    if (simRate > 0 && simRate != in.rate) {
        // Simulate a microphone that delivers simRate (band-limited) audio.
        Resampler rs;
        rs.configure(in.rate, simRate);
        Wav o; o.rate = simRate; o.channels = 1;
        std::vector<float> mono(in.frames());
        for (size_t f = 0; f < in.frames(); ++f) {
            float s = 0;
            for (int c = 0; c < in.channels; ++c) s += in.data[f * in.channels + c];
            mono[f] = s / in.channels;
        }
        o.data.resize((size_t)rs.maxOutput((int)mono.size()));
        o.data.resize((size_t)rs.process(mono.data(), (int)mono.size(), o.data.data(), (int)o.data.size()));
        in = o;
    }

    if (bandLimit > 0) {
        // Simulate a device that reports 48 kHz but whose content stops at bandLimit/2
        // (many wireless dongles and cheap USB codecs work like this).
        const int origRate = in.rate;
        for (int pass = 0; pass < 2; ++pass) {
            Resampler rs;
            const int from = pass == 0 ? origRate : bandLimit, to = pass == 0 ? bandLimit : origRate;
            rs.configure(from, to);
            Wav o; o.rate = to; o.channels = 1;
            std::vector<float> mono(in.frames());
            for (size_t f = 0; f < in.frames(); ++f) {
                float s = 0;
                for (int c = 0; c < in.channels; ++c) s += in.data[f * in.channels + c];
                mono[f] = s / in.channels;
            }
            o.data.resize((size_t)rs.maxOutput((int)mono.size()));
            o.data.resize((size_t)rs.process(mono.data(), (int)mono.size(), o.data.data(), (int)o.data.size()));
            in = o;
        }
        if (!bandLimitOut.empty()) writeWav(bandLimitOut, in);
    }

    Pipeline p;
    const std::string status = p.configure(in.rate, in.channels, opt);
    Analysis an;
    an.inputRate = in.rate;
    ResolveInfo info;
    Resolved res = resolve(u, an, &info);
    p.setParams(res);

    if (twoPass) {
        // Pass 1: let the voice profile learn this speaker + microphone (what the app does in
        // the first minutes of use, or with the "analyse my voice" button).
        Wav tmp = in;
        runOnce(p, tmp);
        p.getAnalysis(an);
        const uint32_t frames = an.speechFrames;
        if (std::getenv("WV_PRINT_LTAS")) {
            const BandLayout& L = bandLayout();
            for (int b = 0; b < kBands; ++b) std::printf("  %6.0f Hz %7.1f dB\n", L.centreHz[b], an.ltasDb[b]);
        }
        p.reset();
        res = resolve(u, an, &info);
        p.setParams(res);
        if (!quiet) std::printf("learning pass: %u speech frames in the voice profile\n", frames);
    }

    if (!traceFile.empty()) gTrace = std::fopen(traceFile.c_str(), "w");
    // Pass 2: process; append silence so the delayed tail comes out too.
    const int lat = p.latencyFrames();
    Wav io = in;
    io.data.resize(io.data.size() + (size_t)(lat + in.rate / 10) * in.channels, 0.0f);
    const auto t0 = std::chrono::steady_clock::now();
    runOnce(p, io);
    const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    // Remove the latency so the output lines up with the input sample for sample.
    Wav out;
    out.rate = io.rate;
    out.channels = io.channels;
    out.data.assign(io.data.begin() + (size_t)lat * io.channels, io.data.begin() + (size_t)lat * io.channels + in.data.size());
    if (!writeWav(argv[2], out)) { std::fprintf(stderr, "cannot write %s\n", argv[2]); return 1; }

    if (!quiet) {
        const StageTiming& t = p.timing();
        const double audio = (double)t.hops * kHop / kFs;
        auto pct = [&](double s) { return 100.0 * s / audio; };
        std::printf("AI: %s (%s)\n", p.aiActive() ? "on" : "OFF - no denoising", status.c_str());
        std::printf("latency: %.1f ms (%d frames at %d Hz)\n", 1000.0 * lat / in.rate, lat, in.rate);
        std::printf("detected cutoff: %s  restore: %s\n",
                    info.detectedCutoffHz > 0 ? (std::to_string((int)info.detectedCutoffHz) + " Hz").c_str() : "full band",
                    res.restoreOn ? "on" : (in.rate < 32000 ? "off (device rate too low to carry it)" : "off"));
        std::printf("voice profile used: %s, auto EQ fit error %.2f dB, gains:", info.calibrated ? "yes" : "no (tone preset only)",
                    info.autoEqFitErrDb);
        for (int k = 0; k < kEqBank; ++k) std::printf(" %+.1f", info.autoEqGainDb[k]);
        std::printf("\nCPU (one core, %% of real time): AI+spectral %.3f  restore %.3f  eq %.3f  texture %.3f  dynamics %.3f  | total %.3f %%\n",
                    pct(t.ai), pct(t.restore), pct(t.eq), pct(t.texture), pct(t.dynamics), pct(t.total));
        std::printf("whole pass incl. resampling: %.3f %% of real time\n", 100.0 * wall / ((double)io.frames() / io.rate));
    }
    return 0;
}
