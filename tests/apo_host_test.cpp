// apohosttest: loads WarmVoiceApo.dll the way the Windows audio engine (audiodg.exe) does -
// class factory, creation WITH COM aggregation, Initialize, format check, LockForProcess,
// a few seconds of audio through APOProcess, unlock, release - and checks every step.
//   apohosttest <path to WarmVoiceApo.dll>
// SPDX-License-Identifier: MIT
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <initguid.h>
#include <audioenginebaseapo.h>
#include <mmdeviceapi.h>
#include <ks.h>
#include <ksmedia.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <vector>

#include "guids.h"
#include "wv/winlink.h"

static int g_fail = 0;
#define CHECK(c, ...)                                                   \
    do {                                                                \
        if (c) { std::printf("ok    "); std::printf(__VA_ARGS__); std::printf("\n"); } \
        else { std::printf("FAIL  "); std::printf(__VA_ARGS__); std::printf("\n"); ++g_fail; } \
    } while (0)

// The engine's outer object: forwards everything but IUnknown to the APO's inner IUnknown.
struct Outer final : IUnknown {
    std::atomic<long> refs{1};
    IUnknown* inner = nullptr;
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (riid == __uuidof(IUnknown)) { *ppv = static_cast<IUnknown*>(this); AddRef(); return S_OK; }
        return inner ? inner->QueryInterface(riid, ppv) : E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return (ULONG)++refs; }
    STDMETHODIMP_(ULONG) Release() override {
        const long r = --refs;
        if (r == 0) {
            if (inner) inner->Release();
            delete this;
        }
        return (ULONG)r;
    }
};

struct MediaType final : IAudioMediaType {
    std::atomic<long> refs{1};
    WAVEFORMATEXTENSIBLE wf{};
    UNCOMPRESSEDAUDIOFORMAT u{};
    MediaType(float rate, UINT ch) {
        wf.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
        wf.Format.nChannels = (WORD)ch;
        wf.Format.nSamplesPerSec = (DWORD)rate;
        wf.Format.wBitsPerSample = 32;
        wf.Format.nBlockAlign = (WORD)(4 * ch);
        wf.Format.nAvgBytesPerSec = wf.Format.nSamplesPerSec * wf.Format.nBlockAlign;
        wf.Format.cbSize = 22;
        wf.Samples.wValidBitsPerSample = 32;
        wf.dwChannelMask = ch == 1 ? SPEAKER_FRONT_CENTER : (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT);
        wf.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
        u.guidFormatType = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
        u.dwSamplesPerFrame = ch;
        u.dwBytesPerSampleContainer = 4;
        u.dwValidBitsPerSample = 32;
        u.fFramesPerSecond = rate;
        u.dwChannelMask = wf.dwChannelMask;
    }
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IAudioMediaType)) { *ppv = this; AddRef(); return S_OK; }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return (ULONG)++refs; }
    STDMETHODIMP_(ULONG) Release() override {
        const long r = --refs;
        if (r == 0) delete this;
        return (ULONG)r;
    }
    STDMETHODIMP IsCompressedFormat(BOOL* c) override { *c = FALSE; return S_OK; }
    STDMETHODIMP IsEqual(IAudioMediaType*, DWORD* f) override { *f = 0; return S_OK; }
    STDMETHODIMP_(const WAVEFORMATEX*) GetAudioFormat() override { return &wf.Format; }
    STDMETHODIMP GetUncompressedAudioFormat(UNCOMPRESSEDAUDIOFORMAT* p) override { *p = u; return S_OK; }
};

