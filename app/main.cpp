// WarmVoice.exe - the settings panel. The sound is processed by the APO inside the Windows
// audio engine; this app only sends settings, shows meters, and runs the A/B recording.
// Closing the window keeps it in the tray; the APO keeps working even when the app is not
// running at all.
// SPDX-License-Identifier: MIT
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <knownfolders.h>
#include <wrl.h>
#include <WebView2.h>
#include <propkeydef.h>
// Instantiate only the device PROPERTYKEYs used here; initguid must come after the headers
// above (shlobj/cguid break when it is included first).
#include <initguid.h>
#include <functiondiscoverykeys_devpkey.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "wv/pipeline.h"
#include "wv/resolve.h"
#include "wv/shared.h"
#include "wv/winlink.h"

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;
using namespace wv;

namespace {

constexpr UINT WM_TRAY = WM_APP + 1;
constexpr UINT WM_RECDONE = WM_APP + 2;
constexpr UINT_PTR kTimer = 1;

HWND g_hwnd = nullptr;
ComPtr<ICoreWebView2Controller> g_controller;
ComPtr<ICoreWebView2> g_webview;
std::wstring g_exeDir;

// ----------------------------------------------------------------------------- helpers
std::wstring exeDir() {
    wchar_t p[MAX_PATH];
    GetModuleFileNameW(nullptr, p, MAX_PATH);
    std::wstring s(p);
    return s.substr(0, s.find_last_of(L"\\/") + 1);
}
std::wstring appDataDir() {
    PWSTR p = nullptr;
    std::wstring d;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &p))) d = std::wstring(p) + L"\\WarmVoice";
    CoTaskMemFree(p);
    CreateDirectoryW(d.c_str(), nullptr);
    return d;
}
std::string toUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}
std::wstring jsonStr(const std::wstring& s) {
    std::wstring o = L"\"";
    for (wchar_t c : s) {
        if (c == L'"' || c == L'\\') { o += L'\\'; o += c; }
        else if (c < 0x20) o += L' ';
        else o += c;
    }
    return o + L"\"";
}
std::wstring num(double v) {
    if (!std::isfinite(v)) v = -120;
    wchar_t b[32];
    swprintf(b, 32, L"%.3f", v);
    return b;
}
void post(const std::wstring& json) {
    if (g_webview) g_webview->PostWebMessageAsJson(json.c_str());
}

// ----------------------------------------------------------------------------- settings
// The UI speaks in "key|value" pairs; the same keys are stored in settings.ini.
struct Settings {
    UserParams u;
    std::wstring mic;   // endpoint id
    double zoom = 0.85; // page zoom (Ctrl + mouse wheel changes it; remembered)

