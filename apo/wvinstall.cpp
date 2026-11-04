// wvinstall: put the WarmVoice APO on a microphone (run as administrator).
//
//   wvinstall list                      microphones, their format and WarmVoice status
//   wvinstall install <n|id> [--slot efx|mfx|sfx]   default slot: efx (shared by all apps, works for raw streams)
//   wvinstall uninstall <n|id|all>      restore what was there before
//   wvinstall status <n|id>             is the APO actually running? (needs audio flowing)
//
// The previous registry values are backed up under HKLM\SOFTWARE\WarmVoice\Backup\<endpoint>,
// so uninstall puts back exactly what the driver had (including other vendors' effects).
// SPDX-License-Identifier: MIT
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <initguid.h>   // instantiate the SDK GUID / PROPERTYKEY constants used below
#include <aclapi.h>
#include <audioenginebaseapo.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mmdeviceapi.h>
#include <propvarutil.h>
#include <sddl.h>

#include <cstdio>
#include <string>
#include <vector>

#include "guids.h"
#include "wv/shared.h"

static const wchar_t* kMMDevicesCapture = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\Capture\\";
static const wchar_t* kBackupRoot = L"SOFTWARE\\WarmVoice\\Backup\\";
// AUDIO_SIGNALPROCESSINGMODE_DEFAULT
static const wchar_t* kModeDefault = L"{C18E2F7E-933D-4965-B7D1-1EEF228D2AF3}";

struct Endpoint {
    std::wstring id, guid, name;
    DWORD rate = 0, channels = 0;
};

static std::wstring pkeyName(const PROPERTYKEY& k) {
    wchar_t g[64];
    StringFromGUID2(k.fmtid, g, 64);
    return std::wstring(g) + L"," + std::to_wstring(k.pid);
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

static std::vector<std::wstring> managedValues() {
    return {pkeyName(PKEY_FX_StreamEffectClsid),
            pkeyName(PKEY_FX_ModeEffectClsid),
            pkeyName(PKEY_FX_EndpointEffectClsid),
            pkeyName(PKEY_SFX_ProcessingModes_Supported_For_Streaming),
            pkeyName(PKEY_MFX_ProcessingModes_Supported_For_Streaming),
            pkeyName(PKEY_EFX_ProcessingModes_Supported_For_Streaming),
            pkeyName(PKEY_AudioEndpoint_Disable_SysFx)};
}

static bool restartAudioService() {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return false;
    SC_HANDLE svc = OpenServiceW(scm, L"Audiosrv", SERVICE_STOP | SERVICE_START | SERVICE_QUERY_STATUS);
    bool ok = false;
    if (svc) {
        SERVICE_STATUS st{};
        ControlService(svc, SERVICE_CONTROL_STOP, &st);
        for (int i = 0; i < 50; ++i) {   // up to 5 s
            if (QueryServiceStatus(svc, &st) && st.dwCurrentState == SERVICE_STOPPED) break;
            Sleep(100);
        }
        ok = StartServiceW(svc, 0, nullptr) != 0 || GetLastError() == ERROR_SERVICE_ALREADY_RUNNING;
        CloseServiceHandle(svc);
    }
    CloseServiceHandle(scm);
    return ok;
}

static bool registerDll() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir(exe);
    dir = dir.substr(0, dir.find_last_of(L"\\/") + 1);
    HMODULE m = LoadLibraryW((dir + L"WarmVoiceApo.dll").c_str());
    if (!m) { std::wprintf(L"WarmVoiceApo.dll not found next to wvinstall.exe\n"); return false; }
    auto reg = (HRESULT(STDAPICALLTYPE*)())GetProcAddress(m, "DllRegisterServer");
    const bool ok = reg && SUCCEEDED(reg());
    FreeLibrary(m);
    return ok;
}

