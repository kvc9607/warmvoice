// wvinstall: put the WarmVoice APO on a microphone (run as administrator).
//
//   wvinstall list                          microphones, their format and WarmVoice status
//   wvinstall install <n|id|default> [--slot auto|efx|mfx|sfx]
//        auto (the default) tries the slots one by one - endpoint (EFX), mode (MFX), stream
//        (SFX) - and keeps the first one Windows really runs on this microphone. Each try
//        opens the mic for a few seconds and checks the APO is processing the sound.
//   wvinstall uninstall <n|id|default|all>  restore what was there before
//   wvinstall status <n|id|default>         opens the mic briefly and checks the APO runs
//   wvinstall diagnose <n|id|default>       writes everything to %ProgramData%\WarmVoice\report.txt
//   --pause                                 wait for Enter at the end (when started from the app)
//
// The previous registry values are backed up under HKLM\SOFTWARE\WarmVoice\Backup\<endpoint>,
// so uninstall puts back exactly what the driver had (including other vendors' effects).
// Everything printed also goes to %ProgramData%\WarmVoice\report.txt.
// SPDX-License-Identifier: MIT
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <initguid.h>   // instantiate the SDK GUID / PROPERTYKEY constants used below
#include <aclapi.h>
#include <audioclient.h>
#include <audioenginebaseapo.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mmdeviceapi.h>
#include <propvarutil.h>
#include <sddl.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cwchar>
#include <fcntl.h>
#include <io.h>
#include <string>
#include <vector>

#include "guids.h"
#include "wv/shared.h"
#include "wv/winlink.h"

static const wchar_t* kMMDevicesCapture = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\Capture\\";
static const wchar_t* kBackupRoot = L"SOFTWARE\\WarmVoice\\Backup\\";
// AUDIO_SIGNALPROCESSINGMODE_DEFAULT
static const wchar_t* kModeDefault = L"{C18E2F7E-933D-4965-B7D1-1EEF228D2AF3}";
// Windows 10 1803+ "composite" effect lists (several APOs per slot). When a driver has these,
// Windows uses them instead of the single-APO values, so we must be in them too.
static const wchar_t* kCompositeSfx = L"{D04E05A6-594B-4FB6-A80D-01AF5EED7D1D},13";
static const wchar_t* kCompositeMfx = L"{D04E05A6-594B-4FB6-A80D-01AF5EED7D1D},14";
static const wchar_t* kCompositeEfx = L"{D04E05A6-594B-4FB6-A80D-01AF5EED7D1D},15";

// ---------------------------------------------------------------- output (console + report)
static FILE* g_report = nullptr;
static void say(const wchar_t* fmt, ...) {
    wchar_t buf[4096];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(buf, _countof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    std::fputws(buf, stdout);
    if (g_report) {
        const int n = WideCharToMultiByte(CP_UTF8, 0, buf, -1, nullptr, 0, nullptr, nullptr);
        std::string u(n > 0 ? n - 1 : 0, '\0');
        if (n > 1) WideCharToMultiByte(CP_UTF8, 0, buf, -1, &u[0], n, nullptr, nullptr);
        std::fwrite(u.data(), 1, u.size(), g_report);
        std::fflush(g_report);
    }
}

// Technical details: only into report.txt (the console shows plain Chinese messages).
static void note(const wchar_t* fmt, ...) {
    if (!g_report) return;
    wchar_t buf[4096];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(buf, _countof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    const int n = WideCharToMultiByte(CP_UTF8, 0, buf, -1, nullptr, 0, nullptr, nullptr);
    std::string u(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, buf, -1, &u[0], n, nullptr, nullptr);
    std::fwrite(u.data(), 1, u.size(), g_report);
    std::fflush(g_report);
}

struct Endpoint {
    std::wstring id, guid, name;
    DWORD rate = 0, channels = 0;
};

static std::wstring pkeyName(const PROPERTYKEY& k) {
    wchar_t g[64];
    StringFromGUID2(k.fmtid, g, 64);
    return std::wstring(g) + L"," + std::to_wstring(k.pid);
}

// The data folder: the audio engine (LOCAL SERVICE) writes its log there, users read it.
static void prepareDataDir() {
    const std::wstring d = wv::dataDir();
    CreateDirectoryW(d.c_str(), nullptr);
    PSECURITY_DESCRIPTOR sd = nullptr;
    // inherit to files: SYSTEM + admins full, LOCAL SERVICE + users modify
    if (ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:PAI(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;0x1301bf;;;LS)(A;OICI;0x1301bf;;;BU)", SDDL_REVISION_1, &sd, nullptr)) {
        BOOL present = FALSE, def = FALSE;
        PACL dacl = nullptr;
        if (GetSecurityDescriptorDacl(sd, &present, &dacl, &def) && present)
            SetNamedSecurityInfoW(const_cast<wchar_t*>(d.c_str()), SE_FILE_OBJECT,
                                  DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr, nullptr, dacl, nullptr);
        LocalFree(sd);
    }
}

static std::vector<Endpoint> listEndpoints() {
    std::vector<Endpoint> out;
    IMMDeviceEnumerator* en = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en)))) return out;
    IMMDeviceCollection* col = nullptr;
    if (SUCCEEDED(en->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &col))) {
        UINT n = 0;
        col->GetCount(&n);
        for (UINT i = 0; i < n; ++i) {
            IMMDevice* dev = nullptr;
            if (FAILED(col->Item(i, &dev))) continue;
            Endpoint e;
            LPWSTR id = nullptr;
            if (SUCCEEDED(dev->GetId(&id))) { e.id = id; CoTaskMemFree(id); }
            const size_t dot = e.id.find_last_of(L'.');
            e.guid = dot == std::wstring::npos ? e.id : e.id.substr(dot + 1);
            for (auto& ch : e.guid) ch = (wchar_t)towlower(ch);   // same spelling as the APO uses
            IPropertyStore* ps = nullptr;
            if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &ps))) {
                PROPVARIANT v;
                PropVariantInit(&v);
                if (SUCCEEDED(ps->GetValue(PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR) e.name = v.pwszVal;
                PropVariantClear(&v);
                if (SUCCEEDED(ps->GetValue(PKEY_AudioEngine_DeviceFormat, &v)) && v.vt == VT_BLOB &&
                    v.blob.cbSize >= sizeof(WAVEFORMATEX)) {
                    auto* wf = (WAVEFORMATEX*)v.blob.pBlobData;
                    e.rate = wf->nSamplesPerSec;
                    e.channels = wf->nChannels;
                }
                PropVariantClear(&v);
                ps->Release();
            }
            dev->Release();
            out.push_back(e);
        }
        col->Release();
    }
    en->Release();
    return out;
}