    bool set(const std::string& k, double v) {
        auto st = [&](int i) { u.stageOn[i] = v != 0; return true; };
        if (k == "zoom") { zoom = std::max(0.5, std::min(2.0, v)); return true; }
        if (k == "enabled") { u.enabled = v != 0; return true; }
        if (k == "tone") { u.tone = (Tone)(int)v; return true; }
        if (k == "volume") { u.volume = (float)v; return true; }
        if (k == "denoise") { u.denoiseStrength = (float)v; return true; }
        if (k == "restore") { u.restoreAmount = (float)v; return true; }
        if (k == "optimize") { u.optimizeAmount = (float)v; return true; }
        if (k == "texture") { u.textureAmount = (float)v; return true; }
        if (k == "textureStyle") { u.textureStyle = (TextureStyle)(int)v; return true; }
        if (k == "compThr") { u.compThresholdDb = (float)v; return true; }
        if (k == "compRatio") { u.compRatio = (float)v; return true; }
        if (k == "compSpeed") { u.compSpeed = (int)v; return true; }
        if (k.rfind("stage.", 0) == 0) { const int i = std::atoi(k.c_str() + 6); if (i > 0 && i < (int)Stage::Count) return st(i); }
        if (k.rfind("eq.", 0) == 0 && k.size() > 5) {
            const int i = k[3] - '0';
            const std::string f = k.substr(5);
            if (i < 0 || i >= kEqNodes) return false;
            if (f == "hz") u.eq[i].hz = (float)v;
            else if (f == "gain") u.eq[i].gainDb = (float)v;
            else if (f == "q") u.eq[i].q = (float)v;
            return true;
        }
        return false;
    }
    std::map<std::string, double> all() const {
        std::map<std::string, double> m;
        m["enabled"] = u.enabled; m["tone"] = (int)u.tone; m["volume"] = u.volume; m["denoise"] = u.denoiseStrength;
        m["restore"] = u.restoreAmount; m["optimize"] = u.optimizeAmount; m["texture"] = u.textureAmount;
        m["textureStyle"] = (int)u.textureStyle; m["compThr"] = u.compThresholdDb; m["compRatio"] = u.compRatio;
        m["compSpeed"] = u.compSpeed;
        m["zoom"] = zoom;
        for (int i = 1; i < (int)Stage::Count; ++i) m["stage." + std::to_string(i)] = u.stageOn[i];
        for (int i = 0; i < kEqNodes; ++i) {
            m["eq." + std::to_string(i) + ".hz"] = u.eq[i].hz;
            m["eq." + std::to_string(i) + ".gain"] = u.eq[i].gainDb;
            m["eq." + std::to_string(i) + ".q"] = u.eq[i].q;
        }
        return m;
    }
    std::wstring path() const { return appDataDir() + L"\\settings.ini"; }
    void save() const {
        std::ofstream f(path());
        for (auto& kv : all()) f << kv.first << "=" << kv.second << "\n";
        f << "mic=" << toUtf8(mic) << "\n";
    }
    void load() {
        std::ifstream f(path());
        std::string line;
        while (std::getline(f, line)) {
            const size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            const std::string k = line.substr(0, eq), v = line.substr(eq + 1);
            if (k == "mic") {
                const int n = MultiByteToWideChar(CP_UTF8, 0, v.c_str(), (int)v.size(), nullptr, 0);
                mic.assign(n, L'\0');
                MultiByteToWideChar(CP_UTF8, 0, v.c_str(), (int)v.size(), &mic[0], n);
            } else {
                set(k, std::atof(v.c_str()));
            }
        }
    }
} g_set;

// ----------------------------------------------------------------------------- microphones
struct Mic { std::wstring id, guid, name; bool installed = false; };
std::vector<Mic> listMics(std::wstring* defaultId) {
    std::vector<Mic> out;
    ComPtr<IMMDeviceEnumerator> en;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en)))) return out;
    ComPtr<IMMDevice> def;
    if (defaultId && SUCCEEDED(en->GetDefaultAudioEndpoint(eCapture, eConsole, &def))) {
        LPWSTR id = nullptr;
        if (SUCCEEDED(def->GetId(&id))) { *defaultId = id; CoTaskMemFree(id); }
    }
    ComPtr<IMMDeviceCollection> col;
    if (FAILED(en->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &col))) return out;
    UINT n = 0;
    col->GetCount(&n);
    for (UINT i = 0; i < n; ++i) {
        ComPtr<IMMDevice> d;
        if (FAILED(col->Item(i, &d))) continue;
        Mic m;
        LPWSTR id = nullptr;
        if (SUCCEEDED(d->GetId(&id))) { m.id = id; CoTaskMemFree(id); }
        const size_t dot = m.id.find_last_of(L'.');
        m.guid = dot == std::wstring::npos ? m.id : m.id.substr(dot + 1);
        for (auto& c : m.guid) c = (wchar_t)towlower(c);
        ComPtr<IPropertyStore> ps;
        if (SUCCEEDED(d->OpenPropertyStore(STGM_READ, &ps))) {
            PROPVARIANT v;
            PropVariantInit(&v);
            if (SUCCEEDED(ps->GetValue(PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR) m.name = v.pwszVal;
            PropVariantClear(&v);
        }
        HKEY k;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, (L"SOFTWARE\\WarmVoice\\Backup\\" + m.guid).c_str(), 0, KEY_READ, &k) == ERROR_SUCCESS) {
            m.installed = true;
            RegCloseKey(k);
        }
        out.push_back(m);
    }
    return out;
}

// ----------------------------------------------------------------------------- APO link
struct Link {
    LinkHandle h;
    SharedBlock* b = nullptr;
    std::wstring guid;
    uint64_t lastBeat = 0;
    int stillHops = 0;
    int retry = 0;
    bool fresh = false;   // just connected: send the settings

