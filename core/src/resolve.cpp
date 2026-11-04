// SPDX-License-Identifier: MIT
#include "wv/resolve.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "wv/bands.h"

namespace wv {

namespace {

struct Anchor { float hz, db; };

float interpLog(const Anchor* a, int n, float hz) {
    if (hz <= a[0].hz) return a[0].db;
    if (hz >= a[n - 1].hz) return a[n - 1].db;
    for (int i = 1; i < n; ++i) {
        if (hz <= a[i].hz) {
            const float t = std::log(hz / a[i - 1].hz) / std::log(a[i].hz / a[i - 1].hz);
            return a[i - 1].db + t * (a[i].db - a[i - 1].db);
        }
    }
    return a[n - 1].db;
}

// Smooth long-term spectrum shape of clean, close-miked speech, relative to 1 kHz.
// Starting point for the smart optimisation; to be refined by listening tests.
const Anchor kRef[] = {{63, -12}, {100, -4}, {160, 2}, {250, 4}, {400, 4}, {630, 2}, {1000, 0},
                       {1600, -3}, {2500, -6}, {4000, -9}, {6300, -12}, {10000, -16}, {16000, -22}};
const Anchor kWarm[] = {{50, 1}, {120, 2.5f}, {250, 2}, {500, 0.5f}, {1000, 0}, {2000, -0.5f},
                        {3500, -1}, {6000, -0.5f}, {10000, 0.5f}, {16000, 1}};
const Anchor kClear[] = {{50, -1.5f}, {120, -1}, {250, -1.5f}, {500, -0.5f}, {1000, 0}, {2000, 1},
                         {3500, 2.5f}, {6000, 2}, {10000, 2.5f}, {16000, 2}};

enum class BandType { LowShelf, Peak, HighShelf };
struct BankBand { BandType type; float hz, q; };
const BankBand kBank[kEqBank] = {
    {BandType::LowShelf, 100, 0.8f}, {BandType::Peak, 200, 1.2f},  {BandType::Peak, 400, 1.2f},
    {BandType::Peak, 800, 1.2f},     {BandType::Peak, 1600, 1.2f}, {BandType::Peak, 3000, 1.2f},
    {BandType::Peak, 5000, 1.2f},    {BandType::Peak, 7500, 1.3f}, {BandType::HighShelf, 11000, 0.8f},
    {BandType::Peak, 15000, 1.0f}};
const float kCentres[kEqBank] = {100, 200, 400, 800, 1600, 3000, 5000, 7500, 11000, 15000};

BiquadCoefs bankCoefs(int k, float gainDb) {
    const float fs = (float)kFs;
    switch (kBank[k].type) {
        case BandType::LowShelf: return BiquadCoefs::lowShelf(kBank[k].hz, gainDb, fs, kBank[k].q);
        case BandType::HighShelf: return BiquadCoefs::highShelf(kBank[k].hz, gainDb, fs, kBank[k].q);
        default: return BiquadCoefs::peaking(kBank[k].hz, kBank[k].q, gainDb, fs);
    }
}

// Solve the small normal-equation system (n <= 10) by Gaussian elimination.
void solve(std::vector<double>& A, std::vector<double>& b, int n) {
    for (int c = 0; c < n; ++c) {
        int piv = c;
        for (int r = c + 1; r < n; ++r)
            if (std::fabs(A[r * n + c]) > std::fabs(A[piv * n + c])) piv = r;
        for (int j = 0; j < n; ++j) std::swap(A[c * n + j], A[piv * n + j]);
        std::swap(b[c], b[piv]);
        for (int r = c + 1; r < n; ++r) {
            const double f = A[r * n + c] / A[c * n + c];
            for (int j = c; j < n; ++j) A[r * n + j] -= f * A[c * n + j];
            b[r] -= f * b[c];
        }
    }
    for (int c = n - 1; c >= 0; --c) {
        double s = b[c];
        for (int j = c + 1; j < n; ++j) s -= A[c * n + j] * b[j];
        b[c] = s / A[c * n + c];
    }
}

// Fit the fixed 10-band bank to a wanted curve (Gauss-Newton, 4 iterations).
void fitBank(const std::vector<float>& gridHz, const std::vector<float>& want, float* gains, float* rmsErr) {
    const int G = (int)gridHz.size(), K = kEqBank;
    std::vector<double> J((size_t)G * K);
    for (int k = 0; k < K; ++k) {
        const BiquadCoefs c = bankCoefs(k, 6.0f);
        for (int i = 0; i < G; ++i) J[(size_t)i * K + k] = c.magDb(gridHz[i], (float)kFs) / 6.0;
    }
    for (int k = 0; k < K; ++k) gains[k] = 0;
    std::vector<float> cur(G);
    for (int it = 0; it < 4; ++it) {
        std::fill(cur.begin(), cur.end(), 0.0f);
        for (int k = 0; k < K; ++k) {
            if (gains[k] == 0) continue;
            const BiquadCoefs c = bankCoefs(k, gains[k]);
            for (int i = 0; i < G; ++i) cur[i] += c.magDb(gridHz[i], (float)kFs);
        }
        std::vector<double> AtA((size_t)K * K, 0.0), Atr(K, 0.0);
        for (int i = 0; i < G; ++i) {
            const double r = want[i] - cur[i];
            for (int a = 0; a < K; ++a) {
                Atr[a] += J[(size_t)i * K + a] * r;
                for (int b = 0; b < K; ++b) AtA[(size_t)a * K + b] += J[(size_t)i * K + a] * J[(size_t)i * K + b];
            }
        }
        for (int a = 0; a < K; ++a) AtA[(size_t)a * K + a] += 0.05;  // light regularisation
        solve(AtA, Atr, K);
        for (int k = 0; k < K; ++k) gains[k] = clampv(gains[k] + (float)Atr[k], -12.0f, 12.0f);
    }
    double e = 0;
    std::fill(cur.begin(), cur.end(), 0.0f);
    for (int k = 0; k < K; ++k) {
        const BiquadCoefs c = bankCoefs(k, gains[k]);
        for (int i = 0; i < G; ++i) cur[i] += c.magDb(gridHz[i], (float)kFs);
    }
    for (int i = 0; i < G; ++i) e += (want[i] - cur[i]) * (want[i] - cur[i]);
    if (rmsErr) *rmsErr = (float)std::sqrt(e / G);
}

float detectCutoff(const Analysis& a) {
    const float nyq = a.inputRate * 0.5f;
    float cut = nyq < 20000.0f ? nyq * 0.97f : 0.0f;   // a low device rate is a hard limit
    if (a.rawFrames < 200) return cut;                   // about 2 s of input
    // A link that stops at fc leaves (near) digital silence above it in the RAW input - noise
    // included - so look for a cliff: > 25 dB drop within two bands, ending far below the peak.
    const BandLayout& L = bandLayout();
    float peak = -300;
    for (int b = 0; b < kBands; ++b) peak = std::max(peak, a.rawLtasDb[b]);
    const int lo = L.bandOfHz(3000.0f);
    for (int b = kBands - 3; b >= lo; --b) {
        if (a.rawLtasDb[b] > peak - 60.0f && a.rawLtasDb[b] - a.rawLtasDb[b + 2] > 25.0f &&
            a.rawLtasDb[b + 2] < peak - 70.0f) {
            // content ends inside band b: use its lower edge so the new band starts in time
            const float hz = (float)L.edge[b] * ((float)kFs / kFft);
            if (cut == 0.0f || hz < cut) cut = hz;
            break;
        }
    }
    return cut;
}

}  // namespace

float toneCurveDb(Tone t, float hz) {
    switch (t) {
        case Tone::Warm: return interpLog(kWarm, (int)(sizeof(kWarm) / sizeof(kWarm[0])), hz);
        case Tone::Clear: return interpLog(kClear, (int)(sizeof(kClear) / sizeof(kClear[0])), hz);
        default: return 0.0f;
    }
}

float referenceSpeechDb(float hz) { return interpLog(kRef, (int)(sizeof(kRef) / sizeof(kRef[0])), hz); }

const float* eqBankCentres() { return kCentres; }

Resolved resolve(const UserParams& u, const Analysis& a, ResolveInfo* info) {
    Resolved r;
    const auto on = [&](Stage s) { return u.stageOn[(int)s] != 0; };
    const BandLayout& L = bandLayout();
    r.enabled = u.enabled;

    // ---- dynamics first (the volume decides where the input trim goes) ----
    r.levelOn = on(Stage::Level);
    r.compOn = on(Stage::Compress);
    r.levelTargetDb = -24.0f + 0.4f * clampv(u.volume, 0.0f, 30.0f);   // volume 12 -> -19.2 dBFS
    r.compThresholdDb = (r.levelOn ? r.levelTargetDb : -19.0f) + u.compThresholdDb;
    r.compRatio = clampv(u.compRatio, 1.0f, 10.0f);
    static const float atk[3] = {0.015f, 0.005f, 0.002f}, rel[3] = {0.25f, 0.08f, 0.04f};
    const int sp = clampv(u.compSpeed, 0, 2);
    r.compAttack = atk[sp];
    r.compRelease = rel[sp];
    r.ceiling = dbToLin(clampv(u.ceilingDb, -12.0f, 0.0f));

    // Without the leveller the volume slider is a plain gain.
    r.fixedGainDb = r.levelOn ? 0.0f : u.volume - 12.0f;
    // Rumble / DC cut, the first filter of the EQ stage (the AI does not need it in front).
    r.hpf = BiquadCoefs::highpass(70.0f, 0.707f, (float)kFs);

    // ---- AI denoiser ----
    // Strength 0..1 -> how far noise may be turned down: 6 dB .. 100 dB (= silent in pauses).
    r.denoiseOn = on(Stage::Denoise);
    const float ds = clampv(u.denoiseStrength, 0.0f, 1.0f);
    r.attenLimitDb = 6.0f + 94.0f * ds * ds;
    r.optimizeOn = on(Stage::Optimize);
    const float amt = clampv(u.optimizeAmount, 0.0f, 1.0f);
    r.dynRatio = 0.4f + 0.6f * amt;
    r.dynMaxCutDb = 3.0f + 6.0f * amt;

    // ---- restoration ----
    const float cutoff = detectCutoff(a);
    // Only useful when Windows hands us >= 32 kHz audio whose content stops early; a 16 kHz
    // endpoint can never carry the restored band to the apps.
    r.restoreOn = on(Stage::Restore) && a.inputRate >= 32000 && cutoff > 0.0f && cutoff < 11000.0f;
    r.restoreCutoffHz = r.restoreOn ? cutoff : 0.0f;
    r.restoreGain = r.restoreOn ? 0.5f * clampv(u.restoreAmount, 0.0f, 1.0f) : 0.0f;

    // ---- automatic tone layers (tone preset / smart correction) -> one 10-band bank ----
    const bool calibrated = a.speechFrames >= 500;
    std::vector<float> grid, want;
    for (int i = 0; i < 48; ++i) grid.push_back(40.0f * std::pow(18000.0f / 40.0f, i / 47.0f));
    want.assign(grid.size(), 0.0f);
    bool autoEq = u.tone != Tone::Natural;
    if (autoEq && calibrated && r.optimizeOn) {
        // Compare this voice + microphone with the target and correct the difference.
        const float topHz = std::min(cutoff > 0 ? cutoff : 16000.0f, 16000.0f);
        float d[kBands] = {}, w[kBands] = {};
        float mNorm = 0, rNorm = 0; int nn = 0;
        float target[kBands];
        for (int b = 0; b < kBands; ++b) {
            target[b] = referenceSpeechDb(L.centreHz[b]) + toneCurveDb(u.tone, L.centreHz[b]);
            if (L.centreHz[b] >= 500 && L.centreHz[b] <= 2000) { mNorm += a.ltasDb[b]; rNorm += target[b]; ++nn; }
        }
        mNorm /= std::max(1, nn); rNorm /= std::max(1, nn);
        for (int b = 0; b < kBands; ++b) {
            const float hz = L.centreHz[b];
            w[b] = (hz >= 90.0f && hz <= topHz) ? 1.0f : 0.0f;
            d[b] = w[b] * ((target[b] - rNorm) - (a.ltasDb[b] - mNorm));
        }
        // "Fix the microphone, keep the person":
        //  * an overall tilt (dull / harsh) is almost always the microphone -> correct fully
        //  * the remaining shape is partly the person's own voice -> correct only half of it,
        //    and only its broad outline
        double sw = 0, sx = 0, sy = 0, sxx = 0, sxy = 0;
        for (int b = 0; b < kBands; ++b) {
            const float hz = L.centreHz[b];
            if (w[b] == 0.0f || hz < 300.0f || hz > 8000.0f) continue;
            const double x = std::log2(hz / 1000.0);
            sw += 1; sx += x; sy += d[b]; sxx += x * x; sxy += x * d[b];
        }
        float slope = 0, icpt = 0;
        if (sw > 3) {
            slope = (float)((sw * sxy - sx * sy) / (sw * sxx - sx * sx));
            icpt = (float)((sy - slope * sx) / sw);
        }
        slope = clampv(slope, -3.0f, 3.0f);   // dB per octave
        float resid[kBands];
        for (int b = 0; b < kBands; ++b) {
            // the tilt line is only trusted where it was measured (300 Hz .. 8 kHz)
            const float hz = clampv(L.centreHz[b], 300.0f, 8000.0f);
            const float tilt = icpt + slope * std::log2(hz / 1000.0f);
            resid[b] = w[b] * (d[b] - tilt);
            d[b] = tilt;
        }
        for (int pass = 0; pass < 2; ++pass) {
            float s2[kBands];
            for (int b = 0; b < kBands; ++b) {
                const float l = resid[b > 0 ? b - 1 : b], c = resid[b], h = resid[b < kBands - 1 ? b + 1 : b];
                s2[b] = w[b] * (0.25f * l + 0.5f * c + 0.25f * h);
            }
            std::copy(s2, s2 + kBands, resid);
        }
        float mean = 0; int nm = 0;
        for (int b = 0; b < kBands; ++b) {
            d[b] = w[b] * (d[b] + 0.5f * resid[b]);
            if (L.centreHz[b] >= 500 && L.centreHz[b] <= 2000) { mean += d[b]; ++nm; }
        }
        mean /= std::max(1, nm);   // overall loudness is the leveller's job, not the EQ's
        for (int b = 0; b < kBands; ++b) {
            const float lim = L.centreHz[b] < 200.0f ? 3.0f : 6.0f;
            d[b] = w[b] * clampv(d[b] - mean, -lim, lim) * amt;
        }
        for (size_t i = 0; i < grid.size(); ++i) {
            const float hz = grid[i];
            int b = 0;
            while (b < kBands - 2 && L.centreHz[b + 1] < hz) ++b;
            const float t = clampv((hz - L.centreHz[b]) / (L.centreHz[b + 1] - L.centreHz[b]), 0.0f, 1.0f);
            want[i] = d[b] + t * (d[b + 1] - d[b]);
        }
    } else if (autoEq) {
        for (size_t i = 0; i < grid.size(); ++i) want[i] = toneCurveDb(u.tone, grid[i]);
    }
    float gains[kEqBank] = {}, fitErr = 0;
    r.autoEqOn = autoEq;
    if (autoEq) fitBank(grid, want, gains, &fitErr);
    for (int k = 0; k < kEqBank; ++k) r.eqBank[k] = autoEq ? bankCoefs(k, gains[k]) : BiquadCoefs::identity();

    // ---- user EQ nodes, exact ----
    r.userEqOn = on(Stage::Eq);
    bool anyUser = false;
    for (int k = 0; k < kEqNodes; ++k) {
        const EqNode& n = u.eq[k];
        const bool active = r.userEqOn && std::fabs(n.gainDb) > 0.01f;
        anyUser |= active;
        if (!active) { r.userEq[k] = BiquadCoefs::identity(); continue; }
        if (k == 0) r.userEq[k] = BiquadCoefs::lowShelf(n.hz, n.gainDb, (float)kFs, n.q);
        else if (k == kEqNodes - 1) r.userEq[k] = BiquadCoefs::highShelf(n.hz, n.gainDb, (float)kFs, n.q);
        else r.userEq[k] = BiquadCoefs::peaking(n.hz, n.q, n.gainDb, (float)kFs);
    }
    r.userEqOn = anyUser;

    // The dynamic stage runs before the EQ, so tell it what the EQ will do afterwards.
    for (int b = 0; b < kBands; ++b) {
        float db = 0;
        const float hz = std::max(20.0f, L.centreHz[b]);
        db += r.hpf.magDb(hz, (float)kFs);
        if (r.autoEqOn) for (int k = 0; k < kEqBank; ++k) db += r.eqBank[k].magDb(hz, (float)kFs);
        if (r.userEqOn) for (int k = 0; k < kEqNodes; ++k) db += r.userEq[k].magDb(hz, (float)kFs);
        r.eqRespDb[b] = db;
    }

    // ---- texture ----
    r.textureOn = on(Stage::Texture) && u.textureAmount > 0.0f;
    const float ta = clampv(u.textureAmount, 0.0f, 1.0f);
    static const float even[3] = {0.6f, 1.0f, 0.3f}, odd[3] = {0.2f, 0.3f, 0.8f};
    const int st = clampv((int)u.textureStyle, 0, 2);
    r.texDrive = ta;
    r.texEven = even[st];
    r.texOdd = odd[st];
    r.texMix = r.textureOn ? 0.8f * ta : 0.0f;

    if (info) {
        info->detectedCutoffHz = cutoff;
        info->calibrated = calibrated;
        for (int k = 0; k < kEqBank; ++k) info->autoEqGainDb[k] = gains[k];
        info->autoEqFitErrDb = fitErr;
    }
    return r;
}

}  // namespace wv