static bool enablePrivilege(const wchar_t* name) {
    HANDLE tok;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tok)) return false;
    TOKEN_PRIVILEGES tp{};
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    bool ok = LookupPrivilegeValueW(nullptr, name, &tp.Privileges[0].Luid) &&
              AdjustTokenPrivileges(tok, FALSE, &tp, 0, nullptr, nullptr) && GetLastError() == ERROR_SUCCESS;
    CloseHandle(tok);
    return ok;
}

// The FxProperties key belongs to the audio service; give administrators write access to it
// (the same thing other APO installers do). Only this one key is touched.
static bool grantAdminWrite(const std::wstring& subkey) {
    enablePrivilege(SE_TAKE_OWNERSHIP_NAME);
    enablePrivilege(SE_RESTORE_NAME);
    std::wstring obj = L"MACHINE\\" + subkey;
    PSID admins = nullptr;
    SID_IDENTIFIER_AUTHORITY nt = SECURITY_NT_AUTHORITY;
    if (!AllocateAndInitializeSid(&nt, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &admins))
        return false;
    DWORD r = SetNamedSecurityInfoW(&obj[0], SE_REGISTRY_KEY, OWNER_SECURITY_INFORMATION, admins, nullptr, nullptr, nullptr);
    PACL oldDacl = nullptr, newDacl = nullptr;
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (r == ERROR_SUCCESS)
        r = GetNamedSecurityInfoW(obj.c_str(), SE_REGISTRY_KEY, DACL_SECURITY_INFORMATION, nullptr, nullptr, &oldDacl, nullptr, &sd);
    if (r == ERROR_SUCCESS) {
        EXPLICIT_ACCESSW ea{};
        ea.grfAccessPermissions = KEY_ALL_ACCESS;
        ea.grfAccessMode = GRANT_ACCESS;
        ea.grfInheritance = CONTAINER_INHERIT_ACE;
        ea.Trustee.TrusteeForm = TRUSTEE_IS_SID;
        ea.Trustee.TrusteeType = TRUSTEE_IS_GROUP;
        ea.Trustee.ptstrName = (LPWSTR)admins;
        r = SetEntriesInAclW(1, &ea, oldDacl, &newDacl);
    }
    if (r == ERROR_SUCCESS)
        r = SetNamedSecurityInfoW(&obj[0], SE_REGISTRY_KEY, DACL_SECURITY_INFORMATION, nullptr, nullptr, newDacl, nullptr);
    if (newDacl) LocalFree(newDacl);
    if (sd) LocalFree(sd);
    FreeSid(admins);
    return r == ERROR_SUCCESS;
}

