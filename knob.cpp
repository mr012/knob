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
#include <devicetopology.h>
#include <physicalmonitorenumerationapi.h>
#include <highlevelmonitorconfigurationapi.h>
#include <lowlevelmonitorconfigurationapi.h>
#include <wbemidl.h>
#include <winhttp.h>
#include <cfgmgr32.h>
#include <shcore.h>
#include <roapi.h>
#include <winstring.h>
#include <d2d1.h>
#include <d2d1_1.h>
#include <d2d1effects.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <ctime>
#include <map>
#include <set>
#include <string>
#include <vector>
namespace Gdiplus { using std::min; using std::max; }
#include <gdiplus.h>
using namespace Gdiplus;

#define APP_NAME    L"Knob"
#define APP_VERSION L"1.42.11"
#define UPDATE_REPO L"mr012/knob"   // GitHub repo whose latest release is checked for updates   // compared when a newer exe replaces a running one

// ---------------------------------------------------------------- globals
enum { WM_APP_TRAY = WM_APP + 1, WM_APP_AUDIOCHANGED, WM_APP_MONITORS, WM_APP_SHOW, WM_APP_VOLCHANGED, WM_APP_VERSION, WM_APP_QUIT, WM_APP_UPDINFO, WM_APP_CALDONE, WM_APP_FOCUSPANEL, WM_APP_MEDIA };
enum { TIMER_REFRESH = 1, TIMER_AUDIODEBOUNCE = 2, TIMER_ANIM = 3, TIMER_IDENT = 4, TIMER_SPIN = 5, TIMER_UPDCHECK = 7, TIMER_TRAYPIN = 8, TIMER_CALRETRY = 9, TIMER_SCREENOFF = 10, TIMER_NIGHT = 14, TIMER_DELAYAPPLY = 15, TIMER_TRIM = 16, TIMER_FOCUSWATCH = 17, TIMER_OFFAT = 18, TIMER_POPKEY = 19, TIMER_LOCKKEY = 20 };
enum Tab { T_SOUND = 0, T_APPS = 1, T_DISPLAY = 2 };

static HINSTANCE g_inst;
static HWND g_hwnd;
static UINT WM_TASKBARCREATED;
static NOTIFYICONDATAW g_nid;
static HICON g_trayIcon;
static bool g_visible = false;
static HWND g_set;   // Settings window (declared early: the panel checks it)
static bool g_pairHidden = false;   // panel tucked away because Settings went behind other windows
static DWORD g_hideTick = 0, g_shownTick = 0; static bool g_hadFocus = false, g_focusRetried = false;   // the panel on its own: closes as soon as it loses focus
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
static int SI();

// global shortcuts
// a short, fixed list of actions (no macros). None has keys until the user picks some.
// The list in Settings is in this order; "Switch to" and "Apply profile" name a speaker or profile and can be added more than once.
enum HkAction { HK_OPEN, HK_TABSOUND, HK_TABAPPS, HK_TABDISPLAY, HK_SWITCHTO, HK_NEXTOUT, HK_VOLUP, HK_VOLDOWN, HK_MUTE, HK_MIC, HK_APPMUTE,
                HK_BRIUP, HK_BRIDOWN, HK_APPLYPROF, HK_SCREENOFF, HK_SYNC, HK_PARTY, HK_COUNT };
struct HotKey { UINT mods; UINT vk; bool ok; };
struct Shortcut { int act; std::wstring param; HotKey k; };
static std::vector<Shortcut> g_sc;   // one row each in Settings; nothing is set until the user adds it
static const wchar_t* HK_NAMES[HK_COUNT] = {L"Open or close Knob", L"Open on Sound", L"Open on Apps", L"Open on Display", L"Switch to", L"Next speaker",
    L"Volume up", L"Volume down", L"Mute or unmute", L"Mute microphone", L"Mute the app in front", L"Brightness up", L"Brightness down",
    L"Apply profile", L"Turn off displays", L"Run Auto sync", L"Show party screen"};
static const wchar_t* HK_KEYS[HK_COUNT] = {L"open", L"tabsound", L"tabapps", L"tabdisplay", L"switchto", L"nextout", L"volup", L"voldown", L"mute", L"mic",
    L"appmute", L"briup", L"bridown", L"applyprof", L"screenoff", L"sync", L"party"};
static int HkParamKind(int a) { return a == HK_SWITCHTO ? 1 : a == HK_APPLYPROF ? 2 : 0; }   // 1 a speaker, 2 a profile
static bool g_noSave = false, g_restartAfterExit = false;
static int g_themeMode = 0;          // 0 follow Windows, 1 light, 2 dark
static int g_openOn = 0;             // where the panel opens: 0 main display (like Windows' own flyouts), 1 the screen with the mouse
static std::wstring g_callDev;       // output for calls (empty = same as main)
static bool g_autoSwitch = true;     // switch to headphones when they connect
static std::wstring g_autoPrev, g_autoTarget;   // output to go back to when they disconnect
static std::set<std::wstring> g_autoPrevMirror;  // speakers that played together before the headphones took over
static std::set<std::wstring> g_prevOutIds; static bool g_prevOutKnown = false;
// update check (GitHub releases)
enum { UPD_NONE, UPD_AVAILABLE, UPD_DOWNLOADING, UPD_FAILED };
static int g_updState = UPD_NONE;
static std::wstring g_updVersion, g_updUrl, g_updStatus = L"";
static volatile LONG g_updBusy = 0;
static Gdiplus::RectF g_verRect;
static int g_capture = -1;      // shortcut row waiting for a key combo
static int g_step = 5;          // volume step for shortcuts (%)

// display profiles: name -> per-monitor brightness/contrast
struct Profile { std::wstring name; std::map<std::wstring, std::pair<int, int>> v; };
static std::vector<Profile> g_prof;   // user-named display profiles (max 8)
static const int MAX_PROFILES = 8;
static bool g_restoreBright = true;                                // put each monitor back to its last brightness when it reconnects
static std::map<std::wstring, std::pair<int, int>> g_lastMon;       // monitor name -> last brightness / contrast
static std::map<std::wstring, int> g_delay;                         // output id -> sync delay (ms)
static bool g_restoreAllNext = false;                               // after resume: treat every monitor as reconnected
struct NightCfg { bool on = false; int str = 40; };
static std::map<std::wstring, NightCfg> g_night;   // monitor name -> night light
static int g_nightAuto = 0;                        // 0 whenever it's switched on, 1 evenings only (7 pm - 7 am)
static double g_geoLat = 999, g_geoLon = 999; static long long g_geoWhen = 0;   // rough location for "Sunset to sunrise" (2 decimals)
static int g_newSeen = 0;   // "NEW" tags in Settings: a bit per setting, cleared once you've used it
// Turn off displays, the clock on the black screen, Party screen, refresh rate on battery
static const int OFF_MIN[5] = {0, 15, 30, 60, 120};
static int g_offDelay = 0;              // Turn off displays: 0 right away, else after OFF_MIN[g_offDelay] minutes
static DWORD g_offAt = 0;               // a timer is running: GetTickCount() when the screens go off
static bool g_offClock = false;         // the time and date in dim grey on the black screen
static bool g_offFull = false;          // also tell monitors to power their screen down (DDC/CI), so an LCD's backlight goes dark too
static std::wstring g_clockScr;         // which screen shows it: "" the main screen, "*" every screen, else a screen's name
static std::wstring g_partyScr;         // Party screen: "" the main screen, else a screen's name
static int g_partyLook = 0;             // 0 glow waves, 1 aurora, 2 light line
static int g_partyClose = 0;            // 0 any key or mouse move ends it, 1 only Esc
static bool g_partyInfo = true;         // the cover, title and artist small in a corner
static bool g_rateBat = false;          // the laptop screen drops to 60 Hz on battery and goes back to its highest on power
// flyouts (Settings › Flyouts): off until you switch them on, so they never double up with another flyout app
static bool g_popMedia = false, g_popNext = false, g_popLock = false;
static int g_fStayMs = 2000, g_fAnimMs = 300, g_lkStayMs = 2000;   // g_lkStayMs: how long the lock-key flyout stays, apart from the others;   // how long flyouts stay (0 = media stays until you close it) and how long they take to slide in (0 = no motion)
static int g_fPos = 0, g_fScreen = 0, g_fLayout = 0, g_fGlow = 1, g_lkKeys = 15;   // g_lkKeys: Caps 1, Num 2, Scroll 4, Insert 8
static bool g_fCover = true, g_fCentre = true, g_fPlayer = true;

static std::wstring OffLeft(); static void ScreenOffRequest(); static void PartyStart();
static bool IsClockScreen(const RECT& rc); static void PaintClock(HWND h, HDC dc);
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
static const PROPERTYKEY PK_HwInstance = {{0xb3f8fa53, 0x0004, 0x438e, {0x90, 0x03, 0x51, 0xa4, 0x6e, 0x13, 0x9b, 0xfc}}, 2};   // "{1}.USB\\VID_..." / "{1}.BTHENUM\\..." / "{1}.HDAUDIO\\..."
static const PROPERTYKEY PK_FormFactor = {{0x1da5d803, 0xd492, 0x4edd, {0x8c, 0x23, 0xe0, 0xc0, 0xff, 0xee, 0x7f, 0x0e}}, 0};

// ---------------------------------------------------------------- models
enum DevKind { DK_SPEAKER, DK_HEADPHONES, DK_MIC, DK_DISPLAY };
enum DevIcon { DI_SPEAKER, DI_LAPTOP, DI_DISPLAY, DI_HEADPHONES, DI_EARBUDS, DI_USB, DI_VIRTUAL, DI_MIC };
struct Endpoint {
    std::wstring id, name, detail;   // name/detail as shown: recognisable name, then how it's connected
    std::wstring rawName, rawIface;  // what Windows calls it
    int kind = DK_SPEAKER, icon = DI_SPEAKER;
    bool bt = false;
    int battery = -1;   // Bluetooth devices that report it, percent
    bool isDefault = false;
    IAudioEndpointVolume* vol = nullptr;
    float level = 0;
    BOOL mute = FALSE;
};
struct AppGroup {
    std::wstring key, name, path;
    bool system = false;
    std::vector<DWORD> pids;
    std::wstring outId;   // output this app is sent to (empty = follows the main output)
    std::vector<ISimpleAudioVolume*> vols;
    std::vector<Meter*> meters;
    float level = 1;
    BOOL mute = FALSE;
    float peak = 0;
    bool onMain = false;   // has a session on the main speaker: what Windows' own volume mixer lists
};
struct ProcInfo { std::wstring name; Gdiplus::Bitmap* icon = nullptr; };
struct AppLive { float smooth = 0; DWORD lastActive = 0; };

static IMMDeviceEnumerator* g_enum;
#ifdef KNOB_DEMO   // development builds only (build.sh dev): Knob.exe --demo shows the design's sample devices, apps and screens
static bool g_demo = false;
#else
static constexpr bool g_demo = false;   // official builds: no demo mode, and the compiler leaves all of it out
#endif
static std::wstring g_demoDefault;   // demo: which speaker "Windows" plays on   // --demo: the design's sample devices, apps and screens (for side-by-side checks)
static std::vector<Endpoint> g_out, g_in;
static std::vector<AppGroup> g_apps;
static std::map<std::wstring, ProcInfo> g_procCache;
static std::map<std::wstring, AppLive> g_live;
static std::set<std::wstring> g_mirror;   // extra outputs that play a copy of the default output

struct MonUI { std::wstring name, detail, gdi; int pct = 0, con = 50, orient = 0; bool ok = false, hasCon = false, internal = false; RECT rc{};
               int cloneOf = -1; LUID adapter{}; UINT32 target = 0; bool hdrOk = false, hdr = false;   // duplicate of another screen; HDR
               std::vector<int> rates; int rate = 0; std::vector<int> inputs; int input = -1; };
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
    int isz = std::max(64, (int)S(48));   // taken big and shrunk when drawn: a 22 px copy was stretched to 18-24 px and looked soft
    HICON big = nullptr;
    if (SHDefExtractIconW(path.c_str(), 0, 0, &big, nullptr, MAKELONG(isz, isz)) == S_OK && big) { pi.icon = IconToBitmap(big); DestroyIcon(big); }
    return g_procCache[path] = pi;
}
static void DrawAppIcon(Graphics& g, Gdiplus::Bitmap* icn, RectF r) {   // shrink the big icon smoothly, with no halo at its edges
    Gdiplus::ImageAttributes ia; ia.SetWrapMode(Gdiplus::WrapModeTileFlipXY);
    auto im = g.GetInterpolationMode(); auto po = g.GetPixelOffsetMode();
    g.SetInterpolationMode(InterpolationModeHighQualityBicubic); g.SetPixelOffsetMode(PixelOffsetModeHalf);
    g.DrawImage(icn, r, 0, 0, (REAL)icn->GetWidth(), (REAL)icn->GetHeight(), Gdiplus::UnitPixel, &ia);
    g.SetInterpolationMode(im); g.SetPixelOffsetMode(po);
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
    g_keepAwake = GetPrivateProfileIntW(L"main", L"keepawake", 0, g_iniPath.c_str()) != 0;
    g_tab = std::max(0, std::min(2, (int)GetPrivateProfileIntW(L"main", L"tab", 0, g_iniPath.c_str())));
    g_style = std::max(0, std::min(1, (int)GetPrivateProfileIntW(L"main", L"style", 1, g_iniPath.c_str())));
    g_accentHue = (int)GetPrivateProfileIntW(L"main", L"accenthue", 25, g_iniPath.c_str()) % 360;
    g_accentSat = std::max(0, std::min(100, (int)GetPrivateProfileIntW(L"main", L"accentsat", 85, g_iniPath.c_str())));
    g_accentId = GetPrivateProfileIntW(L"main", L"accentmode", 0, g_iniPath.c_str()) ? 1 : 0;
    g_restoreBright = GetPrivateProfileIntW(L"main", L"restorebright", 1, g_iniPath.c_str()) != 0;
    g_trayPinned = (int)GetPrivateProfileIntW(L"main", L"traypinned", -1, g_iniPath.c_str());
    g_themeMode = std::max(0, std::min(2, (int)GetPrivateProfileIntW(L"main", L"theme", 0, g_iniPath.c_str())));
    g_openOn = std::max(0, std::min(1, (int)GetPrivateProfileIntW(L"main", L"openon", 0, g_iniPath.c_str())));
    g_autoSwitch = GetPrivateProfileIntW(L"main", L"autoswitch", 1, g_iniPath.c_str()) != 0;
    g_nightAuto = std::min(2, (int)GetPrivateProfileIntW(L"main", L"nightauto", 0, g_iniPath.c_str()));
    g_newSeen = (int)GetPrivateProfileIntW(L"main", L"newseen", 0, g_iniPath.c_str());
    {
        auto gi = [&](const wchar_t* k, int d, int lo, int hi) { return std::max(lo, std::min(hi, (int)GetPrivateProfileIntW(L"main", k, d, g_iniPath.c_str()))); };
        auto gs = [&](const wchar_t* k) { wchar_t b[256] = {}; GetPrivateProfileStringW(L"main", k, L"", b, 256, g_iniPath.c_str()); return std::wstring(b); };
        g_offDelay = gi(L"offdelay", 0, 0, 4); g_offClock = gi(L"offclock", 0, 0, 1); g_offFull = gi(L"offfull", 0, 0, 1); g_clockScr = gs(L"clockscr");
        g_partyScr = gs(L"partyscr"); { int old = gi(L"partylook", 0, 0, 3); old = old == 1 ? 0 : old > 1 ? old - 1 : 0; g_partyLook = gi(L"partylook2", old, 0, 2); }   // the old list had Mirror bars second g_partyInfo = gi(L"partyinfo", 1, 0, 1); g_partyClose = gi(L"partyclose", 0, 0, 1); g_rateBat = gi(L"ratebat", 0, 0, 1);
        g_popMedia = gi(L"popmedia", 0, 0, 1); g_popNext = gi(L"popnext", 0, 0, 1); g_popLock = gi(L"poplock", 0, 0, 1);
        g_fPos = gi(L"fpos", 0, 0, 5); g_fScreen = gi(L"fscreen", 0, 0, 2); g_fStayMs = gi(L"fstayms", 2000, 0, 60000); g_fAnimMs = gi(L"fanimms", 300, 0, 2000); g_lkStayMs = gi(L"lkstayms", g_fStayMs ? g_fStayMs : 2000, 100, 60000);
        g_fLayout = gi(L"flayout", 0, 0, 1); g_fGlow = gi(L"fglow", 1, 0, 5); g_lkKeys = gi(L"lkkeys", 15, 0, 15);
        g_fCover = gi(L"fcover", 1, 0, 1); g_fCentre = gi(L"fcentre", 1, 0, 1); g_fPlayer = gi(L"fplayer", 1, 0, 1);
    }
    { wchar_t b[64]; GetPrivateProfileStringW(L"main", L"geo", L"", b, 64, g_iniPath.c_str()); double la, lo; long long w;
      if (swscanf(b, L"%lf,%lf,%lld", &la, &lo, &w) == 3) { g_geoLat = la; g_geoLon = lo; g_geoWhen = w; } }
    { wchar_t cb[512] = {}; GetPrivateProfileStringW(L"main", L"calldev", L"", cb, 512, g_iniPath.c_str()); g_callDev = cb; }
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
        readSection(L"nightlight", [](const std::wstring& k, const std::wstring& v) {
            int on = 0, st = 40; if (swscanf(v.c_str(), L"%d,%d", &on, &st) >= 1) g_night[k] = {on != 0, std::max(0, std::min(100, st))}; });
        readSection(L"lastdisplay", [](const std::wstring& k, const std::wstring& v) {
            int b = -1, c = -1; if (swscanf(v.c_str(), L"%d,%d", &b, &c) >= 1) g_lastMon[k] = {b, c}; });
    }
    g_step = (int)GetPrivateProfileIntW(L"main", L"step", 5, g_iniPath.c_str());
    if (g_step != 2 && g_step != 5 && g_step != 10) g_step = 5;
    g_sc.clear();
    int nsc = (int)GetPrivateProfileIntW(L"shortcuts", L"count", -1, g_iniPath.c_str());
    if (nsc >= 0) {
        for (int i = 0; i < nsc && i < 64; i++) {
            wchar_t b[512];
            if (!GetPrivateProfileStringW(L"shortcuts", (L"s" + std::to_wstring(i)).c_str(), L"", b, 512, g_iniPath.c_str())) continue;
            std::wstring s = b; Shortcut x{-1, L"", {0, 0, false}};
            size_t p1 = s.find(L'|'), p2 = p1 == std::wstring::npos ? p1 : s.find(L'|', p1 + 1), p3 = p2 == std::wstring::npos ? p2 : s.find(L'|', p2 + 1);
            if (p3 == std::wstring::npos) continue;
            std::wstring key = s.substr(0, p1);
            for (int a = 0; a < HK_COUNT; a++) if (key == HK_KEYS[a]) x.act = a;
            if (x.act < 0) continue;
            x.k.mods = (UINT)_wtoi(s.substr(p1 + 1, p2 - p1 - 1).c_str()); x.k.vk = (UINT)_wtoi(s.substr(p2 + 1, p3 - p2 - 1).c_str());
            x.param = s.substr(p3 + 1);
            g_sc.push_back(x);
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
    std::wstring s;
    for (auto& id : g_mirror) { if (!s.empty()) s += L'|'; s += id; }
    WritePrivateProfileStringW(L"main", L"mirror", s.c_str(), g_iniPath.c_str());
    WritePrivateProfileStringW(L"main", L"theme", std::to_wstring(g_themeMode).c_str(), g_iniPath.c_str());
    WritePrivateProfileStringW(L"main", L"openon", std::to_wstring(g_openOn).c_str(), g_iniPath.c_str());
    WritePrivateProfileStringW(L"main", L"autoswitch", g_autoSwitch ? L"1" : L"0", g_iniPath.c_str());
    WritePrivateProfileStringW(L"main", L"nightauto", std::to_wstring(g_nightAuto).c_str(), g_iniPath.c_str());
    WritePrivateProfileStringW(L"main", L"newseen", std::to_wstring(g_newSeen).c_str(), g_iniPath.c_str());
    {
        auto wi = [&](const wchar_t* k, int v) { WritePrivateProfileStringW(L"main", k, std::to_wstring(v).c_str(), g_iniPath.c_str()); };
        wi(L"offdelay", g_offDelay); wi(L"offclock", g_offClock); wi(L"offfull", g_offFull); WritePrivateProfileStringW(L"main", L"clockscr", g_clockScr.c_str(), g_iniPath.c_str());
        WritePrivateProfileStringW(L"main", L"partyscr", g_partyScr.c_str(), g_iniPath.c_str()); wi(L"partylook2", g_partyLook); wi(L"partyinfo", g_partyInfo); wi(L"partyclose", g_partyClose); wi(L"ratebat", g_rateBat);
        wi(L"popmedia", g_popMedia); wi(L"popnext", g_popNext); wi(L"poplock", g_popLock);
        wi(L"fpos", g_fPos); wi(L"fscreen", g_fScreen); wi(L"fstayms", g_fStayMs); wi(L"fanimms", g_fAnimMs); wi(L"lkstayms", g_lkStayMs); wi(L"flayout", g_fLayout); wi(L"fglow", g_fGlow); wi(L"lkkeys", g_lkKeys);
        wi(L"fcover", g_fCover); wi(L"fcentre", g_fCentre); wi(L"fplayer", g_fPlayer);
    }
    if (g_geoLat < 900) { wchar_t b[64]; swprintf(b, 64, L"%.2f,%.2f,%lld", g_geoLat, g_geoLon, g_geoWhen); WritePrivateProfileStringW(L"main", L"geo", b, g_iniPath.c_str()); }
    { std::wstring n; for (auto& kv : g_night) { n += kv.first + L"=" + (kv.second.on ? L"1," : L"0,") + std::to_wstring(kv.second.str); n.push_back(0); } n.push_back(0);
      WritePrivateProfileSectionW(L"nightlight", n.c_str(), g_iniPath.c_str()); }
    WritePrivateProfileStringW(L"main", L"calldev", g_callDev.c_str(), g_iniPath.c_str());
    WritePrivateProfileStringW(L"main", L"keepawake", g_keepAwake ? L"1" : L"0", g_iniPath.c_str());
    WritePrivateProfileStringW(L"main", L"tab", std::to_wstring(g_tab).c_str(), g_iniPath.c_str());
    WritePrivateProfileStringW(L"main", L"style", std::to_wstring(g_style).c_str(), g_iniPath.c_str());
    WritePrivateProfileStringW(L"main", L"accentmode", std::to_wstring(g_accentId).c_str(), g_iniPath.c_str());
    WritePrivateProfileStringW(L"main", L"accenthue", std::to_wstring(g_accentHue).c_str(), g_iniPath.c_str());
    WritePrivateProfileStringW(L"main", L"accentsat", std::to_wstring(g_accentSat).c_str(), g_iniPath.c_str());
    WritePrivateProfileStringW(L"main", L"restorebright", g_restoreBright ? L"1" : L"0", g_iniPath.c_str());
    {
        std::wstring d, l;
        for (auto& kv : g_delay) if (kv.second != 0) { d += kv.first + L"=" + std::to_wstring(kv.second); d.push_back(0); }
        for (auto& kv : g_lastMon) { l += kv.first + L"=" + std::to_wstring(kv.second.first) + L"," + std::to_wstring(kv.second.second); l.push_back(0); }
        d.push_back(0); l.push_back(0);
        WritePrivateProfileSectionW(L"delay", d.c_str(), g_iniPath.c_str());
        WritePrivateProfileSectionW(L"lastdisplay", l.c_str(), g_iniPath.c_str());
    }
    WritePrivateProfileStringW(L"main", L"step", std::to_wstring(g_step).c_str(), g_iniPath.c_str());
    WritePrivateProfileSectionW(L"shortcuts", L"\0", g_iniPath.c_str());
    WritePrivateProfileStringW(L"shortcuts", L"count", std::to_wstring(g_sc.size()).c_str(), g_iniPath.c_str());
    for (size_t i = 0; i < g_sc.size(); i++)
        WritePrivateProfileStringW(L"shortcuts", (L"s" + std::to_wstring(i)).c_str(),
            (std::wstring(HK_KEYS[g_sc[i].act]) + L"|" + std::to_wstring(g_sc[i].k.mods) + L"|" + std::to_wstring(g_sc[i].k.vk) + L"|" + g_sc[i].param).c_str(), g_iniPath.c_str());
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

// ---- what kind of device is this, and what should we call it?
static bool HasWord(const std::wstring& low, const wchar_t* w) {   // whole-word-ish match
    size_t L = wcslen(w);
    for (size_t p = low.find(w); p != std::wstring::npos; p = low.find(w, p + 1)) {
        bool a = p == 0 || !iswalnum(low[p - 1]), b = p + L >= low.size() || !iswalnum(low[p + L]);
        if (a && b) return true;
    }
    return false;
}
static bool LooksLikeEarbuds(const std::wstring& name) {
    std::wstring l = ToLower(name);
    for (auto w : {L"buds", L"airpods", L"earpods", L"earbuds", L"freebuds", L"pods", L"tws", L"ear"}) if (HasWord(l, w)) return true;
    for (auto w : {L"buds ", L"wf-", L"galaxy buds", L"pixel buds", L"jabra elite", L"nothing ear", L"ear (", L"liberty"}) if (l.find(w) != std::wstring::npos) return true;
    return l.size() > 4 && l.compare(l.size() - 4, 4, L"buds") == 0;
}
static bool IsGenericName(const std::wstring& n) {
    std::wstring l = ToLower(n);
    for (auto w : {L"speakers", L"speaker", L"headphones", L"headphone", L"headset", L"speakers/headphones", L"headset earphone", L"digital output",
                   L"line out", L"spdif out", L"s/pdif", L"microphone", L"microphone array", L"headset microphone", L"line in", L"hands-free", L"stereo mix", L"external microphone"})
        if (l == w) return true;
    return false;
}
static bool IsCodecName(const std::wstring& n) {   // chip / driver names that mean nothing to people
    std::wstring l = ToLower(n);
    for (auto w : {L"realtek", L"high definition audio", L"intel", L"smart sound", L"amd", L"conexant", L"idt", L"cirrus", L"synaptics", L"audio device", L"soundwire", L"sound blaster", L"nvidia", L"microsoft"})
        if (l.find(w) != std::wstring::npos) return true;
    return false;
}
static std::wstring CleanBtName(std::wstring n) {   // "Buds Pro 2 Hands-Free AG Audio" -> "Buds Pro 2"
    for (auto w : {L" Hands-Free AG Audio", L" Hands-Free", L" Stereo", L" Headset", L" Avrcp Transport"}) {
        size_t p = n.find(w); if (p != std::wstring::npos && p > 0) n = n.substr(0, p);
    }
    return n;
}
static bool IsLaptop() {
    static int v = -1;
    if (v < 0) { SYSTEM_POWER_STATUS ps; v = GetSystemPowerStatus(&ps) && ps.BatteryFlag != 128 && ps.BatteryFlag != 255 ? 1 : 0; }
    return v == 1;
}
static std::wstring ConnectedPath(IMMDevice* dev) {   // the hardware behind the endpoint: hdaudio / usb / bthenum / root ...
    std::wstring r;
    IDeviceTopology* t = nullptr;
    if (SUCCEEDED(dev->Activate(__uuidof(IDeviceTopology), CLSCTX_ALL, nullptr, (void**)&t)) && t) {
        IConnector* c = nullptr;
        if (SUCCEEDED(t->GetConnector(0, &c)) && c) {
            LPWSTR id = nullptr;
            if (SUCCEEDED(c->GetDeviceIdConnectedTo(&id)) && id) { r = ToLower(id); CoTaskMemFree(id); }
            c->Release();
        }
        t->Release();
    }
    return r;
}
// Bluetooth speakers often report a plain hardware path, so also ask: does this endpoint's physical device (container) have a Bluetooth part?
static const PROPERTYKEY PK_ContainerId = {{0x8c7ed206, 0x3f8a, 0x4827, {0xb3, 0xab, 0xae, 0x9e, 0x1f, 0xae, 0xfc, 0x6c}}, 2};
static std::set<std::wstring> g_btContainers;
static std::map<std::wstring, int> g_btBattery;   // container -> battery percent, for Bluetooth devices that report it
static std::wstring GuidKey(const GUID& g) { wchar_t b[64]; swprintf(b, 64, L"%08lx-%04x-%04x-%02x%02x%02x%02x%02x%02x%02x%02x", g.Data1, g.Data2, g.Data3, g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3], g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]); return b; }
static void RefreshBtContainers() {
    g_btContainers.clear(); g_btBattery.clear();
    static const DEVPROPKEY bk = {{0x104ea319, 0x6ee2, 0x4701, {0xbd, 0x47, 0x8d, 0xdb, 0xf4, 0x25, 0xbb, 0xe5}}, 2};   // Windows keeps a Bluetooth device's battery here
    static const DEVPROPKEY dk = {{0x8c7ed206, 0x3f8a, 0x4827, {0xb3, 0xab, 0xae, 0x9e, 0x1f, 0xae, 0xfc, 0x6c}}, 2};
    for (auto en : {L"BTHENUM", L"BTHLEENUM", L"BTHHFENUM", L"BTHLE"}) {
        ULONG len = 0;
        if (CM_Get_Device_ID_List_SizeW(&len, en, CM_GETIDLIST_FILTER_ENUMERATOR) != CR_SUCCESS || len < 2) continue;
        std::vector<wchar_t> buf(len + 2, 0);
        if (CM_Get_Device_ID_ListW(en, buf.data(), len, CM_GETIDLIST_FILTER_ENUMERATOR) != CR_SUCCESS) continue;
        for (const wchar_t* p = buf.data(); *p; p += wcslen(p) + 1) {
            DEVINST di; if (CM_Locate_DevNodeW(&di, (DEVINSTID_W)p, CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS) continue;
            GUID g{}; DEVPROPTYPE ty = 0; ULONG sz = sizeof(g);
            if (CM_Get_DevNode_PropertyW(di, &dk, &ty, (PBYTE)&g, &sz, 0) == CR_SUCCESS && sz == sizeof(g)) {
                std::wstring k = GuidKey(g);
                if (k != L"00000000-0000-0000-ffff-ffffffffffff" && k != L"00000000-0000-0000-0000-000000000000") {   // not the PC itself: the Bluetooth radio lives there, and the built-in speakers too
                    g_btContainers.insert(k);
                    BYTE pct = 0; DEVPROPTYPE bt2 = 0; ULONG bs = sizeof(pct);
                    if (CM_Get_DevNode_PropertyW(di, &bk, &bt2, &pct, &bs, 0) == CR_SUCCESS && bs >= 1 && pct <= 100) { auto it = g_btBattery.find(k); if (it == g_btBattery.end() || pct > it->second) g_btBattery[k] = pct; }
                }
            }
        }
    }
}
static bool InBtContainer(IPropertyStore* ps, int* battery = nullptr) {
    PROPVARIANT pv; PropVariantInit(&pv); bool r = false; std::wstring key;
    if (SUCCEEDED(ps->GetValue(PK_ContainerId, &pv))) {
        if (pv.vt == VT_CLSID && pv.puuid) key = GuidKey(*pv.puuid);
        else if (pv.vt == VT_LPWSTR && pv.pwszVal) { GUID g; if (SUCCEEDED(CLSIDFromString(pv.pwszVal, &g))) key = GuidKey(g); }
        r = !key.empty() && g_btContainers.count(key) > 0;
        if (r && battery) { auto it = g_btBattery.find(key); if (it != g_btBattery.end()) *battery = it->second; }
    }
    PropVariantClear(&pv); return r;
}
static void Classify(Endpoint& e, bool capture, UINT ff, const std::wstring& path) {
    std::wstring nm = e.rawName, ifc = e.rawIface, both = ToLower(nm + L" " + ifc);
    bool inCont = path.find(L"|btcontainer") != std::wstring::npos;   // the physical device also has a Bluetooth part: only believed when the sound chip isn't the PC's own
    bool bt = path.find(L"bth") != std::wstring::npos || path.find(L"bluetooth") != std::wstring::npos;
    bool builtChip = path.find(L"hdaudio") != std::wstring::npos || path.find(L"intelaudio") != std::wstring::npos || path.find(L"intcaudio") != std::wstring::npos ||
                     path.find(L"acp") != std::wstring::npos || path.find(L"sst") != std::wstring::npos || path.find(L"soundwire") != std::wstring::npos || path.find(L"pci") != std::wstring::npos;
    if (!bt && inCont && !builtChip && path.find(L"usb") == std::wstring::npos) bt = true;
    bool usb = !bt && path.find(L"usb") != std::wstring::npos;
    bool codec = !bt && !usb && builtChip;   // built-in sound chip
    if (!bt && !usb && !codec && (ff == 3 || ff == 5) && !IsCodecName(ifc) && !ifc.empty() && LooksLikeEarbuds(ifc)) bt = true;   // earbuds are wireless
    bool virt = path.find(L"root#") != std::wstring::npos || path.find(L"\\root") != std::wstring::npos;
    for (auto w : {L"vb-audio", L"voicemeeter", L"virtual", L"steam streaming", L"nvidia broadcast", L"cable input", L"cable output"}) if (both.find(w) != std::wstring::npos) virt = true;
    if (bt) virt = false;
    bool headset = ff == 3 || ff == 5;
    e.bt = bt;
    std::wstring name = nm, conn = ifc;
    if (IsGenericName(nm) && !ifc.empty() && !IsCodecName(ifc)) name = bt ? CleanBtName(ifc) : ifc;
    if (capture) {
        e.icon = (bt || headset) ? DI_HEADPHONES : DI_MIC;
        if (bt) conn = L"Bluetooth";
        else if (usb) conn = L"USB";
        else if (virt) { conn = L"Virtual"; e.icon = DI_VIRTUAL; }
        else if (ff == 4 || IsGenericName(nm)) {   // built-in or jack
            if (headset) { name = L"Headset microphone"; conn = codec ? L"3.5 mm jack" : L""; }
            else if (codec || path.size() <= 1) { if (IsLaptop() || ToLower(nm).find(L"array") != std::wstring::npos) name = IsLaptop() ? L"Laptop microphone" : L"Microphone"; conn = L"Built-in"; }
            else conn = IsCodecName(ifc) ? L"" : conn;
        }
    } else if (ff == 9) { e.icon = DI_DISPLAY; conn = L"HDMI / DP"; if (IsGenericName(nm) || IsCodecName(nm)) name = ifc.empty() ? L"Display audio" : ifc; }
    else if (ff == 0) { e.icon = DI_DISPLAY; conn = L"Wireless display"; }
    else if (virt) { e.icon = DI_VIRTUAL; conn = L"Virtual"; }
    else if (bt) { e.icon = (LooksLikeEarbuds(name) ? DI_EARBUDS : headset ? DI_HEADPHONES : DI_SPEAKER); conn = ff == 5 ? L"Bluetooth \u00B7 calls" : L"Bluetooth"; }
    else if (usb) { e.icon = headset ? (LooksLikeEarbuds(name) ? DI_EARBUDS : DI_HEADPHONES) : DI_USB; conn = L"USB"; }
    else if (headset) {   // only call it a jack when it's really the built-in sound chip
        e.icon = LooksLikeEarbuds(name) ? DI_EARBUDS : DI_HEADPHONES;
        conn = codec ? L"3.5 mm jack" : L"";
        if (codec && (IsCodecName(name) || IsGenericName(name))) name = L"Headphones";
    }
    else if (ff == 8) { e.icon = DI_SPEAKER; conn = L"Optical"; }
    else if (ff == 2) { e.icon = DI_SPEAKER; conn = L"Line out"; }
    else if ((ff == 1 || IsGenericName(nm)) && (codec || path.size() <= 1)) {
        if (IsLaptop()) { e.icon = DI_LAPTOP; name = L"Laptop speakers"; conn = L"Built-in"; }
        else { e.icon = DI_SPEAKER; name = L"Speakers"; conn = codec ? L"Built-in" : L""; }
    }
    else conn = IsCodecName(ifc) ? L"" : conn;   // unknown: no label rather than a wrong one
    // names stay exactly as Windows shows them ("Speakers" + "Realtek(R) Audio"); only the icon comes from the checks above
    (void)name; (void)conn;
    // the device's own name first ("realme Brick Speaker"), Windows' type name ("Speakers") second
    if (!ifc.empty() && ToLower(ifc) != ToLower(nm)) { e.name = ifc; e.detail = nm; }
    else { e.name = nm; e.detail = L""; }
}
static void DemoEndpoints(EDataFlow flow, std::vector<Endpoint>& v) {
    struct D { const wchar_t *n, *d; int kind, icon; bool bt; int lvl; bool def; };
    static const D out[] = {{L"Realtek(R) Audio", L"Speakers", DK_SPEAKER, DI_LAPTOP, false, 17, true}, {L"Arctis Nova 5", L"Headphones", DK_HEADPHONES, DI_HEADPHONES, false, 100, false},
        {L"realme Brick Speaker", L"Speakers", DK_SPEAKER, DI_SPEAKER, true, 60, false}, {L"Nothing CMF", L"CMF Buds Pro 2", DK_HEADPHONES, DI_EARBUDS, true, 70, false},
        {L"27E2N1100", L"NVIDIA High Definition Audio", DK_DISPLAY, DI_DISPLAY, false, 50, false}, {L"CABLE Input", L"VB-Audio Virtual Cable", DK_SPEAKER, DI_VIRTUAL, false, 100, false}};
    static const D in[] = {{L"Realtek(R) Audio", L"Microphone Array", DK_MIC, DI_MIC, false, 100, true}, {L"Arctis Nova 5", L"Headset Microphone", DK_HEADPHONES, DI_HEADPHONES, false, 85, false}};
    bool o = flow == eRender; const D* L = o ? out : in; int n = o ? 6 : 2;
    for (int i = 0; i < n; i++) { Endpoint e; e.id = (o ? L"demo-out-" : L"demo-in-") + std::to_wstring(i); e.name = L[i].n; e.detail = L[i].d; e.kind = L[i].kind; e.icon = L[i].icon;
        e.bt = L[i].bt; if (e.bt) e.battery = i == 2 ? 82 : 64; e.level = L[i].lvl / 100.f; e.isDefault = L[i].def; v.push_back(e); }
    static bool once = false;
    if (o && !once) { once = true; g_mirror = {L"demo-out-1"}; g_delay[L"demo-out-1"] = -10; g_delay[L"demo-out-2"] = -200; }   // the design's state: two speakers together, synced
    if (o && !g_demoDefault.empty()) for (auto& e : v) e.isDefault = e.id == g_demoDefault;
}
static void LoadEndpoints(EDataFlow flow, std::vector<Endpoint>& v) {
    FreeEndpoints(v);
    if (g_demo) { DemoEndpoints(flow, v); return; }
    if (!g_enum) return;
    RefreshBtContainers();
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
        UINT ff = 10;
        if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &ps))) {
            e.name = PropStr(ps, PK_DeviceDesc);
            e.detail = PropStr(ps, PK_IfaceName);
            if (e.name.empty()) e.name = PropStr(ps, PK_FriendlyName);
            PROPVARIANT pv; PropVariantInit(&pv);
            if (SUCCEEDED(ps->GetValue(PK_FormFactor, &pv)) && (pv.vt == VT_UI4 || pv.vt == VT_I4)) {
                ff = pv.ulVal;
                if (ff == 3 || ff == 5) e.kind = DK_HEADPHONES;          // Headphones, Headset
                else if (ff == 9) e.kind = DK_DISPLAY;                   // DigitalAudioDisplayDevice (HDMI/DP)
                else if (ff == 4) e.kind = DK_MIC;
            }
            PropVariantClear(&pv);
            ps->Release();
        }
        if (e.name.empty()) e.name = L"Audio device";
        e.rawName = e.name; e.rawIface = e.detail;
        std::wstring hwPath; int batt = -1;
        if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &ps))) { hwPath = ToLower(PropStr(ps, PK_HwInstance)); if (InBtContainer(ps, &batt)) hwPath += L"|btcontainer"; ps->Release(); ps = nullptr; }
        Classify(e, flow == eCapture, ff, hwPath + L"|" + ConnectedPath(dev));
        if (e.bt && flow != eCapture) e.battery = batt;
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
    if (g_demo) { g_demoDefault = id; return; }
    for (auto& o : g_out) if (o.id == id && o.isDefault) return;   // already the main speaker: no switch, so apps aren't disturbed
    IPolicyConfig* pc = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_PolicyConfigClient, nullptr, CLSCTX_ALL, IID_IPolicyConfig, (void**)&pc)) && pc) {
        pc->SetDefaultEndpoint(id.c_str(), eConsole);
        pc->SetDefaultEndpoint(id.c_str(), eMultimedia);
        if (g_callDev.empty()) pc->SetDefaultEndpoint(id.c_str(), eCommunications);   // calls follow, unless a call device is set
        pc->Release();
    }
}
static void SetCallDevice(const std::wstring& id) {   // empty = same as the main output
    g_callDev = id;
    std::wstring target = id;
    if (target.empty()) { for (auto& o : g_out) if (o.isDefault) target = o.id; }
    IPolicyConfig* pc = nullptr;
    if (!target.empty() && SUCCEEDED(CoCreateInstance(CLSID_PolicyConfigClient, nullptr, CLSCTX_ALL, IID_IPolicyConfig, (void**)&pc)) && pc) {
        pc->SetDefaultEndpoint(target.c_str(), eCommunications); pc->Release();
    }
}
// Windows' own audio settings, reached from Knob
static const wchar_t* AUDIO_KEY = L"Software\\Microsoft\\Multimedia\\Audio";
static DWORD AudioRegGet(const wchar_t* name, DWORD def) { DWORD v = def, sz = sizeof(v); RegGetValueW(HKEY_CURRENT_USER, AUDIO_KEY, name, RRF_RT_REG_DWORD, nullptr, &v, &sz); return v; }
static void AudioRegSet(const wchar_t* name, DWORD v) { RegSetKeyValueW(HKEY_CURRENT_USER, AUDIO_KEY, name, REG_DWORD, &v, sizeof(v)); }
static bool MonoOn() { return AudioRegGet(L"AccessibilityMonoMixState", 0) != 0; }
static int Ducking() { return (int)std::min<DWORD>(3, AudioRegGet(L"UserDuckingPreference", 1)); }   // 0 mute, 1 -80%, 2 -50%, 3 nothing

static bool OutSelected(int i) { return g_out[i].isDefault || g_mirror.count(g_out[i].id) > 0; }
static int SelectedOutCount() { int n = 0; for (int i = 0; i < (int)g_out.size(); i++) if (OutSelected(i)) n++; return n; }
static int DefaultOut() { for (int i = 0; i < (int)g_out.size(); i++) if (g_out[i].isDefault) return i; return -1; }

// ---------------------------------------------------------------- audio: app sessions
static void FreeApps() {
    for (auto& a : g_apps) { for (auto* v : a.vols) v->Release(); for (auto* m : a.meters) m->Release(); }
    g_apps.clear();
}

// ---- per-app output: the same Windows feature as Settings > Volume mixer > app > Output device.
// Windows offers no public API for it; this uses the one Windows itself and EarTrumpet use.
typedef HRESULT (WINAPI* PFN_RoGetActivationFactory)(void* cls, REFIID iid, void** f);
typedef HRESULT (WINAPI* PFN_WindowsCreateString)(PCWSTR s, UINT32 n, void** hs);
typedef HRESULT (WINAPI* PFN_WindowsDeleteString)(void* hs);
typedef PCWSTR (WINAPI* PFN_WindowsGetStringRawBuffer)(void* hs, UINT32* n);
static void* g_apcf = nullptr;   // IAudioPolicyConfigFactory
static PFN_WindowsCreateString pWCS; static PFN_WindowsDeleteString pWDS; static PFN_WindowsGetStringRawBuffer pWGB;
static bool AppRouteReady() {
    static int state = 0;   // 0 not tried, 1 ok, 2 unavailable
    if (state) return state == 1;
    state = 2;
    HMODULE cb = LoadLibraryW(L"combase.dll");
    if (!cb) return false;
    auto pRGAF = (PFN_RoGetActivationFactory)(void*)GetProcAddress(cb, "RoGetActivationFactory");
    pWCS = (PFN_WindowsCreateString)(void*)GetProcAddress(cb, "WindowsCreateString");
    pWDS = (PFN_WindowsDeleteString)(void*)GetProcAddress(cb, "WindowsDeleteString");
    pWGB = (PFN_WindowsGetStringRawBuffer)(void*)GetProcAddress(cb, "WindowsGetStringRawBuffer");
    if (!pRGAF || !pWCS || !pWDS || !pWGB) return false;
    const wchar_t* cls = L"Windows.Media.Internal.AudioPolicyConfig";
    void* hs = nullptr;
    if (FAILED(pWCS(cls, (UINT32)wcslen(cls), &hs))) return false;
    static const GUID iidNew = {0xab3d4648, 0xe242, 0x459f, {0xb0, 0x2f, 0x54, 0x1c, 0x70, 0x30, 0x63, 0x24}};   // Windows 10 21H2 and later
    static const GUID iidOld = {0x2a59116d, 0x6c4f, 0x45e0, {0xa7, 0x4f, 0x70, 0x7e, 0x3f, 0xef, 0x92, 0x58}};
    if (FAILED(pRGAF(hs, iidNew, &g_apcf)) || !g_apcf) { g_apcf = nullptr; pRGAF(hs, iidOld, &g_apcf); }
    pWDS(hs);
    if (!g_apcf) return false;
    state = 1;
    return true;
}
typedef HRESULT (STDMETHODCALLTYPE* PFN_SetPersisted)(void* self, UINT pid, EDataFlow flow, ERole role, void* devId);
typedef HRESULT (STDMETHODCALLTYPE* PFN_GetPersisted)(void* self, UINT pid, EDataFlow flow, ERole role, void** devId);
typedef HRESULT (STDMETHODCALLTYPE* PFN_ClearAll)(void* self);
static void** ApcfVtbl() { return *(void***)g_apcf; }
static std::wstring AppOutputGet(DWORD pid) {   // endpoint id the app is pinned to, or empty
    if (!AppRouteReady()) return L"";
    std::wstring r;
    for (ERole role : {eMultimedia, eConsole}) {
        void* hs = nullptr;
        if (SUCCEEDED(((PFN_GetPersisted)ApcfVtbl()[26])(g_apcf, pid, eRender, role, &hs)) && hs) {
            UINT32 n = 0; PCWSTR b = pWGB(hs, &n);
            std::wstring full(b ? b : L"", n);
            size_t a = full.find(L"MMDEVAPI#"), z = full.rfind(L"#{");
            if (a != std::wstring::npos && z != std::wstring::npos && z > a + 9) r = full.substr(a + 9, z - a - 9);
            pWDS(hs);
        }
        if (!r.empty()) break;
    }
    return r;
}
static bool AppOutputSet(const AppGroup& a, const std::wstring& endpointId) {   // empty id = follow the main output again
    if (!AppRouteReady() || a.pids.empty()) return false;
    void* hs = nullptr;
    if (!endpointId.empty()) {
        std::wstring full = L"\\\\?\\SWD#MMDEVAPI#" + endpointId + L"#{e6327cad-dcec-4949-ae8a-991e976a79d2}";
        if (FAILED(pWCS(full.c_str(), (UINT32)full.size(), &hs))) return false;
    }
    bool ok = true;
    for (DWORD pid : a.pids)
        for (ERole role : {eConsole, eMultimedia})
            if (FAILED(((PFN_SetPersisted)ApcfVtbl()[25])(g_apcf, pid, eRender, role, hs))) ok = false;
    if (hs) pWDS(hs);
    return ok;
}
static void AppOutputResetAll() { if (AppRouteReady()) ((PFN_ClearAll)ApcfVtbl()[27])(g_apcf); }
// per-app extra speakers: live only (never saved), gone when the app or Knob closes
struct AppExtra { DWORD pid = 0; std::wstring primary; std::vector<std::wstring> ids; };
static std::map<std::wstring, AppExtra> g_appExtra;   // app key -> speakers it also plays on
static void RestartAppRouters();
static std::vector<std::wstring> AppRoutes(const AppGroup& a);   // every speaker an app plays on (empty = with everything else)
static void LoadApps(bool all = false) {   // all = include apps Windows marks idle (only for the mute-the-app-in-front shortcut)
    FreeApps();
    if (g_demo) {
        struct D { const wchar_t* n; int lvl; bool sys; } L[] = {{L"Brave", 100, false}, {L"Discord", 72, false}, {L"Spotify", 85, false}, {L"System sounds", 60, true}};
        DWORD pid = 9000;
        for (auto& d : L) { AppGroup a; a.key = d.sys ? L"*system" : ToLower(d.n); a.name = d.n; a.system = d.sys; a.level = d.lvl / 100.f; if (!d.sys) a.pids.push_back(pid++); g_apps.push_back(a); }
        g_apps[1].outId = L"demo-out-1"; g_apps[2].outId = L"demo-out-0";   // Discord on the headphones, Spotify on the laptop and the Brick
        AppExtra x; x.pid = g_apps[2].pids[0]; x.primary = L"demo-out-0"; x.ids = {L"demo-out-2"}; g_appExtra[g_apps[2].key] = x;
        return;
    }
    if (!g_enum) return;
    std::map<std::wstring, size_t> idx;
    DWORD self = GetCurrentProcessId();
    IMMDeviceCollection* c = nullptr;
    if (FAILED(g_enum->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &c))) return;
    std::wstring mainId;   // Windows' mixer shows the apps on the speaker you picked; old sessions left on other speakers stay hidden
    { IMMDevice* dd = nullptr; LPWSTR id = nullptr;
      if (SUCCEEDED(g_enum->GetDefaultAudioEndpoint(eRender, eConsole, &dd)) && dd) { if (SUCCEEDED(dd->GetId(&id)) && id) { mainId = id; CoTaskMemFree(id); } dd->Release(); } }
    UINT n = 0; c->GetCount(&n);
    for (UINT i = 0; i < n; i++) {
        IMMDevice* dev = nullptr;
        if (FAILED(c->Item(i, &dev))) continue;
        bool isMain = false;
        { LPWSTR id = nullptr; if (SUCCEEDED(dev->GetId(&id)) && id) { isMain = mainId == id; CoTaskMemFree(id); } }
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
                                if (isMain) g_apps[it->second].onMain = true;
                                if (!sys && std::find(g_apps[it->second].pids.begin(), g_apps[it->second].pids.end(), pid) == g_apps[it->second].pids.end())
                                    g_apps[it->second].pids.push_back(pid);
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
    for (auto& a : g_apps) if (!a.system) for (DWORD pid : a.pids) { a.outId = AppOutputGet(pid); if (!a.outId.empty()) break; }   // where each app is sent (any of its processes)
    bool gone = false;   // copies for apps that have closed stop here
    for (auto it = g_appExtra.begin(); it != g_appExtra.end();) {
        HANDLE ph = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, it->second.pid); DWORD code = 0;
        bool alive = ph && GetExitCodeProcess(ph, &code) && code == STILL_ACTIVE;
        if (ph) CloseHandle(ph);
        if (alive) ++it; else { it = g_appExtra.erase(it); gone = true; }
    }
    if (gone) RestartAppRouters();
    if (all) return;
    // same list as Windows' volume mixer: every app Windows has on the main speaker, playing or paused.
    // Windows' own helper programs (in the Windows folder, like Biometric Enrollment Host) stay hidden. Apps you sent to another speaker stay too.
    static std::wstring winDir; if (winDir.empty()) { wchar_t w[MAX_PATH] = {}; GetWindowsDirectoryW(w, MAX_PATH); winDir = ToLower(std::wstring(w)) + L"\\"; }
    for (size_t i = g_apps.size(); i-- > 0;) {
        AppGroup& a = g_apps[i];
        if (a.system || !a.outId.empty() || g_appExtra.count(a.key)) continue;
        if (a.onMain && a.key.compare(0, winDir.size(), winDir) != 0) continue;
        for (auto* v : a.vols) v->Release();
        for (auto* m : a.meters) m->Release();
        g_apps.erase(g_apps.begin() + i);
    }
}

static bool AppPlaying(const AppGroup& a) {
    if (g_demo) return a.name == L"Brave" || a.name == L"Spotify";
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
struct RouterCtx { std::wstring src; DWORD pid = 0; std::vector<RouterTarget> targets; volatile LONG stop = 0; HANDLE thread = nullptr;
                   volatile LONG calOnly = -1; bool mic = false; };   // auto sync: -1 normal, -2 all copies silent, k = only target k plays; mic: src is a microphone
static RouterCtx* g_router;

// ---- one app's own sound (Windows 11: "process loopback"), so it can also play on extra speakers
static bool ProcLoopbackOk() {   // Windows 11 / Server 2022 (build 20348) and later
    static int v = -1;
    if (v < 0) {
        typedef LONG (WINAPI* PFN_RtlGetVersion)(PRTL_OSVERSIONINFOW);
        RTL_OSVERSIONINFOW vi = {sizeof(vi)};
        auto f = (PFN_RtlGetVersion)(void*)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
        v = f && f(&vi) == 0 && vi.dwBuildNumber >= 20348 ? 1 : 0;
    }
    return v == 1;
}
struct KnobLoopParams { DWORD activationType; DWORD pid; DWORD mode; };   // AUDIOCLIENT_ACTIVATION_PARAMS, process loopback
struct ActHandler : public IActivateAudioInterfaceCompletionHandler, public IAgileObject {
    LONG ref = 1; HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    virtual ~ActHandler() { CloseHandle(ev); }
    STDMETHODIMP QueryInterface(REFIID r, void** p) override {
        if (IsEqualIID(r, IID_IUnknown) || IsEqualIID(r, __uuidof(IActivateAudioInterfaceCompletionHandler))) *p = static_cast<IActivateAudioInterfaceCompletionHandler*>(this);
        else if (IsEqualIID(r, __uuidof(IAgileObject))) *p = static_cast<IAgileObject*>(this);
        else { *p = nullptr; return E_NOINTERFACE; }
        AddRef(); return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&ref); }
    STDMETHODIMP_(ULONG) Release() override { LONG r = InterlockedDecrement(&ref); if (!r) delete this; return r; }
    STDMETHODIMP ActivateCompleted(IActivateAudioInterfaceAsyncOperation*) override { SetEvent(ev); return S_OK; }
};
static IAudioClient* ActivateProcessLoopback(DWORD pid) {
    typedef HRESULT (WINAPI* PFN_AAIA)(LPCWSTR, REFIID, PROPVARIANT*, IActivateAudioInterfaceCompletionHandler*, IActivateAudioInterfaceAsyncOperation**);
    static PFN_AAIA f = nullptr; static bool tried = false;
    if (!tried) { tried = true; HMODULE m = LoadLibraryW(L"Mmdevapi.dll"); if (m) f = (PFN_AAIA)(void*)GetProcAddress(m, "ActivateAudioInterfaceAsync"); }
    if (!f) return nullptr;
    KnobLoopParams prm = {1 /*process loopback*/, pid, 0 /*include the process tree*/};
    PROPVARIANT pv; PropVariantInit(&pv);
    pv.vt = VT_BLOB; pv.blob.cbSize = sizeof(prm); pv.blob.pBlobData = (BYTE*)&prm;
    ActHandler* hnd = new ActHandler;
    IActivateAudioInterfaceAsyncOperation* op = nullptr;
    IAudioClient* ac = nullptr;
    if (SUCCEEDED(f(L"VAD\\Process_Loopback", __uuidof(IAudioClient), &pv, hnd, &op)) && op) {
        if (WaitForSingleObject(hnd->ev, 5000) == WAIT_OBJECT_0) {
            HRESULT hr = E_FAIL; IUnknown* u = nullptr;
            if (SUCCEEDED(op->GetActivateResult(&hr, &u)) && SUCCEEDED(hr) && u) { u->QueryInterface(__uuidof(IAudioClient), (void**)&ac); u->Release(); }
        }
        op->Release();
    }
    hnd->Release();
    return ac;
}

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
        if (ctx->pid) {   // one app's sound only; Windows asks us to pick the format
            if (!(cap = ActivateProcessLoopback(ctx->pid))) break;
            wf = (WAVEFORMATEX*)CoTaskMemAlloc(sizeof(WAVEFORMATEX));
            if (!wf) break;
            *wf = {}; wf->wFormatTag = WAVE_FORMAT_IEEE_FLOAT; wf->nChannels = 2; wf->nSamplesPerSec = 48000; wf->wBitsPerSample = 32;
            wf->nBlockAlign = 8; wf->nAvgBytesPerSec = 48000 * 8;
            if (FAILED(cap->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                       2000000, 0, wf, nullptr))) break;
        } else {
            if (FAILED(en->GetDevice(ctx->src.c_str(), &sdev))) break;
            if (FAILED(sdev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&cap))) break;
            if (FAILED(cap->GetMixFormat(&wf))) break;
            if (FAILED(cap->Initialize(AUDCLNT_SHAREMODE_SHARED, ctx->mic ? 0 : AUDCLNT_STREAMFLAGS_LOOPBACK, 2000000, 0, wf, nullptr))) break;   // a mic is recorded directly
        }
        if (FAILED(cap->GetService(__uuidof(IAudioCaptureClient), (void**)&cc))) break;
        const UINT32 rate = wf->nSamplesPerSec, fb = wf->nBlockAlign;
        const UINT32 preFrames = rate * 40 / 1000, hiFrames = rate * 120 / 1000;
        for (int ti = 0; ti < (int)ctx->targets.size(); ti++) {
            auto& t = ctx->targets[ti];
            IMMDevice* d = nullptr;
            if (FAILED(en->GetDevice(t.id.c_str(), &d))) continue;
            Out o; o.ti = ti;
            o.delayFrames = (UINT32)((UINT64)rate * std::max(0, std::min(2000, t.delayMs)) / 1000);
            o.q.reserve((size_t)(o.delayFrames + rate) * fb);   // room for a second of audio, so it rarely has to grow
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
                outs.push_back(std::move(o));
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
static std::vector<RouterCtx*> g_appRouters;
static int DelayFor(const std::wstring& id) {   // how far a speaker is held back to match the main output (0 for the main one)
    for (auto& o : g_out) if (o.id == id && o.isDefault) return 0;
    auto it = g_delay.find(id); return it == g_delay.end() ? 0 : it->second;
}
static void RestartAppRouters() {
    if (g_demo) return;
    for (auto* r : g_appRouters) InterlockedExchange(&r->stop, 1);
    for (auto* r : g_appRouters) { if (r->thread) { WaitForSingleObject(r->thread, 3000); CloseHandle(r->thread); } delete r; }
    g_appRouters.clear();
    if (!ProcLoopbackOk()) return;
    int di = DefaultOut();
    for (auto& kv : g_appExtra) {
        AppExtra& x = kv.second;
        if (!x.pid || x.ids.empty()) continue;
        std::wstring prim = !x.primary.empty() ? x.primary : (di >= 0 ? g_out[di].id : L"");
        for (auto& id : x.ids) if (DelayFor(id) < DelayFor(prim)) {   // a slower speaker: it should be the one the app plays on directly
            AppGroup tmp; tmp.pids = {x.pid};
            if (AppOutputSet(tmp, id)) { std::replace(x.ids.begin(), x.ids.end(), id, prim); prim = id; x.primary = id; }
        }
        int pd = DelayFor(prim);
        auto* r = new RouterCtx;
        r->pid = x.pid;
        for (auto& id : x.ids) if (id != prim) r->targets.push_back({id, std::max(0, DelayFor(id) - pd)});   // line up with the app's own speaker
        if (r->targets.empty()) { delete r; continue; }
        r->thread = CreateThread(nullptr, 0, RouterProc, r, 0, nullptr);
        if (r->thread) SetThreadPriority(r->thread, THREAD_PRIORITY_TIME_CRITICAL);
        g_appRouters.push_back(r);
    }
}
static void RestartMirror();
// ---- a microphone played on speakers (Settings › Sound › Inputs): what it hears comes out there, lined up like everything else
static std::map<std::wstring, std::vector<std::wstring>> g_micRoutes;   // mic id -> speakers
static std::vector<RouterCtx*> g_micRouters;
static void StopMicRouters() {
    for (auto* r : g_micRouters) InterlockedExchange(&r->stop, 1);
    for (auto* r : g_micRouters) { if (r->thread) { WaitForSingleObject(r->thread, 3000); CloseHandle(r->thread); } delete r; }
    g_micRouters.clear();
}
static void RestartMicRouters() {
    StopMicRouters();
    if (g_demo) return;
    for (auto& kv : g_micRoutes) {
        if (kv.second.empty()) continue;
        int mn = INT_MAX; for (auto& id : kv.second) mn = std::min(mn, DelayFor(id));
        auto* r = new RouterCtx; r->src = kv.first; r->mic = true;
        for (auto& id : kv.second) r->targets.push_back({id, DelayFor(id) - mn});   // the slowest of its speakers waits nothing
        r->thread = CreateThread(nullptr, 0, RouterProc, r, 0, nullptr);
        if (r->thread) SetThreadPriority(r->thread, THREAD_PRIORITY_TIME_CRITICAL);
        g_micRouters.push_back(r);
    }
}
static void RestartRouter() { RestartMirror(); RestartAppRouters(); RestartMicRouters(); }
static std::vector<std::wstring> AppRoutes(const AppGroup& a) {
    std::vector<std::wstring> r;
    auto it = g_appExtra.find(a.key);
    if (!a.outId.empty()) r.push_back(a.outId);
    else if (it != g_appExtra.end() && !it->second.ids.empty()) { int di = DefaultOut(); if (di >= 0) r.push_back(g_out[di].id); }   // still on the main speaker, plus copies
    if (it != g_appExtra.end()) for (auto& id : it->second.ids) if (std::find(r.begin(), r.end(), id) == r.end()) r.push_back(id);
    return r;
}
static void AppRouteAdd(AppGroup& a, const std::wstring& id) {   // also play this app on that speaker
    if (a.system || a.pids.empty()) return;
    int di = DefaultOut();
    std::wstring here = !a.outId.empty() ? a.outId : (di >= 0 ? g_out[di].id : L"");
    if (!ProcLoopbackOk()) { if (AppOutputSet(a, id)) a.outId = id; g_appExtra.erase(a.key); }
    else if (a.outId.empty() && id != here) {   // leave the app playing where it is (moving it makes browsers drop their sound) and add a copy
        AppExtra& x = g_appExtra[a.key];
        x.pid = a.pids[0]; x.primary.clear();
        if (std::find(x.ids.begin(), x.ids.end(), id) == x.ids.end()) x.ids.push_back(id);
    }
    else if (!a.outId.empty() && a.outId != id) {   // Windows 11: a copy of just this app's sound
        AppExtra& x = g_appExtra[a.key];
        x.pid = a.pids[0]; x.primary = a.outId;
        if (std::find(x.ids.begin(), x.ids.end(), id) == x.ids.end()) x.ids.push_back(id);
    }
    RestartAppRouters();
}
static void AppRouteRemove(AppGroup& a, const std::wstring& id) {
    auto it = g_appExtra.find(a.key);
    int di = DefaultOut();
    if (a.outId.empty() && it != g_appExtra.end() && di >= 0 && id == g_out[di].id) {   // off the main speaker: it has to move to one of its copies
        std::wstring next = it->second.ids.empty() ? L"" : it->second.ids[0];
        if (!next.empty()) it->second.ids.erase(it->second.ids.begin());
        if (AppOutputSet(a, next)) a.outId = next;
        it->second.primary = a.outId;
    } else if (id == a.outId) {   // its own speaker goes: the first copy takes over, or it plays with everything else again
        std::wstring next;
        if (it != g_appExtra.end() && !it->second.ids.empty()) { next = it->second.ids[0]; it->second.ids.erase(it->second.ids.begin()); }
        if (AppOutputSet(a, next)) a.outId = next;
        if (it != g_appExtra.end()) it->second.primary = a.outId;
    } else if (it != g_appExtra.end()) it->second.ids.erase(std::remove(it->second.ids.begin(), it->second.ids.end(), id), it->second.ids.end());
    if (it != g_appExtra.end() && it->second.ids.empty()) g_appExtra.erase(it);
    RestartAppRouters();
}
static void AppRouteClear(AppGroup& a) { if (AppOutputSet(a, L"")) a.outId.clear(); g_appExtra.erase(a.key); RestartAppRouters(); }
static std::vector<std::wstring> UsedSpeakers() {   // every speaker something plays on through Knob
    std::vector<std::wstring> r;
    for (int i = 0; i < (int)g_out.size(); i++) if (OutSelected(i)) r.push_back(g_out[i].id);
    for (auto& kv : g_appExtra) { auto ids = kv.second.ids; ids.push_back(kv.second.primary); for (auto& id : ids) if (!id.empty() && std::find(r.begin(), r.end(), id) == r.end()) r.push_back(id); }
    for (auto& a : g_apps) if (!a.system) for (auto& id : AppRoutes(a)) if (std::find(r.begin(), r.end(), id) == r.end()) r.push_back(id);   // same speakers the routing board draws
    for (auto& kv : g_micRoutes) for (auto& id : kv.second) if (std::find(r.begin(), r.end(), id) == r.end()) r.push_back(id);
    return r;
}
static int SpeakersInUse() { return (int)UsedSpeakers().size(); }
static bool OutPlays(int i) { auto u = UsedSpeakers(); return std::find(u.begin(), u.end(), g_out[i].id) != u.end(); }   // anything plays here: the main speaker, a copy, or an app sent to it
static int MinDelay() { int m = 0; bool first = true; for (auto& id : UsedSpeakers()) { int d = DelayFor(id); if (first || d < m) m = d; first = false; } return m; }
static int ShownDelay(const std::wstring& id) { return DelayFor(id) - MinDelay(); }   // the slowest speaker shows 0 ms
static void UpdateTrayTip();
static void OneSpeaker() {   // back to a single speaker: every app and Windows sound on the default speaker, no copies
    AppOutputResetAll();
    for (auto& a : g_apps) a.outId.clear();
    g_appExtra.clear();
    g_mirror.clear();
    g_micRoutes.clear();
    SaveSettings();
    RestartRouter();
    UpdateTrayTip();
}
static void RestartMirror() {
    if (g_demo) return;
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
struct CalResult { std::vector<std::pair<std::wstring, int>> delays; std::vector<std::wstring> missed; std::wstring status, swapTo, swapName; };
static std::set<std::wstring> g_calMissed;   // speakers the last Auto sync couldn't hear
static int g_calPass = 0;
static bool g_calRefineRun = false, g_calChained = false;   // the extra pass after a sync: measures once more with the new delays and trims what is left
static bool g_calApps = false;   // this run also measures speakers only apps play on: no main-speaker switch, delays may go below 0   // 1 = second run after Knob made the slowest speaker the main output
static std::wstring g_calStatus;
static bool g_calShow = false;   // the panel's Auto sync card: ask, then measuring, then the result
static bool g_calAsk = false;    // showing "Pause music and keep the room quiet" before it starts
static volatile LONG g_calCancel = 0;
static int g_calK = 0;           // which speaker is chirping now
static std::vector<std::wstring> g_calIds, g_calNames;
static std::map<std::wstring, int> g_calHeard;   // speaker -> what the microphone measured (ms), this run
static std::map<std::wstring, int> g_delayMeasured;   // what Auto sync set, so a tweak can be reset
static const UINT32 CAL_RATE = 48000;

static const GUID KNOB_IID_IAudioClient2 = {0x726778cd, 0xf60a, 0x4eda, {0x82, 0xde, 0xe4, 0x76, 0x10, 0xcd, 0x78, 0xaa}};
static std::vector<float> CalChirp(bool quad = false) {   // 50 ms sweep 1 -> 6 kHz, Hann window; quad = the same sweep shifted 90 degrees (for the envelope)
    const int n = CAL_RATE / 20;
    std::vector<float> c(n);
    const double f0 = 1000, f1 = 6000, T = n / (double)CAL_RATE;
    for (int i = 0; i < n; i++) {
        double t = i / (double)CAL_RATE;
        double ph = 2 * 3.14159265358979 * (f0 * t + (f1 - f0) * t * t / (2 * T));
        double w = 0.5 - 0.5 * cos(2 * 3.14159265358979 * i / (n - 1));
        c[i] = (float)((quad ? cos(ph) : sin(ph)) * w);
    }
    return c;
}
static void CalFormat(WAVEFORMATEX& f) {
    f = {}; f.wFormatTag = WAVE_FORMAT_IEEE_FLOAT; f.nChannels = 1; f.nSamplesPerSec = CAL_RATE;
    f.wBitsPerSample = 32; f.nBlockAlign = 4; f.nAvgBytesPerSec = CAL_RATE * 4;
}
// one round: returns copy-minus-main offset in ms; flags: 0 ok, 1 copy not heard, 2 nothing heard, 3 device error
static int CalAnalyze(const std::vector<float>& rec, const std::vector<float>& chirp, const std::vector<float>& chirpQ, UINT32 gap, double& offsetMs);
static int CalRound(IMMDevice* outDev, IMMDevice* micDev, LONG target, const std::vector<float>& chirp, const std::vector<float>& chirpQ, double& offsetMs) {
    WAVEFORMATEX f; CalFormat(f);
    IAudioClient *rac = nullptr, *cac = nullptr; IAudioRenderClient* rc = nullptr; IAudioCaptureClient* cc = nullptr;
    std::vector<float> rec;
    int result = 3;
    const UINT32 gap = CAL_RATE * 8 / 10, lead = CAL_RATE / 10, total = lead + gap + CAL_RATE * 9 / 10;   // the copy may land up to ~2 s later
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
        while (GetTickCount() - t0 < 3800) {
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
    return CalAnalyze(rec, chirp, chirpQ, gap, offsetMs);
}

// Envelope of the matched filter (in-phase and 90-degree versions) around a coarse peak, with the top found to a fraction of a sample.
// It ignores the carrier's phase, which each speaker bends differently, so the main and the copy are timed the same way.
static void CalRefine(const std::vector<float>& rec, const std::vector<float>& c, const std::vector<float>& q, size_t at, double& pos) {
    const size_t m = c.size();
    int lo = std::max(0, (int)at - 48), hi = (int)at + 48;
    if (rec.size() < m + 1) return;
    hi = std::min(hi, (int)(rec.size() - m));
    if (hi - lo < 4) return;
    std::vector<double> env(hi - lo + 1);
    for (int i = lo; i <= hi; i++) {
        double a = 0, b = 0; const float* r = &rec[i];
        for (size_t j = 0; j < m; j++) { a += (double)r[j] * c[j]; b += (double)r[j] * q[j]; }
        env[i - lo] = sqrt(a * a + b * b);
    }
    int bi = 0; for (int k = 1; k < (int)env.size(); k++) if (env[k] > env[bi]) bi = k;
    double frac = 0;
    if (bi > 0 && bi + 1 < (int)env.size()) { double y0 = env[bi - 1], y1 = env[bi], y2 = env[bi + 1], d = y0 - 2 * y1 + y2; if (d < 0) frac = std::max(-1.0, std::min(1.0, 0.5 * (y0 - y2) / d)); }
    pos = lo + bi + frac;
}
static int CalAnalyze(const std::vector<float>& rec, const std::vector<float>& chirp, const std::vector<float>& chirpQ, UINT32 gap, double& offsetMs) {   // what the microphone recorded -> copy minus main, in ms
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
    double mainPos = (double)mainAt, copyPos = (double)copyAt;   // then sharpen both to a fraction of a sample on the matched filter's envelope
    CalRefine(rec, chirp, chirpQ, mainAt, mainPos); CalRefine(rec, chirp, chirpQ, copyAt, copyPos);
    offsetMs = (copyPos - mainPos) * 1000.0 / CAL_RATE;
    return 0;
}
static DWORD WINAPI CalibrateProc(LPVOID p) {
    std::vector<CalTarget>* ts = (std::vector<CalTarget>*)p;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    CalResult* res = new CalResult;
    IMMDeviceEnumerator* en = nullptr; IMMDevice *outDev = nullptr, *micDev = nullptr;
    std::vector<float> chirp = CalChirp(), chirpQ = CalChirp(true);
    std::wstring notes;
    do {
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&en))) { res->status = L"Couldn't open audio devices"; break; }
        if (FAILED(en->GetDefaultAudioEndpoint(eRender, eConsole, &outDev))) { res->status = L"No main output"; break; }
        {   // the laptop's built-in mic hears the room best, and never puts Bluetooth buds into call mode
            auto builtIn = [](IMMDevice* d) {   // not Bluetooth, not USB
                IPropertyStore* ps = nullptr; std::wstring hw;
                if (SUCCEEDED(d->OpenPropertyStore(STGM_READ, &ps)) && ps) { hw = ToLower(PropStr(ps, PK_HwInstance)); ps->Release(); }
                hw += L"|" + ToLower(ConnectedPath(d));
                return hw.find(L"bth") == std::wstring::npos && hw.find(L"usb") == std::wstring::npos;
            };
            IMMDevice* def = nullptr;
            if (SUCCEEDED(en->GetDefaultAudioEndpoint(eCapture, eConsole, &def)) && def) { if (builtIn(def)) micDev = def; else def->Release(); }
            IMMDeviceCollection* mc = nullptr;
            if (!micDev && SUCCEEDED(en->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &mc)) && mc) {
                UINT mn = 0; mc->GetCount(&mn);
                for (UINT i = 0; i < mn && !micDev; i++) { IMMDevice* d = nullptr; if (SUCCEEDED(mc->Item(i, &d)) && d) { if (builtIn(d)) micDev = d; else d->Release(); } }
                mc->Release();
            }
        }
        if (!micDev && FAILED(en->GetDefaultAudioEndpoint(eCapture, eConsole, &micDev))) { res->status = L"No microphone found — auto sync needs one"; break; }
        int ok = 0;
        int worst = 0; std::wstring worstId, worstName;   // most negative delay = slowest speaker
        for (int k = 0; k < (int)ts->size(); k++) {
            if (g_calCancel) { res->status = L"Stopped"; res->delays.clear(); ok = 0; break; }
            if (g_hwnd) PostMessageW(g_hwnd, WM_APP + 33, k, 0);   // the panel shows which speaker is chirping
            CalTarget& t = (*ts)[k];
            double off = 0; int r = CalRound(outDev, micDev, k, chirp, chirpQ, off);
            if (r == 0) {
                // Speakers jitter by a few ms from round to round (Bluetooth most of all): take two rounds and use their average when they
                // agree within 5 ms, otherwise a third and the middle one. The check pass after a sync takes a single round.
                std::vector<double> got{off};
                for (int again = 1; again < (g_calRefineRun ? 1 : 3) && !g_calCancel; again++) {
                    double o2 = 0; if (CalRound(outDev, micDev, k, chirp, chirpQ, o2) != 0) continue;
                    got.push_back(o2);
                    if (got.size() == 2 && fabs(got[0] - got[1]) <= 5) break;
                }
                std::sort(got.begin(), got.end());
                off = got.size() == 1 ? got[0] : got.size() == 2 ? (got[0] + got[1]) / 2 : got[1];
                int nd = (int)lround(t.delayMs - off);   // to the millisecond
                if (g_calRefineRun) { if (abs(nd - t.delayMs) > 40) nd = t.delayMs; }   // the check pass only trims; a big jump means this reading was off
                if (g_hwnd && !g_calRefineRun) PostMessageW(g_hwnd, WM_APP + 34, k, (LPARAM)(INT_PTR)lround(fabs(off)));   // the check pass leaves the list showing the first measurement
                if (nd < worst) { worst = nd; worstId = t.id; worstName = t.name; }
                if (nd < -10 && g_calPass == 1) notes += t.name + L" couldn't be lined up fully. ";
                nd = g_calApps ? std::max(-2000, std::min(2000, nd)) : std::max(0, std::min(2000, nd));   // app copies: keep "faster than main" too
                res->delays.push_back({t.id, nd});
                ok++;
            } else if (g_calRefineRun) { continue; }   // the check pass: keep what the first pass set
            else if (r == 2) { res->status = L"The microphone couldn't hear the main output — turn the volume up"; break; }
            else if (r == 3) { notes += L"Couldn't use " + t.name + L". "; res->missed.push_back(t.id); }
            else { notes += L"Couldn't hear " + t.name + L" \u2014 turn it up and try again. "; res->missed.push_back(t.id); }
        }
        if (res->status == L"Stopped") break;
        if (res->status.empty() && worst < -10 && !g_calPass && !g_calApps) {   // let Knob make the slowest speaker the main output, then sync again
            res->swapTo = worstId; res->swapName = worstName; res->delays.clear();
            res->status = L"Syncing\u2026 keep the room quiet";   // the slowest speaker quietly becomes the one Windows plays on, then a second pass
        }
        if (res->status.empty()) res->status = ok ? L"In sync \u00b7 " + std::to_wstring(ok + 1) + L" speakers lined up. " + notes : notes;
    } while (0);
    if (micDev) micDev->Release();
    if (outDev) outDev->Release();
    if (en) en->Release();
    CoUninitialize();
    delete ts;
    if (!g_hwnd || !PostMessageW(g_hwnd, WM_APP_CALDONE, 0, (LPARAM)res)) delete res;
    return 0;
}
// every other app is muted while Auto sync measures, so music doesn't drown out the test sounds; put back exactly as it was afterwards
static std::vector<std::pair<ISimpleAudioVolume*, BOOL>> g_calMuted;
static void CalMuteOthers(bool on) {
    if (!on) { for (auto& m : g_calMuted) { m.first->SetMute(m.second, nullptr); m.first->Release(); } g_calMuted.clear(); return; }
    if (!g_calMuted.empty() || g_demo || !g_enum) return;   // already muted (second pass)
    DWORD self = GetCurrentProcessId();
    IMMDeviceCollection* c = nullptr;
    if (FAILED(g_enum->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &c))) return;
    UINT n = 0; c->GetCount(&n);
    for (UINT i = 0; i < n; i++) {
        IMMDevice* dev = nullptr; IAudioSessionManager2* mgr = nullptr; IAudioSessionEnumerator* se = nullptr;
        if (FAILED(c->Item(i, &dev))) continue;
        if (SUCCEEDED(dev->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, (void**)&mgr)) && mgr && SUCCEEDED(mgr->GetSessionEnumerator(&se)) && se) {
            int cnt = 0; se->GetCount(&cnt);
            for (int j = 0; j < cnt; j++) {
                IAudioSessionControl* sc = nullptr; IAudioSessionControl2* sc2 = nullptr; ISimpleAudioVolume* sv = nullptr;
                if (FAILED(se->GetSession(j, &sc)) || !sc) continue;
                DWORD pid = 0;
                if (SUCCEEDED(sc->QueryInterface(__uuidof(IAudioSessionControl2), (void**)&sc2)) && sc2) { sc2->GetProcessId(&pid); sc2->Release(); }
                if (pid != self && SUCCEEDED(sc->QueryInterface(__uuidof(ISimpleAudioVolume), (void**)&sv)) && sv) {   // Knob's own test sound stays
                    BOOL m = FALSE; sv->GetMute(&m); sv->SetMute(TRUE, nullptr); g_calMuted.push_back({sv, m});
                }
                sc->Release();
            }
            se->Release();
        }
        if (mgr) mgr->Release();
        dev->Release();
    }
    c->Release();
}
static void StartAutoSync() {
    if (g_calibrating) return;
    if (!g_calChained) { g_calPass = 0; g_calRefineRun = false; }   // a fresh run; only the follow-up passes Knob starts itself keep their state
    g_calChained = false;
    int di = DefaultOut();
    std::vector<RouterTarget> all;
    if (g_router) all = g_router->targets;
    std::vector<std::wstring> appIds;   // speakers apps are copied to, and each app's own speaker
    for (auto& kv : g_appExtra) { appIds.insert(appIds.end(), kv.second.ids.begin(), kv.second.ids.end()); appIds.push_back(kv.second.primary); }
    for (auto& a : g_apps) if (!a.system && g_appExtra.count(a.key)) for (auto& id : AppRoutes(a)) appIds.push_back(id);
    for (auto& kv : g_micRoutes) appIds.insert(appIds.end(), kv.second.begin(), kv.second.end());   // speakers a mic plays on
    {
        for (auto& id : appIds) {
            if (id.empty() || (di >= 0 && g_out[di].id == id)) continue;
            bool have = false; for (auto& t : all) if (t.id == id) have = true;
            if (!have) { auto it = g_delay.find(id); all.push_back({id, it == g_delay.end() ? 0 : std::max(0, it->second)}); }
        }
    }
    if (all.empty() || di < 0) { g_calStatus = L"Connect an app to two or more speakers first"; return; }
    g_calApps = !g_router || all.size() > g_router->targets.size();
    if (g_calApps) {   // a short-lived copy of the main speaker's sound to all of them, just for measuring
        StopRouter();
        g_router = new RouterCtx;
        g_router->src = g_out[di].id;
        g_router->targets = all;
        g_router->thread = CreateThread(nullptr, 0, RouterProc, g_router, 0, nullptr);
        if (g_router->thread) SetThreadPriority(g_router->thread, THREAD_PRIORITY_TIME_CRITICAL);
        Sleep(300);
    }
    auto* ts = new std::vector<CalTarget>;
    for (auto& t : all) {
        std::wstring name = t.id;
        for (auto& e : g_out) if (e.id == t.id) name = e.name;
        ts->push_back({t.id, name, t.delayMs});
    }
    InterlockedExchange(&g_calibrating, 1);
    InterlockedExchange(&g_calCancel, 0);
    g_calStatus = L"Listening… other apps are muted until it's done";
    g_calIds.clear(); g_calNames.clear(); g_calK = 0; if (!g_calPass) g_calHeard.clear();
    for (auto& t : *ts) { g_calIds.push_back(t.id); g_calNames.push_back(t.name); }
    CalMuteOthers(true);
    HANDLE h = CreateThread(nullptr, 0, CalibrateProc, ts, 0, nullptr);
    if (h) CloseHandle(h); else { delete ts; InterlockedExchange(&g_calibrating, 0); g_calStatus = L"Couldn't start"; CalMuteOthers(false); }
}

// ---------------------------------------------------------------- display worker (DDC/CI + WMI + orientation)
struct Mon {
    std::wstring name, detail, gdi; HANDLE phys = nullptr; bool wmi = false, internal = false, ok = false, hasCon = false;
    int cloneOf = -1; LUID adapter{}; UINT32 target = 0; bool hdrOk = false, hdr = false;
    DWORD mn = 0, mx = 100, cur = 50, cmn = 0, cmx = 100, ccur = 50; int orient = 0; RECT rc{};
    bool vcpB = false, vcpC = false;   // fall back to raw VCP codes 0x10 / 0x12 when the high-level API fails
    std::vector<int> inputs; int input = -1;   // VCP 0x60 input sources the monitor lists
};
static std::map<std::wstring, std::vector<int>> w_inputCache;   // capability strings are slow: ask each monitor once
static std::vector<int> ReadInputs(HANDLE phys) {
    std::vector<int> v;
    DWORD len = 0;
    if (!GetCapabilitiesStringLength(phys, &len) || !len || len > 16384) return v;
    std::vector<char> buf(len + 1);
    if (!CapabilitiesRequestAndCapabilitiesReply(phys, buf.data(), len)) return v;
    std::string caps(buf.data());
    for (auto& c : caps) c = (char)toupper((unsigned char)c);
    size_t vcp = caps.find("VCP(");
    size_t p = caps.find("60(", vcp == std::string::npos ? 0 : vcp);
    while (p != std::string::npos && p > 0 && isxdigit((unsigned char)caps[p - 1])) p = caps.find("60(", p + 1);   // a real code, not part of 160(
    if (p == std::string::npos) return v;
    size_t e = caps.find(')', p);
    std::string list = caps.substr(p + 3, e == std::string::npos ? std::string::npos : e - p - 3);
    size_t i = 0;
    while (i < list.size()) {
        while (i < list.size() && !isxdigit((unsigned char)list[i])) i++;
        size_t j = i;
        while (j < list.size() && isxdigit((unsigned char)list[j])) j++;
        if (j > i) v.push_back((int)strtol(list.substr(i, j - i).c_str(), nullptr, 16) & 0xFF);
        i = j;
    }
    return v;
}
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

struct DcName { std::wstring friendly, tech; bool internal; LUID adapter; UINT32 target; bool hdrOk, hdr; };
static std::map<std::wstring, std::vector<DcName>> QueryDisplayNames() {   // GDI source -> its screens (more than one when duplicated)
    std::map<std::wstring, std::vector<DcName>> r;
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
        DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO ac = {};   // HDR: can this screen do it, is it on
        ac.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO; ac.header.size = sizeof(ac);
        ac.header.adapterId = paths[i].targetInfo.adapterId; ac.header.id = paths[i].targetInfo.id;
        bool hdrOk = false, hdr = false;
        if (DisplayConfigGetDeviceInfo(&ac.header) == ERROR_SUCCESS) { hdrOk = ac.advancedColorSupported && !ac.advancedColorForceDisabled && !ac.wideColorEnforced; hdr = hdrOk && ac.advancedColorEnabled; }
        r[src.viewGdiDeviceName].push_back({tgt.monitorFriendlyDeviceName, tech, internal, paths[i].targetInfo.adapterId, paths[i].targetInfo.id, hdrOk, hdr});
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
        int orient = 0;
        { DEVMODEW dm = {}; dm.dmSize = sizeof(dm); if (EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm)) orient = (int)dm.dmDisplayOrientation; }
        auto it = names.find(mi.szDevice);
        std::vector<DcName> nm; if (it != names.end()) nm = it->second;
        auto readPhys = [&](Mon& m) {   // brightness, contrast and inputs of one physical screen
            if (m.internal || !m.phys) return;
            DWORD a = 0, b = 0, c = 0;
            bool ok = GetMonitorBrightness(m.phys, &a, &b, &c);
            if (!ok) { Sleep(60); ok = GetMonitorBrightness(m.phys, &a, &b, &c); }
            if (ok && c > a) { m.mn = a; m.cur = b; m.mx = c; m.ok = true; }
            if (!m.ok) {   // many monitors only answer raw VCP requests (what PowerToys uses)
                DWORD cur = 0, mx = 0;
                if (GetVCPFeatureAndVCPFeatureReply(m.phys, 0x10, nullptr, &cur, &mx) && mx > 0) { m.mn = 0; m.cur = cur; m.mx = mx; m.ok = true; m.vcpB = true; }
            }
            {   // input sources (HDMI / DisplayPort / USB-C), asked once per monitor
                auto ic = w_inputCache.find(m.name + m.gdi);
                if (ic == w_inputCache.end()) ic = w_inputCache.emplace(m.name + m.gdi, ReadInputs(m.phys)).first;
                m.inputs = ic->second;
                if (m.inputs.size() >= 2) { DWORD cur = 0, mx = 0; if (GetVCPFeatureAndVCPFeatureReply(m.phys, 0x60, nullptr, &cur, &mx)) m.input = (int)(cur & 0xFF); }
                else m.inputs.clear();
            }
            DWORD ca = 0, cb = 0, ccx = 0;
            if (GetMonitorContrast(m.phys, &ca, &cb, &ccx) && ccx > ca) { m.cmn = ca; m.ccur = cb; m.cmx = ccx; m.hasCon = true; }
            else {
                Sleep(40);
                DWORD cur = 0, mx = 0;
                if (GetVCPFeatureAndVCPFeatureReply(m.phys, 0x12, nullptr, &cur, &mx) && mx > 0) { m.cmn = 0; m.ccur = cur; m.cmx = mx; m.hasCon = true; m.vcpC = true; }
            }
        };
        auto makeMon = [&](size_t k) {   // screen k of this source (k > 0: it shows the same picture as screen 0)
            Mon m;
            m.gdi = mi.szDevice; m.rc = mi.rcMonitor; m.orient = orient;
            if (k < nm.size()) { m.name = nm[k].friendly; m.internal = nm[k].internal; m.detail = nm[k].tech; m.adapter = nm[k].adapter; m.target = nm[k].target; m.hdrOk = nm[k].hdrOk; m.hdr = nm[k].hdr; }
            return m;
        };
        size_t first = w_mons.size();
        DWORD np = 0;
        std::vector<PHYSICAL_MONITOR> pm;
        if (GetNumberOfPhysicalMonitorsFromHMONITOR(h, &np) && np) { pm.resize(np); if (!GetPhysicalMonitorsFromHMONITOR(h, np, pm.data())) pm.clear(); }
        size_t count = std::max<size_t>(1, std::max(pm.size(), nm.size()));
        for (size_t k = 0; k < count; k++) {
            Mon m = makeMon(k);
            if (k < pm.size()) {
                m.phys = pm[k].hPhysicalMonitor;
                if (m.name.empty() && !m.internal) m.name = pm[k].szPhysicalMonitorDescription;
                readPhys(m);
            }
            if (m.name.empty()) m.name = m.internal ? L"Built-in display" : L"Display";
            if (k > 0) m.cloneOf = (int)first;
            w_mons.push_back(m);
        }
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
        u.inputs = m.inputs; u.input = m.input;
        u.cloneOf = m.cloneOf; u.adapter = m.adapter; u.target = m.target; u.hdrOk = m.hdrOk; u.hdr = m.hdr;
        {   // refresh rates offered at the current resolution
            DEVMODEW cur = {}; cur.dmSize = sizeof(cur);
            if (EnumDisplaySettingsW(m.gdi.c_str(), ENUM_CURRENT_SETTINGS, &cur)) {
                u.rate = (int)cur.dmDisplayFrequency;
                std::set<int> rs;
                DEVMODEW dm = {}; dm.dmSize = sizeof(dm);
                for (DWORD i = 0; EnumDisplaySettingsW(m.gdi.c_str(), i, &dm); i++)
                    if (dm.dmPelsWidth == cur.dmPelsWidth && dm.dmPelsHeight == cur.dmPelsHeight && dm.dmBitsPerPel == cur.dmBitsPerPel && dm.dmDisplayFrequency > 1)
                        rs.insert((int)dm.dmDisplayFrequency);
                u.rates.assign(rs.begin(), rs.end());
            }
        }
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
                int idx = kv.first / 4, type = kv.first % 4;
                if (idx < 0 || idx >= (int)w_mons.size()) continue;
                auto& m = w_mons[idx];
                if (type == 2) { if (m.phys && SetVCPFeature(m.phys, 0x60, (DWORD)kv.second)) m.input = kv.second; continue; }   // switch input
                if (type == 3) { if (m.phys) SetVCPFeature(m.phys, 0xD6, (DWORD)kv.second); continue; }   // power: 1 on, 4 screen off (DDC/CI)
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
    g_bSets[idx * 4 + type] = pct;
    g_bSetGen = g_monGen;
    LeaveCriticalSection(&g_bcs);
    SetEvent(g_bEvent);
}

static void SetMonitorPower(int idx, bool on) {   // the monitor's own screen-off command; the PC and the other screens carry on
    if (idx < 0 || idx >= (int)g_mons.size()) return;
    EnterCriticalSection(&g_bcs); g_bSets[idx * 4 + 3] = on ? 1 : 4; g_bSetGen = g_monGen; LeaveCriticalSection(&g_bcs);
    SetEvent(g_bEvent);
}
static void SetMonitorInput(int idx, int code) {
    if (idx < 0 || idx >= (int)g_mons.size()) return;
    g_mons[idx].input = code;
    EnterCriticalSection(&g_bcs);
    g_bSets[idx * 4 + 2] = code;
    g_bSetGen = g_monGen;
    LeaveCriticalSection(&g_bcs);
    SetEvent(g_bEvent);
}
static void SetRefreshRate(int idx, int hz) {
    if (idx < 0 || idx >= (int)g_mons.size()) return;
    DEVMODEW dm = {}; dm.dmSize = sizeof(dm);
    if (!EnumDisplaySettingsW(g_mons[idx].gdi.c_str(), ENUM_CURRENT_SETTINGS, &dm)) return;
    dm.dmDisplayFrequency = (DWORD)hz;
    dm.dmFields = DM_DISPLAYFREQUENCY | DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL;
    if (ChangeDisplaySettingsExW(g_mons[idx].gdi.c_str(), &dm, nullptr, CDS_UPDATEREGISTRY, nullptr) == DISP_CHANGE_SUCCESSFUL)
        g_mons[idx].rate = hz;
}
static std::wstring InputName(int code, const std::vector<int>& all) {
    auto has = [&](int c) { return std::find(all.begin(), all.end(), c) != all.end(); };
    switch (code) {
    case 0x01: return L"VGA"; case 0x02: return L"VGA 2"; case 0x03: return L"DVI"; case 0x04: return L"DVI 2";
    case 0x0F: return has(0x10) ? L"DisplayPort 1" : L"DisplayPort"; case 0x10: return L"DisplayPort 2";
    case 0x11: return has(0x12) ? L"HDMI 1" : L"HDMI"; case 0x12: return L"HDMI 2"; case 0x13: return L"HDMI 3";
    case 0x1B: case 0x19: return L"USB-C";
    default: { wchar_t b[24]; swprintf(b, 24, L"Input %02X", code); return b; }
    }
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

// ---- HDR, Extend / Duplicate, night light and "dim further" (per screen)
static bool SetHdr(int idx, bool on) {
    if (idx < 0 || idx >= (int)g_mons.size() || !g_mons[idx].hdrOk) return false;
    DISPLAYCONFIG_SET_ADVANCED_COLOR_STATE st = {};
    st.header.type = DISPLAYCONFIG_DEVICE_INFO_SET_ADVANCED_COLOR_STATE; st.header.size = sizeof(st);
    st.header.adapterId = g_mons[idx].adapter; st.header.id = g_mons[idx].target;
    st.enableAdvancedColor = on ? 1 : 0;
    if (DisplayConfigSetDeviceInfo(&st.header) != ERROR_SUCCESS) return false;
    g_mons[idx].hdr = on;
    return true;
}
static bool KeepDisplayChange() {   // like Windows: keep the new layout, or go back by itself after 15 seconds
    typedef int (WINAPI* PFN_MBT)(HWND, LPCWSTR, LPCWSTR, UINT, WORD, DWORD);
    static auto mbt = (PFN_MBT)(void*)GetProcAddress(GetModuleHandleW(L"user32.dll"), "MessageBoxTimeoutW");
    const wchar_t* t = L"Keep these display settings?\n\nIf you don't answer, the previous settings come back in 15 seconds.";
    int r = mbt ? mbt(nullptr, t, APP_NAME, MB_YESNO | MB_ICONQUESTION | MB_TOPMOST | MB_SETFOREGROUND, 0, 15000)
                : MessageBoxW(nullptr, t, APP_NAME, MB_YESNO | MB_ICONQUESTION | MB_TOPMOST);
    return r == IDYES;
}
static std::wstring g_dispStatus;
static int g_connScreens = 0;   // screens plugged in, including ones switched off by "Only this screen"
struct OffScreen { std::wstring name; bool internal = false; };
static std::vector<OffScreen> g_offScreens;   // plugged in but switched off: listed with their name and a way to bring them back
static void CountScreens() {   // every screen plugged in, active or not
    UINT32 np = 0, nm = 0; g_connScreens = 0; g_offScreens.clear();
    if (GetDisplayConfigBufferSizes(QDC_ALL_PATHS, &np, &nm) != ERROR_SUCCESS) return;
    std::vector<DISPLAYCONFIG_PATH_INFO> p(np); std::vector<DISPLAYCONFIG_MODE_INFO> m(nm);
    if (QueryDisplayConfig(QDC_ALL_PATHS, &np, p.data(), &nm, m.data(), nullptr) != ERROR_SUCCESS) return;
    auto key = [](const DISPLAYCONFIG_PATH_INFO& x) { return ((unsigned long long)x.targetInfo.adapterId.LowPart << 32) ^ x.targetInfo.id ^ ((unsigned long long)x.targetInfo.adapterId.HighPart << 48); };
    std::set<unsigned long long> seen, active;
    for (UINT32 i = 0; i < np; i++) if (p[i].flags & DISPLAYCONFIG_PATH_ACTIVE) active.insert(key(p[i]));
    for (UINT32 i = 0; i < np; i++) {
        if (!p[i].targetInfo.targetAvailable) continue;
        unsigned long long k = key(p[i]);
        if (!seen.insert(k).second) continue;
        if (active.count(k)) continue;
        DISPLAYCONFIG_TARGET_DEVICE_NAME tgt = {};
        tgt.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME; tgt.header.size = sizeof(tgt);
        tgt.header.adapterId = p[i].targetInfo.adapterId; tgt.header.id = p[i].targetInfo.id;
        OffScreen o;
        if (DisplayConfigGetDeviceInfo(&tgt.header) == ERROR_SUCCESS) {
            auto t = tgt.outputTechnology;
            o.internal = t == DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INTERNAL || t == DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DISPLAYPORT_EMBEDDED ||
                         t == DISPLAYCONFIG_OUTPUT_TECHNOLOGY_UDI_EMBEDDED || t == DISPLAYCONFIG_OUTPUT_TECHNOLOGY_LVDS;
            o.name = tgt.monitorFriendlyDeviceName;
        }
        if (o.name.empty()) o.name = o.internal ? L"Built-in display" : L"Display";
        g_offScreens.push_back(o);
    }
    g_connScreens = (int)seen.size();
}   // shown under the Display tab when Windows refuses a layout
static bool ActivePathState(UINT32& n, bool& cloned) {   // how many screens are on, and whether two of them show the same picture
    UINT32 np = 0, nm = 0; n = 0; cloned = false;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &np, &nm) != ERROR_SUCCESS) return false;
    std::vector<DISPLAYCONFIG_PATH_INFO> p(np); std::vector<DISPLAYCONFIG_MODE_INFO> m(nm);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &np, p.data(), &nm, m.data(), nullptr) != ERROR_SUCCESS) return false;
    n = np;
    for (UINT32 i = 0; i < np; i++) for (UINT32 j = i + 1; j < np; j++)
        if (p[i].sourceInfo.adapterId.LowPart == p[j].sourceInfo.adapterId.LowPart && p[i].sourceInfo.adapterId.HighPart == p[j].sourceInfo.adapterId.HighPart && p[i].sourceInfo.id == p[j].sourceInfo.id) cloned = true;
    return true;
}
static void RunDisplaySwitch(const wchar_t* arg) {   // Windows' own Win+P switcher: the most reliable way to get Extend / Duplicate back
    wchar_t sys[MAX_PATH]; if (!GetSystemDirectoryW(sys, MAX_PATH)) return;
    std::wstring cmd = L"\"" + std::wstring(sys) + L"\\DisplaySwitch.exe\" " + arg;
    STARTUPINFOW si = {}; si.cb = sizeof(si); PROCESS_INFORMATION pi = {};
    if (CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) { WaitForSingleObject(pi.hProcess, 8000); CloseHandle(pi.hProcess); CloseHandle(pi.hThread); }
}
static void SetDisplayMode(int idx, int dupOf) {   // dupOf -1 = extend, else show the same picture as screen dupOf
    if (idx < 0 || idx >= (int)g_mons.size()) return;
    CountScreens();
    UINT32 np = 0, nm = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &np, &nm) != ERROR_SUCCESS) return;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(np), oldP; std::vector<DISPLAYCONFIG_MODE_INFO> modes(nm), oldM;
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &np, paths.data(), &nm, modes.data(), nullptr) != ERROR_SUCCESS) return;
    paths.resize(np); modes.resize(nm); oldP = paths; oldM = modes;
    LONG r;
    g_dispStatus.clear();
    auto findP = [&](const MonUI& m) { for (UINT32 i = 0; i < np; i++) if (paths[i].targetInfo.id == m.target && paths[i].targetInfo.adapterId.LowPart == m.adapter.LowPart && paths[i].targetInfo.adapterId.HighPart == m.adapter.HighPart) return (int)i; return -1; };
    if (dupOf == -1) r = SetDisplayConfig(0, nullptr, 0, nullptr, SDC_APPLY | SDC_TOPOLOGY_EXTEND | SDC_SAVE_TO_DATABASE);
    else if (dupOf == -2) {   // this screen only; the others switch off (Extend brings them back)
        int b = findP(g_mons[idx]);
        if (b < 0) return;
        DISPLAYCONFIG_PATH_INFO one = paths[b];
        r = SetDisplayConfig(1, &one, nm, modes.data(), SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_ALLOW_CHANGES | SDC_SAVE_TO_DATABASE);
        if (r != ERROR_SUCCESS) { one.sourceInfo.modeInfoIdx = DISPLAYCONFIG_PATH_MODE_IDX_INVALID; one.targetInfo.modeInfoIdx = DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
                                  r = SetDisplayConfig(1, &one, 0, nullptr, SDC_APPLY | SDC_TOPOLOGY_SUPPLIED | SDC_ALLOW_PATH_ORDER_CHANGES); }
        if (r != ERROR_SUCCESS) r = SetDisplayConfig(0, nullptr, 0, nullptr, SDC_APPLY | (g_mons[idx].internal ? SDC_TOPOLOGY_INTERNAL : SDC_TOPOLOGY_EXTERNAL));
    }
    else if (np == 2 || dupOf == -3) r = SetDisplayConfig(0, nullptr, 0, nullptr, SDC_APPLY | SDC_TOPOLOGY_CLONE | SDC_SAVE_TO_DATABASE);   // two screens: Windows' own Duplicate
    else {
        if (dupOf >= (int)g_mons.size()) return;
        auto find = [&](const MonUI& m) { for (UINT32 i = 0; i < np; i++) if (paths[i].targetInfo.id == m.target && paths[i].targetInfo.adapterId.LowPart == m.adapter.LowPart && paths[i].targetInfo.adapterId.HighPart == m.adapter.HighPart) return (int)i; return -1; };
        int a = find(g_mons[dupOf]), b = find(g_mons[idx]);
        if (a < 0 || b < 0) return;
        paths[b].sourceInfo = paths[a].sourceInfo;   // both screens show the same desktop
        paths[b].targetInfo.modeInfoIdx = DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
        r = SetDisplayConfig(np, paths.data(), nm, modes.data(), SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_ALLOW_CHANGES | SDC_SAVE_TO_DATABASE);
        if (r != ERROR_SUCCESS) {   // let Windows pick the modes for this topology
            for (auto& pth : paths) { pth.sourceInfo.modeInfoIdx = DISPLAYCONFIG_PATH_MODE_IDX_INVALID; pth.targetInfo.modeInfoIdx = DISPLAYCONFIG_PATH_MODE_IDX_INVALID; }
            r = SetDisplayConfig(np, paths.data(), 0, nullptr, SDC_APPLY | SDC_TOPOLOGY_SUPPLIED | SDC_ALLOW_PATH_ORDER_CHANGES);
        }
    }
    if (dupOf != -2) {   // Extend / Duplicate: check Windows really did it, and if not, ask Windows' own Win+P switcher
        auto done = [&]() { UINT32 n = 0; bool cl = false; if (!ActivePathState(n, cl)) return false; return dupOf == -1 ? (n >= 2 && !cl) : cl; };
        Sleep(400);
        if (r != ERROR_SUCCESS || !done()) {
            RunDisplaySwitch(dupOf == -1 ? L"/extend" : L"/clone"); Sleep(800);
            r = done() ? ERROR_SUCCESS : ERROR_GEN_FAILURE;
        }
    }
    if (r != ERROR_SUCCESS) { g_dispStatus = dupOf == -1 ? L"Windows couldn't extend the screens" : dupOf == -2 ? L"Windows couldn't switch the other screens off" : L"Your graphics card can't show these two screens the same picture"; return; }
    if (!KeepDisplayChange())
        SetDisplayConfig((UINT32)oldP.size(), oldP.data(), (UINT32)oldM.size(), oldM.data(), SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_ALLOW_CHANGES | SDC_SAVE_TO_DATABASE);
}

// night light: a warm tint through the screen's gamma ramp; strength 0-100
static std::set<std::wstring> g_gammaSet;          // GDI sources Knob has tinted (put back on exit)
// "Sunset to sunrise": where you are comes from your internet address (looked up once a week, only with that schedule on)
static bool HttpGet(const std::wstring& url, std::string& out, const std::wstring& saveTo, DWORD* status);
static DWORD WINAPI GeoProc(void*) {
    std::string js;
    if (HttpGet(L"https://ipapi.co/json/", js, L"", nullptr)) {
        auto num = [&](const char* k) { size_t p = js.find(std::string("\"") + k + "\""); if (p == std::string::npos) return 999.0; p = js.find(':', p); return p == std::string::npos ? 999.0 : atof(js.c_str() + p + 1); };
        double la = num("latitude"), lo = num("longitude");
        if (la > -90 && la < 90 && lo > -180 && lo < 180) {   // handed to the main thread, which owns these values
            double* v = new double[2]{la, lo};
            if (!PostMessageW(g_hwnd, WM_APP + 31, 0, (LPARAM)v)) delete[] v;
        }
    }
    return 0;
}
static void GeoRefresh() {
    static DWORD last = 0;
    if (g_nightAuto != 2 || (g_geoLat < 900 && time(nullptr) - g_geoWhen < 7 * 86400)) return;
    if (last && GetTickCount() - last < 3600 * 1000) return;   // at most once an hour when it fails
    last = GetTickCount();
    HANDLE t = CreateThread(nullptr, 0, GeoProc, nullptr, 0, nullptr); if (t) CloseHandle(t);
}
static int SunMinutes(bool rise) {   // local minutes after midnight of sunrise or sunset today; -1 none (polar day/night)
    SYSTEMTIME st; GetLocalTime(&st);
    static const int cum[12] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
    int N = cum[st.wMonth - 1] + st.wDay + ((st.wMonth > 2 && st.wYear % 4 == 0) ? 1 : 0);
    const double D = 3.14159265358979 / 180;
    double lngH = g_geoLon / 15, t = N + ((rise ? 6 : 18) - lngH) / 24, M = 0.9856 * t - 3.289;
    double L = fmod(M + 1.916 * sin(M * D) + 0.020 * sin(2 * M * D) + 282.634 + 720, 360.0);
    double RA = fmod(atan(0.91764 * tan(L * D)) / D + 720, 360.0);
    RA += floor(L / 90) * 90 - floor(RA / 90) * 90; RA /= 15;
    double sinDec = 0.39782 * sin(L * D), cosDec = cos(asin(sinDec));
    double cosH = (cos(90.833 * D) - sinDec * sin(g_geoLat * D)) / (cosDec * cos(g_geoLat * D));
    if (cosH > 1 || cosH < -1) return -1;
    double H = (rise ? 360 - acos(cosH) / D : acos(cosH) / D) / 15;
    double UT = fmod(H + RA - 0.06571 * t - 6.622 - lngH + 48, 24.0);
    SYSTEMTIME u; GetSystemTime(&u);
    FILETIME fl, fu; SystemTimeToFileTime(&st, &fl); SystemTimeToFileTime(&u, &fu);
    ULARGE_INTEGER ql, qu; ql.LowPart = fl.dwLowDateTime; ql.HighPart = fl.dwHighDateTime; qu.LowPart = fu.dwLowDateTime; qu.HighPart = fu.dwHighDateTime;
    double off = ((long long)ql.QuadPart - (long long)qu.QuadPart) / 600000000.0;   // minutes ahead of UTC
    return (int)fmod(UT * 60 + off + 2880, 1440.0);
}
static bool NightActive(const std::wstring& name) {
    auto it = g_night.find(name); if (it == g_night.end() || !it->second.on || it->second.str <= 0) return false;
    SYSTEMTIME t; GetLocalTime(&t); int now = t.wHour * 60 + t.wMinute;
    if (g_nightAuto == 2 && g_geoLat < 900) {   // sunset to sunrise
        int rise = SunMinutes(true), set = SunMinutes(false);
        if (rise >= 0 && set >= 0) return set > rise ? (now >= set || now < rise) : (now >= set && now < rise);
    }
    if (g_nightAuto >= 1) return t.wHour >= 19 || t.wHour < 7;   // evenings (and sunset to sunrise until the location is known)
    return true;
}
static bool SetGamma(const std::wstring& gdi, int str) {
    HDC dc = CreateDCW(L"DISPLAY", gdi.c_str(), nullptr, nullptr);
    if (!dc) return false;
    bool ok = false;
    for (float k : {1.f, 0.6f, 0.35f}) {   // Windows refuses ramps too far from normal: back off until it takes
        float s = str / 100.f * k;
        float mul[3] = {1.f, 1.f - 0.28f * s, 1.f - 0.62f * s};
        WORD ramp[3][256];
        for (int c = 0; c < 3; c++) for (int i = 0; i < 256; i++) ramp[c][i] = (WORD)std::min(65535.f, i * 257 * mul[c]);
        if (SetDeviceGammaRamp(dc, ramp)) { ok = true; break; }
        if (str == 0) break;
    }
    DeleteDC(dc);
    if (ok) { if (str > 0) g_gammaSet.insert(gdi); else g_gammaSet.erase(gdi); }
    return ok;
}
static void ApplyNight() {   // every screen gets its tint (or none); duplicates share their source's
    std::set<std::wstring> done;
    for (auto& m : g_mons) {
        if (m.cloneOf >= 0 || done.count(m.gdi)) continue;
        done.insert(m.gdi);
        int str = NightActive(m.name) ? g_night[m.name].str : 0;
        if (str > 0 || g_gammaSet.count(m.gdi)) SetGamma(m.gdi, str);
    }
}
static void ResetGamma() { auto all = g_gammaSet; for (auto& gdi : all) SetGamma(gdi, 0); }

// dim further: a dark, click-through layer over one screen, below what the monitor itself can do
static std::map<std::wstring, int> g_dim;           // monitor name -> 0-80 % darker (not saved)
static std::map<std::wstring, HWND> g_dimWnds;
static LRESULT CALLBACK DimProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_PAINT) { PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps); FillRect(dc, &ps.rcPaint, (HBRUSH)GetStockObject(BLACK_BRUSH)); EndPaint(h, &ps); return 0; }
    if (msg == WM_NCHITTEST) return HTTRANSPARENT;
    return DefWindowProcW(h, msg, wp, lp);
}
static void ApplyDim() {
    static bool reg = false;
    if (!reg) { WNDCLASSW wc = {}; wc.lpfnWndProc = DimProc; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"KnobDim"; RegisterClassW(&wc); reg = true; }
    std::set<std::wstring> want;
    for (auto& m : g_mons) {
        auto it = g_dim.find(m.name);
        if (it == g_dim.end() || it->second <= 0 || m.cloneOf >= 0) continue;
        want.insert(m.name);
        HWND& w = g_dimWnds[m.name];
        if (!w) w = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"KnobDim", L"", WS_POPUP,
                                    0, 0, 1, 1, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!w) continue;
        SetLayeredWindowAttributes(w, 0, (BYTE)std::min(230, it->second * 255 / 100), LWA_ALPHA);
        SetWindowPos(w, HWND_TOPMOST, m.rc.left, m.rc.top, m.rc.right - m.rc.left, m.rc.bottom - m.rc.top, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        if (g_hwnd && !g_set && IsWindowVisible(g_hwnd)) SetWindowPos(g_hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);   // the panel stays readable
    }
    for (auto it = g_dimWnds.begin(); it != g_dimWnds.end();) {
        if (!want.count(it->first)) { if (it->second) DestroyWindow(it->second); it = g_dimWnds.erase(it); } else ++it;
    }
}

// ---------------------------------------------------------------- theme + fonts
struct Theme {
    bool dark = true, taskbarLight = false, glass = false;   // glass = Glass style is active (Windows 11 acrylic available)
    Color surface, t1, t2, t3, track, thumb, subtle, subtle2, subtle3, accent, onAccent;
    Color tabsBg, tabSel, badge, master, hover, ddBtn, ddBtnHover, valBg, rail, popup;
    Color selRow[2];   // selected row, at rest and hovered
} T;

static Color WithAlpha(Color c, int a) { return Color((BYTE)a, c.GetR(), c.GetG(), c.GetB()); }
static Color Hex(DWORD rgb) { return Color(255, (rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255); }


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
    if (g_themeMode == 1) T.dark = false; else if (g_themeMode == 2) T.dark = true;   // Appearance > Theme
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
    {   // the same elements in both styles (as in the design); Solid only swaps the surface
        T.surface = gl ? (d ? Color(40, 16, 16, 16) : Color(40, 250, 250, 250)) : (d ? Color(255, 32, 32, 32) : Color(255, 243, 243, 243));
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
        auto mixW = [&](float wa, float wb, Color b) {   // color-mix(accent wa%, b) the way the design's CSS does it
            float ab = b.GetA() / 255.f, al = wa + wb * ab, k = wa / al;
            return Color((BYTE)lroundf(al * 255), (BYTE)lroundf(T.accent.GetR() * k + b.GetR() * (1 - k)), (BYTE)lroundf(T.accent.GetG() * k + b.GetG() * (1 - k)), (BYTE)lroundf(T.accent.GetB() * k + b.GetB() * (1 - k)));
        };
        T.selRow[0] = d ? mixW(.07f, .93f, Color(15, 255, 255, 255)) : mixW(.08f, .92f, Color(102, 255, 255, 255));
        T.selRow[1] = d ? mixW(.10f, .90f, Color(23, 255, 255, 255)) : mixW(.11f, .89f, Color(115, 255, 255, 255));
        if (!gl) T.popup = d ? Color(255, 44, 44, 44) : Color(255, 249, 249, 249);
    }
}

static FontFamily *g_famText, *g_famSemi, *g_famIcon;
static Font *g_fName, *g_fName600, *g_fSmall, *g_fSmall600, *g_fH3, *g_fTab, *g_fTab600, *g_fIcon, *g_fIconS, *g_fNum, *g_f11, *g_f11s;

static FontFamily* PickFamily(std::initializer_list<const wchar_t*> names) {
    for (auto n : names) { FontFamily* f = new FontFamily(n); if (f->IsAvailable()) return f; delete f; }
    return FontFamily::GenericSansSerif()->Clone();
}
static void InitFonts() {
    if (!g_famText) {
        g_famText = PickFamily({L"Segoe UI Variable Text", L"Segoe UI", L"Tahoma"});
        g_famSemi = PickFamily({L"Segoe UI Semibold", L"Segoe UI Variable Text", L"Segoe UI", L"Tahoma"});
        g_famIcon = g_famText;   // icons are drawn as shapes; the fonts below only carry their size
    }
    for (Font** f : {&g_fName, &g_fName600, &g_fSmall, &g_fSmall600, &g_fH3, &g_fTab, &g_fTab600, &g_fIcon, &g_fIconS, &g_fNum, &g_f11, &g_f11s}) { delete *f; *f = nullptr; }
    g_fName = new Font(g_famText, S(13), FontStyleRegular, UnitPixel);
    g_fName600 = new Font(g_famSemi, S(13), FontStyleRegular, UnitPixel);
    g_fSmall = new Font(g_famText, S(11.5f), FontStyleRegular, UnitPixel);
    g_fSmall600 = new Font(g_famSemi, S(12), FontStyleRegular, UnitPixel);
    g_fH3 = new Font(g_famSemi, S(10.5f), FontStyleRegular, UnitPixel);
    g_fTab = new Font(g_famText, S(13), FontStyleRegular, UnitPixel);
    g_fTab600 = new Font(g_famSemi, S(13), FontStyleRegular, UnitPixel);
    g_fIcon = new Font(g_famIcon, S(18), FontStyleRegular, UnitPixel);
    g_fIconS = new Font(g_famIcon, S(16), FontStyleRegular, UnitPixel);
    g_fNum = new Font(g_famSemi, S(10), FontStyleRegular, UnitPixel);
    g_f11 = new Font(g_famText, S(11), FontStyleRegular, UnitPixel);    // value pills, chips
    g_f11s = new Font(g_famSemi, S(11), FontStyleRegular, UnitPixel);   // refresh rate, delay
}

// Segoe Fluent Icons / MDL2 glyphs
enum : wchar_t {
    G_VOLUME = 0xE767, G_MUTE = 0xE74F, G_MIC = 0xE720, G_MICOFF = 0xEC54, G_SPEAKERS = 0xE7F5, G_HEADPHONE = 0xE7F6,
    G_TV = 0xE7F4, G_BRIGHT = 0xE706, G_CONTRAST = 0xE793, G_ROTATE = 0xE7AD, G_APPS = 0xE71D, G_CHEVDOWN = 0xE70D,
    G_SETTINGS = 0xE713, G_RINGER = 0xEA8F, G_EQ = 0xE9E9, G_IDENT = 0xE7F4, G_REFRESH = 0xE72C, G_EYE = 0xE890,
    G_POWER = 0xE7E8, G_CLOSE = 0xE711, G_OPEN = 0xE8A7
};

// ---------------------------------------------------------------- layout model
enum ElKind { E_H3, E_OUT, E_IN, E_MASTER, E_APP, E_MONHEAD, E_MONB, E_MONC, E_MONO, E_MONDIM, E_MONNL, E_NOTE, E_DTOP, E_MONOFF };
struct El { ElKind k; int idx; RectF r; std::wstring text; };
enum HitKind { H_NONE, H_TAB, H_ROW, H_SLIDER, H_MUTE, H_DROPDOWN, H_IDENT, H_GEAR, H_REFRESH, H_DDOPT, H_MENUBG, H_MICALL, H_DELAYCHIP, H_UPDATE, H_RATEDD, H_INPUTDD, H_PROFDD, H_SCREENOFF, H_APPOUT, H_RESETOUT, H_ROUTING, H_NIGHT, H_HDR, H_MODEDD, H_ONESPK, H_AUTOSYNC, H_DLMINUS, H_DLPLUS, H_DLSYNC, H_CALDONE, H_DLRESET, H_CALSTART, H_CALCANCEL, H_OFFEXT, H_OFFDUP, H_PARTY };
struct Hit { HitKind h = H_NONE; int el = -1; int idx = -1; };

static const float LW = 380, TABH = 54, FTR = 44, PANEH = 470, ROWH = 78, ROWGAP = 3;
static std::vector<El> g_els;
static int g_W = 0, g_H = 0;
static float g_contentH = 0;
static Hit g_hover;
static int g_dragEl = -1;
static int g_hoverSlider = -1; static DWORD g_hoverSince = 0;
static int g_ddOpen = -1;          // display index whose list is open (0 for the profile list)
static int g_ddKind = 0;           // which list: 0 orientation, 1 refresh rate, 2 input, 3 profiles
static DWORD g_spinUntil = 0;      // refresh icon animation
static int SI() { return g_tab; }

static float PaneTop() { return S(TABH); }
static float PaneBottom() { return g_H - S(FTR); }
static float PaneX() { return S(12); }
static float PaneW() { return g_W - S(34); }   // rows are 346 wide: the pane keeps a 10 px gutter for its scrollbar, as in the design

static const float APPLINE = 22;
static const int SP_SOUND_PAGE = 1;   // Settings › Sound (where the routing lives)
static bool OutHasApps(int i) {   // some app is sent to this speaker on its own
    for (auto& a : g_apps) { if (a.system) continue; auto r = AppRoutes(a); if (std::find(r.begin(), r.end(), g_out[i].id) != r.end()) return true; }
    return false;
}
static void Layout() {
    g_els.clear();
    float x = PaneX(), w = PaneW();
    float y = PaneTop() + S(6) - g_scroll[SI()];
    bool prevRow = false;
    // section label: 18 above, 8 below (the design's h3); h is taller where buttons sit beside it
    auto h3 = [&](const wchar_t* t, float h = 13) { y += S(18); g_els.push_back({E_H3, -1, RectF(x + S(10), y, w - S(16), S(h)), t}); y += S(h) + S(8); prevRow = false; };
    auto row = [&](ElKind k, int i) { if (prevRow) y += S(ROWGAP); g_els.push_back({k, i, RectF(x, y, w, S(ROWH)), L""}); y += S(ROWH); prevRow = true; };
    if (g_tab == T_SOUND) {
        h3(L"OUTPUT", 24);
        for (int i = 0; i < (int)g_out.size(); i++) {
            row(E_OUT, i);
            if (OutHasApps(i)) { g_els.back().r.Height += S(APPLINE); y += S(APPLINE); }   // a line of app icons under the name
        }
        h3(L"INPUT");
        for (int i = 0; i < (int)g_in.size(); i++) row(E_IN, i);
    } else if (g_tab == T_APPS) {
        row(E_MASTER, 0);
        h3(L"APPS", 18);
        for (int i = 0; i < (int)g_apps.size(); i++) row(E_APP, i);
        if (g_apps.empty()) { g_els.push_back({E_NOTE, -1, RectF(x + S(10), y, w, S(24)), L"No apps are using sound right now"}); y += S(30); }
    } else {
        y += S(14);
        g_els.push_back({E_DTOP, 0, RectF(x, y, w, S(32)), L""}); y += S(32);   // profile list + turn off displays
        h3(L"DISPLAYS");
        if (!g_dispStatus.empty()) { g_els.push_back({E_NOTE, -1, RectF(x + S(10), y, w - S(20), S(20)), g_dispStatus}); y += S(24); }
        if (g_mons.empty()) { g_els.push_back({E_NOTE, -1, RectF(x + S(10), y + S(6), w, S(24)), g_monsLoaded ? L"No displays found" : L"Looking for displays…"}); y += S(36); }
        for (int i = 0; i < (int)g_mons.size(); i++) {
            if (i) y += S(14);
            y += S(4);
            g_els.push_back({E_MONHEAD, i, RectF(x, y, w, S(30)), L""}); y += S(30);
            g_els.push_back({E_MONB, i, RectF(x, y, w, S(36)), L""}); y += S(36);
            bool dimRow = g_mons[i].ok && (g_mons[i].pct == 0 || (g_dim.count(g_mons[i].name) && g_dim[g_mons[i].name] > 0)) && g_mons[i].cloneOf < 0;
            if (dimRow) { g_els.push_back({E_MONDIM, i, RectF(x, y, w, S(36)), L""}); y += S(36); }   // only at 0%: go darker than the monitor can
            if (g_mons[i].hasCon) { g_els.push_back({E_MONC, i, RectF(x, y, w, S(36)), L""}); y += S(36); }
            if (g_night.count(g_mons[i].name) && g_night[g_mons[i].name].on) { g_els.push_back({E_MONNL, i, RectF(x, y, w, S(36)), L""}); y += S(36); }   // night light strength
            g_els.push_back({E_MONO, i, RectF(x, y, w, S(36)), L""}); y += S(36);
            y += S(2);
        }
        for (int k = 0; k < (int)g_offScreens.size(); k++) {   // switched-off screens: just the name and a way back, no sliders
            y += S(g_mons.empty() && !k ? 4 : 14);
            g_els.push_back({E_MONOFF, k, RectF(x, y, w, S(34)), L""}); y += S(34);
        }
    }
    g_contentH = y + g_scroll[SI()] - PaneTop() + S(14);
}

static void ClampScroll() {
    float view = PaneBottom() - PaneTop();
    float mx = std::max(0.f, g_contentH - view);
    g_scroll[SI()] = std::max(0.f, std::min(g_scroll[SI()], mx));
}

// row geometry, as in the design: padding 7 8 5 10; name line 30; control line 36
static bool IsRowKind(ElKind k) { return k == E_OUT || k == E_IN || k == E_MASTER || k == E_APP; }
static float RowCy1(const El& e) { return e.r.Y + S(22); }
static float RowCy2(const El& e) { return e.r.Y + e.r.Height - S(23); }
static float CtlPadL(const El& e) { return IsRowKind(e.k) ? S(8) : S(6); }    // rows sit 2 px further in than the display lines
static float CtlPadR(const El& e) { return IsRowKind(e.k) ? S(8) : S(6); }
static RectF CtlMute(const El& e, float cy) { return RectF(e.r.X + CtlPadL(e), cy - S(16), S(32), S(32)); }
static RectF CtlVal(const El& e, float cy) { return RectF(e.r.X + e.r.Width - CtlPadR(e) - S(40), cy - S(10), S(40), S(20)); }
static RectF CtlSlider(const El& e, float cy) { float l = e.r.X + CtlPadL(e) + S(42), r = CtlVal(e, cy).X - S(12); return RectF(l, cy - S(16), r - l, S(32)); }
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
static std::map<int, float> g_appChipW;   // measured when drawn: the chip is as wide as its label, up to a limit
static RectF AppOutChip(const El& e) { auto it = g_appChipW.find(e.idx); float w = it != g_appChipW.end() ? it->second : S(122); return RectF(e.r.X + e.r.Width - S(8) - w, e.r.Y + S(22) - S(13), w, S(26)); }
static bool AnyAppRouted() { for (auto& a : g_apps) if (!a.outId.empty()) return true; return !g_appExtra.empty(); }
static RectF AppIconRect(const El& e) { float lx = e.r.X + S(10) + S(16), cy = e.r.Y + S(22); return RectF(lx - S(16), cy - S(16), S(32), S(32)); }
static RectF AppDdAnchor(const El& e) { return g_apps[e.idx].system ? AppIconRect(e) : AppOutChip(e); }   // the list opens from the chip
static bool NeedsSync();
static Graphics* MeasureG() {   // one small surface for measuring text outside of painting
    static Bitmap* bm = nullptr; static Graphics* mg = nullptr;
    if (!mg) { bm = new Bitmap(4, 4, PixelFormat32bppPARGB); mg = new Graphics(bm); mg->SetTextRenderingHint(TextRenderingHintAntiAliasGridFit); }
    return mg;
}
static float MeasureW(const wchar_t* s, Font* f) { RectF out; MeasureG()->MeasureString(s, -1, f, PointF(0, 0), &out); return out.Width; }
// Sound header, right to left: Routing · "N speakers ×"   (Auto sync lives in Settings › Sound only)
static bool ShowSyncBtns() { return SpeakersInUse() > 1; }
static RectF RoutingRect(const El& e) {
    float w = S(4) + MeasureW(L"Routing", g_fSmall) + S(4), right = e.r.X + e.r.Width;
    return RectF(right - w, e.r.Y + e.r.Height / 2 - S(9), w, S(18));
}
static std::wstring OneLabel() { return std::to_wstring(SpeakersInUse()) + L" speakers"; }
static RectF OneRect(const El& e) { float w = S(10) + MeasureW(OneLabel().c_str(), g_f11) + S(6) + S(11) + S(9); RectF r = RoutingRect(e); return RectF(r.X - S(8) - w, e.r.Y + e.r.Height / 2 - S(12), w, S(24)); }
static RectF ResetOutRect(const El& e) { float w = S(4) + MeasureW(L"Reset outputs", g_fSmall) + S(4); return RectF(e.r.X + e.r.Width - w, e.r.Y + e.r.Height / 2 - S(9), w, S(18)); }
static std::wstring OutLabel(const Endpoint& o) {   // the device's name; add its type when two devices share a name
    int same = 0; for (auto& x : g_out) if (x.name == o.name) same++;
    return same > 1 && !o.detail.empty() ? o.name + L" \u00b7 " + o.detail : o.name;
}
static std::wstring OutName(const std::wstring& id) { for (auto& o : g_out) if (o.id == id) return OutLabel(o); return L"Default output"; }
static float g_delayChipW = 88;
static RectF DelayChip(const El& e) { float w = g_delayChipW; return RectF(e.r.X + e.r.Width - S(8) - w, e.r.Y + S(22) - S(13), w, S(26)); }
static bool DelayAnchor(RectF& chip) { for (auto& e : g_els) if (e.k == E_OUT && e.idx == g_delayOpen) { chip = DelayChip(e); return true; } return false; }
static std::wstring DelayNote(int i, bool* bold = nullptr);
static float DelayNoteLines();
static RectF DelayPopup(const RectF& chip) {   // the design's "bstep" popup: 256 wide, under the chip
    float w = S(256), h = S(12) + S(17) + S(8) + S(34) + S(8) + DelayNoteLines() * S(17.25f) + S(10) + S(24) + S(12);
    float y = chip.Y + S(30);
    if (y + h > PaneBottom() - S(4)) y = chip.Y - S(4) - h;
    return RectF(std::max(PaneX(), chip.X + chip.Width - w), y, w, h);
}
static RectF DlMinus(const RectF& pop) { return RectF(pop.X + S(14), pop.Y + S(37), S(34), S(34)); }
static RectF DlPlus(const RectF& pop) { return RectF(pop.X + pop.Width - S(14) - S(34), pop.Y + S(37), S(34), S(34)); }
static RectF DlReset(const RectF& pop) { float w = S(22) + MeasureW(L"Reset", g_f11s); return RectF(pop.X + pop.Width - S(14) - w, pop.Y + pop.Height - S(12) - S(24), w, S(24)); }
static bool InUse(int i) { auto u = UsedSpeakers(); return i >= 0 && i < (int)g_out.size() && std::find(u.begin(), u.end(), g_out[i].id) != u.end(); }
static int DelayOf(int i) { return ShownDelay(g_out[i].id); }   // how long this speaker waits; the slowest shows 0
static bool HasDelayChip(int i) { return SpeakersInUse() > 1 && InUse(i) && (DelayOf(i) > 0 || g_delayOpen == i); }
static bool NeedsSync() {   // several speakers, and one of them was never measured
    if (SpeakersInUse() < 2) return false;
    int di = DefaultOut();
    for (auto& id : UsedSpeakers()) if ((di < 0 || g_out[di].id != id) && !g_delay.count(id)) return true;
    return false;
}
static void NudgeDelay(int i, int ms) {   // −/+ in the delay popup: going below 0 holds the other speakers back instead
    if (i < 0 || i >= (int)g_out.size()) return;
    if (g_out[i].isDefault) { for (auto& id : UsedSpeakers()) if (id != g_out[i].id) g_delay[id] = std::max(-2000, std::min(2000, DelayFor(id) - ms)); }
    else g_delay[g_out[i].id] = std::max(-2000, std::min(2000, DelayFor(g_out[i].id) + ms));
}

static bool InRect(const RectF& r, float x, float y) { return x >= r.X && x < r.X + r.Width && y >= r.Y && y < r.Y + r.Height; }

// ---- Display tab controls and their lists (in-panel popups, styled like the panel)
static const wchar_t* ORIENT_NAMES[4] = {L"Landscape", L"Portrait", L"Landscape (flipped)", L"Portrait (flipped)"};
static RectF OffExt(const El& e) { float w = S(18) + MeasureW(L"Extend", g_fSmall); return RectF(e.r.X + e.r.Width - S(10) - w, e.r.Y + S(2), w, S(30)); }
static RectF OffDup(const El& e) { float w = S(18) + MeasureW(L"Duplicate", g_fSmall); RectF x = OffExt(e); return RectF(x.X - S(6) - w, e.r.Y + S(2), w, S(30)); }
static RectF DTopParty(const El& e) { float w = S(12) + S(14) + S(6) + MeasureW(L"Party", g_fSmall) + S(12); return RectF(e.r.X + e.r.Width - S(10) - w, e.r.Y, w, S(32)); }
static RectF DTopOff(const El& e) {   // "Turn off", or the countdown with a × to cancel it
    std::wstring t = g_offAt ? OffLeft() : L"Turn off";
    float w = S(12) + S(14) + S(6) + MeasureW(t.c_str(), g_fSmall) + (g_offAt ? S(16) : 0) + S(12);
    RectF p = DTopParty(e); return RectF(p.X - S(8) - w, e.r.Y, w, S(32));
}
static RectF DTopProfile(const El& e) { RectF o = DTopOff(e); return RectF(e.r.X + S(10), e.r.Y, o.X - S(8) - e.r.X - S(10), S(32)); }
static bool HasRates(int i) { return i >= 0 && i < (int)g_mons.size() && g_mons[i].rates.size() >= 2; }
// header pills, right to left: night light, HDR (screens that can), refresh rate
static float MeasureW(const wchar_t* s, Font* f);
static RectF MoonPill(const El& e) { return RectF(e.r.X + e.r.Width - S(6) - S(30), e.r.Y + e.r.Height / 2 - S(10), S(30), S(20)); }
static RectF HdrPill(const El& e) { RectF m = MoonPill(e); return RectF(m.X - S(6) - S(42), m.Y, S(42), m.Height); }
static RectF RateBtn(const El& e) {
    RectF l = g_mons[e.idx].hdrOk ? HdrPill(e) : MoonPill(e);
    std::wstring t = std::to_wstring(g_mons[e.idx].rate) + L" Hz"; float w = S(9) + MeasureW(t.c_str(), g_f11s) + S(22);
    return RectF(l.X - S(6) - w, e.r.Y + e.r.Height / 2 - S(11), w, S(22));
}
// bottom row, full width: orientation · input · extend / duplicate
static bool HasInputs(int i) { return i >= 0 && i < (int)g_mons.size() && !g_mons[i].inputs.empty(); }
static bool HasMode(int i) { return i >= 0 && i < (int)g_mons.size() && (g_mons.size() > 1 || g_connScreens > 1); }
static RectF MonoBtn(const El& e, int which) {   // 0 orientation, 1 input, 2 extend/duplicate; empty if not shown
    float wts[3] = {1.24f, HasInputs(e.idx) ? 0.88f : 0, HasMode(e.idx) ? 1.04f : 0};   // the widths the design ends up with
    if (!wts[which]) return RectF();
    int n = (wts[1] > 0) + (wts[2] > 0) + 1;
    float gap = S(8), avail = e.r.Width - S(14) - gap * (n - 1), sum = wts[0] + wts[1] + wts[2], x = e.r.X + S(8);
    for (int k = 0; k < which; k++) if (wts[k]) x += avail * wts[k] / sum + gap;
    return RectF(x, e.r.Y + S(2), avail * wts[which] / sum, S(32));
}
static RectF OrientBtn(const El& e) { return MonoBtn(e, 0); }
static RectF InputBtn(const El& e) { return MonoBtn(e, 1); }
static std::vector<int> g_ddModeMap;   // Extend / Duplicate list row -> screen (-1 extend)
static bool ProfileActive(int p);
static void ApplyProfile(int p);
static std::vector<int> g_ddProfMap;   // list row -> profile index (-1 new, -2 manage)
static std::vector<std::wstring> DropdownOptions(int& cur) {
    std::vector<std::wstring> o; cur = -1;
    if (g_ddKind == 3) {
        g_ddProfMap.clear();
        for (int i = 0; i < (int)g_prof.size(); i++) if (!g_prof[i].v.empty()) { o.push_back(g_prof[i].name); g_ddProfMap.push_back(i); if (ProfileActive(i)) cur = (int)o.size() - 1; }
        o.push_back(L"Save current as new profile"); g_ddProfMap.push_back(-1);
        o.push_back(L"Manage profiles\u2026"); g_ddProfMap.push_back(-2);
        return o;
    }
    if (g_ddKind == 4) {   // where an app plays: ticks, not a single choice (Windows 11 can copy an app to several speakers)
        if (g_ddOpen < 0 || g_ddOpen >= (int)g_apps.size()) return o;
        o.push_back(L"Default speaker");
        for (size_t i = 0; i < g_out.size(); i++) o.push_back(OutLabel(g_out[i]));
        return o;
    }
    if (g_ddOpen < 0 || g_ddOpen >= (int)g_mons.size()) return o;
    MonUI& m = g_mons[g_ddOpen];
    if (g_ddKind == 5) {   // extend, show the same picture as another screen, or this screen only
        g_ddModeMap = {-1}; o.push_back(L"Extend"); cur = (g_mons.size() == 1 && g_connScreens > 1) ? -1 : 0;
        for (int k = 0; k < (int)g_mons.size(); k++) {
            if (k == g_ddOpen) continue;
            o.push_back(g_mons[k].name); g_ddModeMap.push_back(k);
            if (m.cloneOf == k || (g_mons[k].cloneOf >= 0 && g_mons[k].cloneOf == m.cloneOf && m.cloneOf >= 0)) cur = (int)o.size() - 1;
        }
        o.push_back(L"Only this screen"); g_ddModeMap.push_back(-2); if (g_mons.size() == 1 && g_connScreens > 1) cur = (int)o.size() - 1;
        return o;
    }
    if (g_ddKind == 0) { for (int i = 0; i < 4; i++) o.push_back(ORIENT_NAMES[i]); cur = m.orient; }
    else if (g_ddKind == 1) { for (size_t i = 0; i < m.rates.size(); i++) { o.push_back(std::to_wstring(m.rates[i]) + L" Hz"); if (m.rates[i] == m.rate) cur = (int)i; } }
    else if (g_ddKind == 2) { for (size_t i = 0; i < m.inputs.size(); i++) { o.push_back(InputName(m.inputs[i], m.inputs)); if (m.inputs[i] == m.input) cur = (int)i; } }
    return o;
}
static bool DropdownRect(RectF& btn) {
    if (g_ddOpen < 0) return false;
    for (auto& e : g_els) {
        if (g_ddKind == 3 && e.k == E_DTOP) { btn = DTopProfile(e); return true; }
        if (g_ddKind == 4) { if (e.k == E_APP && e.idx == g_ddOpen) { btn = AppDdAnchor(e); return true; } continue; }
        if (e.idx != g_ddOpen) continue;
        if (g_ddKind == 0 && e.k == E_MONO) { btn = OrientBtn(e); return true; }
        if (g_ddKind == 1 && e.k == E_MONHEAD) { btn = RateBtn(e); return true; }
        if (g_ddKind == 2 && e.k == E_MONO) { btn = InputBtn(e); return true; }
        if (g_ddKind == 5 && e.k == E_MONO) { btn = MonoBtn(e, 2); return true; }
    }
    return false;
}
static float DdExtraBefore(int i) {   // a line (and a "Duplicate" heading) inside some lists, as in the design
    if (g_ddKind == 5 && i < (int)g_ddModeMap.size() && g_ddModeMap[i] == -2) return S(9);   // line above "Only this screen"
    if (g_ddKind == 5 && i == 1) return S(9) + S(19);
    if (g_ddKind == 4 && i == 1) return S(9);
    if (g_ddKind == 3 && i < (int)g_ddProfMap.size() && g_ddProfMap[i] == -1 && i > 0) return S(9);
    return 0;
}
static float MeasureW(const wchar_t* s, Font* f);
static RectF DropdownPopup(const RectF& btn) {
    int cur; auto opts = DropdownOptions(cur); int n = (int)opts.size();
    float h = S(4) * 2 + S(32) * n; for (int i = 0; i < n; i++) h += DdExtraBefore(i);
    float content = 0;   // as wide as the longest item ("max-content"), never narrower than its button
    for (int i = 0; i < n; i++) content = std::max(content, MeasureW(opts[i].c_str(), g_fName) + S(24) + (g_ddKind == 5 && i > 0 && g_ddModeMap[i] >= 0 ? S(26) : 0) + (g_ddKind == 3 && g_ddProfMap[i] == -1 ? S(19) : 0));
    content += S(8);
    float w = g_ddKind == 1 ? S(110) : g_ddKind == 4 ? S(244) : g_ddKind == 3 ? std::max(S(230), content) : std::max(btn.Width, content);
    w = std::min(w, PaneW());
    bool right = g_ddKind == 1 || g_ddKind == 4 || g_ddKind == 5;   // lists that hang from the right edge of their button
    float x = right ? btn.X + btn.Width - w : btn.X;
    x = std::max(PaneX(), std::min(x, PaneX() + PaneW() - w));
    float below = btn.Y + btn.Height + S(4), above = btn.Y - S(4) - h;
    bool up = g_ddKind == 0 || g_ddKind == 2 || g_ddKind == 5;   // the bottom row of a screen opens upwards
    if (g_ddKind == 4) up = (btn.Y - PaneTop()) > (PaneBottom() - btn.Y - btn.Height);   // the side with more room
    float y = up ? above : below;
    if (up && y < PaneTop() + S(4)) y = below;
    if (!up && y + h > PaneBottom() - S(4)) y = above >= PaneTop() + S(4) ? above : std::max(PaneTop() + S(4), PaneBottom() - S(4) - h);
    return RectF(x, y, w, h);
}
static RectF DropdownOption(const RectF& pop, int i) {
    float y = pop.Y + S(4); for (int k = 0; k <= i; k++) y += DdExtraBefore(k);
    return RectF(pop.X + S(4), y + i * S(32), pop.Width - S(8), S(32));
}

static RectF CalCard();
static RectF CalBtn(int which);   // 0 Cancel / Stop, 1 Start / Done
static Hit HitTest(float x, float y) {
    Hit h;
    if (g_calShow && g_tab == T_SOUND && InRect(CalCard(), x, y)) {
        h.h = InRect(CalBtn(0), x, y) ? H_CALCANCEL : InRect(CalBtn(1), x, y) ? (g_calAsk ? H_CALSTART : H_CALDONE) : H_MENUBG;
        return h;
    }
    RectF dchip;
    if (g_delayOpen >= 0 && DelayAnchor(dchip)) {
        RectF pop = DelayPopup(dchip);
        if (InRect(pop, x, y)) {
            h.h = InRect(DlMinus(pop), x, y) ? H_DLMINUS : InRect(DlPlus(pop), x, y) ? H_DLPLUS : InRect(DlReset(pop), x, y) ? H_DLRESET : H_MENUBG;
            return h;
        }
    }
    RectF btn;
    if (DropdownRect(btn)) {
        RectF pop = DropdownPopup(btn);
        if (InRect(pop, x, y)) {
            int cur; int n = (int)DropdownOptions(cur).size();
            for (int i = 0; i < n; i++) if (InRect(DropdownOption(pop, i), x, y)) { h.h = H_DDOPT; h.idx = i; return h; }
            h.h = H_MENUBG; return h;
        }
    }
    for (int i = 0; i < 3; i++) if (InRect(TabRect(i), x, y)) { h.h = H_TAB; h.idx = i; return h; }
    if (InRect(GearRect(), x, y)) { h.h = H_GEAR; return h; }
    if (InRect(IdentRect(), x, y)) { h.h = H_IDENT; return h; }
    if (InRect(RefreshRect(), x, y)) { h.h = H_REFRESH; return h; }
    if (InRect(MicAllRect(), x, y)) { h.h = H_MICALL; return h; }
    if (g_updState == UPD_AVAILABLE || g_updState == UPD_FAILED) { if (InRect(g_verRect, x, y)) { h.h = H_UPDATE; return h; } }
    if (y < PaneTop() || y >= PaneBottom()) return h;
    for (int i = 0; i < (int)g_els.size(); i++) {
        El& e = g_els[i];
        if (e.k == E_H3 && e.text == L"APPS" && AppRouteReady() && InRect(ResetOutRect(e), x, y)) { h.h = H_RESETOUT; h.el = i; return h; }
        if (e.k == E_H3 && e.text == L"OUTPUT" && InRect(RoutingRect(e), x, y)) { h.h = H_ROUTING; h.el = i; return h; }
        if (e.k == E_H3 && e.text == L"OUTPUT" && ShowSyncBtns() && InRect(OneRect(e), x, y)) { h.h = H_ONESPK; h.el = i; return h; }
        if (!InRect(e.r, x, y)) continue;
        h.el = i; h.idx = e.idx;
        if (IsRowKind(e.k)) {
            float cy2 = RowCy2(e);
            RectF sl = CtlSlider(e, cy2); sl.X -= S(10); sl.Width += S(20);
            if (e.k == E_OUT && HasDelayChip(e.idx) && InRect(DelayChip(e), x, y)) h.h = H_DELAYCHIP;
            else if (e.k == E_APP && !g_apps[e.idx].system && (AppRouteReady() || g_demo) && InRect(AppOutChip(e), x, y)) h.h = H_APPOUT;
            else if (InRect(CtlMute(e, cy2), x, y)) h.h = H_MUTE;
            else if (InRect(RectF(sl.X, cy2 - S(18), sl.Width, S(36)), x, y)) h.h = H_SLIDER;
            else h.h = (e.k == E_OUT || e.k == E_IN) ? H_ROW : H_NONE;
        } else if (e.k == E_MONOFF) {
            if (InRect(OffExt(e), x, y)) h.h = H_OFFEXT;
            else if (InRect(OffDup(e), x, y)) h.h = H_OFFDUP;
        } else if (e.k == E_MONB || e.k == E_MONC || e.k == E_MONDIM || e.k == E_MONNL) {
            RectF sl = CtlSlider(e, SliderCy(e)); sl.X -= S(10); sl.Width += S(20);
            if (InRect(sl, x, y) && (g_mons[e.idx].ok || e.k == E_MONNL || e.k == E_MONDIM)) h.h = H_SLIDER;
        } else if (e.k == E_MONO) {
            if (InRect(OrientBtn(e), x, y)) h.h = H_DROPDOWN;
            else if (HasInputs(e.idx) && InRect(InputBtn(e), x, y)) h.h = H_INPUTDD;
            else if (HasMode(e.idx) && InRect(MonoBtn(e, 2), x, y)) h.h = H_MODEDD;
        } else if (e.k == E_MONHEAD) {
            if (InRect(MoonPill(e), x, y)) h.h = H_NIGHT;
            else if (g_mons[e.idx].hdrOk && InRect(HdrPill(e), x, y)) h.h = H_HDR;
            else if (HasRates(e.idx) && InRect(RateBtn(e), x, y)) h.h = H_RATEDD;
        } else if (e.k == E_DTOP) {
            if (InRect(DTopProfile(e), x, y)) h.h = H_PROFDD;
            else if (InRect(DTopOff(e), x, y)) h.h = H_SCREENOFF;
            else if (InRect(DTopParty(e), x, y)) h.h = H_PARTY;
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
    case E_MONDIM: { auto it = g_dim.find(g_mons[e.idx].name); return it == g_dim.end() ? 0.f : it->second / 80.f; }
    case E_MONNL: { auto it = g_night.find(g_mons[e.idx].name); return it == g_night.end() ? 0.4f : it->second.str / 100.f; }
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
    case E_MONB: { int p = (int)lround(v * 100); if (p != g_mons[e.idx].pct) { g_mons[e.idx].pct = p; MonSet(e.idx, 0, p); }
                   if (p > 0 && g_dim.count(g_mons[e.idx].name)) { g_dim.erase(g_mons[e.idx].name); ApplyDim(); }   // brighter again: no extra dimming
                   break; }
    case E_MONDIM: { g_dim[g_mons[e.idx].name] = (int)lround(v * 80); ApplyDim(); break; }
    case E_MONNL: { auto& n = g_night[g_mons[e.idx].name]; n.str = (int)lround(v * 100); ApplyNight(); break; }
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
static bool HkRepeats(int a) { return a == HK_VOLUP || a == HK_VOLDOWN || a == HK_BRIUP || a == HK_BRIDOWN; }
static int g_scRegistered = 0;
static void UnregisterHotkeys() { for (int i = 0; i < g_scRegistered; i++) UnregisterHotKey(g_hwnd, 100 + i); g_scRegistered = 0; }
static bool HkSameKeys(int i) {   // another row has the same keys
    for (int j = 0; j < (int)g_sc.size(); j++) if (j != i && g_sc[i].k.vk && g_sc[j].k.vk == g_sc[i].k.vk && g_sc[j].k.mods == g_sc[i].k.mods) return true;
    return false;
}
static void RegisterHotkeys() {
    UnregisterHotkeys();
    for (int i = 0; i < (int)g_sc.size() && i < 200; i++) {
        HotKey& k = g_sc[i].k; k.ok = false;
        if (k.vk) k.ok = RegisterHotKey(g_hwnd, 100 + i, k.mods | (HkRepeats(g_sc[i].act) ? 0 : MOD_NOREPEAT), k.vk) != 0;
    }
    g_scRegistered = (int)std::min<size_t>(g_sc.size(), 200);
}

// ---------------------------------------------------------------- tray
static HICON MakeGlyphIcon(Color c, int sz);
static HICON MakeTrayIcon() {
    UINT dpi = GetDpiForSystem();
    return MakeGlyphIcon(T.taskbarLight ? Color(255, 28, 28, 28) : Color(255, 255, 255, 255), GetSystemMetricsForDpi(SM_CXSMICON, dpi));
}
static HICON MakeGlyphIcon(Color c, int sz) {   // the mixer mark, one colour (tray and Settings title bar)
    Bitmap bmp(sz, sz, PixelFormat32bppARGB);
    {
        Graphics g(&bmp);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.Clear(Color(0, 0, 0, 0));
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
static void TextWrap(Graphics& g, const wchar_t* s, Font* f, Color c, RectF r) {   // top-aligned, wraps, ellipsis on overflow
    StringFormat sf;
    sf.SetTrimming(StringTrimmingEllipsisWord);
    sf.SetFormatFlags(StringFormatFlagsLineLimit);
    SolidBrush b(c);
    g.DrawString(s, -1, f, r, &sf, &b);
}
static void PillText(Graphics& g, const wchar_t* s, Font* f, Color c, RectF r, StringAlignment h = StringAlignmentNear) { Text(g, s, f, c, RectF(r.X, r.Y - S(1), r.Width, r.Height), h); }
static float TextW(Graphics& g, const wchar_t* s, Font* f) { RectF out; g.MeasureString(s, -1, f, PointF(0, 0), &out); return out.Width; }
// ---------------------------------------------------------------- line icons
// The same 20-unit SVG icon set as the Knob UI design, parsed once and drawn with GDI+,
// so the app looks like the design and doesn't depend on which icon font Windows has.
static const struct { const char* n; const char* s; } ICONS[] = {
    {"sunLow", "<circle cx=\"10\" cy=\"10\" r=\"3\"/><path d=\"M10 4.5v.5M10 15v.5M4.5 10h.5M15 10h.5M6.1 6.1l.4.4M13.5 13.5l.4.4M6.1 13.9l.4-.4M13.5 6.5l.4-.4\"/>"},
    {"moon", "<path d=\"M15.5 12.5A6.5 6.5 0 0 1 7.5 4.5a6.5 6.5 0 1 0 8 8z\"/>"},
    {"speaker", "<path d=\"M3 7.5h2.5L9.5 4v12l-4-3.5H3z\"/><path d=\"M12.5 7.5a3.5 3.5 0 0 1 0 5\"/><path d=\"M14.8 5.2a6.8 6.8 0 0 1 0 9.6\"/>"},
    {"speakerOff", "<path d=\"M3 7.5h2.5L9.5 4v12l-4-3.5H3z\"/><path d=\"M13 8l4 4M17 8l-4 4\"/>"},
    {"headphones", "<path d=\"M3.5 13.5V10a6.5 6.5 0 0 1 13 0v3.5\"/><path d=\"M3.5 12.5h2v4h-1a1 1 0 0 1-1-1zM16.5 12.5h-2v4h1a1 1 0 0 0 1-1z\"/>"},
    {"mic", "<rect x=\"7.5\" y=\"2.5\" width=\"5\" height=\"9\" rx=\"2.5\"/><path d=\"M4.5 9.5a5.5 5.5 0 0 0 11 0M10 15v2.5\"/>"},
    {"micOff", "<rect x=\"7.5\" y=\"2.5\" width=\"5\" height=\"9\" rx=\"2.5\"/><path d=\"M4.5 9.5a5.5 5.5 0 0 0 11 0M10 15v2.5M3 3l14 14\"/>"},
    {"sun", "<circle cx=\"10\" cy=\"10\" r=\"3\"/><path d=\"M10 2.5v1.5M10 16v1.5M2.5 10H4M16 10h1.5M4.7 4.7l1 1M14.3 14.3l1 1M4.7 15.3l1-1M14.3 5.7l1-1\"/>"},
    {"contrast", "<circle cx=\"10\" cy=\"10\" r=\"6.5\"/><path d=\"M10 3.5a6.5 6.5 0 0 1 0 13z\" fill=\"currentColor\"/>"},
    {"rotate", "<path d=\"M4 10a6 6 0 1 0 1.8-4.3\"/><path d=\"M4.5 3v3.5H8\"/>"},
    {"monitor", "<rect x=\"2.5\" y=\"3.5\" width=\"15\" height=\"10\" rx=\"1.5\"/><path d=\"M7 16.5h6M10 13.5v3\"/>"},
    {"apps", "<rect x=\"3\" y=\"3\" width=\"5.5\" height=\"5.5\" rx=\"1.2\"/><rect x=\"11.5\" y=\"3\" width=\"5.5\" height=\"5.5\" rx=\"1.2\"/><rect x=\"3\" y=\"11.5\" width=\"5.5\" height=\"5.5\" rx=\"1.2\"/><rect x=\"11.5\" y=\"11.5\" width=\"5.5\" height=\"5.5\" rx=\"1.2\"/>"},
    {"chevron", "<path d=\"M6 8l4 4 4-4\"/>"},
    {"gear", "<circle cx=\"10\" cy=\"10\" r=\"2.5\"/><path d=\"M8.6 2.6h2.8l.4 2 1.4.8 1.9-.7 1.4 2.4-1.5 1.3v1.6l1.5 1.3-1.4 2.4-1.9-.7-1.4.8-.4 2H8.6l-.4-2-1.4-.8-1.9.7-1.4-2.4L5 10.8V9.2L3.5 7.9l1.4-2.4 1.9.7 1.4-.8z\"/>"},
    {"bell", "<path d=\"M5.5 13.5v-4a4.5 4.5 0 0 1 9 0v4l1.5 1.5h-12z\"/><path d=\"M8.5 17h3\"/>"},
    {"eye", "<path d=\"M2 10s3-5.5 8-5.5S18 10 18 10s-3 5.5-8 5.5S2 10 2 10z\"/><circle cx=\"10\" cy=\"10\" r=\"2.2\"/>"},
    {"power", "<path d=\"M10 2.5v6.5\"/><path d=\"M14.5 5.2a6.5 6.5 0 1 1-9 0\"/>"},
    {"open", "<path d=\"M11 3.5h5.5V9M16.5 3.5L9.5 10.5\"/><path d=\"M14 12v3.5a1 1 0 0 1-1 1H4.5a1 1 0 0 1-1-1V7a1 1 0 0 1 1-1H8\"/>"},
    {"exit", "<path d=\"M5 5l10 10M15 5L5 15\"/>"},
    {"timer", "<circle cx=\"10\" cy=\"11\" r=\"6\"/><path d=\"M10 8v3l2 1.5M8 2.5h4\"/>"},
    {"plus", "<path d=\"M10 4v12M4 10h12\"/>"},
    {"check", "<path d=\"M4 10.5l3.5 3.5L16 6\"/>"},
    {"save", "<path d=\"M4 4h9l3 3v9H4z\"/><path d=\"M7 4v4h5V4M7 16v-5h6v5\"/>"},
    {"pencil", "<path d=\"M13.5 4.5l2 2L8 14l-3 1 1-3z\"/>"},
    {"trash", "<path d=\"M4.5 6h11M8 6V4h4v2M6 6l.8 10h6.4L14 6\"/>"},
    {"keyboard", "<rect x=\"2.5\" y=\"5\" width=\"15\" height=\"10\" rx=\"1.5\"/><path d=\"M5.5 8h1M8.5 8h1M11.5 8h1M14 8h.5M6 12h8\"/>"},
    {"palette", "<path d=\"M10 3a7 7 0 1 0 0 14c1 0 1.5-.8 1.2-1.6-.4-1 .2-1.9 1.3-1.9H14a3 3 0 0 0 3-3C17 6 14 3 10 3z\"/><circle cx=\"6.5\" cy=\"9.5\" r=\"1\"/><circle cx=\"9\" cy=\"6.5\" r=\"1\"/><circle cx=\"12.5\" cy=\"7\" r=\"1\"/>"},
    {"info", "<circle cx=\"10\" cy=\"10\" r=\"7\"/><path d=\"M10 9v4.5M10 6.5v.2\"/>"},
    {"refresh", "<path d=\"M16 10a6 6 0 1 1-1.8-4.3\"/><path d=\"M16 3.5V7h-3.5\"/>"},
    {"identify", "<rect x=\"2.5\" y=\"3.5\" width=\"15\" height=\"10\" rx=\"1.5\"/><path d=\"M9 6.8l1.2-.8v5\"/><path d=\"M7 16.5h6\"/>"},
    {"laptop", "<rect x=\"4\" y=\"4.5\" width=\"12\" height=\"8\" rx=\"1.2\"/><path d=\"M2.5 15.5h15\"/>"},
    {"earbuds", "<path d=\"M6.5 4.5A2.5 2.5 0 0 1 9 7v1a2.5 2.5 0 0 1-2 2.45V15a1 1 0 0 1-2 0V9.2A2.5 2.5 0 0 1 4 7a2.5 2.5 0 0 1 2.5-2.5z\"/><path d=\"M13.5 4.5A2.5 2.5 0 0 0 11 7v1a2.5 2.5 0 0 0 2 2.45V15a1 1 0 0 0 2 0V9.2A2.5 2.5 0 0 0 16 7a2.5 2.5 0 0 0-2.5-2.5z\"/>"},
    {"speakerBox", "<rect x=\"5\" y=\"2.5\" width=\"10\" height=\"15\" rx=\"2\"/><circle cx=\"10\" cy=\"11.5\" r=\"3\"/><circle cx=\"10\" cy=\"6\" r=\".9\" fill=\"currentColor\" stroke=\"none\"/>"},
    {"usb", "<path d=\"M10 3v11\"/><path d=\"M8 5l2-2 2 2\"/><path d=\"M10 11.5L6.5 9.5V7.5M10 10l3.5-2V6.5\"/><circle cx=\"10\" cy=\"15.8\" r=\"1.6\"/>"},
    {"wave", "<path d=\"M2.5 10h2.2l2-5 3 10 3-8 1.8 3h3\"/>"},
    {"bt", "<path d=\"M7 7l6 6-3 3V4l3 3-6 6\"/>"},
    {"mixer", "<path d=\"M5 3v14M10 3v14M15 3v14\"/><circle cx=\"5\" cy=\"12\" r=\"1.8\" fill=\"currentColor\" stroke=\"none\"/><circle cx=\"10\" cy=\"7\" r=\"1.8\" fill=\"currentColor\" stroke=\"none\"/><circle cx=\"15\" cy=\"10.5\" r=\"1.8\" fill=\"currentColor\" stroke=\"none\"/>"},
};
static const struct { const char* n; const char* s; } ICONS_EXTRA[] = {
    {"back", "<path d=\"M12 5l-5 5 5 5\"/>"},
    {"chevRight", "<path d=\"M8 6l4 4-4 4\"/>"},
    {"chevUp", "<path d=\"M6 12l4-4 4 4\"/>"},
    {"mFlyout", "<rect x=\"3\" y=\"11\" width=\"14\" height=\"6\" rx=\"2\"/><path d=\"M6 14h5\"/><path d=\"M3 4.5h14M3 7.5h9\"/>"},
    {"mShuf", "<path d=\"M3 6h3c3 0 5 8 8 8h3\"/><path d=\"M3 14h3c1.3 0 2.4-1.5 3.4-3.2M11.6 9.2C12.6 7.5 13.7 6 15 6h2\"/><path d=\"M15.5 4l2 2-2 2M15.5 12l2 2-2 2\"/>"},
    {"mRep", "<path d=\"M4 9V8a2 2 0 0 1 2-2h10\"/><path d=\"M14 4l2 2-2 2\"/><path d=\"M16 11v1a2 2 0 0 1-2 2H4\"/><path d=\"M6 16l-2-2 2-2\"/>"},
    {"mPlay", "<path d=\"M7 5.2v9.6l7.6-4.8z\" fill=\"currentColor\" stroke=\"none\"/>"},
    {"mPause", "<rect x=\"6\" y=\"5\" width=\"2.6\" height=\"10\" rx=\".8\" fill=\"currentColor\" stroke=\"none\"/><rect x=\"11.4\" y=\"5\" width=\"2.6\" height=\"10\" rx=\".8\" fill=\"currentColor\" stroke=\"none\"/>"},
    {"mPrev", "<path d=\"M5.5 5v10\"/><path d=\"M15 5.4v9.2L8 10z\" fill=\"currentColor\" stroke=\"none\"/>"},
    {"mNext", "<path d=\"M14.5 5v10\"/><path d=\"M5 5.4v9.2L12 10z\" fill=\"currentColor\" stroke=\"none\"/>"},
};
struct IcoPart { GraphicsPath* path; bool fill, stroke; };
static std::map<std::string, std::vector<IcoPart>> g_icoCache;
static std::string IcoAttr(const std::string& tag, const char* name) {
    std::string key = std::string(" ") + name + "=\"";
    size_t a = tag.find(key); if (a == std::string::npos) return "";
    a += key.size(); size_t b = tag.find('"', a);
    return b == std::string::npos ? "" : tag.substr(a, b - a);
}
static float IcoNum(const std::string& s, size_t& i, bool* ok = nullptr) {   // SVG number: "1.2", "-.8", ".4.4" is two numbers
    while (i < s.size() && (s[i] == ' ' || s[i] == ',')) i++;
    size_t st = i; bool dot = false;
    if (i < s.size() && (s[i] == '-' || s[i] == '+')) i++;
    while (i < s.size() && (isdigit((unsigned char)s[i]) || (s[i] == '.' && !dot))) { if (s[i] == '.') dot = true; i++; }
    if (ok) *ok = i > st;
    return i > st ? (float)atof(s.substr(st, i - st).c_str()) : 0.f;
}
static void IcoArc(GraphicsPath& p, float x1, float y1, float rx, float ry, int fa, int fs, float x2, float y2) {
    if (rx <= 0 || ry <= 0 || (x1 == x2 && y1 == y2)) { p.AddLine(x1, y1, x2, y2); return; }
    double xp = (x1 - x2) / 2.0, yp = (y1 - y2) / 2.0;
    double lam = xp * xp / (rx * rx) + yp * yp / (ry * ry);
    if (lam > 1) { rx = (float)(rx * sqrt(lam)); ry = (float)(ry * sqrt(lam)); }
    double num = (double)rx * rx * ry * ry - (double)rx * rx * yp * yp - (double)ry * ry * xp * xp;
    double den = (double)rx * rx * yp * yp + (double)ry * ry * xp * xp;
    double co = (fa != fs ? 1 : -1) * sqrt(std::max(0.0, num / den));
    double cxp = co * rx * yp / ry, cyp = -co * ry * xp / rx;
    double cx = cxp + (x1 + x2) / 2.0, cy = cyp + (y1 + y2) / 2.0;
    double t1 = atan2((yp - cyp) / ry, (xp - cxp) / rx), t2 = atan2((-yp - cyp) / ry, (-xp - cxp) / rx);
    double dt = t2 - t1;
    if (fs && dt < 0) dt += 2 * 3.14159265358979; else if (!fs && dt > 0) dt -= 2 * 3.14159265358979;
    p.AddArc((REAL)(cx - rx), (REAL)(cy - ry), 2 * rx, 2 * ry, (REAL)(t1 * 180 / 3.14159265358979), (REAL)(dt * 180 / 3.14159265358979));
}
static GraphicsPath* IcoParsePath(const std::string& d) {
    GraphicsPath* p = new GraphicsPath();
    float cx = 0, cy = 0, sx = 0, sy = 0, lcx = 0, lcy = 0; char cmd = 0; size_t i = 0; bool open = false;
    auto lineTo = [&](float x, float y) { p->AddLine(cx, cy, x, y); cx = x; cy = y; open = true; };
    while (i < d.size()) {
        while (i < d.size() && (d[i] == ' ' || d[i] == ',')) i++;
        if (i >= d.size()) break;
        if (isalpha((unsigned char)d[i])) cmd = d[i++];
        bool rel = islower((unsigned char)cmd) != 0; char C = (char)toupper((unsigned char)cmd);
        if (C == 'Z') { p->CloseFigure(); cx = sx; cy = sy; open = false; continue; }
        bool ok = true;
        if (C == 'M') {
            float x = IcoNum(d, i, &ok), y = IcoNum(d, i); if (!ok) break;
            if (rel) { x += cx; y += cy; }
            p->StartFigure(); cx = sx = x; cy = sy = y; open = false;
            cmd = rel ? 'l' : 'L';   // extra pairs after a move are lines
        } else if (C == 'L') { float x = IcoNum(d, i, &ok), y = IcoNum(d, i); if (!ok) break; if (rel) { x += cx; y += cy; } lineTo(x, y); }
        else if (C == 'H') { float x = IcoNum(d, i, &ok); if (!ok) break; lineTo(rel ? cx + x : x, cy); }
        else if (C == 'V') { float y = IcoNum(d, i, &ok); if (!ok) break; lineTo(cx, rel ? cy + y : y); }
        else if (C == 'C') {
            float v[6]; for (float& x : v) { x = IcoNum(d, i, &ok); if (!ok) break; } if (!ok) break;
            if (rel) for (int k = 0; k < 6; k++) v[k] += k % 2 ? cy : cx;
            p->AddBezier(cx, cy, v[0], v[1], v[2], v[3], v[4], v[5]); lcx = v[2]; lcy = v[3]; cx = v[4]; cy = v[5]; open = true; continue;
        } else if (C == 'S') {   // smooth curve: first control point mirrors the previous one
            float v[4]; for (float& x : v) { x = IcoNum(d, i, &ok); if (!ok) break; } if (!ok) break;
            if (rel) for (int k = 0; k < 4; k++) v[k] += k % 2 ? cy : cx;
            float c1x = 2 * cx - lcx, c1y = 2 * cy - lcy;
            p->AddBezier(cx, cy, c1x, c1y, v[0], v[1], v[2], v[3]); lcx = v[0]; lcy = v[1]; cx = v[2]; cy = v[3]; open = true; continue;
        } else if (C == 'A') {
            float rx = IcoNum(d, i, &ok); if (!ok) break; float ry = IcoNum(d, i); IcoNum(d, i);
            int fa = (int)IcoNum(d, i), fs = (int)IcoNum(d, i); float x = IcoNum(d, i), y = IcoNum(d, i);
            if (rel) { x += cx; y += cy; }
            IcoArc(*p, cx, cy, rx, ry, fa, fs, x, y); cx = x; cy = y; open = true;
        } else { i++; }
        lcx = cx; lcy = cy;
    }
    (void)open;
    return p;
}
static const std::vector<IcoPart>& IcoGet(const char* name) {
    auto it = g_icoCache.find(name);
    if (it != g_icoCache.end()) return it->second;
    const char* src = nullptr;
    for (auto& x : ICONS) if (!strcmp(x.n, name)) src = x.s;
    for (auto& x : ICONS_EXTRA) if (!strcmp(x.n, name)) src = x.s;
    std::vector<IcoPart> parts;
    std::string s = src ? src : "";
    size_t a = 0;
    while ((a = s.find('<', a)) != std::string::npos) {
        size_t b = s.find('>', a); if (b == std::string::npos) break;
        std::string tag = s.substr(a, b - a + 1); a = b + 1;
        IcoPart pt{nullptr, IcoAttr(tag, "fill") == "currentColor", IcoAttr(tag, "stroke") != "none"};
        if (!tag.compare(0, 5, "<path")) pt.path = IcoParsePath(IcoAttr(tag, "d"));
        else if (!tag.compare(0, 7, "<circle")) {
            float x = (float)atof(IcoAttr(tag, "cx").c_str()), y = (float)atof(IcoAttr(tag, "cy").c_str()), r = (float)atof(IcoAttr(tag, "r").c_str());
            pt.path = new GraphicsPath(); pt.path->AddEllipse(x - r, y - r, 2 * r, 2 * r);
        } else if (!tag.compare(0, 5, "<rect")) {
            float x = (float)atof(IcoAttr(tag, "x").c_str()), y = (float)atof(IcoAttr(tag, "y").c_str());
            float w = (float)atof(IcoAttr(tag, "width").c_str()), h = (float)atof(IcoAttr(tag, "height").c_str()), r = (float)atof(IcoAttr(tag, "rx").c_str());
            pt.path = new GraphicsPath();
            if (r <= 0) pt.path->AddRectangle(RectF(x, y, w, h));
            else { float d2 = 2 * r; pt.path->AddArc(x, y, d2, d2, 180, 90); pt.path->AddArc(x + w - d2, y, d2, d2, 270, 90);
                   pt.path->AddArc(x + w - d2, y + h - d2, d2, d2, 0, 90); pt.path->AddArc(x, y + h - d2, d2, d2, 90, 90); pt.path->CloseFigure(); }
        }
        if (pt.path) parts.push_back(pt);
    }
    return g_icoCache[name] = parts;
}
// draws a design icon centred on (cx, cy); size in pixels (already scaled), like the design's ic(name, size)
static void IconVec(Graphics& g, const char* name, float size, Color c, float cx, float cy, float rot) {
    const auto& parts = IcoGet(name);
    float k = size / 20.f;
    Matrix m; m.Translate(cx, cy); if (rot) m.Rotate(rot); m.Scale(k, k); m.Translate(-10, -10);   // points go to pixels first, so curves stay smooth
    Pen pen(c, 1.5f * k); pen.SetStartCap(LineCapRound); pen.SetEndCap(LineCapRound); pen.SetLineJoin(LineJoinRound);
    SolidBrush br(c);
    for (auto& p : parts) {
        GraphicsPath* q = p.path->Clone(); q->Transform(&m);
        if (p.fill) g.FillPath(&br, q);
        if (p.stroke) g.DrawPath(&pen, q);
        delete q;
    }
}
// Icons are drawn 4x larger and averaged down, so thin strokes look the same as in a browser; results are cached.
static std::map<std::string, Bitmap*> g_icoBmp;
static void Icon(Graphics& g, const char* name, float size, Color c, float cx, float cy, float rot = 0) {
    int box = (int)ceilf(size) + 4; const int SS = 4;
    char key[160]; snprintf(key, sizeof key, "%s|%.2f|%08x|%.0f", name, size, (unsigned)c.GetValue(), rot);
    Bitmap*& bm = g_icoBmp[key];
    if (!bm) {
        if (g_icoBmp.size() > 600) { for (auto& x : g_icoBmp) if (x.second && x.second != bm) delete x.second; g_icoBmp.clear(); return Icon(g, name, size, c, cx, cy, rot); }
        Bitmap big(box * SS, box * SS, PixelFormat32bppPARGB);
        { Graphics bg(&big); bg.Clear(Color(0, 0, 0, 0)); bg.SetSmoothingMode(SmoothingModeAntiAlias); bg.SetPixelOffsetMode(PixelOffsetModeHalf);
          IconVec(bg, name, size * SS, Color(255, c.GetR(), c.GetG(), c.GetB()), box * SS / 2.f, box * SS / 2.f, rot); }
        bm = new Bitmap(box, box, PixelFormat32bppPARGB);
        BitmapData sd, dd; Rect rs(0, 0, box * SS, box * SS), rd(0, 0, box, box);
        big.LockBits(&rs, ImageLockModeRead, PixelFormat32bppPARGB, &sd); bm->LockBits(&rd, ImageLockModeWrite, PixelFormat32bppPARGB, &dd);
        for (int y = 0; y < box; y++) for (int x = 0; x < box; x++) {
            unsigned a = 0, r = 0, gg = 0, b = 0;
            for (int j = 0; j < SS; j++) { const BYTE* p = (const BYTE*)sd.Scan0 + (y * SS + j) * sd.Stride + x * SS * 4;
                for (int i = 0; i < SS; i++, p += 4) { b += p[0]; gg += p[1]; r += p[2]; a += p[3]; } }
            const unsigned n = SS * SS, al = c.GetA();
            BYTE* q = (BYTE*)dd.Scan0 + y * dd.Stride + x * 4;
            q[0] = (BYTE)(b / n * al / 255); q[1] = (BYTE)(gg / n * al / 255); q[2] = (BYTE)(r / n * al / 255); q[3] = (BYTE)(a / n * al / 255);
        }
        big.UnlockBits(&sd); bm->UnlockBits(&dd);
    }
    float x = floorf(cx - box / 2.f + 0.5f), y = floorf(cy - box / 2.f + 0.5f);
    InterpolationMode im = g.GetInterpolationMode(); PixelOffsetMode po = g.GetPixelOffsetMode();
    Matrix wm; g.GetTransform(&wm);
    g.SetInterpolationMode(wm.IsIdentity() ? InterpolationModeNearestNeighbor : InterpolationModeHighQualityBilinear); g.SetPixelOffsetMode(PixelOffsetModeHalf);
    g.DrawImage(bm, x, y, (REAL)box, (REAL)box);
    g.SetInterpolationMode(im); g.SetPixelOffsetMode(po);
}
static const char* GlyphIcon(wchar_t ch) {   // the old Segoe glyph codes, mapped onto the design's icons
    switch (ch) {
    case 0xE767: return "speaker"; case 0xE74F: return "speakerOff"; case 0xE720: return "mic"; case 0xEC54: return "micOff";
    case 0xE7F5: return "speakerBox"; case 0xE7F6: return "headphones"; case 0xE7F4: return "monitor"; case 0xE706: return "sun";
    case 0xE793: return "contrast"; case 0xE7AD: return "rotate"; case 0xE71D: return "apps"; case 0xE70D: return "chevron";
    case 0xE70E: return "chevUp"; case 0xE713: return "gear"; case 0xEA8F: return "bell"; case 0xE9E9: return "mixer";
    case 0xE72C: return "refresh"; case 0xE890: return "eye"; case 0xE7E8: return "power"; case 0xE711: return "exit";
    case 0xE8A7: return "open"; case 0xE916: return "timer"; case 0xE74E: return "save"; case 0xE708: return "moon";
    case 0xEC8A: return "sunLow"; case 0xE73E: return "check"; case 0xE710: return "plus"; case 0xE76C: return "chevRight";
    case 0xE72B: return "back"; case 0xE70F: return "pencil"; case 0xE74D: return "trash"; case 0xE765: return "keyboard";
    case 0xE790: return "palette"; case 0xE946: return "info";
    }
    return nullptr;
}
static void Glyph(Graphics& g, wchar_t ch, Font* f, Color c, float cx, float cy) {
    if (const char* n = GlyphIcon(ch)) { Icon(g, n, f->GetSize(), c, cx, cy); return; }
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

static void DrawSlider(Graphics& g, RectF r, float v, bool dim, int state /*0 rest,1 hover,2 drag*/, bool win11 = false) {   // win11: the Settings window's slider
    v = std::max(0.f, std::min(1.f, v));
    float cy = r.Y + r.Height / 2;
    float fx = r.X + r.Width * v;
    Color ac = dim ? T.t3 : T.accent;
    if (!win11) {   // thin rail + white handle with a soft accent halo
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
    FillRound(g, T.valBg, r, S(8)); PillText(g, pct, g_f11, T.t2, r, StringAlignmentCenter);
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

static const char* DevIconName(int icon) {
    switch (icon) { case DI_LAPTOP: return "laptop"; case DI_DISPLAY: return "monitor"; case DI_HEADPHONES: return "headphones"; case DI_EARBUDS: return "earbuds";
    case DI_USB: return "usb"; case DI_VIRTUAL: return "wave"; case DI_MIC: return "mic"; default: return "speakerBox"; }
}
static void DrawDevIcon(Graphics& g, int icon, float cx, float cy, Color c, float size = 18) { Icon(g, DevIconName(icon), S(size), c, cx, cy); }
static wchar_t KindGlyph(int k) { return k == DK_HEADPHONES ? G_HEADPHONE : k == DK_MIC ? G_MIC : k == DK_DISPLAY ? G_TV : G_SPEAKERS; }

enum { TIMER_POP = 12 };
static bool g_animating = false;
static void KickAnim() { if (g_hwnd) SetTimer(g_hwnd, TIMER_POP, 15, nullptr); }
static void DrawEl(Graphics& g, int index) {
    El& e = g_els[index];
    if (e.r.Y + e.r.Height < PaneTop() || e.r.Y > PaneBottom()) return;
    bool hovEl = g_hover.el == index;
    if (e.k == E_H3) {
        SpacedText(g, e.text.c_str(), g_fH3, T.t3, e.r.X, e.r.Y + e.r.Height / 2, S(1));
        if (e.text == L"APPS") {   // put every app back with everything else; always there so nothing jumps, faded when there's nothing to reset
            RectF r = ResetOutRect(e); bool can = AnyAppRouted(), hov = g_hover.h == H_RESETOUT && can;
            if (hov) FillRound(g, T.subtle, r, S(6));
            Text(g, L"Reset outputs", g_fSmall, can ? T.accent : WithAlpha(T.accent, 89), r, StringAlignmentCenter);
        }
        if (e.text == L"OUTPUT") {   // how many speakers play, the way to Settings › Sound, and Auto sync
            RectF rr = RoutingRect(e);
            if (g_hover.h == H_ROUTING) FillRound(g, T.subtle, rr, S(6));
            Text(g, L"Routing", g_fSmall, T.accent, rr, StringAlignmentCenter);
            if (ShowSyncBtns()) {
                RectF op = OneRect(e); bool hov = g_hover.h == H_ONESPK;   // "3 speakers ×": click to go back to one speaker
                FillRound(g, hov ? T.subtle3 : T.subtle2, op, op.Height / 2);
                PillText(g, OneLabel().c_str(), g_f11, T.t1, RectF(op.X + S(10), op.Y, op.Width - S(10), op.Height));
                Icon(g, "exit", S(11), WithAlpha(T.t1, 179), op.X + op.Width - S(9) - S(5.5f), op.Y + op.Height / 2);
            }
        }
        return;
    }
    if (e.k == E_NOTE) {
        StringFormat sf; sf.SetLineAlignment(StringAlignmentNear); SolidBrush b(T.t3);
        g.DrawString(e.text.c_str(), -1, g_fSmall, e.r, &sf, &b); return;
    }

    if (IsRowKind(e.k)) {
        bool sel = (e.k == E_OUT && OutPlays(e.idx)) || (e.k == E_IN && g_in[e.idx].isDefault);
        bool playing = e.k == E_APP && AppPlaying(g_apps[e.idx]) && !g_apps[e.idx].mute;
        bool pickable = e.k == E_OUT || e.k == E_IN;
        bool rowHover = hovEl && g_hover.h == H_ROW;
        float rr = S(12);
        // full-row, flat highlights (the design: selected = a light accent wash, playing = a faint one)
        if (e.k == E_MASTER) FillRound(g, T.master, e.r, rr);
        else if (sel) FillRound(g, T.selRow[rowHover ? 1 : 0], e.r, rr);
        else if (playing) FillRound(g, WithAlpha(T.accent, 10), e.r, rr);
        else if (pickable && rowHover) FillRound(g, T.hover, e.r, rr);

        float cy1 = RowCy1(e), cy2 = RowCy2(e), lx = e.r.X + S(10) + S(16);
        std::wstring name, detail; wchar_t muteG = G_VOLUME;
        bool muted = ElMuted(e), calDetail = false;
        if (e.k == E_APP) {
            AppGroup& a = g_apps[e.idx];
            name = a.name;
            Gdiplus::Bitmap* icn = a.path.empty() ? nullptr : GetProcInfo(a.path).icon;
            float isz = S(24);
            if (a.system) { FillRound(g, T.badge, RectF(lx - S(11), cy1 - S(11), S(22), S(22)), S(10)); Icon(g, "bell", S(14), T.t1, lx, cy1); }
            else if (icn) DrawAppIcon(g, icn, RectF(lx - isz / 2, cy1 - isz / 2, isz, isz));
            else {   // no icon: a tile with the app's first letter, like the design
                static const DWORD tint[6] = {0xd9532b, 0x5865f2, 0x1a9f48, 0x2a7de1, 0xe07a10, 0x8e44ad};
                unsigned hsh = 0; for (wchar_t ch : a.name) hsh = hsh * 31 + ch;
                RectF tr(lx - S(12), cy1 - S(12), S(24), S(24)); FillRound(g, Hex(tint[hsh % 6]), tr, S(7));
                std::wstring l = a.name.substr(0, 1); Text(g, l.c_str(), g_fSmall600, Color(255, 255, 255, 255), tr, StringAlignmentCenter);
            }
            muteG = muted ? G_MUTE : G_VOLUME;
        } else {
            wchar_t lg = G_VOLUME; int devIcon = -1; bool devBt = false;
            if (e.k == E_MASTER) { name = L"Master volume"; muteG = muted ? G_MUTE : G_VOLUME; }
            else {
                Endpoint& d = (e.k == E_OUT ? g_out : g_in)[e.idx];
                name = d.name; detail = d.detail; lg = KindGlyph(d.kind); devIcon = d.icon; devBt = d.bt;
                muteG = e.k == E_IN ? (muted ? G_MICOFF : G_MIC) : (muted ? G_MUTE : G_VOLUME);
            }
            if (e.k == E_OUT && g_calibrating) {   // Auto sync: which speaker is chirping, and what each one measured
                const std::wstring& oid = g_out[e.idx].id;
                int pos = (int)(std::find(g_calIds.begin(), g_calIds.end(), oid) - g_calIds.begin());
                if (pos < (int)g_calIds.size() && pos == g_calK && !g_calHeard.count(oid)) {
                    detail = L"Chirp\u2026"; calDetail = true;
                    float t = (GetTickCount() % 900) / 900.f, grow = S(4) + t * S(4);
                    Pen rp(WithAlpha(T.accent, (int)(255 * (1 - t))), S(2)); GraphicsPath gp; AddRound(gp, RectF(lx - S(16) - grow, cy1 - S(16) - grow, S(32) + 2 * grow, S(32) + 2 * grow), S(13)); g.DrawPath(&rp, &gp);
                    g_animating = true; KickAnim();
                } else if (g_calHeard.count(oid)) { detail = L"\u2713 " + std::to_wstring(g_calHeard[oid]) + L" ms"; calDetail = true; }
            }
            FillRound(g, sel ? T.accent : T.badge, RectF(lx - S(16), cy1 - S(16), S(32), S(32)), S(10));   // rounded-square badge
            if (devIcon >= 0 && devIcon != DI_MIC) DrawDevIcon(g, devIcon, lx, cy1, sel ? T.onAccent : T.t1);
            else Glyph(g, lg, g_fIcon, sel ? T.onAccent : T.t1, lx, cy1);
            if (devBt) {   // Bluetooth: one clear blue circle on the corner of the icon
                float bx = lx + S(13.5f), by = cy1 + S(13.5f), br = S(8.5f);
                SolidBrush bb(Color(255, 31, 111, 235)); g.FillEllipse(&bb, bx - br, by - br, 2 * br, 2 * br);
                Icon(g, "bt", S(11), Color(255, 255, 255, 255), bx, by);
            }
        }
        float nx = e.r.X + S(52), maxW = e.r.X + e.r.Width - S(8) - nx;
        if (e.k == E_APP && !g_apps[e.idx].system) {   // where it plays: always shown, like Windows' volume mixer
            auto routes = AppRoutes(g_apps[e.idx]);
            bool on = !routes.empty();
            std::wstring lbl = on ? OutName(routes[0]) + (routes.size() > 1 ? L" + " + std::to_wstring(routes.size() - 1) : L"") : L"Default speaker";
            g_appChipW[e.idx] = std::min(S(170), S(9) + S(13) + S(7) + TextW(g, lbl.c_str(), g_f11) + S(11));
            RectF c = AppOutChip(e);
            maxW = c.X - S(8) - nx;
            bool hov = hovEl && g_hover.h == H_APPOUT, open = g_ddOpen == e.idx && g_ddKind == 4;
            FillRound(g, hov || open ? T.subtle2 : T.subtle, c, c.Height / 2);
            Icon(g, "speaker", S(13), on ? T.accent : (hov ? T.t1 : T.t3), c.X + S(9) + S(6.5f), c.Y + c.Height / 2);
            PillText(g, lbl.c_str(), g_f11, on || hov ? T.t1 : T.t3, RectF(c.X + S(29), c.Y, c.Width - S(29) - S(9), c.Height));
        }
        if (e.k == E_OUT && OutHasApps(e.idx)) {   // which apps play here: icons only, each covering a fifth of the next
            float ix = nx, iy = cy1 + S(15), isz = S(18);
            std::vector<AppGroup*> here;
            for (auto& a : g_apps) { if (a.system) continue; auto r = AppRoutes(a); if (std::find(r.begin(), r.end(), g_out[e.idx].id) != r.end()) here.push_back(&a); }
            for (int k = (int)here.size() - 1; k >= 0; k--) {   // right to left, so each icon sits on top of the one after it
                RectF ir(ix + k * (isz - S(4)), iy, isz, isz);
                Gdiplus::Bitmap* icn = here[k]->path.empty() ? nullptr : GetProcInfo(here[k]->path).icon;
                if (!icn) FillRound(g, Color(T.dark ? 100 : 56, 0, 0, 0), RectF(ir.X + S(1.5f), ir.Y, ir.Width, ir.Height), S(5));   // small shadow, only behind the solid letter tiles (it showed as a dark box behind real icons)
                if (icn) DrawAppIcon(g, icn, ir);
                else {
                    static const DWORD tint[6] = {0xd9532b, 0x5865f2, 0x1a9f48, 0x2a7de1, 0xe07a10, 0x8e44ad};
                    unsigned hsh = 0; for (wchar_t ch : here[k]->name) hsh = hsh * 31 + ch;
                    FillRound(g, Hex(tint[hsh % 6]), ir, S(5)); std::wstring l = here[k]->name.substr(0, 1); Text(g, l.c_str(), g_fNum, Color(255, 255, 255, 255), ir, StringAlignmentCenter);
                }
            }
        }
        if (e.k == E_OUT && HasDelayChip(e.idx)) {
            wchar_t t[32]; swprintf(t, 32, L"+%d ms", DelayOf(e.idx));
            g_delayChipW = S(9) + S(14) + S(7) + TextW(g, t, g_f11s) + S(11);
            RectF c = DelayChip(e);
            maxW = c.X - S(8) - nx;
            bool hov = hovEl && g_hover.h == H_DELAYCHIP, open = g_delayOpen == e.idx;
            FillRound(g, hov || open ? T.subtle3 : T.subtle2, c, c.Height / 2);   // neutral pill; only the icon and time in the accent
            Icon(g, "timer", S(14), T.accent, c.X + S(9) + S(7), c.Y + c.Height / 2);
            PillText(g, t, g_f11s, T.accent, RectF(c.X + S(30), c.Y, c.Width - S(30), c.Height));
        }
        Font* nf = e.k == E_MASTER ? g_fName600 : g_fName;
        float nw = std::min(TextW(g, name.c_str(), nf), maxW * (detail.empty() ? 1.f : 0.7f));
        Text(g, name.c_str(), nf, T.t1, RectF(nx, cy1 - S(10), nw + S(2), S(20)));
        float after = nx + nw + S(10);
        if (!detail.empty()) Text(g, detail.c_str(), g_fSmall, calDetail ? T.accent : T.t3, RectF(after, cy1 - S(9), std::max(0.f, maxW - nw - S(10)), S(18)));
        if (e.k == E_OUT && g_out[e.idx].battery >= 0 && !calDetail) {   // Bluetooth battery: a small battery and the percent, after the name
            int pc = g_out[e.idx].battery;
            float dw = detail.empty() ? 0.f : std::min(TextW(g, detail.c_str(), g_fSmall), std::max(0.f, maxW - nw - S(10))) + S(9);
            std::wstring pt = std::to_wstring(pc) + L"%";
            float bx0 = after + dw, bw = S(15), bh = S(8), by0 = cy1 - bh / 2;
            if (bx0 + bw + S(4) + TextW(g, pt.c_str(), g_fSmall) <= e.r.X + e.r.Width - S(8)) {
                Color bc = pc <= 15 ? Color(255, 226, 74, 63) : T.t3;
                Pen bp(bc, S(1.2f)); GraphicsPath bpth; AddRound(bpth, RectF(bx0, by0, bw, bh), S(2.2f)); g.DrawPath(&bp, &bpth);
                SolidBrush tip(bc); g.FillRectangle(&tip, bx0 + bw + S(1), cy1 - S(1.8f), S(1.5f), S(3.6f));
                float fw = std::max(S(1.5f), (bw - S(3.2f)) * pc / 100.f); FillRound(g, bc, RectF(bx0 + S(1.6f), by0 + S(1.6f), fw, bh - S(3.2f)), S(1));
                Text(g, pt.c_str(), g_fSmall, bc, RectF(bx0 + bw + S(5), cy1 - S(9), S(40), S(18)));
            }
        }
        if (playing) {   // live bars driven by the app's real peak level
            float lvl = std::min(1.f, g_live[g_apps[e.idx].key].smooth * 4.f);
            DWORD t = GetTickCount();
            float bx = nx + nw - S(2) + S(8);
            for (int b = 0; b < 3; b++) {
                float wob = 0.5f + 0.5f * sinf(t / (110.f + b * 35.f) + b * 2.1f);
                float amp = 0.45f + 0.55f * lvl;
                float hgt = S(3) + (S(10) - S(3)) * (0.15f + 0.85f * wob * amp);
                FillRound(g, T.accent, RectF(bx + b * S(4.5f), cy1 + S(5) - hgt, S(2.5f), hgt), S(1));
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

    if (e.k == E_DTOP) {
        RectF pb = DTopProfile(e), ob = DTopOff(e);
        bool open = g_ddOpen >= 0 && g_ddKind == 3;
        FillRound(g, open || (hovEl && g_hover.h == H_PROFDD) ? T.ddBtnHover : T.ddBtn, pb, S(9));
        int act = -1; for (int i = 0; i < (int)g_prof.size(); i++) if (ProfileActive(i)) { act = i; break; }
        Icon(g, "save", S(13), T.t2, pb.X + S(11) + S(6.5f), pb.Y + pb.Height / 2);
        PillText(g, act >= 0 ? g_prof[act].name.c_str() : L"Profile", g_fName, T.t1, RectF(pb.X + S(32), pb.Y, pb.Width - S(60), pb.Height));
        Icon(g, "chevron", S(14), T.t1, pb.X + pb.Width - S(19), pb.Y + pb.Height / 2, open ? 180.f : 0.f);
        bool ho = hovEl && g_hover.h == H_SCREENOFF, hp = hovEl && g_hover.h == H_PARTY;
        FillRound(g, ho ? T.subtle3 : T.subtle2, ob, S(8));
        if (g_offAt) {   // counting down: click to cancel
            Icon(g, "timer", S(14), T.accent, ob.X + S(12) + S(7), ob.Y + ob.Height / 2);
            std::wstring t = OffLeft();
            PillText(g, t.c_str(), g_fSmall600, T.accent, RectF(ob.X + S(32), ob.Y, ob.Width - S(32) - S(20), ob.Height));
            Icon(g, "exit", S(9), ho ? T.t1 : T.t3, ob.X + ob.Width - S(12) - S(5), ob.Y + ob.Height / 2);
        } else {
            Icon(g, "power", S(14), ho ? T.t1 : T.t2, ob.X + S(12) + S(7), ob.Y + ob.Height / 2);
            PillText(g, L"Turn off", g_fSmall, ho ? T.t1 : T.t2, RectF(ob.X + S(32), ob.Y, ob.Width - S(32), ob.Height));
        }
        RectF pr = DTopParty(e);
        FillRound(g, hp ? T.subtle3 : T.subtle2, pr, S(8));
        Icon(g, "wave", S(14), hp ? T.t1 : T.t2, pr.X + S(12) + S(7), pr.Y + pr.Height / 2);
        PillText(g, L"Party", g_fSmall, hp ? T.t1 : T.t2, RectF(pr.X + S(32), pr.Y, pr.Width - S(32), pr.Height));
        return;
    }
    if (e.k == E_MONOFF) {   // a screen that is off: dimmed number and name, then the ways to turn it back on
        const OffScreen& o = g_offScreens[e.idx];
        float cy0 = e.r.Y + e.r.Height / 2, lx0 = e.r.X + S(8) + S(16);
        RectF nb(lx0 - S(10), cy0 - S(7.5f), S(20), S(15));
        GraphicsPath p; AddRound(p, RectF(nb.X + S(0.75f), nb.Y + S(0.75f), nb.Width - S(1.5f), nb.Height - S(1.5f)), S(3.5f));
        Pen pen(T.t3, S(1.5f)); g.DrawPath(&pen, &p);
        wchar_t num[4]; swprintf(num, 4, L"%d", (int)g_mons.size() + e.idx + 1);
        Text(g, num, g_fNum, T.t3, nb, StringAlignmentCenter);
        auto btn = [&](RectF b, const wchar_t* label, bool hov) { FillRound(g, hov ? T.ddBtnHover : T.ddBtn, b, S(9)); PillText(g, label, g_fSmall, T.t1, b, StringAlignmentCenter); };
        btn(OffDup(e), L"Duplicate", hovEl && g_hover.h == H_OFFDUP);
        btn(OffExt(e), L"Extend", hovEl && g_hover.h == H_OFFEXT);
        float nx = e.r.X + S(50), maxW = OffDup(e).X - S(8) - nx;
        std::wstring nm = o.name + L"  \u00B7  off";
        Text(g, nm.c_str(), g_fName, T.t3, RectF(nx, cy0 - S(10), maxW, S(20)));
        return;
    }
    MonUI& m = g_mons[e.idx];
    float cy = e.r.Y + e.r.Height / 2, lx = e.r.X + S(8) + S(16);
    if (e.k == E_MONHEAD) {
        RectF nb(lx - S(10), cy - S(7.5f), S(20), S(15));
        GraphicsPath p; AddRound(p, RectF(nb.X + S(0.75f), nb.Y + S(0.75f), nb.Width - S(1.5f), nb.Height - S(1.5f)), S(3.5f));
        Pen pen(T.t1, S(1.5f)); g.DrawPath(&pen, &p);
        wchar_t num[4]; swprintf(num, 4, L"%d", e.idx + 1);
        Text(g, num, g_fNum, T.t1, nb, StringAlignmentCenter);
        float nx = e.r.X + S(50);
        auto pill = [&](RectF r, bool on, bool hov) {   // small outlined pill; filled with the accent when on
            if (on) FillRound(g, T.accent, r, r.Height / 2);
            else { if (hov) FillRound(g, T.subtle, r, r.Height / 2); Pen op(WithAlpha(T.t1, 64), S(1)); GraphicsPath gp; AddRound(gp, RectF(r.X + S(0.5f), r.Y + S(0.5f), r.Width - S(1), r.Height - S(1)), r.Height / 2 - S(0.5f)); g.DrawPath(&op, &gp); }
        };
        bool nOn = g_night.count(m.name) && g_night[m.name].on;
        RectF mp = MoonPill(e); bool mh = hovEl && g_hover.h == H_NIGHT; pill(mp, nOn, false);
        Icon(g, "moon", S(12), nOn ? T.onAccent : mh ? T.t1 : T.t3, mp.X + mp.Width / 2, mp.Y + mp.Height / 2);
        float left = mp.X;
        if (m.hdrOk) {
            RectF hp = HdrPill(e); bool hh = hovEl && g_hover.h == H_HDR; pill(hp, m.hdr, false);
            Text(g, L"HDR", g_fH3, m.hdr ? T.onAccent : hh ? T.t1 : T.t3, RectF(hp.X, hp.Y, hp.Width, hp.Height), StringAlignmentCenter);   // centred, equal gaps
            left = hp.X;
        }
        if (HasRates(e.idx)) {
            RectF rp = RateBtn(e); bool open = g_ddOpen == e.idx && g_ddKind == 1, rh = open || (hovEl && g_hover.h == H_RATEDD);
            pill(rp, false, rh);
            std::wstring r = std::to_wstring(m.rate) + L" Hz";
            PillText(g, r.c_str(), g_f11s, rh ? T.t1 : T.t2, RectF(rp.X + S(9), rp.Y, rp.Width - S(9), rp.Height));
            Icon(g, "chevron", S(11), rh ? T.t1 : T.t2, rp.X + rp.Width - S(7) - S(5.5f), rp.Y + rp.Height / 2, open ? 180.f : 0.f);
            left = rp.X;
        }
        float maxW = left - S(8) - nx;
        Text(g, m.name.c_str(), g_fName, T.t1, RectF(nx, cy - S(10), maxW, S(20)));   // the connection is on the input button below
        return;
    }
    if (e.k == E_MONO) {   // orientation · input · extend / duplicate, across the whole row
        auto ddBtn = [&](RectF b, const std::wstring& label, bool open, bool hov, int badge, bool input) {
            float bcy = b.Y + b.Height / 2;
            FillRound(g, hov || open ? T.ddBtnHover : input ? T.subtle2 : T.ddBtn, b, S(9));
            float tx = b.X + S(9);
            if (badge > 0) {   // the screen it copies, by its number
                RectF nb(tx, bcy - S(8), S(18), S(16)); GraphicsPath gp; AddRound(gp, RectF(nb.X + S(0.75f), nb.Y + S(0.75f), nb.Width - S(1.5f), nb.Height - S(1.5f)), S(3.5f));
                Pen pp(WithAlpha(T.t1, 217), S(1.5f)); g.DrawPath(&pp, &gp);
                wchar_t num[4]; swprintf(num, 4, L"%d", badge); Text(g, num, g_fNum, WithAlpha(T.t1, 217), nb, StringAlignmentCenter); tx += S(26);
            }
            PillText(g, label.c_str(), g_fSmall, T.t1, RectF(tx, b.Y, b.X + b.Width - S(26) - tx, b.Height));
            Icon(g, "chevron", S(input ? 12 : 14), T.t1, b.X + b.Width - S(14), bcy, open ? 180.f : 0.f);
        };
        int o = std::max(0, std::min(3, m.orient));
        ddBtn(OrientBtn(e), ORIENT_NAMES[o], g_ddOpen == e.idx && g_ddKind == 0, hovEl && g_hover.h == H_DROPDOWN, 0, false);
        if (HasInputs(e.idx)) ddBtn(InputBtn(e), m.input >= 0 ? InputName(m.input, m.inputs) : L"Input", g_ddOpen == e.idx && g_ddKind == 2, hovEl && g_hover.h == H_INPUTDD, 0, true);
        if (HasMode(e.idx)) {
            bool dup = m.cloneOf >= 0 && m.cloneOf < (int)g_mons.size(), only = g_mons.size() == 1 && g_connScreens > 1;
            ddBtn(MonoBtn(e, 2), dup ? g_mons[m.cloneOf].name : only ? L"Only this screen" : L"Extend", g_ddOpen == e.idx && g_ddKind == 5, hovEl && g_hover.h == H_MODEDD, dup ? m.cloneOf + 1 : 0, false);
        }
        return;
    }
    float icx = e.r.X + S(6) + S(16);
    if (e.k == E_MONC) Icon(g, "contrast", S(18), T.t1, icx, cy);
    else if (e.k == E_MONDIM) Icon(g, "sunLow", S(18), T.t1, icx, cy);              // dim further
    else if (e.k == E_MONNL) Icon(g, "moon", S(18), Color(255, 255, 154, 60), icx, cy);   // night light strength, warm
    else Icon(g, "sun", S(18), T.t1, icx, cy);
    RectF sl = CtlSlider(e, cy);
    if (e.k == E_MONDIM || e.k == E_MONNL) {   // Knob's own controls: always available
        float v = ElValue(e);
        int st = g_dragEl == index ? 2 : (hovEl && g_hover.h == H_SLIDER ? 1 : 0);
        DrawSlider(g, sl, v, false, st);
        RectF vr = CtlVal(e, cy);
        if (e.k == E_MONDIM) {   // "Off", or how much darker, in the accent
            wchar_t t[8]; int dv = (int)lround(v * 80); if (dv > 0) swprintf(t, 8, L"%d", dv); else wcscpy(t, L"Off");
            FillRound(g, T.valBg, vr, S(8)); PillText(g, t, g_f11, dv > 0 ? T.accent : T.t2, vr, StringAlignmentCenter);
        } else DrawValue(g, vr, (int)lround(v * 100));
        return;
    }
    if (!m.ok) { Text(g, L"Not available — turn on DDC/CI in the monitor menu", g_fSmall, T.t3, RectF(sl.X - S(8), sl.Y, sl.Width + S(46), sl.Height)); return; }
    float v = ElValue(e);
    int st = g_dragEl == index ? 2 : (hovEl && g_hover.h == H_SLIDER ? 1 : 0);
    DrawSlider(g, sl, v, false, st);
    DrawValue(g, CtlVal(e, cy), (int)lround(v * 100));
}

// ---- small motion: popups open with a short fade + slide, the tab pill glides between tabs
static const DWORD POP_MS = 170, TAB_MS = 240;
struct PopAnim { long long key = LLONG_MIN; DWORD start = 0; Bitmap* snap = nullptr; RectF box; float m = 0; bool up = false, closing = false; DWORD closeStart = 0; };
static const DWORD CLOSE_MS = 130;
static PopAnim g_paDD, g_paDelay, g_paSetDD;
static DWORD g_tabStart = 0; static float g_tabFrom = 0, g_tabTo = 0;

static float PopT(PopAnim& a, long long key) {
    if (key != a.key) { a.key = key; a.start = GetTickCount(); a.closing = false; KickAnim(); }
    float t = (GetTickCount() - a.start) / (float)POP_MS;
    if (t < 1) { g_animating = true; return t; }
    return 1.f;
}
static float TabPos() {
    if (!g_tabStart) return (float)g_tab;
    float t = (GetTickCount() - g_tabStart) / (float)TAB_MS;
    if (t >= 1) return g_tabTo;
    g_animating = true;
    float e = t < 0.5f ? 4 * t * t * t : 1 - powf(-2 * t + 2, 3) / 2;   // ease in-out
    return g_tabFrom + (g_tabTo - g_tabFrom) * e;
}
static void BlitPop(Graphics& g, Bitmap* bmp, RectF box, float m, bool opensUp, float e) {
    float bw = (float)bmp->GetWidth(), bh = (float)bmp->GetHeight();
    ColorMatrix cm = {{{1, 0, 0, 0, 0}, {0, 1, 0, 0, 0}, {0, 0, 1, 0, 0}, {0, 0, 0, e, 0}, {0, 0, 0, 0, 1}}};
    ImageAttributes ia; ia.SetColorMatrix(&cm);
    float sc = 0.95f + 0.05f * e, slide = (1 - e) * S(8) * (opensUp ? 1 : -1);
    float dw = bw * sc, dh = bh * sc;
    float ax = box.X - m + bw / 2.f, ay = opensUp ? box.Y - m + bh : box.Y - m;   // grows from the edge next to its button
    g.DrawImage(bmp, RectF(ax - dw / 2, (opensUp ? ay - dh : ay) + slide, dw, dh), 0, 0, bw, bh, UnitPixel, &ia);
}
template <class F> static void DrawPopAnimated(Graphics& g, PopAnim& a, RectF box, float t, bool opensUp, F draw) {
    float m = S(20);
    int bw = (int)ceilf(box.Width + 2 * m), bh = (int)ceilf(box.Height + 2 * m);
    Bitmap* bmp = new Bitmap(bw, bh, PixelFormat32bppPARGB);   // snapshot: used while opening and kept for the closing fade
    {
        Graphics bg(bmp);
        bg.SetSmoothingMode(SmoothingModeAntiAlias);
        bg.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
        bg.TranslateTransform(m - box.X, m - box.Y);
        draw(bg);
    }
    delete a.snap; a.snap = bmp; a.box = box; a.m = m; a.up = opensUp;
    if (t >= 1) { draw(g); return; }
    BlitPop(g, bmp, box, m, opensUp, 1 - (1 - t) * (1 - t) * (1 - t));
}
static void ClosePop(Graphics& g, PopAnim& a) {   // call when the popup is not open: starts / runs the closing fade
    if (a.key != LLONG_MIN) { a.key = LLONG_MIN; if (a.snap) { a.closing = true; a.closeStart = GetTickCount(); KickAnim(); } }
    if (!a.closing || !a.snap) return;
    float t = (GetTickCount() - a.closeStart) / (float)CLOSE_MS;
    if (t >= 1) { a.closing = false; delete a.snap; a.snap = nullptr; return; }
    g_animating = true;
    float e = (1 - t) * (1 - t);   // ease in on the way out
    BlitPop(g, a.snap, a.box, a.m, a.up, e);
}

static void DrawDropdownBody(Graphics& g) {
    RectF btn;
    if (!DropdownRect(btn)) return;
    RectF pop = DropdownPopup(btn);
    float rad = S(12);
    Shadow(g, pop, rad);
    FillRound(g, T.popup, pop, rad);
    int curIdx; auto opts = DropdownOptions(curIdx);
    std::vector<std::wstring> routes;
    if (g_ddKind == 4 && g_ddOpen >= 0 && g_ddOpen < (int)g_apps.size()) routes = AppRoutes(g_apps[g_ddOpen]);
    for (int i = 0; i < (int)opts.size(); i++) {
        RectF r = DropdownOption(pop, i);
        float ex = DdExtraBefore(i);
        if (ex > 0) {   // the line (and heading) above this item
            float ly = r.Y - ex + S(4);
            SolidBrush lb(T.subtle2); g.FillRectangle(&lb, pop.X + S(10), ly, pop.Width - S(20), S(1));
            if (g_ddKind == 5 && i == 1 && g_ddModeMap[i] >= 0) Text(g, L"Duplicate", g_f11, T.t3, RectF(r.X + S(12), ly + S(4), r.Width, S(19)));
        }
        bool cur = curIdx == i && g_ddKind != 4, hov = g_hover.h == H_DDOPT && g_hover.idx == i;
        if (cur) FillRound(g, WithAlpha(T.accent, 36), r, S(8));
        else if (hov) FillRound(g, T.subtle, r, S(8));
        float tx = r.X + S(12);
        if (g_ddKind == 4) {   // a tick box per speaker (Windows 11 can copy an app to several)
            bool tick = i == 0 ? routes.empty() : (i - 1 < (int)g_out.size() && std::find(routes.begin(), routes.end(), g_out[i - 1].id) != routes.end());
            RectF bx(r.X + S(12), r.Y + r.Height / 2 - S(8), S(16), S(16));
            if (tick) FillRound(g, T.accent, bx, S(4));
            else {
                FillRound(g, WithAlpha(T.t1, hov ? 23 : 10), bx, S(4));
                Pen bp(WithAlpha(T.t1, 140), S(1)); GraphicsPath gp; AddRound(gp, RectF(bx.X + S(0.5f), bx.Y + S(0.5f), bx.Width - S(1), bx.Height - S(1)), S(3.5f)); g.DrawPath(&bp, &gp);
            }
            if (tick) { Pen cp(T.onAccent, S(1.5f)); cp.SetStartCap(LineCapRound); cp.SetEndCap(LineCapRound); cp.SetLineJoin(LineJoinRound);
                        float k = S(10) / 20.f, ox = bx.X + S(3), oy = bx.Y + S(3); PointF pts[3] = {PointF(ox + 4 * k, oy + 10.5f * k), PointF(ox + 7.5f * k, oy + 14 * k), PointF(ox + 16 * k, oy + 6 * k)};
                        g.DrawLines(&cp, pts, 3); }
            tx = bx.X + bx.Width + S(10);
        }
        if (g_ddKind == 5 && i > 0 && i < (int)g_ddModeMap.size() && g_ddModeMap[i] >= 0) {   // the screen's number in a small box
            RectF nb(tx, r.Y + r.Height / 2 - S(8), S(18), S(16)); GraphicsPath gp; AddRound(gp, RectF(nb.X + S(0.75f), nb.Y + S(0.75f), nb.Width - S(1.5f), nb.Height - S(1.5f)), S(3.5f));
            Pen pp(WithAlpha(T.t1, 217), S(1.5f)); g.DrawPath(&pp, &gp);
            wchar_t num[4]; swprintf(num, 4, L"%d", g_ddModeMap[i] + 1); Text(g, num, g_fNum, WithAlpha(T.t1, 217), nb, StringAlignmentCenter);
            tx += S(26);
        }
        if (g_ddKind == 3 && i < (int)g_ddProfMap.size() && g_ddProfMap[i] == -1) { Icon(g, "plus", S(13), T.t1, tx + S(6.5f), r.Y + r.Height / 2); tx += S(19); }
        Text(g, opts[i].c_str(), g_fName, T.t1, RectF(tx, r.Y, r.X + r.Width - S(6) - tx, r.Height));
    }
}

static void GhostBtn(Graphics& g, RectF b, const wchar_t* t, bool hov, bool accent = false, bool enabled = true) {   // the design's small round buttons
    FillRound(g, accent && enabled ? (hov ? WithAlpha(T.accent, 225) : T.accent) : (hov && enabled ? T.subtle3 : T.subtle2), b, b.Height / 2);
    Text(g, t, g_f11s, accent && enabled ? T.onAccent : enabled ? T.t1 : T.t3, b, StringAlignmentCenter);
}
// ---- Auto sync card (the design's "bpop"): ask first, then which speaker is chirping, then what each one waits
static bool CalFailed() { return !g_calibrating && !g_calAsk && !g_calStatus.empty() && g_calStatus.rfind(L"In sync", 0) != 0; }
struct RichWord { std::wstring w; bool bold; };
static std::vector<std::vector<RichWord>> CalText(float width) {   // the card's sentence, wrapped; some words bold
    std::vector<RichWord> words;
    auto addWords = [&](const std::wstring& s, bool b) { std::wstring cur; for (wchar_t c : s + L" ") { if (c == L' ') { if (!cur.empty()) words.push_back({cur, b}); cur.clear(); } else cur += c; } };
    if (g_calAsk) { addWords(L"Knob plays a short chirp on each speaker and listens with your microphone.", false); addWords(L"Pause music and keep the room quiet.", true); }
    else if (g_calibrating) { addWords(L"Listening for", false); addWords((g_calK < (int)g_calNames.size() ? g_calNames[g_calK] : std::wstring(L"the speakers")) + L"…", true); }
    else if (CalFailed()) addWords(g_calStatus, false);
    std::vector<std::vector<RichWord>> lines(1);
    float x = 0, sp = MeasureW(L"a b", g_fSmall) - MeasureW(L"ab", g_fSmall);
    for (auto& w : words) {
        float ww = MeasureW(w.w.c_str(), g_fSmall) - S(2);
        if (x > 0 && x + ww > width) { lines.push_back({}); x = 0; }
        lines.back().push_back(w); x += ww + sp;
    }
    if (lines.back().empty()) lines.pop_back();
    return lines;
}
static float CalBodyH() {
    float w = PaneW() + S(10) - S(28);
    if (!g_calAsk && !g_calibrating && !CalFailed()) return S(4) + (float)UsedSpeakers().size() * S(21) - S(5) + S(12);   // the list
    float h = CalText(w).size() * S(17.25f) + S(10);
    if (g_calibrating) h += S(2) + S(4) + S(12);   // progress bar
    return h;
}
static RectF CalCard() {
    float h = S(14) + S(17) + S(4) + CalBodyH() + S(24) + S(14);
    return RectF(S(12), PaneBottom() - S(14) - h, g_W - S(24), h);
}
static RectF CalBtn(int which) {
    RectF c = CalCard(); float y = c.Y + c.Height - S(14) - S(24);
    const wchar_t* primary = g_calAsk ? L"Start" : g_calibrating ? nullptr : L"Done";
    const wchar_t* second = g_calAsk ? L"Cancel" : g_calibrating ? L"Stop" : nullptr;
    float pw = primary ? S(22) + MeasureW(primary, g_f11s) : 0, x = c.X + c.Width - S(14);
    if (which == 1) return primary ? RectF(x - pw, y, pw, S(24)) : RectF();
    if (!second) return RectF();
    float sw = S(22) + MeasureW(second, g_f11s);
    return RectF(x - (primary ? pw + S(6) : 0) - sw, y, sw, S(24));
}
static void DrawCalCard(Graphics& g) {
    if (!g_calShow) return;
    RectF c = CalCard(); float rad = S(12);
    Shadow(g, c, rad); FillRound(g, T.popup, c, rad);
    Font f13(g_famSemi, S(13), FontStyleRegular, UnitPixel), fB(g_famSemi, S(11.5f), FontStyleRegular, UnitPixel);
    bool done = !g_calAsk && !g_calibrating && !CalFailed();
    std::wstring title = g_calAsk ? L"Auto sync" : g_calibrating ? L"Measuring " + std::to_wstring(std::min(g_calK + 1, (int)std::max<size_t>(1, g_calIds.size()))) + L" of " + std::to_wstring(std::max<size_t>(1, g_calIds.size()))
                       : done ? L"Synced" : L"Couldn't sync";
    float tx = c.X + S(14), ty = c.Y + S(14) + S(8.5f);
    Icon(g, done ? "check" : "timer", S(15), T.accent, tx + S(7.5f), ty);
    Text(g, title.c_str(), &f13, T.t1, RectF(tx + S(23), ty - S(10), c.Width - S(50), S(20)));
    float y = c.Y + S(14) + S(17) + S(4);
    if (done) {   // what each speaker waits; the slowest waits nothing
        y += S(4);
        for (auto& id : UsedSpeakers()) {
            int d = ShownDelay(id);
            Text(g, OutName(id).c_str(), g_fSmall, T.t1, RectF(tx, y, c.Width - S(140), S(16)));
            if (g_calMissed.count(id)) Text(g, L"not heard", &fB, Color(255, 232, 72, 72), RectF(c.X + c.Width - S(14) - S(120), y, S(120), S(16)), StringAlignmentFar);
            else if (d) { wchar_t t[24]; swprintf(t, 24, L"+%d ms", d); Text(g, t, &fB, T.accent, RectF(c.X + c.Width - S(14) - S(120), y, S(120), S(16)), StringAlignmentFar); }
            else Text(g, L"slowest · 0 ms", g_fSmall, T.t3, RectF(c.X + c.Width - S(14) - S(120), y, S(120), S(16)), StringAlignmentFar);
            y += S(21);
        }
    } else {
        auto lines = CalText(c.Width - S(28));
        float sp = MeasureW(L"a b", g_fSmall) - MeasureW(L"ab", g_fSmall);
        for (auto& ln : lines) {
            float x = tx;
            for (auto& w : ln) { Font* f = w.bold ? &fB : g_fSmall; Text(g, w.w.c_str(), f, w.bold ? T.t1 : T.t3, RectF(x - S(1), y, S(400), S(17.25f))); x += TextW(g, w.w.c_str(), f) - S(2) + sp; }
            y += S(17.25f);
        }
        y += S(10);
        if (g_calibrating) {   // how far along
            RectF bar(tx, y + S(2), c.Width - S(28), S(4));
            FillRound(g, T.subtle2, bar, S(2));
            float f = g_calIds.empty() ? 0.f : (float)g_calK / g_calIds.size();
            if (f > 0) FillRound(g, T.accent, RectF(bar.X, bar.Y, bar.Width * f, bar.Height), S(2));
        }
    }
    RectF b0 = CalBtn(0), b1 = CalBtn(1);
    if (b0.Width > 0) GhostBtn(g, b0, g_calAsk ? L"Cancel" : L"Stop", g_hover.h == H_CALCANCEL);
    if (b1.Width > 0) GhostBtn(g, b1, g_calAsk ? L"Start" : L"Done", g_hover.h == H_CALSTART || g_hover.h == H_CALDONE, true);
}
static int MeasuredShown(const std::wstring& id) {   // what Auto sync measured, shown the same way as the chip (slowest 0)
    auto it = g_delayMeasured.find(id); if (it == g_delayMeasured.end()) return ShownDelay(id);
    int m = 0; bool first = true;
    for (auto& u : UsedSpeakers()) { auto f = g_delayMeasured.find(u); int d = f != g_delayMeasured.end() ? f->second : DelayFor(u); if (first || d < m) m = d; first = false; }
    bool isDef = false; for (auto& o : g_out) if (o.id == id && o.isDefault) isDef = true;
    return (isDef ? 0 : it->second) - m;
}
static std::wstring DelayNote(int i, bool*) {
    if (i < 0 || i >= (int)g_out.size()) return L"";
    int base = MeasuredShown(g_out[i].id), now = DelayOf(i), tweak = now - base;
    std::wstring s = L"Auto sync measured +" + std::to_wstring(base) + L" ms";
    if (tweak) s += L". Your tweak: " + std::wstring(tweak > 0 ? L"+" : L"−") + std::to_wstring(abs(tweak)) + L" ms";
    return s + L".";
}
static Graphics* MeasureG();
static float DelayNoteLines() {
    StringFormat sf; RectF out; INT chars = 0, lines = 0;
    std::wstring t = DelayNote(g_delayOpen);
    MeasureG()->MeasureString(t.c_str(), -1, g_fSmall, RectF(0, 0, S(228), 1000), &sf, &out, &chars, &lines);
    return (float)std::max(1, (int)lines);
}
static void DrawDelayPopupBody(Graphics& g) {
    RectF chip;
    if (g_delayOpen < 0 || !DelayAnchor(chip)) return;
    RectF pop = DelayPopup(chip);
    float rad = S(10);
    Shadow(g, pop, rad);
    FillRound(g, T.popup, pop, rad);
    Font f13(g_famSemi, S(13), FontStyleRegular, UnitPixel);
    Text(g, OutLabel(g_out[g_delayOpen]).c_str(), &f13, T.t1, RectF(pop.X + S(14), pop.Y + S(12), pop.Width - S(28), S(17)));
    for (int k = 0; k < 2; k++) {
        RectF b = k ? DlPlus(pop) : DlMinus(pop); bool hv = g_hover.h == (k ? H_DLPLUS : H_DLMINUS);
        FillRound(g, hv ? T.subtle3 : T.subtle2, b, S(8));
        Font f18(g_famText, S(18), FontStyleRegular, UnitPixel);
        Text(g, k ? L"+" : L"−", &f18, T.t1, b, StringAlignmentCenter);
    }
    {   // "+200 ms", big, in the accent
        Font f21(g_famSemi, S(21), FontStyleRegular, UnitPixel);
        wchar_t t[16]; swprintf(t, 16, L"+%d", DelayOf(g_delayOpen));
        float w1 = TextW(g, t, &f21), w2 = TextW(g, L"ms", g_fSmall), cx = pop.X + pop.Width / 2, x = cx - (w1 + S(2) + w2) / 2, cy = pop.Y + S(37) + S(17);
        Text(g, t, &f21, T.accent, RectF(x, cy - S(14), w1 + S(4), S(28)));
        Text(g, L"ms", g_fSmall, T.t3, RectF(x + w1 + S(2) - S(2), cy - S(7), w2 + S(6), S(18)));
    }
    std::wstring note = DelayNote(g_delayOpen);
    TextWrap(g, note.c_str(), g_fSmall, T.t3, RectF(pop.X + S(14), pop.Y + S(79), pop.Width - S(28), DelayNoteLines() * S(17.25f) + S(4)));
    bool canReset = DelayOf(g_delayOpen) != MeasuredShown(g_out[g_delayOpen].id);
    GhostBtn(g, DlReset(pop), L"Reset", g_hover.h == H_DLRESET, false, canReset);
}

static void DrawDropdown(Graphics& g) {
    RectF btn;
    if (!DropdownRect(btn)) { ClosePop(g, g_paDD); return; }
    RectF pop = DropdownPopup(btn);
    float t = PopT(g_paDD, (long long)g_ddKind * 100000 + g_ddOpen);
    DrawPopAnimated(g, g_paDD, pop, t, pop.Y < btn.Y, [&](Graphics& gg) { DrawDropdownBody(gg); });
}
static void DrawDelayPopup(Graphics& g) {
    RectF chip;
    if (g_delayOpen < 0 || !DelayAnchor(chip)) { ClosePop(g, g_paDelay); return; }
    RectF pop = DelayPopup(chip);
    float t = PopT(g_paDelay, g_delayOpen);
    DrawPopAnimated(g, g_paDelay, pop, t, pop.Y < chip.Y, [&](Graphics& gg) { DrawDelayPopupBody(gg); });
}

static bool g_dwmRound = false;

// Smooth changes: when the theme, style or accent changes, both windows fade from the old look to the new one.
struct Fade { std::vector<BYTE> last, from, scratch; int w = 0, h = 0; DWORD start = 0, armed = 0, ms = 380; float t = 1; bool on = false; int top = 0; };   // start = 0: the clock begins at the first frame painted in the new look
static Fade g_fadeFly, g_fadeSet;
static const DWORD FADE_THEME_MS = 380, FADE_PANE_MS = 200;
static float EaseInOut(float t) { return t < .5f ? 4 * t * t * t : 1 - powf(-2 * t + 2, 3) / 2; }
enum { TIMER_FADE = 11 };
static void FadeApply(Fade& f, void* bitsV, int w, int h) {   // call after drawing, before showing
    BYTE* bits = (BYTE*)bitsV; size_t n = (size_t)w * h * 4;
    f.t = 1;
    if (f.on && !f.start && GetTickCount() - f.armed > 1500) f.on = false;   // never painted in the new look: don't replay it later
    if (!f.on) { f.last.assign(bits, bits + n); f.w = w; f.h = h; return; }   // reuses its memory
    if (!f.start) f.start = GetTickCount();   // the restyle itself takes a moment; time the fade from the first new frame, not from the click
    std::vector<BYTE>& fresh = f.scratch; fresh.assign(bits, bits + n);
    {
        float t = (GetTickCount() - f.start) / (float)f.ms;
        if (t >= 1 || f.w != w || f.h != h || f.from.size() != n) f.on = false;
        else {
            t = EaseInOut(t); f.t = t;
            int a = (int)(t * 256);
            for (size_t i = (size_t)std::max(0, std::min(h, f.top)) * w * 4; i < n; i++) bits[i] = (BYTE)(f.from[i] + (((int)bits[i] - (int)f.from[i]) * a >> 8));
        }
    }
    f.last.swap(fresh); f.w = w; f.h = h;
}
static void FadeFree(Fade& f) { std::vector<BYTE>().swap(f.last); std::vector<BYTE>().swap(f.from); std::vector<BYTE>().swap(f.scratch); f.on = false; }
static void StartFade() {
    for (Fade* f : {&g_fadeFly, &g_fadeSet}) {
        if (f == &g_fadeFly && !g_visible) { f->on = false; continue; }   // hidden: nothing to fade, and it must not replay when it opens
        if (!f->last.empty()) { f->from = f->last; f->start = 0; f->armed = GetTickCount(); f->ms = FADE_THEME_MS; f->on = true; f->top = 0; }
    }
    if (g_hwnd) SetTimer(g_hwnd, TIMER_FADE, 15, nullptr);
}
static void StartPaneFade() {   // tab switch: cross-fade the content under the tabs only
    if (g_fadeFly.last.empty()) return;
    g_fadeFly.from = g_fadeFly.last; g_fadeFly.start = 0; g_fadeFly.armed = GetTickCount(); g_fadeFly.ms = FADE_PANE_MS; g_fadeFly.on = true; g_fadeFly.top = (int)S(54);
    if (g_hwnd) SetTimer(g_hwnd, TIMER_FADE, 15, nullptr);
}
// One 32-bit drawing surface per window, kept between frames (animations paint ~60 times a second)
struct Canvas { HBITMAP dib = nullptr; void* bits = nullptr; int w = 0, h = 0; };
static Canvas g_cvFly, g_cvSet;
static HBITMAP CanvasFor(Canvas& c, HDC hdc, int w, int h, void** bits) {
    if (!c.dib || c.w != w || c.h != h) {
        if (c.dib) DeleteObject(c.dib);
        BITMAPINFO bi = {}; bi.bmiHeader.biSize = sizeof(bi.bmiHeader); bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h;
        bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
        c.bits = nullptr; c.dib = CreateDIBSection(hdc, &bi, DIB_RGB_COLORS, &c.bits, nullptr, 0); c.w = w; c.h = h;
        if (!c.dib) { c.w = c.h = 0; c.bits = nullptr; }
    }
    *bits = c.bits; return c.dib;
}
static void CanvasFree(Canvas& c) { if (c.dib) DeleteObject(c.dib); c = Canvas(); }
static void Paint(HDC hdc) {
    if (g_W <= 0 || g_H <= 0) return;
    // 32-bit premultiplied canvas: alpha is honoured by DWM when Glass (acrylic) is on
    void* bits = nullptr;
    HBITMAP dib = CanvasFor(g_cvFly, hdc, g_W, g_H, &bits);
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
        for (int i = 0; i < (int)g_els.size(); i++) {   // a screen with night light on gets a faint warm wash, as in the design
            if (g_els[i].k != E_MONHEAD) continue;
            int mi = g_els[i].idx; if (mi < 0 || mi >= (int)g_mons.size() || !g_night.count(g_mons[mi].name) || !g_night[g_mons[mi].name].on) continue;
            float bottom = g_els[i].r.Y + g_els[i].r.Height;
            for (auto& e2 : g_els) if (e2.idx == mi && (e2.k == E_MONB || e2.k == E_MONC || e2.k == E_MONDIM || e2.k == E_MONNL || e2.k == E_MONO)) bottom = std::max(bottom, e2.r.Y + e2.r.Height);
            FillRound(g, Color(18, 255, 138, 42), RectF(g_els[i].r.X, g_els[i].r.Y - S(4), g_els[i].r.Width, bottom + S(2) - g_els[i].r.Y + S(4)), S(10));
        }
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

        {
        // tabs
        FillRound(g, T.tabsBg, RectF(S(12), S(12), g_W - S(24), S(38)), S(8));
        const wchar_t* names[3] = {L"Sound", L"Apps", L"Display"};
        {   // the selected pill glides from tab to tab
            RectF r0 = TabRect(0), r1 = TabRect(1);
            RectF pr(r0.X + TabPos() * (r1.X - r0.X), r0.Y, r0.Width, r0.Height);
            FillRound(g, T.tabSel, pr, S(6));
        }
        for (int i = 0; i < 3; i++) {
            RectF r = TabRect(i);
            bool sel = g_tab == i, hov = g_hover.h == H_TAB && g_hover.idx == i;
            if (!sel && hov && fabsf(TabPos() - i) > 0.5f) FillRound(g, T.hover, r, S(6));
            Font* f = sel ? g_fTab600 : g_fTab;
            float tw = TextW(g, names[i], f), total = S(17) + S(8) + tw, sx = r.X + (r.Width - total) / 2;
            Icon(g, i == 0 ? "speaker" : i == 1 ? "apps" : "sun", S(17), T.t1, sx + S(8.5f), r.Y + r.Height / 2);
            Text(g, names[i], f, T.t1, RectF(sx + S(17) + S(8), r.Y, tw + S(4), r.Height));
        }
        }

        // footer (same surface as the panel: no separate band)
        float fy = PaneBottom() + S(FTR) / 2;
        Icon(g, "mixer", S(14), T.t1, S(16) + S(7), fy);
        {   // "Knob" with the version (or the update pill) underneath
            float nx = S(16) + S(14) + S(10);
            Text(g, APP_NAME, g_fSmall600, T.t1, RectF(nx, fy - S(14), S(120), S(15)));
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
                Text(g, vt.c_str(), g_f11, T.t3, RectF(nx, fy + S(1), vw + S(8), S(14)));
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
        if (g_hover.h == H_IDENT) FillRound(g, T.hover, IdentRect(), S(6));
        Icon(g, "identify", S(18), T.t1, IdentRect().X + S(16), IdentRect().Y + S(16));
        DrawIconBtn(g, GearRect(), G_SETTINGS, T.t1, g_hover.h == H_GEAR);

        // popups on top
        DrawDropdown(g);
        DrawDelayPopup(g);
        if (g_tab == T_SOUND) DrawCalCard(g);
        if (!g_dwmRound) { Pen bp(T.subtle3, S(1)); g.DrawRectangle(&bp, RectF(0, 0, g_W - 1.f, g_H - 1.f)); }
    }
    FadeApply(g_fadeFly, bits, g_W, g_H);
    BitBlt(hdc, 0, 0, g_W, g_H, mdc, 0, 0, SRCCOPY);
    SelectObject(mdc, old);
    DeleteDC(mdc);
}

static void ForceForeground(HWND h) {   // Windows only lets the active app take focus; borrow the active window's input for a moment
    if (!h) return;
    HWND fg = GetForegroundWindow();
    if (fg == h) return;
    DWORD me = GetCurrentThreadId(), ft = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
    bool att = ft && ft != me && AttachThreadInput(me, ft, TRUE);
    BringWindowToTop(h); SetForegroundWindow(h);
    if (att) AttachThreadInput(me, ft, FALSE);
}
static void ShowFlyout();
static void PairHide() {   // Settings lost the foreground to another app: tuck the panel away with it
    if (!g_set || !g_visible || g_pairHidden) return;
    g_pairHidden = true;
    ShowWindow(g_hwnd, SW_HIDE);
}
static void PairBack() {   // you clicked another app: panel and Settings both stay on screen, behind that app like normal windows
    if (!g_set || !g_visible || g_pairHidden) return;
    SetWindowPos(g_hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    SetWindowPos(g_set, g_hwnd, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);   // Settings sits right under the panel
}
static void PairShow(bool activateSettings) {   // Settings came back: bring the panel back beside it
    if (!g_set) return;
    if (g_pairHidden) { g_pairHidden = false; ShowWindow(g_hwnd, SW_SHOWNOACTIVATE); InvalidateRect(g_hwnd, nullptr, FALSE); }
    if (g_visible) {   // like G-Helper's extra windows: both are ordinary windows (not always on top) and come forward together
        SetWindowPos(g_hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);   // not always-on-top...
        SetWindowPos(g_hwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);   // ...but above every other app (NOTOPMOST alone leaves it where it was)
        SetWindowPos(g_set, g_hwnd, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);   // Settings right under the panel
    }
    if (activateSettings) { if (IsIconic(g_set)) ShowWindow(g_set, SW_RESTORE); SetForegroundWindow(g_set); }
}
static void HideFlyout();
static void OpenSettings(int page, bool newProfile);


static void SetTab(int t);
static void NextOutput();
static void StartAutoSync();
static void SwitchOutput(int i);
static HMONITOR PanelMonitor() {   // the screen Knob's windows open on
    if (g_openOn == 0) { POINT o = {0, 0}; return MonitorFromPoint(o, MONITOR_DEFAULTTOPRIMARY); }
    POINT pt; GetCursorPos(&pt); return MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
}
static void RunHotkey(int a, const std::wstring& param = L"") {
    if (a == HK_OPEN) { if (g_set && (g_visible || g_pairHidden)) PairShow(true); else if (g_visible) HideFlyout(); else ShowFlyout(); return; }
    if (a == HK_TABSOUND || a == HK_TABAPPS || a == HK_TABDISPLAY) {   // open straight on that tab
        int t = a == HK_TABSOUND ? T_SOUND : a == HK_TABAPPS ? T_APPS : T_DISPLAY;
        bool front = GetForegroundWindow() == g_hwnd;
        if (g_visible && !g_set && front && g_tab == t) { HideFlyout(); return; }   // same keys again closes it (only the panel on its own)
        if (g_pairHidden) PairShow(false);   // Settings is open behind another app: bring the panel back first
        if (!g_visible) { g_tab = t; g_tabStart = 0; ShowFlyout(); }   // g_tabStart = 0: the tab highlight jumps straight to this tab
        else {   // already open (or behind other windows): go to that tab and take focus
            LoadEndpoints(eRender, g_out); LoadEndpoints(eCapture, g_in);
            SetTab(t);
            if (t == T_APPS) { LoadApps(); UpdatePeaks(); }
            InvalidateRect(g_hwnd, nullptr, FALSE);
            if (g_set) PairShow(false);   // Settings comes forward with the panel
            ForceForeground(g_hwnd);
        }
        return;
    }
    int d = DefaultOut();
    switch (a) {
    case HK_NEXTOUT: NextOutput(); break;
    case HK_VOLUP: case HK_VOLDOWN:
        if (d >= 0) {
            float delta = (a == HK_VOLUP ? 1 : -1) * g_step / 100.f;
            for (int i = 0; i < (int)g_out.size(); i++) if (OutSelected(i)) SetEndpointLevel(g_out[i], g_out[i].level + delta);
            UpdateTrayTip();
        }
        break;
    case HK_MUTE:
        if (d >= 0 && g_out[d].vol) { BOOL m = !g_out[d].mute; g_out[d].vol->SetMute(m, nullptr); g_out[d].mute = m; UpdateTrayTip(); }
        break;
    case HK_MIC: MicToggleMute(); break;
    case HK_APPMUTE: {   // the app whose window is in front
        DWORD pid = 0; GetWindowThreadProcessId(GetForegroundWindow(), &pid);
        std::wstring path = ToLower(ProcPath(pid));
        LoadApps(true);   // the app in front may be quiet right now, so look at every app Windows knows
        for (auto& ap : g_apps) {
            if (ap.system) continue;
            bool match = std::find(ap.pids.begin(), ap.pids.end(), pid) != ap.pids.end() || (!path.empty() && ap.key == path);
            if (!match) continue;
            ap.mute = !ap.mute;
            for (auto* sv : ap.vols) sv->SetMute(ap.mute, nullptr);
            break;
        }
        if (!g_visible) FreeApps(); else LoadApps();   // back to the mixer's list
        break;
    }
    case HK_BRIUP: case HK_BRIDOWN: {
        for (int i = 0; i < (int)g_mons.size(); i++) {
            if (!g_mons[i].ok) continue;
            int p = std::max(0, std::min(100, g_mons[i].pct + (a == HK_BRIUP ? g_step : -g_step)));
            g_mons[i].pct = p; MonSet(i, 0, p);
        }
        if (g_visible) InvalidateRect(g_hwnd, nullptr, FALSE);
        break;
    }
    case HK_SWITCHTO: for (int i = 0; i < (int)g_out.size(); i++) if (g_out[i].name == param || OutLabel(g_out[i]) == param) { SwitchOutput(i); break; } break;
    case HK_APPLYPROF: for (int i = 0; i < (int)g_prof.size(); i++) if (g_prof[i].name == param && !g_prof[i].v.empty()) { ApplyProfile(i); break; } break;
    case HK_SCREENOFF: ScreenOffRequest(); break;
    case HK_PARTY: HideFlyout(); PartyStart(); break;
    case HK_SYNC: StartAutoSync(); break;
    }
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
    DWORD tint = T.dark ? 0xB8141414 : 0xA8F3F3F3;   // dark: ~72% #141414 so bright wallpapers can't wash it out; light: ~66%
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
    HMONITOR hm = g_visible ? MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST) : PanelMonitor();
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
    SetWindowPos(g_hwnd, g_set ? HWND_NOTOPMOST : HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE);   // on top only as a quick flyout; next to Settings it is a normal window
}

static void StartAnim() { if (g_visible && g_tab == T_APPS) SetTimer(g_hwnd, TIMER_ANIM, 70, nullptr); else KillTimer(g_hwnd, TIMER_ANIM); }

static void ShowFlyout() {
    if (g_pairHidden) { PairShow(true); return; }   // Settings is open: bring the pair back
    KillTimer(g_hwnd, TIMER_TRIM);
    LoadEndpoints(eRender, g_out);
    LoadEndpoints(eCapture, g_in);
    LoadApps();
    UpdatePeaks();
    UpdateTrayTip();
    BrightRequest(!g_monsLoaded, g_monsLoaded);
    PositionFlyout();
    ShowWindow(g_hwnd, SW_SHOW);
    ForceForeground(g_hwnd);
    g_visible = true;
    g_shownTick = GetTickCount(); g_hadFocus = false; g_focusRetried = false;
    SetTimer(g_hwnd, TIMER_FOCUSWATCH, 150, nullptr);   // a safety net next to WM_ACTIVATE: not in front any more means closed
    SetTimer(g_hwnd, TIMER_REFRESH, 1000, nullptr);
    StartAnim();
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

static void HideFlyout() {
    if (!g_visible) return;
    if (g_set) return;   // the panel stays while Settings is open (Esc, clicking away, tray and shortcut all wait)
    g_visible = false;
    g_dragEl = -1; g_ddOpen = -1; g_delayOpen = -1;
    SaveSettings();
    if (g_capture >= 0) { g_capture = -1; RegisterHotkeys(); }
    if (GetCapture() == g_hwnd) ReleaseCapture();
    KillTimer(g_hwnd, TIMER_REFRESH);
    KillTimer(g_hwnd, TIMER_FOCUSWATCH);
    KillTimer(g_hwnd, TIMER_ANIM);
    ShowWindow(g_hwnd, SW_HIDE);
    g_hideTick = GetTickCount();
    FreeApps();
    SetTimer(g_hwnd, TIMER_TRIM, 60000, nullptr);   // hidden for a minute: give back the memory used for drawing
}

static void SetTab(int t) {
    if (t == g_tab) return;
    g_tabFrom = TabPos(); g_tabTo = (float)t; g_tabStart = GetTickCount();
    g_tab = t;
    StartPaneFade(); KickAnim();
    g_hover = Hit(); g_ddOpen = -1; g_delayOpen = -1;
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

static bool IsCallEndpoint(const Endpoint& e) {   // Bluetooth hands-free (call quality) output
    std::wstring l = ToLower(e.rawName + L" " + e.rawIface);
    return e.bt && (ToLower(e.rawName) == L"headset" || l.find(L"hands-free") != std::wstring::npos);
}
// "Turn off displays" without locking: real screen-off sends Modern Standby laptops to sleep (and the lock screen),
// so Knob covers every screen with black and turns brightness to 0, then puts everything back on any input.
static std::vector<HWND> g_blackWnds;
static std::vector<int> g_blackPrev;
static std::vector<std::wstring> g_blackPowered;   // monitors told to switch their screen off (by name: the list can change while they're off)
static POINT g_blackPt; static DWORD g_blackTick;
// the black fades in over BLACK_FADE ms, then the screens' own brightness goes to 0; waking fades it back out
static const DWORD BLACK_FADE = 320;
static int g_blackDir = 0;              // 1 fading to black, -1 fading back, 0 holding
static DWORD g_blackT0; static int g_blackA = 0, g_blackA0 = 0;
static bool g_blackCursorHidden = false;
static void BlackSetAlpha(int a) { g_blackA = a; for (HWND w : g_blackWnds) SetLayeredWindowAttributes(w, 0, (BYTE)a, LWA_ALPHA); }
static void BlackDestroy() {
    for (HWND w : g_blackWnds) DestroyWindow(w);
    g_blackWnds.clear(); g_blackDir = 0; g_blackA = 0;
}
static void BlackoutEnd(bool now = false) {   // now = at exit: no fade, never leave the screens dark
    if (g_blackWnds.empty()) return;
    if (g_blackCursorHidden) { ShowCursor(TRUE); g_blackCursorHidden = false; }
    for (int i = 0; i < (int)g_blackPrev.size() && i < (int)g_mons.size(); i++) if (g_blackPrev[i] >= 0) { g_mons[i].pct = g_blackPrev[i]; MonSet(i, 0, g_blackPrev[i]); }   // brightness back first, under the black
    g_blackPrev.clear();
    for (auto& n : g_blackPowered) for (int i = 0; i < (int)g_mons.size(); i++) if (g_mons[i].name == n) SetMonitorPower(i, true);   // back on first, under the black
    g_blackPowered.clear();
    if (now) { BlackDestroy(); return; }
    if (g_blackDir == -1) return;
    g_blackDir = -1; g_blackA0 = g_blackA; g_blackT0 = GetTickCount();
    SetTimer(g_blackWnds[0], 1, 15, nullptr);
}
static LRESULT CALLBACK BlackProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_ERASEBKGND: { if (GetWindowLongPtrW(h, GWLP_USERDATA) & 1) return 1; RECT r; GetClientRect(h, &r); FillRect((HDC)wp, &r, (HBRUSH)GetStockObject(BLACK_BRUSH)); return 1; }
    case WM_PAINT: { PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps); if (GetWindowLongPtrW(h, GWLP_USERDATA) & 1) PaintClock(h, dc); EndPaint(h, &ps); return 0; }
    case WM_SETCURSOR: SetCursor(nullptr); return TRUE;
    case WM_TIMER: {   // one fade step; smooth start and end
        if (wp == 2) { SetWindowLongPtrW(h, GWLP_USERDATA, ((LONG_PTR)(GetTickCount() % 9409) << 1) | 1); InvalidateRect(h, nullptr, FALSE); return 0; }   // the clock: next minute, a little elsewhere
        float t = std::min(1.f, (GetTickCount() - g_blackT0) / (float)BLACK_FADE), e = t * t * (3 - 2 * t);
        int target = g_blackDir == 1 ? 255 : 0;
        BlackSetAlpha(g_blackA0 + (int)lroundf((target - g_blackA0) * e));
        if (t >= 1) {
            KillTimer(h, 1);
            if (g_blackDir == 1) {   // fully black: now dim every display we control to 0, remembering where it was
                g_blackDir = 0;
                g_blackPrev.assign(g_mons.size(), -1);
                for (int i = 0; i < (int)g_mons.size(); i++) if (g_mons[i].ok && !IsClockScreen(g_mons[i].rc)) { g_blackPrev[i] = g_mons[i].pct; MonSet(i, 0, 0); }   // the clock's screen keeps its brightness
                if (g_offFull) for (int i = 0; i < (int)g_mons.size(); i++)   // monitors: their own power-down, so the backlight goes dark too
                    if (!g_mons[i].internal && g_mons[i].cloneOf < 0 && !IsClockScreen(g_mons[i].rc)) { SetMonitorPower(i, false); g_blackPowered.push_back(g_mons[i].name); }
            } else if (g_blackDir == -1) BlackDestroy();
        }
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (GetTickCount() - g_blackTick < 800) return 0;   // ignore the click that started it
        POINT p; GetCursorPos(&p);
        if (abs(p.x - g_blackPt.x) > 6 || abs(p.y - g_blackPt.y) > 6) BlackoutEnd();
        return 0;
    }
    case WM_KEYDOWN: case WM_SYSKEYDOWN: case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN: case WM_MOUSEWHEEL:
        if (GetTickCount() - g_blackTick > 300) BlackoutEnd();
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}
static BOOL CALLBACK BlackMonProc(HMONITOR, HDC, LPRECT rc, LPARAM) {
    HWND w = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED, L"KnobBlack", L"", WS_POPUP, rc->left, rc->top, rc->right - rc->left, rc->bottom - rc->top, nullptr, nullptr, g_inst, nullptr);
    if (w && IsClockScreen(*rc)) { SetWindowLongPtrW(w, GWLP_USERDATA, ((LONG_PTR)(GetTickCount() % 9409) << 1) | 1); SetTimer(w, 2, 60000, nullptr); }
    if (w) { SetLayeredWindowAttributes(w, 0, 0, LWA_ALPHA); ShowWindow(w, SW_SHOW); g_blackWnds.push_back(w); }   // starts see-through, then fades to black
    return TRUE;
}
static void BlackoutStart() {
    if (!g_blackWnds.empty()) return;
    static bool reg = false;
    if (!reg) { WNDCLASSW wc = {}; wc.lpfnWndProc = BlackProc; wc.hInstance = g_inst; wc.lpszClassName = L"KnobBlack"; wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH); RegisterClassW(&wc); reg = true; }
    EnumDisplayMonitors(nullptr, nullptr, BlackMonProc, 0);
    if (g_blackWnds.empty()) return;
    SetForegroundWindow(g_blackWnds[0]);
    ShowCursor(FALSE); g_blackCursorHidden = true;
    GetCursorPos(&g_blackPt); g_blackTick = GetTickCount();
    g_blackDir = 1; g_blackA0 = 0; g_blackA = 0; g_blackT0 = GetTickCount();
    SetTimer(g_blackWnds[0], 1, 15, nullptr);
}
static void ClickOutput(int i);
static void AutoSwitchCheck() {   // headphones or earbuds just appeared: play there; gone: go back
    std::set<std::wstring> now; for (auto& o : g_out) now.insert(o.id);
    if (g_prevOutKnown && g_autoSwitch) {
        const Endpoint* pick = nullptr;   // prefer the music output over a Bluetooth "calls" one
        for (auto& o : g_out) if (!g_prevOutIds.count(o.id) && (o.icon == DI_HEADPHONES || o.icon == DI_EARBUDS))
            if (!pick || (IsCallEndpoint(*pick) && !IsCallEndpoint(o))) pick = &o;
        if (pick) {
            std::wstring id = pick->id; bool isDef = pick->isDefault;
            if (!isDef || !g_mirror.empty()) {   // the headphones alone, like clicking them; remember what played before
                for (auto& d : g_out) if (d.isDefault) g_autoPrev = d.id;
                g_autoPrevMirror = g_mirror;
                g_autoTarget = id;
                for (int k = 0; k < (int)g_out.size(); k++) if (g_out[k].id == id) { ClickOutput(k); break; }
            }
        }
        if (!g_autoTarget.empty() && !now.count(g_autoTarget)) {   // they disconnected
            if (now.count(g_autoPrev)) {
                SetDefaultDevice(g_autoPrev); LoadEndpoints(eRender, g_out);
                for (auto& m : g_autoPrevMirror) if (now.count(m) && m != g_autoPrev) g_mirror.insert(m);   // the speakers that played together before
                if (!g_mirror.empty()) { SaveSettings(); RestartRouter(); }
            }
            g_autoPrevMirror.clear();
            g_autoTarget.clear(); g_autoPrev.clear();
        }
    }
    g_prevOutIds.clear(); for (auto& o : g_out) g_prevOutIds.insert(o.id);
    g_prevOutKnown = true;
}
static void ClickOutput(int i) {   // a click switches to that speaker alone, like Windows; several speakers live in Settings › Sound
    bool had = !g_mirror.empty();
    g_mirror.clear();
    if (!g_out[i].isDefault) SetDefaultDevice(g_out[i].id);
    LoadEndpoints(eRender, g_out);
    if (had) { SaveSettings(); RestartRouter(); }
    UpdateTrayTip();
}
static void SwitchOutput(int i) {   // shortcut "Switch to": that speaker, with the same popup as Next speaker
    if (i < 0 || i >= (int)g_out.size()) return;
    std::wstring id = g_out[i].id;
    ClickOutput(i);
    for (int k = 0; k < (int)g_out.size(); k++) if (g_out[k].id == id) { break; }
    if (g_visible) InvalidateRect(g_hwnd, nullptr, FALSE);
}
static void NextOutput() {   // shortcut: the next speaker in the list (skips Bluetooth call-quality outputs)
    int n = (int)g_out.size(), d = DefaultOut();
    for (int k = 1; k < n; k++) {
        int i = (d + k + n) % n;
        if (IsCallEndpoint(g_out[i])) continue;
        ClickOutput(i);
       
        if (g_visible) InvalidateRect(g_hwnd, nullptr, FALSE);
        return;
    }
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
    // GitHub's raw files are cached for a few minutes, so version.txt and Knob.exe can briefly disagree.
    // Ask which commit main is on right now, then read both files from that exact commit (never cached stale).
    std::wstring base = UPDATE_RAW;
    {
        std::string api;
        if (HttpGet(L"https://api.github.com/repos/" UPDATE_REPO L"/commits/main", api)) {
            size_t a = api.find("\"sha\":\""), b = a == std::string::npos ? a : api.find('"', a + 7);
            if (a != std::string::npos && b != std::string::npos && b - (a + 7) == 40) {
                std::string sha = api.substr(a + 7, 40);
                base = L"https://raw.githubusercontent.com/" UPDATE_REPO L"/" + std::wstring(sha.begin(), sha.end()) + L"/dist/";
            }
        }
    }
    if (HttpGet(base + L"version.txt", j, L"", &code)) {
        std::string v;
        for (char c : j) if ((c >= '0' && c <= '9') || c == '.') v += c; else if (!v.empty()) break;
        if (!v.empty()) info = new UpdInfo{std::wstring(v.begin(), v.end()), base + L"Knob.exe"};
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
static std::wstring FileVersionOf(const std::wstring& file) {   // "1.37.2" from the exe's version resource
    DWORD h = 0, n = GetFileVersionInfoSizeW(file.c_str(), &h);
    if (!n) return L"";
    std::vector<BYTE> b(n);
    VS_FIXEDFILEINFO* fi = nullptr; UINT len = 0;
    if (!GetFileVersionInfoW(file.c_str(), 0, n, b.data()) || !VerQueryValueW(b.data(), L"\\", (void**)&fi, &len) || !fi) return L"";
    return std::to_wstring(HIWORD(fi->dwFileVersionMS)) + L"." + std::to_wstring(LOWORD(fi->dwFileVersionMS)) + L"." + std::to_wstring(HIWORD(fi->dwFileVersionLS));
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
        else {   // it must really be the newer version, or the same old one would just reinstall itself
            std::wstring got = FileVersionOf(file);
            if (!got.empty() && ParseVer(got) <= ParseVer(APP_VERSION)) err = L"GitHub sent an older copy (" + got + L"). Try again in a few minutes";
        }
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

// ---------------------------------------------------------------- sound levels (Party screen, Pulse glow)
// A loopback copy of the main speaker split into bands, low notes first. The thread only runs while something
// on screen draws from it (the party screen, or a media flyout with Pulse), and stops the moment that closes.
static const int LV_N = 48;
static float g_lv[LV_N];                // 0..1 per band, written by the level thread
static volatile LONG g_lvRefs = 0, g_lvStop = 0;
static HANDLE g_lvThread = nullptr;
static void Fft(float* re, float* im, int n) {   // in place, radix 2
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { std::swap(re[i], re[j]); std::swap(im[i], im[j]); }
    }
    for (int len = 2; len <= n; len <<= 1) {
        float a = -6.2831853f / len, wr = cosf(a), wi = sinf(a);
        for (int i = 0; i < n; i += len) {
            float cr = 1, ci = 0;
            for (int k = 0; k < len / 2; k++) {
                int p = i + k, q = p + len / 2;
                float tr = re[q] * cr - im[q] * ci, ti = re[q] * ci + im[q] * cr;
                re[q] = re[p] - tr; im[q] = im[p] - ti; re[p] += tr; im[p] += ti;
                float nr = cr * wr - ci * wi; ci = cr * wi + ci * wr; cr = nr;
            }
        }
    }
}
static DWORD WINAPI LevelProc(LPVOID) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (g_demo) {   // demo builds: no sound to listen to, so the bands move like music
        float t = 0, w[LV_N]; for (int b = 0; b < LV_N; b++) w[b] = (b * 37 % 100) / 100.f;
        while (!g_lvStop) {
            Sleep(16); t += 0.045f; float beat = powf(std::max(0.f, sinf(t * 7.3f)), 5);
            for (int b = 0; b < LV_N; b++) { float f = b / (float)(LV_N - 1); w[b] = std::max(0.f, std::min(1.f, w[b] + ((b * 7919 + (int)(t * 1000)) % 100 / 100.f - .5f) * .2f));
                float target = std::min(1.f, beat * .85f * std::max(0.f, 1 - f * 2.2f) + w[b] * (.25f + .45f * (1 - f * .5f)) * (.5f + .5f * fabsf(sinf(t * (2.1f + b * .7f) + b * 2.3f))) + .06f);
                g_lv[b] += (target - g_lv[b]) * (target > g_lv[b] ? .55f : .14f); }
        }
        for (auto& x : g_lv) x = 0;
        CoUninitialize(); return 0;
    }
    IMMDeviceEnumerator* en = nullptr; IMMDevice* dev = nullptr; IAudioClient* ac = nullptr; IAudioCaptureClient* cc = nullptr; WAVEFORMATEX* wf = nullptr;
    bool ok = SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&en))
        && SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev))
        && SUCCEEDED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&ac))
        && SUCCEEDED(ac->GetMixFormat(&wf))
        && SUCCEEDED(ac->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, 1000000, 0, wf, nullptr))
        && SUCCEEDED(ac->GetService(__uuidof(IAudioCaptureClient), (void**)&cc))
        && SUCCEEDED(ac->Start());
    if (ok) {
        const int N = 1024;
        std::vector<float> ring(N, 0.f), re(N), im(N), win(N);
        for (int i = 0; i < N; i++) win[i] = 0.5f - 0.5f * cosf(6.2831853f * i / (N - 1));
        const int ch = std::max(1, (int)wf->nChannels), bits = wf->wBitsPerSample, fb = wf->nBlockAlign;
        const float rate = (float)wf->nSamplesPerSec;
        int lo[LV_N], hi[LV_N];
        for (int b = 0; b < LV_N; b++) {   // 40 Hz .. 14 kHz, evenly on a log scale like hearing
            float f0 = 40.f * powf(350.f, (float)b / LV_N), f1 = 40.f * powf(350.f, (float)(b + 1) / LV_N);
            lo[b] = std::max(1, (int)(f0 * N / rate)); hi[b] = std::max(lo[b] + 1, (int)(f1 * N / rate));
        }
        int wpos = 0; float agc = 0.02f, sm[LV_N] = {};
        DWORD lastPkt = GetTickCount();
        while (!g_lvStop) {
            Sleep(16);
            UINT32 pk = 0;
            while (SUCCEEDED(cc->GetNextPacketSize(&pk)) && pk) {
                BYTE* d = nullptr; UINT32 fr = 0; DWORD fl = 0;
                if (FAILED(cc->GetBuffer(&d, &fr, &fl, nullptr, nullptr))) break;
                for (UINT32 i = 0; i < fr; i++) {
                    float v = 0;
                    if (!(fl & AUDCLNT_BUFFERFLAGS_SILENT)) {
                        const BYTE* f = d + (size_t)i * fb;
                        for (int c = 0; c < ch && c < 2; c++) v += bits == 32 ? ((const float*)f)[c] : ((const short*)f)[c] / 32768.f;
                    }
                    ring[wpos] = v; wpos = (wpos + 1) % N;
                }
                cc->ReleaseBuffer(fr);
                lastPkt = GetTickCount();
            }
            if (GetTickCount() - lastPkt > 120) { for (auto& x : ring) x = 0; }   // nothing playing: Windows sends no packets at all
            for (int i = 0; i < N; i++) { re[i] = ring[(wpos + i) % N] * win[i]; im[i] = 0; }
            Fft(re.data(), im.data(), N);
            float lv[LV_N], mx = 0;
            for (int b = 0; b < LV_N; b++) {
                float s = 0; for (int k = lo[b]; k < hi[b]; k++) s += sqrtf(re[k] * re[k] + im[k] * im[k]);
                lv[b] = s / (hi[b] - lo[b]) * (1.f + 2.f * b / LV_N);   // a little lift for the treble, which carries less energy
                mx = std::max(mx, lv[b]);
            }
            agc = std::max(mx, std::max(0.02f, agc * 0.996f));   // follows the loudest band, so quiet and loud songs both move
            for (int b = 0; b < LV_N; b++) {
                float t = std::min(1.f, lv[b] / agc);
                sm[b] += (t - sm[b]) * (t > sm[b] ? 0.6f : 0.14f);   // quick up, slow fall
                g_lv[b] = sm[b];
            }
        }
        ac->Stop();
    }
    if (cc) cc->Release(); if (ac) ac->Release(); if (dev) dev->Release(); if (en) en->Release(); if (wf) CoTaskMemFree(wf);
    for (auto& x : g_lv) x = 0;
    CoUninitialize();
    return 0;
}
static void LevelsStart() {
    if (InterlockedIncrement(&g_lvRefs) != 1) return;
    g_lvStop = 0; g_lvThread = CreateThread(nullptr, 0, LevelProc, nullptr, 0, nullptr);
}
static void LevelsStop() {
    if (InterlockedDecrement(&g_lvRefs) != 0) return;
    if (g_lvThread) { g_lvStop = 1; WaitForSingleObject(g_lvThread, 1500); CloseHandle(g_lvThread); g_lvThread = nullptr; }
}
static float LvBar(int k, int n) {   // n bars from the bands, each blended with its neighbours so the shape reads as one soft wave
    auto at = [&](int i) { i = std::max(0, std::min(n - 1, i)); int a = i * LV_N / n, b = std::max(a + 1, (i + 1) * LV_N / n); float s = 0; for (int j = a; j < b; j++) s += g_lv[j]; return s / (b - a); };
    return (at(k - 1) + 2 * at(k) + at(k + 1)) / 4;
}

// ---------------------------------------------------------------- now playing (Windows' media sessions: the same list its own media controls show)
// mingw has no header for Windows.Media.Control, so the interfaces are called by their vtable slots (checked against the Windows SDK).
static const IID IID_GsmtcStatics = {0x2050c4ee, 0x11a0, 0x57de, {0xae, 0xd7, 0xc9, 0x7c, 0x70, 0x33, 0x82, 0x45}};
static const IID IID_KnobAsyncInfo = {0x00000036, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
struct MSess { std::wstring aumid, app, title, artist; int status = 0; bool canPP = false, canNext = false, canPrev = false, canShuf = false, canRep = false, canSeek = false, shuffle = false; int repeat = 0; INT64 start = 0, pos = 0, end = 0, updated = 0; };
struct MediaSnap { std::vector<MSess> s; int cur = -1; Gdiplus::Bitmap* cover = nullptr; bool newCover = false; int why = 0; };   // why 2: the next song started on its own
static MediaSnap g_ms;                  // what the flyouts and the party screen show (UI thread)
static Gdiplus::Bitmap* g_cover = nullptr;
static int g_coverGen = 0;   // changes with every new cover, so the flyout's pre-drawn copies know when to be redone
static Color g_coverAcc, g_coverC1, g_coverC2; static bool g_coverHas = false;
static DWORD g_mediaTid = 0; static HANDLE g_mediaThread = nullptr;
static volatile DWORD g_mediaCmdTick = 0;   // a media key or a flyout button, just now: the song change that follows isn't "Up next"
static std::wstring g_mediaPick;            // the player you picked in the flyout's list ("" = the one Windows shows)
static CRITICAL_SECTION g_mcs;
enum { MC_REFRESH = WM_APP + 1, MC_CMD, MC_QUIT };
enum { MCMD_PLAYPAUSE, MCMD_NEXT, MCMD_PREV, MCMD_SHUFFLE, MCMD_REPEAT, MCMD_SEEK };
static void** Vt(void* o) { return *(void***)o; }
static void Rel(void* o) { if (o) ((IUnknown*)o)->Release(); }
static HRESULT GetP(void* o, int slot, void** out) { *out = nullptr; return ((HRESULT(STDMETHODCALLTYPE*)(void*, void**))Vt(o)[slot])(o, out); }
static std::wstring HStr(void* o, int slot) {
    HSTRING h = nullptr; std::wstring s;
    if (SUCCEEDED(((HRESULT(STDMETHODCALLTYPE*)(void*, HSTRING*))Vt(o)[slot])(o, &h)) && h) { UINT32 n = 0; const wchar_t* p = WindowsGetStringRawBuffer(h, &n); if (p) s.assign(p, n); WindowsDeleteString(h); }
    return s;
}
static bool GetB(void* o, int slot) { BYTE b = 0; ((HRESULT(STDMETHODCALLTYPE*)(void*, BYTE*))Vt(o)[slot])(o, &b); return b != 0; }
static INT64 GetI64(void* o, int slot) { INT64 v = 0; ((HRESULT(STDMETHODCALLTYPE*)(void*, INT64*))Vt(o)[slot])(o, &v); return v; }
static bool Await(void* op, void** res, DWORD ms = 2500) {   // wait for a WinRT async call (on the media thread only); takes the operation
    if (!op) return false;
    void* info = nullptr; bool ok = false;
    if (SUCCEEDED(((IUnknown*)op)->QueryInterface(IID_KnobAsyncInfo, &info)) && info) {
        DWORD t0 = GetTickCount(); int st = 0;
        for (;;) {
            st = 0; ((HRESULT(STDMETHODCALLTYPE*)(void*, int*))Vt(info)[7])(info, &st);
            if (st != 0 || GetTickCount() - t0 > ms) break;
            Sleep(3);
        }
        if (st == 1) ok = res ? SUCCEEDED(GetP(op, 8, res)) : true;   // Completed: IAsyncOperation<T>::GetResults
        Rel(info);
    }
    Rel(op);
    return ok;
}
struct MDelegate : IUnknown {   // the event handler Windows calls when a song, its state or the list of players changes
    LONG ref = 1; WPARAM w;   // w 1: the list of players changed
    explicit MDelegate(WPARAM x) : w(x) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** p) override {
        static const IID agile = {0x94ea2b94, 0xe9cc, 0x49e0, {0xc0, 0xff, 0xee, 0x64, 0xca, 0x8f, 0x5b, 0x90}};
        // a WinRT delegate's own IID is generated from its type (a version-5 UUID); anything else isn't implemented here
        if (iid == IID_IUnknown || iid == agile || (iid.Data3 & 0xF000) == 0x5000) { *p = this; AddRef(); return S_OK; }
        *p = nullptr; return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&ref); }
    ULONG STDMETHODCALLTYPE Release() override { LONG r = InterlockedDecrement(&ref); if (!r) delete this; return r; }
    virtual HRESULT STDMETHODCALLTYPE Invoke(void*, void*) { if (g_mediaTid) PostThreadMessageW(g_mediaTid, MC_REFRESH, w, 0); return S_OK; }
    virtual ~MDelegate() {}   // after Invoke, so Invoke stays the fourth slot Windows calls
};
static std::wstring MediaAppName(const std::wstring& id) {   // "Spotify.exe", "308046B0AF4A39CB" or "Microsoft.ZuneMusic_8wekyb3d8bbwe!Microsoft.ZuneMusic" -> a name people know
    static const struct { const wchar_t* k; const wchar_t* n; } known[] = {{L"spotify", L"Spotify"}, {L"msedge", L"Edge"}, {L"chrome", L"Chrome"}, {L"brave", L"Brave"},
        {L"firefox", L"Firefox"}, {L"308046b0af4a39cb", L"Firefox"}, {L"opera", L"Opera"}, {L"vivaldi", L"Vivaldi"}, {L"zunemusic", L"Media Player"}, {L"zunevideo", L"Films & TV"},
        {L"vlc", L"VLC"}, {L"applemusic", L"Apple Music"}, {L"itunes", L"iTunes"}, {L"tidal", L"TIDAL"}, {L"deezer", L"Deezer"}, {L"amazon", L"Amazon Music"},
        {L"foobar", L"foobar2000"}, {L"musicbee", L"MusicBee"}, {L"aimp", L"AIMP"}, {L"discord", L"Discord"}, {L"mpc-", L"MPC"}, {L"potplayer", L"PotPlayer"}};
    std::wstring l = ToLower(id);
    for (auto& k : known) if (l.find(k.k) != std::wstring::npos) return k.n;
    std::wstring s = id;
    size_t b = s.find_last_of(L"\\!"); if (b != std::wstring::npos) s = s.substr(b + 1);
    if (s.size() > 4 && ToLower(s.substr(s.size() - 4)) == L".exe") s.resize(s.size() - 4);
    size_t d = s.find_last_of(L'.'); if (d != std::wstring::npos && d + 1 < s.size()) s = s.substr(d + 1);
    if (!s.empty()) s[0] = towupper(s[0]);
    return s.empty() ? std::wstring(L"Player") : s;
}
static void ReadSess(void* s, MSess& m) {
    m.aumid = HStr(s, 6); m.app = MediaAppName(m.aumid);
    void* pi = nullptr;
    if (SUCCEEDED(GetP(s, 9, &pi)) && pi) {   // GetPlaybackInfo
        int st = 0; ((HRESULT(STDMETHODCALLTYPE*)(void*, int*))Vt(pi)[7])(pi, &st); m.status = st;   // 4 playing, 5 paused
        void* c = nullptr;
        if (SUCCEEDED(GetP(pi, 6, &c)) && c) { m.canNext = GetB(c, 12); m.canPrev = GetB(c, 13); m.canPP = GetB(c, 16) || GetB(c, 6) || GetB(c, 7); m.canShuf = GetB(c, 17); m.canRep = GetB(c, 18); m.canSeek = GetB(c, 20); Rel(c); }
        void* r = nullptr;
        if (SUCCEEDED(GetP(pi, 9, &r)) && r) { int v = 0; ((HRESULT(STDMETHODCALLTYPE*)(void*, int*))Vt(r)[6])(r, &v); m.repeat = v == 2 ? 1 : v == 1 ? 2 : 0; Rel(r); }   // list = all, track = one
        if (SUCCEEDED(GetP(pi, 11, &r)) && r) { BYTE b = 0; ((HRESULT(STDMETHODCALLTYPE*)(void*, BYTE*))Vt(r)[6])(r, &b); m.shuffle = b != 0; Rel(r); }
        Rel(pi);
    }
    void* tl = nullptr;
    if (SUCCEEDED(GetP(s, 8, &tl)) && tl) { INT64 st = GetI64(tl, 6); m.start = st; m.end = std::max<INT64>(0, GetI64(tl, 7) - st); m.pos = std::max<INT64>(0, GetI64(tl, 10) - st); m.updated = GetI64(tl, 11); Rel(tl); }
    void* op = nullptr, *mp = nullptr;
    if (SUCCEEDED(GetP(s, 7, &op)) && Await(op, &mp) && mp) {   // TryGetMediaPropertiesAsync
        m.title = HStr(mp, 6); m.artist = HStr(mp, 9); if (m.artist.empty()) m.artist = HStr(mp, 8);
        Rel(mp);
    }
}
static Gdiplus::Bitmap* ReadCover(void* s) {   // the cover, squared and kept small (128 px): it's only ever shown small or blurred
    Gdiplus::Bitmap* out = nullptr;
    void* op = nullptr, *mp = nullptr, *ref = nullptr;
    if (FAILED(GetP(s, 7, &op)) || !Await(op, &mp) || !mp) return nullptr;
    if (SUCCEEDED(GetP(mp, 15, &ref)) && ref) {   // Thumbnail -> OpenReadAsync -> a stream GDI+ can read
        void* op2 = nullptr, *ras = nullptr;
        if (SUCCEEDED(GetP(ref, 6, &op2)) && Await(op2, &ras) && ras) {
            IStream* st = nullptr;
            if (SUCCEEDED(CreateStreamOverRandomAccessStream((IUnknown*)ras, IID_IStream, (void**)&st)) && st) {
                Gdiplus::Bitmap* b = Gdiplus::Bitmap::FromStream(st);
                if (b && b->GetLastStatus() == Ok && b->GetWidth() > 0 && b->GetHeight() > 0) {
                    UINT w = b->GetWidth(), h = b->GetHeight(), sq = std::min(w, h);
                    out = new Gdiplus::Bitmap(128, 128, PixelFormat32bppPARGB);
                    Graphics g(out); g.SetInterpolationMode(InterpolationModeHighQualityBicubic); g.SetPixelOffsetMode(PixelOffsetModeHalf);
                    g.DrawImage(b, Rect(0, 0, 128, 128), (INT)(w - sq) / 2, (INT)(h - sq) / 2, (INT)sq, (INT)sq, UnitPixel);
                }
                delete b; st->Release();
            }
            Rel(ras);
        }
        Rel(ref);
    }
    Rel(mp);
    return out;
}
static DWORD WINAPI MediaProc(LPVOID) {
    RoInitialize(RO_INIT_MULTITHREADED);
    MSG msg; PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);   // a queue for the events and the buttons
    void* mgr = nullptr, *statics = nullptr; HSTRING cls = nullptr;
    const wchar_t* cn = L"Windows.Media.Control.GlobalSystemMediaTransportControlsSessionManager";
    WindowsCreateString(cn, (UINT32)wcslen(cn), &cls);
    if (cls && SUCCEEDED(RoGetActivationFactory(cls, IID_GsmtcStatics, &statics)) && statics) { void* op = nullptr; if (SUCCEEDED(GetP(statics, 6, &op))) Await(op, &mgr, 5000); Rel(statics); }
    if (cls) WindowsDeleteString(cls);
    if (!mgr) { RoUninitialize(); return 0; }   // no media service (Windows 10 before 1809)
    typedef HRESULT (STDMETHODCALLTYPE* PFN_ADD)(void*, void*, INT64*);
    typedef HRESULT (STDMETHODCALLTYPE* PFN_REM)(void*, INT64);
    MDelegate* del = new MDelegate(0), *delList = new MDelegate(1);
    INT64 tCur = 0, tList = 0;
    ((PFN_ADD)Vt(mgr)[8])(mgr, delList, &tCur);    // CurrentSessionChanged
    ((PFN_ADD)Vt(mgr)[10])(mgr, delList, &tList);  // SessionsChanged
    struct Sub { void* s; INT64 t1, t2; };
    std::vector<Sub> subs;
    int shown = -1;
    std::wstring coverKey, lastTitle, lastId;
    auto resub = [&]() {   // every player, so the list in the flyout and Up next stay right
        for (auto& x : subs) { ((PFN_REM)Vt(x.s)[28])(x.s, x.t1); ((PFN_REM)Vt(x.s)[30])(x.s, x.t2); Rel(x.s); }
        subs.clear(); shown = -1;
        void* list = nullptr;
        if (SUCCEEDED(GetP(mgr, 7, &list)) && list) {
            UINT32 n = 0; ((HRESULT(STDMETHODCALLTYPE*)(void*, UINT32*))Vt(list)[7])(list, &n);
            for (UINT32 i = 0; i < n && i < 12; i++) {
                void* s = nullptr;
                if (FAILED(((HRESULT(STDMETHODCALLTYPE*)(void*, UINT32, void**))Vt(list)[6])(list, i, &s)) || !s) continue;
                Sub x{s, 0, 0};
                ((PFN_ADD)Vt(s)[27])(s, del, &x.t1);   // PlaybackInfoChanged
                ((PFN_ADD)Vt(s)[29])(s, del, &x.t2);   // MediaPropertiesChanged
                subs.push_back(x);
            }
            Rel(list);
        }
    };
    auto send = [&]() {
        MediaSnap* m = new MediaSnap;
        void* cur = nullptr; std::wstring curId;
        if (SUCCEEDED(GetP(mgr, 6, &cur)) && cur) { curId = HStr(cur, 6); Rel(cur); }
        EnterCriticalSection(&g_mcs); std::wstring pick = g_mediaPick; LeaveCriticalSection(&g_mcs);
        for (auto& x : subs) { MSess s; ReadSess(x.s, s); m->s.push_back(s); }
        for (int i = 0; i < (int)m->s.size() && m->cur < 0; i++) if (!pick.empty() && m->s[i].aumid == pick) m->cur = i;   // the one you picked
        for (int i = 0; i < (int)m->s.size() && m->cur < 0; i++) if (m->s[i].aumid == curId) m->cur = i;                   // else the one Windows shows
        for (int i = 0; i < (int)m->s.size() && m->cur < 0; i++) if (m->s[i].status == 4) m->cur = i;
        if (m->cur < 0 && !m->s.empty()) m->cur = 0;
        shown = m->cur;
        if (m->cur >= 0) {
            MSess& c = m->s[m->cur];
            std::wstring key = c.aumid + L"|" + c.title + L"|" + c.artist;
            if (key != coverKey && !c.title.empty()) {
                m->cover = ReadCover(subs[m->cur].s); m->newCover = true;
                if (m->cover) coverKey = key;   // no cover yet (it often arrives a moment after the title): try again next time
            }
            if (!c.title.empty()) {
                bool newSong = c.aumid == lastId && !lastTitle.empty() && c.title != lastTitle;
                if (newSong && c.status == 4 && GetTickCount() - g_mediaCmdTick > 3000) m->why = 2;
                lastId = c.aumid; lastTitle = c.title;
            }
        } else if (!coverKey.empty()) { coverKey.clear(); m->newCover = true; }
        if (!PostMessageW(g_hwnd, WM_APP_MEDIA, 0, (LPARAM)m)) { delete m->cover; delete m; }
    };
    resub(); send();
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == MC_QUIT) break;
        if (msg.message == MC_REFRESH) {   // events come in bursts: gather them, then read once
            Sleep(60);
            MSG m2; bool resubNeeded = msg.wParam == 1;
            while (PeekMessageW(&m2, nullptr, MC_REFRESH, MC_REFRESH, PM_REMOVE)) if (m2.wParam == 1) resubNeeded = true;
            UINT32 n = 0; void* list = nullptr;
            if (SUCCEEDED(GetP(mgr, 7, &list)) && list) { ((HRESULT(STDMETHODCALLTYPE*)(void*, UINT32*))Vt(list)[7])(list, &n); Rel(list); }
            if (resubNeeded || n != subs.size()) resub();   // a player opened or closed
            send();
        } else if (msg.message == MC_CMD && shown >= 0 && shown < (int)subs.size()) {
            void* s = subs[shown].s, *op = nullptr;
            switch ((int)msg.wParam) {
            case MCMD_PLAYPAUSE: GetP(s, 20, &op); break;
            case MCMD_NEXT: GetP(s, 16, &op); break;
            case MCMD_PREV: GetP(s, 17, &op); break;
            case MCMD_SHUFFLE: ((HRESULT(STDMETHODCALLTYPE*)(void*, BYTE, void**))Vt(s)[23])(s, (BYTE)(msg.lParam != 0), &op); break;
            case MCMD_REPEAT: ((HRESULT(STDMETHODCALLTYPE*)(void*, int, void**))Vt(s)[21])(s, (int)msg.lParam, &op); break;
            case MCMD_SEEK: ((HRESULT(STDMETHODCALLTYPE*)(void*, INT64, void**))Vt(s)[24])(s, (INT64)msg.lParam, &op); break;
            }
            if (op) Await(op, nullptr, 1500);
            Sleep(120);   // the player needs a moment before it reports the new state
            send();
        }
    }
    for (auto& x : subs) { ((PFN_REM)Vt(x.s)[28])(x.s, x.t1); ((PFN_REM)Vt(x.s)[30])(x.s, x.t2); Rel(x.s); }
    ((PFN_REM)Vt(mgr)[9])(mgr, tCur); ((PFN_REM)Vt(mgr)[11])(mgr, tList);
    Rel(mgr); del->Release(); delList->Release();
    RoUninitialize();
    return 0;
}
static void MediaStart() {   // started the first time something needs it; idle (no polling) until Windows reports a change
    if (g_mediaThread || g_demo) return;
    g_mediaThread = CreateThread(nullptr, 0, MediaProc, nullptr, 0, &g_mediaTid);
}
static void MediaCmd(int c, LPARAM v = 0) { g_mediaCmdTick = GetTickCount(); if (g_mediaTid) PostThreadMessageW(g_mediaTid, MC_CMD, c, v); }
static void MediaRefresh() { if (g_mediaTid) PostThreadMessageW(g_mediaTid, MC_REFRESH, 0, 0); }
static MSess* MediaCur() { return g_ms.cur >= 0 && g_ms.cur < (int)g_ms.s.size() ? &g_ms.s[g_ms.cur] : nullptr; }
static INT64 MediaPos(const MSess& m) {   // where the song is now: the last reported position plus the time since, while it plays
    INT64 p = m.pos;
    if (m.status == 4 && m.updated > 0) { FILETIME ft; GetSystemTimeAsFileTime(&ft); INT64 now = ((INT64)ft.dwHighDateTime << 32) | ft.dwLowDateTime; p += std::max<INT64>(0, now - m.updated); }
    return m.end > 0 ? std::min(p, m.end) : p;
}
static void RgbToHsl(float r, float g, float b, float& h, float& s, float& l) {
    float mx = std::max(r, std::max(g, b)), mn = std::min(r, std::min(g, b)), d = mx - mn;
    l = (mx + mn) / 2; s = d < 1e-4f ? 0 : d / (1 - fabsf(2 * l - 1)); h = 0;
    if (d >= 1e-4f) { if (mx == r) h = 60 * fmodf((g - b) / d, 6); else if (mx == g) h = 60 * ((b - r) / d + 2); else h = 60 * ((r - g) / d + 4); if (h < 0) h += 360; }
}
static void CoverColours() {   // the cover's colours: an accent for buttons and the bar, and two tints for the glow
    g_coverHas = false;
    if (!g_cover) return;
    BitmapData bd; Rect rc(0, 0, (INT)g_cover->GetWidth(), (INT)g_cover->GetHeight());
    if (g_cover->LockBits(&rc, ImageLockModeRead, PixelFormat32bppARGB, &bd) != Ok) return;
    double acc[3][4] = {};   // whole, left half, right half: weighted r g b and weight
    for (int y = 0; y < rc.Height; y += 2) {
        const BYTE* row = (const BYTE*)bd.Scan0 + y * bd.Stride;
        for (int x = 0; x < rc.Width; x += 2) {
            float b = row[x * 4] / 255.f, g = row[x * 4 + 1] / 255.f, r = row[x * 4 + 2] / 255.f, h, s, l;
            RgbToHsl(r, g, b, h, s, l);
            float w = 0.05f + s * (1 - fabsf(2 * l - 1));   // colourful pixels count more than grey, black or white
            for (int k : {0, x < rc.Width / 2 ? 1 : 2}) { acc[k][0] += r * w; acc[k][1] += g * w; acc[k][2] += b * w; acc[k][3] += w; }
        }
    }
    g_cover->UnlockBits(&bd);
    auto pick = [&](int k, float lMin, float lMax, float sMin) {
        float r = (float)(acc[k][0] / acc[k][3]), g = (float)(acc[k][1] / acc[k][3]), b = (float)(acc[k][2] / acc[k][3]), h, s, l;
        RgbToHsl(r, g, b, h, s, l);
        return Hsl(h, std::max(s, sMin), std::max(lMin, std::min(lMax, l)));
    };
    g_coverAcc = T.dark ? pick(0, 0.52f, 0.66f, 0.45f) : pick(0, 0.38f, 0.48f, 0.45f);
    g_coverC1 = pick(1, 0.35f, 0.6f, 0.3f); g_coverC2 = pick(2, 0.35f, 0.6f, 0.3f);
    g_coverHas = true;
}

// ---------------------------------------------------------------- flyouts: media, Up next, lock keys (Settings › Flyouts)
// Small windows that appear by themselves near the edge of a screen, never take focus, and stay hidden while
// something runs full screen. Same surface as the panel (Solid or Glass); buttons take the cover's colour.
enum { POP_MEDIA, POP_NEXT, POP_LOCK };
extern const char* g_crumb;
static DWORD g_popVolT0 = 0;   // the media flyout was opened by a volume change just now: it shows the volume too
static HWND g_pop = nullptr;
static int g_popKind = -1, g_popW = 0, g_popH = 0, g_popDir = 0;   // g_popDir 1 arriving, -1 leaving, 0 resting
static DWORD g_popT0 = 0, g_popHideAt = 0;
static POINT g_popAt;                    // where it rests
static int g_popSlide = 0;               // pixels it travels while arriving / leaving (signed)
static bool g_popShown = false, g_popHover = false, g_popGlass = false, g_popList = false, g_popLv = false;
static int g_popHot = -1, g_popDrag = -1; static float g_popSeek = 0;   // g_popDrag 7: dragging the progress bar
static int g_popLockKey = 0; static bool g_popLockOn = false; static DWORD g_popLockT0 = 0;
static bool g_insertOn = false;          // Insert / overwrite: no light for it, so Knob keeps count
static Canvas g_cvPop;
static Gdiplus::Bitmap* g_glowFull = nullptr, *g_glowRise = nullptr;   // the cover, very blurred (and faded upwards), built once per cover
static std::vector<std::pair<int, RectF>> g_popHits;
static bool g_popTry = false;            // Settings › Flyouts › Try: shows even when nothing is playing
static MSess g_popNone;                  // what Try shows then
static const wchar_t* LOCK_TXT[4][3] = {{L"Caps Lock", L"Caps Lock is on", L"Caps Lock is off"}, {L"Num Lock", L"Num Lock is on", L"Num Lock is off"},
                                       {L"Scroll Lock", L"Scroll Lock is on", L"Scroll Lock is off"}, {L"Insert", L"Overwrite mode", L"Insert mode"}};
static int PopDur() { int d = std::max(0, std::min(2000, g_fAnimMs)); return g_popKind == POP_LOCK ? std::min(d, 200) : d; }   // a lock-key flyout only needs a quick slide
static Color PopAcc() { return g_fCover && g_coverHas ? g_coverAcc : T.accent; }
static bool PopAnimatedGlow() { return g_popKind == POP_MEDIA && (g_fGlow == 3 || g_fGlow == 4 || g_fGlow == 5); }
static void BoxBlur(std::vector<UINT32>& px, int w, int h, int r, int passes) {   // premultiplied ARGB, separable box blur (a few passes look Gaussian)
    if (r < 1 || w < 2 || h < 2) return;
    std::vector<UINT32> tmp(px.size());
    auto pass = [&](const UINT32* src, UINT32* dst, int n, int stride, int count, int lineStride) {
        for (int line = 0; line < count; line++) {
            const UINT32* s = src + line * lineStride; UINT32* d = dst + line * lineStride;
            int acc[4] = {}, div = 2 * r + 1;
            auto add = [&](int i, int sign) { i = std::max(0, std::min(n - 1, i)); UINT32 c = s[i * stride]; for (int k = 0; k < 4; k++) acc[k] += sign * (int)((c >> (k * 8)) & 255); };
            for (int i = -r; i <= r; i++) add(i, 1);
            for (int i = 0; i < n; i++) {
                d[i * stride] = (UINT32)(acc[0] / div) | ((UINT32)(acc[1] / div) << 8) | ((UINT32)(acc[2] / div) << 16) | ((UINT32)(acc[3] / div) << 24);
                add(i + r + 1, 1); add(i - r, -1);
            }
        }
    };
    for (int p = 0; p < passes; p++) { pass(px.data(), tmp.data(), w, 1, h, w); pass(tmp.data(), px.data(), h, w, w, 1); }
}
static Gdiplus::Bitmap* BitmapFrom(const std::vector<UINT32>& px, int w, int h) {
    Gdiplus::Bitmap* b = new Gdiplus::Bitmap(w, h, PixelFormat32bppPARGB);
    BitmapData bd; Rect rc(0, 0, w, h);
    if (b->LockBits(&rc, ImageLockModeWrite, PixelFormat32bppPARGB, &bd) == Ok) {
        for (int y = 0; y < h; y++) memcpy((BYTE*)bd.Scan0 + y * bd.Stride, px.data() + y * w, w * 4);
        b->UnlockBits(&bd);
    }
    return b;
}
static void BuildGlows() {   // cover -> 8 px -> 64 px, blurred: a soft wash of its colours (cheap to draw at any size)
    delete g_glowFull; delete g_glowRise; g_glowFull = g_glowRise = nullptr;
    const int W = 64, H = 64;
    std::vector<UINT32> px(W * H);
    {
        Gdiplus::Bitmap small(8, 8, PixelFormat32bppPARGB);
        { Graphics g(&small); g.SetInterpolationMode(InterpolationModeHighQualityBilinear);
          if (g_cover) g.DrawImage(g_cover, Rect(0, 0, 8, 8));
          else { Color a = PopAcc(); LinearGradientBrush lb(PointF(0, 0), PointF(8, 8), a, Color(255, a.GetB(), a.GetR(), a.GetG())); g.FillRectangle(&lb, 0, 0, 8, 8); } }
        Gdiplus::Bitmap big(W, H, PixelFormat32bppPARGB);
        { Graphics g(&big); g.SetInterpolationMode(InterpolationModeBilinear); g.SetPixelOffsetMode(PixelOffsetModeHalf);
          ImageAttributes ia; ia.SetWrapMode(WrapModeTileFlipXY); g.DrawImage(&small, Rect(0, 0, W, H), 0, 0, 8, 8, UnitPixel, &ia); }
        BitmapData bd; Rect rc(0, 0, W, H);
        if (big.LockBits(&rc, ImageLockModeRead, PixelFormat32bppPARGB, &bd) == Ok) { for (int y = 0; y < H; y++) memcpy(px.data() + y * W, (BYTE*)bd.Scan0 + y * bd.Stride, W * 4); big.UnlockBits(&bd); }
    }
    BoxBlur(px, W, H, 4, 2);
    for (auto& c : px) {   // a little more colour, like the design's saturate()
        int b = c & 255, g = (c >> 8) & 255, r = (c >> 16) & 255, l = (r * 3 + g * 6 + b) / 10;
        auto k = [&](int v) { return std::max(0, std::min(255, l + (v - l) * 13 / 10)); };
        c = 0xFF000000u | (k(r) << 16) | (k(g) << 8) | k(b);
    }
    g_glowFull = BitmapFrom(px, W, H);
    for (int y = 0; y < H; y++) {   // rising: strong at the bottom, gone by 85% of the way up
        float up = 1 - y / (float)(H - 1), m = up <= .2f ? 1.f : up >= .85f ? 0.f : 1 - (up - .2f) / .65f;
        for (int x = 0; x < W; x++) { UINT32 c = px[y * W + x]; UINT32 o = 0; for (int k = 0; k < 4; k++) o |= (UINT32)(((c >> (k * 8)) & 255) * m) << (k * 8); px[y * W + x] = o; }
    }
    g_glowRise = BitmapFrom(px, W, H);
}
static void DrawFaded(Graphics& g, Gdiplus::Bitmap* b, RectF r, float alpha) {
    if (!b || alpha <= 0) return;
    ColorMatrix cm = {{{1, 0, 0, 0, 0}, {0, 1, 0, 0, 0}, {0, 0, 1, 0, 0}, {0, 0, 0, alpha, 0}, {0, 0, 0, 0, 1}}};
    ImageAttributes ia; ia.SetColorMatrix(&cm); ia.SetWrapMode(WrapModeTileFlipXY);
    g.SetInterpolationMode(InterpolationModeBilinear);
    float bw = (float)b->GetWidth(), bh = (float)b->GetHeight();
    float cx0 = 0, cy0 = 0, cx1 = (float)std::max(1, g_popW), cy1 = (float)std::max(1, g_popH);   // the flyout's canvas
    float x0 = std::max(r.X, cx0), y0 = std::max(r.Y, cy0), x1 = std::min(r.X + r.Width, cx1), y1 = std::min(r.Y + r.Height, cy1);
    if (x1 <= x0 || y1 <= y0) return;
    float sx = (x0 - r.X) / r.Width * bw, sy = (y0 - r.Y) / r.Height * bh, sw = (x1 - x0) / r.Width * bw, sh = (y1 - y0) / r.Height * bh;
    g.DrawImage(b, RectF(x0, y0, x1 - x0, y1 - y0), sx, sy, sw, sh, UnitPixel, &ia);
}
static std::wstring Mmss(INT64 t100ns) { int s = (int)(t100ns / 10000000); wchar_t b[16]; swprintf(b, 16, L"%d:%02d", s / 60, s % 60); return b; }
static std::map<std::wstring, Gdiplus::Bitmap*> g_mediaIcons;   // the player's own icon by app id (null: it has none we can find)
static Gdiplus::Bitmap* MediaIcon(const std::wstring& aumid) {
    if (aumid.empty()) return nullptr;
    auto it = g_mediaIcons.find(aumid); if (it != g_mediaIcons.end()) return it->second;
    Gdiplus::Bitmap* out = nullptr;
    IShellItemImageFactory* fac = nullptr;
    if (SUCCEEDED(SHCreateItemFromParsingName((L"shell:AppsFolder\\" + aumid).c_str(), nullptr, IID_PPV_ARGS(&fac))) && fac) {
        HBITMAP hb = nullptr;
        if (SUCCEEDED(fac->GetImage(SIZE{64, 64}, SIIGBF_ICONONLY | SIIGBF_BIGGERSIZEOK, &hb)) && hb) {
            DIBSECTION ds = {};
            if (GetObject(hb, sizeof(ds), &ds) >= (int)sizeof(BITMAP) && ds.dsBm.bmBitsPixel == 32 && ds.dsBm.bmBits && ds.dsBm.bmWidth > 0 && ds.dsBm.bmWidth <= 1024 && ds.dsBm.bmHeight > 0 && ds.dsBm.bmHeight <= 1024) {
                int w = ds.dsBm.bmWidth, h = ds.dsBm.bmHeight, st = ds.dsBm.bmWidthBytes; const BYTE* bits = (const BYTE*)ds.dsBm.bmBits;
                bool bottomUp = ds.dsBmih.biHeight > 0;
                std::vector<UINT32> px((size_t)w * h);   // copied row by row: no borrowed memory, no odd strides
                for (int y = 0; y < h; y++) memcpy(px.data() + (size_t)y * w, bits + (size_t)(bottomUp ? h - 1 - y : y) * st, (size_t)w * 4);
                out = BitmapFrom(px, w, h);
                if (out && out->GetLastStatus() != Ok) { delete out; out = nullptr; }
            }
            DeleteObject(hb);
        }
        fac->Release();
    }
    g_mediaIcons[aumid] = out;
    return out;
}
static void PopGlow(Graphics& g, float W, float H) {   // the cover glow behind the media flyout
    bool lt = !T.dark; Color a = PopAcc();
    float t = GetTickCount() / 1000.f;
    switch (g_fGlow) {
    case 1: DrawFaded(g, g_glowRise, RectF(-0.2f * W, 0.3f * H, 1.4f * W, 1.1f * H), lt ? .3f : .34f); break;   // rising glow
    case 2: DrawFaded(g, g_glowFull, RectF(-0.15f * W, -0.3f * H, 1.3f * W, 1.6f * H), lt ? .38f : .45f); break;   // blurred cover
    case 3: {   // aurora: two soft bands of the cover's colours drifting slowly
        DrawFaded(g, g_glowFull, RectF(-0.2f * W, -0.2f * H, 1.4f * W, 1.5f * H), .18f);
        auto blob = [&](float cx, float cy, float rw, float rh, float rot, Color c, float al) {
            GraphicsPath p; p.AddEllipse(-rw / 2, -rh / 2, rw, rh);
            PathGradientBrush pb(&p); Color mid((BYTE)(al * 255), c.GetR(), c.GetG(), c.GetB()), edge(0, c.GetR(), c.GetG(), c.GetB());
            pb.SetCenterColor(mid); int n = 1; pb.SetSurroundColors(&edge, &n);
            Matrix m; m.Translate(cx, cy); m.Rotate(rot); GraphicsState st = g.Save(); g.MultiplyTransform(&m); g.FillPath(&pb, &p); g.Restore(st);
        };
        auto ease = [](float x) { return 0.5f - 0.5f * cosf(x * 3.14159265f); };
        float p1 = ease(fmodf(t / 7, 2) < 1 ? fmodf(t / 7, 1) : 1 - fmodf(t / 7, 1)), p2 = ease(fmodf(t / 9, 2) < 1 ? fmodf(t / 9, 1) : 1 - fmodf(t / 9, 1));
        Color c2 = g_coverHas ? g_coverC2 : a; float h2, s2, l2; RgbToHsl(c2.GetR() / 255.f, c2.GetG() / 255.f, c2.GetB() / 255.f, h2, s2, l2);
        blob(W * (0.15f + 0.45f * 0.7f * p1), H * (0.5f + 0.1f - 0.18f * p1), 0.9f * W, 1.3f * H, -12 + 30 * p1, g_coverHas ? g_coverC1 : a, lt ? .3f : .45f);
        blob(W * (0.85f - 0.5f * 0.7f * p2), H * (0.5f - 0.06f + 0.18f * p2), 0.9f * W, 1.3f * H, 10 - 26 * p2, Hsl(h2 + 35, std::max(s2, .4f), l2), lt ? .3f : .45f);
        break;
    }
    case 4: {   // edge light: a line in the cover's colour travelling round the edge, with a soft glow around it
        DrawFaded(g, g_glowFull, RectF(-0.2f * W, -0.2f * H, 1.4f * W, 1.4f * H), .14f);
        float head = fmodf(t / 3.2f, 1.f) * 360.f, rad = S(8), cx = W / 2, cy = H / 2;
        std::vector<PointF> pts;   // the rounded edge, one point every few pixels
        auto arc = [&](float ax, float ay, float a0) { for (int i = 0; i <= 6; i++) { float an = (a0 + i * 15) * 3.14159265f / 180; pts.push_back(PointF(ax + rad * cosf(an), ay + rad * sinf(an))); } };
        float in = S(1);
        auto line = [&](PointF p0, PointF p1) { int n = std::max(2, (int)(hypotf(p1.X - p0.X, p1.Y - p0.Y) / S(4))); for (int i = 0; i < n; i++) pts.push_back(PointF(p0.X + (p1.X - p0.X) * i / n, p0.Y + (p1.Y - p0.Y) * i / n)); };
        line(PointF(in + rad, in), PointF(W - in - rad, in)); arc(W - in - rad, in + rad, 270);
        line(PointF(W - in, in + rad), PointF(W - in, H - in - rad)); arc(W - in - rad, H - in - rad, 0);
        line(PointF(W - in - rad, H - in), PointF(in + rad, H - in)); arc(in + rad, H - in - rad, 90);
        line(PointF(in, H - in - rad), PointF(in, in + rad)); arc(in + rad, in + rad, 180);
        auto f = [](float u) { return u < .45f ? 0.f : u < .68f ? (u - .45f) / .23f * .55f : u < .8f ? .55f + (u - .68f) / .12f * .45f : u < .9f ? 1 - (u - .8f) / .1f * .45f : .55f * (1 - (u - .9f) / .1f); };
        for (int pass = 0; pass < 4; pass++) {   // three wide faint strokes for the glow, then the line
            static const float wd[4] = {12, 8, 5, 2}, al[4] = {.12f, .16f, .22f, .55f};
            for (size_t i = 0; i + 1 < pts.size() + 1; i++) {
                PointF p0 = pts[i], p1 = pts[(i + 1) % pts.size()];
                float ang = atan2f((p0.X + p1.X) / 2 - cx, -((p0.Y + p1.Y) / 2 - cy)) * 180 / 3.14159265f; if (ang < 0) ang += 360;
                float u = fmodf(ang - head + 720.f, 360.f) / 360.f, k = f(u) * al[pass];
                if (k < 0.01f) continue;
                Pen pen(Color((BYTE)(k * 255), a.GetR(), a.GetG(), a.GetB()), S(wd[pass])); pen.SetStartCap(LineCapRound); pen.SetEndCap(LineCapRound);
                g.DrawLine(&pen, p0, p1);
            }
        }
        break;
    }
    case 5: {   // pulse with the music: 22 blurred bars rising from the bottom edge (zero = the edge)
        DrawFaded(g, g_glowFull, RectF(-0.2f * W, -0.2f * H, 1.4f * W, 1.4f * H), .1f);
        const int n = 22, bw = 88, bh = 40; std::vector<UINT32> px(bw * bh, 0);
        for (int k = 0; k < n; k++) {
            float v = LvBar(k, n) * 0.72f; int top = bh - (int)(v * bh), x0 = k * bw / n, x1 = (k + 1) * bw / n - 1;
            for (int y = std::max(0, top); y < bh; y++) {
                float fy = (bh - y) / (float)std::max(1, bh - top), m = 1 - 0.6f * fy;   // full at the bottom, 40% at the top of the bar
                UINT32 c = ((UINT32)(255 * m) << 24) | ((UINT32)(a.GetR() * m) << 16) | ((UINT32)(a.GetG() * m) << 8) | (UINT32)(a.GetB() * m);
                for (int x = x0; x < x1; x++) px[y * bw + x] = c;
            }
        }
        BoxBlur(px, bw, bh, 2, 2);
        Gdiplus::Bitmap* b = BitmapFrom(px, bw, bh);
        DrawFaded(g, b, RectF(0.02f * W, 0, 0.96f * W, H), lt ? .5f : .6f);
        delete b;
        break;
    }
    }
}
static void PopIconBtn(Graphics& g, int id, RectF r, const char* icon, Color c, bool enabled, float size = 15) {
    if (enabled) g_popHits.push_back({id, r});
    if (enabled && g_popHot == id) FillRound(g, T.subtle2, r, S(6));
    Icon(g, icon, S(size), enabled ? c : WithAlpha(c, 77), r.X + r.Width / 2, r.Y + r.Height / 2);
}
struct PopCacheKey { int gen, glow, w, h, rad; bool lt; DWORD acc; };
static Gdiplus::Bitmap* g_popGlowBmp = nullptr, *g_popCoverBmp = nullptr; static PopCacheKey g_popGlowK = {}, g_popCoverK = {};
static void PopCacheFree() { delete g_popGlowBmp; g_popGlowBmp = nullptr; delete g_popCoverBmp; g_popCoverBmp = nullptr; }
static void PaintPop(HDC hdc) {
    int w = g_popW, h = g_popH;
    if (w <= 0 || h <= 0) return;
    void* bits = nullptr;
    HBITMAP dib = CanvasFor(g_cvPop, hdc, w, h, &bits);
    if (!dib) return;
    HDC mdc = CreateCompatibleDC(hdc); HGDIOBJ old = SelectObject(mdc, dib);
    g_popHits.clear();
    {
        Gdiplus::Bitmap canvas(w, h, w * 4, PixelFormat32bppPARGB, (BYTE*)bits);
        Graphics g(&canvas);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetTextRenderingHint(g_popGlass ? TextRenderingHintAntiAliasGridFit : TextRenderingHintClearTypeGridFit);
        g.SetPixelOffsetMode(PixelOffsetModeHalf);
        g.SetCompositingMode(CompositingModeSourceCopy);
        { Color sf = g_popGlass ? T.surface : (T.dark ? (g_popKind == POP_MEDIA ? Color(255, 22, 22, 22) : Color(255, 32, 32, 32)) : Color(255, 243, 243, 243)); SolidBrush b(sf); g.FillRectangle(&b, 0, 0, w, h); }
        g.SetCompositingMode(CompositingModeSourceOver);
        float W = (float)w, H = (float)h;
        Color acc = PopAcc();
        Font fT(g_famSemi, S(13.5f), FontStyleRegular, UnitPixel), fA(g_famText, S(12), FontStyleRegular, UnitPixel), fTime(g_famText, S(10.5f), FontStyleRegular, UnitPixel);
        Font fLbl(g_famSemi, S(11.5f), FontStyleRegular, UnitPixel), fLock(g_famSemi, S(13.5f), FontStyleBold, UnitPixel), fChip(g_famText, S(11), FontStyleRegular, UnitPixel);
        MSess* m = MediaCur();
        if (!m && g_popTry) { g_popNone = MSess(); g_popNone.title = L"Nothing is playing"; g_popNone.artist = L"Play something and it shows up here"; g_popNone.app = L"Knob"; m = &g_popNone; }
        auto cover = [&](RectF r, float rad, bool shadow) {
            if (shadow) { FillRound(g, Color(40, 0, 0, 0), RectF(r.X, r.Y + S(3), r.Width, r.Height + S(1)), rad + S(1)); FillRound(g, Color(30, 0, 0, 0), RectF(r.X - S(1), r.Y + S(2), r.Width + S(2), r.Height + S(4)), rad + S(2)); }
            GraphicsPath p; AddRound(p, r, rad);
            if (g_cover) {   // scaled and rounded once per cover and size, then only copied
                int cw = std::max(1, (int)lroundf(r.Width)), ch = std::max(1, (int)lroundf(r.Height));
                PopCacheKey k = {g_coverGen, 0, cw, ch, (int)lroundf(rad), false, 0};
                if (!g_popCoverBmp || memcmp(&k, &g_popCoverK, sizeof k) != 0) {
                    delete g_popCoverBmp; g_popCoverBmp = new Gdiplus::Bitmap(cw, ch, PixelFormat32bppPARGB); g_popCoverK = k;
                    Graphics cg(g_popCoverBmp); cg.SetSmoothingMode(SmoothingModeAntiAlias); cg.SetPixelOffsetMode(PixelOffsetModeHalf); cg.SetInterpolationMode(InterpolationModeHighQualityBicubic);
                    GraphicsPath cp; AddRound(cp, RectF(0, 0, (float)cw, (float)ch), rad); cg.SetClip(&cp); cg.DrawImage(g_cover, RectF(0, 0, (float)cw, (float)ch));
                }
                g.DrawImage(g_popCoverBmp, Rect((int)lroundf(r.X), (int)lroundf(r.Y), cw, ch), 0, 0, cw, ch, UnitPixel);
            }
            else { LinearGradientBrush lb(PointF(r.X, r.Y), PointF(r.X + r.Width, r.Y + r.Height), acc, WithAlpha(acc, 140)); g.FillPath(&lb, &p); Icon(g, "mPlay", r.Width * .4f, Color(230, 255, 255, 255), r.X + r.Width / 2, r.Y + r.Height / 2); }
            Pen edge(Color(20, 255, 255, 255), S(1)); g.DrawPath(&edge, &p);
        };
        auto textCol = [&](const std::wstring& t, const std::wstring& a, RectF r, bool centre) {
            StringFormat sf; sf.SetAlignment(centre ? StringAlignmentCenter : StringAlignmentNear); sf.SetTrimming(StringTrimmingEllipsisCharacter); sf.SetFormatFlags(StringFormatFlagsNoWrap);
            SolidBrush b1(T.t1), b2(T.t2);
            g.DrawString(t.c_str(), -1, &fT, RectF(r.X, r.Y, r.Width, S(19)), &sf, &b1);
            g.DrawString(a.c_str(), -1, &fA, RectF(r.X, r.Y + S(20), r.Width, S(17)), &sf, &b2);
        };
        if (g_popKind == POP_MEDIA && m) {
            bool compact = g_fLayout == 1;
            INT64 pos = g_popDrag == 7 ? (INT64)(g_popSeek * m->end) : MediaPos(*m);
            float frac = m->end > 0 ? std::max(0.f, std::min(1.f, (float)pos / m->end)) : 0.f;
            g_crumb = "flyout glow";
            if (g_fGlow == 1 || g_fGlow == 2) {   // the still looks are drawn once, then copied
                PopCacheKey k = {g_coverGen, g_fGlow, w, h, 0, !T.dark, (DWORD)PopAcc().GetValue()};
                if (!g_popGlowBmp || memcmp(&k, &g_popGlowK, sizeof k) != 0) {
                    delete g_popGlowBmp; g_popGlowBmp = new Gdiplus::Bitmap(w, h, PixelFormat32bppPARGB); g_popGlowK = k;
                    Graphics cg(g_popGlowBmp); cg.SetSmoothingMode(SmoothingModeAntiAlias); PopGlow(cg, W, H);
                }
                g.DrawImage(g_popGlowBmp, Rect(0, 0, w, h), 0, 0, w, h, UnitPixel);
            } else if (g_fGlow) PopGlow(g, W, H);
            g_crumb = "flyout body";
            auto bar = [&](RectF tr) {   // the track, the part played, and the knob when the mouse is on it
                g_popHits.push_back({7, RectF(tr.X - S(4), tr.Y - S(6), tr.Width + S(8), tr.Height + S(12))});
                FillRound(g, T.track, tr, tr.Height / 2);
                if (frac > 0) FillRound(g, acc, RectF(tr.X, tr.Y, std::max(tr.Height, tr.Width * frac), tr.Height), tr.Height / 2);
                if (g_popHot == 7 || g_popDrag == 7) { float kx = tr.X + tr.Width * frac, cy = tr.Y + tr.Height / 2; SolidBrush sh(Color(70, 0, 0, 0)); g.FillEllipse(&sh, kx - S(6), cy - S(5), S(12), S(12)); SolidBrush wb(Color(255, 255, 255, 255)); g.FillEllipse(&wb, kx - S(6), cy - S(6), S(12), S(12)); }
            };
            bool playing = m->status == 4;
            auto volPill = [&](float rightEdge, float y) {   // after a volume change: the speaker and its level, for a moment
                if (!g_popVolT0 || GetTickCount() - g_popVolT0 > 2200) return;
                int di = DefaultOut(); if (di < 0) return;
                bool mu = g_out[di].mute; int pc = (int)lround(g_out[di].level * 100);
                std::wstring t = mu ? L"Mute" : std::to_wstring(pc);
                float tw = TextW(g, t.c_str(), &fChip), w = S(8) + S(14) + S(5) + std::max(tw, S(16)) + S(9);
                RectF pr(rightEdge - w, y, w, S(20));
                FillRound(g, T.subtle2, pr, S(10));
                Glyph(g, mu ? G_MUTE : G_VOLUME, g_fIcon, T.t1, pr.X + S(8) + S(7), pr.Y + S(10));
                Text(g, t.c_str(), &fChip, T.t1, RectF(pr.X + S(8) + S(14) + S(5), pr.Y, std::max(tw, S(16)) + S(4), pr.Height));
            };
            if (compact) volPill(W - S(12), S(3));
            else volPill(W - S(14) - (g_fStayMs == 0 ? S(30) : 0), S(10));
            if (!compact) {
                cover(RectF(S(14), S(18), S(58), S(58)), S(10), true);
                textCol(m->title, m->artist, RectF(S(84), S(36), W - S(84) - S(14), S(40)), g_fCentre);
                float cy = S(103), x = (W - S(198)) / 2;
                PopIconBtn(g, 1, RectF(x, cy - S(16), S(32), S(32)), "mShuf", m->shuffle && m->canShuf ? acc : T.t2, m->canShuf);
                if (m->shuffle && m->canShuf) { SolidBrush d(acc); g.FillEllipse(&d, x + S(14), cy + S(12), S(4), S(4)); }
                x += S(38); PopIconBtn(g, 2, RectF(x, cy - S(16), S(32), S(32)), "mPrev", T.t2, m->canPrev);
                x += S(38);
                { RectF pr(x + S(4), cy - S(19), S(38), S(38)); g_popHits.push_back({3, pr}); SolidBrush pb(g_popHot == 3 ? Color(255, std::min(255, acc.GetR() + 20), std::min(255, acc.GetG() + 20), std::min(255, acc.GetB() + 20)) : acc); g.FillEllipse(&pb, pr);
                  Icon(g, playing ? "mPause" : "mPlay", S(16), Color(255, 255, 255, 255), pr.X + pr.Width / 2 + (playing ? 0 : S(1)), pr.Y + pr.Height / 2); }
                x += S(52); PopIconBtn(g, 4, RectF(x, cy - S(16), S(32), S(32)), "mNext", T.t2, m->canNext);
                x += S(38); PopIconBtn(g, 5, RectF(x, cy - S(16), S(32), S(32)), "mRep", m->repeat && m->canRep ? acc : T.t2, m->canRep);
                if (m->repeat && m->canRep) { SolidBrush d(acc); g.FillEllipse(&d, x + S(14), cy + S(12), S(4), S(4)); if (m->repeat == 2) Text(g, L"1", &fChip, acc, RectF(x + S(20), cy - S(16), S(10), S(12))); }
                if (g_fPlayer) {   // which player; a list to switch when there's more than one
                    bool many = g_ms.s.size() > 1;
                    float tw2 = std::min(S(60), TextW(g, m->app.c_str(), &fChip)), cw = S(4) + S(13) + S(5) + tw2 + (many ? S(16) : 0) + S(7);
                    RectF chip(W - S(14) - cw, cy - S(10), cw, S(20));
                    if (many) g_popHits.push_back({6, chip});
                    FillRound(g, g_popList || (many && g_popHot == 6) ? T.subtle3 : T.subtle2, chip, S(10));
                    static const DWORD tint[6] = {0xd9532b, 0x5865f2, 0x1a9f48, 0x2a7de1, 0xe07a10, 0x8e44ad};
                    unsigned hsh = 0; for (wchar_t ch : m->app) hsh = hsh * 31 + ch;
                    RectF ic(chip.X + S(3.5f), chip.Y + S(3), S(14), S(14));
                    g_crumb = "flyout player icon"; Gdiplus::Bitmap* pic = MediaIcon(m->aumid); g_crumb = "flyout player chip";
                    if (pic) DrawAppIcon(g, pic, ic);
                    else { FillRound(g, Hex(tint[hsh % 6]), ic, S(3)); Font fI(g_famSemi, S(8), FontStyleBold, UnitPixel); Text(g, m->app.substr(0, 1).c_str(), &fI, Color(255, 255, 255, 255), ic, StringAlignmentCenter); }
                    Text(g, m->app.c_str(), &fChip, g_popList || g_popHot == 6 ? T.t1 : T.t2, RectF(ic.X + S(18), chip.Y, tw2 + S(2), chip.Height));
                    if (many) Icon(g, "chevron", S(11), T.t2, chip.X + chip.Width - S(7) - S(5.5f), chip.Y + chip.Height / 2, g_popList ? 180.f : 0.f);
                }
                std::wstring a0 = Mmss(pos), a1 = Mmss(m->end);
                float tw0 = TextW(g, L"00:00", &fTime);
                Text(g, a0.c_str(), &fTime, T.t3, RectF(S(14), S(130), tw0, S(16)));
                Text(g, a1.c_str(), &fTime, T.t3, RectF(W - S(14) - tw0, S(130), tw0, S(16)), StringAlignmentFar);
                if (m->canSeek || m->end > 0) bar(RectF(S(14) + tw0 + S(8), S(136), W - 2 * (S(14) + tw0 + S(8)), S(4)));
                if (g_fStayMs == 0) {   // stays until you close it
                    RectF xr(W - S(10) - S(22), S(8), S(22), S(22)); g_popHits.push_back({8, xr});
                    if (g_popHot == 8) FillRound(g, T.subtle2, xr, S(6));
                    Icon(g, "exit", S(10), g_popHot == 8 ? T.t1 : T.t3, xr.X + S(11), xr.Y + S(11));
                }
                if (g_popList) {   // the players, opening upwards over the cover
                    float rowH = S(30), lw = S(190);
                    for (auto& s2 : g_ms.s) lw = std::max(lw, std::min(S(300), S(28) + TextW(g, (s2.app + L" · " + s2.title).c_str(), &fA)));
                    int n = (int)g_ms.s.size(), fit = std::max(1, (int)((cy - S(10) - S(6) - S(8)) / rowH));
                    RectF box(W - S(14) - lw, std::max(S(6), cy - S(10) - S(6) - std::min(n, fit) * rowH - S(8)), lw, std::min(n, fit) * rowH + S(8));
                    FillRound(g, Color(90, 0, 0, 0), RectF(box.X, box.Y + S(3), box.Width, box.Height + S(2)), S(9));
                    FillRound(g, T.dark ? Color(255, 44, 44, 44) : Color(255, 251, 251, 251), box, S(8));
                    for (int i = 0; i < std::min(n, fit); i++) {
                        RectF it(box.X + S(4), box.Y + S(4) + i * rowH, box.Width - S(8), rowH);
                        g_popHits.push_back({100 + i, it});
                        if (g_popHot == 100 + i) FillRound(g, T.subtle2, it, S(5));
                        bool sel = i == g_ms.cur;
                        Text(g, (g_ms.s[i].app + L" · " + g_ms.s[i].title).c_str(), sel ? &fLbl : &fA, sel ? acc : T.t1, RectF(it.X + S(10), it.Y, it.Width - S(14), it.Height));
                    }
                }
            } else {   // compact: one slim row
                cover(RectF(S(12), S(10), S(44), S(44)), S(8), false);
                float cx0 = W - S(12) - S(106);
                textCol(m->title, m->artist, RectF(S(66), S(13), cx0 - S(66) - S(10), S(40)), false);
                float cy = S(32), x = cx0;
                PopIconBtn(g, 2, RectF(x, cy - S(16), S(32), S(32)), "mPrev", T.t2, m->canPrev);
                x += S(36);
                { RectF pr(x, cy - S(17), S(34), S(34)); g_popHits.push_back({3, pr}); SolidBrush pb(acc); g.FillEllipse(&pb, pr); Icon(g, playing ? "mPause" : "mPlay", S(15), Color(255, 255, 255, 255), pr.X + pr.Width / 2 + (playing ? 0 : S(1)), pr.Y + pr.Height / 2); }
                x += S(38); PopIconBtn(g, 4, RectF(x, cy - S(16), S(32), S(32)), "mNext", T.t2, m->canNext);
                if (m->end > 0) bar(RectF(S(12), H - S(7) - S(2), W - S(24), S(4)));
            }
        } else if (g_popKind == POP_NEXT && m) {   // Up next: the song that just started on its own
            RectF nx(S(12), (H - S(22)) / 2, S(22), S(22)); SolidBrush nb(acc); g.FillEllipse(&nb, nx);
            Icon(g, "mNext", S(13), Color(255, 255, 255, 255), nx.X + S(11), nx.Y + S(11));
            float lw = TextW(g, L"Up next", &fLbl);
            Text(g, L"Up next", &fLbl, T.t2, RectF(S(44), 0, lw + S(4), H));
            float cx = S(44) + lw + S(13);
            cover(RectF(cx, (H - S(32)) / 2, S(32), S(32)), S(7), false);
            textCol(m->title, m->artist, RectF(cx + S(42), (H - S(37)) / 2, W - cx - S(42) - S(16), S(40)), false);
        } else if (g_popKind == POP_LOCK) {   // lock keys: the lock closes or opens, the words change, the bar grows or shrinks
            float sp = std::max(0.4f, std::min(1.f, g_fAnimMs / 500.f)), t = (GetTickCount() - g_popLockT0) / 1000.f, ta = std::max(0.f, t - 0.05f * sp);   // sp: follows the Animation time, never slower than before
            bool still = g_fAnimMs == 0;
            auto backOut = [](float x) { x = std::max(0.f, std::min(1.f, x)); float c1 = 1.70158f, c3 = c1 + 1; return 1 + c3 * powf(x - 1, 3) + c1 * powf(x - 1, 2); };
            auto easeIO = [](float x) { x = std::max(0.f, std::min(1.f, x)); return x < .5f ? 4 * x * x * x : 1 - powf(-2 * x + 2, 3) / 2; };
            float ks = still ? 1 : backOut(ta / (0.32f * sp)), kt = still ? 1 : easeIO(ta / (0.2f * sp)), kb = still ? 1 : easeIO(ta / (0.24f * sp));
            bool on = g_popLockOn;
            {   // the lock, 20 x 22 units
                float u = S(1), lx = S(14), ly = (H - S(22)) / 2;
                float openA = on ? 25 * (1 - ks) : 25 * ks;   // degrees the shackle is turned open
                GraphicsState st = g.Save();
                Matrix mx; mx.Translate(lx + 14 * u, ly + 10 * u); mx.Rotate(openA); mx.Translate(-14 * u, -10 * u + (openA > 0.5f ? -u * openA / 25 : 0)); g.MultiplyTransform(&mx);
                Pen sp(T.t1, S(1.8f)); sp.SetStartCap(LineCapRound); sp.SetEndCap(LineCapRound);
                GraphicsPath sh; sh.AddLine(6 * u, 10 * u, 6 * u, 7 * u); sh.AddArc(6 * u, 3 * u, 8 * u, 8 * u, 180, 180); sh.AddLine(14 * u, 7 * u, 14 * u, 10 * u);
                g.DrawPath(&sp, &sh);
                g.Restore(st);
                FillRound(g, T.t1, RectF(lx + 3.5f * u, ly + 10 * u, 13 * u, 10 * u), 2.6f * u);
                SolidBrush hole(g_popGlass ? Color(255, T.dark ? 40 : 230, T.dark ? 40 : 230, T.dark ? 40 : 230) : (T.dark ? Color(255, 32, 32, 32) : Color(255, 243, 243, 243)));
                g.FillEllipse(&hole, lx + 8.6f * u, ly + 13.6f * u, 2.8f * u, 2.8f * u);
            }
            int k = std::max(0, std::min(3, g_popLockKey));
            const wchar_t* now = on ? LOCK_TXT[k][1] : LOCK_TXT[k][2], *was = on ? LOCK_TXT[k][2] : LOCK_TXT[k][1];
            float tx = S(14) + S(20) + S(11), tw = W - tx - S(18), ty = (H - S(24)) / 2;
            auto word = [&](const wchar_t* s, float alpha, float dy) {
                if (alpha <= 0.01f) return;
                StringFormat sf; sf.SetAlignment(StringAlignmentCenter); SolidBrush b(WithAlpha(T.t1, (int)(T.t1.GetA() * alpha)));
                g.DrawString(s, -1, &fLock, RectF(tx, ty + dy - S(1), tw, S(20)), &sf, &b);
            };
            word(was, 1 - kt, -S(5) * kt); word(now, kt, S(5) * (1 - kt));
            float bw0 = on ? S(36) : S(60), bw1 = on ? S(60) : S(36), bwv = bw0 + (bw1 - bw0) * kb;
            Color off = WithAlpha(T.t3, T.t3.GetA() / 2), onc = T.accent;
            Color from = on ? off : onc, to = on ? onc : off;
            auto lerp = [&](int a2, int b2) { return (BYTE)(a2 + (b2 - a2) * kb); };
            Color bc(lerp(from.GetA(), to.GetA()), lerp(from.GetR(), to.GetR()), lerp(from.GetG(), to.GetG()), lerp(from.GetB(), to.GetB()));
            FillRound(g, bc, RectF(tx + (tw - bwv) / 2, ty + S(21), bwv, S(3)), S(1.5f));
        }
    }
    BitBlt(hdc, 0, 0, w, h, mdc, 0, 0, SRCCOPY);
    SelectObject(mdc, old); DeleteDC(mdc);
}
static bool FullscreenBusy() {   // a game, a presentation or a full-screen app is in front: no flyouts
    QUERY_USER_NOTIFICATION_STATE q;
    return SUCCEEDED(SHQueryUserNotificationState(&q)) && (q == QUNS_BUSY || q == QUNS_RUNNING_D3D_FULL_SCREEN || q == QUNS_PRESENTATION_MODE);
}
static void PopPlace(bool atRest, float k) {   // k 0..1 of the way in
    int dx = 0, dy = (int)lroundf(g_popSlide * (1 - k));
    SetWindowPos(g_pop, HWND_TOPMOST, g_popAt.x + dx, g_popAt.y + dy, g_popW, g_popH, SWP_NOACTIVATE | (atRest ? 0 : SWP_NOSIZE));
    if (!g_popGlass) SetLayeredWindowAttributes(g_pop, 0, (BYTE)lroundf(255 * std::max(0.f, std::min(1.f, k))), LWA_ALPHA);
}
static void PopTimer() {
    int ms = g_popDir != 0 || (g_popKind == POP_LOCK && GetTickCount() - g_popLockT0 < 700) || g_popDrag >= 0 ? 15 : PopAnimatedGlow() ? 33 : 250;
    SetTimer(g_pop, 1, ms, nullptr);
}
static void PopEnd() {   // gone: let go of what only the flyout needed
    KillTimer(g_pop, 1);
    ShowWindow(g_pop, SW_HIDE);
    g_popShown = false; g_popDir = 0; g_popList = false; g_popHot = -1; g_popDrag = -1; g_popTry = false;
    if (g_popLv) { g_popLv = false; LevelsStop(); }
    CanvasFree(g_cvPop); PopCacheFree();
}
static void PopHide() {
    if (!g_popShown || g_popDir == -1) return;
    if (PopDur() == 0) { PopEnd(); return; }
    g_popDir = -1; g_popT0 = GetTickCount(); PopTimer();
}
static void PopArm() {   // how long it stays: the setting, longer while the mouse is on it; media can stay until closed
    int ms = g_popKind == POP_LOCK ? std::max(100, std::min(60000, g_lkStayMs)) : std::max(0, std::min(60000, g_fStayMs));
    if (!ms && g_popKind != POP_MEDIA) ms = 2000;
    g_popHideAt = ms ? GetTickCount() + ms : 0;
}
static LRESULT CALLBACK PopProc(HWND h, UINT msg, WPARAM wp, LPARAM lp);
static void PopApplyStyle() {
    bool wantGlass = g_style == 1 && TransparencyOn() && T.glass;
    LONG ex = GetWindowLongW(g_pop, GWL_EXSTYLE);
    LONG want = wantGlass ? (ex & ~WS_EX_LAYERED) : (ex | WS_EX_LAYERED);
    if (want != ex) SetWindowLongW(g_pop, GWL_EXSTYLE, want);
    int none = 1; DwmSetWindowAttribute(g_pop, 38, &none, sizeof(none));
    g_popGlass = wantGlass && SetAccent(g_pop, 4, T.dark ? 0xB8141414 : 0xA8F3F3F3);
    if (!g_popGlass) SetAccent(g_pop, 0, 0);
    MARGINS mg = g_popGlass ? MARGINS{-1, -1, -1, -1} : MARGINS{0, 0, 0, 0}; DwmExtendFrameIntoClientArea(g_pop, &mg);
    BOOL dark = T.dark; DwmSetWindowAttribute(g_pop, 20, &dark, sizeof(dark));
    DWORD pref = 2; DwmSetWindowAttribute(g_pop, 33, &pref, sizeof(pref));
    COLORREF bc = 0xFFFFFFFE; DwmSetWindowAttribute(g_pop, 34, &bc, sizeof(bc));
}
static void PopShow(int kind) {
    g_crumb = "PopShow";
    if ((kind == POP_MEDIA || kind == POP_NEXT) && !MediaCur() && !g_popTry) return;
    if (FullscreenBusy()) return;
    if (!g_pop) {
        WNDCLASSW wc = {}; wc.lpfnWndProc = PopProc; wc.hInstance = g_inst; wc.lpszClassName = L"KnobPop"; wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        RegisterClassW(&wc);
        g_pop = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED, L"KnobPop", L"", WS_POPUP, 0, 0, 10, 10, nullptr, nullptr, g_inst, nullptr);
        if (!g_pop) return;
    }
    bool wasShown = g_popShown && g_popDir != -1, sameKind = wasShown && g_popKind == kind;
    if (wasShown && !sameKind && g_popKind == POP_MEDIA && kind == POP_NEXT) return;   // the media flyout is up: it shows the new song itself
    PopApplyStyle();
    HMONITOR hm;
    if (g_fScreen == 1) { POINT pt; GetCursorPos(&pt); hm = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST); }
    else if (g_fScreen == 2) hm = MonitorFromWindow(GetForegroundWindow(), MONITOR_DEFAULTTOPRIMARY);
    else hm = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    UINT dx = 96, dy = 96; GetDpiForMonitor(hm, MDT_EFFECTIVE_DPI, &dx, &dy);
    if (!g_visible && !g_set) ApplyDpi(kind == POP_MEDIA ? (int)(dx * 118 / 100) : (int)dx);   // the panel and Settings keep their own size while open; the media flyout is a bit larger
    MONITORINFO mi = {sizeof(mi)}; GetMonitorInfoW(hm, &mi);
    RECT wa = mi.rcWork;
    int w = (int)S(380), hh = 0;
    if (kind == POP_MEDIA) hh = (int)S(g_fLayout ? 68 : 158);
    else if (kind == POP_NEXT) hh = (int)S(52);
    else {
        int k = std::max(0, std::min(3, g_popLockKey));
        Font fLock(g_famSemi, S(13.5f), FontStyleBold, UnitPixel);
        float tw = std::max(S(60), std::max(MeasureW(LOCK_TXT[k][1], &fLock), MeasureW(LOCK_TXT[k][2], &fLock))) + S(4);
        w = (int)(S(14) + S(20) + S(11) + tw + S(18)); hh = (int)S(48);
    }
    g_popW = w; g_popH = hh; g_popKind = kind;
    int mH = (int)S(24), mB = (int)S(30);
    int x = g_fPos == 1 || g_fPos == 4 ? wa.left + mH : g_fPos == 2 || g_fPos == 5 ? wa.right - mH - w : wa.left + (wa.right - wa.left - w) / 2;
    bool top = g_fPos >= 3;
    int y = top ? wa.top + mH : wa.bottom - mB - hh;
    g_popAt = POINT{x, y}; g_popSlide = (int)S(20) * (top ? -1 : 1);
    if (!sameKind) { g_popList = false; g_popHot = -1; }
    if (kind == POP_MEDIA && g_fGlow == 5 && !g_popLv) { g_popLv = true; LevelsStart(); }
    if ((kind != POP_MEDIA || g_fGlow != 5) && g_popLv) { g_popLv = false; LevelsStop(); }
    PopArm();
    if (wasShown) { if (g_popDir == 0) PopPlace(true, 1); }
    else {
        g_popShown = true;
        if (PopDur() > 0) { g_popDir = 1; g_popT0 = GetTickCount(); PopPlace(true, 0); }
        else { g_popDir = 0; PopPlace(true, 1); }
        ShowWindow(g_pop, SW_SHOWNOACTIVATE);
    }
    PopTimer();
    InvalidateRect(g_pop, nullptr, FALSE);
}
static void PopVolumeChanged() {   // the volume moved (a key, a Knob shortcut, Windows' slider): show the media flyout with the level
    if (!g_popMedia || g_visible || g_set || !MediaCur()) return;
    g_popVolT0 = GetTickCount();
    PopShow(POP_MEDIA);
}
static void PopLockKey(int k, bool on) {   // a lock key changed: show the old state turning into the new one
    if (!g_popLock || !(g_lkKeys & (1 << k))) return;
    g_popLockKey = k; g_popLockOn = on; g_popLockT0 = GetTickCount();
    PopShow(POP_LOCK);
}
static void PopClick(int id) {
    MSess* m = MediaCur();
    if (id == 8) { PopHide(); return; }
    if (id == 6) { g_popList = !g_popList; InvalidateRect(g_pop, nullptr, FALSE); return; }
    if (id >= 100) {   // picked a player: it stays the one shown until you pick another or it closes
        int i = id - 100;
        if (i < (int)g_ms.s.size()) { EnterCriticalSection(&g_mcs); g_mediaPick = g_ms.s[i].aumid; LeaveCriticalSection(&g_mcs); g_ms.cur = i; MediaRefresh(); }
        g_popList = false; InvalidateRect(g_pop, nullptr, FALSE); return;
    }
    g_popList = false;
    if (!m) return;
    if (id == 3) { MediaCmd(MCMD_PLAYPAUSE); m->status = m->status == 4 ? 5 : 4; }   // shown at once; the player confirms a moment later
    if (id == 2) MediaCmd(MCMD_PREV);
    if (id == 4) MediaCmd(MCMD_NEXT);
    if (id == 1) { m->shuffle = !m->shuffle; MediaCmd(MCMD_SHUFFLE, m->shuffle); }
    if (id == 5) { m->repeat = (m->repeat + 1) % 3; MediaCmd(MCMD_REPEAT, m->repeat == 1 ? 2 : m->repeat == 2 ? 1 : 0); }
    InvalidateRect(g_pop, nullptr, FALSE);
}
static LRESULT CALLBACK PopProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_PAINT: { PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps); PaintPop(dc); EndPaint(h, &ps); return 0; }
    case WM_ERASEBKGND: return 1;
    case WM_TIMER: {
        DWORD now = GetTickCount();
        if (g_popDir != 0) {
            float t = std::min(1.f, (now - g_popT0) / (float)std::max(1, PopDur()));
            float k = g_popDir == 1 ? 1 - powf(1 - t, 3) : 1 - t * t * t;   // arriving eases out, leaving eases in
            PopPlace(false, k);
            if (t >= 1) { if (g_popDir == -1) { PopEnd(); return 0; } g_popDir = 0; }
        }
        if (g_popDir == 0 && !g_popHover && g_popDrag < 0 && !g_popList && g_popHideAt && (int)(now - g_popHideAt) >= 0) { PopHide(); return 0; }
        PopTimer();
        if (g_popDir == 0 || PopAnimatedGlow() || g_popDrag >= 0 || (g_popKind == POP_LOCK && now - g_popLockT0 < 700)) InvalidateRect(h, nullptr, FALSE);   // sliding in or out only moves the window: its picture is already there
        return 0;
    }
    case WM_MOUSEMOVE: {
        float x = (float)GET_X_LPARAM(lp), y = (float)GET_Y_LPARAM(lp);
        if (!g_popHover) { g_popHover = true; TRACKMOUSEEVENT tme = {sizeof(tme), TME_LEAVE, h, 0}; TrackMouseEvent(&tme); }
        if (g_popDrag == 7) {
            for (auto& ht : g_popHits) if (ht.first == 7) { RectF r = ht.second; g_popSeek = std::max(0.f, std::min(1.f, (x - r.X - S(4)) / (r.Width - S(8)))); }
            InvalidateRect(h, nullptr, FALSE); return 0;
        }
        int hot = -1; for (auto& ht : g_popHits) if (InRect(ht.second, x, y)) hot = ht.first;
        if (hot != g_popHot) { g_popHot = hot; InvalidateRect(h, nullptr, FALSE); }
        return 0;
    }
    case WM_MOUSELEAVE:
        g_popHover = false; g_popHot = -1;
        if (g_popHideAt) g_popHideAt = std::max(g_popHideAt, GetTickCount() + 1200);   // a moment more after the mouse leaves
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_LBUTTONDOWN: {
        float x = (float)GET_X_LPARAM(lp), y = (float)GET_Y_LPARAM(lp);
        int hit = -1; for (auto& ht : g_popHits) if (InRect(ht.second, x, y)) hit = ht.first;
        if (hit == 7) {   // drag the progress bar to move in the song
            for (auto& ht : g_popHits) if (ht.first == 7) { RectF r = ht.second; g_popSeek = std::max(0.f, std::min(1.f, (x - r.X - S(4)) / (r.Width - S(8)))); }
            g_popDrag = 7; SetCapture(h); PopTimer(); InvalidateRect(h, nullptr, FALSE); return 0;
        }
        if (hit < 0) { if (g_popList) { g_popList = false; InvalidateRect(h, nullptr, FALSE); } return 0; }
        PopClick(hit);
        return 0;
    }
    case WM_LBUTTONUP:
        if (g_popDrag == 7) {
            g_popDrag = -1; ReleaseCapture();
            if (MSess* m = MediaCur()) {
                INT64 p = (INT64)(g_popSeek * m->end);
                MediaCmd(MCMD_SEEK, (LPARAM)(p + m->start));
                FILETIME ft; GetSystemTimeAsFileTime(&ft); m->pos = p; m->updated = ((INT64)ft.dwHighDateTime << 32) | ft.dwLowDateTime;   // stays where you let go
            }
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    case WM_CAPTURECHANGED: if (g_popDrag >= 0 && (HWND)lp != h) g_popDrag = -1; return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}
static void MediaApply(MediaSnap* m) {   // a new snapshot from the media thread
    g_crumb = "MediaApply";
    if (m->newCover) { delete g_cover; g_coverGen++; g_cover = m->cover; m->cover = nullptr; CoverColours(); BuildGlows(); }
    int why = m->why;
    std::wstring prevId; int prevSt = -1;
    if (MSess* pc = MediaCur()) { prevId = pc->aumid; prevSt = pc->status; }
    g_ms.s = std::move(m->s); g_ms.cur = m->cur;
    delete m->cover; delete m;
    if (MSess* nc = MediaCur()) {   // play or pause, from a key, a website, the player itself: the media flyout shows it
        bool flip = nc->aumid == prevId && ((prevSt == 4 && nc->status == 5) || (prevSt == 5 && nc->status == 4));
        if (flip && g_popMedia && !g_visible && !g_set && GetTickCount() > 5000) { g_popVolT0 = 0; PopShow(POP_MEDIA); }
    }
    if (why == 2 && g_popNext) PopShow(POP_NEXT);
    if (g_popShown && g_pop) { if (!MediaCur() && g_popKind != POP_LOCK && !g_popTry) PopHide(); else InvalidateRect(g_pop, nullptr, FALSE); }
}
static void FlyoutKeysSync();   // listen to the keyboard only while a flyout that needs it is on

// ---------------------------------------------------------------- Party screen
// A screen of soft light that moves with the music, in the cover's colours. The light is drawn small (320 px wide)
// and blurred on the CPU, then Direct2D stretches it to the screen on the graphics card, so it stays light.
// Only exists while it's on screen: any key or mouse move fades it out and frees everything.
static const wchar_t* PARTY_LOOKS[3] = {L"Glow waves", L"Aurora", L"Light line"};
static HWND g_party = nullptr;
static ID2D1Factory* g_d2f = nullptr;
static ID2D1HwndRenderTarget* g_prt = nullptr;
static ID2D1Bitmap* g_pbmp = nullptr, *g_pinfo = nullptr;
static ID2D1SolidColorBrush* g_pline = nullptr;
// the full-resolution path: Direct3D + Direct2D 1.1, real Gaussian blur on the graphics card (the old path below is only a fallback)
static ID3D11Device* g_pd3d = nullptr; static ID2D1Factory1* g_pf1 = nullptr; static ID2D1Device* g_pdev = nullptr; static ID2D1DeviceContext* g_pdc = nullptr;
static IDXGISwapChain1* g_psc = nullptr; static ID2D1Bitmap1* g_ptarget = nullptr, *g_pmid = nullptr; static ID2D1Effect* g_pblur = nullptr;
static int g_pw = 0, g_ph = 0, g_pDir = 0, g_pA = 0;   // low-res size; fade direction and alpha
static DWORD g_pT0 = 0, g_pTick = 0; static POINT g_pPt;
static float g_pTime = 0; static bool g_pLive = false;   // fully started (levels on, cursor hidden)
static std::vector<float> g_pbuf;      // r g b per low-res pixel
static std::wstring g_pInfoKey;
static HMONITOR PickMonitor(const std::wstring& name) {   // "" = main screen, else the screen with that name
    if (!name.empty()) for (auto& m : g_mons) if (m.name == name && m.cloneOf < 0) return MonitorFromRect(&m.rc, MONITOR_DEFAULTTONEAREST);
    return MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
}
static void PartyGpuFree() {
    if (g_pdc) g_pdc->SetTarget(nullptr);
    if (g_pblur) { g_pblur->Release(); g_pblur = nullptr; }
    if (g_pmid) { g_pmid->Release(); g_pmid = nullptr; }
    if (g_ptarget) { g_ptarget->Release(); g_ptarget = nullptr; }
    if (g_pdc) { g_pdc->Release(); g_pdc = nullptr; }
    if (g_pdev) { g_pdev->Release(); g_pdev = nullptr; }
    if (g_pf1) { g_pf1->Release(); g_pf1 = nullptr; }
    if (g_psc) { g_psc->Release(); g_psc = nullptr; }
    if (g_pd3d) { g_pd3d->Release(); g_pd3d = nullptr; }
}
static void PartyFree() {
    if (g_pinfo) { g_pinfo->Release(); g_pinfo = nullptr; }
    if (g_pline) { g_pline->Release(); g_pline = nullptr; }
    if (g_pbmp) { g_pbmp->Release(); g_pbmp = nullptr; }
    if (g_prt) { g_prt->Release(); g_prt = nullptr; }
    PartyGpuFree();
    std::vector<float>().swap(g_pbuf); g_pInfoKey.clear();
}
static std::vector<std::wstring> ScreenNames() { std::vector<std::wstring> v; for (auto& m : g_mons) if (m.cloneOf < 0 && std::find(v.begin(), v.end(), m.name) == v.end()) v.push_back(m.name); return v; }
static void PartyEnd() {
    if (!g_party || g_pDir == -1) return;
    g_pDir = -1; g_pT0 = GetTickCount();
}
static void PartyInfoBitmap() {   // the cover, title and artist for the corner, drawn once per song with GDI+
    MSess* m = MediaCur();
    std::wstring key = m && g_partyInfo ? m->title + L"|" + m->artist + L"|" + m->app + (g_cover ? L"c" : L"") : L"";
    if (key == g_pInfoKey && (g_pinfo || key.empty())) return;
    g_pInfoKey = key;
    if (g_pinfo) { g_pinfo->Release(); g_pinfo = nullptr; }
    if (key.empty() || (!g_prt && !g_pdc)) return;
    const int w = (int)S(420), h = (int)S(48);
    Gdiplus::Bitmap bmp(w, h, PixelFormat32bppPARGB);
    {
        Graphics g(&bmp); g.Clear(Color(0, 0, 0, 0)); g.SetSmoothingMode(SmoothingModeAntiAlias); g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
        RectF cr(0, 0, S(48), S(48)); GraphicsPath p; AddRound(p, cr, S(8));
        if (g_cover) { g.SetClip(&p); g.SetInterpolationMode(InterpolationModeHighQualityBicubic); g.DrawImage(g_cover, cr); g.ResetClip(); }
        else { SolidBrush b(PopAcc()); g.FillPath(&b, &p); }
        Font f1(g_famSemi, S(14), FontStyleRegular, UnitPixel), f2(g_famText, S(12), FontStyleRegular, UnitPixel);
        StringFormat sf; sf.SetTrimming(StringTrimmingEllipsisCharacter); sf.SetFormatFlags(StringFormatFlagsNoWrap);
        SolidBrush b1(Color(191, 255, 255, 255)), b2(Color(134, 255, 255, 255));
        g.DrawString(m->title.c_str(), -1, &f1, RectF(S(60), S(5), w - S(60), S(20)), &sf, &b1);
        g.DrawString((m->artist + L" · " + m->app).c_str(), -1, &f2, RectF(S(60), S(25), w - S(60), S(18)), &sf, &b2);
    }
    BitmapData bd; Rect rc(0, 0, w, h);
    if (bmp.LockBits(&rc, ImageLockModeRead, PixelFormat32bppPARGB, &bd) == Ok) {
        if (g_pdc) {
            D2D1_BITMAP_PROPERTIES1 bp1 = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.f, 96.f);
            ID2D1Bitmap1* b1 = nullptr;
            if (SUCCEEDED(g_pdc->CreateBitmap(D2D1::SizeU(w, h), bd.Scan0, bd.Stride, &bp1, &b1))) g_pinfo = b1;
        } else {
            D2D1_BITMAP_PROPERTIES bp = {{DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED}, 96, 96};
            g_prt->CreateBitmap(D2D1::SizeU(w, h), bd.Scan0, bd.Stride, bp, &g_pinfo);
        }
        bmp.UnlockBits(&bd);
    }
}
static bool PartyGpuInit(HWND hw, int w, int h) {
    typedef HRESULT (WINAPI* PFN_D3DCD)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT, UINT, ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
    typedef HRESULT (WINAPI* PFN_D2DCF)(D2D1_FACTORY_TYPE, REFIID, const D2D1_FACTORY_OPTIONS*, void**);
    HMODULE d3 = LoadLibraryW(L"d3d11.dll"), d2 = LoadLibraryW(L"d2d1.dll");
    if (!d3 || !d2) return false;
    PFN_D3DCD cd = (PFN_D3DCD)GetProcAddress(d3, "D3D11CreateDevice"); PFN_D2DCF cf = (PFN_D2DCF)GetProcAddress(d2, "D2D1CreateFactory");
    if (!cd || !cf) return false;
    IDXGIDevice* dxd = nullptr; IDXGIAdapter* adp = nullptr; IDXGIFactory2* fac = nullptr; IDXGISurface* surf = nullptr; bool ok = false;
    do {
        if (FAILED(cd(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &g_pd3d, nullptr, nullptr))) break;
        if (FAILED(g_pd3d->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxd))) break;
        if (FAILED(cf(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), nullptr, (void**)&g_pf1))) break;
        if (FAILED(g_pf1->CreateDevice(dxd, &g_pdev))) break;
        if (FAILED(g_pdev->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &g_pdc))) break;
        if (FAILED(dxd->GetAdapter(&adp))) break;
        if (FAILED(adp->GetParent(__uuidof(IDXGIFactory2), (void**)&fac))) break;
        DXGI_SWAP_CHAIN_DESC1 sd = {}; sd.Width = w; sd.Height = h; sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM; sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.BufferCount = 2; sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;   // the plain copy model: works with the fading window
        if (FAILED(fac->CreateSwapChainForHwnd(g_pd3d, hw, &sd, nullptr, nullptr, &g_psc))) break;
        fac->MakeWindowAssociation(hw, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
        if (FAILED(g_psc->GetBuffer(0, __uuidof(IDXGISurface), (void**)&surf))) break;
        D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), 96.f, 96.f);
        if (FAILED(g_pdc->CreateBitmapFromDxgiSurface(surf, &bp, &g_ptarget))) break;
        D2D1_BITMAP_PROPERTIES1 mp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.f, 96.f);
        if (FAILED(g_pdc->CreateBitmap(D2D1::SizeU(w, h), nullptr, 0, &mp, &g_pmid))) break;
        if (FAILED(g_pdc->CreateEffect(CLSID_D2D1GaussianBlur, &g_pblur))) break;
        g_pdc->SetDpi(96.f, 96.f);
        ok = true;
    } while (0);
    if (surf) surf->Release(); if (fac) fac->Release(); if (adp) adp->Release(); if (dxd) dxd->Release();
    if (!ok) PartyGpuFree();
    return ok;
}
static void PartyFrameGpu() {   // the same looks at the screen's own resolution: shapes drawn with gradients, softened by a real Gaussian blur
    ID2D1DeviceContext* dc = g_pdc; if (!dc || !g_ptarget || !g_pmid) return;
    D2D1_SIZE_U psz = g_ptarget->GetPixelSize(); const float W = (float)psz.width, H = (float)psz.height;
    g_pTime += 0.033f; const float t = g_pTime;
    Color a = PopAcc(); const float ar = a.GetR() / 255.f, ag = a.GetG() / 255.f, ab = a.GetB() / 255.f;
    PartyInfoBitmap();
    float sigma = 0; ID2D1PathGeometry* line = nullptr;
    dc->SetTarget(g_pmid); dc->BeginDraw(); dc->Clear(D2D1::ColorF(0, 0, 0, 0));
    auto linGrad = [&](ID2D1GradientStopCollection* gs, float x0, float y0, float x1, float y1) { ID2D1LinearGradientBrush* br = nullptr; dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(x0, y0), D2D1::Point2F(x1, y1)), gs, &br); return br; };
    if (g_partyLook == 0) {   // glow waves: bars rising from the bottom, brighter at their base
        D2D1_GRADIENT_STOP st[2] = {{0.f, D2D1::ColorF(ar, ag, ab, 0.75f)}, {1.f, D2D1::ColorF(ar, ag, ab, 0.26f)}};
        ID2D1GradientStopCollection* gs = nullptr; dc->CreateGradientStopCollection(st, 2, &gs);
        if (gs) {
            const int n = 32; float x0 = -W * 0.04f, span = W * 1.08f, gap = W * 0.0047f, bw = span / n - gap;
            for (int k = 0; k < n; k++) {
                float hgt = LvBar(k, n) * H, bx = x0 + k * (bw + gap); if (hgt < 2.f) continue;
                if (ID2D1LinearGradientBrush* br = linGrad(gs, bx, H, bx, H - hgt)) { dc->FillRectangle(D2D1::RectF(bx, H - hgt, bx + bw, H), br); br->Release(); }
            }
            gs->Release();
        }
        sigma = W * 0.014f;
    } else if (g_partyLook == 1) {   // aurora: three curtains of light rays hanging from slow wavy lines; the music lifts the rays
        float bass = (g_lv[0] + g_lv[1] + g_lv[2]) / 3;
        {   // the night sky, a touch of colour low down
            D2D1_GRADIENT_STOP st[2] = {{0.f, D2D1::ColorF(ar * 0.1f + 0.008f, ag * 0.1f + 0.024f, ab * 0.1f + 0.067f, 0.9f)}, {1.f, D2D1::ColorF(ar * 0.1f + 0.008f, ag * 0.1f + 0.024f, ab * 0.1f + 0.067f, 0.f)}};
            ID2D1GradientStopCollection* gs = nullptr; dc->CreateGradientStopCollection(st, 2, &gs);
            if (gs) { ID2D1RadialGradientBrush* rb = nullptr; dc->CreateRadialGradientBrush(D2D1::RadialGradientBrushProperties(D2D1::Point2F(W / 2, H), D2D1::Point2F(0, 0), 0.6f * W, 0.7f * H), gs, &rb);
                      if (rb) { dc->FillRectangle(D2D1::RectF(0, 0, W, H), rb); rb->Release(); } gs->Release(); }
        }
        static const float base[3][3] = {{0.24f, 1.0f, 0.63f}, {0.18f, 0.85f, 1.0f}, {0.70f, 0.42f, 1.0f}};
        static const float mixA[3] = {0.35f, 0.30f, 0.30f}, op[3] = {1.0f, 0.75f, 0.55f};
        dc->SetPrimitiveBlend(D2D1_PRIMITIVE_BLEND_ADD);   // overlapping rays add their light
        for (int l = 0; l < 3; l++) {
            float cr = ar * mixA[l] + base[l][0] * (1 - mixA[l]), cg = ag * mixA[l] + base[l][1] * (1 - mixA[l]), cb = ab * mixA[l] + base[l][2] * (1 - mixA[l]), K = op[l] * 0.55f;
            D2D1_GRADIENT_STOP st[4] = {{0.f, D2D1::ColorF(cr, cg, cb, 0.f)}, {0.06f, D2D1::ColorF(cr, cg, cb, K)}, {0.35f, D2D1::ColorF(cr, cg, cb, K * 0.55f)}, {1.f, D2D1::ColorF(cr, cg, cb, 0.f)}};
            ID2D1GradientStopCollection* gs = nullptr; dc->CreateGradientStopCollection(st, 4, &gs); if (!gs) continue;
            const int rays = 64;
            for (int k = 0; k < rays; k++) {
                float x = k / (float)(rays - 1), bi = LvBar((int)(x * 31), 32);
                float bottom = (34 + l * 9 + sinf(x * 3.1f + t * .32f + l * 1.9f) * 10 + sinf(x * 7.3f - t * .47f + l) * 4) / 100.f * H;
                float hgt = (14 + l * 4 + bi * (34 - l * 6) + sinf(k * .9f + t * 1.3f + l) * 3 + bass * 6) / 100.f * H;
                float cx = x * W, half = W * 0.012f, yb = H - bottom;
                if (ID2D1LinearGradientBrush* br = linGrad(gs, cx, yb, cx, yb - hgt)) { dc->FillRectangle(D2D1::RectF(cx - half, yb - hgt, cx + half, yb), br); br->Release(); }
            }
            gs->Release();
        }
        dc->SetPrimitiveBlend(D2D1_PRIMITIVE_BLEND_SOURCE_OVER);
        sigma = W * 0.007f;
    } else {   // light line: one smooth line across the middle; each part of the song swings it up or down
        const int n = 40;
        std::vector<D2D1_POINT_2F> pts; pts.push_back(D2D1::Point2F(0, H / 2.f));
        for (int k = 0; k < n; k++) pts.push_back(D2D1::Point2F((k + .5f) / n * W, H / 2.f - LvBar(k, n) * H * 0.44f * sinf(k * 1.1f + t * 2.4f)));
        pts.push_back(D2D1::Point2F(W, H / 2.f));
        if (g_pf1 && SUCCEEDED(g_pf1->CreatePathGeometry(&line)) && line) {
            ID2D1GeometrySink* sink = nullptr;
            if (SUCCEEDED(line->Open(&sink)) && sink) {   // smooth curves through the midpoints of the points
                sink->BeginFigure(pts[0], D2D1_FIGURE_BEGIN_HOLLOW);
                for (size_t i = 1; i + 1 < pts.size(); i++) {
                    D2D1_POINT_2F e = D2D1::Point2F((pts[i].x + pts[i + 1].x) / 2, (pts[i].y + pts[i + 1].y) / 2);
                    sink->AddQuadraticBezier(D2D1::QuadraticBezierSegment(pts[i], e));
                }
                sink->AddLine(pts.back());
                sink->EndFigure(D2D1_FIGURE_END_OPEN); sink->Close(); sink->Release();
            }
            ID2D1SolidColorBrush* br = nullptr; dc->CreateSolidColorBrush(D2D1::ColorF(ar, ag, ab, 0.5f), &br);
            ID2D1StrokeStyle* ss = nullptr; g_pf1->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND, 10.f, D2D1_DASH_STYLE_SOLID, 0.f), nullptr, 0, &ss);
            if (br) { dc->DrawGeometry(line, br, W * 0.012f, ss); br->Release(); }
            if (ss) ss->Release();
        }
        sigma = W * 0.009f;
    }
    HRESULT hr1 = dc->EndDraw();
    dc->SetTarget(g_ptarget); dc->BeginDraw(); dc->Clear(D2D1::ColorF(0, 0, 0, 1));
    if (SUCCEEDED(hr1)) {
        g_pblur->SetInput(0, g_pmid);
        g_pblur->SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION, sigma);
        g_pblur->SetValue(D2D1_GAUSSIANBLUR_PROP_BORDER_MODE, D2D1_BORDER_MODE_HARD);
        dc->DrawImage(g_pblur);
    }
    if (line) {   // the line itself, sharp, over its glow
        ID2D1SolidColorBrush* br = nullptr; dc->CreateSolidColorBrush(D2D1::ColorF(ar, ag, ab, 1.f), &br);
        if (br) { dc->DrawGeometry(line, br, S(3)); br->Release(); }
        line->Release();
    }
    if (g_pinfo) { D2D1_SIZE_F is = g_pinfo->GetSize(); dc->DrawBitmap(g_pinfo, D2D1::RectF(S(36), H - S(32) - is.height, S(36) + is.width, H - S(32)), 1.f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR); }
    HRESULT hr2 = dc->EndDraw();
    if (hr2 == (HRESULT)D2DERR_RECREATE_TARGET || hr1 == (HRESULT)D2DERR_RECREATE_TARGET) { PartyFree(); return; }
    HRESULT hp = g_psc->Present(1, 0);
    if (hp == DXGI_ERROR_DEVICE_REMOVED || hp == DXGI_ERROR_DEVICE_RESET) PartyFree();
}
static void PartyFrame() {
    if (g_pdc) { PartyFrameGpu(); return; }
    if (!g_prt && g_d2f && g_party) {   // the graphics card was reset: make a new target
        RECT rc; GetClientRect(g_party, &rc);
        g_d2f->CreateHwndRenderTarget(D2D1::RenderTargetProperties(), D2D1::HwndRenderTargetProperties(g_party, D2D1::SizeU(rc.right, rc.bottom)), &g_prt);
    }
    if (!g_prt) return;
    if (!g_pbmp) {
        D2D1_BITMAP_PROPERTIES bp = {{DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE}, 96, 96};
        if (FAILED(g_prt->CreateBitmap(D2D1::SizeU(g_pw, g_ph), bp, &g_pbmp))) return;
    }
    g_pTime += 0.033f;
    const int W = g_pw, H = g_ph; float t = g_pTime;
    std::vector<float>& b = g_pbuf; b.assign((size_t)W * H * 3, 0.f);
    Color a = PopAcc(); float ar = a.GetR() / 255.f, ag = a.GetG() / 255.f, ab = a.GetB() / 255.f;
    auto add = [&](int x, int y, float r, float g, float bl, float k) { if (x < 0 || y < 0 || x >= W || y >= H) return; float* p = &b[((size_t)y * W + x) * 3]; p[0] += r * k; p[1] += g * k; p[2] += bl * k; };
    auto blur = [&](int r, int passes) {   // separable box blur on the float buffer
        if (r < 1) return;
        std::vector<float> tmp(b.size());
        for (int pass = 0; pass < passes; pass++) {
            for (int dir = 0; dir < 2; dir++) {
                const std::vector<float>& src = dir ? tmp : b; std::vector<float>& dst = dir ? b : tmp;
                int n = dir ? H : W, lines = dir ? W : H;
                for (int ln = 0; ln < lines; ln++) {
                    float acc[3] = {};
                    auto at = [&](int i) -> const float* { i = std::max(0, std::min(n - 1, i)); return &src[(dir ? ((size_t)i * W + ln) : ((size_t)ln * W + i)) * 3]; };
                    for (int i = -r; i <= r; i++) { const float* p = at(i); acc[0] += p[0]; acc[1] += p[1]; acc[2] += p[2]; }
                    for (int i = 0; i < n; i++) {
                        float* d = &dst[(dir ? ((size_t)i * W + ln) : ((size_t)ln * W + i)) * 3];
                        d[0] = acc[0] / (2 * r + 1); d[1] = acc[1] / (2 * r + 1); d[2] = acc[2] / (2 * r + 1);
                        const float* pa = at(i + r + 1), *pr = at(i - r);
                        for (int k = 0; k < 3; k++) acc[k] += pa[k] - pr[k];
                    }
                }
            }
        }
    };
    float sc = W / 320.f;
    std::vector<D2D1_POINT_2F> line;
    if (g_partyLook == 0) {   // glow waves rise from the bottom
        int n = 32;
        float x0 = -W * 0.04f, span = W * 1.08f, gap = 1.5f * sc, bw = span / n - gap;
        for (int k = 0; k < n; k++) {
            float hgt = LvBar(k, n) * H, bx = x0 + k * (bw + gap);
            for (int y = 0; y < H; y++) {
                float fromB = (H - 1 - y) / std::max(1.f, hgt); if (fromB > 1) continue;
                float m = 1 - 0.65f * fromB;
                for (int x = (int)bx; x < (int)(bx + bw); x++) add(x, y, ar, ag, ab, m);
            }
        }
        blur((int)lroundf(4 * sc), 3);
        for (auto& v : b) v *= 0.75f;
    } else if (g_partyLook == 1) {   // aurora: three curtains of soft light rays hanging from slow wavy lines; the music lifts the rays
        float bass = (g_lv[0] + g_lv[1] + g_lv[2]) / 3;
        for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {   // the night sky, a touch of colour low down
            float dx = (x - W / 2.f) / (W * 0.6f), dy = (y - H) / (H * 0.7f), d = std::min(1.f, sqrtf(dx * dx + dy * dy));
            add(x, y, ar * 0.1f + 0.008f, ag * 0.1f + 0.024f, ab * 0.1f + 0.067f, 0.9f * (1 - d));
        }
        static const float base[3][3] = {{0.24f, 1.0f, 0.63f}, {0.18f, 0.85f, 1.0f}, {0.70f, 0.42f, 1.0f}};
        static const float mixA[3] = {0.35f, 0.30f, 0.30f}, op[3] = {1.0f, 0.75f, 0.55f};
        for (int l = 0; l < 3; l++) {
            float cr = ar * mixA[l] + base[l][0] * (1 - mixA[l]), cg = ag * mixA[l] + base[l][1] * (1 - mixA[l]), cb = ab * mixA[l] + base[l][2] * (1 - mixA[l]);
            const int rays = 64;
            for (int k = 0; k < rays; k++) {
                float x = k / (float)(rays - 1), bi = LvBar((int)(x * 31), 32);
                float bottom = (34 + l * 9 + sinf(x * 3.1f + t * .32f + l * 1.9f) * 10 + sinf(x * 7.3f - t * .47f + l) * 4) / 100.f * H;
                float hgt = (14 + l * 4 + bi * (34 - l * 6) + sinf(k * .9f + t * 1.3f + l) * 3 + bass * 6) / 100.f * H;
                float cx = x * W, half = W * 0.012f;
                for (int yy = 0; yy < (int)hgt; yy++) {
                    float u = yy / std::max(1.f, hgt);   // 0 at the bottom of the ray
                    float m = u < .06f ? u / .06f : u < .35f ? 1 - (u - .06f) / .29f * .45f : .55f * (1 - (u - .35f) / .65f);
                    int y = (int)(H - bottom - yy);
                    for (int xx = (int)(cx - half); xx <= (int)(cx + half); xx++) add(xx, y, cr, cg, cb, m * op[l] * 0.55f);
                }
            }
        }
        blur((int)lroundf(2 * sc), 2);
    } else {   // light line: one smooth line across the middle; each part of the song swings it up or down
        const int n = 40;
        std::vector<PointF> pts; pts.push_back(PointF(0, H / 2.f));
        for (int k = 0; k < n; k++) pts.push_back(PointF((k + .5f) / n * W, H / 2.f - LvBar(k, n) * H * 0.44f * sinf(k * 1.1f + t * 2.4f)));
        pts.push_back(PointF((float)W, H / 2.f));
        std::vector<PointF> curve;   // quadratic curves through the midpoints
        curve.push_back(pts[0]);
        for (size_t i = 1; i + 1 < pts.size(); i++) {
            PointF p0 = curve.back(), c = pts[i], e((pts[i].X + pts[i + 1].X) / 2, (pts[i].Y + pts[i + 1].Y) / 2);
            for (int s = 1; s <= 4; s++) { float u = s / 4.f, v = 1 - u; curve.push_back(PointF(v * v * p0.X + 2 * v * u * c.X + u * u * e.X, v * v * p0.Y + 2 * v * u * c.Y + u * u * e.Y)); }
        }
        curve.push_back(pts.back());
        float rad = 4.8f * sc;
        for (size_t i = 0; i + 1 < curve.size(); i++) {   // the wide glow, stamped along the line
            PointF p0 = curve[i], p1 = curve[i + 1]; int steps = std::max(1, (int)(hypotf(p1.X - p0.X, p1.Y - p0.Y)));
            for (int s = 0; s < steps; s++) {
                float px = p0.X + (p1.X - p0.X) * s / steps, py = p0.Y + (p1.Y - p0.Y) * s / steps;
                for (int y = (int)(py - rad); y <= (int)(py + rad); y++) for (int x = (int)(px - rad); x <= (int)(px + rad); x++) {
                    if (x < 0 || y < 0 || x >= W || y >= H) continue;
                    float* p = &b[((size_t)y * W + x) * 3]; float k = 0.5f * (1 - hypotf(x - px, y - py) / rad); if (k <= 0) continue;
                    p[0] = std::max(p[0], ar * k); p[1] = std::max(p[1], ag * k); p[2] = std::max(p[2], ab * k);
                }
            }
        }
        blur((int)lroundf(3 * sc), 3);
        float kx = 0, ky = 0; RECT rc; GetClientRect(g_party, &rc); kx = (rc.right - rc.left) / (float)W; ky = (rc.bottom - rc.top) / (float)H;
        for (auto& p : curve) line.push_back(D2D1::Point2F(p.X * kx, p.Y * ky));
    }
    std::vector<UINT32> px((size_t)W * H);
    for (size_t i = 0; i < px.size(); i++) {
        auto c8 = [](float v) { return (UINT32)std::max(0, std::min(255, (int)(v * 255))); };
        px[i] = 0xFF000000u | (c8(b[i * 3]) << 16) | (c8(b[i * 3 + 1]) << 8) | c8(b[i * 3 + 2]);
    }
    g_pbmp->CopyFromMemory(nullptr, px.data(), W * 4);
    PartyInfoBitmap();
    g_prt->BeginDraw();
    D2D1_SIZE_F sz = g_prt->GetSize();
    g_prt->DrawBitmap(g_pbmp, D2D1::RectF(0, 0, sz.width, sz.height), 1.f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    if (!line.empty()) {
        if (!g_pline) g_prt->CreateSolidColorBrush(D2D1::ColorF(ar, ag, ab, 1.f), &g_pline);
        if (g_pline) { g_pline->SetColor(D2D1::ColorF(ar, ag, ab, 1.f)); for (size_t i = 0; i + 1 < line.size(); i++) g_prt->DrawLine(line[i], line[i + 1], g_pline, S(3)); }
    }
    if (g_pinfo) { D2D1_SIZE_F is = g_pinfo->GetSize(); g_prt->DrawBitmap(g_pinfo, D2D1::RectF(S(36), sz.height - S(32) - is.height, S(36) + is.width, sz.height - S(32)), 1.f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR); }
    if (g_prt->EndDraw() == (HRESULT)D2DERR_RECREATE_TARGET) PartyFree();
}
static LRESULT CALLBACK PartyProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_ERASEBKGND: { RECT r; GetClientRect(h, &r); FillRect((HDC)wp, &r, (HBRUSH)GetStockObject(BLACK_BRUSH)); return 1; }
    case WM_PAINT: { PAINTSTRUCT ps; BeginPaint(h, &ps); EndPaint(h, &ps); return 0; }
    case WM_SETCURSOR: SetCursor(nullptr); return TRUE;
    case WM_TIMER: {
        if (g_pDir != 0) {
            float k = std::min(1.f, (GetTickCount() - g_pT0) / (float)BLACK_FADE), e = k * k * (3 - 2 * k);
            g_pA = g_pDir == 1 ? (int)(255 * e) : (int)(255 * (1 - e));
            SetLayeredWindowAttributes(h, 0, (BYTE)g_pA, LWA_ALPHA);
            if (k >= 1) { if (g_pDir == -1) { DestroyWindow(h); return 0; } g_pDir = 0; }
        }
        PartyFrame();
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (g_partyClose == 1 || GetTickCount() - g_pTick < 800) return 0;   // Esc only, or ignore the click that started it
        POINT p; GetCursorPos(&p);
        if (abs(p.x - g_pPt.x) > 6 || abs(p.y - g_pPt.y) > 6) PartyEnd();
        return 0;
    }
    case WM_KEYDOWN: case WM_SYSKEYDOWN: case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN: case WM_MOUSEWHEEL:
        if (g_partyClose == 1 && !(msg == WM_KEYDOWN && wp == VK_ESCAPE)) return 0;   // Esc only: other keys and the mouse do nothing
        if (GetTickCount() - g_pTick > 300) PartyEnd();
        return 0;
    case WM_DESTROY:
        KillTimer(h, 1);
        PartyFree();
        if (g_d2f) { g_d2f->Release(); g_d2f = nullptr; }
        g_party = nullptr; g_pDir = 0;
        if (g_pLive) { g_pLive = false; LevelsStop(); ShowCursor(TRUE); ApplyKeepAwake(); }   // the screen may sleep again (unless Keep awake is on)
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}
static void PartyStart() {
    if (g_party) { PartyEnd(); return; }   // the shortcut again ends it
    static bool reg = false;
    if (!reg) { WNDCLASSW wc = {}; wc.lpfnWndProc = PartyProc; wc.hInstance = g_inst; wc.lpszClassName = L"KnobParty"; wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH); RegisterClassW(&wc); reg = true; }
    if (!g_d2f) {   // Direct2D is loaded only for the party screen
        typedef HRESULT (WINAPI* PFN_D2DCF)(D2D1_FACTORY_TYPE, REFIID, const D2D1_FACTORY_OPTIONS*, void**);
        static HMODULE d2d = LoadLibraryW(L"d2d1.dll");
        PFN_D2DCF cf = d2d ? (PFN_D2DCF)GetProcAddress(d2d, "D2D1CreateFactory") : nullptr;
        if (!cf || FAILED(cf(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory), nullptr, (void**)&g_d2f))) return;
    }
    MediaStart(); MediaRefresh();
    HMONITOR hm = PickMonitor(g_partyScr);
    MONITORINFO mi = {sizeof(mi)}; GetMonitorInfoW(hm, &mi);
    RECT r = mi.rcMonitor; int w = r.right - r.left, h = r.bottom - r.top;
    g_party = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED, L"KnobParty", L"", WS_POPUP, r.left, r.top, w, h, nullptr, nullptr, g_inst, nullptr);
    if (!g_party) return;
    SetLayeredWindowAttributes(g_party, 0, 0, LWA_ALPHA);
    if (!PartyGpuInit(g_party, w, h)) {   // no full-resolution path on this PC: the older, lower-resolution one
        D2D1_RENDER_TARGET_PROPERTIES rp = D2D1::RenderTargetProperties();
        if (FAILED(g_d2f->CreateHwndRenderTarget(rp, D2D1::HwndRenderTargetProperties(g_party, D2D1::SizeU(w, h)), &g_prt))) { DestroyWindow(g_party); return; }
    }
    g_pw = 320; g_ph = std::max(90, 320 * h / std::max(1, w));
    g_pTime = 0; g_pInfoKey.clear();
    ShowWindow(g_party, SW_SHOW); SetForegroundWindow(g_party);
    ShowCursor(FALSE);   // undone in WM_DESTROY (g_pLive)
    GetCursorPos(&g_pPt); g_pTick = GetTickCount();
    g_pDir = 1; g_pT0 = GetTickCount(); g_pA = 0;
    SetThreadExecutionState(ES_CONTINUOUS | ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED);   // a party: the screen stays on
    LevelsStart(); g_pLive = true;
    SetTimer(g_party, 1, 33, nullptr);
}

// ---------------------------------------------------------------- Turn off displays: the timer, and the clock on the black screen
static void ScreenOffNow() { g_offAt = 0; KillTimer(g_hwnd, TIMER_OFFAT); HideFlyout(); SetTimer(g_hwnd, TIMER_SCREENOFF, 500, nullptr); }   // after the click settles
static void ScreenOffRequest() { ScreenOffNow(); }   // the button or the shortcut: right away. (Settings can also switch them off by themselves after no activity)
static std::wstring OffLeft() { int s = g_offAt ? std::max(0, (int)(g_offAt - GetTickCount()) / 1000) : 0; wchar_t b[24]; swprintf(b, 24, L"Off in %d:%02d", s / 60, s % 60); return b; }
static bool IsClockScreen(const RECT& rc) {   // does this screen show the clock on the black screen?
    if (!g_offClock) return false;
    if (g_clockScr == L"*") return true;
    HMONITOR want = PickMonitor(g_clockScr);
    MONITORINFO mi = {sizeof(mi)}; GetMonitorInfoW(want, &mi);
    return EqualRect(&mi.rcMonitor, &rc) != 0;
}
static void PaintClock(HWND h, HDC dc) {   // dim grey time and date, like a phone's always-on screen; drifts a little every minute
    RECT rc; GetClientRect(h, &rc); int w = rc.right, hh = rc.bottom;
    HDC mdc = CreateCompatibleDC(dc); HBITMAP bm = CreateCompatibleBitmap(dc, w, hh); HGDIOBJ old = SelectObject(mdc, bm);
    {
        Graphics g(mdc); g.Clear(Color(255, 0, 0, 0)); g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
        static FontFamily* thin = nullptr; if (!thin) thin = PickFamily({L"Segoe UI Variable Display Light", L"Segoe UI Light", L"Segoe UI"});
        float big = std::max(64.f * g_scale, std::min(150.f * g_scale, w * 0.11f)), small = std::max(14.f * g_scale, std::min(20.f * g_scale, w * 0.016f));
        wchar_t tb[64] = {}, db[128] = {};
        GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS | TIME_NOTIMEMARKER, nullptr, nullptr, tb, 64);
        GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, 0, nullptr, L"dddd, MMMM d", db, 128, nullptr);
        std::wstring ts = tb; while (!ts.empty() && ts.back() == L' ') ts.pop_back();
        LONG_PTR seed = GetWindowLongPtrW(h, GWLP_USERDATA) >> 1;   // the drift for this minute
        float dx = ((seed % 97) / 48.f - 1) * w * 0.06f, dy = (((seed / 97) % 97) / 48.f - 1) * hh * 0.06f;
        Font ft(thin, big, FontStyleRegular, UnitPixel), fd(g_famText, small, FontStyleRegular, UnitPixel);
        StringFormat sf; sf.SetAlignment(StringAlignmentCenter);
        SolidBrush b1(Color(255, 92, 92, 92)), b2(Color(255, 74, 74, 74));
        float cy = hh / 2.f + dy;
        g.DrawString(ts.c_str(), -1, &ft, RectF(dx, cy - big * 0.75f, (float)w, big * 1.3f), &sf, &b1);
        g.DrawString(db, -1, &fd, RectF(dx, cy + big * 0.5f, (float)w, small * 2), &sf, &b2);
    }
    BitBlt(dc, 0, 0, w, hh, mdc, 0, 0, SRCCOPY);
    SelectObject(mdc, old); DeleteObject(bm); DeleteDC(mdc);
}

// ---------------------------------------------------------------- refresh rate on battery
static void RateBatApply() {   // the laptop screen: 60 Hz on battery, its highest on power (only when the power source changes)
    if (!g_rateBat || !g_monsLoaded) return;
    SYSTEM_POWER_STATUS ps; if (!GetSystemPowerStatus(&ps) || ps.ACLineStatus > 1) return;
    bool bat = ps.ACLineStatus == 0;
    for (int i = 0; i < (int)g_mons.size(); i++) {
        MonUI& m = g_mons[i];
        if (!m.internal || m.rates.size() < 2) continue;
        int lo = *std::min_element(m.rates.begin(), m.rates.end()), hi = *std::max_element(m.rates.begin(), m.rates.end());
        int want = bat ? (std::find(m.rates.begin(), m.rates.end(), 60) != m.rates.end() ? 60 : lo) : hi;
        if (want != m.rate) SetRefreshRate(i, want);
    }
}

// ---------------------------------------------------------------- arrangement at real size (Settings › Display)
// Windows lines screens up by their pixels, so a small sharp laptop screen looks bigger than a large monitor and the
// edges between them don't meet. Here they're drawn at their real size; drag one where it is on your desk and Apply
// tells Windows. Nothing runs afterwards: the mouse simply crosses where the edges really meet.
struct ArrScreen { int mon = -1; float wmm = 0, hmm = 0; };
static ArrScreen g_arrMain, g_arrOther;
static int g_arrSide = 0;            // where the other screen sits: 0 above, 1 right, 2 below, 3 left of the main one
static float g_arrOff = 0;           // its centre's offset along that edge, in mm
static bool g_arrPix = false, g_arrDrag = false, g_arrDirty = false;
static PointF g_arrGrab, g_arrGrabC; static std::wstring g_arrMsg;
static bool g_lockPop[4] = {};   // Try: the state each lock key showed last
static RectF g_arrRMain, g_arrROther; static float g_arrScale = 1;   // last drawn: px per mm
static bool EdidSize(const std::wstring& gdi, float& wmm, float& hmm) {   // the screen's real size, from its EDID
    DISPLAY_DEVICEW dd = {sizeof(dd)};
    if (!EnumDisplayDevicesW(gdi.c_str(), 0, &dd, EDD_GET_DEVICE_INTERFACE_NAME)) return false;
    std::wstring id = dd.DeviceID;   // \\?\DISPLAY#DELA1C4#5&2d4e8a3&0&UID4353#{...}
    std::vector<std::wstring> part; std::wstring cur;
    for (wchar_t c : id) { if (c == L'#') { part.push_back(cur); cur.clear(); } else cur += c; }
    if (part.size() < 3) return false;
    std::wstring key = L"SYSTEM\\CurrentControlSet\\Enum\\DISPLAY\\" + part[1] + L"\\" + part[2] + L"\\Device Parameters";
    BYTE e[256]; DWORD sz = sizeof(e);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, key.c_str(), L"EDID", RRF_RT_REG_BINARY, nullptr, e, &sz) != ERROR_SUCCESS || sz < 128) return false;
    int w = e[66] | ((e[68] & 0xF0) << 4), h = e[67] | ((e[68] & 0x0F) << 8);   // first detailed timing: mm
    if (w < 100 || h < 60) { w = e[21] * 10; h = e[22] * 10; }                   // else the basic block: cm
    if (w < 100 || h < 60) return false;
    wmm = (float)w; hmm = (float)h;
    return true;
}
static bool ArrReady() {   // two screens side by side (not duplicated), both with a known size
    std::vector<int> act; for (int i = 0; i < (int)g_mons.size(); i++) if (g_mons[i].cloneOf < 0 && g_mons[i].rc.right > g_mons[i].rc.left) act.push_back(i);
    if (act.size() != 2) return false;
    int mi = g_mons[act[0]].rc.left == 0 && g_mons[act[0]].rc.top == 0 ? act[0] : act[1], oi = mi == act[0] ? act[1] : act[0];
    if (g_arrMain.mon != mi || g_arrOther.mon != oi) {   // (re)read the sizes and start from what Windows has now
        g_arrMain = ArrScreen(); g_arrOther = ArrScreen();
        if (g_demo) { g_arrMain = {mi, 302, 189}; g_arrOther = {oi, 598, 336}; }
        else if (!EdidSize(g_mons[mi].gdi, g_arrMain.wmm, g_arrMain.hmm) || !EdidSize(g_mons[oi].gdi, g_arrOther.wmm, g_arrOther.hmm)) return false;
        g_arrMain.mon = mi; g_arrOther.mon = oi;
        const RECT& a = g_mons[mi].rc; const RECT& b = g_mons[oi].rc;
        float mmpx = g_arrMain.wmm / std::max(1L, a.right - a.left);
        if (b.bottom <= a.top) g_arrSide = 0; else if (b.left >= a.right) g_arrSide = 1; else if (b.top >= a.bottom) g_arrSide = 2; else g_arrSide = 3;
        g_arrOff = g_arrSide % 2 == 0 ? ((b.left + b.right) / 2.f - (a.left + a.right) / 2.f) * mmpx : ((b.top + b.bottom) / 2.f - (a.top + a.bottom) / 2.f) * mmpx;
        g_arrDirty = false;
    }
    return g_arrMain.wmm > 0 && g_arrOther.wmm > 0;
}
static void ArrClamp() {   // they always share at least a bit of edge
    float half = g_arrSide % 2 == 0 ? (g_arrMain.wmm + g_arrOther.wmm) / 2 : (g_arrMain.hmm + g_arrOther.hmm) / 2;
    g_arrOff = std::max(-half + 40, std::min(half - 40, g_arrOff));
}
static bool ArrApply() {   // the middle of the shared edge lines up exactly; the rest as near as pixels allow
    if (!ArrReady()) return false;
    const MonUI& M = g_mons[g_arrMain.mon]; const MonUI& O = g_mons[g_arrOther.mon];
    int mw = M.rc.right - M.rc.left, mh = M.rc.bottom - M.rc.top, ow = O.rc.right - O.rc.left, oh = O.rc.bottom - O.rc.top;
    int x = 0, y = 0;
    if (g_arrSide % 2 == 0) {
        float c = g_arrMain.wmm / 2 + g_arrOff, a0 = std::max(0.f, c - g_arrOther.wmm / 2), a1 = std::min(g_arrMain.wmm, c + g_arrOther.wmm / 2), mid = (a0 + a1) / 2;
        float pm = mid * mw / g_arrMain.wmm, po = (mid - (c - g_arrOther.wmm / 2)) * ow / g_arrOther.wmm;
        x = M.rc.left + (int)lroundf(pm - po); y = g_arrSide == 0 ? M.rc.top - oh : M.rc.bottom;
    } else {
        float c = g_arrMain.hmm / 2 + g_arrOff, a0 = std::max(0.f, c - g_arrOther.hmm / 2), a1 = std::min(g_arrMain.hmm, c + g_arrOther.hmm / 2), mid = (a0 + a1) / 2;
        float pm = mid * mh / g_arrMain.hmm, po = (mid - (c - g_arrOther.hmm / 2)) * oh / g_arrOther.hmm;
        y = M.rc.top + (int)lroundf(pm - po); x = g_arrSide == 1 ? M.rc.right : M.rc.left - ow;
    }
    if (g_demo) { g_arrDirty = false; return true; }
    DEVMODEW dm = {}; dm.dmSize = sizeof(dm);
    if (!EnumDisplaySettingsW(O.gdi.c_str(), ENUM_CURRENT_SETTINGS, &dm)) return false;
    dm.dmFields = DM_POSITION; dm.dmPosition.x = x; dm.dmPosition.y = y;
    if (ChangeDisplaySettingsExW(O.gdi.c_str(), &dm, nullptr, CDS_UPDATEREGISTRY | CDS_NORESET, nullptr) != DISP_CHANGE_SUCCESSFUL) return false;
    ChangeDisplaySettingsExW(nullptr, nullptr, nullptr, 0, nullptr);
    g_arrDirty = false;
    return true;
}

// ---------------------------------------------------------------- keys for the flyouts: media keys and lock keys, heard without taking them
// Raw input with INPUTSINK only listens (the key still does its normal job), and only while a flyout that needs it is on.
static bool g_rawOn = false;
static int g_lockPending = -1; static bool g_lockKnown[4] = {}; static bool g_lockLast[4] = {};
static void FlyoutKeysSync() {
    if (g_popMedia || g_popNext) MediaStart();
    bool want = g_popMedia || g_popLock;
    if (want == g_rawOn || !g_hwnd) return;
    RAWINPUTDEVICE rid = {0x01, 0x06, want ? (DWORD)RIDEV_INPUTSINK : (DWORD)RIDEV_REMOVE, want ? g_hwnd : nullptr};
    if (RegisterRawInputDevices(&rid, 1, sizeof(rid))) g_rawOn = want;
}
static void FlyoutKey(USHORT vk) {
    if (vk >= VK_MEDIA_NEXT_TRACK && vk <= VK_MEDIA_PLAY_PAUSE && g_popMedia) {
        g_mediaCmdTick = GetTickCount(); MediaStart(); MediaRefresh();
        SetTimer(g_hwnd, TIMER_POPKEY, 220, nullptr);   // a moment for the player to react, then show what it's doing
        return;
    }
    int k = vk == VK_CAPITAL ? 0 : vk == VK_NUMLOCK ? 1 : vk == VK_SCROLL ? 2 : vk == VK_INSERT ? 3 : -1;
    if (k < 0 || !g_popLock || !(g_lkKeys & (1 << k))) return;
    g_lockPending = k; SetTimer(g_hwnd, TIMER_LOCKKEY, 40, nullptr);
}
static void FlyoutLockNow() {
    int k = g_lockPending; g_lockPending = -1;
    if (k < 0) return;
    bool on;
    if (k == 3) on = g_insertOn = !g_insertOn;   // Insert has no light: Knob keeps count
    else {
        static const int vks[3] = {VK_CAPITAL, VK_NUMLOCK, VK_SCROLL};
        on = (GetKeyState(vks[k]) & 1) != 0;
        if (g_lockKnown[k] && on == g_lockLast[k]) on = !on;   // the state hadn't reached this app yet: every press flips it
    }
    g_lockKnown[k] = true; g_lockLast[k] = on;
    PopLockKey(k, on);
}

// ---------------------------------------------------------------- settings window
enum { SP_GENERAL, SP_SOUND, SP_DISPLAY, SP_FLYOUTS, SP_APPEAR, SP_ABOUT, SP_COUNT };
static const wchar_t* SP_NAMES[SP_COUNT] = {L"General", L"Sound", L"Display", L"Flyouts", L"Appearance", L"About"};
enum SKind { SK_NAV, SK_SWITCH, SK_LINK, SK_BUTTON, SK_STYLE, SK_SWATCH, SK_HK, SK_STEP, SK_PAPPLY, SK_PUPDATE, SK_PRENAME, SK_PDELETE, SK_PNEW, SK_DELAY, SK_RESETHK, SK_ACCMODE, SK_COLOR, SK_THEME, SK_DDBTN, SK_DDOPT, SK_HKDEL, SK_HKADD, SK_RTSRC, SK_RTDST, SK_RTDELAY, SK_NLSL, SK_ARR, SK_ARRVIEW, SK_CHIP, SK_MS };
static int g_sddOpen = -1;
static float g_sddScroll = 0, g_sddMax = 0;   // an open list taller than the window scrolls on its own
static RectF g_sddRect;
static float g_segPos[4] = {-1, -1, -1, -1};   // animated pill position: theme, style, accent, arrangement view
static float SegPos(int which, int target) {   // eases towards the selected option; keeps repainting until it gets there
    float& p = g_segPos[which];
    if (p < 0) p = (float)target;
    if (fabsf(p - target) > 0.01f) { p += (target - p) * 0.35f; if (g_set) SetTimer(g_set, 77, 15, nullptr); }
    else p = (float)target;
    return p;
}   // open Settings dropdown: 1 call device, 2 during calls
struct SItem { int kind; int id; RectF r; };
enum { WM_APP_STARTEDIT = WM_APP + 20 };

static HWND g_edit;
static WNDPROC g_editProc;
static HFONT g_editFont;
static HBRUSH g_editBrush;
static int g_setPage = SP_GENERAL, g_editProfile = -1, g_sDragDelay = -1, g_sDragColor = 0;   // 1 hue, 2 intensity
static int g_sDragNl = -1;   // night light strength slider being dragged (screen index)
static int g_setW = 0, g_setH = 0;
static float g_setScroll = 0, g_setContentH = 0;
static std::vector<SItem> g_sItems;
static int g_sHover = -1;
static Theme g_setTheme;
static RectF g_editRect;
static const float NAVW = 220;
// routing board (Settings › Sound): sounds on the left, speakers on the right, a line = "plays there"
struct RtLine { int src, dst; PointF p[4]; };
static std::vector<RtLine> g_rtLines;
static std::map<int, RectF> g_rtSrc, g_rtDst;   // src: RT_EVERY, an app index, or RT_MIC0 - mic index; dst: output index
static const int RT_NONE = -1000, RT_EVERY = -1, RT_MIC0 = -100;
static int g_rtDrag = RT_NONE, g_rtSel = RT_NONE, g_rtHoverLine = -1;
static int g_rtDragDst = -1, g_rtSelDst = -1;   // dragging from / selected speaker
static std::vector<std::wstring> RtRoutes(int src) {   // the speakers a sound was sent to (lines on the board)
    std::vector<std::wstring> r;
    if (src == RT_EVERY) { for (int i = 0; i < (int)g_out.size(); i++) if (OutSelected(i)) r.push_back(g_out[i].id); }
    else if (src <= RT_MIC0) { int k = RT_MIC0 - src; if (k < (int)g_in.size()) { auto it = g_micRoutes.find(g_in[k].id); if (it != g_micRoutes.end()) r = it->second; } }
    else if (src >= 0 && src < (int)g_apps.size()) { const AppGroup& a = g_apps[src]; if (!a.outId.empty() || g_appExtra.count(a.key)) r = AppRoutes(a); }   // an app with no line of its own plays with Everything else
    return r;
}
static bool RtLineRemovable(const RtLine& l) {   // Everything else always keeps one speaker
    if (l.src != RT_EVERY) return true;
    int n = 0; for (int i = 0; i < (int)g_out.size(); i++) if (OutSelected(i)) n++;
    return n > 1;
}
static void RtConnect(int src, int dst) {   // a line from a sound to a speaker: it plays there too
    if (dst < 0 || dst >= (int)g_out.size()) return;
    const std::wstring& id = g_out[dst].id;
    if (src == RT_EVERY) { if (!OutSelected(dst)) ToggleOutput(dst); }
    else if (src <= RT_MIC0) {
        int k = RT_MIC0 - src; if (k >= (int)g_in.size()) return;
        auto& v = g_micRoutes[g_in[k].id];
        if (std::find(v.begin(), v.end(), id) == v.end()) { v.push_back(id); RestartMicRouters(); }
    } else if (src >= 0 && src < (int)g_apps.size()) {
        AppGroup& a = g_apps[src];
        if (a.outId.empty() && !g_appExtra.count(a.key) && g_out[dst].isDefault) { if (AppOutputSet(a, id)) a.outId = id; }   // pin it to the speaker it's already on
        else AppRouteAdd(a, id);
    }
}
static void RtDisconnect(const RtLine& l) {   // clicked a line: that sound stops playing there
    if (!RtLineRemovable(l) || l.dst < 0 || l.dst >= (int)g_out.size()) return;
    const std::wstring id = g_out[l.dst].id;
    if (l.src == RT_EVERY) ToggleOutput(l.dst);
    else if (l.src <= RT_MIC0) {
        int k = RT_MIC0 - l.src; if (k >= (int)g_in.size()) return;
        auto it = g_micRoutes.find(g_in[k].id);
        if (it != g_micRoutes.end()) { it->second.erase(std::remove(it->second.begin(), it->second.end(), id), it->second.end()); if (it->second.empty()) g_micRoutes.erase(it); RestartMicRouters(); }
    } else if (l.src >= 0 && l.src < (int)g_apps.size()) AppRouteRemove(g_apps[l.src], id);
}
static PointF g_rtMouse;
static int g_editDelay = -1;    // speaker whose delay is being typed
static int g_editMs = -1;       // Settings › Flyouts: which time is being typed (0 stay, 1 animation, 2 lock keys stay)
static std::vector<int> g_hkAddMap;   // "Add a shortcut" list row -> action
static PointF Bez(const RtLine& l, float t) { float u = 1 - t; return PointF(u*u*u*l.p[0].X + 3*u*u*t*l.p[1].X + 3*u*t*t*l.p[2].X + t*t*t*l.p[3].X,
                                                                        u*u*u*l.p[0].Y + 3*u*u*t*l.p[1].Y + 3*u*t*t*l.p[2].Y + t*t*t*l.p[3].Y); }
static int RtLineAt(float x, float y) {   // the line under the mouse, if any
    for (int i = (int)g_rtLines.size() - 1; i >= 0; i--)
        for (int k = 0; k <= 24; k++) { PointF q = Bez(g_rtLines[i], k / 24.f); if ((q.X - x) * (q.X - x) + (q.Y - y) * (q.Y - y) < S(6) * S(6)) return i; }
    return -1;
}

static bool g_setGlass = false;   // Settings window uses the Glass style too
// The title bar is painted by Windows, so it is faded by hand: its colour follows the same curve as the window body.
static COLORREF g_capCur = 0xFFFFFFFF, g_capFrom = 0, g_capTo = 0;   // 0xFFFFFFFF: not set yet
static bool g_capAnim = false, g_capDarkDone = true; static BOOL g_capDark = FALSE;
static COLORREF LerpRGB(COLORREF a, COLORREF b, float t) {
    auto ch = [&](int sh) { int x = (a >> sh) & 255, y = (b >> sh) & 255; return (int)(x + (y - x) * t + .5f); };
    return RGB(ch(0), ch(8), ch(16));
}
static void SetCaption(COLORREF c) {
    DwmSetWindowAttribute(g_set, 35 /*DWMWA_CAPTION_COLOR*/, &c, sizeof(c));
    DwmSetWindowAttribute(g_set, 34 /*DWMWA_BORDER_COLOR*/, &c, sizeof(c));
    g_capCur = c;
}
static void CaptionStep(float t) {   // after each Settings frame: t is the fade's eased progress, 1 when finished
    if (!g_capAnim || !g_set) return;
    if (t >= .5f && !g_capDarkDone) { DwmSetWindowAttribute(g_set, 20, &g_capDark, sizeof(g_capDark)); g_capDarkDone = true; }   // caption buttons and text flip halfway
    if (t >= 1) { SetCaption(g_capTo); g_capAnim = false; } else SetCaption(LerpRGB(g_capFrom, g_capTo, t));
}
static void ApplySettingsStyle() {
    if (!g_set) return;
    g_setGlass = false;   // Settings always uses the solid look; Glass is for the panel only
    BOOL dark = g_setTheme.dark;
    int backdrop = g_setGlass ? 3 : 1;   // acrylic behind the whole window (title bar included), or none
    DwmSetWindowAttribute(g_set, 38, &backdrop, sizeof(backdrop));
    MARGINS m = g_setGlass ? MARGINS{-1, -1, -1, -1} : MARGINS{0, 0, 0, 0};
    DwmExtendFrameIntoClientArea(g_set, &m);
    // solid: paint the title bar the same colour as the window body
    COLORREF cap = g_setTheme.dark ? RGB(32, 32, 32) : RGB(243, 243, 243);
    if (g_fadeSet.on && g_capCur != 0xFFFFFFFF && cap != g_capCur) {   // theme change with the body fading: fade the title bar with it
        g_capFrom = g_capCur; g_capTo = cap; g_capDark = dark; g_capDarkDone = false; g_capAnim = true;
    } else { g_capAnim = false; DwmSetWindowAttribute(g_set, 20, &dark, sizeof(dark)); SetCaption(cap); }
    InvalidateRect(g_set, nullptr, FALSE);
}
static void BuildSettingsTheme() {
    Theme keep = T;
    T.glass = false;
    LoadTheme();
    g_setTheme = T;
    T = keep;
}

static bool HandleCaptureKey(UINT vk) {   // shared by the flyout and the settings window
    if (g_capture < 0 || g_capture >= (int)g_sc.size()) { g_capture = -1; return false; }
    if (vk == VK_CONTROL || vk == VK_MENU || vk == VK_SHIFT || vk == VK_LWIN || vk == VK_RWIN) return true;
    UINT mods = (GetKeyState(VK_CONTROL) < 0 ? MOD_CONTROL : 0) | (GetKeyState(VK_MENU) < 0 ? MOD_ALT : 0) |
                (GetKeyState(VK_SHIFT) < 0 ? MOD_SHIFT : 0) | ((GetKeyState(VK_LWIN) < 0 || GetKeyState(VK_RWIN) < 0) ? MOD_WIN : 0);
    if (vk == VK_ESCAPE && !mods) g_capture = -1;
    else if ((vk == VK_BACK || vk == VK_DELETE) && !mods) { g_sc[g_capture].k.mods = 0; g_sc[g_capture].k.vk = 0; g_capture = -1; }
    else if (mods || (vk >= VK_F13 && vk <= VK_F24)) { g_sc[g_capture].k.mods = mods; g_sc[g_capture].k.vk = vk; g_capture = -1; }
    else return true;   // a lone key would steal normal typing: wait for a combination
    RegisterHotkeys(); SaveSettings();
    return true;
}

static void CommitRename(bool keep) {
    if (!g_edit) return;
    if (g_editMs >= 0) {   // a time in milliseconds
        if (keep) {
            wchar_t b[16]; GetWindowTextW(g_edit, b, 16);
            int v = _wtoi(b);
            if (g_editMs == 0) g_fStayMs = std::max(0, std::min(60000, v)); else if (g_editMs == 2) g_lkStayMs = std::max(100, std::min(60000, v)); else g_fAnimMs = std::max(0, std::min(2000, v));
            SaveSettings();
        }
        HWND e = g_edit; g_edit = nullptr; g_editMs = -1; DestroyWindow(e);
        if (g_set) InvalidateRect(g_set, nullptr, FALSE);
        return;
    }
    if (g_editDelay >= 0) {   // typed sync delay
        if (keep && g_editDelay < (int)g_out.size()) {
            wchar_t b[16]; GetWindowTextW(g_edit, b, 16);
            int v = std::max(0, std::min(2000, (int)lround(_wtoi(b) / 5.0) * 5));
            g_delay[g_out[g_editDelay].id] = v + MinDelay(); SaveSettings(); RestartRouter();
        }
        HWND e = g_edit; g_edit = nullptr; g_editDelay = -1; DestroyWindow(e);
        if (g_set) InvalidateRect(g_set, nullptr, FALSE);
        if (g_visible) InvalidateRect(g_hwnd, nullptr, FALSE);
        return;
    }
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

static void StartDelayEdit(int i) {
    CommitRename(true);
    if (!g_set || i < 0 || i >= (int)g_out.size()) return;
    g_editDelay = i;
    if (g_editFont) DeleteObject(g_editFont);
    g_editFont = CreateFontW(-(int)S(13), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    g_edit = CreateWindowExW(0, L"EDIT", std::to_wstring(ShownDelay(g_out[i].id)).c_str(), WS_CHILD | WS_VISIBLE | ES_CENTER | ES_NUMBER, 0, 0, 10, 10, g_set, (HMENU)1, g_inst, nullptr);
    SendMessageW(g_edit, WM_SETFONT, (WPARAM)g_editFont, TRUE);
    SendMessageW(g_edit, EM_LIMITTEXT, 3, 0);
    g_editProc = (WNDPROC)SetWindowLongPtrW(g_edit, GWLP_WNDPROC, (LONG_PTR)EditSubclass);
    SendMessageW(g_edit, EM_SETSEL, 0, -1);
    g_editRect = RectF();
    InvalidateRect(g_set, nullptr, FALSE);
    SetFocus(g_edit);
}
static void StartMsEdit(int id) {
    CommitRename(true);
    if (!g_set) return;
    g_editMs = id;
    if (g_editFont) DeleteObject(g_editFont);
    g_editFont = CreateFontW(-(int)S(13), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    g_edit = CreateWindowExW(0, L"EDIT", std::to_wstring(id == 0 ? g_fStayMs : id == 2 ? g_lkStayMs : g_fAnimMs).c_str(), WS_CHILD | WS_VISIBLE | ES_NUMBER | ES_AUTOHSCROLL, 0, 0, 10, 10, g_set, (HMENU)1, g_inst, nullptr);
    SendMessageW(g_edit, WM_SETFONT, (WPARAM)g_editFont, TRUE);
    SendMessageW(g_edit, EM_LIMITTEXT, 5, 0);
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

// lines of wrapped text (the design wraps descriptions and notes; GDI+ can't set line height, so lines are drawn one by one)
static std::vector<std::wstring> WrapLines(Graphics& g, const std::wstring& s, Font* f, float w) {
    std::vector<std::wstring> out; std::wstring line, word;
    auto width = [&](const std::wstring& t) { RectF r; g.MeasureString(t.c_str(), -1, f, PointF(0, 0), &r); return r.Width - S(2); };
    size_t i = 0;
    while (i <= s.size()) {
        if (i == s.size() || s[i] == L' ' || s[i] == L'\n') {
            std::wstring cand = line.empty() ? word : line + L" " + word;
            if (!line.empty() && width(cand) > w) { out.push_back(line); line = word; } else line = cand;
            if (i < s.size() && s[i] == L'\n') { out.push_back(line); line.clear(); }
            word.clear();
        } else word += s[i];
        i++;
    }
    if (!line.empty()) out.push_back(line);
    return out;
}
static void DrawLinesAt(Graphics& g, const std::vector<std::wstring>& ls, Font* f, Color c, float x, float y, float lh) {
    for (size_t i = 0; i < ls.size(); i++) Text(g, ls[i].c_str(), f, c, RectF(x - S(1), y + i * lh, S(2000), lh));
}

static void PaintSettings(HDC hdc) {
    if (g_setW <= 0 || g_setH <= 0) return;
    void* bits = nullptr;
    HBITMAP dib = CanvasFor(g_cvSet, hdc, g_setW, g_setH, &bits);
    if (!dib) return;
    std::swap(T, g_setTheme);
    HDC mdc = CreateCompatibleDC(hdc);
    HGDIOBJ old = SelectObject(mdc, dib);
    RectF editRect;
    g_sItems.clear();
    {
        Bitmap canvas(g_setW, g_setH, g_setW * 4, PixelFormat32bppPARGB, (BYTE*)bits);
        Graphics g(&canvas);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
        g.SetPixelOffsetMode(PixelOffsetModeHalf);
        Color winBg = T.dark ? Color(255, 32, 32, 32) : Color(255, 243, 243, 243);
        g.Clear(winBg);
        // the design's type: page title 24 semibold, card title 14, description 12, section 13 semibold
        Font fTitle(g_famSemi, S(24), FontStyleRegular, UnitPixel), fBody(g_famText, S(14), FontStyleRegular, UnitPixel), fBody13(g_famText, S(13), FontStyleRegular, UnitPixel);
        Font fBody600(g_famSemi, S(14), FontStyleRegular, UnitPixel), fDesc(g_famText, S(12), FontStyleRegular, UnitPixel), fDesc600(g_famSemi, S(12), FontStyleRegular, UnitPixel);
        Font fH4(g_famSemi, S(13), FontStyleRegular, UnitPixel), fNav600(g_famSemi, S(13), FontStyleRegular, UnitPixel), fSmall115(g_famText, S(11.5f), FontStyleRegular, UnitPixel);
        Font fRt(g_famText, S(12.5f), FontStyleRegular, UnitPixel), fRhd(g_famSemi, S(11), FontStyleRegular, UnitPixel), fNew(g_famSemi, S(9), FontStyleBold, UnitPixel);
        Font fBold14(g_famSemi, S(14), FontStyleBold, UnitPixel), fNum11(g_famSemi, S(11), FontStyleRegular, UnitPixel), fPill(g_famSemi, S(11.5f), FontStyleRegular, UnitPixel);
        Color card = T.dark ? Color(255, 43, 43, 43) : Color(255, 251, 251, 251);
        Color cardHover = T.dark ? Color(255, 47, 47, 47) : Color(255, 255, 255, 255);
        auto add = [&](int k, int id, RectF r) { g_sItems.push_back({k, id, r}); return (int)g_sItems.size() - 1; };
        auto isHover = [&](int k, int id) { return g_sHover >= 0 && g_sHover < (int)g_sItems.size() && g_sItems[g_sHover].kind == k && g_sItems[g_sHover].id == id; };
        auto tw = [&](const wchar_t* s, Font* f) { RectF r; g.MeasureString(s, -1, f, PointF(0, 0), &r); return r.Width - S(2); };

        // ---- navigation (200 wide): Knob, then the five pages
        Icon(g, "mixer", S(16), T.t1, S(28), S(27));
        Text(g, L"Knob", &fBody600, T.t1, RectF(S(47), S(17), S(140), S(20)));
        for (int i = 0; i < SP_COUNT; i++) {
            RectF r(S(8), S(51) + i * S(38), S(NAVW) - S(16), S(36));
            add(SK_NAV, i, r);
            bool sel = g_setPage == i;
            if (sel) { FillRound(g, T.subtle2, r, S(6)); FillRound(g, T.accent, RectF(r.X, r.Y + S(10), S(3), S(16)), S(1.5f)); }
            else if (isHover(SK_NAV, i)) FillRound(g, T.subtle, r, S(6));
            static const char* navIcons[SP_COUNT] = {"gear", "speaker", "monitor", "mFlyout", "palette", "info"};
            Icon(g, navIcons[i], S(16), T.t1, r.X + S(20), r.Y + r.Height / 2);
            Text(g, SP_NAMES[i], sel ? &fNav600 : &fBody13, T.t1, RectF(r.X + S(39), r.Y, r.Width - S(44), r.Height));
        }

        // ---- page: 12 px in, 436 px cards, margins that collapse like the design's
        float x0 = S(NAVW) + S(12), cw = g_setW - x0 - S(32);
        g.SetClip(RectF(S(NAVW), 0, g_setW - S(NAVW), (float)g_setH));
        float y = S(12) - g_setScroll, mb = 0;
        auto top = [&](float mt) { y = y - mb + std::max(mb, mt); };
        auto after = [&](float h, float m) { y += h + m; mb = m; };
        top(S(6)); Text(g, SP_NAMES[g_setPage], &fTitle, T.t1, RectF(x0 + S(3), y - S(2), cw, S(33))); after(S(29), S(18));
        auto h4 = [&](const wchar_t* t) { top(S(18)); Text(g, t, &fH4, T.t1, RectF(x0 + S(3), y - S(1), cw, S(18))); after(S(16), S(8)); };
        auto snote = [&](const wchar_t* t, float mBottom = 14) {   // help text: 12 px, line height 18
            top(0); auto ls = WrapLines(g, t, &fDesc, cw - S(8)); DrawLinesAt(g, ls, &fDesc, T.t3, x0 + S(4), y + S(1.5f), S(18)); after(ls.size() * S(18), S(mBottom));
        };
        // title + description block; returns its height; draws when d is true
        auto newTag = [&](int bit) { return bit >= 0 && !(g_newSeen & (1 << bit)); };
        auto stBlock = [&](float x, float yy, float w, const wchar_t* t, const wchar_t* d, int newBit, bool draw, Color dc = Color(0, 0, 0, 0)) {
            std::wstring tt = t; bool nw = newTag(newBit);
            auto tl = WrapLines(g, nw ? tt + L" NEWWW" : tt, &fBody, w);   // leave room for the tag
            if (nw && !tl.empty()) { std::wstring& last = tl.back(); size_t p = last.rfind(L" NEWWW"); if (p != std::wstring::npos) last = last.substr(0, p); else if (last == L"NEWWW") tl.pop_back(); }
            std::vector<std::wstring> dl; if (d && *d) dl = WrapLines(g, d, &fDesc, w);
            float h = tl.size() * S(17) + (dl.empty() ? 0 : S(2) + dl.size() * S(15));
            if (draw) {
                DrawLinesAt(g, tl, &fBody, T.t1, x, yy, S(17));
                if (nw && !tl.empty()) {   // small "NEW" after the title, in the accent
                    float lx = x + tw(tl.back().c_str(), &fBody) + S(6), ly = yy + (tl.size() - 1) * S(17) + S(8.5f) + S(0.5f);
                    SpacedText(g, L"NEW", &fNew, T.accent, lx, ly, S(0.55f));
                }
                if (!dl.empty()) DrawLinesAt(g, dl, &fDesc, dc.GetA() ? dc : T.t3, x, yy + tl.size() * S(17) + S(2), S(15));
            }
            return h;
        };
        // a card: 18 px in, at least 52 tall, the text block beside whatever sits on the right (rightW)
        auto cardStart = [&](const wchar_t* t, const wchar_t* d, float rightW, int newBit, RectF& r, float minH = 52, Color dc = Color(0, 0, 0, 0)) {
            top(0);
            float stw = cw - S(36) - (rightW > 0 ? rightW + S(10) : 0);
            float h = stBlock(0, 0, stw, t, d, newBit, false);
            r = RectF(x0, y, cw, std::max(S(minH), h + S(20)));
            return [&, t, d, stw, h, newBit, dc](RectF rr, Color bg) { FillRound(g, bg, rr, S(6)); stBlock(rr.X + S(18), rr.Y + (rr.Height - h) / 2, stw, t, d, newBit, true, dc); };
        };
        auto cardEnd = [&](const RectF& r) { after(r.Height, S(4)); };
        auto sbtnW = [&](const wchar_t* t, bool acc = false) { return std::max(S(72), S(32) + tw(t, acc ? &fDesc600 : &fDesc)); };
        auto sbtn = [&](int kind, int id, RectF r, const wchar_t* t, bool accent = false, bool enabled = true) {
            if (enabled) add(kind, id, r);
            bool hv = enabled && isHover(kind, id);
            Color bg = accent && enabled ? (hv ? WithAlpha(T.accent, 225) : T.accent) : (hv ? T.subtle3 : T.subtle2);
            FillRound(g, bg, r, S(5));
            Text(g, t, accent && enabled ? &fDesc600 : &fDesc, accent && enabled ? T.onAccent : (enabled ? T.t1 : T.t3), r, StringAlignmentCenter);
        };
        auto sib = [&](int kind, int id, RectF r, const char* icon) {
            add(kind, id, r);
            if (isHover(kind, id)) FillRound(g, T.subtle2, r, S(5));
            Icon(g, icon, S(15), T.t1, r.X + r.Width / 2, r.Y + r.Height / 2);
        };
        // dropdowns styled like the rest of Knob; the open list is drawn last so it sits on top
        RectF ddPopBtn; std::vector<std::wstring> ddPopOpts; int ddPopCur = -1, ddPopId = -1;
        auto ddButton = [&](int id, RectF b, const std::wstring& label, const std::vector<std::wstring>& opts, int cur) {
            add(SK_DDBTN, id, b);
            bool open = g_sddOpen == id;
            FillRound(g, open || isHover(SK_DDBTN, id) ? T.subtle3 : T.subtle2, b, S(6));
            PillText(g, label.c_str(), &fDesc, T.t1, RectF(b.X + S(12), b.Y, b.Width - S(40), b.Height));
            Icon(g, "chevron", S(14), T.t1, b.X + b.Width - S(12) - S(7), b.Y + b.Height / 2, open ? 180.f : 0.f);
            if (open) { ddPopBtn = b; ddPopOpts = opts; ddPopCur = cur; ddPopId = id; }
        };
        auto ddCard = [&](int id, const wchar_t* t, const wchar_t* d, const std::vector<std::wstring>& opts, int cur, int newBit) {
            RectF r; auto draw = cardStart(t, d, S(210), newBit, r); draw(r, card);
            RectF b(r.X + r.Width - S(18) - S(210), r.Y + r.Height / 2 - S(16), S(210), S(32));
            ddButton(id, b, cur >= 0 && cur < (int)opts.size() ? opts[cur] : L"", opts, cur);
            cardEnd(r);
        };
        auto switchRight = [&](RectF r, bool on) {
            DrawSwitch(g, r.X + r.Width - S(18) - S(40), r.Y + r.Height / 2, on);
            Text(g, on ? L"On" : L"Off", &fBody13, T.t1, RectF(r.X + r.Width - S(18) - S(40) - S(16) - S(60), r.Y, S(60), r.Height), StringAlignmentFar);
        };
        auto switchCard = [&](int id, const wchar_t* t, const wchar_t* d, bool on, int newBit = -1) {
            RectF r; auto draw = cardStart(t, d, S(19) + S(16) + S(40), newBit, r);
            add(SK_SWITCH, id, r);
            draw(r, isHover(SK_SWITCH, id) ? cardHover : card);
            switchRight(r, on);
            cardEnd(r);
        };
        auto msCard = [&](int id, const wchar_t* t, const wchar_t* d, int v) {   // a time, typed in milliseconds
            RectF r; auto draw = cardStart(t, d, S(130), -1, r); draw(r, card);
            RectF b(r.X + r.Width - S(18) - S(130), r.Y + r.Height / 2 - S(16), S(130), S(32));
            bool ed = g_editMs == id;
            add(SK_MS, id, b);
            FillRound(g, ed || isHover(SK_MS, id) ? T.subtle3 : T.subtle2, b, S(6));
            if (ed) { Pen ep(T.accent, S(1.5f)); GraphicsPath gp; AddRound(gp, b, S(6)); g.DrawPath(&ep, &gp); editRect = RectF(b.X + S(12), b.Y + S(8), b.Width - S(50), S(17)); }
            else { wchar_t n[16]; swprintf(n, 16, L"%d", v); Text(g, n, &fBody13, T.t1, RectF(b.X + S(12), b.Y, b.Width - S(48), b.Height)); }
            Text(g, L"ms", &fBody13, T.t3, RectF(b.X + b.Width - S(36), b.Y, S(26), b.Height), StringAlignmentFar);
            cardEnd(r);
        };
        auto linkCard = [&](int id, const wchar_t* t, const wchar_t* d) {
            RectF r; auto draw = cardStart(t, d, S(14), -1, r);
            add(SK_LINK, id, r);
            draw(r, isHover(SK_LINK, id) ? cardHover : card);
            Icon(g, "open", S(14), T.t1, r.X + r.Width - S(18) - S(7), r.Y + r.Height / 2);
            cardEnd(r);
        };
        auto btnCard = [&](const wchar_t* t, const wchar_t* d, int kind, int id, const wchar_t* label, bool accent = false, bool enabled = true) {
            float bw = sbtnW(label, accent);
            RectF r; auto draw = cardStart(t, d, bw, -1, r); draw(r, card);
            sbtn(kind, id, RectF(r.X + r.Width - S(18) - bw, r.Y + r.Height / 2 - S(16), bw, S(32)), label, accent, enabled);
            cardEnd(r);
        };
        auto segc = [&](RectF r, int which, int kind, const std::vector<const wchar_t*>& labels, int cur) {   // segmented control, right of a card
            std::vector<float> ws; float total = S(6) + S(2) * (labels.size() - 1);
            for (auto* l : labels) { float w = std::max(S(64), S(20) + tw(l, &fDesc600)); ws.push_back(w); total += w; }
            RectF box(r.X + r.Width - S(18) - total, r.Y + r.Height / 2 - S(16), total, S(32));
            FillRound(g, T.subtle, box, S(8));
            std::vector<float> xs; float bx = box.X + S(3); for (float w : ws) { xs.push_back(bx); bx += w + S(2); }
            float p = SegPos(which, cur); int a = std::max(0, std::min((int)ws.size() - 1, (int)floorf(p))), b2 = std::min((int)ws.size() - 1, a + 1); float f = p - a;
            FillRound(g, T.dark ? T.subtle3 : Color(255, 255, 255, 255), RectF(xs[a] + (xs[b2] - xs[a]) * f, box.Y + S(3), ws[a] + (ws[b2] - ws[a]) * f, S(26)), S(6));
            for (size_t i = 0; i < labels.size(); i++) {
                RectF b(xs[i], box.Y + S(3), ws[i], S(26));
                add(kind, (int)i, b);
                bool on = cur == (int)i;
                if (!on && isHover(kind, (int)i)) FillRound(g, T.subtle, b, S(6));
                Text(g, labels[i], on ? &fDesc600 : &fDesc, on ? T.t1 : T.t2, b, StringAlignmentCenter);
            }
            return box.Width;
        };
        auto segW = [&](const std::vector<const wchar_t*>& labels) { float t2 = S(6) + S(2) * (labels.size() - 1); for (auto* l : labels) t2 += std::max(S(64), S(20) + tw(l, &fDesc600)); return t2; };

        switch (g_setPage) {
        case SP_GENERAL: {
            switchCard(1, L"Keep awake", L"Stop the screen from turning off and the PC from sleeping", g_keepAwake);
            switchCard(2, L"Start with Windows", L"Open Knob in the tray when you sign in", StartupEnabled());
            ddCard(3, L"Open the panel on", L"Main display opens by the tray, like Windows' own panels. Screen with the mouse follows you when you use the shortcut", {L"Main display", L"Screen with the mouse"}, g_openOn, 0);
            h4(L"Shortcuts");
            for (int i = 0; i < (int)g_sc.size(); i++) {   // a row per shortcut: action, which one (speaker / profile), keys, delete
                Shortcut& sc = g_sc[i];
                bool cap = g_capture == i, dupe = HkSameKeys(i), taken = sc.k.vk && !sc.k.ok && !cap && !dupe;
                const wchar_t* warn = dupe ? L"Same keys as another shortcut" : taken ? L"Another app already uses these keys" : L"";
                int pk = HkParamKind(sc.act);
                std::vector<std::wstring> popts; int pcur = -1;
                if (pk == 1) for (auto& o : g_out) { popts.push_back(OutLabel(o)); if (OutLabel(o) == sc.param || o.name == sc.param) pcur = (int)popts.size() - 1; }
                if (pk == 2) for (auto& p : g_prof) { popts.push_back(p.name); if (p.name == sc.param) pcur = (int)popts.size() - 1; }
                float pw = 0; std::wstring plabel = pcur >= 0 ? popts[pcur] : (sc.param.empty() ? L"Pick one" : sc.param);
                if (pk) pw = std::min(S(120), S(10) + tw(plabel.c_str(), &fDesc) + S(30));
                float rightW = (pk ? pw + S(10) : 0) + S(150) + S(10) + S(32);
                RectF r; auto draw = cardStart(HK_NAMES[sc.act], warn, rightW, -1, r, 52, T.accent); draw(r, card);
                float bx = r.X + r.Width - S(18) - S(32);
                sib(SK_HKDEL, i, RectF(bx, r.Y + r.Height / 2 - S(16), S(32), S(32)), "trash");
                bx -= S(10) + S(150);
                RectF c(bx, r.Y + r.Height / 2 - S(16), S(150), S(32));
                add(SK_HK, i, c);
                FillRound(g, cap ? WithAlpha(T.accent, 64) : (isHover(SK_HK, i) ? T.subtle3 : T.subtle2), c, S(6));
                std::wstring kt = cap ? L"Press keys…" : sc.k.vk ? ComboName(sc.k) : L"Not set";
                Text(g, kt.c_str(), &fDesc, cap || sc.k.vk ? T.t1 : T.t3, c, StringAlignmentCenter);
                if (pk) { bx -= S(10) + pw; ddButton(1000 + i, RectF(bx, c.Y, pw, S(32)), plabel, popts, pcur); }
                cardEnd(r);
            }
            {   // + Add a shortcut: the actions without a row yet ("Switch to" and "Apply profile" can be added again)
                g_hkAddMap.clear();
                std::vector<std::wstring> opts;
                for (int a = 0; a < HK_COUNT; a++) {
                    bool have = false; for (auto& s2 : g_sc) if (s2.act == a) have = true;
                    if (have && !HkParamKind(a)) continue;
                    opts.push_back(HK_NAMES[a]); g_hkAddMap.push_back(a);
                }
                top(0);
                RectF b(x0, y, cw, S(36));
                add(SK_HKADD, 0, b);
                bool hv = isHover(SK_HKADD, 0) || g_sddOpen == 4;
                Pen dp(WithAlpha(T.t1, 56), S(1)); dp.SetDashStyle(DashStyleDash);
                GraphicsPath gp; AddRound(gp, RectF(b.X + S(0.5f), b.Y + S(0.5f), b.Width - S(1), b.Height - S(1)), S(6)); g.DrawPath(&dp, &gp);
                float lw = S(12) + S(6) + tw(L"Add a shortcut", &fSmall115), lx = b.X + (b.Width - lw) / 2;
                Icon(g, "plus", S(12), hv ? T.t1 : T.t3, lx + S(6), b.Y + b.Height / 2);
                Text(g, L"Add a shortcut", &fSmall115, hv ? T.t1 : T.t3, RectF(lx + S(18), b.Y, lw, b.Height));
                if (g_sddOpen == 4) { ddPopBtn = RectF(b.X + b.Width / 2 - S(130), b.Y, S(260), b.Height); ddPopOpts = opts; ddPopCur = -1; ddPopId = 4; }
                after(S(36), 0);
            }
            snote(L"Works from anywhere. Click the keys, then press new ones (Ctrl, Alt, Shift or Win plus a key). Esc cancels, Backspace clears. If another app already uses the keys, Knob tells you.");
            h4(L"Windows settings");
            linkCard(1, L"Sound settings", L"");
            linkCard(2, L"Display settings", L"");
            h4(L"Knob");
            btnCard(L"Exit Knob", L"", SK_BUTTON, 1, L"Exit");
            break;
        }
        case SP_SOUND: {
            h4(L"Where sound plays");
            snote(L"Drag a sound to a speaker, or a speaker to a sound, to play it there. Click a line to remove it. Apps without a line of their own play with Everything else. Click anything to see where it goes.");
            {   // Timing: one place for sync
                int n = SpeakersInUse();
                bool can = n > 1 && !g_calibrating, ns = NeedsSync();
                std::wstring d = g_calibrating ? (g_calStatus.empty() ? std::wstring(L"Measuring… other apps are muted until it's done") : g_calStatus)
                    : n < 2 ? L"When sound plays on two or more speakers, Auto sync lines them up"
                    : ns ? L"Not synced — a speaker hasn't been measured, so it may play early"
                    : L"In sync · the faster speakers wait for the slowest one";
                float b1 = sbtnW(L"Back to one speaker"), b2 = sbtnW(L"Auto sync", true);
                RectF r; auto draw = cardStart(L"Timing", d.c_str(), b1 + S(10) + b2, -1, r, 52, ns && !g_calibrating && n > 1 ? T.accent : Color(0, 0, 0, 0)); draw(r, card);
                sbtn(SK_BUTTON, 5, RectF(r.X + r.Width - S(18) - b2, r.Y + r.Height / 2 - S(16), b2, S(32)), g_calibrating ? L"Measuring…" : L"Auto sync", true, can);
                sbtn(SK_BUTTON, 7, RectF(r.X + r.Width - S(18) - b2 - S(10) - b1, r.Y + r.Height / 2 - S(16), b1, S(32)), L"Back to one speaker", false, can);
                after(r.Height, S(14));
            }
            {   // the board: sounds on the left (Everything else, apps, mics), speakers on the right; a line = "you sent it there"
                top(0);
                g_rtSrc.clear(); g_rtDst.clear(); g_rtLines.clear();
                const float gap = S(6), colGap = S(56);
                float lw = (cw - colGap) / 2, rw = lw, lx = x0, rx = x0 + lw + colGap;
                float by = y + S(4);
                SpacedText(g, L"SOUND", &fRhd, T.t3, lx + S(4), by + S(7), S(0.66f));
                SpacedText(g, L"SPEAKERS", &fRhd, T.t3, rx + S(4), by + S(7), S(0.66f));
                float ly = by + S(22), ry = ly;
                std::vector<int> srcs;   // RT_EVERY, apps (index), mics (RT_MIC0 - k)
                std::vector<std::pair<float, const wchar_t*>> heads;   // section titles in the left column
                std::vector<float> emptyAt;
                srcs.push_back(RT_EVERY); g_rtSrc[RT_EVERY] = RectF(lx, ly, lw, S(44)); ly += S(44) + gap;
                heads.push_back({ly + S(6), L"APPS"}); ly += S(24);
                int nApps = 0;
                for (int i = 0; i < (int)g_apps.size(); i++) if (!g_apps[i].system) { srcs.push_back(i); g_rtSrc[i] = RectF(lx, ly, lw, S(44)); ly += S(44) + gap; nApps++; }
                if (!nApps) { emptyAt.push_back(ly); ly += S(30); }
                if (!g_in.empty()) { heads.push_back({ly + S(6), L"INPUTS"}); ly += S(24); }
                for (int k = 0; k < (int)g_in.size(); k++) { int id = RT_MIC0 - k; srcs.push_back(id); g_rtSrc[id] = RectF(lx, ly, lw, S(44)); ly += S(44) + gap; }
                for (int i = 0; i < (int)g_out.size(); i++) { g_rtDst[i] = RectF(rx, ry, rw, S(48)); ry += S(48) + gap; }
                for (auto& hd : heads) SpacedText(g, hd.second, &fRhd, T.t3, lx + S(4), hd.first + S(7), S(0.66f));
                for (float ey : emptyAt) Text(g, L"Play something and it shows up here", &fDesc, T.t3, RectF(lx + S(4), ey + S(2), lw, S(18)));
                auto dotR = [&](int sidx) { RectF r = g_rtSrc[sidx]; return PointF(r.X + r.Width, r.Y + r.Height / 2); };
                auto dotL = [&](int i) { RectF r = g_rtDst[i]; return PointF(r.X + S(1), r.Y + r.Height / 2); };
                auto addLine = [&](PointF a, PointF b, int src, int dst) { float m = (b.X - a.X) * 0.5f;
                    RtLine l; l.src = src; l.dst = dst; l.p[0] = a; l.p[1] = PointF(a.X + m, a.Y); l.p[2] = PointF(b.X - m, b.Y); l.p[3] = b; g_rtLines.push_back(l); };
                for (int sidx : srcs)   // only lines you made: Everything else goes to the speakers picked in the Sound tab
                    for (auto& id : RtRoutes(sidx)) for (int i = 0; i < (int)g_out.size(); i++) if (g_out[i].id == id) addLine(dotR(sidx), dotL(i), sidx, i);
                int lit = g_rtDrag != RT_NONE ? g_rtDrag : g_rtSel;
                int litD = g_rtDragDst >= 0 ? g_rtDragDst : g_rtSelDst;
                for (int k = 0; k < (int)g_rtLines.size(); k++) {   // one kind of line; the chosen sound's (or speaker's) lines in the accent colour
                    auto& l = g_rtLines[k];
                    bool on = (lit != RT_NONE && l.src == lit) || (litD >= 0 && l.dst == litD), hov = k == g_rtHoverLine && RtLineRemovable(l);
                    Pen pen(hov ? Color(255, 232, 72, 72) : on ? T.accent : WithAlpha(T.t1, 128), hov || on ? S(2.4f) : S(1.6f));
                    pen.SetStartCap(LineCapRound); pen.SetEndCap(LineCapRound);
                    g.DrawBezier(&pen, l.p[0], l.p[1], l.p[2], l.p[3]);
                }
                if (g_rtDrag != RT_NONE && g_rtSrc.count(g_rtDrag)) {   // the line being dragged from a sound
                    PointF a = dotR(g_rtDrag), b = g_rtMouse; float m = (b.X - a.X) * 0.5f;
                    Pen tp(WithAlpha(T.accent, 230), S(1.6f)); float dash[2] = {3.f, 3.f}; tp.SetDashPattern(dash, 2);
                    g.DrawBezier(&tp, a, PointF(a.X + m, a.Y), PointF(b.X - m, b.Y), b);
                }
                if (g_rtDragDst >= 0 && g_rtDst.count(g_rtDragDst)) {   // ... or from a speaker
                    PointF a = dotL(g_rtDragDst), b = g_rtMouse; float m = (b.X - a.X) * 0.5f;
                    Pen tp(WithAlpha(T.accent, 230), S(1.6f)); float dash[2] = {3.f, 3.f}; tp.SetDashPattern(dash, 2);
                    g.DrawBezier(&tp, a, PointF(a.X + m, a.Y), PointF(b.X - m, b.Y), b);
                }
                for (int sidx : srcs) {
                    RectF r = g_rtSrc[sidx];
                    add(SK_RTSRC, sidx, RectF(r.X, r.Y, r.Width + S(14), r.Height));   // the dot on the edge is part of it
                    auto rt = RtRoutes(sidx);
                    bool hot = g_rtDragDst >= 0 && InRect(RectF(r.X, r.Y, r.Width + S(30), r.Height), g_rtMouse.X, g_rtMouse.Y);
                    bool sel = lit == sidx || hot || (litD >= 0 && litD < (int)g_out.size() && std::find(rt.begin(), rt.end(), g_out[litD].id) != rt.end());
                    FillRound(g, card, r, S(6));
                    if (sel) { Pen sp(T.accent, S(1.5f)); GraphicsPath gp; AddRound(gp, RectF(r.X + S(0.75f), r.Y + S(0.75f), r.Width - S(1.5f), r.Height - S(1.5f)), S(5.5f)); g.DrawPath(&sp, &gp); }
                    RectF ic(r.X + S(12), r.Y + S(9), S(26), S(26));
                    std::wstring name, sub;
                    if (sidx == RT_EVERY) {
                        FillRound(g, T.accent, ic, S(6)); Icon(g, "wave", S(15), T.onAccent, ic.X + S(13), ic.Y + S(13));
                        name = L"Everything else"; sub = L"Apps without their own line, and Windows sounds";
                    } else if (sidx <= RT_MIC0) {
                        Endpoint& m = g_in[RT_MIC0 - sidx];
                        FillRound(g, T.subtle2, ic, S(6)); Icon(g, "mic", S(15), T.t1, ic.X + S(13), ic.Y + S(13));
                        name = OutLabel(m); sub = rt.empty() ? L"Not playing anywhere" : rt.size() == 1 ? L"On " + OutName(rt[0]) : L"On " + std::to_wstring(rt.size()) + L" speakers";
                    } else {
                        AppGroup& a = g_apps[sidx];
                        Gdiplus::Bitmap* icn = a.path.empty() ? nullptr : GetProcInfo(a.path).icon;
                        if (icn) DrawAppIcon(g, icn, RectF(ic.X + S(1), ic.Y + S(1), S(24), S(24)));
                        else {
                            static const DWORD tint[6] = {0xd9532b, 0x5865f2, 0x1a9f48, 0x2a7de1, 0xe07a10, 0x8e44ad};
                            unsigned hsh = 0; for (wchar_t ch : a.name) hsh = hsh * 31 + ch;
                            FillRound(g, Hex(tint[hsh % 6]), ic, S(6)); std::wstring l1 = a.name.substr(0, 1); Text(g, l1.c_str(), g_f11s, Color(255, 255, 255, 255), ic, StringAlignmentCenter);
                        }
                        name = a.name; sub = rt.empty() ? L"With everything else" : rt.size() == 1 ? L"Only " + OutName(rt[0]) : std::to_wstring(rt.size()) + L" speakers";
                    }
                    Text(g, name.c_str(), &fRt, T.t1, RectF(r.X + S(48), r.Y + S(6), r.Width - S(60), S(17)));
                    Text(g, sub.c_str(), g_f11, T.t3, RectF(r.X + S(48), r.Y + S(22), r.Width - S(60), S(15)));
                    PointF d = dotR(sidx);
                    float dr = isHover(SK_RTSRC, sidx) ? S(7.2f) : S(6);
                    SolidBrush ring(winBg); g.FillEllipse(&ring, d.X - dr - S(2), d.Y - dr - S(2), 2 * dr + S(4), 2 * dr + S(4));
                    SolidBrush db(rt.empty() ? T.t3 : T.accent); g.FillEllipse(&db, d.X - dr, d.Y - dr, 2 * dr, 2 * dr);   // grey: nothing of its own yet
                }
                for (int i = 0; i < (int)g_out.size(); i++) {
                    Endpoint& o = g_out[i];
                    RectF r = g_rtDst[i];
                    add(SK_RTDST, i, RectF(r.X - S(14), r.Y, r.Width + S(14), r.Height));   // the dot on the edge is part of it
                    bool hot = g_rtDrag != RT_NONE && InRect(RectF(r.X - S(30), r.Y, r.Width + S(30), r.Height), g_rtMouse.X, g_rtMouse.Y);
                    bool used = std::any_of(g_rtLines.begin(), g_rtLines.end(), [&](const RtLine& l) { return l.dst == i; });
                    bool litDst = litD == i || (lit != RT_NONE && std::any_of(g_rtLines.begin(), g_rtLines.end(), [&](const RtLine& l) { return l.dst == i && l.src == lit; }));
                    bool hv = isHover(SK_RTDST, i) && g_rtDrag == RT_NONE && g_rtDragDst < 0;
                    FillRound(g, hv ? cardHover : card, r, S(6));
                    if (hot || g_rtSelDst == i) { Pen sp(T.accent, S(1.5f)); GraphicsPath gp; AddRound(gp, RectF(r.X + S(0.75f), r.Y + S(0.75f), r.Width - S(1.5f), r.Height - S(1.5f)), S(5.5f)); g.DrawPath(&sp, &gp); }
                    else if (litDst) { Pen sp(WithAlpha(T.accent, 140), S(1)); GraphicsPath gp; AddRound(gp, RectF(r.X + S(0.5f), r.Y + S(0.5f), r.Width - S(1), r.Height - S(1)), S(5.5f)); g.DrawPath(&sp, &gp); }
                    RectF ic(r.X + S(12), r.Y + S(11), S(26), S(26));
                    FillRound(g, T.subtle2, ic, S(6));
                    DrawDevIcon(g, o.icon, ic.X + S(13), ic.Y + S(13), T.t1, 15);
                    float right = S(12);
                    bool pill = used && SpeakersInUse() > 1 && (ShownDelay(o.id) > 0 || g_editDelay == i);   // how long it waits; the slowest shows none
                    if (pill) {
                        wchar_t t[24]; swprintf(t, 24, L"+%d ms", ShownDelay(o.id));
                        float pw = std::max(S(58), S(16) + tw(t, &fPill));
                        RectF pr(r.X + r.Width - S(12) - pw, r.Y + S(13), pw, S(22));
                        right = S(12) + pw + S(10);
                        add(SK_RTDELAY, i, pr);
                        if (g_editDelay == i) { editRect = RectF(pr.X + S(6), pr.Y + S(3), pr.Width - S(12), pr.Height - S(6)); Pen ep(T.accent, S(1.5f)); GraphicsPath gp; AddRound(gp, pr, S(11)); g.DrawPath(&ep, &gp); }
                        FillRound(g, isHover(SK_RTDELAY, i) || g_editDelay == i ? T.subtle3 : T.subtle2, pr, S(11));
                        if (g_editDelay != i) PillText(g, t, &fPill, T.accent, pr, StringAlignmentCenter);
                    }
                    std::wstring sub = o.bt ? std::wstring(L"Bluetooth") : o.detail;
                    Text(g, OutLabel(o).c_str(), &fRt, T.t1, RectF(r.X + S(48), r.Y + S(8), r.Width - S(48) - right, S(17)));
                    Text(g, sub.c_str(), g_f11, T.t3, RectF(r.X + S(48), r.Y + S(24), r.Width - S(48) - right, S(15)));
                    PointF d = dotL(i);
                    float dr = hv ? S(7.2f) : S(6);
                    SolidBrush ring(winBg); g.FillEllipse(&ring, d.X - dr - S(2), d.Y - dr - S(2), 2 * dr + S(4), 2 * dr + S(4));
                    SolidBrush db(used ? T.accent : T.t3); g.FillEllipse(&db, d.X - dr, d.Y - dr, 2 * dr, 2 * dr);
                }
                float bottom = std::max(ly, ry) - gap + S(6);
                after(bottom - y, 0);
                if (!ProcLoopbackOk()) snote(L"This version of Windows can send each app to one speaker only. Windows 11 lets an app play on several.");
            }
            h4(L"More sound options");
            {
                std::vector<std::wstring> opts = {L"Same as main output"}; int cur = 0;
                for (auto& o : g_out) { opts.push_back(o.name); if (!g_callDev.empty() && o.id == g_callDev) cur = (int)opts.size() - 1; }
                ddCard(1, L"Device for calls", L"For apps that follow Windows' call device, like Teams and Skype; everything else keeps the main output", opts, cur, 1);
            }
            switchCard(11, L"Switch to headphones when they connect", L"And back to the previous output when they disconnect", g_autoSwitch, 2);
            switchCard(12, L"Mono audio", L"Both ears hear everything — handy with one earbud", MonoOn(), 3);
            ddCard(2, L"During calls", L"How much Windows turns other sounds down", {L"Mute everything else", L"Reduce by 80%", L"Reduce by 50%", L"Do nothing"}, Ducking(), 4);
            break;
        }
        case SP_DISPLAY: {
            switchCard(3, L"Restore display brightness", L"Put a monitor back to its last brightness and contrast when it reconnects or the PC wakes", g_restoreBright);
            if (std::any_of(g_mons.begin(), g_mons.end(), [](const MonUI& m) { return m.internal && m.rates.size() > 1; }))
                switchCard(26, L"Lower refresh rate on battery", L"The laptop screen drops to 60 Hz on battery to last longer, and goes back to its highest when you plug in", g_rateBat, 9);
            if (ArrReady()) {   // two screens: drawn at their real size, drag the other one where it is on your desk
                h4(L"Arrangement");
                snote(L"Windows places screens by their pixels, so a small sharp laptop screen can look bigger than a large monitor and the edges between them don't line up. Here they're drawn at their real size: drag the other screen to where it is on your desk, then Apply.");
                top(0);
                RectF r(x0, y, cw, S(300)); FillRound(g, card, r, S(6));
                std::vector<const wchar_t*> LV = {L"Real size", L"As Windows has it"};
                Text(g, g_arrPix ? L"How Windows lines them up now" : L"Real size, as on your desk", &fBody, T.t1, RectF(r.X + S(18), r.Y + S(12), r.Width - S(36), S(32)));
                segc(RectF(r.X, r.Y + S(12), r.Width, S(32)), 3, SK_ARRVIEW, LV, g_arrPix ? 1 : 0);
                RectF ma(r.X + S(18), r.Y + S(56), r.Width - S(36), r.Height - S(56) - S(60));
                const MonUI& Mm = g_mons[g_arrMain.mon]; const MonUI& Mo = g_mons[g_arrOther.mon];
                float mw, mh, ow, oh, ox, oy;   // the other screen's box relative to the main one (mm, or pixels in Windows' view)
                if (g_arrPix) { mw = (float)(Mm.rc.right - Mm.rc.left); mh = (float)(Mm.rc.bottom - Mm.rc.top); ow = (float)(Mo.rc.right - Mo.rc.left); oh = (float)(Mo.rc.bottom - Mo.rc.top); ox = (float)(Mo.rc.left - Mm.rc.left); oy = (float)(Mo.rc.top - Mm.rc.top); }
                else {
                    mw = g_arrMain.wmm; mh = g_arrMain.hmm; ow = g_arrOther.wmm; oh = g_arrOther.hmm;
                    ox = g_arrSide == 1 ? mw : g_arrSide == 3 ? -ow : mw / 2 + g_arrOff - ow / 2;
                    oy = g_arrSide == 0 ? -oh : g_arrSide == 2 ? mh : mh / 2 + g_arrOff - oh / 2;
                }
                float bx0 = std::min(0.f, ox), by0 = std::min(0.f, oy), bx1 = std::max(mw, ox + ow), by1 = std::max(mh, oy + oh);
                float k = std::min(ma.Width / (bx1 - bx0), ma.Height / (by1 - by0)) * 0.92f;
                float cx = ma.X + (ma.Width - (bx1 - bx0) * k) / 2 - bx0 * k, cy = ma.Y + (ma.Height - (by1 - by0) * k) / 2 - by0 * k;
                g_arrScale = k;
                g_arrRMain = RectF(cx, cy, mw * k, mh * k); g_arrROther = RectF(cx + ox * k, cy + oy * k, ow * k, oh * k);
                auto screen = [&](RectF sr, int num, const MonUI& mon, const ArrScreen& as, bool other) {
                    RectF in(sr.X + S(1), sr.Y + S(1), sr.Width - S(2), sr.Height - S(2));
                    FillRound(g, other && (g_arrDrag || isHover(SK_ARR, 0)) ? T.subtle3 : T.subtle2, in, S(5));
                    Pen pn(other && g_arrDrag ? T.accent : WithAlpha(T.t1, 90), S(other && g_arrDrag ? 1.5f : 1)); GraphicsPath gp; AddRound(gp, in, S(5)); g.DrawPath(&pn, &gp);
                    wchar_t nb[4]; swprintf(nb, 4, L"%d", num); Text(g, nb, &fTitle, T.t1, RectF(in.X, in.Y + in.Height / 2 - S(26), in.Width, S(30)), StringAlignmentCenter);
                    wchar_t sub[96]; swprintf(sub, 96, L"%ls \u00B7 %.0f\u2033", mon.name.c_str(), hypotf(as.wmm, as.hmm) / 25.4f);
                    if (tw(sub, &fDesc) > in.Width - S(12)) swprintf(sub, 96, L"%.0f\u2033", hypotf(as.wmm, as.hmm) / 25.4f);   // small on the map: just its size
                    if (in.Height > S(54)) Text(g, sub, &fDesc, T.t3, RectF(in.X + S(4), in.Y + in.Height / 2 + S(4), in.Width - S(8), S(16)), StringAlignmentCenter);
                    if (other && !g_arrPix) add(SK_ARR, 0, sr);
                };
                screen(g_arrRMain, g_arrMain.mon + 1, Mm, g_arrMain, false);
                screen(g_arrROther, g_arrOther.mon + 1, Mo, g_arrOther, true);
                float by = r.Y + r.Height - S(18) - S(32);
                float aw = sbtnW(L"Apply", true), cw2 = sbtnW(L"Centre it");
                sbtn(SK_BUTTON, 32, RectF(r.X + r.Width - S(18) - aw, by, aw, S(32)), L"Apply", true, g_arrDirty);
                sbtn(SK_BUTTON, 31, RectF(r.X + r.Width - S(18) - aw - S(10) - cw2, by, cw2, S(32)), L"Centre it", false, !g_arrPix);
                Text(g, g_arrMsg.c_str(), &fDesc, T.t3, RectF(r.X + S(18), by, r.Width - S(36) - aw - cw2 - S(20), S(32)));
                after(r.Height, S(4));
            }
            h4(L"Colour");
            snote(L"Per screen. Night light adds a soft orange tint that's easier on the eyes; set how strong it is here, and switch it on from the moon button in the Display tab or by schedule.");
            for (int i = 0; i < (int)g_mons.size(); i++) {
                MonUI& m = g_mons[i];
                if (m.cloneOf >= 0) continue;   // a duplicate shares its source's colour
                top(0);
                RectF r(x0, y, cw, S(124)); FillRound(g, card, r, S(6));
                float ix = r.X + S(18), iw = r.Width - S(36), yy = r.Y + S(10);
                {   // number, name, connection
                    RectF nb(ix, yy, S(22), S(22)); FillRound(g, T.subtle2, nb, S(6));
                    { GraphicsPath gp; AddRound(gp, RectF(nb.X + S(0.75f), nb.Y + S(0.75f), nb.Width - S(1.5f), nb.Height - S(1.5f)), S(5.25f)); Pen pp(T.t1, S(1.5f)); g.DrawPath(&pp, &gp); }
                    wchar_t num[4]; swprintf(num, 4, L"%d", i + 1); Text(g, num, &fNum11, T.t1, nb, StringAlignmentCenter);
                    float nw = std::min(iw - S(140), tw(m.name.c_str(), &fBold14));
                    Text(g, m.name.c_str(), &fBold14, T.t1, RectF(ix + S(32), yy, nw + S(4), S(22)));
                    Text(g, m.detail.c_str(), &fSmall115, T.t3, RectF(ix + S(32) + nw + S(10), yy + S(1), iw - nw - S(42), S(22)));
                }
                yy += S(22) + S(8);
                {   // HDR (only screens that can)
                    RectF hr(ix, yy, iw, S(40));
                    Text(g, L"HDR", &fBody13, T.t1, RectF(hr.X, hr.Y, S(90), hr.Height));
                    if (m.hdrOk) {
                        add(SK_SWITCH, 100 + i, hr);
                        Text(g, m.hdr ? L"Brighter highlights, for HDR games and video" : L"Off", &fSmall115, T.t3, RectF(hr.X + S(100), hr.Y, hr.Width - S(100) - S(85), hr.Height));
                        DrawSwitch(g, hr.X + hr.Width - S(40), hr.Y + hr.Height / 2, m.hdr);
                        Text(g, m.hdr ? L"On" : L"Off", &fBody13, T.t1, RectF(hr.X + hr.Width - S(40) - S(16) - S(60), hr.Y, S(60), hr.Height), StringAlignmentFar);
                    } else Text(g, L"This screen doesn't support HDR", &fSmall115, T.t3, RectF(hr.X + S(100), hr.Y, hr.Width - S(100), hr.Height));
                }
                yy += S(40) + S(2);
                {   // night light strength
                    int str = g_night.count(m.name) ? g_night[m.name].str : 40;
                    Text(g, L"Night light", &fDesc, T.t2, RectF(ix, yy, S(80), S(32)));
                    RectF sl(ix + S(80), yy, iw - S(80) - S(78), S(32));
                    add(SK_NLSL, i, RectF(sl.X - S(10), sl.Y, sl.Width + S(20), sl.Height));
                    DrawSlider(g, sl, str / 100.f, false, g_sDragNl == i ? 2 : (isHover(SK_NLSL, i) ? 1 : 0), true);
                    wchar_t t[8]; swprintf(t, 8, L"%d", str);
                    Text(g, t, &fDesc600, T.t1, RectF(ix + iw - S(56), yy, S(56), S(32)), StringAlignmentFar);
                }
                after(r.Height, S(4));
            }
            ddCard(5, L"Turn night light on automatically", L"Screens with night light switched on follow this", {L"Never — only with the moon button", L"Evenings (7 pm – 7 am)", L"Sunset to sunrise"}, g_nightAuto, 5);
            {   // Screen off and Party screen
                h4(L"Screen off and Party screen");
                snote(L"What your screens show when you're not using them.");
                auto names = ScreenNames();
                ddCard(16, L"Turn off displays", L"The Turn off button and its shortcut always switch your screens off right away. This also turns them off by themselves when you haven't used the keyboard or mouse for a while (not during full-screen video or games)", {L"Only with the button", L"After 15 minutes of no activity", L"After 30 minutes of no activity", L"After 1 hour of no activity", L"After 2 hours of no activity"}, g_offDelay, 6);
                switchCard(29, L"Switch monitors fully off", L"Also sends each monitor its own power-down command (DDC/CI), so even an LCD's backlight goes dark. Any key or mouse move turns them back on; if one stays dark, press its power button. Some DisplayPort monitors disconnect while off, which can move your windows. Laptop screens go to their lowest brightness", g_offFull, 14);
                switchCard(27, L"Clock on the black screen", L"The time and date in dim grey, like a phone's always-on screen. It drifts a little every minute so OLED screens don't burn in. Best on OLED; other screens keep their backlight on", g_offClock, 7);
                if (g_offClock) {
                    std::vector<std::wstring> o = {L"Every screen", L"Main screen"}; int cur = g_clockScr == L"*" ? 0 : 1;
                    for (auto& n : names) { o.push_back(n); if (n == g_clockScr) cur = (int)o.size() - 1; }
                    ddCard(17, L"Show the clock on", L"The other screens just go black", o, cur, -1);
                }
                {
                    std::vector<std::wstring> o = {L"Main screen"}; int cur = 0;
                    for (auto& n : names) { o.push_back(n); if (n == g_partyScr) cur = (int)o.size() - 1; }
                    ddCard(18, L"Party screen shows on", L"", o, cur, -1);
                }
                ddCard(31, L"End the Party screen with", L"Esc only keeps it going when the mouse moves or other keys are pressed", {L"Any key or mouse move", L"Esc only"}, g_partyClose, -1);
                ddCard(19, L"Look", L"All of them follow the music and use the cover's colours", {PARTY_LOOKS[0], PARTY_LOOKS[1], PARTY_LOOKS[2]}, g_partyLook, -1);
                switchCard(28, L"Show what's playing", L"The cover, title and artist, small in a corner", g_partyInfo);
            }
            h4(L"Profiles");
            snote(L"Save the brightness and contrast of every display, then switch with one click from the Display tab.");
            for (int i = 0; i < (int)g_prof.size(); i++) {
                bool active = ProfileActive(i);
                std::wstring sub = g_prof[i].v.empty() ? L"Empty — Save current to fill it" : std::to_wstring(g_prof[i].v.size()) + (g_prof[i].v.size() == 1 ? L" display saved" : L" displays saved");
                if (active) sub += L" · active";
                float aw = sbtnW(L"Apply"), sw2 = sbtnW(L"Save current"), rightW = aw + S(10) + sw2 + S(10) + S(32) + S(10) + S(32);
                RectF r; auto draw = cardStart(g_editProfile == i ? L" " : g_prof[i].name.c_str(), sub.c_str(), rightW, -1, r, 52, active ? T.accent : Color(0, 0, 0, 0)); draw(r, card);
                if (g_editProfile == i) editRect = RectF(r.X + S(14), r.Y + r.Height / 2 - S(20), cw - S(36) - rightW - S(4), S(22));
                float bx = r.X + r.Width - S(18), cy = r.Y + r.Height / 2 - S(16);
                bx -= S(32); sib(SK_PDELETE, i, RectF(bx, cy, S(32), S(32)), "trash");
                bx -= S(10) + S(32); sib(SK_PRENAME, i, RectF(bx, cy, S(32), S(32)), "pencil");
                bx -= S(10) + sw2; sbtn(SK_PUPDATE, i, RectF(bx, cy, sw2, S(32)), L"Save current");
                bx -= S(10) + aw; sbtn(SK_PAPPLY, i, RectF(bx, cy, aw, S(32)), L"Apply", false, !g_prof[i].v.empty());
                cardEnd(r);
            }
            {
                bool can = (int)g_prof.size() < MAX_PROFILES;
                btnCard(can ? L"New profile from the current settings" : L"You can keep up to 8 profiles", L"", SK_PNEW, 0, L"+ New profile", true, can);
            }
            break;
        }
        case SP_FLYOUTS: {
            snote(L"Small windows that appear by themselves, never take focus and stay hidden while a game or video runs full screen.");
            switchCard(20, L"Media", L"When you press a media key: what's playing, its player, the controls and where you are in the song", g_popMedia, 11);
            switchCard(21, L"Up next", L"When the next song starts on its own. Pressing next or previous shows Media instead", g_popNext, 12);
            switchCard(22, L"Lock keys", L"When Caps Lock, Num Lock, Scroll Lock or Insert is pressed", g_popLock, 13);
            {   // try them
                float b1 = sbtnW(L"Media"), b2 = sbtnW(L"Song ends"), b3 = sbtnW(L"Lock keys");
                RectF r; auto draw = cardStart(L"Try them", L"Pressing the keys works too", b1 + b2 + b3 + S(20), -1, r); draw(r, card);
                float bx = r.X + r.Width - S(18), cy = r.Y + r.Height / 2 - S(16);
                bx -= b3; sbtn(SK_BUTTON, 22, RectF(bx, cy, b3, S(32)), L"Lock keys");
                bx -= S(10) + b2; sbtn(SK_BUTTON, 21, RectF(bx, cy, b2, S(32)), L"Song ends");
                bx -= S(10) + b1; sbtn(SK_BUTTON, 20, RectF(bx, cy, b1, S(32)), L"Media");
                cardEnd(r);
            }
            h4(L"Look and place");
            ddCard(10, L"Position", L"", {L"Bottom centre", L"Bottom left", L"Bottom right", L"Top centre", L"Top left", L"Top right"}, g_fPos, -1);
            ddCard(11, L"Screen", L"", {L"Main screen", L"Screen with the mouse", L"Screen with the active window"}, g_fScreen, -1);
            msCard(0, L"Stay on screen", L"In milliseconds, longer while the mouse is on them. 0 keeps the media flyout until you close it. Lock keys have their own time below", g_fStayMs);
            msCard(1, L"Animation", L"How long they take to slide in and out, in milliseconds. 0 for no motion", g_fAnimMs);
            switchCard(23, L"Colours from the cover", L"Buttons and the progress bar take the cover's colour; off uses Knob's accent colour", g_fCover);
            snote(L"Solid or Glass and light or dark follow Settings \u203A Appearance.");
            h4(L"Media");
            ddCard(14, L"Layout", L"Compact is one slim row: cover, title and play controls", {L"Standard", L"Compact"}, g_fLayout, -1);
            ddCard(15, L"Cover glow", L"The cover's colours behind the flyout. The moving ones only move while the flyout is on screen", {L"Off", L"Rising glow", L"Blurred cover", L"Aurora", L"Edge light", L"Pulse with the music"}, g_fGlow, -1);
            if (g_fLayout == 0) {
                switchCard(24, L"Centre the title and artist", L"", g_fCentre);
                switchCard(25, L"Show the player", L"Its name, and a list to switch when more than one is playing", g_fPlayer);
            }
            h4(L"Lock keys");
            {   // which keys show a flyout: one chip each
                static const wchar_t* kn[4] = {L"Caps Lock", L"Num Lock", L"Scroll Lock", L"Insert"};
                std::vector<float> ws; float total = 0; for (auto* k : kn) { ws.push_back(S(24) + tw(k, &fDesc)); total += ws.back(); }
                total += S(6) * 3;
                RectF r; auto draw = cardStart(L"Keys", L"Which keys show a flyout", total, -1, r); draw(r, card);
                float bx = r.X + r.Width - S(18) - total, cy = r.Y + r.Height / 2 - S(14);
                for (int i = 0; i < 4; i++) {
                    RectF c(bx, cy, ws[i], S(28)); add(SK_CHIP, i, c);
                    bool on = (g_lkKeys >> i) & 1, hv = isHover(SK_CHIP, i);
                    FillRound(g, on ? WithAlpha(T.accent, hv ? 70 : 48) : (hv ? T.subtle3 : T.subtle2), c, S(14));
                    Text(g, kn[i], on ? &fDesc600 : &fDesc, on ? T.t1 : T.t2, c, StringAlignmentCenter);
                    bx += ws[i] + S(6);
                }
                cardEnd(r);
            }
            msCard(2, L"Stay on screen", L"How long the lock-key flyout stays, in milliseconds, apart from the other flyouts", g_lkStayMs);
            break;
        }
        case SP_APPEAR: {
            {   // Theme: follow Windows, or force light / dark
                std::vector<const wchar_t*> L3 = {L"System", L"Light", L"Dark"};
                RectF r; auto draw = cardStart(L"Theme", L"Follow Windows, or always light or dark", segW(L3), -1, r); draw(r, card);
                segc(r, 0, SK_THEME, L3, g_themeMode); cardEnd(r);
            }
            {
                std::vector<const wchar_t*> L2 = {L"Solid", L"Glass"};
                RectF r; auto draw = cardStart(L"Style", L"Glass needs Windows 11 22H2 or newer; older versions use Solid", segW(L2), -1, r); draw(r, card);
                segc(r, 1, SK_STYLE, L2, g_style); cardEnd(r);
            }
            {   // accent: Windows, or your own colour
                std::vector<const wchar_t*> L2 = {L"Windows", L"Custom"};
                const wchar_t* ad = g_accentId ? L"Pick any colour below" : L"Follows your Windows accent colour";
                RectF r; auto draw = cardStart(L"Accent color", ad, segW(L2), -1, r); draw(r, card);
                segc(r, 2, SK_ACCMODE, L2, g_accentId); cardEnd(r);
            }
            if (g_accentId) {
                top(0);
                RectF r(x0, y, cw, S(120)); FillRound(g, card, r, S(6));
                Color cur = CustomAccent(T.dark, g_accentHue, g_accentSat);
                wchar_t hx[16]; swprintf(hx, 16, L"#%02X%02X%02X", cur.GetR(), cur.GetG(), cur.GetB());
                float chw = S(24) + tw(hx, &fPill);
                RectF chip(r.X + r.Width - S(18) - chw, r.Y + S(10), chw, S(26));
                FillRound(g, cur, chip, S(13));
                Text(g, hx, &fPill, Color(255, 255, 255, 255), chip, StringAlignmentCenter);
                Text(g, L"Your colour", &fBody, T.t1, RectF(r.X + S(18), r.Y + S(10), S(200), S(26)));
                auto track = [&](int id, const wchar_t* label, float yy, float v) {
                    Text(g, label, &fDesc, T.t2, RectF(r.X + S(18), yy - S(8), S(70), S(16)));
                    RectF t(r.X + S(100), yy - S(5), r.Width - S(100) - S(26), S(10));
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
                    SolidBrush sh(Color(115, 0, 0, 0)); g.FillEllipse(&sh, fx - R, yy - R + S(1), 2 * R, 2 * R);
                    SolidBrush wb(Color(255, 255, 255, 255)); g.FillEllipse(&wb, fx - R, yy - R, 2 * R, 2 * R);
                    SolidBrush cb(cur); g.FillEllipse(&cb, fx - S(5), yy - S(5), S(10), S(10));
                };
                track(0, L"Hue", r.Y + S(65.5f), g_accentHue / 359.f);
                track(1, L"Intensity", r.Y + S(102.5f), g_accentSat / 100.f);
                after(r.Height, S(4));
            }
            break;
        }
        case SP_ABOUT: {
            {   // name, version, what it is
                top(0);
                RectF r(x0, y, cw, S(71)); FillRound(g, card, r, S(6));
                Text(g, APP_NAME L" " APP_VERSION, &fBody, T.t1, RectF(r.X + S(17), r.Y + S(10), r.Width - S(36), S(17)));
                Text(g, L"A tiny tray app for volume, apps, audio outputs and display brightness.", &fDesc, T.t3, RectF(r.X + S(17), r.Y + S(29), r.Width - S(36), S(15)));
                Text(g, L"No install, no background services — only built-in Windows parts.", &fDesc, T.t3, RectF(r.X + S(17), r.Y + S(46), r.Width - S(36), S(15)));
                after(r.Height, S(4));
            }
            {
                std::wstring st = g_updStatus.empty() ? L"Checks github.com/" UPDATE_REPO L" every 6 hours" : g_updStatus;
                if (g_updState == UPD_FAILED) btnCard(L"Updates", st.c_str(), SK_BUTTON, 6, L"Download in browser", true);
                else if (g_updState == UPD_AVAILABLE) btnCard(L"Updates", st.c_str(), SK_BUTTON, 4, (L"Update to " + g_updVersion).c_str(), true);
                else btnCard(L"Updates", st.c_str(), SK_BUTTON, 3, g_updState == UPD_DOWNLOADING ? L"Updating…" : L"Check now", false, g_updState != UPD_DOWNLOADING);
            }
            linkCard(3, L"Open the settings folder", L"settings.ini lives in %APPDATA%\\Knob");
            btnCard(L"Reset all settings", L"Back to defaults: style, accent, shortcut, profiles, delays and remembered brightness", SK_BUTTON, 2, L"Reset settings");
            break;
        }
        }
        if (ddPopId >= 0) {   // open dropdown list, on top of everything
            float ih = S(32), full = ih * ddPopOpts.size() + S(8);
            static int lastPop = -1; if (lastPop != ddPopId) { lastPop = ddPopId; g_sddScroll = 0; }
            float ph = std::min(full, g_setH - S(16));
            g_sddMax = full - ph; g_sddScroll = std::max(0.f, std::min(g_sddScroll, g_sddMax));
            float sc = g_sddScroll;
            float pw = ddPopBtn.Width; for (auto& o : ddPopOpts) pw = std::max(pw, S(28) + tw(o.c_str(), &fDesc));
            pw = std::min(pw, cw);
            RectF pop(std::min(ddPopBtn.X, x0 + cw - pw), ddPopBtn.Y + ddPopBtn.Height + S(4), pw, ph);
            if (pop.Y + pop.Height > g_setH - S(8)) pop.Y = ddPopBtn.Y - S(4) - ph;   // no room below: open upwards
            if (pop.Y < S(8)) pop.Y = std::max(S(8), g_setH - S(8) - ph);              // no room either way: as much of the window as it needs
            g_sddRect = pop;
            for (int i = 0; i < (int)ddPopOpts.size(); i++) {
                RectF it(pop.X + S(4), pop.Y + S(4) + i * ih - sc, pop.Width - S(8), ih);
                if (it.Y + it.Height > pop.Y && it.Y < pop.Y + pop.Height) add(SK_DDOPT, ddPopId * 100 + i, it);   // only rows you can see are clickable
            }
            float pt = PopT(g_paSetDD, ddPopId);
            DrawPopAnimated(g, g_paSetDD, pop, pt, pop.Y < ddPopBtn.Y, [&](Graphics& gg) {
                FillRound(gg, Color(70, 0, 0, 0), RectF(pop.X - S(1), pop.Y + S(3), pop.Width + S(2), pop.Height + S(2)), S(9));
                FillRound(gg, T.dark ? Color(255, 44, 44, 44) : Color(255, 251, 251, 251), pop, S(8));
                Region keep; gg.GetClip(&keep);
                gg.IntersectClip(RectF(pop.X, pop.Y + S(2), pop.Width, pop.Height - S(4)));
                for (int i = 0; i < (int)ddPopOpts.size(); i++) {
                    RectF it(pop.X + S(4), pop.Y + S(4) + i * ih - sc, pop.Width - S(8), ih);
                    if (i == ddPopCur) { FillRound(gg, T.subtle, it, S(5)); FillRound(gg, T.accent, RectF(it.X, it.Y + S(9), S(3), S(14)), S(1.5f)); }
                    else if (isHover(SK_DDOPT, ddPopId * 100 + i)) FillRound(gg, T.subtle2, it, S(5));
                    Text(gg, ddPopOpts[i].c_str(), &fDesc, T.t1, RectF(it.X + S(12), it.Y, it.Width - S(16), it.Height));
                }
                gg.SetClip(&keep);
                if (g_sddMax > 0) {   // thin bar showing where you are in the list
                    float th = std::max(S(24), pop.Height * pop.Height / full), ty = pop.Y + S(4) + (pop.Height - S(8) - th) * (sc / g_sddMax);
                    FillRound(gg, T.t3, RectF(pop.X + pop.Width - S(5), ty, S(3), th), S(1.5f));
                }
            });
        } else ClosePop(g, g_paSetDD);
        g_setContentH = y + g_setScroll + S(22);
        g.ResetClip();
        float view = (float)g_setH;
        if (g_setContentH > view + 1) {
            float th = std::max(S(40), view * view / g_setContentH);
            float ty = S(4) + (view - th - S(8)) * (g_setScroll / (g_setContentH - view));
            FillRound(g, T.t3, RectF(g_setW - S(7), ty, S(3), th), S(1.5f));
        }
    }
    FadeApply(g_fadeSet, bits, g_setW, g_setH);
    BitBlt(hdc, 0, 0, g_setW, g_setH, mdc, 0, 0, SRCCOPY);
    CaptionStep(g_fadeSet.t);
    SelectObject(mdc, old);
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
static void SettingsNlAt(float x) {   // night light strength slider in Settings › Display
    for (auto& it : g_sItems) if (it.kind == SK_NLSL && it.id == g_sDragNl && g_sDragNl < (int)g_mons.size()) {
        RectF sl(it.r.X + S(10), it.r.Y, it.r.Width - S(20), it.r.Height);
        g_night[g_mons[g_sDragNl].name].str = (int)lround(std::max(0.f, std::min(1.f, (x - sl.X) / sl.Width)) * 100);
        ApplyNight(); SettingsChanged();
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
    case WM_TIMER:
        if (wp == 77) { KillTimer(h, 77); InvalidateRect(h, nullptr, FALSE); return 0; }   // sliding pill frame
        if (wp == 78) {   // Settings › Sound: the apps playing right now, without opening the Apps tab
            if (g_setPage != SP_SOUND || !IsWindowVisible(h) || g_rtDrag != RT_NONE || g_rtDragDst >= 0 || g_dragEl >= 0 || g_ddOpen >= 0) return 0;   // never while something holds an app's place in the list
            static int tick = 0;
            if (++tick % 4 == 0 || g_apps.empty()) {
                std::wstring selKey = g_rtSel >= 0 && g_rtSel < (int)g_apps.size() ? g_apps[g_rtSel].key : L"";
                LoadApps();
                if (g_rtSel >= 0) { g_rtSel = RT_NONE; for (int i = 0; i < (int)g_apps.size(); i++) if (!selKey.empty() && g_apps[i].key == selKey) g_rtSel = i; }
            }
            UpdatePeaks();
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        break;
    case WM_SIZE:
        if (wp == SIZE_MINIMIZED) { PairHide(); return 0; }
        g_setW = LOWORD(lp); g_setH = HIWORD(lp); SetPageScroll(g_setScroll); InvalidateRect(h, nullptr, FALSE); return 0;
    case WM_ACTIVATE:   // the panel follows Settings: back when Settings comes forward, away when another app takes over
        if (LOWORD(wp) != WA_INACTIVE) PairShow(false);
        else if ((HWND)lp != g_hwnd) PairBack();
        break;
    case WM_PAINT: { PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps); PaintSettings(dc); EndPaint(h, &ps); return 0; }
    case WM_ERASEBKGND: return 1;
    case WM_MOUSEMOVE: {
        float x = (float)GET_X_LPARAM(lp), y = (float)GET_Y_LPARAM(lp);
        if (g_sDragDelay >= 0) { SettingsDelayAt(x); return 0; }
        if (g_sDragColor) { SettingsColorAt(x); return 0; }
        if (g_sDragNl >= 0) { SettingsNlAt(x); return 0; }
        if (g_arrDrag) {   // the other screen follows the mouse, snapping to the main screen's nearest side
            float cx = g_arrGrabC.X + (x - g_arrGrab.X), cy = g_arrGrabC.Y + (y - g_arrGrab.Y);
            float mx = (cx - (g_arrRMain.X + g_arrRMain.Width / 2)) / g_arrScale, my = (cy - (g_arrRMain.Y + g_arrRMain.Height / 2)) / g_arrScale;
            float hx = (g_arrMain.wmm + g_arrOther.wmm) / 2, hy = (g_arrMain.hmm + g_arrOther.hmm) / 2;
            if (fabsf(my) / hy > fabsf(mx) / hx) { g_arrSide = my < 0 ? 0 : 2; g_arrOff = mx; } else { g_arrSide = mx > 0 ? 1 : 3; g_arrOff = my; }
            if (fabsf(g_arrOff) < 15) g_arrOff = 0;   // centred, give or take 1.5 cm
            ArrClamp(); g_arrDirty = true; g_arrMsg.clear();
            InvalidateRect(h, nullptr, FALSE); return 0;
        }
        if (g_rtDrag != RT_NONE || g_rtDragDst >= 0) { g_rtMouse = PointF(x, y); InvalidateRect(h, nullptr, FALSE); return 0; }
        if (g_setPage == SP_SOUND) { int ln = SettingsHit(x, y) < 0 ? RtLineAt(x, y) : -1; if (ln != g_rtHoverLine) { g_rtHoverLine = ln; InvalidateRect(h, nullptr, FALSE); } }
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
        if (g_edit && (hi < 0 || (g_sItems[hi].kind != SK_PRENAME && g_sItems[hi].kind != SK_RTDELAY && g_sItems[hi].kind != SK_MS))) CommitRename(true);
        if (hi < 0 && g_setPage == SP_SOUND && g_sddOpen < 0) {   // click a line: that sound stops playing there
            int ln = RtLineAt(x, y);
            if (ln >= 0) {
                RtLine l = g_rtLines[ln];
                if (!RtLineRemovable(l)) { g_rtHoverLine = -1; return 0; }   // Everything else keeps its last speaker
                RtDisconnect(l);
                g_rtHoverLine = -1; SettingsChanged(); return 0;
            }
            if (g_rtSel != RT_NONE || g_rtSelDst >= 0) { g_rtSel = RT_NONE; g_rtSelDst = -1; InvalidateRect(h, nullptr, FALSE); }
        }
        if (g_sddOpen >= 0 && (hi < 0 || (g_sItems[hi].kind != SK_DDBTN && g_sItems[hi].kind != SK_DDOPT && g_sItems[hi].kind != SK_HKADD))) {   // click outside an open list closes it
            g_sddOpen = -1; InvalidateRect(h, nullptr, FALSE); return 0;
        }
        if (hi < 0) { InvalidateRect(h, nullptr, FALSE); return 0; }
        SItem it = g_sItems[hi];
        switch (it.kind) {
        case SK_NAV: g_setPage = it.id; g_setScroll = 0; g_sddOpen = -1; if (it.id == SP_SOUND) { LoadApps(); UpdatePeaks(); } break;
        case SK_THEME:
            if (g_themeMode == it.id) break;
            StartFade();
            g_themeMode = it.id; SaveSettings();
            ApplyStyle(); BuildSettingsTheme(); ApplySettingsStyle(); UpdateTrayIcon(); SettingsChanged();
            break;
        case SK_DDBTN: {
            static const int newBit[6] = {-1, 1, 4, 0, -1, 5};   // dropdown id -> its "NEW" tag
            if (it.id >= 1 && it.id <= 5 && newBit[it.id] >= 0 && !(g_newSeen & (1 << newBit[it.id]))) { g_newSeen |= 1 << newBit[it.id]; SaveSettings(); }
            g_sddOpen = g_sddOpen == it.id ? -1 : it.id; InvalidateRect(h, nullptr, FALSE); return 0;
        }
        case SK_DDOPT: {
            int dd = it.id / 100, k = it.id % 100;
            if (dd == 1) { std::wstring id; if (k > 0 && k - 1 < (int)g_out.size()) id = g_out[k - 1].id; SetCallDevice(id); SaveSettings(); }
            if (dd == 2) AudioRegSet(L"UserDuckingPreference", (DWORD)k);
            if (dd == 3) { g_openOn = k ? 1 : 0; SaveSettings(); }
            if (dd == 4 && k < (int)g_hkAddMap.size()) {   // new shortcut: a row, then wait for its keys
                int a = g_hkAddMap[k]; Shortcut s2{a, L"", {0, 0, false}};
                if (HkParamKind(a) == 1) { int di = DefaultOut(); if (!g_out.empty()) s2.param = OutLabel(g_out[di >= 0 ? di : 0]); }
                if (HkParamKind(a) == 2 && !g_prof.empty()) s2.param = g_prof[0].name;
                g_sc.push_back(s2); g_capture = (int)g_sc.size() - 1; UnregisterHotkeys(); SaveSettings();
            }
            if (dd == 5) { g_nightAuto = std::min(2, k); SaveSettings(); GeoRefresh(); ApplyNight(); }
            if (dd >= 10 && dd <= 15 && dd != 12 && dd != 13) { int* v[6] = {&g_fPos, &g_fScreen, nullptr, nullptr, &g_fLayout, &g_fGlow}; *v[dd - 10] = k; if (g_popShown) PopEnd(); SaveSettings(); }
            if (dd == 16) { g_offDelay = std::max(0, std::min(4, k)); g_newSeen |= 1 << 6; SaveSettings(); }
            if (dd == 17 || dd == 18) {
                auto names = ScreenNames(); std::wstring v;
                if (dd == 17) v = k == 0 ? L"*" : k == 1 ? L"" : (k - 2 < (int)names.size() ? names[k - 2] : L"");
                else v = k >= 1 && k - 1 < (int)names.size() ? names[k - 1] : L"";
                (dd == 17 ? g_clockScr : g_partyScr) = v; SaveSettings();
            }
            if (dd == 31) { g_partyClose = std::max(0, std::min(1, k)); SaveSettings(); }
            if (dd == 19) { g_partyLook = std::max(0, std::min(2, k)); SaveSettings(); }
            if (dd >= 1000 && dd - 1000 < (int)g_sc.size()) {   // which speaker / profile a shortcut uses
                Shortcut& s2 = g_sc[dd - 1000];
                if (HkParamKind(s2.act) == 1 && k < (int)g_out.size()) s2.param = OutLabel(g_out[k]);
                if (HkParamKind(s2.act) == 2 && k < (int)g_prof.size()) s2.param = g_prof[k].name;
                SaveSettings();
            }
            g_sddOpen = -1; InvalidateRect(h, nullptr, FALSE); return 0;
        }
        case SK_SWITCH:
            if (it.id == 11) { g_autoSwitch = !g_autoSwitch; g_newSeen |= 1 << 2; }
            else if (it.id == 12) { AudioRegSet(L"AccessibilityMonoMixState", MonoOn() ? 0 : 1); g_newSeen |= 1 << 3; }
            else if (it.id >= 100 && it.id - 100 < (int)g_mons.size()) { int mi = it.id - 100; g_dispStatus = SetHdr(mi, !g_mons[mi].hdr) ? L"" : L"Windows didn't change HDR for this screen"; BrightRequest(true, false); }
            else if (it.id == 1) { g_keepAwake = !g_keepAwake; ApplyKeepAwake(); }
            else if (it.id == 2) SetStartup(!StartupEnabled());
            else if (it.id == 3) g_restoreBright = !g_restoreBright;
            else if (it.id == 20) { g_popMedia = !g_popMedia; g_newSeen |= 1 << 11; FlyoutKeysSync(); }
            else if (it.id == 21) { g_popNext = !g_popNext; g_newSeen |= 1 << 12; FlyoutKeysSync(); }
            else if (it.id == 22) { g_popLock = !g_popLock; g_newSeen |= 1 << 13; FlyoutKeysSync(); }
            else if (it.id == 23) { g_fCover = !g_fCover; BuildGlows(); }
            else if (it.id == 24) g_fCentre = !g_fCentre;
            else if (it.id == 25) g_fPlayer = !g_fPlayer;
            else if (it.id == 26) { g_rateBat = !g_rateBat; g_newSeen |= 1 << 9; RateBatApply(); }
            else if (it.id == 27) { g_offClock = !g_offClock; g_newSeen |= 1 << 7; }
            else if (it.id == 28) g_partyInfo = !g_partyInfo;
            else if (it.id == 29) { g_offFull = !g_offFull; g_newSeen |= 1 << 14; }
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
            if (it.id == 5) { g_calAsk = false; g_calShow = true; StartAutoSync(); }
            if (it.id == 7) { OneSpeaker(); g_calStatus.clear(); }
            if (it.id == 6) ShellExecuteW(nullptr, L"open", L"https://github.com/" UPDATE_REPO L"/raw/main/dist/Knob.exe", nullptr, nullptr, SW_SHOWNORMAL);
            if (it.id == 4) StartUpdate();
            if (it.id == 20 || it.id == 21) { MediaStart(); g_popTry = true; if (g_popShown) PopEnd(); g_popTry = true; PopShow(it.id == 20 ? POP_MEDIA : POP_NEXT); }
            if (it.id == 22) {   // the next lock key that's switched on, alternating on and off
                static int tk = -1; for (int n = 0; n < 4; n++) { tk = (tk + 1) % 4; if (g_lkKeys & (1 << tk)) break; }
                if (g_lkKeys & (1 << tk)) { bool was = g_lockPop[tk]; g_lockPop[tk] = !was; bool keep = g_popLock; g_popLock = true; PopLockKey(tk, !was); g_popLock = keep; }
            }
            if (it.id == 30) PartyStart();
            if (it.id == 31) { g_arrOff = 0; g_arrDirty = true; g_arrMsg.clear(); }
            if (it.id == 32) { g_arrMsg = ArrApply() ? L"Done \u00B7 Windows has the new arrangement" : L"Windows didn't take the new arrangement"; BrightRequest(true, false); }
            if (it.id == 2 && MessageBoxW(h, L"Reset all Knob settings to their defaults? Knob will restart.", APP_NAME, MB_YESNO | MB_ICONQUESTION) == IDYES) {
                g_noSave = true; DeleteFileW(g_iniPath.c_str()); g_restartAfterExit = true;
                DestroyWindow(g_hwnd); return 0;
            }
            break;
        case SK_STYLE: if (g_style == it.id) break; g_style = it.id; SaveSettings(); ApplyStyle(); BuildSettingsTheme(); ApplySettingsStyle();
            g_fadeFly.on = false; g_fadeFly.last.clear(); if (g_visible) { InvalidateRect(g_hwnd, nullptr, FALSE); UpdateWindow(g_hwnd); } break;
        case SK_SWATCH: break;
        case SK_ACCMODE: if (g_accentId == it.id) break; StartFade(); g_accentId = it.id; SaveSettings(); LoadTheme(); BuildSettingsTheme(); UpdateTrayIcon(); SettingsChanged(); break;
        case SK_COLOR: g_sDragColor = it.id + 1; SetCapture(h); SettingsColorAt(x); return 0;
        case SK_HK: g_capture = it.id; UnregisterHotkeys(); break;
        case SK_STEP: g_step = it.id; SaveSettings(); break;
        case SK_RESETHK: g_sc.clear(); RegisterHotkeys(); SaveSettings(); break;
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
        case SK_HKDEL: if (it.id < (int)g_sc.size()) { UnregisterHotkeys(); g_sc.erase(g_sc.begin() + it.id); g_capture = -1; RegisterHotkeys(); SaveSettings(); } break;
        case SK_HKADD: g_sddOpen = g_sddOpen == 4 ? -1 : 4; InvalidateRect(h, nullptr, FALSE); return 0;
        case SK_NLSL: g_sDragNl = it.id; SetCapture(h); SettingsNlAt(x); return 0;
        case SK_ARR: {   // pick up the other screen
            g_arrDrag = true; g_arrGrab = PointF(x, y);
            g_arrGrabC = PointF(g_arrROther.X + g_arrROther.Width / 2, g_arrROther.Y + g_arrROther.Height / 2);
            SetCapture(h); InvalidateRect(h, nullptr, FALSE); return 0;
        }
        case SK_ARRVIEW: g_arrPix = it.id == 1; break;
        case SK_CHIP: g_lkKeys ^= 1 << it.id; SaveSettings(); break;
        case SK_MS: StartMsEdit(it.id); return 0;
        case SK_RTSRC: g_rtSel = it.id; g_rtSelDst = -1; g_rtDrag = it.id; g_rtMouse = PointF(x, y); SetCapture(h); InvalidateRect(h, nullptr, FALSE); return 0;
        case SK_RTDST:   // a speaker: select it (its sounds light up), or drag from it to a sound
            g_rtSelDst = it.id; g_rtSel = RT_NONE; g_rtDragDst = it.id; g_rtMouse = PointF(x, y); SetCapture(h); InvalidateRect(h, nullptr, FALSE); return 0;
        case SK_RTDELAY: StartDelayEdit(it.id); return 0;
        }
        SettingsChanged();
        return 0;
    }
    case WM_LBUTTONUP:
        if (g_rtDrag != RT_NONE) {   // dropped on a speaker: that sound plays there too
            int src = g_rtDrag; g_rtDrag = RT_NONE; ReleaseCapture();
            float x = (float)GET_X_LPARAM(lp), y = (float)GET_Y_LPARAM(lp);
            for (auto& kv : g_rtDst) { RectF r = kv.second; if (InRect(RectF(r.X - S(30), r.Y, r.Width + S(30), r.Height), x, y)) { RtConnect(src, kv.first); break; } }
            SettingsChanged(); return 0;
        }
        if (g_rtDragDst >= 0) {   // dragged from a speaker onto a sound: the same line, drawn the other way
            int dst = g_rtDragDst; g_rtDragDst = -1; ReleaseCapture();
            float x = (float)GET_X_LPARAM(lp), y = (float)GET_Y_LPARAM(lp);
            for (auto& kv : g_rtSrc) { RectF r = kv.second; if (InRect(RectF(r.X, r.Y, r.Width + S(30), r.Height), x, y)) { RtConnect(kv.first, dst); break; } }
            SettingsChanged(); return 0;
        }
        if (g_sDragDelay >= 0) { g_sDragDelay = -1; ReleaseCapture(); SaveSettings(); RestartRouter(); SettingsChanged(); }
        if (g_sDragColor) { g_sDragColor = 0; ReleaseCapture(); SaveSettings(); UpdateTrayIcon(); SettingsChanged(); }
        if (g_sDragNl >= 0) { g_sDragNl = -1; ReleaseCapture(); SaveSettings(); SettingsChanged(); }
        if (g_arrDrag) { g_arrDrag = false; ReleaseCapture(); InvalidateRect(h, nullptr, FALSE); }
        return 0;
    case WM_MOUSEWHEEL:
        if (g_sddOpen >= 0) {   // a list is open: the wheel moves the list (if it scrolls), never the page behind it
            POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}; ScreenToClient(h, &pt);
            if (g_sddMax > 0 && g_sddRect.Contains((float)pt.x, (float)pt.y)) {
                g_sddScroll = std::max(0.f, std::min(g_sddMax, g_sddScroll - GET_WHEEL_DELTA_WPARAM(wp) / 120.f * S(64)));
                InvalidateRect(h, nullptr, FALSE);
            }
            return 0;
        }
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
        COLORREF bg = g_editMs >= 0 ? (g_setTheme.dark ? RGB(69, 69, 69) : RGB(228, 228, 228)) : g_setTheme.dark ? RGB(43, 43, 43) : RGB(251, 251, 251);   // the ms box is a shade lighter than its card
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
        g_rtDrag = RT_NONE; g_rtSel = RT_NONE; g_rtDragDst = -1; g_rtSelDst = -1;
        if (g_capture >= 0) { g_capture = -1; RegisterHotkeys(); }
        if (g_edit) CommitRename(true);
        g_set = nullptr;
        g_capAnim = false; g_capCur = 0xFFFFFFFF;
        CanvasFree(g_cvSet); FadeFree(g_fadeSet);   // Settings closed: its frame buffers aren't needed
        if (!g_visible && !g_pairHidden) FreeApps();   // the routing board's app list
        if (g_pairHidden) { g_pairHidden = false; HideFlyout(); }   // it was tucked away: close it properly
        else if (g_visible) PostMessageW(g_hwnd, WM_APP_FOCUSPANEL, 0, 0);   // quick flyout again: on top and focused (after Settings is gone) so it closes when you click away
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
        HMONITOR hm = g_visible ? MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST) : PanelMonitor();   // next to the panel
        UINT dx = 96, dy = 96; GetDpiForMonitor(hm, MDT_EFFECTIVE_DPI, &dx, &dy);
        ApplyDpi((int)dx);
        MONITORINFO mi = {sizeof(mi)}; GetMonitorInfoW(hm, &mi);
        DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;   // fixed size, close button only
        // roomy by default (960 x 720), shrinking only as far as needed to fit beside the panel on smaller screens
        RECT pad = {0, 0, 0, 0}; AdjustWindowRectExForDpi(&pad, style, FALSE, 0, dx);
        int availW = (mi.rcWork.right - mi.rcWork.left) - (int)S(LW + 36) - (pad.right - pad.left);
        int availH = (mi.rcWork.bottom - mi.rcWork.top) - (int)S(24) - (pad.bottom - pad.top);
        RECT r = {0, 0, (LONG)std::max((int)S(820), std::min((int)S(960), availW)), (LONG)std::max((int)S(600), std::min((int)S(720), availH))};
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
        // title bar shows the plain mixer mark in the text colour, like the panel's footer
        static HICON titleIcon = nullptr;
        if (titleIcon) DestroyIcon(titleIcon);
        titleIcon = MakeGlyphIcon(g_setTheme.dark ? Color(255, 255, 255, 255) : Color(255, 28, 28, 28), GetSystemMetricsForDpi(SM_CXSMICON, dx));
        SendMessageW(g_set, WM_SETICON, ICON_SMALL, (LPARAM)titleIcon);
    }
    g_setPage = page; g_setScroll = 0;
    SetTimer(g_set, 78, 500, nullptr);
    if (page == SP_SOUND) { LoadApps(); UpdatePeaks(); }
    ShowWindow(g_set, IsIconic(g_set) ? SW_RESTORE : SW_SHOW);
    if (g_visible) PairShow(false);   // the panel stops being always-on-top while Settings is open
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
    case WM_APP_VERSION: return ParseVer(APP_VERSION);
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
            if (ParseVer(u->version) > ParseVer(APP_VERSION)) {
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
    case WM_APP + 33: g_calK = (int)wp; if (g_visible) InvalidateRect(h, nullptr, FALSE); if (g_set) InvalidateRect(g_set, nullptr, FALSE); return 0;
    case WM_APP + 34: if ((int)wp < (int)g_calIds.size()) g_calHeard[g_calIds[wp]] = (int)lp; if (g_visible) InvalidateRect(h, nullptr, FALSE); return 0;
    case WM_APP_CALDONE: {
        CalResult* r = (CalResult*)lp;
        InterlockedExchange(&g_calibrating, 0);
        if (r && r->status == L"Stopped") { g_calShow = false; g_calStatus.clear(); delete r; r = nullptr; g_calPass = 0; g_calRefineRun = false; }
        if (r) {
            for (auto& d : r->delays) { g_delay[d.first] = d.second; g_delayMeasured[d.first] = d.second; }
            if (!(g_calRefineRun && r->delays.empty())) g_calStatus = r->status;   // a check pass that found nothing keeps the first result
            g_calMissed.clear(); for (auto& id : r->missed) g_calMissed.insert(id);
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
            } else {
                g_calPass = 0;
                if (!g_calRefineRun && !r->delays.empty()) { g_calRefineRun = true; g_calPass = 2; SetTimer(h, TIMER_CALRETRY, 2500, nullptr); }   // measure once more with the new delays in place
                else g_calRefineRun = false;
            }
            delete r;
        }
        g_routerPending = false; RestartRouter();   // apply the new delays (and any device change that waited)
        if (g_calPass) InterlockedExchange(&g_calibrating, 1);   // stay busy until the second run starts
        else CalMuteOthers(false);   // all done: everyone's sound comes back
        if (g_visible) InvalidateRect(h, nullptr, FALSE);
        if (g_set) InvalidateRect(g_set, nullptr, FALSE);
        return 0;
    }
    case WM_APP_MEDIA: MediaApply((MediaSnap*)lp); if (g_party) PartyInfoBitmap(); return 0;
    case WM_INPUT: {   // flyouts: a media key or a lock key, heard without taking it
        RAWINPUT ri; UINT sz = sizeof(ri);
        if (GetRawInputData((HRAWINPUT)lp, RID_INPUT, &ri, &sz, sizeof(RAWINPUTHEADER)) != (UINT)-1 && ri.header.dwType == RIM_TYPEKEYBOARD) {
            static std::set<USHORT> down;   // held keys repeat: only the first press counts
            USHORT vk = ri.data.keyboard.VKey;
            if (ri.data.keyboard.Flags & RI_KEY_BREAK) down.erase(vk);
            else if (down.insert(vk).second) FlyoutKey(vk);
        }
        return DefWindowProcW(h, msg, wp, lp);
    }
    case WM_APP_FOCUSPANEL:
        if (g_visible && !g_set) { SetWindowPos(h, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE); ForceForeground(h); }
        return 0;
    case WM_APP_QUIT: DestroyWindow(h); return 0;
    case WM_APP_VOLCHANGED:
        InterlockedExchange(&g_volPending, 0);
        RefreshEndpointLevels(g_out); RefreshEndpointLevels(g_in);
        UpdateTrayTip();
        if (g_visible && g_dragEl < 0) InvalidateRect(h, nullptr, FALSE);
        {   // the main speaker's volume or mute really changed (not just a refresh): the media flyout shows it
            static int lastSig = -1; int di = DefaultOut();
            int sig = di < 0 ? -1 : (int)lround(g_out[di].level * 100) * 2 + (g_out[di].mute ? 1 : 0);
            if (lastSig >= 0 && sig >= 0 && sig != lastSig && GetTickCount() > 5000) PopVolumeChanged();
            lastSig = sig;
        }
        return 0;
    case WM_HOTKEY: if (wp >= 100 && (int)wp < 100 + (int)g_sc.size()) RunHotkey(g_sc[wp - 100].act, g_sc[wp - 100].param); return 0;
    case WM_APP_AUDIOCHANGED: SetTimer(h, TIMER_AUDIODEBOUNCE, 200, nullptr); return 0;
    case WM_APP + 31:   // location for sunset to sunrise arrived
        if (double* v = (double*)lp) { g_geoLat = v[0]; g_geoLon = v[1]; g_geoWhen = (long long)time(nullptr); delete[] v; }
        SaveSettings(); ApplyNight(); return 0;
    case WM_APP_MONITORS: {
        if (!g_demo) CountScreens();
        MonMsg* m = (MonMsg*)lp;
        if (g_demo) {   // the design's two screens
            m->mons.clear();
            MonUI a; a.rc = {0, 0, 1920, 1200}; a.name = L"Built-in display"; a.detail = L"Laptop panel"; a.pct = 62; a.ok = true; a.internal = true; a.hdrOk = true; a.rates = {60, 120}; a.rate = 120; a.gdi = L"\\\\.\\DISPLAY1";
            MonUI b; b.rc = {-320, -1440, 2240, 0}; b.name = L"DELL G2724D"; b.detail = L"DisplayPort"; b.pct = 100; b.con = 50; b.ok = true; b.hasCon = true; b.hdrOk = true; b.rates = {60, 120, 144, 165}; b.rate = 165;
            b.inputs = {15, 17, 18}; b.input = 15; b.gdi = L"\\\\.\\DISPLAY2";
            m->mons.push_back(a); m->mons.push_back(b);
        }
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
            if (m->fromEnum) { g_restoreAllNext = false; ApplyNight(); ApplyDim(); }
            if (g_ddOpen >= (int)g_mons.size()) g_ddOpen = -1;
            if (g_visible) InvalidateRect(h, nullptr, FALSE);
        }
        delete m;
        return 0;
    }
    case WM_TIMER:
        if (wp == TIMER_POP) {
            bool still = g_animating; g_animating = false;
            if (!still) { KillTimer(h, TIMER_POP); return 0; }
            if (g_visible) InvalidateRect(h, nullptr, FALSE);
            if (g_set) InvalidateRect(g_set, nullptr, FALSE);
            return 0;
        }
        if (wp == TIMER_NIGHT) { GeoRefresh(); ApplyNight(); return 0; }
        if (wp == TIMER_DELAYAPPLY) { KillTimer(h, TIMER_DELAYAPPLY); RestartRouter(); return 0; }
        if (wp == TIMER_FADE) {
            if (g_visible) InvalidateRect(h, nullptr, FALSE);
            if (g_set) InvalidateRect(g_set, nullptr, FALSE);
            if (!g_fadeFly.on && !g_fadeSet.on) KillTimer(h, TIMER_FADE);
            return 0;
        }
        if (wp == TIMER_AUDIODEBOUNCE) {
            KillTimer(h, TIMER_AUDIODEBOUNCE);
            if (g_dragEl >= 0) { SetTimer(h, TIMER_AUDIODEBOUNCE, 300, nullptr); return 0; }
            LoadEndpoints(eRender, g_out);
            LoadEndpoints(eCapture, g_in);
            AutoSwitchCheck();
            if (g_visible && g_tab == T_APPS) LoadApps();
            RestartRouter();
            UpdateTrayTip();
            if (g_visible) InvalidateRect(h, nullptr, FALSE);
        } else if (wp == TIMER_FOCUSWATCH) {
            if (!g_visible || g_set || g_pairHidden) return 0;   // with Settings open the two windows follow the pair rules instead
            HWND fg = GetForegroundWindow();
            if (!fg) return 0;   // Windows is switching windows
            DWORD pid = 0; GetWindowThreadProcessId(fg, &pid);
            if (fg == h) { g_hadFocus = true; return 0; }
            if (pid == GetCurrentProcessId()) return 0;   // one of Knob's own windows
            DWORD age = GetTickCount() - g_shownTick;
            if (g_hadFocus) HideFlyout();   // it was in front and another app took over
            else if (age > 400 && !g_focusRetried) { g_focusRetried = true; ForceForeground(h); }   // never got focus: one more try
            else if (age > 2000) HideFlyout();
            return 0;
        } else if (wp == TIMER_REFRESH && g_visible && g_dragEl < 0) {
            RefreshEndpointLevels(g_out);
            RefreshEndpointLevels(g_in);
            if (g_tab == T_APPS || g_tab == T_SOUND) { LoadApps(); UpdatePeaks(); }
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
        } else if (wp == TIMER_OFFAT) {   // Turn off displays after no keyboard or mouse activity (when Settings asks for it)
            int mins = OFF_MIN[std::max(0, std::min(4, g_offDelay))];
            if (!mins || !g_blackWnds.empty() || g_party || FullscreenBusy()) return 0;   // not set, already off, a party, or a full-screen video / game
            LASTINPUTINFO li = {sizeof(li)};
            if (GetLastInputInfo(&li) && GetTickCount() - li.dwTime >= (DWORD)mins * 60000u) ScreenOffNow();
            return 0;
        } else if (wp == TIMER_POPKEY) { KillTimer(h, TIMER_POPKEY); PopShow(POP_MEDIA); return 0; }
        else if (wp == TIMER_LOCKKEY) { KillTimer(h, TIMER_LOCKKEY); FlyoutLockNow(); return 0; }
        else if (wp == TIMER_SCREENOFF) {
            KillTimer(h, TIMER_SCREENOFF);
            BlackoutStart();
            return 0;
        } else if (wp == TIMER_CALRETRY) {
            KillTimer(h, TIMER_CALRETRY);
            InterlockedExchange(&g_calibrating, 0);
            RestartRouter();
            g_calChained = true;
            StartAutoSync();   // second run with the new main output, or the check pass after a sync
            if (!g_calibrating) CalMuteOthers(false);   // it couldn't start: give everyone's sound back
            if (g_set) InvalidateRect(g_set, nullptr, FALSE);
            return 0;
        } else if (wp == TIMER_TRAYPIN) {
            static int ticks = 0;
            ticks++;
            TrayPinSync(ticks <= 3);   // first ~20 s after start: restore your choice; afterwards: follow your changes
            SetTimer(h, TIMER_TRAYPIN, ticks < 3 ? 6000 : 60000, nullptr);
            return 0;
        } else if (wp == TIMER_TRIM) {
            KillTimer(h, TIMER_TRIM);
            if (!g_visible && !g_set) {   // nothing on screen: free the frame buffers and let Windows reclaim idle memory
                CanvasFree(g_cvFly); FadeFree(g_fadeFly);
                SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);
            }
            return 0;
        } else if (wp == TIMER_UPDCHECK) {
            SetTimer(h, TIMER_UPDCHECK, 6 * 60 * 60 * 1000, nullptr);   // then every 6 hours
            CheckForUpdates();
        } else if (wp == TIMER_SPIN) {
            if (GetTickCount() >= g_spinUntil) KillTimer(h, TIMER_SPIN);
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    case WM_ACTIVATE:
        if (LOWORD(wp) != WA_INACTIVE && g_set && g_visible) PairShow(false);   // clicked the panel: Settings comes forward with it
        if (LOWORD(wp) == WA_INACTIVE && g_visible) {
            if (!g_set) HideFlyout();
            else if ((HWND)lp != g_set) PairBack();   // clicked another app: panel and Settings step back together, both still visible
        }
        return 0;
    case WM_SYSKEYDOWN:
    case WM_KEYDOWN:
        if (g_capture >= (int)g_sc.size()) g_capture = -1;
        if (g_capture >= 0) {   // recording a new shortcut
            UINT vk = (UINT)wp;
            if (vk == VK_CONTROL || vk == VK_MENU || vk == VK_SHIFT || vk == VK_LWIN || vk == VK_RWIN) return 0;
            UINT mods = (GetKeyState(VK_CONTROL) < 0 ? MOD_CONTROL : 0) | (GetKeyState(VK_MENU) < 0 ? MOD_ALT : 0) |
                        (GetKeyState(VK_SHIFT) < 0 ? MOD_SHIFT : 0) | ((GetKeyState(VK_LWIN) < 0 || GetKeyState(VK_RWIN) < 0) ? MOD_WIN : 0);
            if (vk == VK_ESCAPE && !mods) { g_capture = -1; }
            else if ((vk == VK_BACK || vk == VK_DELETE) && !mods) { g_sc[g_capture].k.mods = 0; g_sc[g_capture].k.vk = 0; g_capture = -1; }
            else if (mods || (vk >= VK_F13 && vk <= VK_F24)) { g_sc[g_capture].k.mods = mods; g_sc[g_capture].k.vk = vk; g_capture = -1; }
            else return 0;   // a plain key alone would steal normal typing: wait for a combo
            RegisterHotkeys(); SaveSettings();
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        if (msg == WM_SYSKEYDOWN) return DefWindowProcW(h, msg, wp, lp);
        if (wp == VK_ESCAPE) {
            if (g_ddOpen >= 0 || g_delayOpen >= 0) { g_ddOpen = -1; g_delayOpen = -1; InvalidateRect(h, nullptr, FALSE); }
            else if (g_calShow && !g_calibrating) { g_calShow = false; g_calAsk = false; InvalidateRect(h, nullptr, FALSE); }   // Esc closes the Auto sync card first
            else HideFlyout();
        } else if (wp == VK_TAB) SetTab((g_tab + (GetKeyState(VK_SHIFT) < 0 ? 2 : 1)) % 3);
        else if (wp == VK_F5) RefreshAll();
        return 0;
    case WM_MOUSEMOVE: {
        float x = (float)GET_X_LPARAM(lp), y = (float)GET_Y_LPARAM(lp);
        if (g_dragEl >= 0 && g_dragEl < (int)g_els.size()) {
            SetElValue(g_els[g_dragEl], SliderValueAt(g_els[g_dragEl], x));
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
        if (g_delayOpen >= 0 && ht.h != H_DLMINUS && ht.h != H_DLPLUS && ht.h != H_DLRESET && ht.h != H_MENUBG) {
            bool same = ht.h == H_DELAYCHIP && ht.idx == g_delayOpen;
            g_delayOpen = -1; InvalidateRect(h, nullptr, FALSE);
            if (same || ht.h != H_DELAYCHIP) return 0;
        }
        if (g_capture >= 0) { g_capture = -1; RegisterHotkeys(); InvalidateRect(h, nullptr, FALSE); }
        if (g_ddOpen >= 0 && ht.h != H_DDOPT && ht.h != H_MENUBG) {
            int btnKind = ht.h == H_DROPDOWN ? 0 : ht.h == H_RATEDD ? 1 : ht.h == H_INPUTDD ? 2 : ht.h == H_PROFDD ? 3 : ht.h == H_APPOUT ? 4 : ht.h == H_MODEDD ? 5 : -1;
            bool sameBtn = btnKind == g_ddKind && (btnKind == 3 || ht.idx == g_ddOpen);
            g_ddOpen = -1; InvalidateRect(h, nullptr, FALSE);
            if (sameBtn || btnKind < 0) return 0;
        }
        switch (ht.h) {
        case H_MENUBG: return 0;
        case H_DDOPT: {
            int idx = g_ddOpen, kind = g_ddKind, k = ht.idx;
            int cur; auto opts = DropdownOptions(cur);
            g_ddOpen = -1;
            if (kind == 0) SetOrientation(idx, k);
            else if (kind == 1 && idx < (int)g_mons.size() && k < (int)g_mons[idx].rates.size()) SetRefreshRate(idx, g_mons[idx].rates[k]);
            else if (kind == 2 && idx < (int)g_mons.size() && k < (int)g_mons[idx].inputs.size()) SetMonitorInput(idx, g_mons[idx].inputs[k]);
            else if (kind == 5 && idx < (int)g_mons.size() && k < (int)g_ddModeMap.size()) {   // extend / duplicate / only, then confirm
                HideFlyout();   // so the "Keep these display settings?" box isn't hidden behind the panel
                SetDisplayMode(idx, g_ddModeMap[k]);
                CountScreens();
                BrightRequest(true, false);
            }
            else if (kind == 4 && idx < (int)g_apps.size()) {
                AppGroup& a = g_apps[idx];
                if (k == 0) AppRouteClear(a);
                else if (k - 1 < (int)g_out.size()) {
                    const std::wstring& id = g_out[k - 1].id;
                    auto r = AppRoutes(a);
                    if (std::find(r.begin(), r.end(), id) != r.end()) AppRouteRemove(a, id); else AppRouteAdd(a, id);
                    if (ProcLoopbackOk()) { g_ddOpen = idx; g_ddKind = 4; }   // ticking several: the list stays open
                }
            }
            else if (kind == 3 && k < (int)g_ddProfMap.size()) {
                int pi = g_ddProfMap[k];
                if (pi >= 0) ApplyProfile(pi);
                else { OpenSettings(2, pi == -1); return 0; }   // new profile / manage, in Settings > Display
            }
            InvalidateRect(h, nullptr, FALSE); return 0;
        }
        case H_RATEDD: g_ddOpen = ht.idx; g_ddKind = 1; break;
        case H_APPOUT: g_ddOpen = ht.idx; g_ddKind = 4; break;
        case H_RESETOUT: AppOutputResetAll(); for (auto& a : g_apps) a.outId.clear(); g_appExtra.clear(); RestartAppRouters(); break;
        case H_ROUTING: OpenSettings(SP_SOUND_PAGE, false); return 0;
        case H_ONESPK: OneSpeaker(); if (g_set) InvalidateRect(g_set, nullptr, FALSE); break;
        case H_INPUTDD: g_ddOpen = ht.idx; g_ddKind = 2; break;
        case H_MODEDD: g_ddOpen = ht.idx; g_ddKind = 5; break;
        case H_NIGHT: if (ht.idx >= 0 && ht.idx < (int)g_mons.size()) { auto& n = g_night[g_mons[ht.idx].name]; n.on = !n.on; SaveSettings(); ApplyNight(); } break;
        case H_HDR:
            if (ht.idx >= 0 && ht.idx < (int)g_mons.size()) {
                g_dispStatus = SetHdr(ht.idx, !g_mons[ht.idx].hdr) ? L"" : L"Windows didn't change HDR for this screen";
                BrightRequest(true, false);
            }
            break;
        case H_PROFDD: g_ddOpen = 0; g_ddKind = 3; break;
        case H_OFFEXT: case H_OFFDUP:
            if (!g_mons.empty()) { HideFlyout(); SetDisplayMode(0, ht.h == H_OFFEXT ? -1 : -3); CountScreens(); BrightRequest(true, false); }
            return 0;
        case H_SCREENOFF: ScreenOffRequest(); return 0;   // right away, or the timer set in Settings (a click while counting cancels)
        case H_PARTY: HideFlyout(); PartyStart(); return 0;
        case H_TAB: SetTab(ht.idx); return 0;
        case H_MICALL: MicToggleMute(); break;
        case H_UPDATE: StartUpdate(); break;
        case H_DELAYCHIP: g_delayOpen = ht.idx; break;
        case H_DLMINUS: case H_DLPLUS:
            NudgeDelay(g_delayOpen, ht.h == H_DLPLUS ? 5 : -5); SaveSettings();
            SetTimer(h, TIMER_DELAYAPPLY, 350, nullptr);   // apply once the clicking stops
            if (g_set) InvalidateRect(g_set, nullptr, FALSE);
            break;
        case H_DLRESET:   // back to what Auto sync measured
            for (auto& kv : g_delayMeasured) g_delay[kv.first] = kv.second;
            SaveSettings(); RestartRouter(); break;
                case H_CALSTART: g_calAsk = false; StartAutoSync(); break;
        case H_CALCANCEL: if (g_calAsk) { g_calAsk = false; g_calShow = false; } else InterlockedExchange(&g_calCancel, 1); break;
        case H_CALDONE: g_calShow = false; g_calStatus.clear(); break;
        case H_GEAR:   // open Settings, or bring it to the front if it's already open
            if (g_set) PairShow(true);
            else OpenSettings(0, false);
            return 0;
        case H_IDENT: IdentifyDisplays(); return 0;
        case H_REFRESH: RefreshAll(); return 0;
        case H_DROPDOWN: g_ddOpen = ht.idx; g_ddKind = 0; break;
        case H_SLIDER:
            g_dragEl = ht.el;
            SetCapture(h);
            SetElValue(g_els[ht.el], SliderValueAt(g_els[ht.el], x));
            break;
        case H_MUTE: ToggleMute(g_els[ht.el]); break;
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
        if (g_dragEl >= 0) {
            if (g_dragEl < (int)g_els.size() && g_els[g_dragEl].k == E_MONNL) SaveSettings();
            g_dragEl = -1; ReleaseCapture(); InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    case WM_CAPTURECHANGED:
        if (g_dragEl >= 0 && (HWND)lp != h) { g_dragEl = -1; InvalidateRect(h, nullptr, FALSE); }
        return 0;
    case WM_MOUSEWHEEL: {
        if (g_ddOpen >= 0 || g_delayOpen >= 0) return 0;
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
            if (g_cover) { CoverColours(); BuildGlows(); }
            if (g_pop && g_popShown) { PopApplyStyle(); InvalidateRect(g_pop, nullptr, FALSE); }
            if (g_set) { BuildSettingsTheme(); ApplySettingsStyle(); }
        }
        return 0;
    case WM_DWMCOLORIZATIONCOLORCHANGED: LoadTheme(); if (g_visible) InvalidateRect(h, nullptr, FALSE); return 0;
    case WM_POWERBROADCAST:
        if (wp == PBT_APMRESUMEAUTOMATIC) { g_restoreAllNext = true; BrightRequest(true, false); PostMessageW(h, WM_APP_AUDIOCHANGED, 0, 0); }
        if (wp == PBT_APMPOWERSTATUSCHANGE) RateBatApply();   // plugged in or unplugged
        return TRUE;
    case WM_DESTROY:
        BlackoutEnd(true); StopMicRouters(); CalMuteOthers(false);
        if (g_party) DestroyWindow(g_party);
        if (g_pop) { if (g_popShown) PopEnd(); DestroyWindow(g_pop); g_pop = nullptr; }
        PostQuitMessage(0); return 0;   // never leave the screens dark; never leave a mic playing
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
        if ((int)v > ParseVer(APP_VERSION)) {   // never downgrade
            MessageBoxW(nullptr, L"A newer version of Knob is already running.", APP_NAME, MB_OK | MB_ICONINFORMATION);
            return true;
        }
        if ((int)v == ParseVer(APP_VERSION)) { PostMessageW(running, WM_APP_SHOW, 0, 0); return true; }   // same version: just open it
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
const char* g_crumb = "start";   // the last step the flyouts reached: written to crash.txt if Knob ever crashes
static LONG WINAPI CrashLog(EXCEPTION_POINTERS* ep) {
    std::wstring p = g_iniPath; size_t k = p.find_last_of(L'\\'); if (k != std::wstring::npos) p = p.substr(0, k + 1); else p.clear();
    HANDLE f = CreateFileW((p + L"crash.txt").c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        char b[400]; HMODULE m = GetModuleHandleW(nullptr);
        int n = snprintf(b, sizeof(b), "Knob %ls crash: code %08lX at +%llX (access %llu at %llX), last step: %s\r\n", APP_VERSION, ep->ExceptionRecord->ExceptionCode,
            (unsigned long long)((BYTE*)ep->ExceptionRecord->ExceptionAddress - (BYTE*)m), (unsigned long long)(ep->ExceptionRecord->NumberParameters > 0 ? ep->ExceptionRecord->ExceptionInformation[0] : 0),
            (unsigned long long)(ep->ExceptionRecord->NumberParameters > 1 ? ep->ExceptionRecord->ExceptionInformation[1] : 0), g_crumb);
        DWORD w; WriteFile(f, b, n, &w, nullptr); CloseHandle(f);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, LPWSTR cmd, int) {
    SetUnhandledExceptionFilter(CrashLog);
#ifdef KNOB_DEMO
    g_demo = cmd && wcsstr(cmd, L"--demo");
#endif
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
    InitializeCriticalSection(&g_mcs);
    g_bEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    HANDLE bthread = CreateThread(nullptr, 0, BrightProc, nullptr, 0, nullptr);
    BrightRequest(true, false);

    LoadEndpoints(eRender, g_out);
    LoadEndpoints(eCapture, g_in);
    AutoSwitchCheck();   // remember what's connected now, so only new headphones trigger a switch
    RestartRouter();
    TrayAdd();
    UpdateTrayTip();
    RegisterHotkeys();
    FlyoutKeysSync();   // media and lock keys, if those flyouts are on
    if (g_demo) {   // the design's sample song, for side-by-side checks
        MediaSnap* m = new MediaSnap; MSess a; a.app = L"Brave"; a.aumid = L"brave"; a.title = L"Alfaaz | Hamza Malik x Zain Zohaib"; a.artist = L"Hamza Malik";
        a.status = 4; a.canPP = a.canNext = a.canPrev = a.canShuf = a.canRep = a.canSeek = true; a.end = 2310000000LL; a.pos = 830000000LL;
        FILETIME ft; GetSystemTimeAsFileTime(&ft); a.updated = ((INT64)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
        MSess b = a; b.app = L"Spotify"; b.aumid = L"spotify"; b.title = L"Kho Gaye Hum Kahan"; b.artist = L"Prateek Kuhad"; b.status = 5;
        m->s = {a, b}; m->cur = 0; m->newCover = true;
        m->cover = new Gdiplus::Bitmap(128, 128, PixelFormat32bppPARGB);
        { Graphics g(m->cover); LinearGradientBrush lb(PointF(0, 0), PointF(128, 128), Color(255, 107, 128, 54), Color(255, 196, 180, 110)); g.FillRectangle(&lb, 0, 0, 128, 128); }
        MediaApply(m);
    }
    SetTimer(g_hwnd, TIMER_UPDCHECK, 15000, nullptr);   // first check 15 s after start
    SetTimer(g_hwnd, TIMER_OFFAT, 15000, nullptr);      // Turn off displays after no activity: looked at every 15 seconds, only when set
    SetTimer(g_hwnd, TIMER_NIGHT, 60000, nullptr);      // night light: follow the evening schedule, and come back if Windows resets it
    SetTimer(g_hwnd, TIMER_TRAYPIN, 3000, nullptr);
    if (cmd && wcsstr(cmd, L"--show")) PostMessageW(g_hwnd, WM_APP_SHOW, 0, 0);   // just installed / updated: show it

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) { TranslateMessage(&m); DispatchMessageW(&m); }

    SaveSettings();
    if (g_mediaThread) { PostThreadMessageW(g_mediaTid, MC_QUIT, 0, 0); WaitForSingleObject(g_mediaThread, 2000); CloseHandle(g_mediaThread); g_mediaThread = nullptr; }
    StopRouter();
    g_appExtra.clear(); RestartAppRouters();   // stop every app copy
    ResetGamma(); g_dim.clear(); ApplyDim();     // screens back to normal colour, no dark layer
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
