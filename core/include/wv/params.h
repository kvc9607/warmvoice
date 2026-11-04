// Parameter blocks. All structs are plain data so they can live in shared memory between
// the UI app and the APO running inside audiodg.exe.
//
//   UserParams  : what the UI sliders say (app -> resolve()).
//   Resolved    : ready-to-use coefficients for the real-time path (app -> APO).
//   Analysis    : measurements produced by the real-time path (APO -> app).
// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>
#include "wv/biquad.h"
#include "wv/common.h"

namespace wv {

enum class Tone : int32_t { Warm = 0, Clear = 1, Natural = 2 };
enum class TextureStyle : int32_t { Condenser = 0, Tube = 1, Broadcast = 2 };
enum class Stage : int32_t { Gain = 0, Denoise, Restore, Optimize, Texture, Eq, Compress, Level, Count };

constexpr int kEqNodes = 5;
constexpr int kEqBank = 10;   // every static tone layer is merged into this one filter bank

struct EqNode { float hz, gainDb, q; };

struct UserParams {
    uint32_t version = 1;
    int32_t enabled = 1;
    Tone tone = Tone::Warm;
    float volume = 12.0f;                 // UI "音量" 0..30
    int32_t stageOn[(int)Stage::Count] = {1, 1, 1, 1, 1, 1, 1, 1};

    // "麦克风放大" sets the microphone's own level in Windows (done by the app, not here).
    float denoiseStrength = 1.0f;         // 0..1; 1 = remove all noise (silent when not talking)
    float restoreAmount = 0.8f;           // high-frequency restoration, 0..1
    float optimizeAmount = 0.75f;         // smart voice optimisation, 0..1
    float textureAmount = 0.5f;           // 0..1
    TextureStyle textureStyle = TextureStyle::Condenser;
    EqNode eq[kEqNodes] = {{80, 0, 0.7f}, {250, 0, 1.0f}, {1000, 0, 1.0f}, {4000, 0, 1.0f}, {12000, 0, 0.7f}};
    float compThresholdDb = -1.0f;        // "开始压的位置", dB relative to the levelled speech level
    float compRatio = 2.5f;
    int32_t compSpeed = 1;                // 0 slow, 1 medium, 2 fast
    float ceilingDb = -1.0f;
};

// Measurements from the real-time path. Written by one thread, read by another:
// readers must copy the block and check `seq` (seqlock) before trusting it.
struct Analysis {
    uint32_t seq = 0;
    uint32_t speechFrames = 0;            // frames that contributed to the voice profile
    float ltasDb[kBands] = {};            // long-term average spectrum of the cleaned voice
    float rawLtasDb[kBands] = {};         // long-term spectrum of the raw input (bandwidth detection)
    uint32_t rawFrames = 0;
    float inPeakDb = -120, inSpeechDb = -120, noiseFloorDb = -120;
    float outLevelDb = -120, gainReductionDb = 0;
    float load = 0;                       // processing time / real time (1.0 = 100 % of budget)
    int32_t aiActive = 0;                 // AI denoiser running
    int32_t aiProblem = 0;                // 0 ok, 1 library missing, 2 engine error, 3 computer too slow
    int32_t bypassed = 0;
    int32_t inputRate = 48000;            // device sample rate (a 16 kHz mic can never carry > 8 kHz)
};

struct Resolved {
    uint32_t version = 1;
    int32_t enabled = 1;
    // AI denoiser + spectral stage (one transform)
    int32_t denoiseOn = 1;
    float attenLimitDb = 100.0f;          // how far the AI may turn noise down
    int32_t optimizeOn = 1;
    float dynRatio = 0.8f, dynMaxCutDb = 7.5f;
    float eqRespDb[kBands] = {};          // static EQ response at band centres (dyn stage looks "through" it)
    // restoration (bandwidth extension)
    int32_t restoreOn = 0;
    float restoreCutoffHz = 0.0f;
    float restoreGain = 0.0f;
    // static EQ: tone + smart correction are smooth curves, merged into ONE 10-band bank;
    // the user's own EQ nodes are applied exactly as drawn.
    BiquadCoefs hpf;                      // rumble / DC cut, always on, first filter of the EQ stage
    int32_t autoEqOn = 1;
    BiquadCoefs eqBank[kEqBank];
    int32_t userEqOn = 1;
    BiquadCoefs userEq[kEqNodes];
    // texture
    int32_t textureOn = 1;
    float texDrive = 0, texMix = 0, texEven = 0, texOdd = 0;
    // dynamics
    int32_t levelOn = 1, compOn = 1;
    float levelTargetDb = -19.0f;
    float fixedGainDb = 0.0f;             // used instead of the leveller when it is switched off
    float compThresholdDb = -20.0f, compRatio = 2.5f, compAttack = 0.005f, compRelease = 0.08f;
    float ceiling = 0.891f;
};

}  // namespace wv