static HKEY openFx(const Endpoint& e, bool write) {
    const std::wstring base = std::wstring(kMMDevicesCapture) + e.guid;
    const std::wstring fx = base + L"\\FxProperties";
    HKEY k = nullptr;
    const REGSAM sam = write ? (KEY_READ | KEY_WRITE) : KEY_READ;
    LONG r = RegOpenKeyExW(HKEY_LOCAL_MACHINE, fx.c_str(), 0, sam, &k);
    if (r == ERROR_FILE_NOT_FOUND && write) {
        grantAdminWrite(base);
        r = RegCreateKeyExW(HKEY_LOCAL_MACHINE, fx.c_str(), 0, nullptr, 0, sam, nullptr, &k, nullptr);
    } else if (r == ERROR_ACCESS_DENIED && write && grantAdminWrite(fx)) {
        r = RegOpenKeyExW(HKEY_LOCAL_MACHINE, fx.c_str(), 0, sam, &k);
    }
    return r == ERROR_SUCCESS ? k : nullptr;
}

// Copy one value (any type) from src to dst; if it is missing in src, record that with a
// marker so uninstall can delete it again.
static void backupValue(HKEY src, HKEY dst, const std::wstring& name) {
    DWORD type = 0, size = 0;
    if (RegQueryValueExW(src, name.c_str(), nullptr, &type, nullptr, &size) != ERROR_SUCCESS) {
        const DWORD missing = 1;
        RegSetValueExW(dst, (L"missing:" + name).c_str(), 0, REG_DWORD, (const BYTE*)&missing, sizeof(missing));
        return;
    }
    std::vector<BYTE> buf(size);
    if (RegQueryValueExW(src, name.c_str(), nullptr, &type, buf.data(), &size) == ERROR_SUCCESS)
        RegSetValueExW(dst, name.c_str(), 0, type, buf.data(), size);
}

static bool waitState(SC_HANDLE svc, DWORD want, int ms) {
    SERVICE_STATUS st{};
    for (int t = 0; t < ms; t += 100) {
        if (QueryServiceStatus(svc, &st) && st.dwCurrentState == want) return true;
        Sleep(100);
    }
    return false;
}

// Restart Audiosrv so it re-reads the effect settings. Services that depend on it (some
// vendor audio services) are stopped first and started again afterwards.
static bool restartAudioService() {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return false;
    SC_HANDLE svc = OpenServiceW(scm, L"Audiosrv", SERVICE_STOP | SERVICE_START | SERVICE_QUERY_STATUS |
                                                       SERVICE_ENUMERATE_DEPENDENTS);
    bool ok = false;
    std::vector<std::wstring> restartLater;
    if (svc) {
        DWORD need = 0, count = 0;
        EnumDependentServicesW(svc, SERVICE_ACTIVE, nullptr, 0, &need, &count);
        if (need) {
            std::vector<BYTE> buf(need);
            auto* deps = (ENUM_SERVICE_STATUSW*)buf.data();
            if (EnumDependentServicesW(svc, SERVICE_ACTIVE, deps, need, &need, &count)) {
                for (DWORD i = 0; i < count; ++i) {
                    SC_HANDLE d = OpenServiceW(scm, deps[i].lpServiceName, SERVICE_STOP | SERVICE_QUERY_STATUS);
                    if (!d) continue;
                    SERVICE_STATUS st{};
                    if (ControlService(d, SERVICE_CONTROL_STOP, &st)) restartLater.push_back(deps[i].lpServiceName);
                    waitState(d, SERVICE_STOPPED, 5000);
                    CloseServiceHandle(d);
                }
            }
        }
        SERVICE_STATUS st{};
        ControlService(svc, SERVICE_CONTROL_STOP, &st);
        const bool stopped = waitState(svc, SERVICE_STOPPED, 10000);
        ok = stopped && StartServiceW(svc, 0, nullptr) != 0 && waitState(svc, SERVICE_RUNNING, 10000);
        if (!stopped) note(L"  audio service did not stop\n");
        CloseServiceHandle(svc);
        for (auto& n : restartLater) {
            SC_HANDLE d = OpenServiceW(scm, n.c_str(), SERVICE_START);
            if (d) { StartServiceW(d, 0, nullptr); CloseServiceHandle(d); }
        }
    }
    CloseServiceHandle(scm);
    return ok;
}

static std::vector<std::wstring> managedValues() {
    return {pkeyName(PKEY_FX_StreamEffectClsid),
            pkeyName(PKEY_FX_ModeEffectClsid),
            pkeyName(PKEY_FX_EndpointEffectClsid),
            kCompositeSfx,
            kCompositeMfx,
            kCompositeEfx,
            pkeyName(PKEY_SFX_ProcessingModes_Supported_For_Streaming),
            pkeyName(PKEY_MFX_ProcessingModes_Supported_For_Streaming),
            pkeyName(PKEY_EFX_ProcessingModes_Supported_For_Streaming),
            pkeyName(PKEY_AudioEndpoint_Disable_SysFx)};
}

static bool registerDll() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir(exe);
    dir = dir.substr(0, dir.find_last_of(L"\\/") + 1);
    HMODULE m = LoadLibraryW((dir + L"WarmVoiceApo.dll").c_str());
    if (!m) { say(L"找不到 WarmVoiceApo.dll，请把压缩包完整解压后再运行 install.bat\n"); return false; }
    auto reg = (HRESULT(STDAPICALLTYPE*)())GetProcAddress(m, "DllRegisterServer");
    const bool ok = reg && SUCCEEDED(reg());
    FreeLibrary(m);
    return ok;
}