static int runOnce(HMODULE dll, UINT channels) {
    std::printf("--- %u channel(s) ---\n", channels);
    auto getClass = (HRESULT(STDAPICALLTYPE*)(REFCLSID, REFIID, void**))GetProcAddress(dll, "DllGetClassObject");
    CHECK(getClass != nullptr, "DllGetClassObject exported");
    if (!getClass) return 1;
    IClassFactory* cf = nullptr;
    HRESULT hr = getClass(CLSID_WarmVoiceApo, __uuidof(IClassFactory), (void**)&cf);
    CHECK(SUCCEEDED(hr) && cf, "class factory (0x%08lx)", (unsigned long)hr);
    if (!cf) return 1;

    // COM rule: aggregated creation for anything but IUnknown must be refused.
    Outer* outer = new Outer;
    void* bad = nullptr;
    hr = cf->CreateInstance(outer, __uuidof(IAudioProcessingObject), &bad);
    CHECK(hr == CLASS_E_NOAGGREGATION && !bad, "aggregated non-IUnknown request refused");

    // What audiodg does: create aggregated, asking for IUnknown.
    hr = cf->CreateInstance(outer, __uuidof(IUnknown), (void**)&outer->inner);
    CHECK(SUCCEEDED(hr) && outer->inner, "aggregated creation (0x%08lx)", (unsigned long)hr);
    cf->Release();
    if (!outer->inner) return 1;

    IAudioProcessingObject* apo = nullptr;
    IAudioProcessingObjectRT* rt = nullptr;
    IAudioProcessingObjectConfiguration* cfg = nullptr;
    IAudioSystemEffects2* fx = nullptr;
    CHECK(SUCCEEDED(outer->QueryInterface(__uuidof(IAudioProcessingObject), (void**)&apo)), "IAudioProcessingObject");
    CHECK(SUCCEEDED(outer->QueryInterface(__uuidof(IAudioProcessingObjectRT), (void**)&rt)), "IAudioProcessingObjectRT");
    CHECK(SUCCEEDED(outer->QueryInterface(__uuidof(IAudioProcessingObjectConfiguration), (void**)&cfg)),
          "IAudioProcessingObjectConfiguration");
    CHECK(SUCCEEDED(outer->QueryInterface(__uuidof(IAudioSystemEffects2), (void**)&fx)), "IAudioSystemEffects2");
    if (!apo || !rt || !cfg || !fx) return 1;
    IUnknown* back = nullptr;
    apo->QueryInterface(__uuidof(IUnknown), (void**)&back);
    CHECK(back == static_cast<IUnknown*>(outer), "IUnknown of an interface is the outer object (identity rule)");
    if (back) back->Release();

    APO_REG_PROPERTIES* reg = nullptr;
    CHECK(SUCCEEDED(apo->GetRegistrationProperties(&reg)) && reg && reg->clsid == CLSID_WarmVoiceApo, "registration properties");
    if (reg) CoTaskMemFree(reg);

    APOInitSystemEffects2 init{};
    init.APOInit.cbSize = sizeof(init);
    init.APOInit.clsid = CLSID_WarmVoiceApo;
    init.AudioProcessingMode = AUDIO_SIGNALPROCESSINGMODE_DEFAULT;
    init.InitializeForDiscoveryOnly = FALSE;
    hr = apo->Initialize(sizeof(init), (BYTE*)&init);
    CHECK(SUCCEEDED(hr), "Initialize (0x%08lx)", (unsigned long)hr);

    MediaType* mt = new MediaType(48000.0f, channels);
    IAudioMediaType* sup = nullptr;
    hr = apo->IsInputFormatSupported(nullptr, mt, &sup);
    CHECK(hr == S_OK && sup, "float32 48 kHz accepted");
    if (sup) sup->Release();
    sup = nullptr;
    hr = apo->IsOutputFormatSupported(mt, mt, &sup);
    CHECK(hr == S_OK && sup, "output format accepted");
    if (sup) sup->Release();

    const UINT32 frames = 480;
    std::vector<float> inBuf(frames * channels), outBuf(frames * channels);
    APO_CONNECTION_DESCRIPTOR din{}, dout{};
    din.Type = APO_CONNECTION_BUFFER_TYPE_EXTERNAL;
    din.pBuffer = (UINT_PTR)inBuf.data();
    din.u32MaxFrameCount = frames;
    din.pFormat = mt;
    din.u32Signature = APO_CONNECTION_DESCRIPTOR_SIGNATURE;
    dout = din;
    dout.pBuffer = (UINT_PTR)outBuf.data();
    APO_CONNECTION_DESCRIPTOR* ins[1] = {&din};
    APO_CONNECTION_DESCRIPTOR* outs[1] = {&dout};
    hr = cfg->LockForProcess(1, ins, 1, outs);
    CHECK(SUCCEEDED(hr), "LockForProcess (0x%08lx)", (unsigned long)hr);

    APO_CONNECTION_PROPERTY pin{}, pout{};
    pin.pBuffer = (UINT_PTR)inBuf.data();
    pin.u32ValidFrameCount = frames;
    pin.u32BufferFlags = BUFFER_VALID;
    pin.u32Signature = APO_CONNECTION_PROPERTY_SIGNATURE;
    pout = pin;
    pout.pBuffer = (UINT_PTR)outBuf.data();
    APO_CONNECTION_PROPERTY* pins[1] = {&pin};
    APO_CONNECTION_PROPERTY* pouts[1] = {&pout};
    double outEnergy = 0;
    bool finite = true;
    unsigned seed = 1;
    for (int blk = 0; blk < 400; ++blk) {   // 4 s: "speech" (tone bursts) + light noise
        for (UINT32 i = 0; i < frames; ++i) {
            const double t = (blk * frames + i) / 48000.0;
            seed = seed * 1664525u + 1013904223u;
            const float noise = ((seed >> 9) / 8388608.0f - 1.0f) * 0.003f;
            const float voice = (std::fmod(t, 1.0) < 0.6) ? 0.1f * (float)std::sin(2 * 3.14159265 * 220 * t) : 0.0f;
            for (UINT c = 0; c < channels; ++c) inBuf[i * channels + c] = voice + noise;
        }
        rt->APOProcess(1, pins, 1, pouts);
        for (float v : outBuf) { finite &= std::isfinite(v) != 0; if (blk >= 200) outEnergy += (double)v * v; }
    }
    CHECK(pout.u32ValidFrameCount == frames && pout.u32BufferFlags == BUFFER_VALID, "output buffer valid");
    CHECK(finite, "output has no NaN / Inf");
    std::printf("info  output energy in the last 2 s: %.6f\n", outEnergy);
    {
        wv::LinkHandle L;
        const bool linked = wv::openLink(L"default", L, false);
        CHECK(linked, "shared memory with the app is there (%s)", linked ? (L.viaFile ? "file" : "global") : "missing");
        if (linked) {
            const unsigned long long hb = L.block->heartbeat.load();
            CHECK(hb >= 400, "heartbeat counts processed blocks (%llu)", hb);
        }
        L.close();
    }
    CHECK(SUCCEEDED(cfg->UnlockForProcess()), "UnlockForProcess");

    fx->Release();
    cfg->Release();
    rt->Release();
    apo->Release();
    mt->Release();
    outer->Release();   // last reference: outer releases inner, the APO deletes itself
    auto canUnload = (HRESULT(STDAPICALLTYPE*)())GetProcAddress(dll, "DllCanUnloadNow");
    CHECK(canUnload && canUnload() == S_OK, "all objects released (DllCanUnloadNow)");
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 2) { std::printf("usage: apohosttest WarmVoiceApo.dll\n"); return 2; }
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    HMODULE dll = LoadLibraryA(argv[1]);
    CHECK(dll != nullptr, "load %s", argv[1]);
    if (!dll) return 1;
    runOnce(dll, 1);
    runOnce(dll, 2);
    std::printf(g_fail ? "\n%d check(s) FAILED\n" : "\nall checks passed\n", g_fail);
    CoUninitialize();
    return g_fail ? 1 : 0;
}
