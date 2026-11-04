// Turns UI settings + measurements into real-time coefficients. Runs on the app (or an
// offline tool), never on the audio thread, so the audio thread only copies numbers.
// SPDX-License-Identifier: MIT
#pragma once
#include "wv/params.h"

namespace wv {

struct ResolveInfo {          // extra results, for the UI and for tests
    float detectedCutoffHz = 0;   // 0 = full band
    bool calibrated = false;
    float autoEqGainDb[kEqBank] = {};
    float autoEqFitErrDb = 0;     // RMS error between wanted curve and the 10-band bank
};

Resolved resolve(const UserParams& u, const Analysis& a, ResolveInfo* info = nullptr);

// Curves (dB) used by resolve(); exposed for the UI graphs.
float toneCurveDb(Tone t, float hz);
float referenceSpeechDb(float hz);

// Centre frequencies of the fixed 10-band bank (for drawing).
const float* eqBankCentres();
// Shape of band k of the auto bank: type 0 low shelf, 1 peak, 2 high shelf.
void eqBankBand(int k, int* type, float* hz, float* q);

}  // namespace wv