static std::wstring backupPath(const Endpoint& e) { return std::wstring(kBackupRoot) + e.guid; }
static bool isInstalled(const Endpoint& e) {
    HKEY bk = nullptr;
    const bool r = RegOpenKeyExW(HKEY_LOCAL_MACHINE, backupPath(e).c_str(), 0, KEY_READ, &bk) == ERROR_SUCCESS;
    if (bk) RegCloseKey(bk);
    return r;
}

// Put every managed value back to what the backup says (the driver's original state).
static void restoreValues(HKEY fx, HKEY bk) {
    for (auto& n : managedValues()) {
        DWORD type = 0, size = 0;
        if (RegQueryValueExW(bk, (L"missing:" + n).c_str(), nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS) {
            RegDeleteValueW(fx, n.c_str());
        } else if (RegQueryValueExW(bk, n.c_str(), nullptr, &type, nullptr, &size) == ERROR_SUCCESS) {
            std::vector<BYTE> buf(size);
            RegQueryValueExW(bk, n.c_str(), nullptr, &type, buf.data(), &size);
            RegSetValueExW(fx, n.c_str(), 0, type, buf.data(), size);
        }
    }
}

static std::vector<std::wstring> readMultiSz(HKEY k, const std::wstring& name, bool* exists) {
    std::vector<std::wstring> out;
    DWORD type = 0, size = 0;
    *exists = RegQueryValueExW(k, name.c_str(), nullptr, &type, nullptr, &size) == ERROR_SUCCESS;
    if (!*exists || size == 0) return out;
    std::vector<wchar_t> buf(size / sizeof(wchar_t) + 2, 0);
    RegQueryValueExW(k, name.c_str(), nullptr, &type, (BYTE*)buf.data(), &size);
    if (type == REG_SZ || type == REG_EXPAND_SZ) { out.emplace_back(buf.data()); return out; }
    for (const wchar_t* p = buf.data(); *p; p += wcslen(p) + 1) out.emplace_back(p);
    return out;
}
static void writeMultiSz(HKEY k, const std::wstring& name, const std::vector<std::wstring>& v) {
    std::wstring s;
    for (auto& x : v) { s += x; s += L'\0'; }
    s += L'\0';
    RegSetValueExW(k, name.c_str(), 0, REG_MULTI_SZ, (const BYTE*)s.c_str(), (DWORD)(s.size() * sizeof(wchar_t)));
}

// Install into one slot. Any earlier WarmVoice slot is undone first, so the sound is never
// processed twice.
static bool applySlot(const Endpoint& e, const std::wstring& slot) {
    HKEY fx = openFx(e, true);
    if (!fx) { say(L"改不了“%ls”的系统设置（需要管理员权限）\n", e.name.c_str()); return false; }
    HKEY bk = nullptr;
    if (!isInstalled(e)) {   // first install: save the driver's values (never overwritten later)
        RegCreateKeyExW(HKEY_LOCAL_MACHINE, backupPath(e).c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &bk, nullptr);
        if (bk) { for (auto& n : managedValues()) backupValue(fx, bk, n); RegCloseKey(bk); bk = nullptr; }
    }
    // Older versions did not back up the composite lists: add them now if missing.
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, backupPath(e).c_str(), 0, KEY_READ | KEY_WRITE, &bk) == ERROR_SUCCESS) {
        for (const wchar_t* c : {kCompositeSfx, kCompositeMfx, kCompositeEfx}) {
            if (RegQueryValueExW(bk, c, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS &&
                RegQueryValueExW(bk, (std::wstring(L"missing:") + c).c_str(), nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
                backupValue(fx, bk, c);
        }
        restoreValues(fx, bk);
        RegCloseKey(bk);
    }
    const PROPERTYKEY& clsidKey = slot == L"sfx" ? PKEY_FX_StreamEffectClsid
                                : slot == L"mfx" ? PKEY_FX_ModeEffectClsid : PKEY_FX_EndpointEffectClsid;
    const PROPERTYKEY& modesKey = slot == L"sfx" ? PKEY_SFX_ProcessingModes_Supported_For_Streaming
                                : slot == L"mfx" ? PKEY_MFX_ProcessingModes_Supported_For_Streaming
                                                 : PKEY_EFX_ProcessingModes_Supported_For_Streaming;
    const wchar_t* composite = slot == L"sfx" ? kCompositeSfx : slot == L"mfx" ? kCompositeMfx : kCompositeEfx;
    const std::wstring clsid = WV_CLSID_STRING;

    wchar_t old[64] = {};
    DWORD sz = sizeof(old) - sizeof(wchar_t), type = 0;
    if (RegQueryValueExW(fx, pkeyName(clsidKey).c_str(), nullptr, &type, (BYTE*)old, &sz) == ERROR_SUCCESS && old[0] &&
        _wcsicmp(old, WV_CLSID_STRING) != 0)
        note(L"  replacing existing effect %ls (backed up)\n", old);
    RegSetValueExW(fx, pkeyName(clsidKey).c_str(), 0, REG_SZ, (const BYTE*)clsid.c_str(), (DWORD)((clsid.size() + 1) * 2));

    // Composite list present: Windows reads that one. Keep the driver's effects and add ours last.
    bool hasComposite = false;
    auto list = readMultiSz(fx, composite, &hasComposite);
    if (hasComposite) {
        bool there = false;
        for (auto& x : list) there |= _wcsicmp(x.c_str(), WV_CLSID_STRING) == 0;
        if (!there) list.push_back(clsid);
        writeMultiSz(fx, composite, list);
        note(L"  composite list: added after %zu existing effect(s)\n", list.size() - 1);
    }
    // Processing modes: keep whatever the driver supports, make sure "default" is there.
    bool hasModes = false;
    auto modes = readMultiSz(fx, pkeyName(modesKey), &hasModes);
    bool haveDefault = false;
    for (auto& x : modes) haveDefault |= _wcsicmp(x.c_str(), kModeDefault) == 0;
    if (!haveDefault) modes.push_back(kModeDefault);
    writeMultiSz(fx, pkeyName(modesKey), modes);
    const DWORD enabled = 0;   // ENDPOINT_SYSFX_ENABLED: Windows "audio enhancements" on
    RegSetValueExW(fx, pkeyName(PKEY_AudioEndpoint_Disable_SysFx).c_str(), 0, REG_DWORD, (const BYTE*)&enabled, sizeof(enabled));
    RegCloseKey(fx);
    return true;
}

// ---------------------------------------------------------------- live check
struct Probe {
    enum State { Running, NotRunning, Busy, OpenFailed } state = OpenFailed;
    HRESULT hr = S_OK;
    bool linkFound = false, viaFile = false;
    wv::Analysis a;
};

static bool waitForEndpoint(const std::wstring& id, int ms) {
    for (int t = 0; t < ms; t += 250) {
        IMMDeviceEnumerator* en = nullptr;
        bool ok = false;
        if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en)))) {
            IMMDevice* d = nullptr;
            DWORD st = 0;
            if (SUCCEEDED(en->GetDevice(id.c_str(), &d))) { ok = SUCCEEDED(d->GetState(&st)) && st == DEVICE_STATE_ACTIVE; d->Release(); }
            en->Release();
        }
        if (ok) return true;
        Sleep(250);
    }
    return false;
}

