// WarmVoice capture APO. Loaded by the Windows audio engine (audiodg.exe) for the microphone
// endpoints it is installed on, preferably as an endpoint effect (EFX): EFX runs once per
// microphone - shared by every app using it - and is applied even to "raw" streams.
//
// Real-time rules followed here: APOProcess never allocates, locks, waits or calls COM.
// Everything that allocates happens in LockForProcess.
// SPDX-License-Identifier: MIT
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <initguid.h>   // instantiate the SDK GUID / PROPERTYKEY constants used below
#include <audioenginebaseapo.h>
#include <audioengineextensionapo.h>
#include <mmdeviceapi.h>
#include <olectl.h>   // SELFREG_E_CLASS
#include <functiondiscoverykeys_devpkey.h>
#include <ks.h>
#include <ksmedia.h>
#include <propvarutil.h>
#include <sddl.h>

#include <atomic>
#include <memory>
#include <new>
#include <string>

#include "guids.h"
#include "wv/pipeline.h"
#include "wv/resolve.h"
#include "wv/shared.h"
#include "wv/winlink.h"

using namespace wv;

static HMODULE g_module = nullptr;
// Diagnostics: %ProgramData%\WarmVoice\apo.log (only on setup calls, never per audio block).
#define WV_LOG(...) wv::logTo(L"apo.log", __VA_ARGS__)
static std::atomic<long> g_objects{0}, g_locks{0}, g_creates{0};

// {1DA5D803-D492-4EDD-8C23-E0C0FFEE7F0E},4  - endpoint GUID string
static const PROPERTYKEY kPkeyEndpointGuid = {{0x1da5d803, 0xd492, 0x4edd, {0x8c, 0x23, 0xe0, 0xc0, 0xff, 0xee, 0x7f, 0x0e}}, 4};

