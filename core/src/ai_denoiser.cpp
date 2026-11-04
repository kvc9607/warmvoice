// SPDX-License-Identifier: MIT
#include "wv/ai_denoiser.h"

#include <cmath>
#include <cstdint>
#include <cstring>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace wv {

extern "C" {
typedef void (*wv_df_spec_hook)(void* user, float* enh, const float* noisy, size_t nFreqs, float lsnr);
typedef void* (*fn_create)(float attenLimDb);
typedef float (*fn_process)(void* st, const float* in, float* out, wv_df_spec_hook hook, void* user);
typedef void (*fn_set_atten)(void* st, float db);
typedef void (*fn_free)(void* st);
}

struct AiDenoiser::Api {
    void* lib = nullptr;
    fn_create create = nullptr;
    fn_process process = nullptr;
    fn_set_atten setAtten = nullptr;
    fn_free free = nullptr;

    void* sym(const char* name) {
#if defined(_WIN32)
        return (void*)GetProcAddress((HMODULE)lib, name);
#else
        return dlsym(lib, name);
#endif
    }
    ~Api() {
        if (!lib) return;
#if defined(_WIN32)
        FreeLibrary((HMODULE)lib);
#else
        dlclose(lib);
#endif
    }
};

namespace {
struct HookCtx {
    SpectralProcessor* sp;
    bool speech;
};
// DeepFilterNet's local SNR estimate (dB, about -15 for silence .. 35 for clean speech) is
// the speech decision for the whole chain - no second voice detector is needed.
constexpr float kSpeechLsnrDb = 4.0f;
void specHook(void* user, float* enh, const float* noisy, size_t nFreqs, float lsnr) {
    if (nFreqs != (size_t)kBins) return;
    static_assert(sizeof(cplx) == 2 * sizeof(float), "complex layout");
    auto* ctx = static_cast<HookCtx*>(user);
    ctx->speech = lsnr > kSpeechLsnrDb;
    // DeepFilterNet scales its spectrum by 1/960 -> power x 960^2.
    ctx->sp->process(reinterpret_cast<cplx*>(enh), reinterpret_cast<const cplx*>(noisy), ctx->speech,
                     (float)kFft * (float)kFft);
}
}  // namespace

std::unique_ptr<AiDenoiser> AiDenoiser::create(const std::string& path, std::string* err) {
    std::unique_ptr<AiDenoiser> d(new AiDenoiser());
    d->api_ = std::make_unique<Api>();
#if defined(_WIN32)
    const std::string name = path.empty() ? std::string("warmvoice_ai.dll") : path;
    d->api_->lib = (void*)LoadLibraryA(name.c_str());
#else
    const std::string name = path.empty() ? std::string("libwarmvoice_ai.so") : path;
    d->api_->lib = dlopen(name.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
    if (!d->api_->lib) { if (err) *err = "AI library not found: " + name; return nullptr; }
    d->api_->create = (fn_create)d->api_->sym("wv_df_create");
    d->api_->process = (fn_process)d->api_->sym("wv_df_process");
    d->api_->setAtten = (fn_set_atten)d->api_->sym("wv_df_set_atten_lim");
    d->api_->free = (fn_free)d->api_->sym("wv_df_free");
    if (!d->api_->create || !d->api_->process || !d->api_->setAtten || !d->api_->free) {
        if (err) *err = "AI library is missing wv_df_* functions";
        return nullptr;
    }
    d->state_ = d->api_->create(45.0f);
    if (!d->state_) { if (err) *err = "AI model failed to initialise"; return nullptr; }
    d->atten_ = 45.0f;
    return d;
}

AiDenoiser::~AiDenoiser() {
    if (state_ && api_ && api_->free) api_->free(state_);
}

void AiDenoiser::reset() {
    // DeepFilterNet keeps short-term normalisation state only; recreating it is the reset.
    if (!api_) return;
    if (state_) api_->free(state_);
    state_ = api_->create(atten_ > 0 ? atten_ : 45.0f);
}

bool AiDenoiser::process(const float* in, float* out, SpectralProcessor& sp, float attenLimitDb) {
    if (!state_) return false;
    if (attenLimitDb != atten_) { api_->setAtten(state_, attenLimitDb); atten_ = attenLimitDb; }
    HookCtx ctx{&sp, false};
    const float lsnr = api_->process(state_, in, out, &specHook, &ctx);
    speech_ = ctx.speech;
    // NaN means the engine caught an internal error. Checked on the bits, because fast-math
    // builds may optimise std::isnan away.
    uint32_t bits;
    std::memcpy(&bits, &lsnr, sizeof bits);
    return (bits & 0x7f800000u) != 0x7f800000u;
}

}  // namespace wv