// Open the microphone like any app would (shared mode) and see whether the APO processes it.
static Probe probe(const Endpoint& e, int ms) {
    Probe p;
    IMMDeviceEnumerator* en = nullptr;
    IMMDevice* dev = nullptr;
    IAudioClient* ac = nullptr;
    IAudioCaptureClient* cc = nullptr;
    WAVEFORMATEX* mix = nullptr;
    p.hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en));
    if (SUCCEEDED(p.hr)) p.hr = en->GetDevice(e.id.c_str(), &dev);
    if (SUCCEEDED(p.hr)) p.hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&ac);
    if (SUCCEEDED(p.hr)) p.hr = ac->GetMixFormat(&mix);
    if (SUCCEEDED(p.hr)) p.hr = ac->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 2000000, 0, mix, nullptr);
    if (SUCCEEDED(p.hr)) p.hr = ac->GetService(IID_PPV_ARGS(&cc));
    if (SUCCEEDED(p.hr)) p.hr = ac->Start();
    if (FAILED(p.hr)) {
        p.state = p.hr == AUDCLNT_E_DEVICE_IN_USE ? Probe::Busy : Probe::OpenFailed;
    } else {
        wv::LinkHandle L;
        uint64_t h0 = 0;
        bool haveH0 = false;
        for (int t = 0; t < ms; t += 50) {
            Sleep(50);
            UINT32 n = 0;
            while (SUCCEEDED(cc->GetNextPacketSize(&n)) && n) {
                BYTE* d; UINT32 f; DWORD fl;
                if (FAILED(cc->GetBuffer(&d, &f, &fl, nullptr, nullptr))) break;
                cc->ReleaseBuffer(f);
            }
            if (!L.block && t >= 500) {
                if (wv::openLink(e.guid, L, false)) { p.linkFound = true; p.viaFile = L.viaFile; }
            }
            if (L.block && !haveH0 && t >= 1000) { h0 = L.block->heartbeat.load(); haveH0 = true; }
        }
        p.state = Probe::NotRunning;
        if (L.block && haveH0 && L.block->heartbeat.load() > h0) {
            p.state = Probe::Running;
            wv::seqRead(L.block->analysisSeq, L.block->analysis, p.a);
        }
        L.close();
        ac->Stop();
    }
    if (mix) CoTaskMemFree(mix);
    if (cc) cc->Release();
    if (ac) ac->Release();
    if (dev) dev->Release();
    if (en) en->Release();
    return p;
}

