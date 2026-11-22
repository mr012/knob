// Knob - a tiny tray flyout for Windows 10/11 (design B: Sound | Apps | Display; Solid / Glass styles)
//  * Sound:   pick any number of outputs (audio is mirrored to all of them), input device, per-device volume/mute
//  * Apps:    master volume (moves every selected output) + per-app volume with live "playing" state
//  * Display: brightness, contrast (DDC/CI), orientation per display; laptop panel via WMI; identify displays
// Pure Win32 + GDI+, no runtime, no dependencies.

#define _WIN32_WINNT 0x0A00
#define WINVER 0x0A00
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shellscalingapi.h>
#include <dwmapi.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <audioclient.h>
#include <audiopolicy.h>
#include <physicalmonitorenumerationapi.h>
#include <highlevelmonitorconfigurationapi.h>
#include <lowlevelmonitorconfigurationapi.h>
#include <wbemidl.h>
#include <winhttp.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <map>
#include <set>
#include <string>
#include <vector>
namespace Gdiplus { using std::min; using std::max; }
#include <gdiplus.h>
using namespace Gdiplus;

#define APP_NAME    L"Knob"
#define APP_VERSION L"1.25.0"
#define APP_VERNUM  ((1 << 16) | (25 << 8) | 0)
#define UPDATE_REPO L"mr012/knob"   // GitHub repo whose latest release is checked for updates   // compared when a newer exe replaces a running one

// ---------------------------------------------------------------- globals
enum { WM_APP_TRAY = WM_APP + 1, WM_APP_AUDIOCHANGED, WM_APP_MONITORS, WM_APP_SHOW, WM_APP_VOLCHANGED, WM_APP_VERSION, WM_APP_QUIT, WM_APP_UPDINFO, WM_APP_CALDONE };
enum { TIMER_REFRESH = 1, TIMER_AUDIODEBOUNCE = 2, TIMER_ANIM = 3, TIMER_IDENT = 4, TIMER_SPIN = 5, TIMER_OSD = 6, TIMER_UPDCHECK = 7, TIMER_TRAYPIN = 8, TIMER_CALRETRY = 9 };
enum Tab { T_SOUND = 0, T_APPS = 1, T_DISPLAY = 2 };

static HINSTANCE g_inst;
static HWND g_hwnd;
static UINT WM_TASKBARCREATED;
static NOTIFYICONDATAW g_nid;
static HICON g_trayIcon;
static bool g_visible = false;
static HWND g_set;   // Settings window (declared early: the panel checks it)
static bool g_pairHidden = false;   // panel tucked away because Settings went behind other windows
static DWORD g_hideTick = 0;
static std::wstring g_iniPath;
static int g_trayPinned = -1;   // tray icon on the taskbar: -1 unknown, 0 hidden, 1 shown
static bool g_trayGuidOk = true;
static const GUID KNOB_TRAY_GUID = {0x5b3c1e7a, 0x2f4d, 0x4c8e, {0x9a, 0x61, 0x3d, 0x0b, 0x7e, 0x24, 0xc5, 0x91}};
static bool g_keepAwake = false;
static int g_tab = T_SOUND;
static int g_style = 1;      // 0 = Solid, 1 = Glass
static int g_accentId = 0;   // 0 = Windows accent, 1 = custom (g_accentHue / g_accentSat)
static int g_accentHue = 25, g_accentSat = 85;
static float g_scroll[4] = {0, 0, 0, 0};   // per tab + shortcuts page
static int g_view = 0;                       // 0 = main tabs, 1 = shortcuts page
static int SI();

// global shortcuts
enum HkAction { HK_OPEN, HK_COUNT };   // only one global shortcut: open the panel
struct HotKey { UINT mods; UINT vk; bool ok; };
static HotKey g_hk[HK_COUNT];
static const wchar_t* HK_NAMES[HK_COUNT] = {L"Open Knob"};
static const wchar_t* HK_KEYS[HK_COUNT] = {L"open"};
static const HotKey HK_DEFAULT[HK_COUNT] = {{0, 0, false}};   // no shortcut until the user picks one
static bool g_noSave = false, g_restartAfterExit = false;
// update check (GitHub releases)
enum { UPD_NONE, UPD_AVAILABLE, UPD_DOWNLOADING, UPD_FAILED };
static int g_updState = UPD_NONE;
static std::wstring g_updVersion, g_updUrl, g_updStatus = L"";
static volatile LONG g_updBusy = 0;
static Gdiplus::RectF g_verRect;
static int g_capture = -1;      // shortcut row waiting for a key combo
static bool g_osd = true;       // on-screen popup when a shortcut is used
static int g_step = 5;          // volume step for shortcuts (%)

// display profiles: name -> per-monitor brightness/contrast
struct Profile { std::wstring name; std::map<std::wstring, std::pair<int, int>> v; };
static std::vector<Profile> g_prof;   // user-named display profiles (max 8)
static const int MAX_PROFILES = 8;
static bool g_restoreBright = true;                                // put each monitor back to its last brightness when it reconnects
static std::map<std::wstring, std::pair<int, int>> g_lastMon;       // monitor name -> last brightness / contrast
static std::map<std::wstring, int> g_delay;                         // output id -> sync delay (ms)
static bool g_restoreAllNext = false;                               // after resume: treat every monitor as reconnected

static int g_dpi = 96;
static float g_scale = 1.f;
static inline float S(float v) { return v * g_scale; }

// ---------------------------------------------------------------- IPolicyConfig (undocumented; used by every default-device switcher)
static const CLSID CLSID_PolicyConfigClient = {0x870af99c, 0x171d, 0x4f9e, {0xaf, 0x0d, 0xe6, 0x3d, 0xf4, 0x0c, 0x2b, 0xc9}};
static const IID IID_IPolicyConfig = {0xf8679f50, 0x850a, 0x41cf, {0x9c, 0x72, 0x43, 0x0f, 0x29, 0x02, 0x90, 0xc8}};
struct IPolicyConfig : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetMixFormat(PCWSTR, WAVEFORMATEX**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDeviceFormat(PCWSTR, INT, WAVEFORMATEX**) = 0;
    virtual HRESULT STDMETHODCALLTYPE ResetDeviceFormat(PCWSTR) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDeviceFormat(PCWSTR, WAVEFORMATEX*, WAVEFORMATEX*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetProcessingPeriod(PCWSTR, INT, PINT64, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetProcessingPeriod(PCWSTR, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetShareMode(PCWSTR, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetShareMode(PCWSTR, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPropertyValue(PCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPropertyValue(PCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDefaultEndpoint(PCWSTR, ERole) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetEndpointVisibility(PCWSTR, INT) = 0;
};

// mingw only forward-declares IAudioMeterInformation
static const IID IID_Meter = {0xC02216F6, 0x8C67, 0x4B5B, {0x9D, 0x00, 0xD0, 0x08, 0xE7, 0x3E, 0x00, 0x64}};
struct Meter : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetPeakValue(float*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetMeteringChannelCount(UINT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetChannelsPeakValues(UINT32, float*) = 0;
    virtual HRESULT STDMETHODCALLTYPE QueryHardwareSupport(DWORD*) = 0;
};

static const PROPERTYKEY PK_FriendlyName = {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};
static const PROPERTYKEY PK_DeviceDesc = {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 2};
static const PROPERTYKEY PK_IfaceName = {{0x026e516e, 0xb814, 0x414b, {0x83, 0xcd, 0x85, 0x6d, 0x6f, 0xef, 0x48, 0x22}}, 2};
static const PROPERTYKEY PK_FormFactor = {{0x1da5d803, 0xd492, 0x4edd, {0x8c, 0x23, 0xe0, 0xc0, 0xff, 0xee, 0x7f, 0x0e}}, 0};

// ---------------------------------------------------------------- models
enum DevKind { DK_SPEAKER, DK_HEADPHONES, DK_MIC, DK_DISPLAY };
struct Endpoint {
    std::wstring id, name, detail;
    int kind = DK_SPEAKER;
    bool isDefault = false;
    IAudioEndpointVolume* vol = nullptr;
    float level = 0;
    BOOL mute = FALSE;
};
struct AppGroup {
    std::wstring key, name, path;
    bool system = false;
    std::vector<ISimpleAudioVolume*> vols;
    std::vector<Meter*> meters;
    float level = 1;
    BOOL mute = FALSE;
    float peak = 0;
};
struct ProcInfo { std::wstring name; Gdiplus::Bitmap* icon = nullptr; };
struct AppLive { float smooth = 0; DWORD lastActive = 0; };

static IMMDeviceEnumerator* g_enum;
static std::vector<Endpoint> g_out, g_in;
static std::vector<AppGroup> g_apps;
static std::map<std::wstring, ProcInfo> g_procCache;
static std::map<std::wstring, AppLive> g_live;
static std::set<std::wstring> g_mirror;   // extra outputs that play a copy of the default output
static std::set<std::wstring> g_mirrorSaved;   // the group to bring back when Multi is turned on again
static bool g_multi = false;   // Multi on: clicking outputs adds/removes them; off: clicking switches output

struct MonUI { std::wstring name, detail, gdi; int pct = 0, con = 50, orient = 0; bool ok = false, hasCon = false, internal = false; RECT rc{}; };
struct MonMsg { int gen; bool fromEnum; std::vector<MonUI> mons; };
static std::vector<MonUI> g_mons;
static int g_monGen = -1;
static bool g_monsLoaded = false;

// ---------------------------------------------------------------- helpers
static std::wstring ToLower(std::wstring s) { if (!s.empty()) CharLowerBuffW(&s[0], (DWORD)s.size()); return s; }

static std::wstring ProcPath(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return L"";
    wchar_t buf[1024]; DWORD n = 1024; std::wstring r;
    if (QueryFullProcessImageNameW(h, 0, buf, &n)) r.assign(buf, n);
    CloseHandle(h);
    return r;
}

static std::wstring FileDescription(const std::wstring& path) {
    DWORD dummy = 0, sz = GetFileVersionInfoSizeW(path.c_str(), &dummy);
    if (!sz) return L"";
    std::vector<BYTE> b(sz);
    if (!GetFileVersionInfoW(path.c_str(), 0, sz, b.data())) return L"";
    struct LangCp { WORD l, c; }* tr = nullptr; UINT len = 0;
    if (!VerQueryValueW(b.data(), L"\\VarFileInfo\\Translation", (void**)&tr, &len) || len < sizeof(LangCp)) return L"";
    wchar_t q[80]; swprintf(q, 80, L"\\StringFileInfo\\%04x%04x\\FileDescription", tr[0].l, tr[0].c);
    wchar_t* s = nullptr;
    if (VerQueryValueW(b.data(), q, (void**)&s, &len) && len > 1 && s && *s) return s;
    return L"";
}

static Gdiplus::Bitmap* IconToBitmap(HICON hi) {
    ICONINFO ii = {};
    if (!GetIconInfo(hi, &ii)) return nullptr;
    Gdiplus::Bitmap* out = nullptr;
    BITMAP bm = {};
    if (ii.hbmColor && GetObject(ii.hbmColor, sizeof(bm), &bm)) {
        int w = bm.bmWidth, h = bm.bmHeight;
        std::vector<DWORD> px((size_t)w * h);
        BITMAPINFO bi = {}; bi.bmiHeader.biSize = sizeof(bi.bmiHeader); bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h;
        bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
        HDC dc = GetDC(nullptr);
        if (GetDIBits(dc, ii.hbmColor, 0, h, px.data(), &bi, DIB_RGB_COLORS)) {
            bool hasAlpha = false;
            for (DWORD p : px) if (p >> 24) { hasAlpha = true; break; }
            if (!hasAlpha && ii.hbmMask) {   // old-style icon: build alpha from the AND mask
                std::vector<DWORD> mk((size_t)w * h);
                if (GetDIBits(dc, ii.hbmMask, 0, h, mk.data(), &bi, DIB_RGB_COLORS))
                    for (size_t i = 0; i < px.size(); i++) px[i] = (px[i] & 0xFFFFFF) | ((mk[i] & 0xFFFFFF) ? 0 : 0xFF000000u);
                else for (auto& p : px) p |= 0xFF000000u;
            }
            out = new Gdiplus::Bitmap(w, h, PixelFormat32bppARGB);
            Gdiplus::BitmapData bd; Gdiplus::Rect r(0, 0, w, h);
            if (out->LockBits(&r, Gdiplus::ImageLockModeWrite, PixelFormat32bppARGB, &bd) == Gdiplus::Ok) {
                for (int y = 0; y < h; y++) memcpy((BYTE*)bd.Scan0 + y * bd.Stride, &px[(size_t)y * w], (size_t)w * 4);
                out->UnlockBits(&bd);
            }
        }
        ReleaseDC(nullptr, dc);
    }
    if (ii.hbmColor) DeleteObject(ii.hbmColor);
    if (ii.hbmMask) DeleteObject(ii.hbmMask);
    return out;
}

static ProcInfo& GetProcInfo(const std::wstring& path) {
    auto it = g_procCache.find(path);
    if (it != g_procCache.end()) return it->second;
    ProcInfo pi;
    pi.name = FileDescription(path);
    if (pi.name.empty()) {
        pi.name = PathFindFileNameW(path.c_str());
        size_t dot = pi.name.rfind(L'.');
        if (dot != std::wstring::npos) pi.name.resize(dot);
    }
    int isz = (int)S(22);
    HICON big = nullptr;
    if (SHDefExtractIconW(path.c_str(), 0, 0, &big, nullptr, MAKELONG(isz, isz)) == S_OK && big) { pi.icon = IconToBitmap(big); DestroyIcon(big); }
    return g_procCache[path] = pi;
}
static void ClearProcCache() {
    for (auto& kv : g_procCache) delete kv.second.icon;
    g_procCache.clear();
}

// ---------------------------------------------------------------- settings
static void LoadSettings() {
    wchar_t p[MAX_PATH] = {};
    SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, p);
    std::wstring dir = std::wstring(p) + L"\\Knob";
    CreateDirectoryW(dir.c_str(), nullptr);
    g_iniPath = dir + L"\\settings.ini";
    if (GetFileAttributesW(g_iniPath.c_str()) == INVALID_FILE_ATTRIBUTES)   // first run as Knob: bring over LiteMix settings
        CopyFileW((std::wstring(p) + L"\\LiteMix\\settings.ini").c_str(), g_iniPath.c_str(), TRUE);
    std::vector<wchar_t> buf(32768);
    GetPrivateProfileStringW(L"main", L"mirror", L"", buf.data(), (DWORD)buf.size(), g_iniPath.c_str());
    std::wstring s = buf.data(), cur;
    for (wchar_t c : s) { if (c == L'|') { if (!cur.empty()) g_mirror.insert(cur); cur.clear(); } else cur += c; }
    if (!cur.empty()) g_mirror.insert(cur);
    GetPrivateProfileStringW(L"main", L"mirrorsaved", L"", buf.data(), (DWORD)buf.size(), g_iniPath.c_str());
    s = buf.data(); cur.clear();
    for (wchar_t c : s) { if (c == L'|') { if (!cur.empty()) g_mirrorSaved.insert(cur); cur.clear(); } else cur += c; }
    if (!cur.empty()) g_mirrorSaved.insert(cur);
    g_multi = GetPrivateProfileIntW(L"main", L"multi", g_mirror.empty() ? 0 : 1, g_iniPath.c_str()) != 0;
    if (!g_multi) g_mirror.clear();
    g_keepAwake = GetPrivateProfileIntW(L"main", L"keepawake", 0, g_iniPath.c_str()) != 0;
    g_tab = std::max(0, std::min(2, (int)GetPrivateProfileIntW(L"main", L"tab", 0, g_iniPath.c_str())));
    g_style = std::max(0, std::min(1, (int)GetPrivateProfileIntW(L"main", L"style", 1, g_iniPath.c_str())));
    {
        static const int oldHue[7] = {0, 181, 113, 37, 346, 262, 0};   // presets from earlier versions
        int mode = (int)GetPrivateProfileIntW(L"main", L"accentmode", 99, g_iniPath.c_str());
        if (mode == 99) {   // migrate the old preset choice
            int old = std::max(0, std::min(6, (int)GetPrivateProfileIntW(L"main", L"accent", 0, g_iniPath.c_str())));
            mode = old ? 1 : 0;
            if (old) { g_accentHue = oldHue[old]; g_accentSat = old == 6 ? 0 : 85; }
        } else {
            g_accentHue = (int)GetPrivateProfileIntW(L"main", L"accenthue", 25, g_iniPath.c_str()) % 360;
            g_accentSat = std::max(0, std::min(100, (int)GetPrivateProfileIntW(L"main", L"accentsat", 85, g_iniPath.c_str())));
        }
        g_accentId = mode ? 1 : 0;
    }
    g_osd = GetPrivateProfileIntW(L"main", L"osd", 1, g_iniPath.c_str()) != 0;
    g_restoreBright = GetPrivateProfileIntW(L"main", L"restorebright", 1, g_iniPath.c_str()) != 0;
    g_trayPinned = (int)GetPrivateProfileIntW(L"main", L"traypinned", -1, g_iniPath.c_str());
    {   // sections stored as key=value lists
        auto readSection = [&](const wchar_t* sec, auto fn) {
            std::vector<wchar_t> b(32768);
            DWORD n = GetPrivateProfileSectionW(sec, b.data(), (DWORD)b.size(), g_iniPath.c_str());
            for (wchar_t* p = b.data(); p < b.data() + n && *p; p += wcslen(p) + 1) {
                std::wstring kv = p; size_t eq = kv.rfind(L'=');
                if (eq != std::wstring::npos) fn(kv.substr(0, eq), kv.substr(eq + 1));
            }
        };
        readSection(L"delay", [](const std::wstring& k, const std::wstring& v) { g_delay[k] = _wtoi(v.c_str()); });
        readSection(L"lastdisplay", [](const std::wstring& k, const std::wstring& v) {
            int b = -1, c = -1; if (swscanf(v.c_str(), L"%d,%d", &b, &c) >= 1) g_lastMon[k] = {b, c}; });
    }
    g_step = (int)GetPrivateProfileIntW(L"main", L"step", 5, g_iniPath.c_str());
    if (g_step != 2 && g_step != 5 && g_step != 10) g_step = 5;
    bool oldDefaults = GetPrivateProfileIntW(L"main", L"settingsver", 1, g_iniPath.c_str()) < 2;
    for (int i = 0; i < HK_COUNT; i++) {
        wchar_t b[64];
        g_hk[i] = HK_DEFAULT[i];
        if (GetPrivateProfileStringW(L"hotkeys", HK_KEYS[i], L"", b, 64, g_iniPath.c_str()) && b[0]) {
            unsigned mods = 0, vk = 0;
            if (swscanf(b, L"%u,%u", &mods, &vk) == 2) { g_hk[i].mods = mods; g_hk[i].vk = vk; }
            if (oldDefaults && mods == (MOD_CONTROL | MOD_ALT) && vk == 'L') { g_hk[i].mods = 0; g_hk[i].vk = 0; }   // drop the old built-in Ctrl+Alt+L
        }
    }
    int np = (int)GetPrivateProfileIntW(L"profiles", L"count", -1, g_iniPath.c_str());
    if (np < 0) { g_prof = {{L"Day", {}}, {L"Evening", {}}, {L"Night", {}}}; np = 3; }   // first run
    else g_prof.resize(std::min(np, MAX_PROFILES));
    for (int p = 0; p < (int)g_prof.size(); p++) {   // "monitor|brightness|contrast;..."
        wchar_t nm[128];
        if (GetPrivateProfileStringW(L"profiles", (L"name" + std::to_wstring(p)).c_str(), L"", nm, 128, g_iniPath.c_str()) && nm[0]) g_prof[p].name = nm;
        if (g_prof[p].name.empty()) g_prof[p].name = L"Profile " + std::to_wstring(p + 1);
        std::vector<wchar_t> b(8192);
        GetPrivateProfileStringW(L"profiles", (L"p" + std::to_wstring(p)).c_str(), L"", b.data(), (DWORD)b.size(), g_iniPath.c_str());
        std::wstring all = b.data(), item;
        auto take = [&](const std::wstring& it) {
            size_t a = it.rfind(L'|'); if (a == std::wstring::npos) return;
            size_t c = it.rfind(L'|', a - 1); if (c == std::wstring::npos) return;
            g_prof[p].v[it.substr(0, c)] = {_wtoi(it.substr(c + 1, a - c - 1).c_str()), _wtoi(it.substr(a + 1).c_str())};
        };
        for (wchar_t ch : all) { if (ch == L';') { take(item); item.clear(); } else item += ch; }
        if (!item.empty()) take(item);
    }
}
static void SaveSettings() {
    if (g_noSave) return;
    WritePrivateProfileStringW(L"main", L"settingsver", L"2", g_iniPath.c_str());
    std::wstring s;
    for (auto& id : g_mirror) { if (!s.empty()) s += L'|'; s += id; }
    WritePrivateProfileStringW(L"main", L"mirror", s.c_str(), g_iniPath.c_str());
    s.clear();
    for (auto& id : g_mirrorSaved) { if (!s.empty()) s += L'|'; s += id; }
    WritePrivateProfileStringW(L"main", L"mirrorsaved", s.c_str(), g_iniPath.c_str());
    WritePrivateProfileStringW(L"main", L"multi", g_multi ? L"1" : L"0", g_iniPath.c_str());
    WritePrivateProfileStringW(L"main", L"keepawake", g_keepAwake ? L"1" : L"0", g_iniPath.c_str());
    WritePrivateProfileStringW(L"main", L"tab", std::to_wstring(g_tab).c_str(), g_iniPath.c_str());
    WritePrivateProfileStringW(L"main", L"style", std::to_wstring(g_style).c_str(), g_iniPath.c_str());
    WritePrivateProfileStringW(L"main", L"accentmode", std::to_wstring(g_accentId).c_str(), g_iniPath.c_str());
    WritePrivateProfileStringW(L"main", L"accenthue", std::to_wstring(g_accentHue).c_str(), g_iniPath.c_str());
    WritePrivateProfileStringW(L"main", L"accentsat", std::to_wstring(g_accentSat).c_str(), g_iniPath.c_str());
    WritePrivateProfileStringW(L"main", L"osd", g_osd ? L"1" : L"0", g_iniPath.c_str());
    WritePrivateProfileStringW(L"main", L"restorebright", g_restoreBright ? L"1" : L"0", g_iniPath.c_str());
    {
        std::wstring d, l;
        for (auto& kv : g_delay) if (kv.second > 0) { d += kv.first + L"=" + std::to_wstring(kv.second); d.push_back(0); }
        for (auto& kv : g_lastMon) { l += kv.first + L"=" + std::to_wstring(kv.second.first) + L"," + std::to_wstring(kv.second.second); l.push_back(0); }
        d.push_back(0); l.push_back(0);
        WritePrivateProfileSectionW(L"delay", d.c_str(), g_iniPath.c_str());
        WritePrivateProfileSectionW(L"lastdisplay", l.c_str(), g_iniPath.c_str());
    }
    WritePrivateProfileStringW(L"main", L"step", std::to_wstring(g_step).c_str(), g_iniPath.c_str());
    for (int i = 0; i < HK_COUNT; i++)
        WritePrivateProfileStringW(L"hotkeys", HK_KEYS[i], (std::to_wstring(g_hk[i].mods) + L"," + std::to_wstring(g_hk[i].vk)).c_str(), g_iniPath.c_str());
    WritePrivateProfileSectionW(L"profiles", L"\0", g_iniPath.c_str());   // rewrite the whole section
    WritePrivateProfileStringW(L"profiles", L"count", std::to_wstring(g_prof.size()).c_str(), g_iniPath.c_str());
    for (int p = 0; p < (int)g_prof.size(); p++) {
        WritePrivateProfileStringW(L"profiles", (L"name" + std::to_wstring(p)).c_str(), g_prof[p].name.c_str(), g_iniPath.c_str());
        std::wstring v;
        for (auto& kv : g_prof[p].v) { if (!v.empty()) v += L';'; v += kv.first + L"|" + std::to_wstring(kv.second.first) + L"|" + std::to_wstring(kv.second.second); }
        WritePrivateProfileStringW(L"profiles", (L"p" + std::to_wstring(p)).c_str(), v.c_str(), g_iniPath.c_str());
    }
}

static const wchar_t* RUNKEY = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static bool StartupEnabled() {
    HKEY k; if (RegOpenKeyExW(HKEY_CURRENT_USER, RUNKEY, 0, KEY_READ, &k)) return false;
    bool r = RegQueryValueExW(k, APP_NAME, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS ||
             RegQueryValueExW(k, L"LiteMix", nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
    RegCloseKey(k); return r;
}
static void SetStartup(bool on, const wchar_t* exePath = nullptr) {
    HKEY k; if (RegCreateKeyExW(HKEY_CURRENT_USER, RUNKEY, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr)) return;
    if (on) {
        wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH);
        if (exePath) lstrcpynW(exe, exePath, MAX_PATH);
        std::wstring v = L"\"" + std::wstring(exe) + L"\"";
        RegSetValueExW(k, APP_NAME, 0, REG_SZ, (const BYTE*)v.c_str(), (DWORD)((v.size() + 1) * sizeof(wchar_t)));
    } else RegDeleteValueW(k, APP_NAME);
    RegDeleteValueW(k, L"LiteMix");
    RegCloseKey(k);
}
static void ApplyKeepAwake() {
    SetThreadExecutionState(g_keepAwake ? (ES_CONTINUOUS | ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED) : ES_CONTINUOUS);
}

// ---------------------------------------------------------------- audio: devices
class NotifClient final : public IMMNotificationClient {
    LONG m_ref = 1;
    void Post() { if (g_hwnd) PostMessageW(g_hwnd, WM_APP_AUDIOCHANGED, 0, 0); }
public:
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&m_ref); }
    ULONG STDMETHODCALLTYPE Release() override { ULONG r = InterlockedDecrement(&m_ref); if (!r) delete this; return r; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** p) override {
        if (riid == IID_IUnknown || riid == __uuidof(IMMNotificationClient)) { *p = this; AddRef(); return S_OK; }
        *p = nullptr; return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override { Post(); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { Post(); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { Post(); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow, ERole, LPCWSTR) override { Post(); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }
};
static NotifClient* g_notif;

static volatile LONG g_volPending = 0;
class VolCallback final : public IAudioEndpointVolumeCallback {
    LONG m_ref = 1;
public:
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&m_ref); }
    ULONG STDMETHODCALLTYPE Release() override { ULONG r = InterlockedDecrement(&m_ref); if (!r) delete this; return r; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** p) override {
        if (riid == IID_IUnknown || riid == __uuidof(IAudioEndpointVolumeCallback)) { *p = this; AddRef(); return S_OK; }
        *p = nullptr; return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE OnNotify(PAUDIO_VOLUME_NOTIFICATION_DATA) override {
        if (!InterlockedExchange(&g_volPending, 1) && g_hwnd) PostMessageW(g_hwnd, WM_APP_VOLCHANGED, 0, 0);   // coalesce bursts
        return S_OK;
    }
};
static VolCallback* g_volCb;

static std::wstring PropStr(IPropertyStore* ps, const PROPERTYKEY& k) {
    PROPVARIANT pv; PropVariantInit(&pv); std::wstring r;
    if (SUCCEEDED(ps->GetValue(k, &pv)) && pv.vt == VT_LPWSTR && pv.pwszVal) r = pv.pwszVal;
    PropVariantClear(&pv);
    return r;
}

static void FreeEndpoints(std::vector<Endpoint>& v) {
    for (auto& e : v) if (e.vol) { if (g_volCb) e.vol->UnregisterControlChangeNotify(g_volCb); e.vol->Release(); }
    v.clear();
}

static void LoadEndpoints(EDataFlow flow, std::vector<Endpoint>& v) {
    FreeEndpoints(v);
    if (!g_enum) return;
    std::wstring defId;
    IMMDevice* d = nullptr;
    if (SUCCEEDED(g_enum->GetDefaultAudioEndpoint(flow, eConsole, &d))) {
        LPWSTR id = nullptr;
        if (SUCCEEDED(d->GetId(&id))) { defId = id; CoTaskMemFree(id); }
        d->Release();
    }
    IMMDeviceCollection* c = nullptr;
    if (FAILED(g_enum->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &c))) return;
    UINT n = 0; c->GetCount(&n);
    for (UINT i = 0; i < n; i++) {
        IMMDevice* dev = nullptr;
        if (FAILED(c->Item(i, &dev))) continue;
        Endpoint e;
        LPWSTR id = nullptr;
        if (SUCCEEDED(dev->GetId(&id))) { e.id = id; CoTaskMemFree(id); }
        e.kind = flow == eCapture ? DK_MIC : DK_SPEAKER;
        IPropertyStore* ps = nullptr;
        if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &ps))) {
            e.name = PropStr(ps, PK_DeviceDesc);
            e.detail = PropStr(ps, PK_IfaceName);
            if (e.name.empty()) e.name = PropStr(ps, PK_FriendlyName);
            PROPVARIANT pv; PropVariantInit(&pv);
            if (SUCCEEDED(ps->GetValue(PK_FormFactor, &pv)) && (pv.vt == VT_UI4 || pv.vt == VT_I4)) {
                UINT ff = pv.ulVal;
                if (ff == 3 || ff == 5) e.kind = DK_HEADPHONES;          // Headphones, Headset
                else if (ff == 9) e.kind = DK_DISPLAY;                   // DigitalAudioDisplayDevice (HDMI/DP)
                else if (ff == 4) e.kind = DK_MIC;
            }
            PropVariantClear(&pv);
            ps->Release();
        }
        if (e.name.empty()) e.name = L"Audio device";
        dev->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, (void**)&e.vol);
        e.isDefault = (e.id == defId);
        if (e.vol) { e.vol->GetMasterVolumeLevelScalar(&e.level); e.vol->GetMute(&e.mute); if (g_volCb) e.vol->RegisterControlChangeNotify(g_volCb); }
        v.push_back(e);
        dev->Release();
    }
    c->Release();
}

static void RefreshEndpointLevels(std::vector<Endpoint>& v) {
    for (auto& e : v) if (e.vol) { e.vol->GetMasterVolumeLevelScalar(&e.level); e.vol->GetMute(&e.mute); }
}

static void SetDefaultDevice(const std::wstring& id) {
    IPolicyConfig* pc = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_PolicyConfigClient, nullptr, CLSCTX_ALL, IID_IPolicyConfig, (void**)&pc)) && pc) {
        pc->SetDefaultEndpoint(id.c_str(), eConsole);
        pc->SetDefaultEndpoint(id.c_str(), eMultimedia);
        pc->SetDefaultEndpoint(id.c_str(), eCommunications);
        pc->Release();
    }
}