    void close() {
        h.close();
        b = nullptr;
    }
    // The APO creates the block when audio first flows; keep trying (every 0.5 s) until then.
    bool open(const std::wstring& g) {
        if (b && g == guid) return true;
        if (g == guid && ++retry % 5 != 1) return false;
        close();
        guid = g;
        if (!openLink(g, h, true)) return false;
        b = h.block;
        fresh = true;
        return true;
    }
    bool running() {
        if (!b) return false;
        const uint64_t hb = b->heartbeat.load();
        if (hb != lastBeat) { lastBeat = hb; stillHops = 0; return true; }
        return ++stillHops < 5;   // half a second without audio = not running
    }
} g_link;

// ----------------------------------------------------------------------------- mic monitor
// While the window is open, keep the selected mic open in shared mode (like any app would) and
// throw the data away. That makes Windows run the APO, so the meters and the A/B recording
// work even when no game or chat app is using the mic, and it tells us when another program
// holds the mic exclusively (then Windows effects cannot run at all).
struct Monitor {
    enum State { Idle = 0, Ok = 1, Busy = 2, Failed = 3 };
    std::atomic<int> state{Idle};
    std::atomic<bool> quit{false};
    std::atomic<long> okSince{0};   // GetTickCount when the stream started delivering
    std::thread th;
    std::wstring id;

    void start(const std::wstring& micId) {
        if (th.joinable() && micId == id) return;
        stop();
        id = micId;
        quit = false;
        th = std::thread([this, micId] { loop(micId); });
    }
    void stop() {
        quit = true;
        if (th.joinable()) th.join();
        state = Idle;
    }
    void loop(std::wstring micId) {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        while (!quit) {
            ComPtr<IMMDeviceEnumerator> en;
            ComPtr<IMMDevice> dev;
            ComPtr<IAudioClient> ac;
            ComPtr<IAudioCaptureClient> cc;
            WAVEFORMATEX* mix = nullptr;
            HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en));
            if (SUCCEEDED(hr)) hr = en->GetDevice(micId.c_str(), &dev);
            if (SUCCEEDED(hr)) hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)ac.GetAddressOf());
            if (SUCCEEDED(hr)) hr = ac->GetMixFormat(&mix);
            if (SUCCEEDED(hr)) hr = ac->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 2000000, 0, mix, nullptr);
            if (mix) CoTaskMemFree(mix);
            if (SUCCEEDED(hr)) hr = ac->GetService(IID_PPV_ARGS(&cc));
            if (SUCCEEDED(hr)) hr = ac->Start();
            if (FAILED(hr)) {
                state = hr == AUDCLNT_E_DEVICE_IN_USE ? Busy : Failed;
                for (int i = 0; i < 20 && !quit; ++i) Sleep(100);   // try again in 2 s
                continue;
            }
            state = Ok;
            okSince = (long)GetTickCount();
            while (!quit) {
                Sleep(50);
                UINT32 n = 0;
                hr = cc->GetNextPacketSize(&n);
                if (FAILED(hr)) break;   // device lost or audio service restarted: reopen
                while (n) {
                    BYTE* d; UINT32 f; DWORD fl;
                    if (FAILED(cc->GetBuffer(&d, &f, &fl, nullptr, nullptr))) break;
                    cc->ReleaseBuffer(f);
                    if (FAILED(cc->GetNextPacketSize(&n))) break;
                }
            }
            ac->Stop();
            state = Failed;
        }
        CoUninitialize();
    }
    // Stream open for a while (3 s) - long enough that a working APO would show up.
    bool settled() const { return state == Ok && (long)GetTickCount() - okSince.load() > 3000; }
} g_mon;
bool g_repairing = false;

Analysis g_an;
Resolved g_res;
ResolveInfo g_info;
bool g_resValid = false;

void pushParams() {
    Analysis a = g_an;
    if (a.inputRate <= 0) a.inputRate = 48000;
    Resolved r = resolve(g_set.u, a, &g_info);
    const bool changed = !g_resValid || std::memcmp(&r, &g_res, sizeof r) != 0;
    g_res = r;
    g_resValid = true;
    if (changed && g_link.b) seqWrite(g_link.b->paramSeq, g_link.b->params, g_res);
}

// ----------------------------------------------------------------------------- A/B recording
struct Recorder {
    std::shared_ptr<std::vector<float>> raw, fx;   // fx replaced atomically when re-processed
    std::atomic<bool> useFx{true}, playing{false};
    std::atomic<size_t> pos{0};
    int rate = 48000;
    // recording
    bool recording = false;
    uint64_t start = 0;
    size_t want = 0;
    std::thread player, worker;
    std::atomic<bool> stopPlayer{false};
    std::atomic<int> processGen{0};