static int install(const Endpoint& e, const std::wstring& slot) {
    if (!registerDll()) { std::wprintf(L"could not register the APO\n"); return 1; }
    HKEY fx = openFx(e, true);
    if (!fx) { std::wprintf(L"cannot open FxProperties for %ls (run as administrator)\n", e.name.c_str()); return 1; }
    HKEY bk = nullptr;
    const std::wstring bkPath = std::wstring(kBackupRoot) + e.guid;
    const bool haveBackup = RegOpenKeyExW(HKEY_LOCAL_MACHINE, bkPath.c_str(), 0, KEY_READ, &bk) == ERROR_SUCCESS;
    if (bk) RegCloseKey(bk);
    if (!haveBackup) {   // never overwrite the original backup on a re-install
        RegCreateKeyExW(HKEY_LOCAL_MACHINE, bkPath.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &bk, nullptr);
        for (auto& n : managedValues()) backupValue(fx, bk, n);
        RegCloseKey(bk);
    }
    const PROPERTYKEY& clsidKey = slot == L"sfx" ? PKEY_FX_StreamEffectClsid
                                : slot == L"mfx" ? PKEY_FX_ModeEffectClsid : PKEY_FX_EndpointEffectClsid;
    const PROPERTYKEY& modesKey = slot == L"sfx" ? PKEY_SFX_ProcessingModes_Supported_For_Streaming
                                : slot == L"mfx" ? PKEY_MFX_ProcessingModes_Supported_For_Streaming
                                                 : PKEY_EFX_ProcessingModes_Supported_For_Streaming;
    // Tell the user when another vendor's effect is being replaced (it is backed up).
    wchar_t old[64] = {};
    DWORD sz = sizeof(old), type = 0;
    if (RegQueryValueExW(fx, pkeyName(clsidKey).c_str(), nullptr, &type, (BYTE*)old, &sz) == ERROR_SUCCESS &&
        _wcsicmp(old, WV_CLSID_STRING) != 0)
        std::wprintf(L"note: replacing existing effect %ls (saved, restored on uninstall)\n", old);
    const std::wstring clsid = WV_CLSID_STRING;
    RegSetValueExW(fx, pkeyName(clsidKey).c_str(), 0, REG_SZ, (const BYTE*)clsid.c_str(), (DWORD)((clsid.size() + 1) * 2));
    std::wstring modes = std::wstring(kModeDefault) + L'\0' + L'\0';
    RegSetValueExW(fx, pkeyName(modesKey).c_str(), 0, REG_MULTI_SZ, (const BYTE*)modes.c_str(), (DWORD)(modes.size() * 2));
    const DWORD enabled = 0;   // ENDPOINT_SYSFX_ENABLED: Windows "audio enhancements" on
    RegSetValueExW(fx, pkeyName(PKEY_AudioEndpoint_Disable_SysFx).c_str(), 0, REG_DWORD, (const BYTE*)&enabled, sizeof(enabled));
    RegCloseKey(fx);
    std::wprintf(L"installed on \"%ls\" (%ls); restarting the Windows audio service...\n", e.name.c_str(), slot.c_str());
    std::wprintf(restartAudioService() ? L"done. Talk into the mic, then run: wvinstall status\n"
                                       : L"please restart the computer to finish.\n");
    return 0;
}

static int uninstall(const Endpoint& e) {
    const std::wstring bkPath = std::wstring(kBackupRoot) + e.guid;
    HKEY bk = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, bkPath.c_str(), 0, KEY_READ, &bk) != ERROR_SUCCESS) {
        std::wprintf(L"\"%ls\": not installed\n", e.name.c_str());
        return 0;
    }
    HKEY fx = openFx(e, true);
    if (!fx) { RegCloseKey(bk); std::wprintf(L"cannot open FxProperties (run as administrator)\n"); return 1; }
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
    RegCloseKey(fx);
    RegCloseKey(bk);
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, bkPath.c_str());
    std::wprintf(L"removed from \"%ls\"\n", e.name.c_str());
    return 0;
}