static bool OutSelected(int i) { return g_out[i].isDefault || g_mirror.count(g_out[i].id) > 0; }
static int SelectedOutCount() { int n = 0; for (int i = 0; i < (int)g_out.size(); i++) if (OutSelected(i)) n++; return n; }
static int DefaultOut() { for (int i = 0; i < (int)g_out.size(); i++) if (g_out[i].isDefault) return i; return -1; }

// ---------------------------------------------------------------- audio: app sessions
static void FreeApps() {
    for (auto& a : g_apps) { for (auto* v : a.vols) v->Release(); for (auto* m : a.meters) m->Release(); }
    g_apps.clear();
}

static void LoadApps() {
    FreeApps();
    if (!g_enum) return;
    std::map<std::wstring, size_t> idx;
    DWORD self = GetCurrentProcessId();
    IMMDeviceCollection* c = nullptr;
    if (FAILED(g_enum->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &c))) return;
    UINT n = 0; c->GetCount(&n);
    for (UINT i = 0; i < n; i++) {
        IMMDevice* dev = nullptr;
        if (FAILED(c->Item(i, &dev))) continue;
        IAudioSessionManager2* mgr = nullptr;
        if (SUCCEEDED(dev->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, (void**)&mgr)) && mgr) {
            IAudioSessionEnumerator* se = nullptr;
            if (SUCCEEDED(mgr->GetSessionEnumerator(&se)) && se) {
                int cnt = 0; se->GetCount(&cnt);
                for (int j = 0; j < cnt; j++) {
                    IAudioSessionControl* sc = nullptr;
                    if (FAILED(se->GetSession(j, &sc)) || !sc) continue;
                    IAudioSessionControl2* sc2 = nullptr;
                    AudioSessionState st = AudioSessionStateInactive;
                    sc->GetState(&st);
                    if (st != AudioSessionStateExpired && SUCCEEDED(sc->QueryInterface(__uuidof(IAudioSessionControl2), (void**)&sc2)) && sc2) {
                        DWORD pid = 0; sc2->GetProcessId(&pid);
                        bool sys = sc2->IsSystemSoundsSession() == S_OK;
                        if (pid != self || sys) {
                            std::wstring key, name, path;
                            if (sys) { key = L"*system"; name = L"System sounds"; }
                            else {
                                path = ProcPath(pid);
                                if (!path.empty()) { key = ToLower(path); name = GetProcInfo(path).name; }
                                else {
                                    LPWSTR dn = nullptr;
                                    if (SUCCEEDED(sc2->GetDisplayName(&dn)) && dn) { name = dn; CoTaskMemFree(dn); }
                                    if (name.empty() || name[0] == L'@') name = L"App (PID " + std::to_wstring(pid) + L")";
                                    key = L"pid:" + std::to_wstring(pid);
                                }
                            }
                            ISimpleAudioVolume* sv = nullptr;
                            if (SUCCEEDED(sc->QueryInterface(__uuidof(ISimpleAudioVolume), (void**)&sv)) && sv) {
                                auto it = idx.find(key);
                                if (it == idx.end()) {
                                    AppGroup g; g.key = key; g.name = name; g.path = path; g.system = sys;
                                    sv->GetMasterVolume(&g.level); sv->GetMute(&g.mute);
                                    idx[key] = g_apps.size();
                                    g_apps.push_back(g);
                                    it = idx.find(key);
                                }
                                g_apps[it->second].vols.push_back(sv);
                                Meter* mi = nullptr;
                                if (SUCCEEDED(sc->QueryInterface(IID_Meter, (void**)&mi)) && mi)
                                    g_apps[it->second].meters.push_back(mi);
                            }
                        }
                        sc2->Release();
                    }
                    sc->Release();
                }
                se->Release();
            }
            mgr->Release();
        }
        dev->Release();
    }
    c->Release();
    // stable alphabetical order (rows never jump while you adjust them); system sounds last
    std::stable_sort(g_apps.begin(), g_apps.end(), [](const AppGroup& a, const AppGroup& b) {
        if (a.system != b.system) return b.system;
        return lstrcmpiW(a.name.c_str(), b.name.c_str()) < 0;
    });
}

static bool AppPlaying(const AppGroup& a) {
    auto it = g_live.find(a.key);
    return it != g_live.end() && GetTickCount() - it->second.lastActive < 800;
}

static bool UpdatePeaks() {   // returns true if anything is (or just was) playing
    bool any = false;
    DWORD now = GetTickCount();
    for (auto& a : g_apps) {
        float pk = 0;
        for (auto* m : a.meters) { float p = 0; if (SUCCEEDED(m->GetPeakValue(&p))) pk = std::max(pk, p); }
        a.peak = pk;
        AppLive& l = g_live[a.key];
        l.smooth = l.smooth * 0.55f + pk * 0.45f;
        if (pk > 0.002f && !a.mute) l.lastActive = now;
        if (now - l.lastActive < 800) any = true;
    }
    return any;
}

// ---------------------------------------------------------------- audio: mirroring (loopback of default output -> extra outputs)
struct RouterTarget { std::wstring id; int delayMs; };
struct RouterCtx { std::wstring src; std::vector<RouterTarget> targets; volatile LONG stop = 0; HANDLE thread = nullptr;
                   volatile LONG calOnly = -1; };   // auto sync: -1 normal, -2 all copies silent, k = only target k plays
static RouterCtx* g_router;

static DWORD WINAPI RouterProc(LPVOID p) {
    RouterCtx* ctx = (RouterCtx*)p;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IMMDeviceEnumerator* en = nullptr;
    IMMDevice* sdev = nullptr;
    IAudioClient* cap = nullptr;
    IAudioCaptureClient* cc = nullptr;
    WAVEFORMATEX* wf = nullptr;
    struct Out { IAudioClient* ac = nullptr; IAudioRenderClient* rc = nullptr; UINT32 buf = 0; bool dead = false;
                 std::vector<BYTE> q; size_t head = 0; UINT32 delayFrames = 0; int ti = 0; };   // q = software delay line
    std::vector<Out> outs;
    do {
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&en))) break;
        if (FAILED(en->GetDevice(ctx->src.c_str(), &sdev))) break;
        if (FAILED(sdev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&cap))) break;
        if (FAILED(cap->GetMixFormat(&wf))) break;
        if (FAILED(cap->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, 2000000, 0, wf, nullptr))) break;
        if (FAILED(cap->GetService(__uuidof(IAudioCaptureClient), (void**)&cc))) break;
        const UINT32 rate = wf->nSamplesPerSec, fb = wf->nBlockAlign;
        const UINT32 preFrames = rate * 40 / 1000, hiFrames = rate * 120 / 1000;
        for (int ti = 0; ti < (int)ctx->targets.size(); ti++) {
            auto& t = ctx->targets[ti];
            IMMDevice* d = nullptr;
            if (FAILED(en->GetDevice(t.id.c_str(), &d))) continue;
            Out o; o.ti = ti;
            o.delayFrames = (UINT32)((UINT64)rate * std::max(0, std::min(1000, t.delayMs)) / 1000);
            o.q.assign((size_t)o.delayFrames * fb, 0);   // start the delay line full of silence
            if (SUCCEEDED(d->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&o.ac)) &&
                SUCCEEDED(o.ac->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                           2000000, 0, wf, nullptr)) &&
                SUCCEEDED(o.ac->GetBufferSize(&o.buf)) &&
                SUCCEEDED(o.ac->GetService(__uuidof(IAudioRenderClient), (void**)&o.rc))) {
                BYTE* dst = nullptr;
                UINT32 pre = std::min(preFrames, o.buf);
                if (SUCCEEDED(o.rc->GetBuffer(pre, &dst))) o.rc->ReleaseBuffer(pre, AUDCLNT_BUFFERFLAGS_SILENT);
                o.ac->Start();
                outs.push_back(o);
            } else { if (o.rc) o.rc->Release(); if (o.ac) o.ac->Release(); }
            d->Release();
        }
        if (outs.empty()) break;
        if (FAILED(cap->Start())) break;
        const UINT32 driftFrames = rate * 150 / 1000;
        while (!ctx->stop) {
            Sleep(4);
            UINT32 pk = 0; HRESULT hr; bool got = false;
            // 1) capture: append every packet to each output's delay line
            while (SUCCEEDED(hr = cc->GetNextPacketSize(&pk)) && pk > 0) {
                BYTE* data = nullptr; UINT32 fr = 0; DWORD fl = 0;
                if (FAILED(hr = cc->GetBuffer(&data, &fr, &fl, nullptr, nullptr))) break;
                got = true;
                size_t bytes = (size_t)fr * fb;
                LONG only = ctx->calOnly;
                for (auto& o : outs) {
                    if (o.dead) continue;
                    size_t at = o.q.size();
                    o.q.resize(at + bytes);
                    if ((fl & AUDCLNT_BUFFERFLAGS_SILENT) || (only != -1 && only != o.ti)) memset(&o.q[at], 0, bytes); else memcpy(&o.q[at], data, bytes);
                }
                cc->ReleaseBuffer(fr);
            }
            if (FAILED(hr)) break;
            // 2) render: feed each output from its delay line
            for (auto& o : outs) {
                if (o.dead) continue;
                UINT32 pad = 0;
                if (FAILED(o.ac->GetCurrentPadding(&pad))) { o.dead = true; continue; }
                UINT32 queued = (UINT32)((o.q.size() - o.head) / fb);
                if (queued > o.delayFrames + driftFrames) {   // this device runs slow: drop the oldest audio to stay in sync
                    o.head += (size_t)(queued - o.delayFrames - preFrames) * fb;
                    queued = (UINT32)((o.q.size() - o.head) / fb);
                }
                UINT32 room = pad < hiFrames ? std::min(o.buf - pad, hiFrames - pad) : 0;
                UINT32 n = std::min(room, queued);
                if (n) {
                    BYTE* dst = nullptr;
                    if (SUCCEEDED(o.rc->GetBuffer(n, &dst))) { memcpy(dst, &o.q[o.head], (size_t)n * fb); o.rc->ReleaseBuffer(n, 0); o.head += (size_t)n * fb; }
                } else if (!got && queued == 0 && pad < preFrames / 2) {   // idle: keep a small silent cushion
                    UINT32 nfr = std::min(preFrames - pad, o.buf - pad);
                    BYTE* dst = nullptr;
                    if (nfr && SUCCEEDED(o.rc->GetBuffer(nfr, &dst))) o.rc->ReleaseBuffer(nfr, AUDCLNT_BUFFERFLAGS_SILENT);
                }
                if (o.head > (1u << 20) && o.head * 2 > o.q.size()) { o.q.erase(o.q.begin(), o.q.begin() + o.head); o.head = 0; }
            }
        }
    } while (0);
    for (auto& o : outs) { if (o.ac) o.ac->Stop(); if (o.rc) o.rc->Release(); if (o.ac) o.ac->Release(); }
    if (cap) cap->Stop();
    if (cc) cc->Release();
    if (cap) cap->Release();
    if (wf) CoTaskMemFree(wf);
    if (sdev) sdev->Release();
    if (en) en->Release();
    CoUninitialize();
    return 0;
}

static void StopRouter() {
    if (!g_router) return;
    InterlockedExchange(&g_router->stop, 1);
    if (g_router->thread) { WaitForSingleObject(g_router->thread, 3000); CloseHandle(g_router->thread); }
    delete g_router;
    g_router = nullptr;
}

static volatile LONG g_calibrating = 0;
static bool g_routerPending = false;
static void RestartRouter() {
    if (g_calibrating) { g_routerPending = true; return; }   // auto sync is using the router; restart when it finishes
    StopRouter();
    int di = DefaultOut();
    if (di < 0) return;
    std::vector<RouterTarget> targets;
    for (auto& e : g_out) if (!e.isDefault && g_mirror.count(e.id)) { auto it = g_delay.find(e.id); targets.push_back({e.id, it == g_delay.end() ? 0 : it->second}); }
    if (targets.empty()) return;
    g_router = new RouterCtx;
    g_router->src = g_out[di].id;
    g_router->targets = targets;
    g_router->thread = CreateThread(nullptr, 0, RouterProc, g_router, 0, nullptr);
    if (g_router->thread) SetThreadPriority(g_router->thread, THREAD_PRIORITY_TIME_CRITICAL);
}

// ---------------------------------------------------------------- auto sync (measure each copy with the microphone)
// Two chirps go out through the main output. During the first every copy is silent; during the second only
// copy k plays. The mic hears main alone, then main + copy k: the gap between them is copy k's offset.
struct CalTarget { std::wstring id, name; int delayMs; };
struct CalResult { std::vector<std::pair<std::wstring, int>> delays; std::wstring status, swapTo, swapName; };
static int g_calPass = 0;   // 1 = second run after Knob made the slowest speaker the main output
static std::wstring g_calStatus;
static const UINT32 CAL_RATE = 48000;

static const GUID KNOB_IID_IAudioClient2 = {0x726778cd, 0xf60a, 0x4eda, {0x82, 0xde, 0xe4, 0x76, 0x10, 0xcd, 0x78, 0xaa}};
static std::vector<float> CalChirp() {   // 50 ms sweep 1 -> 6 kHz, Hann window
    const int n = CAL_RATE / 20;
    std::vector<float> c(n);
    const double f0 = 1000, f1 = 6000, T = n / (double)CAL_RATE;
    for (int i = 0; i < n; i++) {
        double t = i / (double)CAL_RATE;
        double ph = 2 * 3.14159265358979 * (f0 * t + (f1 - f0) * t * t / (2 * T));
        double w = 0.5 - 0.5 * cos(2 * 3.14159265358979 * i / (n - 1));
        c[i] = (float)(sin(ph) * w);
    }
    return c;
}
static void CalFormat(WAVEFORMATEX& f) {
    f = {}; f.wFormatTag = WAVE_FORMAT_IEEE_FLOAT; f.nChannels = 1; f.nSamplesPerSec = CAL_RATE;
    f.wBitsPerSample = 32; f.nBlockAlign = 4; f.nAvgBytesPerSec = CAL_RATE * 4;
}
// one round: returns copy-minus-main offset in ms; flags: 0 ok, 1 copy not heard, 2 nothing heard, 3 device error
static int CalRound(IMMDevice* outDev, IMMDevice* micDev, LONG target, const std::vector<float>& chirp, double& offsetMs) {
    WAVEFORMATEX f; CalFormat(f);
    IAudioClient *rac = nullptr, *cac = nullptr; IAudioRenderClient* rc = nullptr; IAudioCaptureClient* cc = nullptr;
    std::vector<float> rec;
    int result = 3;
    const UINT32 gap = CAL_RATE * 8 / 10, lead = CAL_RATE / 10, total = lead + gap + CAL_RATE * 9 / 10;
    do {
        if (FAILED(outDev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&rac))) break;
        if (FAILED(micDev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&cac))) break;
        IAudioClient2* c2 = nullptr;   // ask for the raw mic signal (no noise suppression), if the driver allows it
        if (SUCCEEDED(cac->QueryInterface(KNOB_IID_IAudioClient2, (void**)&c2))) {
            AudioClientProperties pr = {}; pr.cbSize = sizeof(pr); pr.eCategory = AudioCategory_Other; pr.Options = AUDCLNT_STREAMOPTIONS_RAW;
            c2->SetClientProperties(&pr); c2->Release();
        }
        const DWORD fl = AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
        if (FAILED(rac->Initialize(AUDCLNT_SHAREMODE_SHARED, fl, 30000000, 0, &f, nullptr))) break;
        if (FAILED(cac->Initialize(AUDCLNT_SHAREMODE_SHARED, fl, 10000000, 0, &f, nullptr))) break;
        if (FAILED(rac->GetService(__uuidof(IAudioRenderClient), (void**)&rc))) break;
        if (FAILED(cac->GetService(__uuidof(IAudioCaptureClient), (void**)&cc))) break;
        UINT32 bufFrames = 0; rac->GetBufferSize(&bufFrames);
        UINT32 n = std::min(total, bufFrames);
        BYTE* dst = nullptr;
        if (FAILED(rc->GetBuffer(n, &dst))) break;
        float* out = (float*)dst;
        memset(out, 0, (size_t)n * 4);
        for (size_t i = 0; i < chirp.size(); i++) {
            if (lead + i < n) out[lead + i] = 0.6f * chirp[i];
            if (lead + gap + i < n) out[lead + gap + i] = 0.6f * chirp[i];
        }
        rc->ReleaseBuffer(n, 0);
        if (g_router) InterlockedExchange(&g_router->calOnly, -2);
        cac->Start(); rac->Start();
        DWORD t0 = GetTickCount();
        bool switched = false;
        while (GetTickCount() - t0 < 2600) {
            Sleep(5);
            if (!switched && GetTickCount() - t0 > 500) { if (g_router) InterlockedExchange(&g_router->calOnly, target); switched = true; }
            UINT32 pk = 0;
            while (SUCCEEDED(cc->GetNextPacketSize(&pk)) && pk) {
                BYTE* d = nullptr; UINT32 fr = 0; DWORD bf = 0;
                if (FAILED(cc->GetBuffer(&d, &fr, &bf, nullptr, nullptr))) break;
                size_t at = rec.size(); rec.resize(at + fr);
                if (bf & AUDCLNT_BUFFERFLAGS_SILENT) memset(&rec[at], 0, (size_t)fr * 4); else memcpy(&rec[at], d, (size_t)fr * 4);
                cc->ReleaseBuffer(fr);
            }
        }
        rac->Stop(); cac->Stop();
        result = 2;
    } while (0);
    if (g_router) InterlockedExchange(&g_router->calOnly, -1);
    if (cc) cc->Release();
    if (rc) rc->Release();
    if (cac) cac->Release();
    if (rac) rac->Release();
    if (result == 3) return 3;
    // matched filter
    const size_t m = chirp.size();
    if (rec.size() < m + CAL_RATE) return 2;
    std::vector<float> corr(rec.size() - m);
    for (size_t i = 0; i < corr.size(); i++) {
        float acc = 0; const float* r = &rec[i];
        for (size_t j = 0; j < m; j++) acc += r[j] * chirp[j];
        corr[i] = fabsf(acc);
    }
    auto peakIn = [&](size_t a, size_t b, size_t skipA = 0, size_t skipB = 0) -> std::pair<size_t, float> {
        size_t bi = a; float bv = -1;
        for (size_t i = a; i < b && i < corr.size(); i++) { if (i >= skipA && i < skipB) continue; if (corr[i] > bv) { bv = corr[i]; bi = i; } }
        return {bi, bv};
    };
    const size_t split = CAL_RATE * 3 / 4, guard = CAL_RATE / 100;   // 750 ms, 10 ms
    double noise = 0; for (size_t i = 0; i < std::min(corr.size(), (size_t)(CAL_RATE / 20)); i++) noise += corr[i]; noise /= (CAL_RATE / 20);
    auto p1 = peakIn(0, split);
    if (p1.second < noise * 8 + 1e-4f) return 2;   // mic didn't hear the main output
    size_t expect = p1.first + gap;
    auto pa = peakIn(split, corr.size());
    auto pb = peakIn(split, corr.size(), pa.first > guard ? pa.first - guard : 0, pa.first + guard);
    size_t mainAt, copyAt; float copyV;
    auto isNear = [&](size_t x) { return (x > expect ? x - expect : expect - x) < guard; };
    if (isNear(pa.first)) { mainAt = pa.first; copyAt = pb.first; copyV = pb.second; }
    else if (isNear(pb.first)) { mainAt = pb.first; copyAt = pa.first; copyV = pa.second; }
    else return 1;
    if (copyV < p1.second * 0.2f) {
        // no second chirp: either the copy lands on top of main (already in sync) or it wasn't heard
        if (corr[mainAt] > p1.second * 1.35f) { offsetMs = 0; return 0; }
        return 1;
    }
    offsetMs = ((double)copyAt - (double)mainAt) * 1000.0 / CAL_RATE;
    return 0;
}
static DWORD WINAPI CalibrateProc(LPVOID p) {
    std::vector<CalTarget>* ts = (std::vector<CalTarget>*)p;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    CalResult* res = new CalResult;
    IMMDeviceEnumerator* en = nullptr; IMMDevice *outDev = nullptr, *micDev = nullptr;
    std::vector<float> chirp = CalChirp();
    std::wstring notes;
    do {
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&en))) { res->status = L"Couldn't open audio devices"; break; }
        if (FAILED(en->GetDefaultAudioEndpoint(eRender, eConsole, &outDev))) { res->status = L"No main output"; break; }
        if (FAILED(en->GetDefaultAudioEndpoint(eCapture, eConsole, &micDev))) { res->status = L"No microphone found — auto sync needs one"; break; }
        int ok = 0;
        int worst = 0; std::wstring worstId, worstName;   // most negative delay = slowest speaker
        for (int k = 0; k < (int)ts->size(); k++) {
            CalTarget& t = (*ts)[k];
            double off = 0; int r = CalRound(outDev, micDev, k, chirp, off);
            if (r == 0) {
                double oSecond = 0;   // measure again and average when the two agree
                if (CalRound(outDev, micDev, k, chirp, oSecond) == 0 && fabs(oSecond - off) < 15) off = (off + oSecond) / 2;
                int nd = (int)lround((t.delayMs - off) / 10.0) * 10;
                if (nd < worst) { worst = nd; worstId = t.id; worstName = t.name; }
                if (nd < -10 && g_calPass) notes += t.name + L" is still slower than the main output. ";
                nd = std::max(0, std::min(500, nd));
                res->delays.push_back({t.id, nd});
                ok++;
            } else if (r == 2) { res->status = L"The microphone couldn't hear the main output — turn the volume up"; break; }
            else if (r == 3) notes += L"Couldn't use " + t.name + L". ";
            else notes += L"Couldn't hear " + t.name + L". ";
        }
        if (res->status.empty() && worst < -10 && !g_calPass) {   // let Knob make the slowest speaker the main output, then sync again
            res->swapTo = worstId; res->swapName = worstName; res->delays.clear();
            res->status = L"Making " + worstName + L" the main output, then syncing again\u2026";
        }
        if (res->status.empty()) res->status = ok ? (std::wstring(g_calPass ? L"Made the slowest speaker the main output. " : L"") + L"Synced " + std::to_wstring(ok) + (ok == 1 ? L" output. " : L" outputs. ") + notes) : notes;
    } while (0);
    if (micDev) micDev->Release();
    if (outDev) outDev->Release();
    if (en) en->Release();
    CoUninitialize();
    delete ts;
    if (!g_hwnd || !PostMessageW(g_hwnd, WM_APP_CALDONE, 0, (LPARAM)res)) delete res;
    return 0;
}
static void StartAutoSync() {
    if (g_calibrating) return;
    if (!g_router) { g_calStatus = L"Turn on Play together in the Sound tab and pick two or more outputs first"; return; }
    auto* ts = new std::vector<CalTarget>;
    for (auto& t : g_router->targets) {
        std::wstring name = t.id;
        for (auto& e : g_out) if (e.id == t.id) name = e.name + (e.detail.empty() ? L"" : L" (" + e.detail + L")");
        ts->push_back({t.id, name, t.delayMs});
    }
    InterlockedExchange(&g_calibrating, 1);
    g_calStatus = L"Listening… keep the room quiet";
    HANDLE h = CreateThread(nullptr, 0, CalibrateProc, ts, 0, nullptr);
    if (h) CloseHandle(h); else { delete ts; InterlockedExchange(&g_calibrating, 0); g_calStatus = L"Couldn't start"; }
}

// ---------------------------------------------------------------- display worker (DDC/CI + WMI + orientation)
struct Mon {
    std::wstring name, detail, gdi; HANDLE phys = nullptr; bool wmi = false, internal = false, ok = false, hasCon = false;
    DWORD mn = 0, mx = 100, cur = 50, cmn = 0, cmx = 100, ccur = 50; int orient = 0; RECT rc{};
    bool vcpB = false, vcpC = false;   // fall back to raw VCP codes 0x10 / 0x12 when the high-level API fails
};
static std::vector<Mon> w_mons;
static CRITICAL_SECTION g_bcs;
static HANDLE g_bEvent;
static bool g_bEnumReq = false, g_bReadReq = false, g_bQuit = false;
static std::map<int, int> g_bSets;   // key = index*2 + (0 brightness | 1 contrast) -> pct
static int g_bSetGen = 0;
static int w_gen = 0;
static IWbemServices* g_wmi;

static bool WmiConnect() {
    if (g_wmi) return true;
    IWbemLocator* loc = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER, IID_IWbemLocator, (void**)&loc)) || !loc) return false;
    BSTR ns = SysAllocString(L"ROOT\\WMI");
    HRESULT hr = loc->ConnectServer(ns, nullptr, nullptr, nullptr, 0, nullptr, nullptr, &g_wmi);
    SysFreeString(ns);
    loc->Release();
    if (FAILED(hr)) { g_wmi = nullptr; return false; }
    CoSetProxyBlanket(g_wmi, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr, RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);
    return true;
}
static IWbemClassObject* WmiFirst(const wchar_t* query) {
    if (!WmiConnect()) return nullptr;
    IEnumWbemClassObject* en = nullptr;
    BSTR wql = SysAllocString(L"WQL"), q = SysAllocString(query);
    HRESULT hr = g_wmi->ExecQuery(wql, q, WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr, &en);
    SysFreeString(wql); SysFreeString(q);
    if (FAILED(hr) || !en) return nullptr;
    IWbemClassObject* o = nullptr; ULONG n = 0;
    if (en->Next(3000, 1, &o, &n) != S_OK || !n) o = nullptr;
    en->Release();
    return o;
}
static bool WmiGet(int& out) {
    IWbemClassObject* o = WmiFirst(L"SELECT CurrentBrightness FROM WmiMonitorBrightness WHERE Active=TRUE");
    if (!o) return false;
    VARIANT v; VariantInit(&v);
    bool ok = false;
    if (SUCCEEDED(o->Get(L"CurrentBrightness", 0, &v, nullptr, nullptr))) {
        if (v.vt == VT_UI1) { out = v.bVal; ok = true; }
        else if (v.vt == VT_I4) { out = v.lVal; ok = true; }
    }
    VariantClear(&v);
    o->Release();
    return ok;
}
static bool WmiSet(int pct) {
    IWbemClassObject* o = WmiFirst(L"SELECT * FROM WmiMonitorBrightnessMethods WHERE Active=TRUE");
    if (!o) return false;
    bool ok = false;
    VARIANT path; VariantInit(&path);
    IWbemClassObject *cls = nullptr, *inDef = nullptr, *in = nullptr;
    BSTR clsName = SysAllocString(L"WmiMonitorBrightnessMethods"), meth = SysAllocString(L"WmiSetBrightness");
    if (SUCCEEDED(o->Get(L"__PATH", 0, &path, nullptr, nullptr)) && path.vt == VT_BSTR &&
        SUCCEEDED(g_wmi->GetObject(clsName, 0, nullptr, &cls, nullptr)) &&
        SUCCEEDED(cls->GetMethod(L"WmiSetBrightness", 0, &inDef, nullptr)) && inDef &&
        SUCCEEDED(inDef->SpawnInstance(0, &in))) {
        VARIANT t; VariantInit(&t); t.vt = VT_I4; t.lVal = 0;
        VARIANT b; VariantInit(&b); b.vt = VT_UI1; b.bVal = (BYTE)pct;
        in->Put(L"Timeout", 0, &t, 0);
        in->Put(L"Brightness", 0, &b, 0);
        ok = SUCCEEDED(g_wmi->ExecMethod(path.bstrVal, meth, 0, nullptr, in, nullptr, nullptr));
    }
    SysFreeString(clsName); SysFreeString(meth);
    VariantClear(&path);
    if (in) in->Release();
    if (inDef) inDef->Release();
    if (cls) cls->Release();
    o->Release();
    return ok;
}

struct DcName { std::wstring friendly, tech; bool internal; };
static std::map<std::wstring, DcName> QueryDisplayNames() {
    std::map<std::wstring, DcName> r;
    UINT32 np = 0, nm = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &np, &nm) != ERROR_SUCCESS) return r;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(np);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(nm);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &np, paths.data(), &nm, modes.data(), nullptr) != ERROR_SUCCESS) return r;
    for (UINT32 i = 0; i < np; i++) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME src = {};
        src.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME; src.header.size = sizeof(src);
        src.header.adapterId = paths[i].sourceInfo.adapterId; src.header.id = paths[i].sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&src.header) != ERROR_SUCCESS) continue;
        DISPLAYCONFIG_TARGET_DEVICE_NAME tgt = {};
        tgt.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME; tgt.header.size = sizeof(tgt);
        tgt.header.adapterId = paths[i].targetInfo.adapterId; tgt.header.id = paths[i].targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&tgt.header) != ERROR_SUCCESS) continue;
        auto t = tgt.outputTechnology;
        bool internal = t == DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INTERNAL || t == DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DISPLAYPORT_EMBEDDED ||
                        t == DISPLAYCONFIG_OUTPUT_TECHNOLOGY_UDI_EMBEDDED || t == DISPLAYCONFIG_OUTPUT_TECHNOLOGY_LVDS;
        const wchar_t* tech = L"";
        switch (t) {
        case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_HDMI: tech = L"HDMI"; break;
        case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DISPLAYPORT_EXTERNAL: tech = L"DisplayPort"; break;
        case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DVI: tech = L"DVI"; break;
        case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_HD15: tech = L"VGA"; break;
        case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_MIRACAST: tech = L"Wireless"; break;
        default: if (internal) tech = L"Laptop panel"; break;
        }
        r[src.viewGdiDeviceName] = {tgt.monitorFriendlyDeviceName, tech, internal};
    }
    return r;
}