    void startRecording() {
        stop();
        if (!g_link.b) return;
        rate = (int)std::max<uint32_t>(8000, g_link.b->recordRate.load());
        want = std::min<size_t>((size_t)rate * 10, kRecordCapacity - (size_t)rate / 10);
        start = g_link.b->recordWritten.load();
        recording = true;
        raw.reset();
        fx.reset();
    }
    // called from the UI timer
    void poll() {
        if (!recording || !g_link.b) return;
        const uint64_t w = g_link.b->recordWritten.load();
        if (w - start < want) return;
        auto r = std::make_shared<std::vector<float>>(want);
        for (size_t i = 0; i < want; ++i) (*r)[i] = g_link.b->record[(start + i) % kRecordCapacity];
        raw = r;
        recording = false;
        reprocess();
    }
    // Run the same chain over the recording, with the current settings. Runs the clip twice
    // so the leveller and voice profile are settled, like after a minute of real use.
    void reprocess() {
        if (!raw) return;
        const int gen = ++processGen;
        auto src = raw;
        const Resolved r = g_res;
        const int rt = rate;
        if (worker.joinable()) worker.detach();
        worker = std::thread([this, src, r, rt, gen] {
            Pipeline p;
            PipelineOptions o;
            o.watchdog = false;
            o.aiLibraryPath = toUtf8(g_exeDir) + "warmvoice_ai.dll";
            p.configure(rt, 1, o);
            p.setParams(r);
            std::vector<float> buf = *src;
            for (size_t i = 0; i < buf.size(); i += 480) p.process(&buf[i], (int)std::min<size_t>(480, buf.size() - i));
            const int lat = p.latencyFrames();
            std::vector<float> io(src->size() + lat, 0.0f);
            std::copy(src->begin(), src->end(), io.begin());
            for (size_t i = 0; i < io.size(); i += 480) p.process(&io[i], (int)std::min<size_t>(480, io.size() - i));
            auto out = std::make_shared<std::vector<float>>(io.begin() + lat, io.begin() + lat + src->size());
            if (gen == processGen) std::atomic_store(&fx, out);
            PostMessageW(g_hwnd, WM_RECDONE, 0, 0);
        });
    }
    void play() {
        if (!raw || playing) return;
        if (pos >= raw->size()) pos = 0;
        stopPlayer = false;
        playing = true;
        if (player.joinable()) player.join();
        player = std::thread([this] { playLoop(); });
    }
    void stop() {
        stopPlayer = true;
        if (player.joinable()) player.join();
        playing = false;
    }
    void playLoop() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        ComPtr<IMMDeviceEnumerator> en;
        ComPtr<IMMDevice> dev;
        ComPtr<IAudioClient> ac;
        ComPtr<IAudioRenderClient> rc;
        HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        WAVEFORMATEX wf{};
        wf.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
        wf.nChannels = 1;
        wf.nSamplesPerSec = (DWORD)rate;
        wf.wBitsPerSample = 32;
        wf.nBlockAlign = 4;
        wf.nAvgBytesPerSec = (DWORD)rate * 4;
        bool ok = SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en))) &&
                  SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev)) &&
                  SUCCEEDED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)ac.GetAddressOf())) &&
                  SUCCEEDED(ac->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                           AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                               AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                           400000, 0, &wf, nullptr)) &&
                  SUCCEEDED(ac->SetEventHandle(ev)) && SUCCEEDED(ac->GetService(IID_PPV_ARGS(&rc))) && SUCCEEDED(ac->Start());
        UINT32 bufFrames = 0;
        if (ok) ac->GetBufferSize(&bufFrames);
        float gain = 1.0f;          // cross-fade weight of the fx signal
        while (ok && !stopPlayer) {
            WaitForSingleObject(ev, 100);
            UINT32 pad = 0;
            if (FAILED(ac->GetCurrentPadding(&pad))) break;
            const UINT32 n = bufFrames - pad;
            if (!n) continue;
            BYTE* data = nullptr;
            if (FAILED(rc->GetBuffer(n, &data))) break;
            float* o = (float*)data;
            auto f = std::atomic_load(&fx);
            const std::vector<float>& r = *raw;
            size_t p = pos;
            const float target = (useFx && f) ? 1.0f : 0.0f;
            for (UINT32 i = 0; i < n; ++i) {
                gain += (target - gain) * 0.005f;   // ~10 ms cross-fade when switching
                if (p < r.size()) {
                    const float a = r[p], b = f && p < f->size() ? (*f)[p] : a;
                    o[i] = a + gain * (b - a);
                    ++p;
                } else {
                    o[i] = 0.0f;
                }
            }
            pos = p;
            rc->ReleaseBuffer(n, 0);
            if (p >= r.size()) break;
        }
        if (ac) ac->Stop();
        CloseHandle(ev);
        playing = false;
        PostMessageW(g_hwnd, WM_RECDONE, 0, 0);
        CoUninitialize();
    }
    std::wstring json() const {
        std::wstring st = recording ? L"recording" : (playing ? L"playing" : (raw ? L"ready" : L"idle"));
        double dur = raw ? (double)raw->size() / rate : (double)want / std::max(1, rate);
        double p = recording && g_link.b ? (double)(g_link.b->recordWritten.load() - start) / rate : (double)pos / rate;
        std::wstring w = L"[";
        if (raw) {   // 56 bars of peak level for the waveform
            const size_t step = raw->size() / 56 + 1;
            for (size_t i = 0; i < raw->size(); i += step) {
                float pk = 0;
                for (size_t j = i; j < std::min(raw->size(), i + step); ++j) pk = std::max(pk, std::fabs((*raw)[j]));
                w += (i ? L"," : L"") + num(std::min(1.0, std::sqrt(pk * 4.0)));
            }
        }
        w += L"]";
        return L"{\"type\":\"rec\",\"state\":" + jsonStr(st) + L",\"pos\":" + num(p) + L",\"dur\":" + num(dur) +
               L",\"wave\":" + w + L"}";
    }
} g_rec;

