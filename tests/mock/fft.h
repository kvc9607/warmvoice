// Small mixed-radix FFT (radix 2/3/4/5 + generic). Allocation happens only in the
// constructor, so transforms are safe to call on the real-time audio thread.
// SPDX-License-Identifier: MIT
#pragma once
#include <vector>
#include "wv/common.h"

namespace wv {

class ComplexFft {
public:
    explicit ComplexFft(int n);
    int size() const { return n_; }
    // Forward transform (sign -1), unnormalised. in and out must not alias.
    void forward(const cplx* in, cplx* out) const;

private:
    void work(cplx* out, const cplx* in, int stride, int fstride, const int* factors) const;
    void bfly2(cplx* out, int fstride, int m) const;
    void bfly3(cplx* out, int fstride, int m) const;
    void bfly4(cplx* out, int fstride, int m) const;
    void bfly5(cplx* out, int fstride, int m) const;
    void bflyGeneric(cplx* out, int fstride, int m, int p) const;

    int n_;
    std::vector<int> factors_;   // pairs (radix, remaining length)
    std::vector<cplx> tw_;       // twiddles e^{-2*pi*i*k/n}
    mutable std::vector<cplx> scratch_;
};

// Real FFT of even length n, built on a complex FFT of n/2.
// forward: n reals -> n/2+1 bins. inverse: n/2+1 bins -> n reals, scaled by 1/n.
class RealFft {
public:
    explicit RealFft(int n);
    int size() const { return n_; }
    void forward(const float* in, cplx* out) const;
    void inverse(const cplx* in, float* out) const;

private:
    int n_;
    ComplexFft half_;
    std::vector<cplx> super_;    // e^{-2*pi*i*k/n}, k < n/2
    mutable std::vector<cplx> a_, b_;
};

}  // namespace wv