static BOOL CALLBACK EnumMonProc(HMONITOR h, HDC, LPRECT, LPARAM l) { ((std::vector<HMONITOR>*)l)->push_back(h); return TRUE; }

static void WorkerEnum() {
    for (auto& m : w_mons) if (m.phys) DestroyPhysicalMonitor(m.phys);
    w_mons.clear();
    auto names = QueryDisplayNames();
    std::vector<HMONITOR> hms;
    EnumDisplayMonitors(nullptr, nullptr, EnumMonProc, (LPARAM)&hms);
    for (HMONITOR h : hms) {
        MONITORINFOEXW mi = {}; mi.cbSize = sizeof(mi);
        GetMonitorInfoW(h, &mi);
        Mon m;
        m.gdi = mi.szDevice; m.rc = mi.rcMonitor;
        auto it = names.find(mi.szDevice);
        if (it != names.end()) { m.name = it->second.friendly; m.internal = it->second.internal; m.detail = it->second.tech; }
        DEVMODEW dm = {}; dm.dmSize = sizeof(dm);
        if (EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm)) m.orient = (int)dm.dmDisplayOrientation;
        DWORD np = 0;
        if (GetNumberOfPhysicalMonitorsFromHMONITOR(h, &np) && np) {
            std::vector<PHYSICAL_MONITOR> pm(np);
            if (GetPhysicalMonitorsFromHMONITOR(h, np, pm.data())) {
                m.phys = pm[0].hPhysicalMonitor;
                for (DWORD k = 1; k < np; k++) DestroyPhysicalMonitor(pm[k].hPhysicalMonitor);
                if (m.name.empty() && !m.internal) m.name = pm[0].szPhysicalMonitorDescription;
                if (!m.internal) {
                    DWORD a = 0, b = 0, c = 0;
                    bool ok = GetMonitorBrightness(m.phys, &a, &b, &c);
                    if (!ok) { Sleep(60); ok = GetMonitorBrightness(m.phys, &a, &b, &c); }
                    if (ok && c > a) { m.mn = a; m.cur = b; m.mx = c; m.ok = true; }
                    if (!m.ok) {   // many monitors only answer raw VCP requests (what PowerToys uses)
                        DWORD cur = 0, mx = 0;
                        if (GetVCPFeatureAndVCPFeatureReply(m.phys, 0x10, nullptr, &cur, &mx) && mx > 0) { m.mn = 0; m.cur = cur; m.mx = mx; m.ok = true; m.vcpB = true; }
                    }
                    {
                        DWORD ca = 0, cb = 0, ccx = 0;
                        if (GetMonitorContrast(m.phys, &ca, &cb, &ccx) && ccx > ca) { m.cmn = ca; m.ccur = cb; m.cmx = ccx; m.hasCon = true; }
                        else {
                            Sleep(40);
                            DWORD cur = 0, mx = 0;
                            if (GetVCPFeatureAndVCPFeatureReply(m.phys, 0x12, nullptr, &cur, &mx) && mx > 0) { m.cmn = 0; m.ccur = cur; m.cmx = mx; m.hasCon = true; m.vcpC = true; }
                        }
                    }
                }
            }
        }
        if (m.name.empty()) m.name = m.internal ? L"Built-in display" : L"Display";
        w_mons.push_back(m);
    }
    int cand = -1;
    for (int i = 0; i < (int)w_mons.size(); i++) if (!w_mons[i].ok && w_mons[i].internal) { cand = i; break; }
    if (cand < 0) for (int i = 0; i < (int)w_mons.size(); i++) if (!w_mons[i].ok) { cand = i; break; }
    int v = 0;
    if (cand >= 0 && WmiGet(v)) {
        auto& m = w_mons[cand];
        m.wmi = true; m.ok = true; m.mn = 0; m.mx = 100; m.cur = v;
        if (m.name == L"Display") m.name = L"Built-in display";
        if (m.detail.empty()) m.detail = L"Laptop panel";
    }
}
static void WorkerRead() {
    for (auto& m : w_mons) {
        if (!m.ok) continue;
        if (m.wmi) { int v; if (WmiGet(v)) m.cur = v; }
        else if (m.phys) {
            DWORD a, b, c;
            if (m.vcpB) { DWORD cur, mx; if (GetVCPFeatureAndVCPFeatureReply(m.phys, 0x10, nullptr, &cur, &mx) && mx) { m.cur = cur; m.mx = mx; } }
            else if (GetMonitorBrightness(m.phys, &a, &b, &c) && c > a) { m.mn = a; m.cur = b; m.mx = c; }
            if (m.hasCon) {
                if (m.vcpC) { DWORD cur, mx; if (GetVCPFeatureAndVCPFeatureReply(m.phys, 0x12, nullptr, &cur, &mx) && mx) { m.ccur = cur; m.cmx = mx; } }
                else if (GetMonitorContrast(m.phys, &a, &b, &c) && c > a) { m.cmn = a; m.ccur = b; m.cmx = c; }
            }
        }
    }
}
static int Pct(DWORD mn, DWORD mx, DWORD cur) { return mx > mn ? (int)lround((double)(cur - mn) * 100.0 / (mx - mn)) : 0; }
static void WorkerPost(bool fromEnum) {
    MonMsg* msg = new MonMsg;
    msg->gen = w_gen; msg->fromEnum = fromEnum;
    for (auto& m : w_mons) {
        MonUI u; u.name = m.name; u.detail = m.detail; u.gdi = m.gdi; u.ok = m.ok; u.internal = m.internal;
        u.hasCon = m.hasCon; u.orient = m.orient; u.rc = m.rc;
        u.pct = Pct(m.mn, m.mx, m.cur); u.con = Pct(m.cmn, m.cmx, m.ccur);
        msg->mons.push_back(u);
    }
    if (!g_hwnd || !PostMessageW(g_hwnd, WM_APP_MONITORS, 0, (LPARAM)msg)) delete msg;
}
static DWORD WINAPI BrightProc(LPVOID) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    for (;;) {
        WaitForSingleObject(g_bEvent, INFINITE);
        EnterCriticalSection(&g_bcs);
        bool en = g_bEnumReq, rd = g_bReadReq, quit = g_bQuit;
        std::map<int, int> sets; sets.swap(g_bSets);
        int setGen = g_bSetGen;
        g_bEnumReq = g_bReadReq = false;
        LeaveCriticalSection(&g_bcs);
        if (quit) break;
        if (en) { WorkerEnum(); w_gen++; WorkerPost(true); }
        if (setGen == w_gen) {
            for (auto& kv : sets) {
                int idx = kv.first / 2, type = kv.first % 2;
                if (idx < 0 || idx >= (int)w_mons.size()) continue;
                auto& m = w_mons[idx];
                if (!m.ok) continue;
                int pct = std::max(0, std::min(100, kv.second));
                if (type == 0) {
                    if (m.wmi) WmiSet(pct);
                    else if (m.phys) {
                        DWORD val = m.mn + (DWORD)lround((double)pct * (m.mx - m.mn) / 100.0);
                        if (m.vcpB ? SetVCPFeature(m.phys, 0x10, val) : SetMonitorBrightness(m.phys, val)) m.cur = val;
                    }
                } else if (m.hasCon && m.phys) {
                    DWORD val = m.cmn + (DWORD)lround((double)pct * (m.cmx - m.cmn) / 100.0);
                    if (m.vcpC ? SetVCPFeature(m.phys, 0x12, val) : SetMonitorContrast(m.phys, val)) m.ccur = val;
                }
            }
        }
        if (rd && !en) { WorkerRead(); WorkerPost(false); }
    }
    for (auto& m : w_mons) if (m.phys) DestroyPhysicalMonitor(m.phys);
    if (g_wmi) g_wmi->Release();
    CoUninitialize();
    return 0;
}
static void BrightRequest(bool enumReq, bool readReq) {
    EnterCriticalSection(&g_bcs);
    if (enumReq) g_bEnumReq = true;
    if (readReq) g_bReadReq = true;
    LeaveCriticalSection(&g_bcs);
    SetEvent(g_bEvent);
}
static void MonSet(int idx, int type, int pct) {
    if (idx >= 0 && idx < (int)g_mons.size()) {
        const MonUI& m = g_mons[idx];
        auto& lm = g_lastMon[m.name];
        lm.first = type == 0 ? pct : m.pct;
        lm.second = type == 1 ? pct : (m.hasCon ? m.con : -1);
    }
    EnterCriticalSection(&g_bcs);
    g_bSets[idx * 2 + type] = pct;
    g_bSetGen = g_monGen;
    LeaveCriticalSection(&g_bcs);
    SetEvent(g_bEvent);
}

static void SetOrientation(int idx, int orient) {
    if (idx < 0 || idx >= (int)g_mons.size()) return;
    DEVMODEW dm = {}; dm.dmSize = sizeof(dm);
    if (!EnumDisplaySettingsW(g_mons[idx].gdi.c_str(), ENUM_CURRENT_SETTINGS, &dm)) return;
    bool wasPortrait = (dm.dmDisplayOrientation % 2) == 1, nowPortrait = (orient % 2) == 1;
    if (wasPortrait != nowPortrait) std::swap(dm.dmPelsWidth, dm.dmPelsHeight);
    dm.dmDisplayOrientation = orient;
    dm.dmFields = DM_DISPLAYORIENTATION | DM_PELSWIDTH | DM_PELSHEIGHT;
    if (ChangeDisplaySettingsExW(g_mons[idx].gdi.c_str(), &dm, nullptr, CDS_UPDATEREGISTRY, nullptr) == DISP_CHANGE_SUCCESSFUL)
        g_mons[idx].orient = orient;
}

// ---------------------------------------------------------------- theme + fonts
struct Theme {
    bool dark = true, taskbarLight = false, glass = false;   // glass = Glass style is active (Windows 11 acrylic available)
    Color surface, t1, t2, t3, track, thumb, subtle, subtle2, subtle3, accent, onAccent;
    Color tabsBg, tabSel, badge, master, hover, ddBtn, ddBtnHover, valBg, rail, popup;
} T;

static Color WithAlpha(Color c, int a) { return Color((BYTE)a, c.GetR(), c.GetG(), c.GetB()); }
static Color Hex(DWORD rgb) { return Color(255, (rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255); }

struct AccentPreset { const wchar_t* name; DWORD dark, light; };
static const AccentPreset ACCENTS[7] = {
    {L"Windows accent", 0, 0}, {L"Teal", 0x4cc2c4, 0x007a7d}, {L"Green", 0x6ccb5f, 0x0f7b0f}, {L"Amber", 0xffc057, 0x9d5d00},
    {L"Rose", 0xff99b0, 0xb4235a}, {L"Violet", 0xc3a0ff, 0x6b2fbf}, {L"Graphite", 0xc8c8c8, 0x4a4a4a},
};

static Color Hsl(float h, float sat, float l) {   // h 0..360, sat/l 0..1
    float c = (1 - fabsf(2 * l - 1)) * sat, hp = fmodf(h, 360.f) / 60.f, x = c * (1 - fabsf(fmodf(hp, 2.f) - 1));
    float r = 0, g = 0, b = 0;
    if (hp < 1) { r = c; g = x; } else if (hp < 2) { r = x; g = c; } else if (hp < 3) { g = c; b = x; }
    else if (hp < 4) { g = x; b = c; } else if (hp < 5) { r = x; b = c; } else { r = c; b = x; }
    float m = l - c / 2;
    return Color(255, (BYTE)lroundf((r + m) * 255), (BYTE)lroundf((g + m) * 255), (BYTE)lroundf((b + m) * 255));
}
static Color CustomAccent(bool dark, int hue, int sat) { return Hsl((float)hue, sat / 100.f, dark ? 0.64f : 0.42f); }
static Color WindowsAccent(bool dark) {
    BYTE pal[32]; DWORD psz = sizeof(pal);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Accent", L"AccentPalette", RRF_RT_REG_BINARY, nullptr, pal, &psz) == ERROR_SUCCESS && psz >= 32) {
        int i = dark ? 2 : 4;   // Light1 on dark, Dark1 on light: strong enough to carry white icons
        return Color(255, pal[i * 4], pal[i * 4 + 1], pal[i * 4 + 2]);
    }
    DWORD c; BOOL op;
    if (SUCCEEDED(DwmGetColorizationColor(&c, &op))) return Color(255, (c >> 16) & 255, (c >> 8) & 255, c & 255);
    return Color(255, 0, 120, 212);
}

static void LoadTheme() {
    DWORD v = 0, sz = sizeof(v);
    T.dark = !(RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &v, &sz) == ERROR_SUCCESS && v);
    v = 0; sz = sizeof(v);
    T.taskbarLight = RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"SystemUsesLightTheme", RRF_RT_REG_DWORD, nullptr, &v, &sz) == ERROR_SUCCESS && v;
    T.accent = g_accentId == 0 ? WindowsAccent(T.dark) : CustomAccent(T.dark, g_accentHue, g_accentSat);
    T.onAccent = Color(255, 255, 255, 255);
    bool d = T.dark, gl = T.glass;
    T.t1 = d ? Color(255, 255, 255, 255) : Color(228, 0, 0, 0);
    T.t2 = d ? Color(200, 255, 255, 255) : Color(155, 0, 0, 0);
    T.t3 = d ? Color(139, 255, 255, 255) : Color(114, 0, 0, 0);
    T.track = d ? Color(92, 255, 255, 255) : Color(77, 0, 0, 0);
    T.thumb = d ? Color(255, 69, 69, 69) : Color(255, 255, 255, 255);
    T.subtle = d ? Color(15, 255, 255, 255) : Color(9, 0, 0, 0);
    T.subtle2 = d ? Color(23, 255, 255, 255) : Color(15, 0, 0, 0);
    T.subtle3 = d ? Color(31, 255, 255, 255) : Color(23, 0, 0, 0);
    if (gl) {
        T.surface = d ? Color(36, 24, 24, 24) : Color(40, 250, 250, 250);   // light wash; the acrylic tint does the rest
        T.tabsBg = d ? Color(15, 255, 255, 255) : Color(10, 0, 0, 0);
        T.tabSel = d ? Color(33, 255, 255, 255) : Color(217, 255, 255, 255);
        T.badge = d ? Color(26, 255, 255, 255) : Color(15, 0, 0, 0);
        T.master = d ? Color(18, 255, 255, 255) : Color(140, 255, 255, 255);
        T.hover = d ? Color(15, 255, 255, 255) : Color(10, 0, 0, 0);
        T.ddBtn = d ? Color(20, 255, 255, 255) : Color(13, 0, 0, 0);
        T.ddBtnHover = d ? Color(31, 255, 255, 255) : Color(20, 0, 0, 0);
        T.valBg = d ? Color(18, 255, 255, 255) : Color(13, 0, 0, 0);
        T.rail = d ? Color(41, 255, 255, 255) : Color(36, 0, 0, 0);
        T.popup = d ? Color(240, 40, 40, 40) : Color(245, 249, 249, 249);
    } else {
        T.surface = d ? Color(255, 32, 32, 32) : Color(255, 243, 243, 243);
        T.tabsBg = T.subtle; T.tabSel = d ? T.subtle3 : Color(255, 255, 255, 255);
        T.badge = T.subtle2; T.master = T.subtle; T.hover = T.subtle;
        T.ddBtn = T.subtle2; T.ddBtnHover = T.subtle3; T.valBg = Color(0, 0, 0, 0); T.rail = T.track;
        T.popup = d ? Color(255, 44, 44, 44) : Color(255, 249, 249, 249);
    }
}

static FontFamily *g_famText, *g_famSemi, *g_famIcon;
static Font *g_fName, *g_fName600, *g_fSmall, *g_fSmall600, *g_fH3, *g_fTab, *g_fTab600, *g_fIcon, *g_fIconS, *g_fNum;

static FontFamily* PickFamily(std::initializer_list<const wchar_t*> names) {
    for (auto n : names) { FontFamily* f = new FontFamily(n); if (f->IsAvailable()) return f; delete f; }
    return FontFamily::GenericSansSerif()->Clone();
}
static void InitFonts() {
    if (!g_famText) {
        g_famText = PickFamily({L"Segoe UI Variable Text", L"Segoe UI", L"Tahoma"});
        g_famSemi = PickFamily({L"Segoe UI Semibold", L"Segoe UI Variable Text", L"Segoe UI", L"Tahoma"});
        g_famIcon = PickFamily({L"Segoe Fluent Icons", L"Segoe MDL2 Assets"});
    }
    for (Font** f : {&g_fName, &g_fName600, &g_fSmall, &g_fSmall600, &g_fH3, &g_fTab, &g_fTab600, &g_fIcon, &g_fIconS, &g_fNum}) { delete *f; *f = nullptr; }
    g_fName = new Font(g_famText, S(13), FontStyleRegular, UnitPixel);
    g_fName600 = new Font(g_famSemi, S(13), FontStyleRegular, UnitPixel);
    g_fSmall = new Font(g_famText, S(11.5f), FontStyleRegular, UnitPixel);
    g_fSmall600 = new Font(g_famSemi, S(12), FontStyleRegular, UnitPixel);
    g_fH3 = new Font(g_famSemi, S(10.5f), FontStyleRegular, UnitPixel);
    g_fTab = new Font(g_famText, S(13), FontStyleRegular, UnitPixel);
    g_fTab600 = new Font(g_famSemi, S(13), FontStyleRegular, UnitPixel);
    g_fIcon = new Font(g_famIcon, S(16), FontStyleRegular, UnitPixel);
    g_fIconS = new Font(g_famIcon, S(14), FontStyleRegular, UnitPixel);
    g_fNum = new Font(g_famSemi, S(10), FontStyleRegular, UnitPixel);
}

// Segoe Fluent Icons / MDL2 glyphs
enum : wchar_t {
    G_VOLUME = 0xE767, G_MUTE = 0xE74F, G_MIC = 0xE720, G_MICOFF = 0xEC54, G_SPEAKERS = 0xE7F5, G_HEADPHONE = 0xE7F6,
    G_TV = 0xE7F4, G_BRIGHT = 0xE706, G_CONTRAST = 0xE793, G_ROTATE = 0xE7AD, G_APPS = 0xE71D, G_CHEVDOWN = 0xE70D,
    G_SETTINGS = 0xE713, G_RINGER = 0xEA8F, G_EQ = 0xE9E9, G_IDENT = 0xE7F4, G_REFRESH = 0xE72C, G_EYE = 0xE890,
    G_POWER = 0xE7E8, G_CLOSE = 0xE711, G_OPEN = 0xE8A7
};

// ---------------------------------------------------------------- layout model
enum ElKind { E_H3, E_OUT, E_IN, E_MASTER, E_APP, E_MONHEAD, E_MONB, E_MONC, E_MONO, E_NOTE, E_PROFILES, E_HK, E_OSDROW, E_STEPROW, E_RESETROW };
struct El { ElKind k; int idx; RectF r; std::wstring text; };
enum HitKind { H_NONE, H_TAB, H_ROW, H_SLIDER, H_MUTE, H_DROPDOWN, H_IDENT, H_GEAR, H_REFRESH, H_DDOPT, H_MENU, H_MENUBG, H_BACK, H_HKCHIP, H_OSD, H_STEP, H_RESET, H_PROFILE, H_SAVEOPT, H_MICALL, H_DELAYCHIP, H_DELAYSL, H_UPDATE, H_MULTI };
struct Hit { HitKind h = H_NONE; int el = -1; int idx = -1; };

static const float LW = 380, TABH = 54, FTR = 44, PANEH = 470, ROWH = 76, ROWGAP = 4;
static std::vector<El> g_els;
static int g_W = 0, g_H = 0;
static float g_contentH = 0;
static Hit g_hover;
static int g_dragEl = -1;
static int g_hoverSlider = -1; static DWORD g_hoverSince = 0, g_lastScroll = 0;
static int g_ddOpen = -1;          // display index whose orientation list is open
static bool g_menuOpen = false;    // settings menu (gear)
static DWORD g_spinUntil = 0;      // refresh icon animation
static bool g_saveOpen = false;    // "save profile as" list
static int SI() { return g_view == 1 ? 3 : g_tab; }

static float PaneTop() { return S(TABH); }
static float PaneBottom() { return g_H - S(FTR); }
static float PaneX() { return S(12); }
static float PaneW() { return g_W - S(24); }

static float ProfileRowHeight(float w);
static void Layout() {
    g_els.clear();
    float x = PaneX(), w = PaneW();
    float y = PaneTop() + S(6) - g_scroll[SI()];
    bool first = true;
    auto h3 = [&](const wchar_t* t) { y += first ? S(10) : S(18); g_els.push_back({E_H3, -1, RectF(x + S(10), y, w - S(20), S(14)), t}); y += S(14) + S(8); first = false; };
    auto row = [&](ElKind k, int i) { g_els.push_back({k, i, RectF(x, y, w, S(ROWH)), L""}); y += S(ROWH) + S(ROWGAP); first = false; };
    if (g_view == 1) {
        h3(L"KEYBOARD SHORTCUTS");
        for (int i = 0; i < HK_COUNT; i++) { g_els.push_back({E_HK, i, RectF(x, y, w, S(44)), L""}); y += S(44) + S(2); }
        h3(L"OPTIONS");
        g_els.push_back({E_OSDROW, 0, RectF(x, y, w, S(44)), L""}); y += S(46);
        g_els.push_back({E_STEPROW, 0, RectF(x, y, w, S(44)), L""}); y += S(46);
        g_els.push_back({E_RESETROW, 0, RectF(x, y, w, S(44)), L""}); y += S(46);
        g_els.push_back({E_NOTE, -1, RectF(x + S(10), y + S(4), w - S(20), S(34)), L"Click a shortcut, then press the new keys. Esc cancels, Backspace clears it."}); y += S(40);
    } else if (g_tab == T_SOUND) {
        h3(L"OUTPUT");
        for (int i = 0; i < (int)g_out.size(); i++) row(E_OUT, i);
        h3(L"INPUT");
        for (int i = 0; i < (int)g_in.size(); i++) row(E_IN, i);
    } else if (g_tab == T_APPS) {
        row(E_MASTER, 0);
        h3(L"APPS");
        for (int i = 0; i < (int)g_apps.size(); i++) row(E_APP, i);
        if (g_apps.empty()) { g_els.push_back({E_NOTE, -1, RectF(x + S(10), y, w, S(24)), L"No apps are using sound right now"}); y += S(30); }
    } else {
        h3(L"PROFILES");
        { float ph = ProfileRowHeight(w); g_els.push_back({E_PROFILES, 0, RectF(x, y, w, ph), L""}); y += ph; }
        h3(L"DISPLAYS");
        if (g_mons.empty()) { g_els.push_back({E_NOTE, -1, RectF(x + S(10), y + S(6), w, S(24)), g_monsLoaded ? L"No displays found" : L"Looking for displays…"}); y += S(36); }
        for (int i = 0; i < (int)g_mons.size(); i++) {
            if (i) y += S(14);
            y += S(4);
            g_els.push_back({E_MONHEAD, i, RectF(x, y, w, S(30)), L""}); y += S(30);
            g_els.push_back({E_MONB, i, RectF(x, y, w, S(36)), L""}); y += S(36);
            if (g_mons[i].hasCon) { g_els.push_back({E_MONC, i, RectF(x, y, w, S(36)), L""}); y += S(36); }
            g_els.push_back({E_MONO, i, RectF(x, y, w, S(36)), L""}); y += S(36);
            y += S(2);
        }
    }
    g_contentH = y + g_scroll[SI()] - PaneTop() + S(14);
}

static void ClampScroll() {
    float view = PaneBottom() - PaneTop();
    float mx = std::max(0.f, g_contentH - view);
    g_scroll[SI()] = std::max(0.f, std::min(g_scroll[SI()], mx));
}

// row geometry (row padding 6 6 4 8; identity line 30; control line 36)
static float RowCy1(const El& e) { return e.r.Y + S(21); }
static float RowCy2(const El& e) { return e.r.Y + S(54); }
static RectF CtlMute(const El& e, float cy) { return RectF(e.r.X + S(6), cy - S(16), S(32), S(32)); }
static RectF CtlSlider(const El& e, float cy) { float l = e.r.X + S(48), r = e.r.X + e.r.Width - S(6) - S(36) - S(10); return RectF(l, cy - S(16), r - l, S(32)); }
static RectF CtlVal(const El& e, float cy) { return RectF(e.r.X + e.r.Width - S(6) - S(36), cy - S(10), S(36), S(20)); }
static bool IsRowKind(ElKind k) { return k == E_OUT || k == E_IN || k == E_MASTER || k == E_APP; }
static float SliderCy(const El& e) { return IsRowKind(e.k) ? RowCy2(e) : e.r.Y + e.r.Height / 2; }

static RectF TabRect(int i) {
    float x = S(12) + S(3), w = (g_W - S(24) - S(6) - S(4)) / 3;
    return RectF(x + i * (w + S(2)), S(12) + S(3), w, S(32));
}
static RectF GearRect() { return RectF(g_W - S(6) - S(32), g_H - S(FTR) + S(6), S(32), S(32)); }
static RectF IdentRect() { RectF g = GearRect(); return RectF(g.X - S(10) - S(32), g.Y, S(32), S(32)); }
static RectF RefreshRect() { RectF i = IdentRect(); return RectF(i.X - S(10) - S(32), i.Y, S(32), S(32)); }
static RectF MicAllRect() { RectF r = RefreshRect(); return RectF(r.X - S(10) - S(32), r.Y, S(32), S(32)); }
static int g_delayOpen = -1;     // output whose sync-delay popup is open
static bool g_dragDelay = false;
static RectF DelayChip(const El& e) { return RectF(e.r.X + e.r.Width - S(6) - S(84), e.r.Y + S(9), S(84), S(24)); }
static bool DelayAnchor(RectF& chip) { for (auto& e : g_els) if (e.k == E_OUT && e.idx == g_delayOpen) { chip = DelayChip(e); return true; } return false; }
static RectF DelayPopup(const RectF& chip) {
    float w = S(250), h = S(96);
    float y = chip.Y + chip.Height + S(6);
    if (y + h > PaneBottom() - S(4)) y = chip.Y - S(6) - h;
    return RectF(chip.X + chip.Width - w, y, w, h);
}
static RectF DelaySlider(const RectF& pop) { return RectF(pop.X + S(16), pop.Y + S(36), pop.Width - S(32), S(28)); }
static bool HasDelayChip(int i) { return i >= 0 && i < (int)g_out.size() && !g_out[i].isDefault && g_mirror.count(g_out[i].id); }
static int DelayOf(int i) { auto it = g_delay.find(g_out[i].id); return it == g_delay.end() ? 0 : it->second; }

static bool InRect(const RectF& r, float x, float y) { return x >= r.X && x < r.X + r.Width && y >= r.Y && y < r.Y + r.Height; }
static RectF BackRect() { return RectF(S(12), S(12), S(36), S(36)); }
static std::vector<RectF> g_chipRel;   // profile chips (+ Save as the last one), relative to the row's top-left
static RectF ProfileChip(const El& e, int i) { RectF r = g_chipRel[i]; r.X += e.r.X; r.Y += e.r.Y; return r; }
static RectF MultiRect(const El& e) {   // "Play together" switch on the right of the OUTPUT label (label + toggle)
    float w = S(128), h = S(24);
    return RectF(e.r.X + e.r.Width - w + S(6), e.r.Y + e.r.Height / 2 - h / 2, w, h);
}
static int ChipCount() { return (int)g_chipRel.size(); }
static bool SaveAnchor(RectF& chip) { for (auto& e : g_els) if (e.k == E_PROFILES && ChipCount()) { chip = ProfileChip(e, ChipCount() - 1); return true; } return false; }
static int SaveOptCount() { return (int)g_prof.size() + 1; }   // "Save as <name>" for each + "New profile..."
static RectF SavePopup(const RectF& chip) {
    float w = S(220), h = S(4) * 2 + S(32) * SaveOptCount() + S(2) * (SaveOptCount() - 1);
    float y = chip.Y + chip.Height + S(4);
    if (y + h > PaneBottom() - S(4)) y = chip.Y - S(4) - h;
    float x = std::max(PaneX(), std::min(chip.X, PaneX() + PaneW() - w));   // never off the panel's edge
    return RectF(x, y, w, h);
}
static float ProfileRowHeight(float w) {   // flow the chips onto as many lines as needed
    g_chipRel.clear();
    float x = S(4), y = S(2), h = S(32), gap = S(6);
    HDC dc = GetDC(nullptr);
    {
        Graphics g(dc);
        auto place = [&](float cw) { if (x + cw > w - S(4) && x > S(4)) { x = S(4); y += h + gap; } g_chipRel.push_back(RectF(x, y, cw, h)); x += cw + gap; };
        for (auto& p : g_prof) {
            std::wstring t = p.v.empty() ? L"+ " + p.name : p.name;
            RectF m; g.MeasureString(t.c_str(), -1, g_fSmall600, PointF(0, 0), &m);
            place(std::max(S(64), std::min(w - S(8), m.Width + S(28))));
        }
        place(S(78));   // Save
    }
    ReleaseDC(nullptr, dc);
    return y + h + S(4);
}
static RectF SaveOption(const RectF& pop, int i) { return RectF(pop.X + S(4), pop.Y + S(4) + i * (S(32) + S(2)), pop.Width - S(8), S(32)); }
static RectF HkChip(const El& e) { return RectF(e.r.X + e.r.Width - S(6) - S(176), e.r.Y + e.r.Height / 2 - S(15), S(176), S(30)); }
static RectF StepBox(const El& e) { float bw = S(50); return RectF(e.r.X + e.r.Width - S(6) - 3 * bw - S(8), e.r.Y + e.r.Height / 2 - S(16), 3 * bw + S(8), S(32)); }
static RectF StepBtn(const El& e, int i) { RectF box = StepBox(e); float bw = S(50); return RectF(box.X + S(3) + i * (bw + S(1)), box.Y + S(3), bw, S(26)); }
static RectF ResetBtn(const El& e) { return RectF(e.r.X + e.r.Width - S(6) - S(150), e.r.Y + e.r.Height / 2 - S(15), S(150), S(30)); }