// ----------------------------------------------------------------------------- UI messages
void sendState() {
    std::wstring j = L"{\"type\":\"state\",\"user\":{";
    bool first = true;
    for (auto& kv : g_set.all()) {
        j += (first ? L"" : L",") + jsonStr(std::wstring(kv.first.begin(), kv.first.end())) + L":" + num(kv.second);
        first = false;
    }
    post(j + L"}}");
}
std::vector<Mic> g_mics;
void sendMics() {
    std::wstring def;
    g_mics = listMics(&def);
    bool found = false;
    for (auto& m : g_mics) found |= m.id == g_set.mic;
    if (!found) {   // prefer an installed default mic, then any installed one
        g_set.mic = def;
        for (auto& m : g_mics) if (m.id == def && m.installed) found = true;
        if (!found) for (auto& m : g_mics) if (m.installed) { g_set.mic = m.id; break; }
    }
    std::wstring j = L"{\"type\":\"mics\",\"current\":" + jsonStr(g_set.mic) + L",\"list\":[";
    for (size_t i = 0; i < g_mics.size(); ++i)
        j += (i ? L"," : L"") + std::wstring(L"{\"id\":") + jsonStr(g_mics[i].id) + L",\"name\":" + jsonStr(g_mics[i].name) +
             L",\"installed\":" + (g_mics[i].installed ? L"true" : L"false") + L"}";
    post(j + L"]}");
}
const Mic* currentMic() {
    for (auto& m : g_mics) if (m.id == g_set.mic) return &m;
    return nullptr;
}