static LONGLONG fileSize(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA a{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a)) return 0;
    return ((LONGLONG)a.nFileSizeHigh << 32) | a.nFileSizeLow;
}
// Print what the APO logged since 'from' (bytes into apo.log).
static void showApoLog(LONGLONG from) {
    const std::wstring path = wv::dataDir() + L"\\apo.log";
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) { note(L"    apo.log: none - Windows never loaded WarmVoiceApo.dll\n"); return; }
    LARGE_INTEGER sz{};
    GetFileSizeEx(h, &sz);
    if (from > sz.QuadPart) from = 0;
    LONGLONG len = sz.QuadPart - from;
    if (len > 6000) { from = sz.QuadPart - 6000; len = 6000; }
    std::string s((size_t)len, '\0');
    LARGE_INTEGER off; off.QuadPart = from;
    SetFilePointerEx(h, off, nullptr, FILE_BEGIN);
    DWORD rd = 0;
    if (len > 0) ReadFile(h, &s[0], (DWORD)len, &rd, nullptr);
    CloseHandle(h);
    s.resize(rd);
    if (s.empty()) { note(L"    apo.log: nothing new\n"); return; }
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    size_t a = 0;
    while (a < w.size()) {
        size_t b = w.find(L'\n', a);
        if (b == std::wstring::npos) b = w.size();
        std::wstring line = w.substr(a, b - a);
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        if (!line.empty()) note(L"    log: %ls\n", line.c_str());
        a = b + 1;
    }
}

static void reportProbe(const Endpoint& e, const Probe& p) {
    switch (p.state) {
        case Probe::Running: {
            static const wchar_t* why[] = {L"", L"（AI 文件缺失）", L"（AI 出错）", L"（电脑太忙）"};
            say(L"  结果：正常工作 | AI 降噪%ls%ls | CPU %.1f%%\n", p.a.aiActive ? L"开" : L"没开",
                why[p.a.aiProblem >= 0 && p.a.aiProblem <= 3 ? p.a.aiProblem : 0], p.a.load * 100.0f);
            note(L"  link %ls\n", p.viaFile ? L"file" : L"global");
            break;
        }
        case Probe::NotRunning:
            say(L"  结果：麦克风有声音，但 Windows 没让暖声处理\n");
            note(L"  link found earlier: %d\n", (int)p.linkFound);
            break;
        case Probe::Busy:
            say(L"  结果：麦克风被别的软件独占了（多半是 Voicemeeter）。被独占时 Windows 的\n"
                L"        声音效果都不能运行。请完全退出那个软件，再运行一次。\n");
            break;
        default:
            say(L"  结果：打不开麦克风（错误码 0x%08lx）\n", (unsigned long)p.hr);
    }
}

// ---------------------------------------------------------------- diagnostics
static void dumpFx(const Endpoint& e) {
    note(L"  FxProperties of \"%ls\":\n", e.name.c_str());
    HKEY fx = openFx(e, false);
    if (!fx) { note(L"    (no FxProperties key)\n"); return; }
    for (DWORD i = 0;; ++i) {
        wchar_t name[256];
        DWORD nl = 256, type = 0, size = 0;
        if (RegEnumValueW(fx, i, name, &nl, nullptr, &type, nullptr, &size) != ERROR_SUCCESS) break;
        std::vector<BYTE> buf(size + 4, 0);
        nl = 256;
        RegEnumValueW(fx, i, name, &nl, nullptr, &type, buf.data(), &size);
        std::wstring v;
        if (type == REG_SZ || type == REG_EXPAND_SZ) v = (const wchar_t*)buf.data();
        else if (type == REG_MULTI_SZ) {
            for (const wchar_t* p = (const wchar_t*)buf.data(); *p; p += wcslen(p) + 1) v += std::wstring(v.empty() ? L"" : L" | ") + p;
        } else if (type == REG_DWORD) v = std::to_wstring(*(DWORD*)buf.data());
        else v = L"(" + std::to_wstring(size) + L" bytes)";
        note(L"    %ls = %ls\n", name, v.c_str());
    }
    RegCloseKey(fx);
}