// ---- orientation list (in-panel popup, styled like the panel)
static const wchar_t* ORIENT_NAMES[4] = {L"Landscape", L"Portrait", L"Landscape (flipped)", L"Portrait (flipped)"};
static bool DropdownRect(RectF& btn) {
    if (g_ddOpen < 0) return false;
    for (auto& e : g_els) if (e.k == E_MONO && e.idx == g_ddOpen) { btn = CtlSlider(e, SliderCy(e)); return true; }
    return false;
}
static RectF DropdownPopup(const RectF& btn) {
    float h = S(4) * 2 + S(32) * 4 + S(2) * 3;
    float y = btn.Y + btn.Height + S(4);
    if (y + h > PaneBottom() - S(4)) y = btn.Y - S(4) - h;
    return RectF(btn.X, y, btn.Width, h);
}
static RectF DropdownOption(const RectF& pop, int i) { return RectF(pop.X + S(4), pop.Y + S(4) + i * (S(32) + S(2)), pop.Width - S(8), S(32)); }

// ---- settings menu (in-panel popup above the gear)
enum MenuId { M_AWAKE = 1, M_STARTUP, M_SOUND, M_DISPLAY, M_EXIT, M_SOLID, M_GLASS, M_SHORTCUTS, M_RESTORE, M_ACC0 = 20 };
enum MenuType { MT_SWITCH, MT_LINK, MT_ITEM, MT_LABEL, MT_SEG, MT_SWATCH, MT_VERSION, MT_NAV };
struct MI { int id; int type; RectF r; std::wstring text; wchar_t glyph; };
static std::vector<MI> g_menu;
static RectF g_menuBox;
static void BuildMenu() {
    g_menu.clear();
    float w = S(260), x = g_W - S(8) - w, ix = x + S(6), iw = w - S(12);
    std::vector<MI> items;
    float y = 0;
    auto add = [&](int id, int type, float h, const wchar_t* t, wchar_t gl) { items.push_back({id, type, RectF(ix, y, iw, h), t, gl}); y += h; };
    y += S(6);
    add(M_AWAKE, MT_SWITCH, S(36), L"Keep awake", G_EYE);
    add(M_STARTUP, MT_SWITCH, S(36), L"Start with Windows", G_POWER);
    add(M_RESTORE, MT_SWITCH, S(36), L"Restore display brightness", G_BRIGHT);
    add(M_SHORTCUTS, MT_NAV, S(36), L"Keyboard shortcuts", 0xE765);
    y += S(6);
    add(M_SOUND, MT_LINK, S(36), L"Sound settings", G_VOLUME);
    add(M_DISPLAY, MT_LINK, S(36), L"Display settings", G_TV);
    y += S(6);
    add(M_EXIT, MT_ITEM, S(36), L"Exit", G_CLOSE);
    add(0, MT_LABEL, S(24), L"Style", 0);
    {   // segmented Solid | Glass
        float sy = y + S(4), sw = (iw - S(8) - S(6) - S(2)) / 2;
        items.push_back({-1, MT_LABEL, RectF(ix + S(4), sy, iw - S(8), S(34)), L"#seg", 0});
        items.push_back({M_SOLID, MT_SEG, RectF(ix + S(4) + S(3), sy + S(3), sw, S(28)), L"Solid", 0});
        items.push_back({M_GLASS, MT_SEG, RectF(ix + S(4) + S(3) + sw + S(2), sy + S(3), sw, S(28)), L"Glass", 0});
        y += S(4) + S(34) + S(4);
    }
    std::wstring accLbl = std::wstring(L"Accent color · ") + (g_accentId ? L"Custom" : L"Windows accent");
    items.push_back({0, MT_LABEL, RectF(ix, y, iw, S(24)), accLbl, 0}); y += S(24);
    for (int i = 0; i < 7; i++) items.push_back({M_ACC0 + i, MT_SWATCH, RectF(ix + S(10) + i * S(28), y + S(6), S(22), S(22)), ACCENTS[i].name, 0});
    y += S(34);
    add(0, MT_VERSION, S(26), APP_NAME L" " APP_VERSION, 0);
    y += S(6);
    float top = g_H - S(50) - y;
    for (auto& m : items) { m.r.Y += top; g_menu.push_back(m); }
    g_menuBox = RectF(x, top, w, y);
}

static Hit HitTest(float x, float y) {
    Hit h;
    if (g_menuOpen) {
        BuildMenu();
        if (InRect(g_menuBox, x, y)) {
            h.h = H_MENUBG;
            for (auto& m : g_menu) if (m.id > 0 && InRect(m.r, x, y)) { h.h = H_MENU; h.idx = m.id; break; }
            return h;
        }
    }
    RectF dchip;
    if (g_delayOpen >= 0 && DelayAnchor(dchip)) {
        RectF pop = DelayPopup(dchip);
        if (InRect(pop, x, y)) {
            RectF sl = DelaySlider(pop);
            h.h = InRect(RectF(sl.X - S(10), sl.Y - S(4), sl.Width + S(20), sl.Height + S(8)), x, y) ? H_DELAYSL : H_MENUBG;
            return h;
        }
    }
    RectF chip;
    if (g_saveOpen && SaveAnchor(chip)) {
        RectF pop = SavePopup(chip);
        if (InRect(pop, x, y)) {
            for (int i = 0; i < SaveOptCount(); i++) if (InRect(SaveOption(pop, i), x, y)) { h.h = H_SAVEOPT; h.idx = i; return h; }
            h.h = H_MENUBG; return h;
        }
    }
    RectF btn;
    if (DropdownRect(btn)) {
        RectF pop = DropdownPopup(btn);
        if (InRect(pop, x, y)) {
            for (int i = 0; i < 4; i++) if (InRect(DropdownOption(pop, i), x, y)) { h.h = H_DDOPT; h.idx = i; return h; }
            h.h = H_MENUBG; return h;
        }
    }
    if (g_view == 1) { if (InRect(BackRect(), x, y)) { h.h = H_BACK; return h; } }
    else for (int i = 0; i < 3; i++) if (InRect(TabRect(i), x, y)) { h.h = H_TAB; h.idx = i; return h; }
    if (InRect(GearRect(), x, y)) { h.h = H_GEAR; return h; }
    if (InRect(IdentRect(), x, y)) { h.h = H_IDENT; return h; }
    if (InRect(RefreshRect(), x, y)) { h.h = H_REFRESH; return h; }
    if (InRect(MicAllRect(), x, y)) { h.h = H_MICALL; return h; }
    if (g_updState == UPD_AVAILABLE || g_updState == UPD_FAILED) { if (InRect(g_verRect, x, y)) { h.h = H_UPDATE; return h; } }
    if (y < PaneTop() || y >= PaneBottom()) return h;
    for (int i = 0; i < (int)g_els.size(); i++) {
        El& e = g_els[i];
        if (e.k == E_H3 && e.text == L"OUTPUT" && InRect(MultiRect(e), x, y)) { h.h = H_MULTI; h.el = i; return h; }
        if (!InRect(e.r, x, y)) continue;
        h.el = i; h.idx = e.idx;
        if (IsRowKind(e.k)) {
            float cy2 = RowCy2(e);
            RectF sl = CtlSlider(e, cy2); sl.X -= S(10); sl.Width += S(20);
            if (e.k == E_OUT && HasDelayChip(e.idx) && InRect(DelayChip(e), x, y)) h.h = H_DELAYCHIP;
            else if (InRect(CtlMute(e, cy2), x, y)) h.h = H_MUTE;
            else if (InRect(RectF(sl.X, cy2 - S(18), sl.Width, S(36)), x, y)) h.h = H_SLIDER;
            else h.h = (e.k == E_OUT || e.k == E_IN) ? H_ROW : H_NONE;
        } else if (e.k == E_MONB || e.k == E_MONC) {
            RectF sl = CtlSlider(e, SliderCy(e)); sl.X -= S(10); sl.Width += S(20);
            if (InRect(sl, x, y) && g_mons[e.idx].ok) h.h = H_SLIDER;
        } else if (e.k == E_MONO) {
            if (InRect(CtlSlider(e, SliderCy(e)), x, y)) h.h = H_DROPDOWN;
        } else if (e.k == E_PROFILES) {
            for (int c = 0; c < ChipCount(); c++) if (InRect(ProfileChip(e, c), x, y)) { h.h = H_PROFILE; h.idx = c; }
        } else if (e.k == E_HK) {
            if (InRect(HkChip(e), x, y)) h.h = H_HKCHIP;
        } else if (e.k == E_OSDROW) h.h = H_OSD;
        else if (e.k == E_STEPROW) {
            for (int c = 0; c < 3; c++) if (InRect(StepBtn(e, c), x, y)) { h.h = H_STEP; h.idx = c; }
        } else if (e.k == E_RESETROW) {
            if (InRect(ResetBtn(e), x, y)) h.h = H_RESET;
        }
        return h;
    }
    return h;
}

// ---------------------------------------------------------------- values
static float ElValue(const El& e) {
    switch (e.k) {
    case E_OUT: return g_out[e.idx].level;
    case E_IN: return g_in[e.idx].level;
    case E_APP: return g_apps[e.idx].level;
    case E_MASTER: { int d = DefaultOut(); return d >= 0 ? g_out[d].level : 0; }
    case E_MONB: return g_mons[e.idx].pct / 100.f;
    case E_MONC: return g_mons[e.idx].con / 100.f;
    default: return 0;
    }
}
static bool ElMuted(const El& e) {
    switch (e.k) {
    case E_OUT: return g_out[e.idx].mute;
    case E_IN: return g_in[e.idx].mute;
    case E_APP: return g_apps[e.idx].mute;
    case E_MASTER: { int d = DefaultOut(); return d >= 0 && g_out[d].mute; }
    default: return false;
    }
}

static void UpdateTrayTip();

static void SetEndpointLevel(Endpoint& e, float v) {
    v = std::max(0.f, std::min(1.f, v));
    if (e.vol) { e.vol->SetMasterVolumeLevelScalar(v, nullptr); if (e.mute) { e.vol->SetMute(FALSE, nullptr); e.mute = FALSE; } }
    e.level = v;
}

static void SetElValue(const El& e, float v) {
    v = std::max(0.f, std::min(1.f, v));
    switch (e.k) {
    case E_OUT: SetEndpointLevel(g_out[e.idx], v); UpdateTrayTip(); break;
    case E_IN: SetEndpointLevel(g_in[e.idx], v); break;
    case E_MASTER: {   // move every selected output by the same amount
        int d = DefaultOut(); if (d < 0) break;
        float delta = v - g_out[d].level;
        for (int i = 0; i < (int)g_out.size(); i++) if (OutSelected(i)) SetEndpointLevel(g_out[i], g_out[i].level + delta);
        UpdateTrayTip();
        break;
    }
    case E_APP: {
        AppGroup& a = g_apps[e.idx];
        for (auto* sv : a.vols) { sv->SetMasterVolume(v, nullptr); if (a.mute) sv->SetMute(FALSE, nullptr); }
        a.level = v; a.mute = FALSE;
        break;
    }
    case E_MONB: { int p = (int)lround(v * 100); if (p != g_mons[e.idx].pct) { g_mons[e.idx].pct = p; MonSet(e.idx, 0, p); } break; }
    case E_MONC: { int p = (int)lround(v * 100); if (p != g_mons[e.idx].con) { g_mons[e.idx].con = p; MonSet(e.idx, 1, p); } break; }
    default: break;
    }
}

static void ToggleMute(const El& e) {
    switch (e.k) {
    case E_OUT: case E_IN: { Endpoint& p = (e.k == E_OUT ? g_out : g_in)[e.idx]; if (p.vol) { p.mute = !p.mute; p.vol->SetMute(p.mute, nullptr); } break; }
    case E_MASTER: {
        int d = DefaultOut(); if (d < 0) break;
        BOOL m = !g_out[d].mute;
        for (int i = 0; i < (int)g_out.size(); i++) if (OutSelected(i) && g_out[i].vol) { g_out[i].mute = m; g_out[i].vol->SetMute(m, nullptr); }
        break;
    }
    case E_APP: { AppGroup& a = g_apps[e.idx]; a.mute = !a.mute; for (auto* sv : a.vols) sv->SetMute(a.mute, nullptr); break; }
    default: break;
    }
    UpdateTrayTip();
}

static float SliderValueAt(const El& e, float x) {
    RectF sl = CtlSlider(e, SliderCy(e));
    return (x - sl.X) / std::max(1.f, sl.Width);
}

// ---------------------------------------------------------------- shortcut actions + profiles
static bool AllMicsMuted() { if (g_in.empty()) return false; for (auto& e : g_in) if (!e.mute) return false; return true; }
static void MicToggleMute() {   // mutes every microphone, not just the default one
    BOOL m = !AllMicsMuted();
    for (auto& e : g_in) if (e.vol) { e.mute = m; e.vol->SetMute(m, nullptr); }
}
static bool ProfileEmpty(int p) { return g_prof[p].v.empty(); }
static bool ProfileActive(int p) {
    if (ProfileEmpty(p)) return false;
    int matched = 0;
    for (auto& m : g_mons) {
        auto it = g_prof[p].v.find(m.name);
        if (it == g_prof[p].v.end() || !m.ok) continue;
        if (std::abs(m.pct - it->second.first) > 1) return false;
        if (m.hasCon && it->second.second >= 0 && std::abs(m.con - it->second.second) > 1) return false;
        matched++;
    }
    return matched > 0;
}
static void SaveProfile(int p) {
    g_prof[p].v.clear();
    for (auto& m : g_mons) if (m.ok) g_prof[p].v[m.name] = {m.pct, m.hasCon ? m.con : -1};
    SaveSettings();
}
static void ApplyProfile(int p) {
    for (int i = 0; i < (int)g_mons.size(); i++) {
        auto it = g_prof[p].v.find(g_mons[i].name);
        if (it == g_prof[p].v.end() || !g_mons[i].ok) continue;
        g_mons[i].pct = it->second.first; MonSet(i, 0, it->second.first);
        if (g_mons[i].hasCon && it->second.second >= 0) { g_mons[i].con = it->second.second; MonSet(i, 1, it->second.second); }
    }
}
static std::wstring KeyName(UINT vk) {
    switch (vk) {
    case VK_UP: return L"Up"; case VK_DOWN: return L"Down"; case VK_LEFT: return L"Left"; case VK_RIGHT: return L"Right";
    case VK_PRIOR: return L"Page Up"; case VK_NEXT: return L"Page Down"; case VK_HOME: return L"Home"; case VK_END: return L"End";
    case VK_INSERT: return L"Insert"; case VK_DELETE: return L"Delete"; case VK_SPACE: return L"Space";
    }
    if (vk >= VK_F1 && vk <= VK_F24) return L"F" + std::to_wstring(vk - VK_F1 + 1);
    UINT sc = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    wchar_t b[64];
    if (sc && GetKeyNameTextW((LONG)(sc << 16), b, 64)) return b;
    return L"Key " + std::to_wstring(vk);
}
static std::wstring ComboName(const HotKey& h) {
    if (!h.vk) return L"Not set";
    std::wstring s;
    if (h.mods & MOD_CONTROL) s += L"Ctrl + ";
    if (h.mods & MOD_ALT) s += L"Alt + ";
    if (h.mods & MOD_SHIFT) s += L"Shift + ";
    if (h.mods & MOD_WIN) s += L"Win + ";
    return s + KeyName(h.vk);
}
static bool HkRepeats(int) { return false; }
static void UnregisterHotkeys() { for (int i = 0; i < HK_COUNT; i++) UnregisterHotKey(g_hwnd, 100 + i); }
static void RegisterHotkeys() {
    UnregisterHotkeys();
    for (int i = 0; i < HK_COUNT; i++) {
        g_hk[i].ok = false;
        if (g_hk[i].vk) g_hk[i].ok = RegisterHotKey(g_hwnd, 100 + i, g_hk[i].mods | (HkRepeats(i) ? 0 : MOD_NOREPEAT), g_hk[i].vk) != 0;
    }
}

// ---------------------------------------------------------------- tray
static HICON MakeTrayIcon() {
    UINT dpi = GetDpiForSystem();
    int sz = GetSystemMetricsForDpi(SM_CXSMICON, dpi);
    Bitmap bmp(sz, sz, PixelFormat32bppARGB);
    {
        Graphics g(&bmp);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.Clear(Color(0, 0, 0, 0));
        Color c = T.taskbarLight ? Color(255, 28, 28, 28) : Color(255, 255, 255, 255);
        float s = sz / 16.f;
        Pen line(c, 1.3f * s); line.SetStartCap(LineCapRound); line.SetEndCap(LineCapRound);
        SolidBrush br(c);
        float xs[3] = {3.5f, 8.f, 12.5f}, ks[3] = {10.f, 5.f, 8.5f};
        for (int i = 0; i < 3; i++) {
            g.DrawLine(&line, xs[i] * s, 2.f * s, xs[i] * s, 14.f * s);
            float r = 2.2f * s;
            g.FillEllipse(&br, xs[i] * s - r, ks[i] * s - r, 2 * r, 2 * r);
        }
    }
    HICON h = nullptr;
    bmp.GetHICON(&h);
    return h;
}
// Keep the tray icon on the taskbar across updates. Windows 11 keeps "show on taskbar" per icon in
// HKCU\Control Panel\NotifyIconSettings\<id> (IsPromoted); a rebuilt exe can get a fresh, hidden entry.
static void SaveTrayPinned() {
    wchar_t v[8]; swprintf(v, 8, L"%d", g_trayPinned);
    WritePrivateProfileStringW(L"main", L"traypinned", v, g_iniPath.c_str());
}
static void TrayAdd();
static void UpdateTrayTip();
static void TrayReAdd() { Shell_NotifyIconW(NIM_DELETE, &g_nid); TrayAdd(); UpdateTrayTip(); }
static void TrayPinSync(bool enforce) {
    HKEY root;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Control Panel\\NotifyIconSettings", 0, KEY_READ | KEY_WRITE, &root) != ERROR_SUCCESS) return;
    std::vector<std::wstring> mine; std::vector<DWORD> vals;
    wchar_t name[256];
    for (DWORD i = 0;; i++) {
        DWORD nl = 256;
        if (RegEnumKeyExW(root, i, name, &nl, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        wchar_t path[1024] = {}; DWORD pl = sizeof(path);
        if (RegGetValueW(root, name, L"ExecutablePath", RRF_RT_REG_SZ, nullptr, path, &pl) != ERROR_SUCCESS) continue;
        size_t L = wcslen(path);
        if (L < 9 || _wcsicmp(path + L - 9, L"\\Knob.exe") != 0) continue;
        DWORD pr = 0, sz = sizeof(pr);
        RegGetValueW(root, name, L"IsPromoted", RRF_RT_REG_DWORD, nullptr, &pr, &sz);
        mine.push_back(name); vals.push_back(pr ? 1 : 0);
    }
    if (!mine.empty()) {
        bool anyOn = false, allSame = true;
        for (DWORD v : vals) { anyOn |= v != 0; allSame &= v == vals[0]; }
        if (g_trayPinned < 0) { g_trayPinned = anyOn ? 1 : 0; SaveTrayPinned(); }
        else if (!enforce && allSame && (int)vals[0] != g_trayPinned) { g_trayPinned = (int)vals[0]; SaveTrayPinned(); }   // you changed it
        else if (g_trayPinned == 1) {   // just started, or a new hidden entry appeared: put it back on the taskbar
            bool changed = false;
            for (size_t i = 0; i < mine.size(); i++) if (!vals[i]) {
                DWORD one = 1;
                if (RegSetKeyValueW(root, mine[i].c_str(), L"IsPromoted", REG_DWORD, &one, sizeof(one)) == ERROR_SUCCESS) changed = true;
            }
            if (changed) { RegCloseKey(root); TrayReAdd(); return; }   // Windows reads the setting when the icon is added
        }
    }
    RegCloseKey(root);
}
static void TrayAdd() {
    if (g_trayIcon) DestroyIcon(g_trayIcon);
    g_trayIcon = MakeTrayIcon();
    ZeroMemory(&g_nid, sizeof(g_nid));
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP | (g_trayGuidOk ? NIF_GUID : 0);
    g_nid.guidItem = KNOB_TRAY_GUID;   // same id across updates, so Windows keeps "show on taskbar"
    g_nid.uCallbackMessage = WM_APP_TRAY;
    g_nid.hIcon = g_trayIcon;
    lstrcpynW(g_nid.szTip, APP_NAME, ARRAYSIZE(g_nid.szTip));
    if (!Shell_NotifyIconW(NIM_ADD, &g_nid) && g_trayGuidOk) {
        Shell_NotifyIconW(NIM_DELETE, &g_nid);   // a stale icon with our id: clear it and try again
        if (!Shell_NotifyIconW(NIM_ADD, &g_nid)) {   // Windows won't take the id: plain icon
            g_trayGuidOk = false;
            g_nid.uFlags &= ~NIF_GUID;
            Shell_NotifyIconW(NIM_ADD, &g_nid);
        }
    }
    g_nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &g_nid);
}
static void UpdateTrayTip() {
    std::wstring tip = APP_NAME;
    int d = DefaultOut();
    if (d >= 0) tip += L"\n" + g_out[d].name + L": " + (g_out[d].mute ? std::wstring(L"muted") : std::to_wstring((int)lround(g_out[d].level * 100)) + L"%");
    int n = SelectedOutCount();
    if (n > 1) tip += L"\nPlaying on " + std::to_wstring(n) + L" outputs";
    g_nid.uFlags = NIF_TIP | NIF_SHOWTIP | (g_trayGuidOk ? NIF_GUID : 0);
    lstrcpynW(g_nid.szTip, tip.c_str(), ARRAYSIZE(g_nid.szTip));
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}
static void UpdateTrayIcon() {
    HICON old = g_trayIcon;
    g_trayIcon = MakeTrayIcon();
    g_nid.uFlags = NIF_ICON | (g_trayGuidOk ? NIF_GUID : 0); g_nid.hIcon = g_trayIcon;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
    if (old) DestroyIcon(old);
}

// ---------------------------------------------------------------- painting
static void AddRound(GraphicsPath& p, RectF r, float rad) {
    float d = std::min(rad * 2, std::min(r.Width, r.Height));
    if (d <= 0.5f) { p.AddRectangle(r); return; }
    p.AddArc(r.X, r.Y, d, d, 180, 90);
    p.AddArc(r.X + r.Width - d, r.Y, d, d, 270, 90);
    p.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0, 90);
    p.AddArc(r.X, r.Y + r.Height - d, d, d, 90, 90);
    p.CloseFigure();
}
static void FillRound(Graphics& g, Color c, RectF r, float rad) { if (!c.GetA()) return; SolidBrush b(c); GraphicsPath p; AddRound(p, r, rad); g.FillPath(&b, &p); }

static void Text(Graphics& g, const wchar_t* s, Font* f, Color c, RectF r, StringAlignment h = StringAlignmentNear) {
    StringFormat sf;
    sf.SetAlignment(h);
    sf.SetLineAlignment(StringAlignmentCenter);
    sf.SetTrimming(StringTrimmingEllipsisCharacter);
    sf.SetFormatFlags(StringFormatFlagsNoWrap);
    SolidBrush b(c);
    g.DrawString(s, -1, f, r, &sf, &b);
}
static int TextLines(Graphics& g, const wchar_t* s, Font* f, float w) {   // wrapped line count
    StringFormat sf; RectF out; INT chars = 0, lines = 0;
    g.MeasureString(s, -1, f, RectF(0, 0, w, 10000), &sf, &out, &chars, &lines);
    return std::max(1, (int)lines);
}
static void TextWrap(Graphics& g, const wchar_t* s, Font* f, Color c, RectF r) {   // top-aligned, wraps, ellipsis on overflow
    StringFormat sf;
    sf.SetTrimming(StringTrimmingEllipsisWord);
    sf.SetFormatFlags(StringFormatFlagsLineLimit);
    SolidBrush b(c);
    g.DrawString(s, -1, f, r, &sf, &b);
}
static float TextW(Graphics& g, const wchar_t* s, Font* f) { RectF out; g.MeasureString(s, -1, f, PointF(0, 0), &out); return out.Width; }
static void Glyph(Graphics& g, wchar_t ch, Font* f, Color c, float cx, float cy) {
    wchar_t s[2] = {ch, 0};
    Text(g, s, f, c, RectF(cx - S(16), cy - S(16), S(32), S(32)), StringAlignmentCenter);
}
static void SpacedText(Graphics& g, const wchar_t* s, Font* f, Color c, float x, float cy, float spacing) {   // uppercase section labels
    StringFormat sf(StringFormat::GenericTypographic());
    sf.SetLineAlignment(StringAlignmentCenter);
    SolidBrush b(c);
    for (const wchar_t* p = s; *p; p++) {
        wchar_t ch[2] = {*p, 0};
        RectF m; g.MeasureString(ch, 1, f, PointF(0, 0), &sf, &m);
        g.DrawString(ch, 1, f, RectF(x, cy - S(8), m.Width + S(2), S(16)), &sf, &b);
        x += (*p == L' ' ? S(3) : m.Width) + spacing;
    }
}
static void Shadow(Graphics& g, RectF r, float rad) {
    for (int i = 3; i >= 1; i--) {
        float o = S(i * 3.f);
        FillRound(g, Color((BYTE)(T.dark ? 22 : 12), 0, 0, 0), RectF(r.X - o * 0.5f, r.Y - o * 0.2f + S(4), r.Width + o, r.Height + o), rad + o * 0.5f);
    }
}

static void DrawSlider(Graphics& g, RectF r, float v, bool dim, int state /*0 rest,1 hover,2 drag*/) {
    v = std::max(0.f, std::min(1.f, v));
    float cy = r.Y + r.Height / 2;
    float fx = r.X + r.Width * v;
    Color ac = dim ? T.t3 : T.accent;
    if (T.glass) {   // thin rail + white handle with a soft accent halo
        float th = S(3);
        FillRound(g, T.rail, RectF(r.X, cy - th / 2, r.Width, th), th / 2);
        if (fx > r.X + 1) FillRound(g, ac, RectF(r.X, cy - th / 2, fx - r.X, th), th / 2);
        float R = S(7), halo = state == 2 ? S(8) : state == 1 ? S(6) : S(4);
        if (!dim) { SolidBrush hb(WithAlpha(T.accent, state == 2 ? 46 : state == 1 ? 56 : 61)); g.FillEllipse(&hb, fx - R - halo, cy - R - halo, 2 * (R + halo), 2 * (R + halo)); }
        SolidBrush sh(Color(90, 0, 0, 0)); g.FillEllipse(&sh, fx - R, cy - R + S(1), 2 * R, 2 * R);
        SolidBrush wb(Color(255, 255, 255, 255)); g.FillEllipse(&wb, fx - R, cy - R, 2 * R, 2 * R);
        return;
    }
    float th = S(4);   // Windows 11 slider
    FillRound(g, T.track, RectF(r.X, cy - th / 2, r.Width, th), th / 2);
    if (fx > r.X + 1) FillRound(g, ac, RectF(r.X, cy - th / 2, fx - r.X, th), th / 2);
    float R = S(10);
    SolidBrush shadow(Color(50, 0, 0, 0));
    g.FillEllipse(&shadow, fx - R, cy - R + S(1), 2 * R, 2 * R);
    SolidBrush thumb(T.thumb);
    g.FillEllipse(&thumb, fx - R, cy - R, 2 * R, 2 * R);
    Pen ring(T.subtle2, S(1));
    g.DrawEllipse(&ring, fx - R, cy - R, 2 * R, 2 * R);
    float r2 = state == 2 ? S(4) : state == 1 ? S(6) : S(5);
    SolidBrush ab(ac);
    g.FillEllipse(&ab, fx - r2, cy - r2, 2 * r2, 2 * r2);
}
static void DrawValue(Graphics& g, RectF r, int v) {
    wchar_t pct[8]; swprintf(pct, 8, L"%d", v);
    if (T.glass) { FillRound(g, T.valBg, RectF(r.X + S(2), r.Y, r.Width - S(2), r.Height), S(8)); Text(g, pct, g_fSmall, T.t2, RectF(r.X + S(2), r.Y, r.Width - S(2), r.Height), StringAlignmentCenter); }
    else Text(g, pct, g_fSmall, T.t2, RectF(r.X, r.Y, r.Width - S(4), r.Height), StringAlignmentFar);
}
static void DrawIconBtn(Graphics& g, RectF r, wchar_t glyph, Color c, bool hover) {
    if (hover) FillRound(g, T.hover, r, S(6));
    Glyph(g, glyph, g_fIcon, c, r.X + r.Width / 2, r.Y + r.Height / 2);
}
static void DrawSwitch(Graphics& g, float x, float cy, bool on, float k = 1.f) {   // Windows-style toggle; k scales it
    RectF r(x, cy - S(10 * k), S(40 * k), S(20 * k));
    if (on) FillRound(g, T.accent, r, S(10 * k));
    else { GraphicsPath p; AddRound(p, RectF(r.X + S(0.5f), r.Y + S(0.5f), r.Width - S(1), r.Height - S(1)), S(10 * k)); Pen pen(T.t2, S(1)); g.DrawPath(&pen, &p); }
    float kx = on ? r.X + r.Width - S(16 * k) : r.X + S(4 * k);
    SolidBrush kb(on ? T.onAccent : T.t2);
    g.FillEllipse(&kb, kx, cy - S(6 * k), S(12 * k), S(12 * k));
}