void sendMeters() {
    const Mic* m = currentMic();
    const bool linked = m && m->installed && g_link.open(m->guid);
    const bool running = linked && g_link.running();
    if (linked) seqRead(g_link.b->analysisSeq, g_link.b->analysis, g_an);
    if (linked && g_link.fresh) { g_link.fresh = false; g_resValid = false; pushParams(); }
    std::wstring raw = L"[", voice = L"[";
    for (int b = 0; b < kBands; ++b) {
        raw += (b ? L"," : L"") + num(g_an.rawLtasDb[b]);
        voice += (b ? L"," : L"") + num(g_an.ltasDb[b]);
    }
    raw += L"]"; voice += L"]";
    std::wstring eq = L"[";
    for (int k = 0; k < kEqBank; ++k) {
        int t; float hz, q;
        eqBankBand(k, &t, &hz, &q);
        eq += (k ? L"," : L"") + std::wstring(L"{\"t\":\"") + (t == 0 ? L"ls" : t == 2 ? L"hs" : L"pk") + L"\",\"hz\":" + num(hz) +
              L",\"q\":" + num(q) + L",\"g\":" + num(g_res.autoEqOn ? g_info.autoEqGainDb[k] : 0) + L"}";
    }
    eq += L"]";
    const double lat = 1000.0 * (Pipeline::kAiDelay + 15 + Dynamics::lookahead() + kHop) / kFs;
    std::wstring j = L"{\"type\":\"meters\",\"installed\":" + std::to_wstring(m && m->installed) +
                     L",\"running\":" + std::to_wstring(running) + L",\"mon\":" + std::to_wstring(g_mon.state.load()) +
                     L",\"settled\":" + std::to_wstring(g_mon.settled()) + L",\"repairing\":" + std::to_wstring(g_repairing) +
                     L",\"in\":" + num(g_an.inPeakDb) +
                     L",\"out\":" + num(g_an.outLevelDb) + L",\"ai\":" + std::to_wstring(g_an.aiActive) +
                     L",\"problem\":" + std::to_wstring(running ? g_an.aiProblem : 0) + L",\"load\":" + num(g_an.load) +
                     L",\"rate\":" + std::to_wstring(g_an.inputRate) + L",\"cutoff\":" + num(g_info.detectedCutoffHz) +
                     L",\"calibrated\":" + std::to_wstring(g_info.calibrated) + L",\"frames\":" + std::to_wstring(g_an.speechFrames) +
                     L",\"target\":" + num(g_res.levelTargetDb) + L",\"gr\":" + num(g_an.gainReductionDb) +
                     L",\"latency\":" + num(lat) + L",\"raw\":" + raw + L",\"voice\":" + voice + L",\"autoEq\":" + eq + L"}";
    post(j);
}

// "自动修复" / "装到这个麦克风": run wvinstall as administrator in a console window. It tries
// the effect slots one by one and checks each with real audio.
HANDLE g_repairProc = nullptr;
void startRepair() {
    const Mic* m = currentMic();
    if (!m || g_repairing) return;
    g_mon.stop();   // the audio service restarts during the repair
    const std::wstring exe = g_exeDir + L"wvinstall.exe";
    const std::wstring args = L"install \"" + m->id + L"\" --slot auto --pause";
    SHELLEXECUTEINFOW sei{sizeof(sei)};
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.hwnd = g_hwnd;
    sei.lpVerb = L"runas";
    sei.lpFile = exe.c_str();
    sei.lpParameters = args.c_str();
    sei.nShow = SW_SHOWNORMAL;
    if (ShellExecuteExW(&sei) && sei.hProcess) {
        g_repairProc = sei.hProcess;
        g_repairing = true;
    }
}
void pollRepair() {
    if (!g_repairing || !g_repairProc) return;
    if (WaitForSingleObject(g_repairProc, 0) != WAIT_OBJECT_0) return;
    CloseHandle(g_repairProc);
    g_repairProc = nullptr;
    g_repairing = false;
    g_link.close();
    g_link.guid.clear();
    g_resValid = false;
    sendMics();
}

void onMessage(const std::wstring& wmsg) {
    const std::string msg = toUtf8(wmsg);
    std::vector<std::string> p;
    std::stringstream ss(msg);
    std::string part;
    while (std::getline(ss, part, '|')) p.push_back(part);
    if (p.empty()) return;
    if (p[0] == "ready") { sendState(); sendMics(); post(g_rec.json()); return; }
    if (p[0] == "set" && p.size() >= 3) {
        if (g_set.set(p[1], std::atof(p[2].c_str()))) {
            g_set.save();
            pushParams();
            if (g_rec.raw) g_rec.reprocess();   // hear the change in the A/B clip right away
        }
        return;
    }
    if (p[0] == "reset") { g_set.u = UserParams(); g_set.save(); pushParams(); sendState(); return; }
    if (p[0] == "eq" && p.size() >= 2 && p[1] == "flat") {
        for (auto& n : g_set.u.eq) n.gainDb = 0;
        g_set.save(); pushParams(); sendState(); return;
    }
    if (p[0] == "repair") { startRepair(); return; }
    if (p[0] == "profile" && g_link.b) { g_link.b->resetProfile.fetch_add(1); return; }
    if (p[0] == "mic" && p.size() >= 3 && p[1] == "select") {
        const int n = MultiByteToWideChar(CP_UTF8, 0, p[2].c_str(), (int)p[2].size(), nullptr, 0);
        std::wstring id(n, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, p[2].c_str(), (int)p[2].size(), &id[0], n);
        g_set.mic = id;
        g_set.save();
        g_link.close();
        g_resValid = false;
        sendMics();
        return;
    }
    if (p[0] == "rec" && p.size() >= 2) {
        if (p[1] == "start") g_rec.startRecording();
        else if (p[1] == "play") g_rec.play();
        else if (p[1] == "stop") g_rec.stop();
        else if (p[1] == "ab" && p.size() >= 3) g_rec.useFx = p[2] == "fx";
        post(g_rec.json());
        return;
    }
    if (p[0] == "ui" && p.size() >= 3 && p[1] == "detail") {
        const bool detail = p[2] == "1";
        if (IsZoomed(g_hwnd)) return;   // maximized: the page lays itself out
        const UINT dpi = GetDpiForWindow(g_hwnd);
        MONITORINFO mi{sizeof(mi)};
        GetMonitorInfoW(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST), &mi);
        const RECT wa = mi.rcWork;
        const int w = std::min<int>(MulDiv(detail ? 1200 : 460, dpi, 96), (wa.right - wa.left) * 92 / 100);
        const int h = std::min<int>(MulDiv(detail ? 860 : 820, dpi, 96), (wa.bottom - wa.top) * 92 / 100);
        RECT cur;
        GetWindowRect(g_hwnd, &cur);
        const int x = std::max<int>(wa.left, std::min<int>(cur.left, wa.right - w));
        const int y = std::max<int>(wa.top, std::min<int>(cur.top, wa.bottom - h));
        SetWindowPos(g_hwnd, nullptr, x, y, w, h, SWP_NOZORDER);
    }
}