static void dumpSystem() {
    HKEY k;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", 0, KEY_READ, &k) == ERROR_SUCCESS) {
        wchar_t prod[128] = {}, disp[64] = {}, build[32] = {};
        DWORD ubr = 0, s1 = sizeof(prod), s2 = sizeof(disp), s3 = sizeof(build), s4 = sizeof(ubr);
        RegQueryValueExW(k, L"ProductName", nullptr, nullptr, (BYTE*)prod, &s1);
        RegQueryValueExW(k, L"DisplayVersion", nullptr, nullptr, (BYTE*)disp, &s2);
        RegQueryValueExW(k, L"CurrentBuild", nullptr, nullptr, (BYTE*)build, &s3);
        RegQueryValueExW(k, L"UBR", nullptr, nullptr, (BYTE*)&ubr, &s4);
        RegCloseKey(k);
        note(L"Windows: %ls %ls build %ls.%lu\n", prod, disp, build, ubr);
    }
    // Programs known to grab microphones exclusively.
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    DWORD audiodg = 0;
    if (snap != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W pe{sizeof(pe)};
        for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
            std::wstring n = pe.szExeFile;
            for (auto& c : n) c = (wchar_t)towlower(c);
            if (n == L"audiodg.exe") audiodg = pe.th32ProcessID;
            if (n.find(L"voicemeeter") != std::wstring::npos || n.find(L"vbaudio") != std::wstring::npos ||
                n.find(L"ngenuity") != std::wstring::npos || n.find(L"nahimic") != std::wstring::npos ||
                n.find(L"sonar") != std::wstring::npos || n.find(L"broadcast") != std::wstring::npos)
                say(L"注意：%ls 正在运行，它可能会占用或改动麦克风\n", pe.szExeFile);
        }
        CloseHandle(snap);
    }
    if (audiodg) {
        bool loaded = false, readable = false;
        HANDLE ms = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, audiodg);
        if (ms != INVALID_HANDLE_VALUE) {
            readable = true;
            MODULEENTRY32W me{sizeof(me)};
            for (BOOL ok = Module32FirstW(ms, &me); ok; ok = Module32NextW(ms, &me))
                if (_wcsicmp(me.szModule, L"WarmVoiceApo.dll") == 0) loaded = true;
            CloseHandle(ms);
        }
        note(L"audiodg.exe (pid %lu): WarmVoiceApo.dll %ls\n", audiodg,
            !readable ? L"unknown (cannot look inside)" : loaded ? L"LOADED" : L"not loaded");
    } else {
        note(L"audiodg.exe: not running\n");
    }
}

// ---------------------------------------------------------------- commands
static int uninstall(const Endpoint& e);
static int install(const Endpoint& e, const std::wstring& slotArg) {
    prepareDataDir();
    if (!registerDll()) { say(L"注册暖声组件失败\n"); return 1; }
    std::vector<std::wstring> slots;
    if (slotArg == L"auto") slots = {L"efx", L"mfx", L"sfx"};
    else slots = {slotArg};
    say(L"正在安装到：%ls\n", e.name.c_str());
    for (size_t i = 0; i < slots.size(); ++i) {
        const std::wstring& slot = slots[i];
        if (!applySlot(e, slot)) return 1;
        say(L"第 %zu/%zu 种方式：重启 Windows 声音服务并测试（声音会断几秒）…\n", i + 1, slots.size());
        note(L"  slot %ls\n", slot.c_str());
        const LONGLONG logPos = fileSize(wv::dataDir() + L"\\apo.log");
        if (!restartAudioService()) { say(L"  重启声音服务失败，请重启电脑后再试一次。\n"); return 1; }
        waitForEndpoint(e.id, 8000);
        Sleep(500);
        const Probe p = probe(e, 3000);
        reportProbe(e, p);
        if (p.state != Probe::Running) showApoLog(logPos);
        if (p.state == Probe::Running) {
            say(L"\n成功：暖声已经在“%ls”上工作了。\n", e.name.c_str());
            note(L"slot %ls\n", slot.c_str());
            return 0;
        }
        if (p.state == Probe::Busy) {
            say(L"\n暖声已经装好，但要等占用麦克风的软件退出后才能开始工作。\n");
            return 3;
        }
    }
    // Nothing worked: put the microphone back exactly as it was, so it keeps working normally.
    dumpFx(e);
    uninstall(e);
    restartAudioService();
    say(L"\n没成功：三种方式 Windows 都没让暖声处理这个麦克风。已经把麦克风恢复原样，照常能用。\n");
    return 4;
}

static int uninstall(const Endpoint& e) {
    HKEY bk = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, backupPath(e).c_str(), 0, KEY_READ, &bk) != ERROR_SUCCESS) {
        say(L"“%ls”没装暖声\n", e.name.c_str());
        return 0;
    }
    HKEY fx = openFx(e, true);
    if (!fx) { RegCloseKey(bk); say(L"改不了系统设置（需要管理员权限）\n"); return 1; }
    restoreValues(fx, bk);
    RegCloseKey(fx);
    RegCloseKey(bk);
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, backupPath(e).c_str());
    say(L"已从“%ls”上卸载，恢复原样\n", e.name.c_str());
    return 0;
}