static wchar_t KindGlyph(int k) { return k == DK_HEADPHONES ? G_HEADPHONE : k == DK_MIC ? G_MIC : k == DK_DISPLAY ? G_TV : G_SPEAKERS; }

static void DrawEl(Graphics& g, int index) {
    El& e = g_els[index];
    if (e.r.Y + e.r.Height < PaneTop() || e.r.Y > PaneBottom()) return;
    bool hovEl = g_hover.el == index;
    if (e.k == E_H3) {
        SpacedText(g, e.text.c_str(), g_fH3, T.t3, e.r.X, e.r.Y + e.r.Height / 2, S(1));
        if (e.text == L"OUTPUT") {   // same toggle as Settings, smaller, with its label on the left
            RectF m = MultiRect(e);
            float sw = S(32), sx = m.X + m.Width - S(6) - sw, cy = m.Y + m.Height / 2;
            if (g_hover.h == H_MULTI) FillRound(g, T.hover, m, S(6));
            Text(g, L"Play together", g_fSmall, g_multi ? T.t1 : T.t2, RectF(m.X, m.Y, sx - S(8) - m.X, m.Height), StringAlignmentFar);
            DrawSwitch(g, sx, cy, g_multi, 0.8f);
        }
        return;
    }
    if (e.k == E_NOTE) {
        StringFormat sf; sf.SetLineAlignment(StringAlignmentNear); SolidBrush b(T.t3);
        g.DrawString(e.text.c_str(), -1, g_fSmall, e.r, &sf, &b); return;
    }

    if (IsRowKind(e.k)) {
        bool sel = (e.k == E_OUT && OutSelected(e.idx)) || (e.k == E_IN && g_in[e.idx].isDefault);
        bool playing = e.k == E_APP && AppPlaying(g_apps[e.idx]) && !g_apps[e.idx].mute;
        bool pickable = e.k == E_OUT || e.k == E_IN;
        bool rowHover = hovEl && g_hover.h == H_ROW;
        // full-row, flat highlights
        if (e.k == E_MASTER) FillRound(g, T.master, e.r, S(10));
        else if (sel) {
            if (T.glass && !T.dark) FillRound(g, Color(90, 255, 255, 255), e.r, S(10));
            FillRound(g, rowHover ? T.subtle2 : T.subtle, e.r, S(10));   // neutral lift, then a light accent wash
            int a = T.glass ? (T.dark ? (rowHover ? 22 : 16) : (rowHover ? 20 : 14)) : (rowHover ? 18 : 13);
            FillRound(g, WithAlpha(T.accent, a), e.r, S(10));
        } else if (playing) FillRound(g, WithAlpha(T.accent, T.glass ? 14 : 11), e.r, S(10));
        else if (pickable && rowHover) FillRound(g, T.hover, e.r, S(10));

        float cy1 = RowCy1(e), cy2 = RowCy2(e), lx = e.r.X + S(8) + S(14);
        std::wstring name, detail; wchar_t muteG = G_VOLUME;
        bool muted = ElMuted(e);
        if (e.k == E_APP) {
            AppGroup& a = g_apps[e.idx];
            name = a.name;
            Gdiplus::Bitmap* icn = a.path.empty() ? nullptr : GetProcInfo(a.path).icon;
            float isz = S(22);
            if (icn) { g.SetInterpolationMode(InterpolationModeHighQualityBicubic); g.DrawImage(icn, RectF(lx - isz / 2, cy1 - isz / 2, isz, isz)); }
            else { FillRound(g, T.badge, RectF(lx - S(11), cy1 - S(11), S(22), S(22)), S(7)); Glyph(g, a.system ? G_RINGER : G_APPS, g_fIconS, T.t1, lx, cy1); }
            muteG = muted ? G_MUTE : G_VOLUME;
        } else {
            wchar_t lg = G_VOLUME;
            if (e.k == E_MASTER) { name = L"Master volume"; muteG = muted ? G_MUTE : G_VOLUME; }
            else {
                Endpoint& d = (e.k == E_OUT ? g_out : g_in)[e.idx];
                name = d.name; detail = d.detail; lg = KindGlyph(d.kind);
                muteG = e.k == E_IN ? (muted ? G_MICOFF : G_MIC) : (muted ? G_MUTE : G_VOLUME);
            }
            FillRound(g, sel ? T.accent : T.badge, RectF(lx - S(14), cy1 - S(14), S(28), S(28)), S(9));   // rounded-square badge
            Glyph(g, lg, g_fIconS, sel ? T.onAccent : T.t1, lx, cy1);
        }
        float nx = e.r.X + S(8) + S(28) + S(10), maxW = e.r.X + e.r.Width - S(6) - nx;
        if (e.k == E_OUT && HasDelayChip(e.idx)) {
            RectF c = DelayChip(e);
            maxW = c.X - S(8) - nx;
            bool hov = hovEl && g_hover.h == H_DELAYCHIP, open = g_delayOpen == e.idx;
            FillRound(g, hov || open ? T.ddBtnHover : T.ddBtn, c, S(12));
            wchar_t t[32]; swprintf(t, 32, L"Sync %d ms", DelayOf(e.idx));
            Glyph(g, 0xE916, g_fIconS, T.t1, c.X + S(14), c.Y + c.Height / 2);   // stopwatch
            Text(g, t, g_fSmall, T.t1, RectF(c.X + S(24), c.Y, c.Width - S(28), c.Height));
        }
        Font* nf = e.k == E_MASTER ? g_fName600 : g_fName;
        float nw = std::min(TextW(g, name.c_str(), nf), maxW * 0.7f);
        Text(g, name.c_str(), nf, T.t1, RectF(nx, cy1 - S(10), nw + S(2), S(20)));
        float after = nx + nw + S(8);
        if (!detail.empty()) Text(g, detail.c_str(), g_fSmall, T.t3, RectF(after, cy1 - S(9), std::max(0.f, maxW - nw - S(8)), S(18)));
        if (playing) {   // live bars driven by the app's real peak level
            float lvl = std::min(1.f, g_live[g_apps[e.idx].key].smooth * 4.f);
            DWORD t = GetTickCount();
            for (int b = 0; b < 3; b++) {
                float wob = 0.5f + 0.5f * sinf(t / (110.f + b * 35.f) + b * 2.1f);
                float amp = 0.45f + 0.55f * lvl;
                float hgt = S(3) + (S(10) - S(3)) * (0.15f + 0.85f * wob * amp);
                FillRound(g, T.accent, RectF(after + b * S(4.5f), cy1 + S(5) - hgt, S(2.5f), hgt), S(1));
            }
        }
        RectF mr = CtlMute(e, cy2);
        DrawIconBtn(g, mr, muteG, muted ? T.t3 : T.t1, hovEl && g_hover.h == H_MUTE);
        int st = g_dragEl == index ? 2 : (hovEl && g_hover.h == H_SLIDER ? 1 : 0);
        float v = ElValue(e);
        DrawSlider(g, CtlSlider(e, cy2), v, muted, st);
        DrawValue(g, CtlVal(e, cy2), (int)lround(v * 100));
        return;
    }

    if (e.k == E_PROFILES) {
        int n = ChipCount();
        for (int c = 0; c < n; c++) {
            RectF r = ProfileChip(e, c);
            bool save = c == n - 1;
            bool hov = g_hover.h == H_PROFILE && g_hover.idx == c;
            bool active = !save && ProfileActive(c), empty = !save && ProfileEmpty(c);
            FillRound(g, hov || (save && g_saveOpen) ? T.ddBtnHover : T.ddBtn, r, S(16));
            if (active) FillRound(g, WithAlpha(T.accent, T.glass ? 46 : 40), r, S(16));
            if (save) {
                float tw = TextW(g, L"Save", g_fSmall), sx = r.X + (r.Width - (S(14) + S(6) + tw)) / 2;
                Glyph(g, 0xE74E, g_fIconS, T.t1, sx + S(7), r.Y + r.Height / 2);
                Text(g, L"Save", g_fSmall, T.t1, RectF(sx + S(20), r.Y, tw + S(4), r.Height));
            } else {
                std::wstring label = empty ? L"+ " + g_prof[c].name : g_prof[c].name;
                Text(g, label.c_str(), active ? g_fSmall600 : g_fSmall, empty ? T.t3 : T.t1, RectF(r.X + S(10), r.Y, r.Width - S(20), r.Height), StringAlignmentCenter);
            }
        }
        return;
    }
    if (e.k == E_HK) {
        float cy = e.r.Y + e.r.Height / 2;
        Text(g, HK_NAMES[e.idx], g_fName, T.t1, RectF(e.r.X + S(10), cy - S(10), e.r.Width - S(200), S(20)));
        RectF c = HkChip(e);
        bool hov = hovEl && g_hover.h == H_HKCHIP, cap = g_capture == e.idx;
        FillRound(g, cap ? WithAlpha(T.accent, 46) : hov ? T.ddBtnHover : T.ddBtn, c, S(8));
        std::wstring t = cap ? L"Press keys\u2026" : ComboName(g_hk[e.idx]);
        Color tc = cap ? T.t1 : (!g_hk[e.idx].vk ? T.t3 : (!g_hk[e.idx].ok ? Color(255, 232, 72, 72) : T.t1));
        if (!cap && g_hk[e.idx].vk && !g_hk[e.idx].ok) t += L"  (in use)";
        Text(g, t.c_str(), g_fSmall, tc, RectF(c.X + S(8), c.Y, c.Width - S(16), c.Height), StringAlignmentCenter);
        return;
    }
    if (e.k == E_OSDROW || e.k == E_STEPROW || e.k == E_RESETROW) {
        float cy = e.r.Y + e.r.Height / 2;
        const wchar_t* lbl = e.k == E_OSDROW ? L"Show on-screen popup" : e.k == E_STEPROW ? L"Volume step" : L"Shortcuts";
        Text(g, lbl, g_fName, T.t1, RectF(e.r.X + S(10), cy - S(10), S(200), S(20)));
        if (e.k == E_OSDROW) DrawSwitch(g, e.r.X + e.r.Width - S(6) - S(40), cy, g_osd);
        else if (e.k == E_STEPROW) {
            FillRound(g, T.tabsBg, StepBox(e), S(8));
            const wchar_t* st[3] = {L"2%", L"5%", L"10%"}; int vals[3] = {2, 5, 10};
            for (int c = 0; c < 3; c++) {
                RectF r = StepBtn(e, c);
                bool on = g_step == vals[c], hov = g_hover.h == H_STEP && g_hover.idx == c && hovEl;
                if (on) FillRound(g, T.tabSel, r, S(6)); else if (hov) FillRound(g, T.hover, r, S(6));
                Text(g, st[c], on ? g_fSmall600 : g_fSmall, T.t1, r, StringAlignmentCenter);
            }
        } else {
            RectF r = ResetBtn(e);
            FillRound(g, hovEl && g_hover.h == H_RESET ? T.ddBtnHover : T.ddBtn, r, S(8));
            Text(g, L"Reset to defaults", g_fSmall, T.t1, r, StringAlignmentCenter);
        }
        return;
    }
    MonUI& m = g_mons[e.idx];
    float cy = e.r.Y + e.r.Height / 2, lx = e.r.X + S(8) + S(14);
    if (e.k == E_MONHEAD) {
        RectF nb(lx - S(10), cy - S(7.5f), S(20), S(15));
        GraphicsPath p; AddRound(p, nb, S(3));
        Pen pen(T.t1, S(1.5f)); g.DrawPath(&pen, &p);
        wchar_t num[4]; swprintf(num, 4, L"%d", e.idx + 1);
        Text(g, num, g_fNum, T.t1, nb, StringAlignmentCenter);
        float nx = e.r.X + S(8) + S(28) + S(10), maxW = e.r.X + e.r.Width - S(6) - nx;
        float nw = std::min(TextW(g, m.name.c_str(), g_fName), maxW * 0.7f);
        Text(g, m.name.c_str(), g_fName, T.t1, RectF(nx, cy - S(10), nw + S(2), S(20)));
        if (!m.detail.empty()) Text(g, m.detail.c_str(), g_fSmall, T.t3, RectF(nx + nw + S(8), cy - S(9), std::max(0.f, maxW - nw - S(8)), S(18)));
        return;
    }
    wchar_t lead = e.k == E_MONB ? G_BRIGHT : e.k == E_MONC ? G_CONTRAST : G_ROTATE;
    Glyph(g, lead, g_fIcon, T.t1, e.r.X + S(6) + S(16), cy);
    RectF sl = CtlSlider(e, cy);
    if (e.k == E_MONO) {
        bool hov = hovEl && g_hover.h == H_DROPDOWN;
        FillRound(g, hov || g_ddOpen == e.idx ? T.ddBtnHover : T.ddBtn, sl, S(T.glass ? 9 : 6));
        int o = std::max(0, std::min(3, m.orient));
        Text(g, ORIENT_NAMES[o], g_fName, T.t1, RectF(sl.X + S(12), sl.Y, sl.Width - S(40), sl.Height));
        float gx = sl.X + sl.Width - S(18);
        if (g_ddOpen == e.idx) { g.TranslateTransform(gx, cy); g.RotateTransform(180); Glyph(g, G_CHEVDOWN, g_fIconS, T.t1, 0, 0); g.ResetTransform(); }
        else Glyph(g, G_CHEVDOWN, g_fIconS, T.t1, gx, cy);
        return;
    }
    if (!m.ok) { Text(g, L"Not available — turn on DDC/CI in the monitor menu", g_fSmall, T.t3, RectF(sl.X - S(8), sl.Y, sl.Width + S(46), sl.Height)); return; }
    float v = ElValue(e);
    int st = g_dragEl == index ? 2 : (hovEl && g_hover.h == H_SLIDER ? 1 : 0);
    DrawSlider(g, sl, v, false, st);
    DrawValue(g, CtlVal(e, cy), (int)lround(v * 100));
}

static void DrawDropdown(Graphics& g) {
    RectF btn;
    if (!DropdownRect(btn)) return;
    RectF pop = DropdownPopup(btn);
    float rad = S(T.glass ? 12 : 8);
    Shadow(g, pop, rad);
    FillRound(g, T.popup, pop, rad);
    for (int i = 0; i < 4; i++) {
        RectF r = DropdownOption(pop, i);
        bool cur = g_mons[g_ddOpen].orient == i, hov = g_hover.h == H_DDOPT && g_hover.idx == i;
        if (cur) {
            if (T.glass) FillRound(g, WithAlpha(T.accent, 36), r, S(8));
            else { FillRound(g, T.subtle, r, S(5)); FillRound(g, T.accent, RectF(r.X, r.Y + S(9), S(3), S(14)), S(1.5f)); }
        }
        if (hov) FillRound(g, T.subtle2, r, S(T.glass ? 8 : 5));
        Text(g, ORIENT_NAMES[i], g_fName, T.t1, RectF(r.X + S(12), r.Y, r.Width - S(16), r.Height));
    }
}

static void DrawDelayPopup(Graphics& g) {
    RectF chip;
    if (g_delayOpen < 0 || !DelayAnchor(chip)) return;
    RectF pop = DelayPopup(chip);
    float rad = S(T.glass ? 12 : 8);
    Shadow(g, pop, rad);
    FillRound(g, T.popup, pop, rad);
    Text(g, L"Sync delay", g_fName600, T.t1, RectF(pop.X + S(16), pop.Y + S(8), S(140), S(24)));
    wchar_t t[16]; swprintf(t, 16, L"%d ms", DelayOf(g_delayOpen));
    Text(g, t, g_fSmall600, T.t1, RectF(pop.X + pop.Width - S(86), pop.Y + S(8), S(70), S(24)), StringAlignmentFar);
    DrawSlider(g, DelaySlider(pop), DelayOf(g_delayOpen) / 500.f, false, g_dragDelay ? 2 : (g_hover.h == H_DELAYSL ? 1 : 0));
    Text(g, L"Delays this output so it lines up with the others", g_fSmall, T.t3, RectF(pop.X + S(16), pop.Y + S(66), pop.Width - S(32), S(22)));
}

static void DrawSavePopup(Graphics& g) {
    RectF chip;
    if (!g_saveOpen || !SaveAnchor(chip)) return;
    RectF pop = SavePopup(chip);
    float rad = S(T.glass ? 12 : 8);
    Shadow(g, pop, rad);
    FillRound(g, T.popup, pop, rad);
    for (int i = 0; i < SaveOptCount(); i++) {
        RectF r = SaveOption(pop, i);
        if (g_hover.h == H_SAVEOPT && g_hover.idx == i) FillRound(g, T.subtle2, r, S(T.glass ? 8 : 5));
        bool isNew = i == (int)g_prof.size();
        std::wstring t = isNew ? std::wstring(L"New profile\u2026") : L"Save as " + g_prof[i].name;
        if (isNew) { Glyph(g, 0xE710, g_fIconS, T.t1, r.X + S(18), r.Y + r.Height / 2); Text(g, t.c_str(), g_fName, T.t1, RectF(r.X + S(34), r.Y, r.Width - S(38), r.Height)); }
        else Text(g, t.c_str(), g_fName, T.t1, RectF(r.X + S(12), r.Y, r.Width - S(16), r.Height));
    }
}

static void DrawMenu(Graphics& g) {
    if (!g_menuOpen) return;
    BuildMenu();
    float rad = S(10);
    Shadow(g, g_menuBox, rad);
    FillRound(g, T.popup, g_menuBox, rad);
    for (auto& m : g_menu) {
        bool hov = g_hover.h == H_MENU && g_hover.idx == m.id;
        float cy = m.r.Y + m.r.Height / 2;
        switch (m.type) {
        case MT_SWITCH: case MT_LINK: case MT_ITEM: case MT_NAV: {
            if (hov) FillRound(g, T.subtle, m.r, S(6));
            Glyph(g, m.glyph, g_fIconS, T.t1, m.r.X + S(10) + S(8), cy);
            Text(g, m.text.c_str(), g_fName, T.t1, RectF(m.r.X + S(10) + S(16) + S(12), m.r.Y, m.r.Width - S(100), m.r.Height));
            if (m.type == MT_SWITCH) DrawSwitch(g, m.r.X + m.r.Width - S(10) - S(40), cy, m.id == M_AWAKE ? g_keepAwake : m.id == M_RESTORE ? g_restoreBright : StartupEnabled());
            if (m.type == MT_LINK) Glyph(g, G_OPEN, g_fIconS, T.t2, m.r.X + m.r.Width - S(10) - S(8), cy);
            if (m.type == MT_NAV) Glyph(g, 0xE76C, g_fIconS, T.t2, m.r.X + m.r.Width - S(10) - S(8), cy);
            break;
        }
        case MT_LABEL:
            if (m.text == L"#seg") FillRound(g, T.subtle, m.r, S(8));
            else Text(g, m.text.c_str(), g_fSmall, T.t3, RectF(m.r.X + S(10), m.r.Y + S(6), m.r.Width - S(10), m.r.Height - S(6)));
            break;
        case MT_SEG: {
            bool on = (m.id == M_SOLID) == (g_style == 0);
            if (on) {
                if (!T.dark) FillRound(g, Color(30, 0, 0, 0), RectF(m.r.X, m.r.Y + S(1), m.r.Width, m.r.Height), S(6));
                FillRound(g, T.dark ? T.subtle3 : Color(255, 255, 255, 255), m.r, S(6));
            } else if (hov) FillRound(g, T.subtle, m.r, S(6));
            Text(g, m.text.c_str(), on ? g_fSmall600 : g_fSmall, on ? T.t1 : T.t2, m.r, StringAlignmentCenter);
            break;
        }
        case MT_SWATCH: {
            int i = m.id - M_ACC0;
            RectF r = m.r;
            if (i == g_accentId) {
                SolidBrush ring(T.t1); g.FillEllipse(&ring, r.X - S(4), r.Y - S(4), r.Width + S(8), r.Height + S(8));
                SolidBrush gap(T.popup.GetA() == 255 ? T.popup : (T.dark ? Color(255, 40, 40, 40) : Color(255, 249, 249, 249)));
                g.FillEllipse(&gap, r.X - S(2), r.Y - S(2), r.Width + S(4), r.Height + S(4));
            }
            if (i == 0) {   // Windows accent: multicolor
                Color cs[4] = {Hex(0x60cdff), Hex(0xc3a0ff), Hex(0xff8fa8), Hex(0x6ccb5f)};
                for (int q = 0; q < 4; q++) { SolidBrush b(cs[q]); g.FillPie(&b, r, -90.f + q * 90.f, 90.f); }
            } else { SolidBrush b(Hex(T.dark ? ACCENTS[i].dark : ACCENTS[i].light)); g.FillEllipse(&b, r); }
            break;
        }
        case MT_VERSION: Text(g, m.text.c_str(), g_fSmall, T.t3, RectF(m.r.X + S(10), m.r.Y, m.r.Width, m.r.Height)); break;
        }
    }
}

static bool g_dwmRound = false;

static void Paint(HDC hdc) {
    if (g_W <= 0 || g_H <= 0) return;
    // 32-bit premultiplied canvas: alpha is honoured by DWM when Glass (acrylic) is on
    BITMAPINFO bi = {}; bi.bmiHeader.biSize = sizeof(bi.bmiHeader); bi.bmiHeader.biWidth = g_W; bi.bmiHeader.biHeight = -g_H;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(hdc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dib) return;
    HDC mdc = CreateCompatibleDC(hdc);
    HGDIOBJ old = SelectObject(mdc, dib);
    {
        Bitmap canvas(g_W, g_H, g_W * 4, PixelFormat32bppPARGB, (BYTE*)bits);
        Graphics g(&canvas);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetTextRenderingHint(T.glass ? TextRenderingHintAntiAliasGridFit : TextRenderingHintClearTypeGridFit);
        g.SetPixelOffsetMode(PixelOffsetModeHalf);
        g.SetCompositingMode(CompositingModeSourceCopy);
        { SolidBrush clr(T.surface); g.FillRectangle(&clr, 0, 0, g_W, g_H); }
        g.SetCompositingMode(CompositingModeSourceOver);
        Layout(); ClampScroll(); Layout();

        // pane
        g.SetClip(RectF(0, PaneTop(), (float)g_W, PaneBottom() - PaneTop()));
        for (int i = 0; i < (int)g_els.size(); i++) DrawEl(g, i);
        float view = PaneBottom() - PaneTop();
        if (g_contentH > view + 1) {
            float th = std::max(S(28), view * view / g_contentH);
            float ty = PaneTop() + S(4) + (view - th - S(8)) * (g_scroll[SI()] / (g_contentH - view));
            FillRound(g, T.t3, RectF(g_W - S(6), ty, S(3), th), S(1.5f));
        }
        g.ResetClip();

        // header strip: repaint the surface so scrolled rows never show behind the tabs
        g.SetCompositingMode(CompositingModeSourceCopy);
        { SolidBrush sb(T.surface); g.FillRectangle(&sb, RectF(0, 0, (float)g_W, PaneTop())); g.FillRectangle(&sb, RectF(0, PaneBottom(), (float)g_W, S(FTR))); }
        g.SetCompositingMode(CompositingModeSourceOver);

        if (g_view == 1) {   // shortcuts page header
            RectF b = BackRect();
            if (g_hover.h == H_BACK) FillRound(g, T.hover, b, S(6));
            Glyph(g, 0xE72B, g_fIcon, T.t1, b.X + b.Width / 2, b.Y + b.Height / 2);
            Font hf(g_famSemi, S(15), FontStyleRegular, UnitPixel);
            Text(g, L"Keyboard shortcuts", &hf, T.t1, RectF(b.X + b.Width + S(8), b.Y, S(260), b.Height));
        } else {
        // tabs
        FillRound(g, T.tabsBg, RectF(S(12), S(12), g_W - S(24), S(38)), S(8));
        const wchar_t* names[3] = {L"Sound", L"Apps", L"Display"};
        wchar_t glyphs[3] = {G_VOLUME, G_APPS, G_BRIGHT};
        for (int i = 0; i < 3; i++) {
            RectF r = TabRect(i);
            bool sel = g_tab == i, hov = g_hover.h == H_TAB && g_hover.idx == i;
            if (sel) {
                if (!T.glass) FillRound(g, Color(T.dark ? 40 : 30, 0, 0, 0), RectF(r.X, r.Y + S(1), r.Width, r.Height), S(6));
                FillRound(g, T.tabSel, r, S(6));
            } else if (hov) FillRound(g, T.hover, r, S(6));
            Font* f = sel ? g_fTab600 : g_fTab;
            float tw = TextW(g, names[i], f), total = S(15) + S(8) + tw, sx = r.X + (r.Width - total) / 2;
            Glyph(g, glyphs[i], g_fIconS, T.t1, sx + S(7.5f), r.Y + r.Height / 2);
            Text(g, names[i], f, T.t1, RectF(sx + S(15) + S(8), r.Y, tw + S(4), r.Height));
        }
        }

        // footer (same surface as the panel: no separate band)
        float fy = PaneBottom() + S(FTR) / 2;
        Glyph(g, G_EQ, g_fIconS, T.t1, S(16) + S(7), fy);
        {   // "Knob" with the version (or the update pill) underneath
            float nx = S(16) + S(14) + S(10);
            Text(g, APP_NAME, g_fSmall600, T.t1, RectF(nx, fy - S(17), S(120), S(17)));
            std::wstring vt = L"v" APP_VERSION;
            bool upd = g_updState != UPD_NONE;
            if (g_updState == UPD_AVAILABLE) vt = L"Update to v" + g_updVersion;
            else if (g_updState == UPD_DOWNLOADING) vt = L"Updating\u2026";
            else if (g_updState == UPD_FAILED) vt = L"Update failed \u00B7 retry";
            float vw = TextW(g, vt.c_str(), g_fSmall);
            if (upd) {
                g_verRect = RectF(nx - S(6), fy + S(1), vw + S(12), S(17));
                bool hov = g_hover.h == H_UPDATE;
                FillRound(g, Color((BYTE)(hov ? 80 : 50), 232, 72, 72), g_verRect, S(8.5f));
                Text(g, vt.c_str(), g_fSmall, Color(255, 240, 90, 90), g_verRect, StringAlignmentCenter);
            } else {
                g_verRect = RectF();
                Text(g, vt.c_str(), g_fSmall, T.t3, RectF(nx, fy + S(1), vw + S(8), S(16)));
            }
        }
        {   // mute every microphone
            RectF r = MicAllRect();
            bool muted = AllMicsMuted();
            if (muted) FillRound(g, Color(60, 232, 72, 72), r, S(6));
            else if (g_hover.h == H_MICALL) FillRound(g, T.hover, r, S(6));
            Glyph(g, muted ? G_MICOFF : G_MIC, g_fIcon, muted ? Color(255, 232, 72, 72) : T.t1, r.X + r.Width / 2, r.Y + r.Height / 2);
        }
        {   // refresh (spins while rescanning)
            RectF r = RefreshRect();
            if (g_hover.h == H_REFRESH) FillRound(g, T.hover, r, S(6));
            DWORD now = GetTickCount();
            if (now < g_spinUntil) {
                g.TranslateTransform(r.X + r.Width / 2, r.Y + r.Height / 2);
                g.RotateTransform((float)((now % 700) * 360 / 700));
                Glyph(g, G_REFRESH, g_fIcon, T.t1, 0, 0);
                g.ResetTransform();
            } else Glyph(g, G_REFRESH, g_fIcon, T.t1, r.X + r.Width / 2, r.Y + r.Height / 2);
        }
        DrawIconBtn(g, IdentRect(), G_IDENT, T.t1, g_hover.h == H_IDENT);
        DrawIconBtn(g, GearRect(), G_SETTINGS, T.t1, g_hover.h == H_GEAR || g_menuOpen);

        // popups on top
        DrawDropdown(g);
        DrawSavePopup(g);
        DrawDelayPopup(g);
        DrawMenu(g);
        if (!g_dwmRound) { Pen bp(T.subtle3, S(1)); g.DrawRectangle(&bp, RectF(0, 0, g_W - 1.f, g_H - 1.f)); }
    }
    BitBlt(hdc, 0, 0, g_W, g_H, mdc, 0, 0, SRCCOPY);
    SelectObject(mdc, old);
    DeleteObject(dib);
    DeleteDC(mdc);
}

static void ShowFlyout();
static void PairHide() {   // Settings lost the foreground to another app: tuck the panel away with it
    if (!g_set || !g_visible || g_pairHidden) return;
    g_pairHidden = true;
    ShowWindow(g_hwnd, SW_HIDE);
}
static void PairShow(bool activateSettings) {   // Settings came back: bring the panel back beside it
    if (!g_set) return;
    if (g_pairHidden) { g_pairHidden = false; ShowWindow(g_hwnd, SW_SHOWNOACTIVATE); InvalidateRect(g_hwnd, nullptr, FALSE); }
    if (activateSettings) { if (IsIconic(g_set)) ShowWindow(g_set, SW_RESTORE); SetForegroundWindow(g_set); }
}
static void HideFlyout();
static void OpenSettings(int page, bool newProfile);