// ----------------------------------------------------------------------------- window
NOTIFYICONDATAW g_tray{};
void addTray(HWND h) {
    g_tray.cbSize = sizeof(g_tray);
    g_tray.hWnd = h;
    g_tray.uID = 1;
    g_tray.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_tray.uCallbackMessage = WM_TRAY;
    g_tray.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(g_tray.szTip, L"暖声 WarmVoice");
    Shell_NotifyIconW(NIM_ADD, &g_tray);
}

void createWebView(HWND h) {
    const std::wstring data = appDataDir() + L"\\WebView2";
    CreateCoreWebView2EnvironmentWithOptions(nullptr, data.c_str(), nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [h](HRESULT hr, ICoreWebView2Environment* env) -> HRESULT {
                if (FAILED(hr) || !env) {
                    MessageBoxW(h, L"需要 Microsoft Edge WebView2 运行库（Windows 10/11 一般自带）。", L"暖声", MB_ICONERROR);
                    return hr;
                }
                env->CreateCoreWebView2Controller(h, Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                    [h](HRESULT hr2, ICoreWebView2Controller* c) -> HRESULT {
                        if (FAILED(hr2) || !c) return hr2;
                        g_controller = c;
                        c->get_CoreWebView2(&g_webview);
                        ComPtr<ICoreWebView2Controller2> c2;
                        if (SUCCEEDED(g_controller.As(&c2))) c2->put_DefaultBackgroundColor({255, 14, 16, 18});
                        RECT r;
                        GetClientRect(h, &r);
                        c->put_Bounds(r);
                        // Desktop-sized by default (the page is drawn for touch-sized controls).
                        c->put_ZoomFactor(g_set.zoom);
                        c->add_ZoomFactorChanged(Callback<ICoreWebView2ZoomFactorChangedEventHandler>(
                            [](ICoreWebView2Controller* cc, IUnknown*) -> HRESULT {
                                double z = 1;
                                if (SUCCEEDED(cc->get_ZoomFactor(&z)) && std::fabs(z - g_set.zoom) > 0.01) {
                                    g_set.zoom = z;
                                    g_set.save();
                                }
                                return S_OK;
                            }).Get(), nullptr);
                        ComPtr<ICoreWebView2Settings> s;
                        if (SUCCEEDED(g_webview->get_Settings(&s))) {
                            s->put_AreDefaultContextMenusEnabled(FALSE);
                            s->put_IsStatusBarEnabled(FALSE);
                        }
                        ComPtr<ICoreWebView2_3> w3;
                        if (SUCCEEDED(g_webview.As(&w3)))
                            w3->SetVirtualHostNameToFolderMapping(L"warmvoice.local", (g_exeDir + L"ui").c_str(),
                                                                  COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY_CORS);
                        g_webview->add_WebMessageReceived(Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                            [](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* a) -> HRESULT {
                                LPWSTR s = nullptr;
                                if (SUCCEEDED(a->TryGetWebMessageAsString(&s)) && s) { onMessage(s); CoTaskMemFree(s); }
                                return S_OK;
                            }).Get(), nullptr);
                        g_webview->Navigate(L"https://warmvoice.local/index.html");
                        return S_OK;
                    }).Get());
                return S_OK;
            }).Get());
}

LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_SIZE:
            if (g_controller) { RECT r; GetClientRect(h, &r); g_controller->put_Bounds(r); }
            return 0;
        case WM_GETMINMAXINFO: {
            const UINT dpi = GetDpiForWindow(h) ? GetDpiForWindow(h) : 96;
            auto* mm = (MINMAXINFO*)l;
            mm->ptMinTrackSize.x = MulDiv(380, dpi, 96);
            mm->ptMinTrackSize.y = MulDiv(560, dpi, 96);
            return 0;
        }
        case WM_DPICHANGED: {
            const RECT* r = (const RECT*)l;
            SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        case WM_TIMER: {
            static int tick = 0;
            pollRepair();
            {
                const Mic* m = currentMic();
                if (IsWindowVisible(h) && !IsIconic(h) && m && !g_repairing) g_mon.start(m->id);
                else g_mon.stop();
            }
            g_rec.poll();
            sendMeters();
            if (++tick % 20 == 0) { pushParams(); if (tick % 100 == 0) sendMics(); }   // profile updates, new mics
            if (g_rec.recording || g_rec.playing) post(g_rec.json());
            return 0;
        }
        case WM_RECDONE:
            post(g_rec.json());
            return 0;
        case WM_TRAY:
            if (LOWORD(l) == WM_LBUTTONUP) { ShowWindow(h, SW_SHOW); SetForegroundWindow(h); }
            else if (LOWORD(l) == WM_RBUTTONUP) {
                HMENU menu = CreatePopupMenu();
                AppendMenuW(menu, MF_STRING, 1, L"打开暖声");
                AppendMenuW(menu, MF_STRING, 2, L"退出（声音处理继续工作）");
                POINT pt;
                GetCursorPos(&pt);
                SetForegroundWindow(h);
                const int cmd = TrackPopupMenu(menu, TPM_RETURNCMD, pt.x, pt.y, 0, h, nullptr);
                DestroyMenu(menu);
                if (cmd == 1) { ShowWindow(h, SW_SHOW); SetForegroundWindow(h); }
                if (cmd == 2) DestroyWindow(h);
            }
            return 0;
        case WM_CLOSE:
            ShowWindow(h, SW_HIDE);   // keep running in the tray
            return 0;
        case WM_DESTROY:
            Shell_NotifyIconW(NIM_DELETE, &g_tray);
            g_mon.stop();
            g_rec.stop();
            if (g_rec.worker.joinable()) g_rec.worker.join();   // a joinable thread at exit would abort
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
    HANDLE single = CreateMutexW(nullptr, TRUE, L"WarmVoiceAppSingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND other = FindWindowW(L"WarmVoiceWnd", nullptr);
        if (other) { ShowWindow(other, SW_SHOW); SetForegroundWindow(other); }
        return 0;
    }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    g_exeDir = exeDir();
    g_set.load();

    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.lpszClassName = L"WarmVoiceWnd";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(RGB(14, 16, 18));
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    RegisterClassW(&wc);
    const UINT dpi = GetDpiForSystem();
    RECT wa{0, 0, 1280, 720};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    // Small window, centred, never larger than the screen.
    const int ww = std::min<int>(MulDiv(460, dpi, 96), (wa.right - wa.left) * 92 / 100);
    const int wh = std::min<int>(MulDiv(820, dpi, 96), (wa.bottom - wa.top) * 92 / 100);
    const int wx = wa.left + (wa.right - wa.left - ww) / 2, wy = wa.top + (wa.bottom - wa.top - wh) / 2;
    g_hwnd = CreateWindowExW(0, wc.lpszClassName, L"暖声 WarmVoice", WS_OVERLAPPEDWINDOW, wx, wy, ww, wh, nullptr, nullptr,
                             inst, nullptr);
    addTray(g_hwnd);
    createWebView(g_hwnd);
    ShowWindow(g_hwnd, show);
    SetTimer(g_hwnd, kTimer, 100, nullptr);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    g_link.close();
    CoUninitialize();
    if (single) CloseHandle(single);
    return 0;
}