class WarmVoiceApo final : public IAudioProcessingObject,
                           public IAudioProcessingObjectRT,
                           public IAudioProcessingObjectConfiguration,
                           public IAudioSystemEffects2 {
public:
    // The Windows audio engine creates APOs with COM aggregation (it passes an outer object),
    // so all IUnknown calls on our interfaces go to the "outer" object; the object's own
    // reference count lives in the separate non-delegating "inner" IUnknown below.
    explicit WarmVoiceApo(IUnknown* outer) : outer_(outer ? outer : &inner_) {
        inner_.self = this;
        ++g_objects;
    }
    ~WarmVoiceApo() {
        closeShared();
        --g_objects;
    }

    struct Inner final : IUnknown {
        WarmVoiceApo* self = nullptr;
        std::atomic<long> refs{1};
        STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override { return self->innerQI(riid, ppv); }
        STDMETHODIMP_(ULONG) AddRef() override { return (ULONG)++refs; }
        STDMETHODIMP_(ULONG) Release() override {
            const long r = --refs;
            if (r == 0) delete self;
            return (ULONG)r;
        }
    };
    IUnknown* inner() { return &inner_; }

    HRESULT innerQI(REFIID riid, void** ppv) {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (riid == __uuidof(IUnknown)) {
            *ppv = static_cast<IUnknown*>(&inner_);
            inner_.AddRef();
            return S_OK;
        }
        if (riid == __uuidof(IAudioProcessingObject)) *ppv = static_cast<IAudioProcessingObject*>(this);
        else if (riid == __uuidof(IAudioProcessingObjectRT)) *ppv = static_cast<IAudioProcessingObjectRT*>(this);
        else if (riid == __uuidof(IAudioProcessingObjectConfiguration))
            *ppv = static_cast<IAudioProcessingObjectConfiguration*>(this);
        else if (riid == __uuidof(IAudioSystemEffects) || riid == __uuidof(IAudioSystemEffects2))
            *ppv = static_cast<IAudioSystemEffects2*>(this);
        else {
            if (++unknownQi_ <= 12) {
                wchar_t g[64] = {};
                StringFromGUID2(riid, g, 64);
                WV_LOG("asked for interface %ls: not supported", g);
            }
            return E_NOINTERFACE;
        }
        static_cast<IUnknown*>(*ppv)->AddRef();   // goes to the outer object, as COM requires
        return S_OK;
    }

    // ---- IUnknown of every interface: delegate ----
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override { return outer_->QueryInterface(riid, ppv); }
    STDMETHODIMP_(ULONG) AddRef() override { return outer_->AddRef(); }
    STDMETHODIMP_(ULONG) Release() override { return outer_->Release(); }

    // ---- IAudioProcessingObject ----
    STDMETHODIMP Reset() override {
        if (pipeline_) pipeline_->reset();
        return S_OK;
    }
    STDMETHODIMP GetLatency(HNSTIME* t) override {
        if (!t) return E_POINTER;
        *t = (pipeline_ && rate_ > 0) ? (HNSTIME)(10000000.0 * pipeline_->latencyFrames() / rate_) : 0;
        return S_OK;
    }
    STDMETHODIMP GetRegistrationProperties(APO_REG_PROPERTIES** pp) override {
        if (!pp) return E_POINTER;
        auto* p = (APO_REG_PROPERTIES*)CoTaskMemAlloc(sizeof(APO_REG_PROPERTIES));
        if (!p) return E_OUTOFMEMORY;
        ZeroMemory(p, sizeof(*p));
        p->clsid = CLSID_WarmVoiceApo;
        p->Flags = APO_FLAG_DEFAULT;
        wcscpy_s(p->szFriendlyName, WV_APO_NAME);
        wcscpy_s(p->szCopyrightInfo, L"MIT License");
        p->u32MajorVersion = 0;
        p->u32MinorVersion = 1;
        p->u32MinInputConnections = 1;
        p->u32MaxInputConnections = 1;
        p->u32MinOutputConnections = 1;
        p->u32MaxOutputConnections = 1;
        p->u32MaxInstances = 0xffffffff;
        p->u32NumAPOInterfaces = 1;
        p->iidAPOInterfaceList[0] = __uuidof(IAudioProcessingObject);
        *pp = p;
        WV_LOG("GetRegistrationProperties");
        return S_OK;
    }
    STDMETHODIMP Initialize(UINT32 cbDataSize, BYTE* pbyData) override {
        // Remember which endpoint we serve, to name the shared-memory block.
        endpointGuid_.clear();
        bool discovery = false;
        IMMDeviceCollection* devs = nullptr;
        IPropertyStore* props = nullptr;
        if (pbyData && cbDataSize >= sizeof(APOInitSystemEffects2)) {
            auto* init = reinterpret_cast<APOInitSystemEffects2*>(pbyData);
            discovery = init->InitializeForDiscoveryOnly != FALSE;
            devs = init->pDeviceCollection;
            props = init->pAPOEndpointProperties;
            mode_ = init->AudioProcessingMode;
        } else if (pbyData && cbDataSize >= sizeof(APOInitSystemEffects)) {
            auto* init = reinterpret_cast<APOInitSystemEffects*>(pbyData);
            devs = init->pDeviceCollection;
            props = init->pAPOEndpointProperties;
        }
        if (!discovery) {
            // The last device in the collection is always the endpoint itself; its id ends in
            // the same {guid} the app and the installer use.
            if (devs) {
                UINT n = 0;
                IMMDevice* d = nullptr;
                if (SUCCEEDED(devs->GetCount(&n)) && n > 0 && SUCCEEDED(devs->Item(n - 1, &d)) && d) {
                    LPWSTR id = nullptr;
                    if (SUCCEEDED(d->GetId(&id)) && id) { endpointGuid_ = guidFromEndpointId(id); CoTaskMemFree(id); }
                    d->Release();
                }
            }
            if (endpointGuid_.empty()) readEndpointGuid(props);
        }
        wchar_t m[64] = {};
        StringFromGUID2(mode_, m, 64);
        WV_LOG("Initialize size=%u discovery=%d endpoint=%ls mode=%ls", cbDataSize, (int)discovery,
               endpointGuid_.empty() ? L"(unknown)" : endpointGuid_.c_str(), m);
        return S_OK;
    }
    STDMETHODIMP IsInputFormatSupported(IAudioMediaType* opposite, IAudioMediaType* requested,
                                        IAudioMediaType** supported) override {
        return checkFormat(opposite, requested, supported);
    }
    STDMETHODIMP IsOutputFormatSupported(IAudioMediaType* opposite, IAudioMediaType* requested,
                                         IAudioMediaType** supported) override {
        return checkFormat(opposite, requested, supported);
    }
    STDMETHODIMP GetInputChannelCount(UINT32* n) override {
        if (!n) return E_POINTER;
        *n = (UINT32)channels_;
        return S_OK;
    }

    // ---- IAudioProcessingObjectConfiguration ----
    STDMETHODIMP LockForProcess(UINT32 nIn, APO_CONNECTION_DESCRIPTOR** in, UINT32 nOut,
                                APO_CONNECTION_DESCRIPTOR** out) override {
        if (nIn != 1 || nOut != 1 || !in || !out || !in[0]->pFormat) { WV_LOG("LockForProcess: bad connections"); return E_INVALIDARG; }
        UNCOMPRESSEDAUDIOFORMAT f{};
        HRESULT hr = in[0]->pFormat->GetUncompressedAudioFormat(&f);
        if (FAILED(hr)) { WV_LOG("LockForProcess: no format (0x%08lx)", (unsigned long)hr); return hr; }
        if (f.guidFormatType != KSDATAFORMAT_SUBTYPE_IEEE_FLOAT || f.dwBytesPerSampleContainer != 4) {
            WV_LOG("LockForProcess: format not float32");
            return APOERR_FORMAT_NOT_SUPPORTED;
        }
        rate_ = (int)(f.fFramesPerSecond + 0.5f);
        channels_ = (int)f.dwSamplesPerFrame;
        try {
            pipeline_ = std::make_unique<Pipeline>();
            PipelineOptions opt;
            opt.aiLibraryPath = moduleDir() + "warmvoice_ai.dll";
            pipeline_->configure(rate_, channels_, opt);
        } catch (...) {
            pipeline_.reset();
            WV_LOG("LockForProcess: pipeline setup failed");
            return E_OUTOFMEMORY;
        }
        // Until the app sends settings, run the defaults (warm tone, everything on).
        UserParams u;
        Analysis a;
        a.inputRate = rate_;
        pipeline_->setParams(resolve(u, a));
        openShared();
        // No app has sent settings into the shared block yet: use the last saved ones.
        if (!shared_ || shared_->paramSeq.load() == 0) {
            Resolved saved;
            if (loadParams(endpointGuid_.empty() ? std::wstring(L"default") : endpointGuid_, saved)) {
                pipeline_->setParams(saved);
                WV_LOG("LockForProcess: using the saved settings");
            }
        }
        firstBlock_ = true;
        locked_ = true;
        {
            Analysis now;
            pipeline_->getAnalysis(now);
            WV_LOG("LockForProcess: %d Hz, %d ch, AI %s (problem %d), link %s", rate_, channels_,
                   now.aiActive ? "on" : "off", now.aiProblem, shared_ ? (link_.viaFile ? "file" : "global") : "NONE");
        }
        return S_OK;
    }
    STDMETHODIMP UnlockForProcess() override {
        locked_ = false;
        closeShared();
        pipeline_.reset();
        return S_OK;
    }

    // ---- IAudioProcessingObjectRT ----
    STDMETHODIMP_(void) APOProcess(UINT32 nIn, APO_CONNECTION_PROPERTY** in, UINT32 nOut,
                                   APO_CONNECTION_PROPERTY** out) override {
        if (nIn != 1 || nOut != 1) return;
        APO_CONNECTION_PROPERTY* ip = in[0];
        APO_CONNECTION_PROPERTY* op = out[0];
        const UINT32 frames = ip->u32ValidFrameCount;
        float* src = reinterpret_cast<float*>(ip->pBuffer);
        float* dst = reinterpret_cast<float*>(op->pBuffer);
        const size_t samples = (size_t)frames * channels_;
        if (ip->u32BufferFlags == BUFFER_SILENT) ZeroMemory(dst, samples * sizeof(float));
        else if (dst != src) CopyMemory(dst, src, samples * sizeof(float));

        if (firstBlock_) {   // once per stream start: proves Windows really sends us audio
            firstBlock_ = false;
            WV_LOG("first audio block: %u frames", frames);
        }
        if (pipeline_ && locked_) {
            pollParams();
            tapRaw(dst, frames);
            pipeline_->process(dst, (int)frames);
            publishAnalysis();
        }
        op->u32ValidFrameCount = frames;
        op->u32BufferFlags = BUFFER_VALID;
    }
    STDMETHODIMP_(UINT32) CalcInputFrames(UINT32 n) override { return n; }
    STDMETHODIMP_(UINT32) CalcOutputFrames(UINT32 n) override { return n; }

    // ---- IAudioSystemEffects2 ----
    STDMETHODIMP GetEffectsList(LPGUID* ids, UINT* count, HANDLE) override {
        if (!ids || !count) return E_POINTER;
        *ids = nullptr;
        *count = 0;   // no individually switchable Windows "effects"; the app controls everything
        return S_OK;
    }

private:
    static std::string moduleDir() {
        char path[MAX_PATH] = {};
        GetModuleFileNameA(g_module, path, MAX_PATH);
        std::string s(path);
        const size_t p = s.find_last_of("\\/");
        return p == std::string::npos ? std::string() : s.substr(0, p + 1);
    }

    void readEndpointGuid(IPropertyStore* ps) {
        if (!ps) return;
        PROPVARIANT v;
        PropVariantInit(&v);
        if (SUCCEEDED(ps->GetValue(kPkeyEndpointGuid, &v)) && v.vt == VT_LPWSTR && v.pwszVal) {
            endpointGuid_ = v.pwszVal;
            // Object names are case-sensitive; the app and installer build the same name from
            // the endpoint id, so everyone uses lower case.
            for (auto& ch : endpointGuid_) ch = (wchar_t)towlower(ch);
        }
        PropVariantClear(&v);
    }

    HRESULT checkFormat(IAudioMediaType* opposite, IAudioMediaType* requested, IAudioMediaType** supported) {
        if (!requested || !supported) return E_POINTER;
        *supported = nullptr;
        UNCOMPRESSEDAUDIOFORMAT f{};
        HRESULT hr = requested->GetUncompressedAudioFormat(&f);
        if (FAILED(hr)) return hr;
        const bool isFloat = f.guidFormatType == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT && f.dwBytesPerSampleContainer == 4;
        const bool okShape = f.dwSamplesPerFrame >= 1 && f.dwSamplesPerFrame <= 16 && f.fFramesPerSecond >= 8000.0f &&
                             f.fFramesPerSecond <= 384000.0f;
        if (!isFloat || !okShape) {
            if (++rejects_ <= 4) WV_LOG("format rejected: float=%d bits=%u ch=%u rate=%.0f", (int)isFloat, f.dwBytesPerSampleContainer * 8,
                   f.dwSamplesPerFrame, f.fFramesPerSecond);
            return APOERR_FORMAT_NOT_SUPPORTED;
        }
        // In-place effect: input and output must match.
        if (opposite) {
            UNCOMPRESSEDAUDIOFORMAT o{};
            if (SUCCEEDED(opposite->GetUncompressedAudioFormat(&o)) &&
                (o.dwSamplesPerFrame != f.dwSamplesPerFrame || o.fFramesPerSecond != f.fFramesPerSecond))
                return APOERR_FORMAT_NOT_SUPPORTED;
        }
        requested->AddRef();
        *supported = requested;
        return S_OK;
    }

    // ---- shared memory with the app ----
    void openShared() {
        closeShared();
        const std::wstring g = endpointGuid_.empty() ? std::wstring(L"default") : endpointGuid_;
        DWORD e1 = 0, e2 = 0;
        if (!createLink(g, link_, &e1, &e2)) {
            WV_LOG("shared memory failed: global error %lu, file error %lu", e1, e2);
            return;
        }
        if (link_.viaFile) WV_LOG("global shared memory not allowed (error %lu), using the file", e1);
        shared_ = link_.block;
        if (link_.created || shared_->magic != kSharedMagic || shared_->size != sizeof(SharedBlock)) {
            ZeroMemory((void*)shared_, sizeof(SharedBlock));
            shared_->size = sizeof(SharedBlock);
            shared_->magic = kSharedMagic;
        }
        shared_->recordRate.store((uint32_t)rate_);
        lastParamSeq_ = 0;
    }
    void closeShared() {
        link_.close();
        shared_ = nullptr;
    }
    void pollParams() {
        if (!shared_) return;
        const uint32_t seq = shared_->paramSeq.load(std::memory_order_acquire);
        if (seq != lastParamSeq_ && seq != 0) {
            Resolved r;
            uint32_t seen = 0;
            if (seqRead(shared_->paramSeq, shared_->params, r, &seen) && r.version == Resolved().version) {
                pipeline_->setParams(r);
                lastParamSeq_ = seen;
            }
        }
        const uint32_t rp = shared_->resetProfile.load(std::memory_order_relaxed);
        if (rp != lastResetProfile_) { pipeline_->resetProfile(); lastResetProfile_ = rp; }
    }
    void tapRaw(const float* x, UINT32 frames) {
        if (!shared_) return;
        uint64_t w = shared_->recordWritten.load(std::memory_order_relaxed);
        for (UINT32 i = 0; i < frames; ++i) {
            float s = 0;
            for (int c = 0; c < channels_; ++c) s += x[(size_t)i * channels_ + c];
            shared_->record[w % kRecordCapacity] = s / (float)channels_;
            ++w;
        }
        shared_->recordWritten.store(w, std::memory_order_release);
    }
    void publishAnalysis() {
        if (!shared_) return;
        shared_->heartbeat.fetch_add(1, std::memory_order_relaxed);
        if (++analysisTick_ < 10) return;   // about every 100 ms
        analysisTick_ = 0;
        pipeline_->getAnalysis(analysis_);
        seqWrite(shared_->analysisSeq, shared_->analysis, analysis_);
    }

    Inner inner_;
    IUnknown* outer_;
    int unknownQi_ = 0;
    std::unique_ptr<Pipeline> pipeline_;
    int rate_ = 48000, channels_ = 1;
    bool locked_ = false;
    std::wstring endpointGuid_;
    LinkHandle link_;
    SharedBlock* shared_ = nullptr;
    GUID mode_{};
    int rejects_ = 0;
    bool firstBlock_ = false;
    uint32_t lastParamSeq_ = 0, lastResetProfile_ = 0;
    int analysisTick_ = 0;
    Analysis analysis_;
};