// ---------------------------------------------------------------- on-screen popup for shortcuts
enum { OSD_VOL, OSD_MIC, OSD_BRIGHT };
static HWND g_osdWnd;
static int g_osdKind = OSD_VOL, g_osdVal = 0; static bool g_osdMuted = false;
static LRESULT CALLBACK OsdProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
        RECT rc; GetClientRect(h, &rc);
        {
            Graphics g(dc);
            g.SetSmoothingMode(SmoothingModeAntiAlias);
            g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
            g.Clear(T.dark ? Color(255, 44, 44, 44) : Color(255, 249, 249, 249));
            float W = (float)rc.right, H = (float)rc.bottom, cy = H / 2;
            wchar_t gl = g_osdKind == OSD_BRIGHT ? G_BRIGHT : g_osdKind == OSD_MIC ? (g_osdMuted ? G_MICOFF : G_MIC) : (g_osdMuted ? G_MUTE : G_VOLUME);
            Glyph(g, gl, g_fIcon, T.t1, S(28), cy);
            RectF bar(S(52), cy - S(2), W - S(52) - S(52), S(4));
            FillRound(g, T.track, bar, S(2));
            float v = g_osdMuted ? 0.f : g_osdVal / 100.f;
            if (v > 0) FillRound(g, T.accent, RectF(bar.X, bar.Y, bar.Width * v, bar.Height), S(2));
            wchar_t t[16];
            if (g_osdKind == OSD_MIC) swprintf(t, 16, g_osdMuted ? L"Off" : L"On");
            else if (g_osdMuted) swprintf(t, 16, L"Mute");
            else swprintf(t, 16, L"%d", g_osdVal);
            Text(g, t, g_fSmall600, T.t1, RectF(W - S(46), 0, S(36), H), StringAlignmentCenter);
        }
        EndPaint(h, &ps);
        return 0;
    }
    if (msg == WM_NCHITTEST) return HTTRANSPARENT;
    return DefWindowProcW(h, msg, wp, lp);
}
static void ShowOsd(int kind, int val, bool muted) {
    if (!g_osd) return;
    g_osdKind = kind; g_osdVal = val; g_osdMuted = muted;
    if (kind == OSD_MIC) g_osdVal = muted ? 0 : 100;
    POINT pt; GetCursorPos(&pt);
    HMONITOR hm = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = {sizeof(mi)}; GetMonitorInfoW(hm, &mi);
    int w = (int)S(240), h = (int)S(48);
    int x = (mi.rcWork.left + mi.rcWork.right - w) / 2, y = mi.rcWork.bottom - h - (int)S(64);
    if (!g_osdWnd) {
        g_osdWnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
                                   L"KnobOSD", L"", WS_POPUP, x, y, w, h, nullptr, nullptr, g_inst, nullptr);
        if (!g_osdWnd) return;
        SetLayeredWindowAttributes(g_osdWnd, 0, 245, LWA_ALPHA);
        DWORD pref = 2; DwmSetWindowAttribute(g_osdWnd, 33, &pref, sizeof(pref));
    }
    SetWindowPos(g_osdWnd, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(g_osdWnd, nullptr, FALSE);
    SetTimer(g_hwnd, TIMER_OSD, 1400, nullptr);
}

static void RunHotkey(int a) {
    if (a == HK_OPEN) { if (g_set && (g_visible || g_pairHidden)) PairShow(true); else if (g_visible) HideFlyout(); else ShowFlyout(); }
}

// ---------------------------------------------------------------- style (Solid / Glass)
// Acrylic blur with our own tint (the system backdrop's built-in tint is too heavy to read as glass)
struct KnobAccentPolicy { int state; int flags; DWORD gradient; int anim; };
struct KnobWinCompAttr { int attr; PVOID data; SIZE_T size; };
typedef BOOL (WINAPI* PFN_SWCA)(HWND, KnobWinCompAttr*);
static bool SetAccent(HWND h, int state, DWORD abgr) {
    static PFN_SWCA swca = (PFN_SWCA)GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetWindowCompositionAttribute");
    if (!swca) return false;
    KnobAccentPolicy ap = {state, 0, abgr, 0};
    KnobWinCompAttr d = {19 /*WCA_ACCENT_POLICY*/, &ap, sizeof(ap)};
    return swca(h, &d) != FALSE;
}
static bool TransparencyOn() {   // Settings > Personalization > Colors > Transparency effects
    DWORD v = 1, sz = sizeof(v);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"EnableTransparency", RRF_RT_REG_DWORD, nullptr, &v, &sz);
    return v != 0;
}
static void ApplyStyle() {
    LoadTheme();   // need T.dark for the tint
    bool want = g_style == 1 && TransparencyOn();
    int none = 1; DwmSetWindowAttribute(g_hwnd, 38, &none, sizeof(none));   // no system backdrop; we blur ourselves
    // tint is AABBGGRR: dark ~45% #1C1C1C, light ~55% #F3F3F3
    DWORD tint = T.dark ? 0x731C1C1C : 0x8CF3F3F3;
    T.glass = want && SetAccent(g_hwnd, 4 /*ACCENT_ENABLE_ACRYLICBLURBEHIND*/, tint);
    if (!T.glass) SetAccent(g_hwnd, 0, 0);
    MARGINS m = T.glass ? MARGINS{-1, -1, -1, -1} : MARGINS{0, 0, 0, 0};
    DwmExtendFrameIntoClientArea(g_hwnd, &m);
    BOOL dark = T.dark;
    DwmSetWindowAttribute(g_hwnd, 20, &dark, sizeof(dark));
    LoadTheme();
    if (g_visible) InvalidateRect(g_hwnd, nullptr, FALSE);
}

// ---------------------------------------------------------------- identify displays
static std::vector<HWND> g_identWnds;
static LRESULT CALLBACK IdentProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
        RECT rc; GetClientRect(h, &rc);
        {
            Graphics g(dc);
            g.SetSmoothingMode(SmoothingModeAntiAlias);
            g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
            g.Clear(T.surface);
            int num = (int)GetWindowLongPtrW(h, GWLP_USERDATA);
            wchar_t s[4]; swprintf(s, 4, L"%d", num);
            Font f(g_famSemi, (REAL)(rc.bottom * 0.6), FontStyleRegular, UnitPixel);
            StringFormat sf; sf.SetAlignment(StringAlignmentCenter); sf.SetLineAlignment(StringAlignmentCenter);
            SolidBrush b(T.t1);
            g.DrawString(s, -1, &f, RectF(0, 0, (REAL)rc.right, (REAL)rc.bottom), &sf, &b);
        }
        EndPaint(h, &ps);
        return 0;
    }
    if (msg == WM_NCHITTEST) return HTTRANSPARENT;
    return DefWindowProcW(h, msg, wp, lp);
}
static void CloseIdentify() { for (HWND w : g_identWnds) DestroyWindow(w); g_identWnds.clear(); }
static void IdentifyDisplays() {
    CloseIdentify();
    for (int i = 0; i < (int)g_mons.size(); i++) {
        RECT r = g_mons[i].rc;
        int sz = std::max(120, std::min(220, (int)(r.bottom - r.top) / 5));
        int x = r.left + (r.right - r.left - sz) / 2, y = r.top + (r.bottom - r.top - sz) / 2;
        HWND w = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
                                 L"KnobIdent", L"", WS_POPUP, x, y, sz, sz, nullptr, nullptr, g_inst, nullptr);
        if (!w) continue;
        SetWindowLongPtrW(w, GWLP_USERDATA, i + 1);
        SetLayeredWindowAttributes(w, 0, 235, LWA_ALPHA);
        DWORD pref = 2; DwmSetWindowAttribute(w, 33, &pref, sizeof(pref));
        ShowWindow(w, SW_SHOWNOACTIVATE);
        g_identWnds.push_back(w);
    }
    SetTimer(g_hwnd, TIMER_IDENT, 2500, nullptr);
}

// ---------------------------------------------------------------- flyout show/hide
static void ApplyDpi(int dpi) {
    if (dpi == g_dpi && g_fName) return;
    g_dpi = dpi; g_scale = dpi / 96.f;
    InitFonts();
    ClearProcCache();
}

static void PositionFlyout() {
    POINT pt; GetCursorPos(&pt);
    if (g_visible) { RECT r; GetWindowRect(g_hwnd, &r); pt.x = r.right - 1; pt.y = r.bottom - 1; }
    HMONITOR hm = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    UINT dx = 96, dy = 96;
    GetDpiForMonitor(hm, MDT_EFFECTIVE_DPI, &dx, &dy);
    ApplyDpi((int)dx);
    MONITORINFO mi = {sizeof(mi)};
    GetMonitorInfoW(hm, &mi);
    RECT wa = mi.rcWork, mr = mi.rcMonitor;
    int w = (int)S(LW);
    int maxH = (wa.bottom - wa.top) - (int)S(24);
    int h = std::min((int)(S(TABH) + S(PANEH) + S(FTR)), maxH);   // fixed size; content scrolls
    g_W = w; g_H = h;
    int m = (int)S(12);
    int x = wa.right - w - m, y = wa.bottom - h - m;
    if (wa.top > mr.top) y = wa.top + m;
    if (wa.left > mr.left) x = wa.left + m;
    SetWindowPos(g_hwnd, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE);
}

static void StartAnim() { if (g_visible && g_tab == T_APPS) SetTimer(g_hwnd, TIMER_ANIM, 70, nullptr); else KillTimer(g_hwnd, TIMER_ANIM); }

static void ShowFlyout() {
    if (g_pairHidden) { PairShow(true); return; }   // Settings is open: bring the pair back
    LoadEndpoints(eRender, g_out);
    LoadEndpoints(eCapture, g_in);
    LoadApps();
    UpdatePeaks();
    UpdateTrayTip();
    BrightRequest(!g_monsLoaded, g_monsLoaded);
    PositionFlyout();
    ShowWindow(g_hwnd, SW_SHOW);
    SetForegroundWindow(g_hwnd);
    g_visible = true;
    SetTimer(g_hwnd, TIMER_REFRESH, 1000, nullptr);
    StartAnim();
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

static void HideFlyout() {
    if (!g_visible) return;
    if (g_set) return;   // the panel stays while Settings is open (Esc, clicking away, tray and shortcut all wait)
    g_visible = false;
    g_dragEl = -1; g_menuOpen = false; g_ddOpen = -1; g_saveOpen = false; g_view = 0; g_delayOpen = -1; g_dragDelay = false;
    SaveSettings();
    if (g_capture >= 0) { g_capture = -1; RegisterHotkeys(); }
    if (GetCapture() == g_hwnd) ReleaseCapture();
    KillTimer(g_hwnd, TIMER_REFRESH);
    KillTimer(g_hwnd, TIMER_ANIM);
    ShowWindow(g_hwnd, SW_HIDE);
    g_hideTick = GetTickCount();
    FreeApps();
}

static void SetTab(int t) {
    if (t == g_tab) return;
    g_tab = t;
    g_hover = Hit(); g_ddOpen = -1; g_menuOpen = false; g_saveOpen = false; g_delayOpen = -1;
    if (t == T_APPS) { LoadApps(); UpdatePeaks(); }
    if (t == T_DISPLAY) BrightRequest(!g_monsLoaded, g_monsLoaded);
    SaveSettings();
    StartAnim();
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

// ---------------------------------------------------------------- output selection
static void ToggleOutput(int i) {
    Endpoint& e = g_out[i];
    if (e.isDefault) {
        // turning off the output Windows uses: hand the role to another selected output (if any)
        for (int j = 0; j < (int)g_out.size(); j++) {
            if (j != i && g_mirror.count(g_out[j].id)) {
                std::wstring nid = g_out[j].id;
                g_mirror.erase(nid);
                SetDefaultDevice(nid);
                break;
            }
        }
    } else if (g_mirror.count(e.id)) g_mirror.erase(e.id);
    else g_mirror.insert(e.id);
    SaveSettings();
    LoadEndpoints(eRender, g_out);
    RestartRouter();
    UpdateTrayTip();
}

static void SetMulti(bool on) {
    if (on == g_multi) return;
    g_multi = on;
    if (!on) {   // back to just the main output; sync delays start fresh next time
        if (!g_mirror.empty()) g_mirrorSaved = g_mirror;
        for (auto& id : g_mirrorSaved) g_delay.erase(id);
        for (auto& o : g_out) g_delay.erase(o.id);
        g_mirror.clear();
        g_calStatus.clear();
    }
    else {   // bring the last group back
        int di = DefaultOut();
        for (auto& id : g_mirrorSaved) if (di < 0 || g_out[di].id != id) for (auto& o : g_out) if (o.id == id) g_mirror.insert(id);
    }
    SaveSettings();
    RestartRouter();
    UpdateTrayTip();
}
static void ClickOutput(int i) {
    if (g_multi) { ToggleOutput(i); return; }
    if (!g_out[i].isDefault) { SetDefaultDevice(g_out[i].id); LoadEndpoints(eRender, g_out); UpdateTrayTip(); }   // plain switch
}

// ---------------------------------------------------------------- tray menu (native, follows dark mode)
static void AllowDarkMenus() {   // undocumented uxtheme ordinals
    HMODULE ux = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!ux) return;
    typedef int(WINAPI * SetPreferredAppMode_t)(int);
    typedef void(WINAPI * FlushMenuThemes_t)();
    auto setMode = (SetPreferredAppMode_t)(void*)GetProcAddress(ux, MAKEINTRESOURCEA(135));
    auto flush = (FlushMenuThemes_t)(void*)GetProcAddress(ux, MAKEINTRESOURCEA(136));
    if (setMode) setMode(1);
    if (flush) flush();
}

static void ShowTrayMenu() {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, 1, L"Open");
    AppendMenuW(m, MF_STRING, 4, L"Settings");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (g_keepAwake ? MF_CHECKED : 0), 2, L"Keep awake");
    AppendMenuW(m, MF_STRING | (StartupEnabled() ? MF_CHECKED : 0), 3, L"Start with Windows");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | MF_GRAYED, 0, APP_NAME L" " APP_VERSION);
    AppendMenuW(m, MF_STRING, 9, L"Exit");
    POINT pt; GetCursorPos(&pt);
    SetForegroundWindow(g_hwnd);
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, g_hwnd, nullptr);
    DestroyMenu(m);
    switch (cmd) {
    case 1: ShowFlyout(); break;
    case 2: g_keepAwake = !g_keepAwake; ApplyKeepAwake(); SaveSettings(); break;
    case 3: SetStartup(!StartupEnabled()); break;
    case 4: OpenSettings(0, false); break;
    case 9: DestroyWindow(g_hwnd); break;
    }
}