static int status(const Endpoint& e) {
    say(L"“%ls”：%ls\n", e.name.c_str(), isInstalled(e) ? L"已安装暖声" : L"没装暖声");
    const LONGLONG logPos = fileSize(wv::dataDir() + L"\\apo.log");
    const Probe p = probe(e, 2500);
    reportProbe(e, p);
    if (p.state != Probe::Running) showApoLog(logPos);
    return p.state == Probe::Running ? 0 : 1;
}

static int diagnose(const Endpoint& e) {
    dumpSystem();
    dumpFx(e);
    const int rc = status(e);
    dumpSystem();   // after the probe: is the DLL inside audiodg now?
    note(L"\nLast lines of apo.log:\n");
    const LONGLONG sz = fileSize(wv::dataDir() + L"\\apo.log");
    showApoLog(sz > 3000 ? sz - 3000 : 0);
    return rc;
}

int wmain(int argc, wchar_t** argv) {
    // Print Unicode (device names are often Chinese, e.g. "麦克风") instead of "???".
    _setmode(_fileno(stdout), _O_U16TEXT);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool pauseAtEnd = false;
    std::vector<std::wstring> args;
    for (int i = 1; i < argc; ++i) {
        if (std::wstring(argv[i]) == L"--pause") pauseAtEnd = true;
        else args.push_back(argv[i]);
    }
    const std::wstring cmd = args.empty() ? L"list" : args[0];
    if (cmd != L"list") {
        prepareDataDir();
        _wfopen_s(&g_report, (wv::dataDir() + L"\\report.txt").c_str(), L"ab");
        SYSTEMTIME t;
        GetLocalTime(&t);
        note(L"\n===== wvinstall %ls  %04d-%02d-%02d %02d:%02d:%02d =====\n", cmd.c_str(), t.wYear, t.wMonth, t.wDay, t.wHour,
            t.wMinute, t.wSecond);
    }
    auto finish = [&](int rc) {
        if (g_report) { std::fclose(g_report); g_report = nullptr; }
        CoUninitialize();
        if (pauseAtEnd) { std::fputws(L"\n按回车键关闭这个窗口。", stdout); std::getwchar(); }
        return rc;
    };
    auto eps = listEndpoints();
    if (cmd == L"list") {
        for (size_t i = 0; i < eps.size(); ++i) {
            std::wprintf(L"[%zu] %ls  %lu Hz %lu ch%ls\n     %ls\n", i, eps[i].name.c_str(), eps[i].rate, eps[i].channels,
                         isInstalled(eps[i]) ? L"  ← 已装暖声" : L"", eps[i].id.c_str());
            if (eps[i].rate && eps[i].rate < 32000)
                std::wprintf(L"     （这个麦克风只有 %lu Hz，补高频帮不了它）\n", eps[i].rate);
        }
        if (eps.empty()) std::wprintf(L"没找到可用的麦克风。\n");
        return finish(0);
    }
    if (eps.empty()) {
        say(L"没找到可用的麦克风。请检查麦克风是否插好、在 Windows 声音设置里是否被禁用。\n");
        return finish(2);
    }
    std::vector<const Endpoint*> targets;
    if (args.size() >= 2) {
        const std::wstring sel = args[1];
        std::wstring defId;
        if (sel == L"default") {
            IMMDeviceEnumerator* en = nullptr;
            if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en)))) {
                IMMDevice* dev = nullptr;
                if (SUCCEEDED(en->GetDefaultAudioEndpoint(eCapture, eConsole, &dev))) {
                    LPWSTR id = nullptr;
                    if (SUCCEEDED(dev->GetId(&id))) { defId = id; CoTaskMemFree(id); }
                    dev->Release();
                }
                en->Release();
            }
        }
        std::wstring selGuid = wv::guidFromEndpointId(sel);
        if (sel == L"all") for (auto& e : eps) targets.push_back(&e);
        else if (sel == L"default") { for (auto& e : eps) if (e.id == defId) targets.push_back(&e); }
        else {
            for (size_t i = 0; i < eps.size(); ++i)
                if (sel == std::to_wstring(i) || _wcsicmp(sel.c_str(), eps[i].id.c_str()) == 0 || selGuid == eps[i].guid)
                    targets.push_back(&eps[i]);
        }
    }
    if (targets.empty()) { say(L"没找到指定的麦克风\n"); return finish(2); }
    std::wstring slot = L"auto";
    for (size_t i = 2; i + 1 < args.size(); ++i)
        if (args[i] == L"--slot") slot = args[i + 1];
    int rc = 0;
    for (auto* e : targets) {
        if (cmd == L"install") rc = std::max(rc, install(*e, slot));
        else if (cmd == L"uninstall") rc |= uninstall(*e);
        else if (cmd == L"status") rc |= status(*e);
        else if (cmd == L"diagnose") rc |= diagnose(*e);
        else { say(L"未知命令\n"); return finish(2); }
    }
    if (cmd == L"uninstall") restartAudioService();
    say(L"记录已保存到 %ls\\report.txt\n", wv::dataDir().c_str());
    return finish(rc);
}