// ---------------------------------------------------------------------------- COM plumbing
class Factory final : public IClassFactory {
public:
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IClassFactory)) { *ppv = this; AddRef(); return S_OK; }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return 2; }
    STDMETHODIMP_(ULONG) Release() override { return 1; }
    STDMETHODIMP CreateInstance(IUnknown* outer, REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        // Aggregated creation must ask for IUnknown (COM rule); the audio engine does.
        if (outer && riid != __uuidof(IUnknown)) { WV_LOG("CreateInstance: aggregated but not IUnknown"); return CLASS_E_NOAGGREGATION; }
        auto* apo = new (std::nothrow) WarmVoiceApo(outer);
        if (!apo) return E_OUTOFMEMORY;
        IUnknown* in = apo->inner();
        const HRESULT hr = in->QueryInterface(riid, ppv);
        in->Release();   // drop the creation reference; the caller now holds its own
        if (++g_creates <= 6) WV_LOG("CreateInstance: %s, result 0x%08lx", outer ? "aggregated" : "plain", (unsigned long)hr);
        return hr;
    }
    STDMETHODIMP LockServer(BOOL lock) override {
        if (lock) ++g_locks; else --g_locks;
        return S_OK;
    }
};
static Factory g_factory;

extern "C" BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = h;
        DisableThreadLibraryCalls(h);
        wchar_t exe[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        WV_LOG("loaded into %ls", exe);
    }
    return TRUE;
}

STDAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void** ppv) {
    if (clsid != CLSID_WarmVoiceApo) { WV_LOG("DllGetClassObject: unknown class"); return CLASS_E_CLASSNOTAVAILABLE; }
    return g_factory.QueryInterface(riid, ppv);
}

STDAPI DllCanUnloadNow() { return (g_objects == 0 && g_locks == 0) ? S_OK : S_FALSE; }

// Registration: COM class + Windows audio-engine APO entry. Installing the APO on a specific
// microphone is done by wvinstall.exe (it needs the endpoint id and admin rights).
static LONG setStr(HKEY k, const wchar_t* name, const std::wstring& v) {
    return RegSetValueExW(k, name, 0, REG_SZ, (const BYTE*)v.c_str(), (DWORD)((v.size() + 1) * sizeof(wchar_t)));
}
static LONG setDword(HKEY k, const wchar_t* name, DWORD v) {
    return RegSetValueExW(k, name, 0, REG_DWORD, (const BYTE*)&v, sizeof(v));
}

STDAPI DllRegisterServer() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(g_module, path, MAX_PATH);
    HKEY k;
    std::wstring clsidKey = std::wstring(L"SOFTWARE\\Classes\\CLSID\\") + WV_CLSID_STRING;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, clsidKey.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr)) return SELFREG_E_CLASS;
    setStr(k, nullptr, WV_APO_NAME);
    RegCloseKey(k);
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, (clsidKey + L"\\InprocServer32").c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr))
        return SELFREG_E_CLASS;
    setStr(k, nullptr, path);
    setStr(k, L"ThreadingModel", L"Both");
    RegCloseKey(k);

    std::wstring apoKey = std::wstring(L"SOFTWARE\\Classes\\AudioEngine\\AudioProcessingObjects\\") + WV_CLSID_STRING;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, apoKey.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr)) return SELFREG_E_CLASS;
    setStr(k, L"FriendlyName", WV_APO_NAME);
    setStr(k, L"Copyright", L"MIT License");
    setDword(k, L"MajorVersion", 0);
    setDword(k, L"MinorVersion", 1);
    setDword(k, L"Flags", APO_FLAG_DEFAULT);
    setDword(k, L"MinInputConnections", 1);
    setDword(k, L"MaxInputConnections", 1);
    setDword(k, L"MinOutputConnections", 1);
    setDword(k, L"MaxOutputConnections", 1);
    setDword(k, L"MaxInstances", 0xffffffff);
    setDword(k, L"NumAPOInterfaces", 1);
    wchar_t iid[64];
    StringFromGUID2(__uuidof(IAudioProcessingObject), iid, 64);
    setStr(k, L"APOInterface0", iid);
    RegCloseKey(k);
    return S_OK;
}

STDAPI DllUnregisterServer() {
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, (std::wstring(L"SOFTWARE\\Classes\\CLSID\\") + WV_CLSID_STRING).c_str());
    RegDeleteTreeW(HKEY_LOCAL_MACHINE,
                   (std::wstring(L"SOFTWARE\\Classes\\AudioEngine\\AudioProcessingObjects\\") + WV_CLSID_STRING).c_str());
    return S_OK;
}