static void MenuCommand(int id) {
    switch (id) {
    case M_AWAKE: g_keepAwake = !g_keepAwake; ApplyKeepAwake(); SaveSettings(); break;
    case M_STARTUP: SetStartup(!StartupEnabled()); break;
    case M_RESTORE: g_restoreBright = !g_restoreBright; SaveSettings(); break;
    case M_SOUND: HideFlyout(); ShellExecuteW(nullptr, L"open", L"ms-settings:sound", nullptr, nullptr, SW_SHOWNORMAL); return;
    case M_DISPLAY: HideFlyout(); ShellExecuteW(nullptr, L"open", L"ms-settings:display", nullptr, nullptr, SW_SHOWNORMAL); return;
    case M_EXIT: DestroyWindow(g_hwnd); return;
    case M_SHORTCUTS: g_menuOpen = false; g_view = 1; g_scroll[3] = 0; g_hover = Hit(); break;
    case M_SOLID: g_style = 0; SaveSettings(); ApplyStyle(); break;
    case M_GLASS: g_style = 1; SaveSettings(); ApplyStyle(); break;
    default:
        if (id >= M_ACC0 && id < M_ACC0 + 7) {
            static const int hues[7] = {0, 181, 113, 37, 346, 262, 0};
            int k = id - M_ACC0; g_accentId = k ? 1 : 0;
            if (k) { g_accentHue = hues[k]; g_accentSat = k == 6 ? 0 : 85; }
            SaveSettings(); LoadTheme();
        }
        break;
    }
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

// ---------------------------------------------------------------- refresh everything
static void RefreshAll() {
    LoadEndpoints(eRender, g_out);
    LoadEndpoints(eCapture, g_in);
    if (g_tab == T_APPS) { LoadApps(); UpdatePeaks(); }
    RestartRouter();
    UpdateTrayTip();
    BrightRequest(true, false);   // re-detect displays and read brightness/contrast/orientation again
    g_spinUntil = GetTickCount() + 900;
    SetTimer(g_hwnd, TIMER_SPIN, 30, nullptr);
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

// ---------------------------------------------------------------- updates from GitHub releases
static int ParseVer(const std::wstring& v) {   // "v1.10.2" -> packed number
    int a = 0, b = 0, c = 0;
    swscanf(v.c_str() + (v.size() && (v[0] == L'v' || v[0] == L'V') ? 1 : 0), L"%d.%d.%d", &a, &b, &c);
    return (a << 16) | (b << 8) | c;
}
static bool HttpGet(const std::wstring& url, std::string& out, const std::wstring& saveTo = L"", DWORD* status = nullptr) {
    URL_COMPONENTS uc = {sizeof(uc)};
    wchar_t host[256] = {}, path[2048] = {};
    uc.lpszHostName = host; uc.dwHostNameLength = 256; uc.lpszUrlPath = path; uc.dwUrlPathLength = 2048;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) return false;
    HINTERNET ses = WinHttpOpen(L"Knob/" APP_VERSION, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) return false;
    bool ok = false;
    HINTERNET con = WinHttpConnect(ses, host, uc.nPort, 0);
    HINTERNET req = con ? WinHttpOpenRequest(con, L"GET", path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0) : nullptr;
    if (req) {
        if (WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(req, nullptr)) {
            DWORD code = 0, sz = sizeof(code);
            WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &code, &sz, WINHTTP_NO_HEADER_INDEX);
            if (status) *status = code;
            if (code == 200) {
                HANDLE f = saveTo.empty() ? INVALID_HANDLE_VALUE : CreateFileW(saveTo.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                ok = saveTo.empty() || f != INVALID_HANDLE_VALUE;
                DWORD avail = 0;
                while (ok && WinHttpQueryDataAvailable(req, &avail) && avail) {
                    std::vector<char> buf(avail); DWORD rd = 0;
                    if (!WinHttpReadData(req, buf.data(), avail, &rd)) { ok = false; break; }
                    if (f != INVALID_HANDLE_VALUE) { DWORD wr; WriteFile(f, buf.data(), rd, &wr, nullptr); }
                    else out.append(buf.data(), rd);
                }
                if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
            }
        }
        WinHttpCloseHandle(req);
    }
    if (con) WinHttpCloseHandle(con);
    WinHttpCloseHandle(ses);
    return ok;
}
struct UpdInfo { std::wstring version, url; };
// Updates come straight from the repo: dist/version.txt + dist/Knob.exe on the main branch
#define UPDATE_RAW L"https://raw.githubusercontent.com/" UPDATE_REPO L"/main/dist/"
static DWORD WINAPI UpdateCheckProc(LPVOID) {
    std::string j;
    UpdInfo* info = nullptr;
    DWORD code = 0;
    WPARAM kind = 0;   // 0 = result or unreachable, 2 = no build in the repo yet
    if (HttpGet(UPDATE_RAW L"version.txt", j, L"", &code)) {
        std::string v;
        for (char c : j) if ((c >= '0' && c <= '9') || c == '.') v += c; else if (!v.empty()) break;
        if (!v.empty()) info = new UpdInfo{std::wstring(v.begin(), v.end()), UPDATE_RAW L"Knob.exe"};
        else kind = 2;
    } else if (code == 404) kind = 2;
    if (!g_hwnd || !PostMessageW(g_hwnd, WM_APP_UPDINFO, kind, (LPARAM)info)) delete info;
    InterlockedExchange(&g_updBusy, 0);
    return 0;
}
static void CheckForUpdates() {
    if (InterlockedExchange(&g_updBusy, 1)) return;
    HANDLE t = CreateThread(nullptr, 0, UpdateCheckProc, nullptr, 0, nullptr);
    if (t) CloseHandle(t); else InterlockedExchange(&g_updBusy, 0);
}
static DWORD WINAPI UpdateDownloadProc(LPVOID p) {   // like G-Helper: download, then the new exe replaces this one and restarts
    std::wstring* url = (std::wstring*)p;
    wchar_t tmp[MAX_PATH]; GetTempPathW(MAX_PATH, tmp);
    std::wstring file = std::wstring(tmp) + L"Knob-update-" + std::to_wstring(GetTickCount()) + L".exe";   // fresh name: an old copy can't block it
    std::string dummy;
    DWORD code = 0;
    bool ok = HttpGet(*url, dummy, file, &code);
    if (!ok) { Sleep(1500); ok = HttpGet(*url, dummy, file, &code); }   // one retry
    delete url;
    std::wstring err;
    if (!ok) err = code ? L"GitHub answered HTTP " + std::to_wstring(code) : L"Couldn't download it";
    else {   // must be a real exe, not an error page
        HANDLE f = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        char mz[2] = {}; DWORD rd = 0; LARGE_INTEGER sz = {};
        if (f != INVALID_HANDLE_VALUE) { ReadFile(f, mz, 2, &rd, nullptr); GetFileSizeEx(f, &sz); CloseHandle(f); }
        if (rd != 2 || mz[0] != 'M' || mz[1] != 'Z' || sz.QuadPart < 100000) err = L"The download wasn't a valid Knob.exe";
    }
    if (err.empty()) {
        std::wstring cmd = L"\"" + file + L"\" --update";
        STARTUPINFOW si = {sizeof(si)}; PROCESS_INFORMATION pi = {};
        if (CreateProcessW(file.c_str(), &cmd[0], nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
            CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
            return 0;   // the new copy closes us
        }
        DWORD e = GetLastError();
        err = e == ERROR_ACCESS_DENIED || e == 1260 /*blocked by policy*/ || e == 4551 /*Smart App Control*/
            ? L"Windows blocked the new version from starting (error " + std::to_wstring(e) + L")"
            : L"Couldn't start the new version (error " + std::to_wstring(e) + L")";
    }
    if (!err.empty()) DeleteFileW(file.c_str());
    PostMessageW(g_hwnd, WM_APP_UPDINFO, 1, (LPARAM)new std::wstring(err));
    return 0;
}
static void StartUpdate() {
    if (g_updUrl.empty()) return;
    g_updState = UPD_DOWNLOADING;
    HANDLE t = CreateThread(nullptr, 0, UpdateDownloadProc, new std::wstring(g_updUrl), 0, nullptr);
    if (t) CloseHandle(t); else g_updState = UPD_FAILED;
    if (g_visible) InvalidateRect(g_hwnd, nullptr, FALSE);
}

// ---------------------------------------------------------------- settings window
enum { SP_GENERAL, SP_SOUND, SP_DISPLAY, SP_APPEAR, SP_ABOUT, SP_COUNT };
static const wchar_t* SP_NAMES[SP_COUNT] = {L"General", L"Sound", L"Display", L"Appearance", L"About"};
static const wchar_t SP_GLYPHS[SP_COUNT] = {0xE713, 0xE767, 0xE7F4, 0xE790, 0xE946};
enum SKind { SK_NAV, SK_SWITCH, SK_LINK, SK_BUTTON, SK_STYLE, SK_SWATCH, SK_HK, SK_STEP, SK_PAPPLY, SK_PUPDATE, SK_PRENAME, SK_PDELETE, SK_PNEW, SK_DELAY, SK_RESETHK, SK_ACCMODE, SK_COLOR };
struct SItem { int kind; int id; RectF r; };
enum { WM_APP_STARTEDIT = WM_APP + 20 };

static HWND g_edit;
static WNDPROC g_editProc;
static HFONT g_editFont;
static HBRUSH g_editBrush;
static int g_setPage = SP_GENERAL, g_editProfile = -1, g_sDragDelay = -1, g_sDragColor = 0;   // 1 hue, 2 intensity
static int g_setW = 0, g_setH = 0;
static float g_setScroll = 0, g_setContentH = 0;
static std::vector<SItem> g_sItems;
static int g_sHover = -1;
static Theme g_setTheme;
static RectF g_editRect;
static const float NAVW = 240;

static bool g_setGlass = false;   // Settings window uses the Glass style too
static void ApplySettingsStyle() {
    if (!g_set) return;
    g_setGlass = false;   // Settings always uses the solid look; Glass is for the panel only
    BOOL dark = g_setTheme.dark; DwmSetWindowAttribute(g_set, 20, &dark, sizeof(dark));
    int backdrop = g_setGlass ? 3 : 1;   // acrylic behind the whole window (title bar included), or none
    DwmSetWindowAttribute(g_set, 38, &backdrop, sizeof(backdrop));
    MARGINS m = g_setGlass ? MARGINS{-1, -1, -1, -1} : MARGINS{0, 0, 0, 0};
    DwmExtendFrameIntoClientArea(g_set, &m);
    // solid: paint the title bar the same colour as the window body
    COLORREF cap = g_setGlass ? 0xFFFFFFFF /*default*/ : (g_setTheme.dark ? RGB(32, 32, 32) : RGB(243, 243, 243));
    DwmSetWindowAttribute(g_set, 35 /*DWMWA_CAPTION_COLOR*/, &cap, sizeof(cap));
    COLORREF border = g_setGlass ? 0xFFFFFFFF : cap;
    DwmSetWindowAttribute(g_set, 34 /*DWMWA_BORDER_COLOR*/, &border, sizeof(border));
    InvalidateRect(g_set, nullptr, TRUE);
}
static void BuildSettingsTheme() {
    Theme keep = T;
    T.glass = false;
    LoadTheme();
    g_setTheme = T;
    T = keep;
}

static bool HandleCaptureKey(UINT vk) {   // shared by the flyout and the settings window
    if (g_capture < 0) return false;
    if (vk == VK_CONTROL || vk == VK_MENU || vk == VK_SHIFT || vk == VK_LWIN || vk == VK_RWIN) return true;
    UINT mods = (GetKeyState(VK_CONTROL) < 0 ? MOD_CONTROL : 0) | (GetKeyState(VK_MENU) < 0 ? MOD_ALT : 0) |
                (GetKeyState(VK_SHIFT) < 0 ? MOD_SHIFT : 0) | ((GetKeyState(VK_LWIN) < 0 || GetKeyState(VK_RWIN) < 0) ? MOD_WIN : 0);
    if (vk == VK_ESCAPE && !mods) g_capture = -1;
    else if ((vk == VK_BACK || vk == VK_DELETE) && !mods) { g_hk[g_capture].mods = 0; g_hk[g_capture].vk = 0; g_capture = -1; }
    else if (mods || (vk >= VK_F13 && vk <= VK_F24)) { g_hk[g_capture].mods = mods; g_hk[g_capture].vk = vk; g_capture = -1; }
    else return true;   // a lone key would steal normal typing: wait for a combination
    RegisterHotkeys(); SaveSettings();
    return true;
}

static void CommitRename(bool keep) {
    if (!g_edit) return;
    if (keep && g_editProfile >= 0 && g_editProfile < (int)g_prof.size()) {
        wchar_t b[128]; GetWindowTextW(g_edit, b, 128);
        std::wstring t = b;
        while (!t.empty() && iswspace(t.back())) t.pop_back();
        while (!t.empty() && iswspace(t.front())) t.erase(t.begin());
        for (auto& c : t) if (c == L'|' || c == L';' || c == L'=') c = L' ';
        if (!t.empty()) g_prof[g_editProfile].name = t;
        SaveSettings();
    }
    HWND e = g_edit; g_edit = nullptr; g_editProfile = -1;
    DestroyWindow(e);
    if (g_set) InvalidateRect(g_set, nullptr, FALSE);
    if (g_visible) InvalidateRect(g_hwnd, nullptr, FALSE);
}
static LRESULT CALLBACK EditSubclass(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_KEYDOWN && wp == VK_RETURN) { CommitRename(true); return 0; }
    if (msg == WM_KEYDOWN && wp == VK_ESCAPE) { CommitRename(false); return 0; }
    if (msg == WM_CHAR && (wp == VK_RETURN || wp == VK_ESCAPE)) return 0;
    if (msg == WM_KILLFOCUS) { LRESULT r = CallWindowProcW(g_editProc, h, msg, wp, lp); PostMessageW(g_set, WM_COMMAND, MAKEWPARAM(1, EN_KILLFOCUS), (LPARAM)h); return r; }
    return CallWindowProcW(g_editProc, h, msg, wp, lp);
}
static void StartRename(int p) {
    CommitRename(true);
    if (!g_set || p < 0 || p >= (int)g_prof.size()) return;
    g_editProfile = p;
    if (g_editFont) DeleteObject(g_editFont);
    g_editFont = CreateFontW(-(int)S(14), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    g_edit = CreateWindowExW(0, L"EDIT", g_prof[p].name.c_str(), WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 10, 10, g_set, (HMENU)1, g_inst, nullptr);
    SendMessageW(g_edit, WM_SETFONT, (WPARAM)g_editFont, TRUE);
    SendMessageW(g_edit, EM_LIMITTEXT, 40, 0);
    g_editProc = (WNDPROC)SetWindowLongPtrW(g_edit, GWLP_WNDPROC, (LONG_PTR)EditSubclass);
    SendMessageW(g_edit, EM_SETSEL, 0, -1);
    g_editRect = RectF();
    InvalidateRect(g_set, nullptr, FALSE);
    SetFocus(g_edit);
}

static void SetPageScroll(float v) {
    float view = g_setH - S(80);
    g_setScroll = std::max(0.f, std::min(v, std::max(0.f, g_setContentH - view)));
}

static void PaintSettings(HDC hdc) {
    if (g_setW <= 0 || g_setH <= 0) return;
    std::swap(T, g_setTheme);
    BITMAPINFO bi = {}; bi.bmiHeader.biSize = sizeof(bi.bmiHeader); bi.bmiHeader.biWidth = g_setW; bi.bmiHeader.biHeight = -g_setH;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(hdc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HDC mdc = CreateCompatibleDC(hdc);
    HGDIOBJ old = SelectObject(mdc, dib);
    RectF editRect;
    g_sItems.clear();
    {
        Bitmap canvas(g_setW, g_setH, g_setW * 4, PixelFormat32bppPARGB, (BYTE*)bits);
        Graphics g(&canvas);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetTextRenderingHint(g_setGlass ? TextRenderingHintAntiAliasGridFit : TextRenderingHintClearTypeGridFit);   // ClearType needs an opaque background
        g.SetPixelOffsetMode(PixelOffsetModeHalf);
        if (g_setGlass) g.Clear(T.dark ? Color(110, 24, 24, 24) : Color(120, 246, 246, 246));   // acrylic shows through
        else g.Clear(T.dark ? Color(255, 32, 32, 32) : Color(255, 243, 243, 243));
        Font fTitle(g_famSemi, S(26), FontStyleRegular, UnitPixel), fBody(g_famText, S(14), FontStyleRegular, UnitPixel);
        Font fBody600(g_famSemi, S(14), FontStyleRegular, UnitPixel), fDesc(g_famText, S(12), FontStyleRegular, UnitPixel);
        Color card = g_setGlass ? (T.dark ? Color(150, 48, 48, 48) : Color(170, 255, 255, 255)) : (T.dark ? Color(255, 43, 43, 43) : Color(255, 251, 251, 251));
        Color cardHover = g_setGlass ? (T.dark ? Color(170, 58, 58, 58) : Color(200, 255, 255, 255)) : (T.dark ? Color(255, 50, 50, 50) : Color(255, 246, 246, 246));
        auto add = [&](int k, int id, RectF r) { g_sItems.push_back({k, id, r}); return (int)g_sItems.size() - 1; };
        auto isHover = [&](int k, int id) { return g_sHover >= 0 && g_sHover < (int)g_sItems.size() && g_sItems[g_sHover].kind == k && g_sItems[g_sHover].id == id; };

        // ---- navigation
        float nx = S(12);
        Glyph(g, G_EQ, g_fIcon, T.t1, nx + S(22), S(30));
        Text(g, L"Knob", &fBody600, T.t1, RectF(nx + S(44), S(16), S(160), S(28)));
        for (int i = 0; i < SP_COUNT; i++) {
            RectF r(nx, S(64) + i * S(40), S(NAVW) - S(24), S(36));
            add(SK_NAV, i, r);
            bool sel = g_setPage == i;
            if (sel) { FillRound(g, T.subtle2, r, S(6)); FillRound(g, T.accent, RectF(r.X, r.Y + S(10), S(3), S(16)), S(1.5f)); }
            else if (isHover(SK_NAV, i)) FillRound(g, T.subtle, r, S(6));
            Glyph(g, SP_GLYPHS[i], g_fIconS, T.t1, r.X + S(24), r.Y + r.Height / 2);
            Text(g, SP_NAMES[i], sel ? &fBody600 : &fBody, T.t1, RectF(r.X + S(46), r.Y, r.Width - S(50), r.Height));
        }

        // ---- content
        float x0 = S(NAVW) + S(12), cw = g_setW - x0 - S(28);
        g.SetClip(RectF(S(NAVW), 0, g_setW - S(NAVW), (float)g_setH));
        float y = S(24) - g_setScroll;
        Text(g, SP_NAMES[g_setPage], &fTitle, T.t1, RectF(x0 + S(4), y, cw, S(40)));
        y += S(60);
        auto cardRect = [&](float h) { RectF r(x0, y, cw, h); y += h + S(4); return r; };
        const float lineH = fDesc.GetHeight(&g);
        auto descLines = [&](const wchar_t* d, float right) { return (d && *d) ? std::min(2, TextLines(g, d, &fDesc, cw - right - S(24))) : 0; };
        auto cardH = [&](const wchar_t* d, float right) { int n = descLines(d, right); return n == 0 ? S(52) : n == 1 ? S(64) : S(46) + 2 * lineH + S(14); };
        auto para = [&](const wchar_t* t, float gap) {   // full-width help text, wraps as needed
            float h = TextLines(g, t, &fDesc, cw) * lineH;
            TextWrap(g, t, &fDesc, T.t3, RectF(x0 + S(4), y, cw, h + S(2)));
            y += h + gap;
        };
        auto titleDesc = [&](RectF r, const wchar_t* t, const wchar_t* d, float right) {
            if (d && *d) {
                int n = descLines(d, right);
                float top = r.Y + (r.Height - (S(22) + n * lineH + S(2))) / 2;
                Text(g, t, &fBody, T.t1, RectF(r.X + S(18), top, r.Width - right - S(24), S(22)));
                TextWrap(g, d, &fDesc, T.t3, RectF(r.X + S(18), top + S(22), r.Width - right - S(24), n * lineH + S(2)));
            } else Text(g, t, &fBody, T.t1, RectF(r.X + S(18), r.Y, r.Width - right - S(24), r.Height));
        };
        auto section = [&](const wchar_t* t) { y += S(14); Text(g, t, &fBody600, T.t1, RectF(x0 + S(4), y, cw, S(22))); y += S(30); };
        auto switchCard = [&](int id, const wchar_t* t, const wchar_t* d, bool on) {
            RectF r = cardRect(cardH(d, S(90)));
            add(SK_SWITCH, id, r);
            FillRound(g, isHover(SK_SWITCH, id) ? cardHover : card, r, S(6));
            titleDesc(r, t, d, S(90));
            Text(g, on ? L"On" : L"Off", &fBody, T.t1, RectF(r.X + r.Width - S(110), r.Y, S(40), r.Height), StringAlignmentFar);
            DrawSwitch(g, r.X + r.Width - S(18) - S(40), r.Y + r.Height / 2, on);
        };
        auto linkCard = [&](int id, const wchar_t* t, const wchar_t* d) {
            RectF r = cardRect(cardH(d, S(50)));
            add(SK_LINK, id, r);
            FillRound(g, isHover(SK_LINK, id) ? cardHover : card, r, S(6));
            titleDesc(r, t, d, S(50));
            Glyph(g, G_OPEN, g_fIconS, T.t2, r.X + r.Width - S(30), r.Y + r.Height / 2);
        };
        auto button = [&](int kind, int id, RectF r, const wchar_t* t, bool accent = false, bool enabled = true) {
            if (enabled) add(kind, id, r);
            Color bg = accent ? T.accent : (isHover(kind, id) && enabled ? T.subtle3 : T.subtle2);
            FillRound(g, bg, r, S(5));
            Text(g, t, &fDesc, accent ? T.onAccent : (enabled ? T.t1 : T.t3), r, StringAlignmentCenter);
        };
        auto iconButton = [&](int kind, int id, RectF r, wchar_t gl) {
            add(kind, id, r);
            if (isHover(kind, id)) FillRound(g, T.subtle2, r, S(5));
            Glyph(g, gl, g_fIconS, T.t1, r.X + r.Width / 2, r.Y + r.Height / 2);
        };

        switch (g_setPage) {
        case SP_GENERAL:
            switchCard(1, L"Keep awake", L"Stop the screen from turning off and the PC from sleeping", g_keepAwake);
            switchCard(2, L"Start with Windows", L"Open Knob in the tray when you sign in", StartupEnabled());
            section(L"Shortcut");
            for (int i = 0; i < HK_COUNT; i++) {
                RectF r = cardRect(S(52)); FillRound(g, card, r, S(6));
                titleDesc(r, HK_NAMES[i], L"", S(240));
                RectF c(r.X + r.Width - S(18) - S(210), r.Y + S(10), S(210), S(32));
                add(SK_HK, i, c);
                bool cap = g_capture == i;
                FillRound(g, cap ? WithAlpha(T.accent, 60) : (isHover(SK_HK, i) ? T.subtle3 : T.subtle2), c, S(6));
                std::wstring t = cap ? L"Press keys…" : ComboName(g_hk[i]);
                Color tc = cap ? T.t1 : (!g_hk[i].vk ? T.t3 : (!g_hk[i].ok ? Color(255, 232, 72, 72) : T.t1));
                if (!cap && g_hk[i].vk && !g_hk[i].ok) t += L"  (in use)";
                Text(g, t.c_str(), &fDesc, tc, c, StringAlignmentCenter);
            }
            {
                RectF r;
                r = cardRect(S(52)); FillRound(g, card, r, S(6));
                titleDesc(r, L"Remove the shortcut", L"", S(160));
                button(SK_RESETHK, 0, RectF(r.X + r.Width - S(18) - S(140), r.Y + S(10), S(140), S(32)), L"Clear");
            }
            y += S(6);
            para(L"Opens or closes the Knob panel from anywhere. Click it, then press the new keys (Ctrl, Alt, Shift or Win plus a key). Esc cancels, Backspace turns it off.", S(4));
            section(L"Windows settings");
            linkCard(1, L"Sound settings", L"");
            linkCard(2, L"Display settings", L"");
            section(L"Knob");
            { RectF r = cardRect(S(52)); FillRound(g, card, r, S(6)); titleDesc(r, L"Exit Knob", L"", S(140));
              button(SK_BUTTON, 1, RectF(r.X + r.Width - S(18) - S(110), r.Y + S(10), S(110), S(32)), L"Exit"); }
            break;
        case SP_APPEAR: {
            RectF r = cardRect(cardH(L"Glass needs Windows 11 22H2 or newer; older versions use Solid", S(200))); FillRound(g, card, r, S(6));
            titleDesc(r, L"Style", T.dark || true ? L"Glass needs Windows 11 22H2 or newer; older versions use Solid" : L"", S(200));
            RectF box(r.X + r.Width - S(18) - S(178), r.Y + r.Height / 2 - S(17), S(178), S(34));
            FillRound(g, T.subtle, box, S(8));
            for (int i = 0; i < 2; i++) {
                RectF b(box.X + S(3) + i * S(87), box.Y + S(3), S(85), S(28));
                add(SK_STYLE, i, b);
                bool on = g_style == i;
                if (on) FillRound(g, T.dark ? T.subtle3 : Color(255, 255, 255, 255), b, S(6));
                else if (isHover(SK_STYLE, i)) FillRound(g, T.subtle, b, S(6));
                Text(g, i == 0 ? L"Solid" : L"Glass", on ? g_fSmall600 : g_fSmall, T.t1, b, StringAlignmentCenter);
            }
            {   // accent: Windows, or your own colour
                const wchar_t* ad = g_accentId ? L"Pick any colour below" : L"Follows your Windows accent colour";
                r = cardRect(cardH(ad, S(200))); FillRound(g, card, r, S(6));
                titleDesc(r, L"Accent color", ad, S(200));
                RectF box(r.X + r.Width - S(18) - S(178), r.Y + r.Height / 2 - S(17), S(178), S(34));
                FillRound(g, T.subtle, box, S(8));
                for (int i = 0; i < 2; i++) {
                    RectF b(box.X + S(3) + i * S(87), box.Y + S(3), S(85), S(28));
                    add(SK_ACCMODE, i, b);
                    bool on = g_accentId == i;
                    if (on) FillRound(g, T.dark ? T.subtle3 : Color(255, 255, 255, 255), b, S(6));
                    else if (isHover(SK_ACCMODE, i)) FillRound(g, T.subtle, b, S(6));
                    Text(g, i == 0 ? L"Windows" : L"Custom", on ? g_fSmall600 : g_fSmall, T.t1, b, StringAlignmentCenter);
                }
            }
            if (g_accentId) {
                r = cardRect(S(132)); FillRound(g, card, r, S(6));
                Color cur = CustomAccent(T.dark, g_accentHue, g_accentSat);
                // preview chip + hex
                RectF chip(r.X + r.Width - S(18) - S(96), r.Y + S(14), S(96), S(26));
                FillRound(g, cur, chip, S(13));
                wchar_t hx[16]; swprintf(hx, 16, L"#%02X%02X%02X", cur.GetR(), cur.GetG(), cur.GetB());
                Text(g, hx, g_fSmall600, Color(255, 255, 255, 255), chip, StringAlignmentCenter);
                Text(g, L"Your colour", &fBody, T.t1, RectF(r.X + S(18), r.Y + S(14), S(200), S(26)));
                auto track = [&](int id, const wchar_t* label, float yy, float v) {
                    Text(g, label, &fDesc, T.t2, RectF(r.X + S(18), yy - S(12), S(80), S(24)));
                    RectF t(r.X + S(100), yy - S(5), r.Width - S(100) - S(28), S(10));
                    add(SK_COLOR, id, RectF(t.X - S(10), yy - S(14), t.Width + S(20), S(28)));
                    GraphicsPath gp; AddRound(gp, t, S(5));
                    LinearGradientBrush lb(PointF(t.X, 0), PointF(t.X + t.Width, 0), Color(255, 0, 0, 0), Color(255, 0, 0, 0));
                    if (id == 0) {   // hue rainbow
                        Color cs[7]; REAL pos[7];
                        for (int q = 0; q < 7; q++) { cs[q] = Hsl(q * 60.f, g_accentSat / 100.f, T.dark ? 0.64f : 0.42f); pos[q] = q / 6.f; }
                        lb.SetInterpolationColors(cs, pos, 7);
                    } else {         // intensity: grey -> full colour
                        Color cs[2] = {CustomAccent(T.dark, g_accentHue, 0), CustomAccent(T.dark, g_accentHue, 100)}; REAL pos[2] = {0, 1};
                        lb.SetInterpolationColors(cs, pos, 2);
                    }
                    g.FillPath(&lb, &gp);
                    float fx = t.X + t.Width * v, R = S(9);
                    bool act = g_sDragColor == id + 1 || isHover(SK_COLOR, id);
                    SolidBrush sh(Color(70, 0, 0, 0)); g.FillEllipse(&sh, fx - R, yy - R + S(1), 2 * R, 2 * R);
                    SolidBrush wb(Color(255, 255, 255, 255)); g.FillEllipse(&wb, fx - R, yy - R, 2 * R, 2 * R);
                    float ir = act ? S(6) : S(5.5f);
                    SolidBrush cb(cur); g.FillEllipse(&cb, fx - ir, yy - ir, 2 * ir, 2 * ir);
                };
                track(0, L"Hue", r.Y + S(70), g_accentHue / 359.f);
                track(1, L"Intensity", r.Y + S(106), g_accentSat / 100.f);
            }
            break;
        }
        case SP_SOUND:
            y -= S(8);
            para(L"When several outputs are selected, the main (Windows default) output plays normally and Knob copies its sound to the others. Bluetooth adds its own delay, so delay the faster outputs until they line up.", S(12));
            {
                bool can = g_router && !g_calibrating;
                std::wstring d = !g_calStatus.empty() ? g_calStatus
                    : (g_router ? L"Plays a short chirp on each output and listens with your microphone. Pause music and keep the room quiet."
                                : L"Turn on Play together in the Sound tab and pick two or more outputs to use it");
                RectF r = cardRect(cardH(d.c_str(), S(170))); FillRound(g, card, r, S(6));
                titleDesc(r, L"Auto sync", d.c_str(), S(170));
                button(SK_BUTTON, 5, RectF(r.X + r.Width - S(18) - S(130), r.Y + r.Height / 2 - S(16), S(130), S(32)),
                       g_calibrating ? L"Listening\u2026" : L"Sync now", can, can);
                y += S(8);
            }
            for (int i = 0; i < (int)g_out.size(); i++) {
                Endpoint& o = g_out[i];
                bool main = o.isDefault, on = OutSelected(i);
                RectF r = cardRect(main ? S(64) : S(96)); FillRound(g, card, r, S(6));
                std::wstring sub = main ? L"Main output · the others follow it" : (on ? L"Playing a copy" : L"Not selected — select it in the Sound tab to play here");
                Text(g, (o.name + (o.detail.empty() ? L"" : L"  ·  " + o.detail)).c_str(), &fBody, T.t1, RectF(r.X + S(18), r.Y + S(12), r.Width - S(36), S(22)));
                Text(g, sub.c_str(), &fDesc, T.t3, RectF(r.X + S(18), r.Y + S(36), r.Width - S(36), S(20)));
                if (!main) {
                    auto it = g_delay.find(o.id); int ms = it == g_delay.end() ? 0 : it->second;
                    Text(g, L"Sync delay", &fDesc, T.t2, RectF(r.X + S(18), r.Y + S(60), S(90), S(28)));
                    RectF sl(r.X + S(110), r.Y + S(60), r.Width - S(110) - S(90), S(28));
                    add(SK_DELAY, i, RectF(sl.X - S(10), sl.Y, sl.Width + S(20), sl.Height));
                    DrawSlider(g, sl, ms / 500.f, false, g_sDragDelay == i ? 2 : (isHover(SK_DELAY, i) ? 1 : 0));
                    wchar_t t[16]; swprintf(t, 16, L"%d ms", ms);
                    Text(g, t, g_fSmall600, T.t1, RectF(r.X + r.Width - S(78), r.Y + S(60), S(60), S(28)), StringAlignmentFar);
                }
            }
            break;
        case SP_DISPLAY:
            switchCard(3, L"Restore display brightness", L"Put a monitor back to its last brightness and contrast when it reconnects or the PC wakes", g_restoreBright);
            section(L"Profiles");
            y -= S(4);
            para(L"Save the brightness and contrast of every display, then switch with one click from the Display tab.", S(10));
            for (int i = 0; i < (int)g_prof.size(); i++) {
                RectF r = cardRect(S(64)); FillRound(g, card, r, S(6));
                bool active = ProfileActive(i);
                std::wstring sub = g_prof[i].v.empty() ? L"Empty — Save current to fill it" : std::to_wstring(g_prof[i].v.size()) + (g_prof[i].v.size() == 1 ? L" display saved" : L" displays saved");
                if (active) sub += L" · active";
                float right = S(18) + S(32) + S(4) + S(32) + S(8) + S(110) + S(8) + S(72);
                if (g_editProfile == i) editRect = RectF(r.X + S(14), r.Y + S(8), r.Width - right - S(30), S(28));
                else Text(g, g_prof[i].name.c_str(), &fBody, T.t1, RectF(r.X + S(18), r.Y + S(12), r.Width - right - S(24), S(22)));
                Text(g, sub.c_str(), &fDesc, active ? T.accent : T.t3, RectF(r.X + S(18), r.Y + S(36), r.Width - right - S(24), S(20)));
                float bx = r.X + r.Width - S(18);
                bx -= S(32); iconButton(SK_PDELETE, i, RectF(bx, r.Y + S(16), S(32), S(32)), 0xE74D);
                bx -= S(36); iconButton(SK_PRENAME, i, RectF(bx, r.Y + S(16), S(32), S(32)), 0xE70F);
                bx -= S(118); button(SK_PUPDATE, i, RectF(bx, r.Y + S(16), S(110), S(32)), L"Save current");
                bx -= S(80); button(SK_PAPPLY, i, RectF(bx, r.Y + S(16), S(72), S(32)), L"Apply", false, !g_prof[i].v.empty());
            }
            {
                RectF r = cardRect(S(52)); FillRound(g, card, r, S(6));
                bool can = (int)g_prof.size() < MAX_PROFILES;
                titleDesc(r, can ? L"New profile from the current settings" : L"You can keep up to 8 profiles", L"", S(170));
                button(SK_PNEW, 0, RectF(r.X + r.Width - S(18) - S(150), r.Y + S(10), S(150), S(32)), L"+ New profile", true, can);
            }
            break;
        case SP_ABOUT: {
            RectF r = cardRect(S(96)); FillRound(g, card, r, S(6));
            Text(g, APP_NAME L" " APP_VERSION, &fBody600, T.t1, RectF(r.X + S(18), r.Y + S(14), r.Width - S(36), S(24)));
            Text(g, L"A tiny tray app for volume, apps, audio outputs and display brightness.", &fDesc, T.t2, RectF(r.X + S(18), r.Y + S(42), r.Width - S(36), S(20)));
            Text(g, L"No install, no background services — only built-in Windows parts.", &fDesc, T.t3, RectF(r.X + S(18), r.Y + S(62), r.Width - S(36), S(20)));
            { RectF r = cardRect(S(64)); FillRound(g, card, r, S(6));
              std::wstring st = g_updStatus.empty() ? L"Checks github.com/" UPDATE_REPO L" every 6 hours" : g_updStatus;
              r.Height = cardH(st.c_str(), S(170)); y += r.Height - S(64); FillRound(g, card, r, S(6));
              titleDesc(r, L"Updates", st.c_str(), S(170));
              if (g_updState == UPD_FAILED) button(SK_BUTTON, 6, RectF(r.X + r.Width - S(18) - S(150), r.Y + r.Height / 2 - S(16), S(150), S(32)), L"Download in browser", true);
              else if (g_updState == UPD_AVAILABLE) button(SK_BUTTON, 4, RectF(r.X + r.Width - S(18) - S(150), r.Y + r.Height / 2 - S(16), S(150), S(32)), (L"Update to " + g_updVersion).c_str(), true);
              else button(SK_BUTTON, 3, RectF(r.X + r.Width - S(18) - S(150), r.Y + r.Height / 2 - S(16), S(150), S(32)), g_updState == UPD_DOWNLOADING ? L"Updating\u2026" : L"Check now", false, g_updState != UPD_DOWNLOADING); }
            linkCard(3, L"Open the settings folder", L"settings.ini lives in %APPDATA%\\Knob");
            { const wchar_t* rd = L"Back to defaults: style, accent, shortcut, profiles, delays and remembered brightness";
              RectF r = cardRect(cardH(rd, S(160))); FillRound(g, card, r, S(6)); titleDesc(r, L"Reset all settings", rd, S(160));
              button(SK_BUTTON, 2, RectF(r.X + r.Width - S(18) - S(130), r.Y + r.Height / 2 - S(16), S(130), S(32)), L"Reset settings"); }
            break;
        }
        }
        g_setContentH = y + g_setScroll + S(24);
        g.ResetClip();
        float view = (float)g_setH;
        if (g_setContentH > view + 1) {
            float th = std::max(S(40), view * view / g_setContentH);
            float ty = S(4) + (view - th - S(8)) * (g_setScroll / (g_setContentH - view));
            FillRound(g, T.t3, RectF(g_setW - S(7), ty, S(3), th), S(1.5f));
        }
    }
    BitBlt(hdc, 0, 0, g_setW, g_setH, mdc, 0, 0, SRCCOPY);
    SelectObject(mdc, old);
    DeleteObject(dib);
    DeleteDC(mdc);
    std::swap(T, g_setTheme);
    if (g_edit) {   // keep the rename box on top of the profile's name
        if (editRect.Width > 0 && (editRect.X != g_editRect.X || editRect.Y != g_editRect.Y || editRect.Width != g_editRect.Width)) {
            g_editRect = editRect;
            SetWindowPos(g_edit, HWND_TOP, (int)editRect.X, (int)editRect.Y, (int)editRect.Width, (int)editRect.Height, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        } else if (editRect.Width <= 0) ShowWindow(g_edit, SW_HIDE);
    }
}

static int SettingsHit(float x, float y) {
    for (int i = (int)g_sItems.size() - 1; i >= 0; i--) if (InRect(g_sItems[i].r, x, y)) return i;
    return -1;
}

static void SettingsChanged() {   // anything that the flyout also shows
    if (g_set) InvalidateRect(g_set, nullptr, FALSE);
    if (g_visible) InvalidateRect(g_hwnd, nullptr, FALSE);
}

static void SettingsColorAt(float x) {   // drag on the hue / intensity track
    for (auto& it : g_sItems) if (it.kind == SK_COLOR && it.id == g_sDragColor - 1) {
        RectF t(it.r.X + S(10), it.r.Y, it.r.Width - S(20), it.r.Height);
        float v = std::max(0.f, std::min(1.f, (x - t.X) / t.Width));
        if (it.id == 0) g_accentHue = (int)lroundf(v * 359); else g_accentSat = (int)lroundf(v * 100);
        LoadTheme(); BuildSettingsTheme();
        SettingsChanged();
        return;
    }
}
static void SettingsDelayAt(float x) {
    for (auto& it : g_sItems) if (it.kind == SK_DELAY && it.id == g_sDragDelay) {
        RectF sl(it.r.X + S(10), it.r.Y, it.r.Width - S(20), it.r.Height);
        g_delay[g_out[g_sDragDelay].id] = (int)lround(std::max(0.f, std::min(1.f, (x - sl.X) / sl.Width)) * 50) * 10;
        InvalidateRect(g_set, nullptr, FALSE);
        return;
    }
}

static LRESULT CALLBACK SettingsProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_SIZE:
        if (wp == SIZE_MINIMIZED) { PairHide(); return 0; }
        g_setW = LOWORD(lp); g_setH = HIWORD(lp); SetPageScroll(g_setScroll); InvalidateRect(h, nullptr, FALSE); return 0;
    case WM_ACTIVATE:   // the panel follows Settings: back when Settings comes forward, away when another app takes over
        if (LOWORD(wp) != WA_INACTIVE) PairShow(false);
        else if ((HWND)lp != g_hwnd) PairHide();
        break;
    case WM_PAINT: { PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps); PaintSettings(dc); EndPaint(h, &ps); return 0; }
    case WM_ERASEBKGND: return 1;
    case WM_MOUSEMOVE: {
        float x = (float)GET_X_LPARAM(lp), y = (float)GET_Y_LPARAM(lp);
        if (g_sDragDelay >= 0) { SettingsDelayAt(x); return 0; }
        if (g_sDragColor) { SettingsColorAt(x); return 0; }
        int hv = SettingsHit(x, y);
        if (hv != g_sHover) { g_sHover = hv; InvalidateRect(h, nullptr, FALSE); }
        TRACKMOUSEEVENT tme = {sizeof(tme), TME_LEAVE, h, 0}; TrackMouseEvent(&tme);
        return 0;
    }
    case WM_MOUSELEAVE: g_sHover = -1; InvalidateRect(h, nullptr, FALSE); return 0;
    case WM_LBUTTONDOWN: {
        float x = (float)GET_X_LPARAM(lp), y = (float)GET_Y_LPARAM(lp);
        int hi = SettingsHit(x, y);
        if (g_capture >= 0 && (hi < 0 || g_sItems[hi].kind != SK_HK)) { g_capture = -1; RegisterHotkeys(); }
        if (g_edit && (hi < 0 || g_sItems[hi].kind != SK_PRENAME)) CommitRename(true);
        if (hi < 0) { InvalidateRect(h, nullptr, FALSE); return 0; }
        SItem it = g_sItems[hi];
        switch (it.kind) {
        case SK_NAV: g_setPage = it.id; g_setScroll = 0; break;
        case SK_SWITCH:
            if (it.id == 1) { g_keepAwake = !g_keepAwake; ApplyKeepAwake(); }
            else if (it.id == 2) SetStartup(!StartupEnabled());
            else if (it.id == 3) g_restoreBright = !g_restoreBright;
            else if (it.id == 4) g_osd = !g_osd;
            SaveSettings();
            break;
        case SK_LINK:
            if (it.id == 1) ShellExecuteW(nullptr, L"open", L"ms-settings:sound", nullptr, nullptr, SW_SHOWNORMAL);
            else if (it.id == 2) ShellExecuteW(nullptr, L"open", L"ms-settings:display", nullptr, nullptr, SW_SHOWNORMAL);
            else { std::wstring dir = g_iniPath.substr(0, g_iniPath.rfind(L'\\')); ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL); }
            break;
        case SK_BUTTON:
            if (it.id == 1) { DestroyWindow(g_hwnd); return 0; }
            if (it.id == 3) { g_updStatus = L"Checking\u2026"; CheckForUpdates(); }
            if (it.id == 5) StartAutoSync();
            if (it.id == 6) ShellExecuteW(nullptr, L"open", L"https://github.com/" UPDATE_REPO L"/raw/main/dist/Knob.exe", nullptr, nullptr, SW_SHOWNORMAL);
            if (it.id == 4) StartUpdate();
            if (it.id == 2 && MessageBoxW(h, L"Reset all Knob settings to their defaults? Knob will restart.", APP_NAME, MB_YESNO | MB_ICONQUESTION) == IDYES) {
                g_noSave = true; DeleteFileW(g_iniPath.c_str()); g_restartAfterExit = true;
                DestroyWindow(g_hwnd); return 0;
            }
            break;
        case SK_STYLE: g_style = it.id; SaveSettings(); ApplyStyle(); BuildSettingsTheme(); ApplySettingsStyle(); break;
        case SK_SWATCH: break;
        case SK_ACCMODE: g_accentId = it.id; SaveSettings(); LoadTheme(); BuildSettingsTheme(); UpdateTrayIcon(); SettingsChanged(); break;
        case SK_COLOR: g_sDragColor = it.id + 1; SetCapture(h); SettingsColorAt(x); return 0;
        case SK_HK: g_capture = it.id; UnregisterHotkeys(); break;
        case SK_STEP: g_step = it.id; SaveSettings(); break;
        case SK_RESETHK: for (int i = 0; i < HK_COUNT; i++) g_hk[i] = HK_DEFAULT[i]; RegisterHotkeys(); SaveSettings(); break;
        case SK_PAPPLY: ApplyProfile(it.id); break;
        case SK_PUPDATE: SaveProfile(it.id); break;
        case SK_PRENAME: StartRename(it.id); return 0;
        case SK_PDELETE: if (it.id < (int)g_prof.size()) { g_prof.erase(g_prof.begin() + it.id); SaveSettings(); } break;
        case SK_PNEW:
            if ((int)g_prof.size() < MAX_PROFILES) {
                g_prof.push_back({L"Profile " + std::to_wstring(g_prof.size() + 1), {}});
                SaveProfile((int)g_prof.size() - 1);
                StartRename((int)g_prof.size() - 1);
                return 0;
            }
            break;
        case SK_DELAY: g_sDragDelay = it.id; SetCapture(h); SettingsDelayAt(x); return 0;
        }
        SettingsChanged();
        return 0;
    }
    case WM_LBUTTONUP:
        if (g_sDragDelay >= 0) { g_sDragDelay = -1; ReleaseCapture(); SaveSettings(); RestartRouter(); SettingsChanged(); }
        if (g_sDragColor) { g_sDragColor = 0; ReleaseCapture(); SaveSettings(); UpdateTrayIcon(); SettingsChanged(); }
        return 0;
    case WM_MOUSEWHEEL:
        SetPageScroll(g_setScroll - GET_WHEEL_DELTA_WPARAM(wp) / 120.f * S(60));
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_SYSKEYDOWN:
    case WM_KEYDOWN:
        if (HandleCaptureKey((UINT)wp)) { InvalidateRect(h, nullptr, FALSE); return 0; }
        if (msg == WM_KEYDOWN && wp == VK_ESCAPE) { DestroyWindow(h); return 0; }
        break;
    case WM_COMMAND:
        if (HIWORD(wp) == EN_KILLFOCUS && g_edit && (HWND)lp == g_edit) CommitRename(true);
        return 0;
    case WM_APP_STARTEDIT: StartRename((int)wp); return 0;
    case WM_CTLCOLOREDIT: {
        HDC dc = (HDC)wp;
        COLORREF bg = g_setTheme.dark ? RGB(43, 43, 43) : RGB(251, 251, 251);
        SetTextColor(dc, g_setTheme.dark ? RGB(255, 255, 255) : RGB(26, 26, 26));
        SetBkColor(dc, bg);
        if (g_editBrush) DeleteObject(g_editBrush);
        g_editBrush = CreateSolidBrush(bg);
        return (LRESULT)g_editBrush;
    }
    case WM_DPICHANGED: {
        ApplyDpi(HIWORD(wp));
        RECT* r = (RECT*)lp;
        SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_CLOSE: DestroyWindow(h); return 0;
    case WM_DESTROY:
        if (g_capture >= 0) { g_capture = -1; RegisterHotkeys(); }
        if (g_edit) CommitRename(true);
        g_set = nullptr;
        if (g_pairHidden) { g_pairHidden = false; HideFlyout(); }   // it was tucked away: close it properly
        else if (g_visible) SetForegroundWindow(g_hwnd);   // hand focus back so the panel closes normally again
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static void OpenSettings(int page, bool newProfile) {
    int editIdx = -1;
    if (newProfile && (int)g_prof.size() < MAX_PROFILES) {
        g_prof.push_back({L"Profile " + std::to_wstring(g_prof.size() + 1), {}});
        SaveProfile((int)g_prof.size() - 1);
        editIdx = (int)g_prof.size() - 1;
        page = SP_DISPLAY;
    }
    BuildSettingsTheme();
    if (!g_set) {
        POINT pt; GetCursorPos(&pt);
        HMONITOR hm = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
        UINT dx = 96, dy = 96; GetDpiForMonitor(hm, MDT_EFFECTIVE_DPI, &dx, &dy);
        ApplyDpi((int)dx);
        MONITORINFO mi = {sizeof(mi)}; GetMonitorInfoW(hm, &mi);
        RECT r = {0, 0, (LONG)S(820), (LONG)S(600)};
        DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;   // fixed size
        AdjustWindowRectExForDpi(&r, style, FALSE, 0, dx);
        int w = r.right - r.left, hgt = r.bottom - r.top;
        int x = mi.rcWork.left + ((mi.rcWork.right - mi.rcWork.left) - w) / 2, y = mi.rcWork.top + ((mi.rcWork.bottom - mi.rcWork.top) - hgt) / 2;
        if (g_visible) {   // sit to the left of the open panel instead of under it
            RECT fr; GetWindowRect(g_hwnd, &fr);
            if (x + w > fr.left - (int)S(12)) x = std::max((int)mi.rcWork.left, (int)fr.left - (int)S(12) - w);
            y = std::max((int)mi.rcWork.top, (int)fr.bottom - hgt);
        }
        g_set = CreateWindowExW(0, L"KnobSettings", L"Knob Settings", style, x, y, w, hgt, nullptr, nullptr, g_inst, nullptr);
        if (!g_set) return;
        ApplySettingsStyle();
    }
    g_setPage = page; g_setScroll = 0;
    ShowWindow(g_set, IsIconic(g_set) ? SW_RESTORE : SW_SHOW);
    SetForegroundWindow(g_set);
    InvalidateRect(g_set, nullptr, FALSE);
    if (editIdx >= 0) PostMessageW(g_set, WM_APP_STARTEDIT, editIdx, 0);
}

// ---------------------------------------------------------------- window proc
static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_TASKBARCREATED && WM_TASKBARCREATED) { TrayAdd(); UpdateTrayTip(); return 0; }
    switch (msg) {
    case WM_APP_TRAY: {
        UINT ev = LOWORD(lp);
        if (ev == NIN_SELECT || ev == NIN_KEYSELECT) {
            if (g_set && (g_visible || g_pairHidden)) PairShow(true);   // Settings is open: bring the pair forward
            else if (g_visible) HideFlyout();
            else if (GetTickCount() - g_hideTick > 300) ShowFlyout();
        } else if (ev == WM_CONTEXTMENU) { HideFlyout(); ShowTrayMenu(); }
        return 0;
    }
    case WM_APP_SHOW: ShowFlyout(); return 0;
    case WM_APP_VERSION: return APP_VERNUM;
    case WM_APP_UPDINFO: {
        if (wp == 1) {
            g_updState = UPD_FAILED;
            std::wstring* e = (std::wstring*)lp;
            g_updStatus = L"Update failed: " + (e ? *e : std::wstring(L"unknown reason"));
            delete e;
            lp = 0;
        }
        else if (wp == 2) { g_updState = UPD_NONE; g_updStatus = L"No build on GitHub yet \u00B7 you're up to date"; }
        else if (UpdInfo* u = (UpdInfo*)lp) {
            if (ParseVer(u->version) > APP_VERNUM) {
                g_updVersion = u->version[0] == L'v' ? u->version.substr(1) : u->version; g_updUrl = u->url;
                if (g_updState != UPD_DOWNLOADING) g_updState = UPD_AVAILABLE;
                g_updStatus = L"Version " + g_updVersion + L" is available";
            } else { g_updState = UPD_NONE; g_updStatus = L"You have the latest version"; }
            delete u;
        } else g_updStatus = L"Couldn't reach GitHub";
        if (g_visible) InvalidateRect(h, nullptr, FALSE);
        if (g_set) InvalidateRect(g_set, nullptr, FALSE);
        return 0;
    }
    case WM_APP_CALDONE: {
        CalResult* r = (CalResult*)lp;
        InterlockedExchange(&g_calibrating, 0);
        if (r) {
            for (auto& d : r->delays) g_delay[d.first] = d.second;
            g_calStatus = r->status;
            if (!r->delays.empty()) SaveSettings();
            int di = DefaultOut();
            if (!r->swapTo.empty() && di >= 0) {   // slowest speaker becomes the main output; the old main becomes a copy
                std::wstring oldMain = g_out[di].id;
                g_mirror.erase(r->swapTo);
                g_mirror.insert(oldMain);
                g_delay[oldMain] = 0;
                SetDefaultDevice(r->swapTo);
                LoadEndpoints(eRender, g_out);
                SaveSettings();
                g_calPass = 1;
                SetTimer(h, TIMER_CALRETRY, 2500, nullptr);
            } else g_calPass = 0;
            delete r;
        }
        g_routerPending = false; RestartRouter();   // apply the new delays (and any device change that waited)
        if (g_calPass) InterlockedExchange(&g_calibrating, 1);   // stay busy until the second run starts
        if (g_visible) InvalidateRect(h, nullptr, FALSE);
        if (g_set) InvalidateRect(g_set, nullptr, FALSE);
        return 0;
    }
    case WM_APP_QUIT: DestroyWindow(h); return 0;
    case WM_APP_VOLCHANGED:
        InterlockedExchange(&g_volPending, 0);
        RefreshEndpointLevels(g_out); RefreshEndpointLevels(g_in);
        UpdateTrayTip();
        if (g_visible && g_dragEl < 0) InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_HOTKEY: if (wp >= 100 && wp < 100 + HK_COUNT) RunHotkey((int)wp - 100); return 0;
    case WM_APP_AUDIOCHANGED: SetTimer(h, TIMER_AUDIODEBOUNCE, 200, nullptr); return 0;
    case WM_APP_MONITORS: {
        MonMsg* m = (MonMsg*)lp;
        bool draggingMon = g_dragEl >= 0 && g_dragEl < (int)g_els.size() && (g_els[g_dragEl].k == E_MONB || g_els[g_dragEl].k == E_MONC);
        if (m->fromEnum || !draggingMon) {
            std::set<std::wstring> before;
            for (auto& x : g_mons) before.insert(x.name);
            bool hadList = g_monsLoaded;
            g_mons = m->mons; g_monGen = m->gen; g_monsLoaded = true;
            if (m->fromEnum && g_restoreBright && hadList) {   // a monitor that just (re)appeared gets its last brightness/contrast back
                for (int i = 0; i < (int)g_mons.size(); i++) {
                    if (!g_mons[i].ok || (!g_restoreAllNext && before.count(g_mons[i].name))) continue;
                    auto it = g_lastMon.find(g_mons[i].name);
                    if (it == g_lastMon.end()) continue;
                    if (it->second.first >= 0 && it->second.first != g_mons[i].pct) { g_mons[i].pct = it->second.first; MonSet(i, 0, it->second.first); }
                    if (g_mons[i].hasCon && it->second.second >= 0 && it->second.second != g_mons[i].con) { g_mons[i].con = it->second.second; MonSet(i, 1, it->second.second); }
                }
            }
            if (m->fromEnum) g_restoreAllNext = false;
            if (g_ddOpen >= (int)g_mons.size()) g_ddOpen = -1;
            if (g_visible) InvalidateRect(h, nullptr, FALSE);
        }
        delete m;
        return 0;
    }
    case WM_TIMER:
        if (wp == TIMER_AUDIODEBOUNCE) {
            KillTimer(h, TIMER_AUDIODEBOUNCE);
            if (g_dragEl >= 0) { SetTimer(h, TIMER_AUDIODEBOUNCE, 300, nullptr); return 0; }
            LoadEndpoints(eRender, g_out);
            LoadEndpoints(eCapture, g_in);
            if (g_visible && g_tab == T_APPS) LoadApps();
            RestartRouter();
            UpdateTrayTip();
            if (g_visible) InvalidateRect(h, nullptr, FALSE);
        } else if (wp == TIMER_REFRESH && g_visible && g_dragEl < 0) {
            RefreshEndpointLevels(g_out);
            RefreshEndpointLevels(g_in);
            if (g_tab == T_APPS) { LoadApps(); UpdatePeaks(); }
            static int tick = 0;
            if (g_tab == T_DISPLAY && ++tick % 4 == 0) BrightRequest(false, true);   // picks up changes made with monitor buttons / laptop keys
            InvalidateRect(h, nullptr, FALSE);
        } else if (wp == TIMER_ANIM && g_visible && g_tab == T_APPS) {
            static bool wasAny = true;
            bool any = UpdatePeaks();
            if (any || wasAny) InvalidateRect(h, nullptr, FALSE);
            wasAny = any;
        } else if (wp == TIMER_IDENT) {
            KillTimer(h, TIMER_IDENT);
            CloseIdentify();
        } else if (wp == TIMER_CALRETRY) {
            KillTimer(h, TIMER_CALRETRY);
            InterlockedExchange(&g_calibrating, 0);
            RestartRouter();
            StartAutoSync();   // second run with the new main output
            if (g_set) InvalidateRect(g_set, nullptr, FALSE);
            return 0;
        } else if (wp == TIMER_TRAYPIN) {
            static int ticks = 0;
            ticks++;
            TrayPinSync(ticks <= 3);   // first ~20 s after start: restore your choice; afterwards: follow your changes
            SetTimer(h, TIMER_TRAYPIN, ticks < 3 ? 6000 : 60000, nullptr);
            return 0;
        } else if (wp == TIMER_UPDCHECK) {
            SetTimer(h, TIMER_UPDCHECK, 6 * 60 * 60 * 1000, nullptr);   // then every 6 hours
            CheckForUpdates();
        } else if (wp == TIMER_OSD) {
            KillTimer(h, TIMER_OSD);
            if (g_osdWnd) ShowWindow(g_osdWnd, SW_HIDE);
        } else if (wp == TIMER_SPIN) {
            if (GetTickCount() >= g_spinUntil) KillTimer(h, TIMER_SPIN);
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE && g_visible) {
            if (!g_set) HideFlyout();
            else if ((HWND)lp != g_set) PairHide();   // clicked another app: panel and Settings step back together
        }
        return 0;
    case WM_SYSKEYDOWN:
    case WM_KEYDOWN:
        if (g_capture >= 0) {   // recording a new shortcut
            UINT vk = (UINT)wp;
            if (vk == VK_CONTROL || vk == VK_MENU || vk == VK_SHIFT || vk == VK_LWIN || vk == VK_RWIN) return 0;
            UINT mods = (GetKeyState(VK_CONTROL) < 0 ? MOD_CONTROL : 0) | (GetKeyState(VK_MENU) < 0 ? MOD_ALT : 0) |
                        (GetKeyState(VK_SHIFT) < 0 ? MOD_SHIFT : 0) | ((GetKeyState(VK_LWIN) < 0 || GetKeyState(VK_RWIN) < 0) ? MOD_WIN : 0);
            if (vk == VK_ESCAPE && !mods) { g_capture = -1; }
            else if ((vk == VK_BACK || vk == VK_DELETE) && !mods) { g_hk[g_capture].mods = 0; g_hk[g_capture].vk = 0; g_capture = -1; }
            else if (mods || (vk >= VK_F13 && vk <= VK_F24)) { g_hk[g_capture].mods = mods; g_hk[g_capture].vk = vk; g_capture = -1; }
            else return 0;   // a plain key alone would steal normal typing: wait for a combo
            RegisterHotkeys(); SaveSettings();
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        if (msg == WM_SYSKEYDOWN) return DefWindowProcW(h, msg, wp, lp);
        if (wp == VK_ESCAPE) {
            if (g_menuOpen || g_ddOpen >= 0 || g_saveOpen || g_delayOpen >= 0) { g_menuOpen = false; g_ddOpen = -1; g_saveOpen = false; g_delayOpen = -1; InvalidateRect(h, nullptr, FALSE); }
            else if (g_view == 1) { g_view = 0; InvalidateRect(h, nullptr, FALSE); }
            else HideFlyout();
        } else if (wp == VK_TAB && g_view == 0) SetTab((g_tab + (GetKeyState(VK_SHIFT) < 0 ? 2 : 1)) % 3);
        else if (wp == VK_F5) RefreshAll();
        return 0;
    case WM_MOUSEMOVE: {
        float x = (float)GET_X_LPARAM(lp), y = (float)GET_Y_LPARAM(lp);
        if (g_dragEl >= 0 && g_dragEl < (int)g_els.size()) {
            SetElValue(g_els[g_dragEl], SliderValueAt(g_els[g_dragEl], x));
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        if (g_dragDelay) {
            Layout();
            RectF chip;
            if (g_delayOpen >= 0 && DelayAnchor(chip)) {
                RectF sl = DelaySlider(DelayPopup(chip));
                g_delay[g_out[g_delayOpen].id] = (int)lround(std::max(0.f, std::min(1.f, (x - sl.X) / sl.Width)) * 50) * 10;
            }
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        Layout();
        Hit ht = HitTest(x, y);
        int sl = ht.h == H_SLIDER ? ht.el : -1;
        if (sl != g_hoverSlider) { g_hoverSlider = sl; g_hoverSince = GetTickCount(); }
        if (ht.h != g_hover.h || ht.el != g_hover.el || ht.idx != g_hover.idx) { g_hover = ht; InvalidateRect(h, nullptr, FALSE); }
        TRACKMOUSEEVENT tme = {sizeof(tme), TME_LEAVE, h, 0};
        TrackMouseEvent(&tme);
        return 0;
    }
    case WM_MOUSELEAVE:
        g_hover = Hit(); g_hoverSlider = -1;
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_LBUTTONDOWN: {
        float x = (float)GET_X_LPARAM(lp), y = (float)GET_Y_LPARAM(lp);
        Layout();
        Hit ht = HitTest(x, y);
        // an open popup swallows the click that closes it
        if (g_menuOpen && ht.h != H_MENU && ht.h != H_MENUBG) { g_menuOpen = false; InvalidateRect(h, nullptr, FALSE); return 0; }
        if (g_delayOpen >= 0 && ht.h != H_DELAYSL && ht.h != H_MENUBG) {
            bool same = ht.h == H_DELAYCHIP && ht.idx == g_delayOpen;
            g_delayOpen = -1; InvalidateRect(h, nullptr, FALSE);
            if (same || ht.h != H_DELAYCHIP) return 0;
        }
        if (g_capture >= 0 && ht.h != H_HKCHIP) { g_capture = -1; RegisterHotkeys(); InvalidateRect(h, nullptr, FALSE); }
        if (g_saveOpen && ht.h != H_SAVEOPT && ht.h != H_MENUBG) {
            g_saveOpen = false; InvalidateRect(h, nullptr, FALSE);
            if (ht.h == H_PROFILE && ht.idx == ChipCount() - 1) return 0;
        }
        if (g_ddOpen >= 0 && ht.h != H_DDOPT && ht.h != H_MENUBG) {
            bool sameBtn = ht.h == H_DROPDOWN && ht.idx == g_ddOpen;
            g_ddOpen = -1; InvalidateRect(h, nullptr, FALSE);
            if (sameBtn || ht.h != H_DROPDOWN) return 0;
        }
        switch (ht.h) {
        case H_MENU: MenuCommand(ht.idx); return 0;
        case H_MENUBG: return 0;
        case H_DDOPT: { int idx = g_ddOpen; g_ddOpen = -1; SetOrientation(idx, ht.idx); InvalidateRect(h, nullptr, FALSE); return 0; }
        case H_TAB: SetTab(ht.idx); return 0;
        case H_MICALL: MicToggleMute(); break;
        case H_UPDATE: StartUpdate(); break;
        case H_DELAYCHIP: g_delayOpen = ht.idx; break;
        case H_DELAYSL: {
            RectF chip; if (!DelayAnchor(chip)) break;
            RectF sl = DelaySlider(DelayPopup(chip));
            g_dragDelay = true; SetCapture(h);
            int ms = (int)lround(std::max(0.f, std::min(1.f, (x - sl.X) / sl.Width)) * 50) * 10;
            g_delay[g_out[g_delayOpen].id] = ms;
            break;
        }
        case H_BACK: g_view = 0; g_hover = Hit(); break;
        case H_HKCHIP: g_capture = ht.idx; UnregisterHotkeys(); break;   // free all combos while recording
        case H_OSD: g_osd = !g_osd; SaveSettings(); if (g_osd) ShowOsd(OSD_VOL, DefaultOut() >= 0 ? (int)lround(g_out[DefaultOut()].level * 100) : 0, false); break;
        case H_STEP: { int vals[3] = {2, 5, 10}; g_step = vals[ht.idx]; SaveSettings(); break; }
        case H_RESET: for (int i = 0; i < HK_COUNT; i++) g_hk[i] = HK_DEFAULT[i]; RegisterHotkeys(); SaveSettings(); break;
        case H_PROFILE:
            if (ht.idx == ChipCount() - 1) g_saveOpen = true;
            else if (ProfileEmpty(ht.idx)) SaveProfile(ht.idx);   // first click on an empty profile stores the current setup
            else ApplyProfile(ht.idx);
            break;
        case H_SAVEOPT:
            g_saveOpen = false;
            if (ht.idx < (int)g_prof.size()) SaveProfile(ht.idx);
            else { OpenSettings(2, true); return 0; }   // name a new profile in Settings
            break;
        case H_GEAR:   // open Settings, or bring it to the front if it's already open
            if (g_set) PairShow(true);
            else OpenSettings(0, false);
            return 0;
        case H_IDENT: IdentifyDisplays(); return 0;
        case H_REFRESH: RefreshAll(); return 0;
        case H_DROPDOWN: g_ddOpen = ht.idx; break;
        case H_SLIDER:
            g_dragEl = ht.el;
            SetCapture(h);
            SetElValue(g_els[ht.el], SliderValueAt(g_els[ht.el], x));
            break;
        case H_MUTE: ToggleMute(g_els[ht.el]); break;
        case H_MULTI: SetMulti(!g_multi); break;
        case H_ROW: {
            El& e = g_els[ht.el];
            if (e.k == E_OUT) ClickOutput(e.idx);
            else if (e.k == E_IN && !g_in[e.idx].isDefault) { SetDefaultDevice(g_in[e.idx].id); LoadEndpoints(eCapture, g_in); }
            break;
        }
        default: break;
        }
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }
    case WM_LBUTTONUP:
        if (g_dragEl >= 0) { g_dragEl = -1; ReleaseCapture(); InvalidateRect(h, nullptr, FALSE); }
        if (g_dragDelay) { g_dragDelay = false; ReleaseCapture(); SaveSettings(); RestartRouter(); InvalidateRect(h, nullptr, FALSE); }
        return 0;
    case WM_CAPTURECHANGED:
        if (g_dragEl >= 0 && (HWND)lp != h) { g_dragEl = -1; InvalidateRect(h, nullptr, FALSE); }
        return 0;
    case WM_MOUSEWHEEL: {
        if (g_menuOpen || g_ddOpen >= 0 || g_saveOpen || g_delayOpen >= 0) return 0;
        POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(h, &pt);
        int delta = GET_WHEEL_DELTA_WPARAM(wp);
        Layout();
        Hit ht = HitTest((float)pt.x, (float)pt.y);
        DWORD now = GetTickCount();
        bool sliderWheel = false;   // the wheel only scrolls; sliders change by dragging or with shortcuts
        (void)ht;
        if (sliderWheel) {
            El e = g_els[ht.el];
            SetElValue(e, ElValue(e) + (delta > 0 ? 0.02f : -0.02f) * (std::abs(delta) / 120.f));
        } else {
            g_scroll[SI()] -= delta / 120.f * S(56);
            ClampScroll();
            g_lastScroll = now;
            g_hoverSince = now;
        }
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }
    case WM_PAINT: { PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps); Paint(dc); EndPaint(h, &ps); return 0; }
    case WM_ERASEBKGND: return 1;
    case WM_DPICHANGED: ApplyDpi(HIWORD(wp)); if (g_visible) PositionFlyout(); return 0;
    case WM_DISPLAYCHANGE: BrightRequest(true, false); if (g_visible) PositionFlyout(); return 0;
    case WM_SETTINGCHANGE:
        if (lp && lstrcmpiW((LPCWSTR)lp, L"ImmersiveColorSet") == 0) {
            LoadTheme(); ApplyStyle(); UpdateTrayIcon(); AllowDarkMenus();
            if (g_set) { BuildSettingsTheme(); ApplySettingsStyle(); }
        }
        return 0;
    case WM_DWMCOLORIZATIONCOLORCHANGED: LoadTheme(); if (g_visible) InvalidateRect(h, nullptr, FALSE); return 0;
    case WM_POWERBROADCAST:
        if (wp == PBT_APMRESUMEAUTOMATIC) { g_restoreAllNext = true; BrightRequest(true, false); PostMessageW(h, WM_APP_AUDIOCHANGED, 0, 0); }
        return TRUE;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// ---------------------------------------------------------------- install / update
static std::wstring InstallPath() {   // %LOCALAPPDATA%\Knob\Knob.exe
    wchar_t p[MAX_PATH] = {};
    SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, p);
    std::wstring d = std::wstring(p) + L"\\Knob";
    CreateDirectoryW(d.c_str(), nullptr);
    return d + L"\\Knob.exe";
}
static void WaitForExit(HWND w) {
    DWORD pid = 0; GetWindowThreadProcessId(w, &pid);
    HANDLE ph = pid ? OpenProcess(SYNCHRONIZE, FALSE, pid) : nullptr;
    if (ph) { WaitForSingleObject(ph, 6000); CloseHandle(ph); }
    else for (int i = 0; i < 60 && IsWindow(w); i++) Sleep(100);
}
// Returns true when this process should exit (another copy takes over).
static bool InstallOrUpdate() {
    wchar_t self[MAX_PATH]; GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring inst = InstallPath();
    bool installed = lstrcmpiW(self, inst.c_str()) == 0;
    HWND running = FindWindowW(L"KnobFlyout", nullptr);
    if (running) {
        DWORD_PTR v = 0;
        SendMessageTimeoutW(running, WM_APP_VERSION, 0, 0, SMTO_ABORTIFHUNG, 2000, &v);
        if (v > APP_VERNUM) {   // never downgrade
            MessageBoxW(nullptr, L"A newer version of Knob is already running.", APP_NAME, MB_OK | MB_ICONINFORMATION);
            return true;
        }
        if (v == APP_VERNUM) { PostMessageW(running, WM_APP_SHOW, 0, 0); return true; }   // same version: just open it
        PostMessageW(running, WM_APP_QUIT, 0, 0);   // older Knob: let it save and close
        WaitForExit(running);
    }
    if (HWND old = FindWindowW(L"LiteMixFlyout", nullptr)) {   // the app's previous name
        PostMessageW(old, WM_CLOSE, 0, 0);
        WaitForExit(old);
    }
    if (!installed) {
        bool copied = false;
        for (int i = 0; i < 20 && !copied; i++) { copied = CopyFileW(self, inst.c_str(), FALSE) != 0; if (!copied) Sleep(150); }
        if (copied) {
            if (StartupEnabled()) SetStartup(true, inst.c_str());
            if ((INT_PTR)ShellExecuteW(nullptr, L"open", inst.c_str(), L"--show", nullptr, SW_SHOWNORMAL) > 32) return true;
        }
    }
    return false;   // run from here
}

// ---------------------------------------------------------------- entry
int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, LPWSTR cmd, int) {
    g_inst = hi;
    if (InstallOrUpdate()) return 0;
    {   // tidy up downloaded updates
        wchar_t tmp[MAX_PATH]; GetTempPathW(MAX_PATH, tmp);
        WIN32_FIND_DATAW fd; HANDLE fh = FindFirstFileW((std::wstring(tmp) + L"Knob-update*.exe").c_str(), &fd);
        if (fh != INVALID_HANDLE_VALUE) { do DeleteFileW((std::wstring(tmp) + fd.cFileName).c_str()); while (FindNextFileW(fh, &fd)); FindClose(fh); }
    }
    HANDLE mtx = CreateMutexW(nullptr, TRUE, L"Knob.SingleInstance.v1");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND other = FindWindowW(L"KnobFlyout", nullptr);
        if (other) PostMessageW(other, WM_APP_SHOW, 0, 0);
        return 0;
    }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    GdiplusStartupInput gsi;
    ULONG_PTR gtoken = 0;
    GdiplusStartup(&gtoken, &gsi, nullptr);

    LoadSettings();
    if (StartupEnabled()) SetStartup(true);   // point "Start with Windows" at this copy (and drop the old LiteMix entry)
    LoadTheme();
    AllowDarkMenus();
    ApplyDpi(GetDpiForSystem());
    if (g_keepAwake) ApplyKeepAwake();

    CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&g_enum);
    WM_TASKBARCREATED = RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSEXW wc = {sizeof(wc)};
    wc.style = CS_DROPSHADOW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hi;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"KnobFlyout";
    wc.hIcon = LoadIconW(hi, MAKEINTRESOURCEW(1));
    RegisterClassExW(&wc);
    WNDCLASSEXW ic = {sizeof(ic)};
    ic.lpfnWndProc = IdentProc; ic.hInstance = hi; ic.lpszClassName = L"KnobIdent"; ic.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassExW(&ic);
    WNDCLASSEXW sc = {sizeof(sc)};
    sc.lpfnWndProc = SettingsProc; sc.hInstance = hi; sc.lpszClassName = L"KnobSettings"; sc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    sc.hIcon = LoadIconW(hi, MAKEINTRESOURCEW(1)); sc.hIconSm = LoadIconW(hi, MAKEINTRESOURCEW(1));
    RegisterClassExW(&sc);
    WNDCLASSEXW oc = {sizeof(oc)};
    oc.lpfnWndProc = OsdProc; oc.hInstance = hi; oc.lpszClassName = L"KnobOSD"; oc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassExW(&oc);

    g_hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, wc.lpszClassName, APP_NAME, WS_POPUP, 0, 0, 380, 568, nullptr, nullptr, hi, nullptr);
    if (!g_hwnd) return 1;
    {
        DWORD pref = 2;   // DWMWCP_ROUND (Windows 11)
        g_dwmRound = SUCCEEDED(DwmSetWindowAttribute(g_hwnd, 33, &pref, sizeof(pref)));
        if (g_dwmRound) { COLORREF bc = 0xFFFFFFFE; DwmSetWindowAttribute(g_hwnd, 34, &bc, sizeof(bc)); }   // no border
    }
    ApplyStyle();

    g_volCb = new VolCallback;
    if (g_enum) { g_notif = new NotifClient; g_enum->RegisterEndpointNotificationCallback(g_notif); }
    InitializeCriticalSection(&g_bcs);
    g_bEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    HANDLE bthread = CreateThread(nullptr, 0, BrightProc, nullptr, 0, nullptr);
    BrightRequest(true, false);

    LoadEndpoints(eRender, g_out);
    LoadEndpoints(eCapture, g_in);
    RestartRouter();
    TrayAdd();
    UpdateTrayTip();
    RegisterHotkeys();
    SetTimer(g_hwnd, TIMER_UPDCHECK, 15000, nullptr);   // first check 15 s after start
    SetTimer(g_hwnd, TIMER_TRAYPIN, 3000, nullptr);
    if (cmd && wcsstr(cmd, L"--show")) PostMessageW(g_hwnd, WM_APP_SHOW, 0, 0);   // just installed / updated: show it

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) { TranslateMessage(&m); DispatchMessageW(&m); }

    SaveSettings();
    StopRouter();
    UnregisterHotkeys();
    CloseIdentify();
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
    EnterCriticalSection(&g_bcs); g_bQuit = true; LeaveCriticalSection(&g_bcs);
    SetEvent(g_bEvent);
    if (bthread) { WaitForSingleObject(bthread, 3000); CloseHandle(bthread); }
    if (g_enum && g_notif) { g_enum->UnregisterEndpointNotificationCallback(g_notif); g_notif->Release(); }
    FreeApps();
    FreeEndpoints(g_out); FreeEndpoints(g_in);
    if (g_volCb) g_volCb->Release();
    if (g_enum) g_enum->Release();
    SetThreadExecutionState(ES_CONTINUOUS);
    GdiplusShutdown(gtoken);
    CoUninitialize();
    if (mtx) CloseHandle(mtx);
    if (g_restartAfterExit) { wchar_t self[MAX_PATH]; GetModuleFileNameW(nullptr, self, MAX_PATH); ShellExecuteW(nullptr, L"open", self, nullptr, nullptr, SW_SHOWNORMAL); }
    return 0;
}
