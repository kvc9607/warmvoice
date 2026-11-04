// SPDX-License-Identifier: MIT
// Mixed-radix decimation-in-time FFT. Written for WarmVoice; the structure follows the
// classic recursive "kf_work" scheme (radix 4/2/3/5 butterflies + generic radix).
#include "fft.h"

#include <cmath>
#include <stdexcept>

namespace wv {

ComplexFft::ComplexFft(int n) : n_(n) {
    if (n < 1) throw std::invalid_argument("fft size");
    tw_.resize(n);
    for (int k = 0; k < n; ++k) {
        double ph = -2.0 * 3.14159265358979323846 * k / n;
        tw_[k] = cplx((float)std::cos(ph), (float)std::sin(ph));
    }
    // Factorise: prefer 4, then 2, then odd numbers.
    int p = 4, rem = n;
    const int floorSqrt = (int)std::floor(std::sqrt((double)n));
    int maxRadix = 1;
    while (rem > 1) {
        while (rem % p) {
            switch (p) {
                case 4: p = 2; break;
                case 2: p = 3; break;
                default: p += 2; break;
            }
            if (p > floorSqrt) p = rem;
        }
        rem /= p;
        factors_.push_back(p);
        factors_.push_back(rem);
        if (p > maxRadix) maxRadix = p;
    }
    scratch_.resize(maxRadix);
}

void ComplexFft::forward(const cplx* in, cplx* out) const {
    if (n_ == 1) { out[0] = in[0]; return; }
    work(out, in, 1, 1, factors_.data());
}

void ComplexFft::work(cplx* out, const cplx* f, int fstride, int inStride, const int* factors) const {
    const int p = factors[0];
    const int m = factors[1];
    cplx* outBeg = out;
    cplx* outEnd = out + p * m;
    if (m == 1) {
        do { *out = *f; f += fstride * inStride; } while (++out != outEnd);
    } else {
        do {
            work(out, f, fstride * p, inStride, factors + 2);
            f += fstride * inStride;
        } while ((out += m) != outEnd);
    }
    out = outBeg;
    switch (p) {
        case 2: bfly2(out, fstride, m); break;
        case 3: bfly3(out, fstride, m); break;
        case 4: bfly4(out, fstride, m); break;
        case 5: bfly5(out, fstride, m); break;
        default: bflyGeneric(out, fstride, m, p); break;
    }
}

void ComplexFft::bfly2(cplx* F, int fstride, int m) const {
    cplx* F2 = F + m;
    for (int k = 0; k < m; ++k) {
        cplx t = F2[k] * tw_[k * fstride];
        F2[k] = F[k] - t;
        F[k] += t;
    }
}

void ComplexFft::bfly4(cplx* F, int fstride, int m) const {
    for (int k = 0; k < m; ++k) {
        cplx s0 = F[k + m] * tw_[k * fstride];
        cplx s1 = F[k + 2 * m] * tw_[2 * k * fstride];
        cplx s2 = F[k + 3 * m] * tw_[3 * k * fstride];
        cplx s5 = F[k] - s1;
        F[k] += s1;
        cplx s3 = s0 + s2;
        cplx s4 = s0 - s2;
        F[k + 2 * m] = F[k] - s3;
        F[k] += s3;
        F[k + m] = cplx(s5.real() + s4.imag(), s5.imag() - s4.real());
        F[k + 3 * m] = cplx(s5.real() - s4.imag(), s5.imag() + s4.real());
    }
}

void ComplexFft::bfly3(cplx* F, int fstride, int m) const {
    const float epi3i = tw_[fstride * m].imag();
    for (int k = 0; k < m; ++k) {
        cplx s1 = F[k + m] * tw_[k * fstride];
        cplx s2 = F[k + 2 * m] * tw_[2 * k * fstride];
        cplx s3 = s1 + s2;
        cplx s0 = (s1 - s2) * epi3i;
        cplx fm = F[k] - s3 * 0.5f;
        F[k] += s3;
        F[k + 2 * m] = cplx(fm.real() + s0.imag(), fm.imag() - s0.real());
        F[k + m] = cplx(fm.real() - s0.imag(), fm.imag() + s0.real());
    }
}

void ComplexFft::bfly5(cplx* F, int fstride, int m) const {
    const cplx ya = tw_[fstride * m];
    const cplx yb = tw_[fstride * 2 * m];
    cplx* F0 = F;
    cplx* F1 = F + m;
    cplx* F2 = F + 2 * m;
    cplx* F3 = F + 3 * m;
    cplx* F4 = F + 4 * m;
    for (int u = 0; u < m; ++u) {
        cplx s0 = F0[u];
        cplx s1 = F1[u] * tw_[u * fstride];
        cplx s2 = F2[u] * tw_[2 * u * fstride];
        cplx s3 = F3[u] * tw_[3 * u * fstride];
        cplx s4 = F4[u] * tw_[4 * u * fstride];
        cplx s7 = s1 + s4, s10 = s1 - s4, s8 = s2 + s3, s9 = s2 - s3;
        F0[u] = s0 + s7 + s8;
        cplx s5(s0.real() + s7.real() * ya.real() + s8.real() * yb.real(),
                s0.imag() + s7.imag() * ya.real() + s8.imag() * yb.real());
        cplx s6(s10.imag() * ya.imag() + s9.imag() * yb.imag(),
                -s10.real() * ya.imag() - s9.real() * yb.imag());
        F1[u] = s5 - s6;
        F4[u] = s5 + s6;
        cplx s11(s0.real() + s7.real() * yb.real() + s8.real() * ya.real(),
                 s0.imag() + s7.imag() * yb.real() + s8.imag() * ya.real());
        cplx s12(-s10.imag() * yb.imag() + s9.imag() * ya.imag(),
                 s10.real() * yb.imag() - s9.real() * ya.imag());
        F2[u] = s11 + s12;
        F3[u] = s11 - s12;
    }
}

void ComplexFft::bflyGeneric(cplx* F, int fstride, int m, int p) const {
    cplx* sc = scratch_.data();
    for (int u = 0; u < m; ++u) {
        int k = u;
        for (int q1 = 0; q1 < p; ++q1) { sc[q1] = F[k]; k += m; }
        k = u;
        for (int q1 = 0; q1 < p; ++q1) {
            int twidx = 0;
            F[k] = sc[0];
            for (int q = 1; q < p; ++q) {
                twidx += fstride * k;
                if (twidx >= n_) twidx -= n_;
                F[k] += sc[q] * tw_[twidx];
            }
            k += m;
        }
    }
}

RealFft::RealFft(int n) : n_(n), half_(n / 2) {
    if (n < 2 || (n & 1)) throw std::invalid_argument("real fft size must be even");
    const int M = n / 2;
    super_.resize(M + 1);
    for (int k = 0; k <= M; ++k) {
        double ph = -2.0 * 3.14159265358979323846 * k / n;
        super_[k] = cplx((float)std::cos(ph), (float)std::sin(ph));
    }
    a_.resize(M);
    b_.resize(M);
}

void RealFft::forward(const float* in, cplx* out) const {
    const int M = n_ / 2;
    for (int k = 0; k < M; ++k) a_[k] = cplx(in[2 * k], in[2 * k + 1]);
    half_.forward(a_.data(), b_.data());
    const cplx halfI(0.0f, 0.5f);
    for (int k = 0; k <= M; ++k) {
        cplx z = b_[k == M ? 0 : k];
        cplx zc = std::conj(b_[k == 0 ? 0 : M - k]);
        out[k] = 0.5f * (z + zc) - halfI * super_[k] * (z - zc);
    }
}

void RealFft::inverse(const cplx* in, float* out) const {
    const int M = n_ / 2;
    for (int k = 0; k < M; ++k) {
        cplx x = in[k];
        cplx xc = std::conj(in[M - k]);
        cplx fe = 0.5f * (x + xc);
        cplx fo = 0.5f * (x - xc) * std::conj(super_[k]);  // divide by W^k (|W|=1)
        // Inverse via conjugation: ifft(Z) = conj(fft(conj(Z)))/M
        a_[k] = std::conj(fe + cplx(0.0f, 1.0f) * fo);
    }
    half_.forward(a_.data(), b_.data());
    const float s = 1.0f / (float)M;
    for (int k = 0; k < M; ++k) {
        cplx z = std::conj(b_[k]) * s;
        out[2 * k] = z.real();
        out[2 * k + 1] = z.imag();
    }
}

}  // namespace wv