static int status(const Endpoint& e) {
    const std::wstring name = L"Global\\WarmVoice_" + e.guid;
    HANDLE m = OpenFileMappingW(FILE_MAP_READ, FALSE, name.c_str());
    if (!m) { std::wprintf(L"\"%ls\": APO not running (no audio yet, or not installed)\n", e.name.c_str()); return 1; }
    auto* b = (const wv::SharedBlock*)MapViewOfFile(m, FILE_MAP_READ, 0, 0, sizeof(wv::SharedBlock));
    int rc = 1;
    if (b && b->magic == wv::kSharedMagic) {
        const uint64_t h0 = b->heartbeat.load();
        Sleep(300);
        const uint64_t h1 = b->heartbeat.load();
        wv::Analysis a;
        wv::seqRead(b->analysisSeq, b->analysis, a);
        static const wchar_t* why[] = {L"", L" (AI library missing)", L" (AI engine error)", L" (computer too slow for AI)"};
        std::wprintf(L"\"%ls\": %ls | AI %ls%ls | CPU %.1f%% of budget | in peak %.1f dB | out %.1f dB\n",
                     e.name.c_str(), h1 > h0 ? L"RUNNING" : L"loaded but no audio", a.aiActive ? L"on" : L"OFF",
                     why[a.aiProblem >= 0 && a.aiProblem <= 3 ? a.aiProblem : 0],
                     a.load * 100.0f, a.inPeakDb, a.outLevelDb);
        rc = h1 > h0 ? 0 : 1;
    }
    if (b) UnmapViewOfFile(b);
    CloseHandle(m);
    return rc;
}

int wmain(int argc, wchar_t** argv) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    auto eps = listEndpoints();
    if (argc < 2 || std::wstring(argv[1]) == L"list") {
        for (size_t i = 0; i < eps.size(); ++i) {
            HKEY bk = nullptr;
            const bool inst = RegOpenKeyExW(HKEY_LOCAL_MACHINE, (std::wstring(kBackupRoot) + eps[i].guid).c_str(), 0,
                                            KEY_READ, &bk) == ERROR_SUCCESS;
            if (bk) RegCloseKey(bk);
            std::wprintf(L"[%zu] %ls  %lu Hz %lu ch%ls\n     %ls\n", i, eps[i].name.c_str(), eps[i].rate, eps[i].channels,
                         inst ? L"  <- WarmVoice installed" : L"", eps[i].id.c_str());
            if (eps[i].rate && eps[i].rate < 32000)
                std::wprintf(L"     (this mic runs at %lu Hz: high-frequency restoration cannot help it)\n", eps[i].rate);
        }
        return 0;
    }
    const std::wstring cmd = argv[1];
    std::vector<const Endpoint*> targets;
    if (argc >= 3) {
        const std::wstring sel = argv[2];
        if (sel == L"all") for (auto& e : eps) targets.push_back(&e);
        else {
            for (size_t i = 0; i < eps.size(); ++i)
                if (sel == std::to_wstring(i) || sel == eps[i].id || sel == eps[i].guid) targets.push_back(&eps[i]);
        }
    }
    if (targets.empty()) { std::wprintf(L"choose a microphone: wvinstall list\n"); return 2; }
    std::wstring slot = L"efx";
    for (int i = 3; i + 1 < argc; ++i)
        if (std::wstring(argv[i]) == L"--slot") slot = argv[i + 1];
    int rc = 0;
    for (auto* e : targets) {
        if (cmd == L"install") rc |= install(*e, slot);
        else if (cmd == L"uninstall") rc |= uninstall(*e);
        else if (cmd == L"status") rc |= status(*e);
        else { std::wprintf(L"unknown command\n"); return 2; }
    }
    if (cmd == L"uninstall") restartAudioService();
    CoUninitialize();
    return rc;
}
