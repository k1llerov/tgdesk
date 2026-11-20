// TgDesk — desktop app: setup wizard, bot control panel, users, features, live streams monitor, access log, tray.
// The bot (bot\TgDeskBot.exe) runs as a separate background process; this app talks to it over the local control API.
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef WINVER
#define WINVER 0x0A00
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <commctrl.h>
#include <tlhelp32.h>
#include <thread>
#include <mutex>
#include <atomic>
#include <set>
#include <functional>
#include <cstdarg>
#include "ui.h"
#include "json.h"
#include "http.h"
#include "i18n.h"

using namespace ui;

static const wchar_t* APP_VERSION = L"1.0.0";
static const wchar_t* REPO_URL = L"https://github.com/k1llerov/tgdesk";
static const wchar_t* APP_AUTHOR = L"k1llerov™";

// ============================================================================ utilities
static std::wstring W(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}
static std::string U(const std::wstring& s) {
    if (s.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    std::string o(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), &o[0], n, nullptr, nullptr);
    return o;
}
static double nowSec() { return GetTickCount64() / 1000.0; }

static std::wstring fmt(const wchar_t* f, ...) {
    wchar_t b[512];
    va_list a;
    va_start(a, f);
    vswprintf(b, 512, f, a);
    va_end(a);
    return b;
}
static std::wstring fmtDur(int s) {
    if (s >= 3600) return fmt(L"%d:%02d:%02d", s / 3600, s / 60 % 60, s % 60);
    return fmt(L"%02d:%02d", s / 60, s % 60);
}
static std::wstring fmtUptime(long long s) {
    if (s >= 86400) return fmt(T(L"%lld д. %lld ч"), s / 86400, s / 3600 % 24);
    if (s >= 3600) return fmt(T(L"%lld ч %lld мин"), s / 3600, s / 60 % 60);
    return fmt(T(L"%lld мин %lld с"), s / 60, s % 60);
}
static std::wstring fmtBytes(double n) {
    const wchar_t* u[] = {T(L"Б"), T(L"КБ"), T(L"МБ"), T(L"ГБ"), T(L"ТБ")};
    int i = 0;
    while (n >= 1024 && i < 4) { n /= 1024; i++; }
    return fmt(i == 0 ? L"%.0f %ls" : L"%.1f %ls", n, u[i]);
}
// first letter upper case (towupper does not know Cyrillic)
static std::wstring cap(std::wstring s) {
    if (!s.empty()) CharUpperBuffW(&s[0], 1);
    return s;
}

// ============================================================================ model
static const char* PERMS[] = {"screen", "input", "files", "processes", "media", "power"};
static const int PERM_N = 6;
static const wchar_t* permTitle(int i) {
    static const wchar_t* t[] = {L"Экран", L"Мышь и клавиатура", L"Файлы", L"Процессы", L"Звук и медиа", L"Питание"};
    return T(t[i]);
}
static const wchar_t* permDesc(int i) {
    static const wchar_t* d[] = {
        L"Снимки и трансляция экрана на телефон",
        L"Курсор и ввод текста во время трансляции, буфер обмена",
        L"Просмотр, скачивание и загрузка файлов, открытие на ПК",
        L"Список процессов и их завершение",
        L"Громкость и кнопки медиа",
        L"Блокировка, сон, перезагрузка и выключение",
    };
    return T(d[i]);
}
static const wchar_t PERM_ICONS[] = {0xE7F4, 0xE962, 0xE8B7, 0xE9D9, 0xE767, 0xE7E8};

struct Sess { std::string sid; std::wstring who, ip; int seconds = 0; };
struct LogItem { int id = 0; std::wstring time, cat, action, detail, user, ip, place, device, level, ev; };
struct UserRec { long long id = 0; std::wstring name, username, role, added, lastSeen; std::set<std::string> perms; };

struct Model {
    bool online = false, gotState = false;
    bool configured = false;          // the bot has a token
    std::wstring version, host, tunnelUrl, tunnelState, tunnelError, botUser, botState, quality = L"auto", lang = L"auto";
    std::wstring tunnelMode = L"quick", publicUrl;
    bool tunnelHasToken = false;
    long long uptime = 0;
    bool autostart = false, updateCheck = true, geoip = true;
    std::wstring updVersion, updUrl, updState, updError;
    bool updCanInstall = false, autoUpdate = true, portable = false;
    int updProgress = 0;
    std::map<std::string, bool> features;
    std::vector<UserRec> users;
    bool pairActive = false;
    std::wstring pairCode, pairLink, pairRole;
    int pairExpires = 0;
    ULONGLONG pairTick = 0;
    int lastPairedSeq = -1;
    int blockedIps = 0;
    std::vector<Sess> sessions;
    ULONGLONG sessTick = 0;
    int statToday = 0, statIps = 0, statDenied = 0, statStreams = 0;
    std::vector<LogItem> log;
    int lastLogId = 0;
    double cpu = 0, ram = 0, ramUsed = 0, ramTotal = 0;
    std::wstring os;
    std::vector<float> cpuHist, ramHist;
    std::vector<std::wstring> syslog;
};
static Model g_m;
static std::mutex g_mx;

enum { WM_APP_MODEL = WM_APP + 1, WM_APP_NOTIFY, WM_APP_TOAST, WM_TRAY, WM_APP_SHOW, WM_APP_ENTER };

static HWND g_hwnd;
static HINSTANCE g_inst;
static std::atomic<bool> g_run{true};
static std::atomic<int> g_pageAtomic{0};
static bool g_closeToTray = true;
static std::wstring g_uiLang = L"auto";   // auto | ru | en (app.ini; also sent to the bot)
static NOTIFYICONDATAW g_nid;
static UINT g_taskbarCreated;
static std::wstring g_toast;
static ULONGLONG g_toastUntil = 0;

enum Page { P_HOME, P_STREAMS, P_LOG, P_USERS, P_FEATURES, P_SETTINGS, P_ABOUT, P_COUNT };
static int g_page = P_HOME;
static float g_scroll[P_COUNT], g_scrollT[P_COUNT], g_maxScroll = 0;
static int g_logFilter = 0;

// setup wizard
static bool g_wizard = false;          // wizard is on screen
static bool g_wizardForced = false;    // opened from Settings ("change token")
static int g_wizStep = 0;              // 0 welcome · 1 token · 2 pair owner · 3 features
static std::wstring g_wizError;
static bool g_wizBusy = false;
static bool g_showSecret = false;
static bool g_pairRequested = false;

// users page
static int g_newRole = 0;              // 0 full access · 1 selected features
static std::set<std::string> g_newPerms = {"screen", "media"};
static long long g_expandedUser = 0;

// settings page: Mini App address
static int g_tunSel = -1;              // -1 → take from the model

enum {
    ID_NAV = 100, ID_START = 120, ID_STOP, ID_RESTART, ID_TUN_COPY, ID_TUN_RESTART, ID_OPEN_BOT,
    ID_KICK_ALL, ID_OPEN_WEBLOG, ID_CSV, ID_TG_AUTOSTART, ID_TG_TRAY, ID_OPEN_DIR, ID_OPEN_BOTLOG, ID_OPEN_DATA,
    ID_WIZ_NEXT, ID_WIZ_BACK, ID_WIZ_BOTFATHER, ID_WIZ_CHECK, ID_WIZ_SHOW, ID_WIZ_FINISH, ID_WIZ_NEWCODE,
    ID_PAIR_START, ID_PAIR_CANCEL, ID_PAIR_OPEN, ID_PAIR_COPY, ID_TUN_APPLY, ID_TOKEN_CHANGE, ID_UPD_CHECK, ID_GEOIP,
    ID_UPD_OPEN, ID_ABOUT_GH, ID_ABOUT_ISSUES, ID_ABOUT_LICENSE, ID_HOME_USERS, ID_ABOUT_UNINSTALL, ID_UPD_INSTALL, ID_AUTO_UPD,
    ID_CHIP = 200, ID_QUAL = 300, ID_WIZ_LANG = 400, ID_PAIR_ROLE = 420, ID_PAIR_PERM = 440, ID_FEAT = 460, ID_WIZ_FEAT = 470,
    ID_SET_LANG = 480, ID_TUN_MODE = 500,
    ID_KICK = 1000, ID_USER_EXPAND = 2000, ID_USER_DEL = 3000, ID_USER_ROLE = 100000, ID_USER_PERM = 300000,
};
enum { IN_TOKEN = 1, IN_CFTOKEN, IN_URL };

static void toast(const std::wstring& s) {
    {
        std::lock_guard<std::mutex> l(g_mx);
        g_toast = s;
        g_toastUntil = GetTickCount64() + 2800;
    }
    if (g_hwnd) PostMessage(g_hwnd, WM_APP_TOAST, 0, 0);
}

// ============================================================================ language
static bool systemIsRussian() {
    LANGID l = GetUserDefaultUILanguage();
    WORD p = PRIMARYLANGID(l);
    return p == LANG_RUSSIAN || p == LANG_UKRAINIAN || p == LANG_BELARUSIAN;
}
static void applyLang() { i18n::english = g_uiLang == L"en" || (g_uiLang == L"auto" && !systemIsRussian()); }

// ============================================================================ bot process
static std::wstring botExe() { return api::installDir + L"\\bot\\TgDeskBot.exe"; }
// written by TgDesk-Setup.exe; absent in the portable build
static std::wstring uninstallerExe() { return api::installDir + L"\\Uninstall TgDesk.exe"; }

static bool startBot() {
    std::wstring exe = botExe();
    if (!api::exists(exe)) return false;
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    std::wstring cmd = L"\"" + exe + L"\"";
    std::wstring cwd = api::installDir + L"\\bot";
    BOOL ok = CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW | DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP,
                             nullptr, cwd.c_str(), &si, &pi);
    if (ok) { CloseHandle(pi.hThread); CloseHandle(pi.hProcess); }
    return ok != FALSE;
}

static void killBotProcesses() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof pe;
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
        if (_wcsicmp(pe.szExeFile, L"TgDeskBot.exe") != 0) continue;
        HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pe.th32ProcessID);
        if (h) { TerminateProcess(h, 0); CloseHandle(h); }
    }
    CloseHandle(snap);
}

static bool botAlive() { return api::get(L"/state", 1500).status == 200; }

static void stopBotSync() {
    api::post(L"/shutdown", "{}", 2000);
    for (int i = 0; i < 40 && botAlive(); i++) Sleep(150);
    if (botAlive()) killBotProcesses();
    for (int i = 0; i < 20 && botAlive(); i++) Sleep(150);
}

static void async(std::function<void()> f) { std::thread(std::move(f)).detach(); }

static void actStart() {
    async([] {
        toast(T(L"Запускаю бота…"));
        if (!startBot()) toast(T(L"Не найден bot\\TgDeskBot.exe — переустанови TgDesk"));
    });
}
static void actStop() {
    async([] { toast(T(L"Останавливаю бота…")); stopBotSync(); toast(T(L"Бот остановлен")); });
}
static void actRestart() {
    async([] {
        toast(T(L"Перезапускаю бота…"));
        stopBotSync();
        Sleep(400);
        if (startBot()) toast(T(L"Бот перезапущен")); else toast(T(L"Не найден bot\\TgDeskBot.exe — переустанови TgDesk"));
    });
}

static void copyText(const std::wstring& s) {
    if (!OpenClipboard(g_hwnd)) return;
    EmptyClipboard();
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, (s.size() + 1) * sizeof(wchar_t));
    if (h) {
        memcpy(GlobalLock(h), s.c_str(), (s.size() + 1) * sizeof(wchar_t));
        GlobalUnlock(h);
        SetClipboardData(CF_UNICODETEXT, h);
    }
    CloseClipboard();
}
static void openUrl(const std::wstring& u) {
    // only web links and folders/files of TgDesk are opened here
    ShellExecuteW(nullptr, L"open", u.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// ============================================================================ local app settings (app.ini in the data folder)
static std::wstring iniPath() { return api::dataDir + L"\\app.ini"; }
static void loadIni() {
    g_closeToTray = GetPrivateProfileIntW(L"app", L"closeToTray", 1, iniPath().c_str()) != 0;
    wchar_t b[16];
    GetPrivateProfileStringW(L"app", L"lang", L"", b, 16, iniPath().c_str());
    g_uiLang = b;
    if (g_uiLang.empty()) {
        // first start: the bot config may already have a language (e.g. after reinstall)
        std::string l = api::readConfig()["lang"].str("auto");
        g_uiLang = l == "ru" ? L"ru" : l == "en" ? L"en" : L"auto";
    }
    if (g_uiLang != L"ru" && g_uiLang != L"en") g_uiLang = L"auto";
}
static void saveIni() {
    CreateDirectoryW(api::dataDir.c_str(), nullptr);
    WritePrivateProfileStringW(L"app", L"closeToTray", g_closeToTray ? L"1" : L"0", iniPath().c_str());
    WritePrivateProfileStringW(L"app", L"lang", g_uiLang.c_str(), iniPath().c_str());
}

// ============================================================================ background polling
static void parseState(const JV& j) {
    Model& m = g_m;
    m.gotState = true;
    m.version = W(j["version"].str());
    m.host = W(j["host"].str());
    m.uptime = (long long)j["uptime"].num();
    m.configured = j["configured"].boolean();
    m.lang = W(j["lang"].str("auto"));
    m.tunnelUrl = W(j["tunnel_url"].str());
    m.tunnelState = W(j["tunnel_state"].str());
    m.tunnelError = W(j["tunnel_error"].str());
    m.tunnelMode = W(j["tunnel"]["mode"].str("quick"));
    m.publicUrl = W(j["tunnel"]["public_url"].str());
    m.tunnelHasToken = j["tunnel"]["has_token"].boolean();
    m.botUser = W(j["bot_username"].str());
    m.botState = W(j["bot_state"].str());
    m.autostart = j["autostart"].boolean();
    m.quality = W(j["quality"].str("auto"));
    m.updateCheck = j["update_check"].boolean(true);
    m.geoip = j["geoip"].boolean(true);
    m.updVersion = W(j["update"]["version"].str());
    m.updUrl = W(j["update"]["url"].str());
    m.updCanInstall = j["update"]["can_install"].boolean();
    m.updState = W(j["update"]["state"].str("idle"));
    m.updError = W(j["update"]["error"].str());
    m.updProgress = (int)j["update"]["progress"].num();
    m.autoUpdate = j["auto_update"].boolean(true);
    m.portable = j["portable"].boolean();
    m.features.clear();
    for (int i = 0; i < PERM_N; i++) m.features[PERMS[i]] = j["features"][PERMS[i]].boolean(true);
    m.users.clear();
    for (size_t i = 0; i < j["users"].size(); i++) {
        const JV& u = j["users"].at(i);
        UserRec r;
        r.id = (long long)u["id"].num();
        r.name = W(u["name"].str());
        r.username = W(u["username"].str());
        r.role = W(u["role"].str("user"));
        r.added = W(u["added"].str());
        r.lastSeen = W(u["last_seen"].str());
        for (size_t k = 0; k < u["perms"].size(); k++) r.perms.insert(u["perms"].at(k).str());
        m.users.push_back(r);
    }
    const JV& p = j["pairing"];
    m.pairActive = p["active"].boolean();
    m.pairCode = W(p["code"].str());
    m.pairLink = W(p["link"].str());
    m.pairRole = W(p["role"].str());
    m.pairExpires = p["expires_in"].i();
    m.pairTick = GetTickCount64();
    m.blockedIps = j["blocked_ips"].i();
    api::logPort = j["ports"]["log"].i(api::logPort);
    m.sessions.clear();
    for (size_t i = 0; i < j["sessions"].size(); i++) {
        const JV& s = j["sessions"].at(i);
        Sess x;
        x.sid = s["sid"].str();
        x.who = W(s["who"].str());
        x.ip = W(s["ip"].str());
        x.seconds = s["seconds"].i();
        m.sessions.push_back(x);
    }
    m.sessTick = GetTickCount64();
    m.statToday = j["stats"]["today"].i();
    m.statIps = j["stats"]["ips"].i();
    m.statDenied = j["stats"]["denied"].i();
    m.statStreams = j["stats"]["streams"].i();
}

static void notifyBalloon(const std::wstring& title, const std::wstring& msg) {
    auto* p = new std::pair<std::wstring, std::wstring>(title, msg);
    PostMessage(g_hwnd, WM_APP_NOTIFY, 0, (LPARAM)p);
}

static void pushLang() {
    std::string l = U(g_uiLang);
    async([l] { api::post(L"/settings", "{\"lang\":\"" + l + "\"}"); });
}

static void pollLoop() {
    int tick = 0;
    std::set<std::string> prevSess;
    bool firstLog = true, langSynced = false;
    while (g_run) {
        HttpResult r = api::get(L"/state", 2500);
        JV j;
        bool ok = r.status == 200 && parseJson(r.body, j);
        std::vector<std::pair<std::wstring, std::wstring>> notes;
        bool needLangPush = false;
        {
            std::lock_guard<std::mutex> l(g_mx);
            g_m.online = ok;
            if (ok) {
                parseState(j);
                std::set<std::string> cur;
                for (auto& s : g_m.sessions) {
                    cur.insert(s.sid);
                    if (!prevSess.count(s.sid) && tick != 0) notes.push_back({T(L"Кто-то смотрит экран"), s.who});
                }
                prevSess = cur;
                int seq = j["last_paired"]["seq"].i(0);
                if (seq != g_m.lastPairedSeq) {
                    if (g_m.lastPairedSeq >= 0 && seq > 0)
                        notes.push_back({T(L"Новый пользователь"), W(j["last_paired"]["name"].str())});
                    g_m.lastPairedSeq = seq;
                }
                if (!langSynced) { langSynced = true; needLangPush = g_m.lang != g_uiLang; }
            } else {
                g_m.sessions.clear();
                prevSess.clear();
            }
        }
        if (needLangPush) pushLang();
        if (ok) {
            if (tick % 2 == 0) {
                int after;
                { std::lock_guard<std::mutex> l(g_mx); after = g_m.lastLogId; }
                HttpResult lr = api::get(L"/log?after=" + std::to_wstring(after), 3000);
                JV lj;
                if (lr.status == 200 && parseJson(lr.body, lj)) {
                    std::lock_guard<std::mutex> l(g_mx);
                    for (size_t i = 0; i < lj["items"].size(); i++) {
                        const JV& e = lj["items"].at(i);
                        LogItem it;
                        it.id = e["id"].i();
                        it.time = W(e["time"].str());
                        it.cat = W(e["cat"].str());
                        it.action = W(e["action"].str());
                        it.detail = W(e["detail"].str());
                        it.user = W(e["user"].str());
                        it.ip = W(e["ip"].str());
                        it.place = W(e["place"].str());
                        it.device = W(e["device"].str());
                        it.level = W(e["level"].str());
                        it.ev = W(e["ev"].str());
                        if (it.id <= g_m.lastLogId) continue;
                        g_m.lastLogId = it.id;
                        if (!firstLog && it.cat == L"deny") notes.push_back({T(L"Отказ в доступе"), it.action + (it.ip.empty() ? L"" : L" · " + it.ip)});
                        g_m.log.push_back(it);
                    }
                    // place of an IP arrives a bit later
                    for (size_t i = 0; i < lj["updates"].size(); i++) {
                        const JV& u = lj["updates"].at(i);
                        int id = u["id"].i();
                        for (size_t k = g_m.log.size(); k-- > 0 && k + 120 > g_m.log.size();)
                            if (g_m.log[k].id == id) { if (!u["place"].isNull()) g_m.log[k].place = W(u["place"].str()); break; }
                    }
                    if (g_m.log.size() > 1500) g_m.log.erase(g_m.log.begin(), g_m.log.begin() + (g_m.log.size() - 1200));
                    firstLog = false;
                }
            }
            if (tick % 3 == 0) {
                HttpResult sr = api::get(L"/status", 4000);
                JV sj;
                if (sr.status == 200 && parseJson(sr.body, sj)) {
                    std::lock_guard<std::mutex> l(g_mx);
                    g_m.cpu = sj["cpu"].num();
                    g_m.ram = sj["ram"]["percent"].num();
                    g_m.ramUsed = sj["ram"]["used"].num();
                    g_m.ramTotal = sj["ram"]["total"].num();
                    g_m.os = W(sj["os"].str());
                    g_m.cpuHist.push_back((float)g_m.cpu);
                    g_m.ramHist.push_back((float)g_m.ram);
                    if (g_m.cpuHist.size() > 60) g_m.cpuHist.erase(g_m.cpuHist.begin());
                    if (g_m.ramHist.size() > 60) g_m.ramHist.erase(g_m.ramHist.begin());
                }
            }
            if (g_pageAtomic == P_LOG && g_logFilter == 7 && tick % 2 == 0) {
                HttpResult sl = api::get(L"/syslog?lines=250", 3000);
                if (sl.status == 200) {
                    std::lock_guard<std::mutex> l(g_mx);
                    g_m.syslog.clear();
                    std::string cur;
                    for (char c : sl.body) { if (c == '\n') { g_m.syslog.push_back(W(cur)); cur.clear(); } else cur += c; }
                    if (!cur.empty()) g_m.syslog.push_back(W(cur));
                }
            }
        }
        for (auto& n : notes) notifyBalloon(n.first, n.second);
        if (g_hwnd) PostMessage(g_hwnd, WM_APP_MODEL, 0, 0);
        tick++;
        for (int i = 0; i < 10 && g_run; i++) Sleep(100);
    }
}

static void postSettings(const std::string& body, const wchar_t* okMsg = nullptr) {
    std::wstring ok = okMsg ? okMsg : L"";
    async([body, ok] {
        HttpResult r = api::post(L"/settings", body);
        if (r.status != 200) toast(T(L"Бот не отвечает — настройка не применена"));
        else if (!ok.empty()) toast(ok);
    });
}

// ============================================================================ text inputs (native EDIT controls placed over the drawn boxes)
struct InputCtl { HWND h = nullptr; bool used = false; RectF r; std::wstring cue, cueSet; bool password = false; };
static std::map<int, InputCtl> g_inputs;
static HFONT g_editFont = nullptr;
static HBRUSH g_editBrush = nullptr;
static WNDPROC g_editProc = nullptr;

static LRESULT CALLBACK EditSubProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_KEYDOWN && wp == VK_RETURN) { PostMessage(g_hwnd, WM_APP_ENTER, GetDlgCtrlID(h), 0); return 0; }
    if (msg == WM_CHAR && (wp == '\r' || wp == '\n')) return 0;
    if (msg == WM_SETFOCUS || msg == WM_KILLFOCUS) InvalidateRect(g_hwnd, nullptr, FALSE);
    return CallWindowProcW(g_editProc, h, msg, wp, lp);
}

static void makeEditFont() {
    if (g_editFont) DeleteObject(g_editFont);
    g_editFont = CreateFontW(-(int)(15 * S), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    for (auto& p : g_inputs) if (p.second.h) SendMessageW(p.second.h, WM_SETFONT, (WPARAM)g_editFont, TRUE);
}

static std::wstring inputText(int id) {
    auto it = g_inputs.find(id);
    if (it == g_inputs.end() || !it->second.h) return L"";
    int n = GetWindowTextLengthW(it->second.h);
    std::wstring s(n, 0);
    if (n) GetWindowTextW(it->second.h, &s[0], n + 1);
    while (!s.empty() && iswspace(s.back())) s.pop_back();
    while (!s.empty() && iswspace(s.front())) s.erase(s.begin());
    return s;
}
static void setInputText(int id, const std::wstring& s) {
    auto it = g_inputs.find(id);
    if (it != g_inputs.end() && it->second.h) SetWindowTextW(it->second.h, s.c_str());
}

// draws the box; the EDIT itself is positioned after the frame in layoutInputs()
static void inputBox(Graphics& g, int id, RectF r, const std::wstring& cue, bool password) {
    auto& c = g_inputs[id];
    bool focused = c.h && GetFocus() == c.h;
    fillRR(g, r, 10 * S, cCard2);
    strokeRR(g, r, 10 * S, focused ? cAcc1 : cBorder, focused ? 1.5f : 1.f);
    c.used = true;
    c.cue = cue;
    c.password = password;
    float eh = 24 * S;
    c.r = RectF(r.X + off.X + 14 * S, r.Y + off.Y + (r.Height - eh) / 2, r.Width - 28 * S, eh);
}

static void layoutInputs() {
    for (auto& p : g_inputs) {
        InputCtl& c = p.second;
        bool visible = c.used && c.r.Y >= viewClip.Y && c.r.Y + c.r.Height <= viewClip.Y + viewClip.Height;
        if (!visible) {
            if (c.h && IsWindowVisible(c.h)) {
                if (GetFocus() == c.h) SetFocus(g_hwnd);
                ShowWindow(c.h, SW_HIDE);
            }
            continue;
        }
        if (!c.h) {
            c.h = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL, 0, 0, 10, 10, g_hwnd, (HMENU)(INT_PTR)p.first, g_inst, nullptr);
            SendMessageW(c.h, WM_SETFONT, (WPARAM)g_editFont, TRUE);
            SendMessageW(c.h, EM_SETLIMITTEXT, 300, 0);
            g_editProc = (WNDPROC)SetWindowLongPtrW(c.h, GWLP_WNDPROC, (LONG_PTR)EditSubProc);
            if (p.first == IN_URL) {   // prefill the current public address
                std::wstring u;
                { std::lock_guard<std::mutex> l(g_mx); u = g_m.publicUrl; }
                SetWindowTextW(c.h, u.c_str());
            }
        }
        wchar_t pc = (c.password && !g_showSecret) ? L'\x2022' : 0;
        if ((wchar_t)SendMessageW(c.h, EM_GETPASSWORDCHAR, 0, 0) != pc) { SendMessageW(c.h, EM_SETPASSWORDCHAR, pc, 0); InvalidateRect(c.h, nullptr, TRUE); }
        if (c.cue != c.cueSet) { SendMessageW(c.h, EM_SETCUEBANNER, TRUE, (LPARAM)c.cue.c_str()); c.cueSet = c.cue; }
        RECT cur;
        GetWindowRect(c.h, &cur);
        MapWindowPoints(nullptr, g_hwnd, (POINT*)&cur, 2);
        int x = (int)c.r.X, y = (int)c.r.Y, w = (int)c.r.Width, h = (int)c.r.Height;
        if (cur.left != x || cur.top != y || cur.right - cur.left != w || cur.bottom - cur.top != h || !IsWindowVisible(c.h))
            SetWindowPos(c.h, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }
}

// ============================================================================ drawing: common parts
static const wchar_t* NAV_NAMES[] = {L"Обзор", L"Трансляции", L"Журнал", L"Пользователи", L"Функции", L"Настройки", L"О программе"};
static const wchar_t NAV_ICONS[] = {0xE80F, 0xE714, 0xE81C, 0xE716, 0xE9E9, 0xE713, 0xE946};
static const wchar_t* CHIP_NAMES[] = {L"Все", L"Входы", L"Трансляции", L"Управление", L"Файлы", L"Отказы", L"Система", L"Системный лог"};

static float sidebarW() { return 232 * S; }

static bool catMatches(const std::wstring& c, int f) {
    switch (f) {
        case 0: return true;
        case 1: return c == L"auth";
        case 2: return c == L"stream";
        case 3: return c == L"control" || c == L"power" || c == L"chat" || c == L"view";
        case 4: return c == L"files";
        case 5: return c == L"deny";
        case 6: return c == L"system";
    }
    return true;
}
static Color catColor(const std::wstring& c) {
    if (c == L"stream") return cAcc2;
    if (c == L"control" || c == L"chat") return cAmber;
    if (c == L"power" || c == L"deny") return cRed;
    if (c == L"files") return cAcc1;
    if (c == L"auth") return cGreen;
    if (c == L"view") return cAcc2;
    return cMuted;
}

static std::wstring botStateText(const std::wstring& s) {
    if (s == L"online") return T(L"в сети");
    if (s == L"connecting") return T(L"подключение");
    if (s == L"bad_token") return T(L"токен недействителен");
    if (s == L"offline") return T(L"нет связи с Telegram");
    if (s == L"not_configured") return T(L"не настроен");
    return s;
}
static std::wstring tunnelStateText(const Model& m) {
    const std::wstring& s = m.tunnelState;
    if (s == L"online") return T(L"в сети");
    if (s == L"downloading") return T(L"скачиваю cloudflared");
    if (s == L"connecting") return T(L"подключаюсь");
    if (s == L"starting") return T(L"запуск");
    if (s == L"no_url") return T(L"адрес не задан");
    if (s == L"error") return T(L"ошибка");
    if (s == L"offline") return T(L"нет связи");
    return s;
}
static std::wstring modeTitle(const std::wstring& mode) {
    if (mode == L"cloudflare") return T(L"Свой Cloudflare Tunnel");
    if (mode == L"external") return T(L"Свой адрес");
    return T(L"Cloudflare Quick Tunnel");
}

struct BotSt { Color c; std::wstring text; };
static BotSt botStatus(const Model& m) {
    if (!m.online) return {cRed, T(L"Бот остановлен")};
    if (!m.configured) return {cAmber, T(L"Нужна настройка")};
    if (m.botState != L"online") return {cAmber, cap(botStateText(m.botState))};
    if (m.tunnelUrl.empty()) return {cAmber, T(L"Запускается…")};
    return {cGreen, T(L"Бот в сети")};
}

static float pageTitle(Graphics& g, float W_, const std::wstring& t, const std::wstring& sub) {
    text(g, t, RectF(0, 0, W_, 38 * S), 27, cText, 0, 0, 1, FONT_SEMI);
    text(g, sub, RectF(0, 38 * S, W_, 22 * S), 14, cMuted);
    return 78 * S;
}

static void cardLabel(Graphics& g, RectF r, const std::wstring& s) {
    text(g, s, RectF(r.X + 18 * S, r.Y + 14 * S, r.Width - 36 * S, 18 * S), 11.5f, cMuted, 0, 0, 1, FONT_SEMI);
}

static void lockedPill(Graphics& g, RectF lk, const std::wstring& label) {
    fillRR(g, lk, lk.Height / 2, alpha(cGreen, 34));
    icon(g, 0xE72E, RectF(lk.X + 8 * S, lk.Y, 24 * S, lk.Height), 13, cGreen);
    text(g, label, RectF(lk.X + 30 * S, lk.Y, lk.Width - 34 * S, lk.Height), 13, cGreen, 0, 0, 1, FONT_SEMI);
}

static float measure(Graphics& g, const std::wstring& s, float px, const std::wstring& fam = L"") {
    RectF m;
    StringFormat sf;
    g.MeasureString(s.c_str(), -1, font(px, 0, fam), PointF(0, 0), &sf, &m);
    return m.Width;
}

// a row "title / description … control" inside a card
static void settingRow(Graphics& g, RectF card_, float ry, const std::wstring& title, const std::wstring& sub, float rightReserve = 300) {
    float pad = 22 * S;
    text(g, title, RectF(card_.X + pad, ry, card_.Width - rightReserve * S, 24 * S), 14.5f, cText, 0, 0, 1, FONT_SEMI);
    if (!sub.empty()) text(g, sub, RectF(card_.X + pad, ry + 24 * S, card_.Width - rightReserve * S, 20 * S), 12.5f, cMuted);
}

// ============================================================================ pages
static float updateBanner(Graphics& g, float W_, float y) {
    Model& m = g_m;
    if (m.updVersion.empty()) return y;
    RectF r(0, y, W_, 64 * S);
    gradRR(g, r, 16 * S, alpha(cAcc1, 70), alpha(cAcc2, 50), 0.f);
    strokeRR(g, r, 16 * S, alpha(cAcc2, 140));
    icon(g, 0xE896, RectF(r.X + 16 * S, r.Y, 30 * S, r.Height), 18, cAcc2);
    text(g, fmt(T(L"Доступна новая версия TgDesk %ls"), m.updVersion.c_str()), RectF(r.X + 56 * S, r.Y + 10 * S, r.Width - 260 * S, 24 * S), 15, cText, 0, 0, 1, FONT_SEMI);
    std::wstring sub;
    if (m.updState == L"downloading") sub = fmt(T(L"Скачиваю и проверяю подпись… %d%%"), m.updProgress);
    else if (m.updState == L"installing") sub = T(L"Устанавливаю — TgDesk перезапустится сам через несколько секунд");
    else if (m.updState == L"error") sub = T(L"Не удалось обновить: ") + m.updError;
    else if (m.updCanInstall) sub = T(L"Подпись автора проверяется перед установкой — настройки и пользователи сохранятся");
    else sub = T(L"Скачай установщик со страницы релиза — настройки и пользователи сохранятся");
    text(g, sub, RectF(r.X + 56 * S, r.Y + 34 * S, r.Width - 260 * S, 20 * S), 12.5f, m.updState == L"error" ? cRed : cMuted);
    bool busy = m.updState == L"downloading" || m.updState == L"installing";
    if (busy) {
        bar(g, RectF(r.X + r.Width - 190 * S, r.Y + 28 * S, 174 * S, 8 * S), m.updState == L"installing" ? 1.f : m.updProgress / 100.f, cAcc1, cAcc2);
        animating = true;
    } else if (m.updCanInstall)
        button(g, ID_UPD_INSTALL, RectF(r.X + r.Width - 190 * S, r.Y + 13 * S, 174 * S, 38 * S), T(L"Обновить сейчас"), Primary, 0xE896);
    else
        button(g, ID_UPD_OPEN, RectF(r.X + r.Width - 190 * S, r.Y + 13 * S, 174 * S, 38 * S), T(L"Страница релиза"), Primary, 0xE774);
    return y + r.Height + 16 * S;
}

static float pageHome(Graphics& g, float W_, double t) {
    Model& m = g_m;
    float y = pageTitle(g, W_, T(L"Обзор"), (m.host.empty() ? L"" : m.host) + (m.os.empty() ? L"" : L"  ·  " + m.os));
    y = updateBanner(g, W_, y);
    float gap = 16 * S, cw = (W_ - gap * 2) / 3, ch = 160 * S;

    // --- bot
    {
        RectF r(0, y, cw, ch);
        card(g, r);
        cardLabel(g, r, T(L"БОТ"));
        BotSt st = botStatus(m);
        statusDot(g, r.X + 26 * S, r.Y + 58 * S, 5.5f * S, st.c, false, t);
        text(g, st.text, RectF(r.X + 42 * S, r.Y + 42 * S, r.Width - 56 * S, 32 * S), 20, cText, 0, 0, 1, FONT_SEMI);
        std::wstring sub = m.online ? T(L"Работает ") + fmtUptime(m.uptime) + L"  ·  v" + m.version : std::wstring(T(L"Нажми «Запустить»"));
        text(g, sub, RectF(r.X + 18 * S, r.Y + 78 * S, r.Width - 36 * S, 20 * S), 12.5f, cMuted);
        float bw = (r.Width - 32 * S - 8 * S) / 2, by = r.Y + ch - 54 * S;
        if (!m.online) button(g, ID_START, RectF(r.X + 16 * S, by, r.Width - 32 * S, 38 * S), T(L"Запустить"), Primary, 0xE768);
        else {
            button(g, ID_STOP, RectF(r.X + 16 * S, by, bw, 38 * S), T(L"Стоп"), Danger);
            button(g, ID_RESTART, RectF(r.X + 16 * S + bw + 8 * S, by, bw, 38 * S), T(L"Перезапуск"), Secondary);
        }
    }
    // --- Mini App address
    {
        RectF r(cw + gap, y, cw, ch);
        card(g, r);
        cardLabel(g, r, T(L"АДРЕС MINI APP"));
        bool up = m.online && !m.tunnelUrl.empty();
        Color c = up ? cGreen : (m.online ? cAmber : cMuted);
        statusDot(g, r.X + 26 * S, r.Y + 58 * S, 5.5f * S, c, false, t);
        std::wstring ts = !m.online ? std::wstring(T(L"Нет данных")) : up ? std::wstring(T(L"В сети")) : cap(tunnelStateText(m));
        text(g, ts, RectF(r.X + 42 * S, r.Y + 42 * S, r.Width - 56 * S, 32 * S), 20, cText, 0, 0, 1, FONT_SEMI);
        std::wstring url = m.tunnelUrl;
        if (url.rfind(L"https://", 0) == 0) url = url.substr(8);
        text(g, up ? url : (m.tunnelState == L"error" && !m.tunnelError.empty() ? m.tunnelError : std::wstring(T(L"Адрес появится после подключения"))),
             RectF(r.X + 18 * S, r.Y + 78 * S, r.Width - 36 * S, 20 * S), 12.5f, cMuted);
        float bw = (r.Width - 32 * S - 8 * S) / 2, by = r.Y + ch - 54 * S;
        button(g, ID_TUN_COPY, RectF(r.X + 16 * S, by, bw, 38 * S), T(L"Копировать"), Secondary, 0, up);
        button(g, ID_TUN_RESTART, RectF(r.X + 16 * S + bw + 8 * S, by, bw, 38 * S), T(L"Заново"), Secondary, 0, m.online && m.configured);
    }
    // --- Telegram
    {
        RectF r((cw + gap) * 2, y, cw, ch);
        card(g, r);
        cardLabel(g, r, L"TELEGRAM");
        bool has = !m.botUser.empty();
        statusDot(g, r.X + 26 * S, r.Y + 58 * S, 5.5f * S, has && m.botState == L"online" ? cGreen : (m.online ? cAmber : cMuted), false, t);
        text(g, has ? L"@" + m.botUser : L"—", RectF(r.X + 42 * S, r.Y + 42 * S, r.Width - 56 * S, 32 * S), 20, cText, 0, 0, 1, FONT_SEMI);
        std::wstring bs = m.online ? botStateText(m.botState) : std::wstring(T(L"бот остановлен"));
        text(g, T(L"Связь: ") + cap(bs), RectF(r.X + 18 * S, r.Y + 78 * S, r.Width - 36 * S, 20 * S), 12.5f, cMuted);
        button(g, ID_OPEN_BOT, RectF(r.X + 16 * S, r.Y + ch - 54 * S, r.Width - 32 * S, 38 * S), T(L"Открыть бота"), Secondary, 0xE8A7, has);
    }

    // ---------------------------------------------------------------- second row
    y += ch + gap;
    float ch2 = 304 * S;
    float lw = std::min(W_ * 0.5f, 560 * S);
    {
        RectF r(0, y, lw, ch2);
        card(g, r, true);
        text(g, T(L"Защита"), RectF(r.X + 20 * S, r.Y + 16 * S, r.Width - 40 * S, 26 * S), 17, cText, 0, 0, 1, FONT_SEMI);
        int owners = 0;
        for (auto& u : m.users) if (u.role == L"owner") owners++;
        struct Row { wchar_t glyph; std::wstring title, sub; Color c; };
        std::vector<Row> rows = {
            {0xE716, fmt(T(L"Доступ есть у %d чел."), (int)m.users.size()),
             m.users.empty() ? std::wstring(T(L"Добавь себя в разделе «Пользователи»")) : fmt(T(L"Владельцев: %d · чужим бот не отвечает"), owners),
             m.users.empty() ? cAmber : cGreen},
            {0xE8D7, T(L"Каждый запрос проверяется подписью Telegram"), T(L"Токен бота зашифрован средствами Windows"), cGreen},
            {0xE7BA, T(L"Защита от подбора"), m.blockedIps ? fmt(T(L"Заблокировано адресов: %d"), m.blockedIps) : std::wstring(T(L"8 неудачных попыток — блок на 15 минут")), m.blockedIps ? cAmber : cGreen},
            {0xE7B3, T(L"Плашка трансляции включена всегда"), T(L"Видна на ПК, пока кто-то смотрит экран"), cGreen},
        };
        for (size_t i = 0; i < rows.size(); i++) {
            float ry = r.Y + 56 * S + i * 58 * S;
            fillRR(g, RectF(r.X + 20 * S, ry, 40 * S, 40 * S), 12 * S, alpha(rows[i].c, 34));
            icon(g, rows[i].glyph, RectF(r.X + 20 * S, ry, 40 * S, 40 * S), 17, rows[i].c);
            text(g, rows[i].title, RectF(r.X + 72 * S, ry + 1 * S, r.Width - 92 * S, 21 * S), 14, cText, 0, 0, 1, FONT_SEMI);
            text(g, rows[i].sub, RectF(r.X + 72 * S, ry + 22 * S, r.Width - 92 * S, 19 * S), 12.5f, cMuted);
            if (i == 0) addHit(ID_HOME_USERS, RectF(r.X + 12 * S, ry - 4 * S, r.Width - 24 * S, 48 * S));
        }
    }
    {
        RectF r(lw + gap, y, W_ - lw - gap, ch2);
        card(g, r);
        text(g, T(L"Нагрузка ПК"), RectF(r.X + 20 * S, r.Y + 16 * S, r.Width - 40 * S, 26 * S), 17, cText, 0, 0, 1, FONT_SEMI);
        float ix = r.X + 20 * S, iw = r.Width - 40 * S;
        text(g, T(L"Процессор"), RectF(ix, r.Y + 54 * S, iw, 20 * S), 13, cMuted);
        text(g, m.online ? fmt(L"%.0f%%", m.cpu) : L"—", RectF(ix, r.Y + 54 * S, iw, 20 * S), 13, cText, 0, 2, 1, FONT_SEMI);
        bar(g, RectF(ix, r.Y + 78 * S, iw, 8 * S), (float)m.cpu / 100.f, cAcc1, cAcc2);
        spark(g, RectF(ix, r.Y + 94 * S, iw, 38 * S), m.cpuHist, cAcc2);
        text(g, T(L"Память"), RectF(ix, r.Y + 142 * S, iw, 20 * S), 13, cMuted);
        text(g, m.online ? fmt(T(L"%.0f%%  ·  %ls из %ls"), m.ram, fmtBytes(m.ramUsed).c_str(), fmtBytes(m.ramTotal).c_str()) : L"—",
             RectF(ix, r.Y + 142 * S, iw, 20 * S), 13, cText, 0, 2, 1, FONT_SEMI);
        bar(g, RectF(ix, r.Y + 166 * S, iw, 8 * S), (float)m.ram / 100.f, Color(255, 236, 72, 153), cAcc1);
        // today
        const wchar_t* lab[] = {T(L"События"), T(L"Трансляции"), T(L"Отказы")};
        int val[] = {m.statToday, m.statStreams, m.statDenied};
        Color col[] = {cText, cAcc2, m.statDenied ? cRed : cText};
        float bw = (iw - 16 * S) / 3, by = r.Y + 196 * S;
        for (int i = 0; i < 3; i++) {
            RectF bx(ix + i * (bw + 8 * S), by, bw, 84 * S);
            fillRR(g, bx, 12 * S, cCard2);
            text(g, std::to_wstring(val[i]), RectF(bx.X, bx.Y + 12 * S, bx.Width, 36 * S), 26, col[i], 0, 1, 1, FONT_SEMI);
            text(g, lab[i], RectF(bx.X, bx.Y + 50 * S, bx.Width, 22 * S), 12, cMuted, 0, 1, 1);
        }
    }
    return y + ch2 + 40 * S;
}

static float pageStreams(Graphics& g, float W_, double t) {
    Model& m = g_m;
    float y = pageTitle(g, W_, T(L"Трансляции"), T(L"Кто сейчас смотрит экран ПК и история подключений"));
    float rowH = 68 * S;
    // --- live now
    {
        float h = 62 * S + (m.sessions.empty() ? 96 * S : rowH * m.sessions.size());
        RectF r(0, y, W_, h);
        card(g, r, !m.sessions.empty());
        std::wstring title = T(L"Сейчас в эфире");
        text(g, title, RectF(r.X + 20 * S, r.Y + 16 * S, 300 * S, 28 * S), 17, cText, 0, 0, 1, FONT_SEMI);
        if (!m.sessions.empty()) {
            float bx = r.X + 20 * S + measure(g, title, 17, FONT_SEMI) + 8 * S;
            gradRR(g, RectF(bx, r.Y + 20 * S, 26 * S, 22 * S), 11 * S, cRed, Color(255, 236, 72, 153), 0.f);
            text(g, std::to_wstring(m.sessions.size()), RectF(bx, r.Y + 20 * S, 26 * S, 22 * S), 12.5f, Color(255, 255, 255, 255), 0, 1, 1, FONT_SEMI);
            if (m.sessions.size() > 1)
                button(g, ID_KICK_ALL, RectF(r.X + r.Width - 170 * S, r.Y + 12 * S, 154 * S, 36 * S), T(L"Отключить всех"), Danger);
        }
        if (m.sessions.empty()) {
            icon(g, 0xE7F4, RectF(r.X, r.Y + 52 * S, r.Width, 30 * S), 24, alpha(cMuted, 140));
            text(g, m.online ? T(L"Сейчас никто не смотрит экран") : T(L"Бот остановлен"), RectF(r.X, r.Y + 84 * S, r.Width, 24 * S), 14, cMuted, 0, 1, 1);
        }
        ULONGLONG extra = (GetTickCount64() - m.sessTick) / 1000;
        for (size_t i = 0; i < m.sessions.size(); i++) {
            const Sess& s = m.sessions[i];
            float ry = r.Y + 56 * S + i * rowH;
            if (i > 0) { SolidBrush lb(alpha(cBorder, 160)); g.FillRectangle(&lb, RectF(r.X + 20 * S, ry, r.Width - 40 * S, 1)); }
            std::wstring first = s.who.empty() ? L"?" : s.who.substr(0, 1);
            avatar(g, r.X + 44 * S, ry + rowH / 2, 20 * S, first, cAcc1, cAcc2);
            const std::wstring& name = s.who;
            auto dot = name.find(L" · ");
            std::wstring who1 = dot == std::wstring::npos ? name : name.substr(0, dot);
            std::wstring who2 = dot == std::wstring::npos ? L"" : name.substr(dot + 3);
            text(g, who1, RectF(r.X + 78 * S, ry + 10 * S, r.Width - 330 * S, 26 * S), 15, cText, 0, 0, 1, FONT_SEMI);
            text(g, who2, RectF(r.X + 78 * S, ry + 36 * S, r.Width - 330 * S, 22 * S), 12.5f, cMuted);
            statusDot(g, r.X + r.Width - 262 * S, ry + rowH / 2, 4.5f * S, cRed, true, t);
            text(g, fmtDur(s.seconds + (int)extra), RectF(r.X + r.Width - 250 * S, ry, 90 * S, rowH), 16, cText, 0, 0, 1, FONT_SEMI);
            button(g, ID_KICK + (int)i, RectF(r.X + r.Width - 144 * S, ry + 15 * S, 124 * S, 38 * S), T(L"Отключить"), Danger, 0xE711);
        }
        y += h + 16 * S;
    }
    // --- history
    {
        std::vector<const LogItem*> items;
        for (size_t i = m.log.size(); i-- > 0 && items.size() < 40;)
            if (m.log[i].cat == L"stream") items.push_back(&m.log[i]);
        float rh = 56 * S;
        float h = 62 * S + (items.empty() ? 80 * S : rh * items.size()) + 8 * S;
        RectF r(0, y, W_, h);
        card(g, r);
        text(g, T(L"История трансляций"), RectF(r.X + 20 * S, r.Y + 16 * S, 400 * S, 28 * S), 17, cText, 0, 0, 1, FONT_SEMI);
        if (items.empty()) text(g, T(L"Пока пусто"), RectF(r.X, r.Y + 70 * S, r.Width, 30 * S), 14, cMuted, 0, 1, 1);
        float visTop = viewClip.Y - off.Y, visBot = viewClip.Y + viewClip.Height - off.Y;
        for (size_t i = 0; i < items.size(); i++) {
            float ry = r.Y + 56 * S + i * rh;
            if (ry + rh < visTop || ry > visBot) continue;
            const LogItem& it = *items[i];
            bool start = it.ev == L"start";
            circle(g, r.X + 32 * S, ry + rh / 2, 5 * S, start ? cGreen : cMuted);
            text(g, it.action + (it.user.empty() ? L"" : L" — " + it.user.substr(0, it.user.find(L" (@"))), RectF(r.X + 52 * S, ry + 6 * S, r.Width - 260 * S, 24 * S), 14, cText, 0, 0, 1, FONT_SEMI);
            text(g, it.detail + (it.device.empty() ? L"" : L"  ·  " + it.device) + (it.ip.empty() ? L"" : L"  ·  " + it.ip), RectF(r.X + 52 * S, ry + 29 * S, r.Width - 260 * S, 20 * S), 12.5f, cMuted);
            text(g, it.time, RectF(r.X + r.Width - 200 * S, ry, 180 * S, rh), 12.5f, cMuted, 0, 2, 1);
        }
        y += h;
    }
    return y + 40 * S;
}

static float pageLog(Graphics& g, float W_, double t) {
    Model& m = g_m;
    float y = pageTitle(g, W_, T(L"Журнал"), T(L"Кто и что делал: входы, управление, файлы, отказы"));
    float cx = 0;
    for (int i = 0; i < 8; i++) {
        std::wstring name = T(CHIP_NAMES[i]);
        float w = measure(g, name, 12.5f, FONT_SEMI) + 30 * S;
        chip(g, ID_CHIP + i, RectF(cx, y, w, 32 * S), name, g_logFilter == i);
        cx += w + 8 * S;
    }
    button(g, ID_CSV, RectF(W_ - 110 * S, 6 * S, 110 * S, 38 * S), L"CSV", Secondary, 0xE896);
    button(g, ID_OPEN_WEBLOG, RectF(W_ - 110 * S - 8 * S - 170 * S, 6 * S, 170 * S, 38 * S), T(L"Веб-журнал"), Secondary, 0xE774);
    y += 46 * S;

    float visTop = viewClip.Y - off.Y, visBot = viewClip.Y + viewClip.Height - off.Y;
    if (g_logFilter == 7) {
        float lh = 19 * S;
        float h = std::max(80 * S, lh * m.syslog.size() + 24 * S);
        RectF r(0, y, W_, h);
        card(g, r);
        if (m.syslog.empty()) text(g, m.online ? T(L"Загружаю…") : T(L"Бот остановлен"), r, 14, cMuted, 0, 1, 1);
        for (size_t i = 0; i < m.syslog.size(); i++) {
            float ry = r.Y + 12 * S + i * lh;
            if (ry + lh < visTop || ry > visBot) continue;
            const std::wstring& ln = m.syslog[i];
            Color c = ln.find(L" ERROR ") != std::wstring::npos ? cRed : ln.find(L" WARNING ") != std::wstring::npos ? cAmber : cMuted;
            text(g, ln, RectF(r.X + 16 * S, ry, r.Width - 32 * S, lh), 12, c, 0, 0, 1, FONT_MONO);
        }
        return y + h + 40 * S;
    }
    std::vector<const LogItem*> items;
    for (size_t i = m.log.size(); i-- > 0;)
        if (catMatches(m.log[i].cat, g_logFilter)) items.push_back(&m.log[i]);
    float rh = 74 * S;
    float h = items.empty() ? 100 * S : rh * items.size() + 8 * S;
    RectF r(0, y, W_, h);
    card(g, r);
    if (items.empty()) text(g, T(L"Записей нет"), r, 14, cMuted, 0, 1, 1);
    for (size_t i = 0; i < items.size(); i++) {
        float ry = r.Y + 4 * S + i * rh;
        if (ry + rh < visTop || ry > visBot) continue;
        const LogItem& it = *items[i];
        Color c = catColor(it.cat);
        bool deny = it.cat == L"deny";
        if (deny) fillRR(g, RectF(r.X + 6 * S, ry + 3 * S, r.Width - 12 * S, rh - 6 * S), 10 * S, alpha(cRed, 22));
        fillRR(g, RectF(r.X + 18 * S, ry + 12 * S, 4 * S, rh - 24 * S), 2 * S, c);
        text(g, it.action, RectF(r.X + 34 * S, ry + 8 * S, r.Width - 260 * S, 24 * S), 14, deny ? Color(255, 255, 150, 165) : cText, 0, 0, 1, FONT_SEMI);
        std::wstring meta = it.user;
        if (!it.ip.empty()) meta += (meta.empty() ? L"" : L"  ·  ") + it.ip;
        if (!it.place.empty()) meta += L"  ·  " + it.place;
        if (!it.device.empty()) meta += L"  ·  " + it.device;
        const std::wstring& line2 = it.detail;
        text(g, line2.empty() ? meta : line2, RectF(r.X + 34 * S, ry + 32 * S, r.Width - 260 * S, 20 * S), 12.5f, cMuted);
        if (!line2.empty() && !meta.empty()) text(g, meta, RectF(r.X + 34 * S, ry + 52 * S, r.Width - 260 * S, 18 * S), 11.5f, alpha(cMuted, 170));
        text(g, it.time, RectF(r.X + r.Width - 210 * S, ry + 6 * S, 190 * S, 24 * S), 12.5f, cMuted, 0, 2, 1);
    }
    return y + h + 40 * S;
}

// ---------------------------------------------------------------------------- pairing card (users page and wizard)
static float pairingCode(Graphics& g, RectF r, float y, bool wizard) {
    Model& m = g_m;
    int left = m.pairExpires - (int)((GetTickCount64() - m.pairTick) / 1000);
    if (left < 0) left = 0;
    // code digits
    float dw = 54 * S, dh = 68 * S, dg = 10 * S;
    float total = dw * 6 + dg * 5;
    float x0 = r.X + (r.Width - total) / 2;
    for (int i = 0; i < 6 && i < (int)m.pairCode.size(); i++) {
        RectF d(x0 + i * (dw + dg), y, dw, dh);
        fillRR(g, d, 14 * S, cCard2);
        strokeRR(g, d, 14 * S, alpha(cAcc1, 150));
        text(g, m.pairCode.substr(i, 1), d, 30, cText, 0, 1, 1, FONT_SEMI);
    }
    y += dh + 14 * S;
    text(g, fmt(T(L"Код действует ещё %d:%02d · одноразовый"), left / 60, left % 60), RectF(r.X, y, r.Width, 20 * S), 13, left < 60 ? cAmber : cMuted, 0, 1, 1);
    y += 30 * S;
    std::wstring how = m.pairLink.empty()
        ? fmt(T(L"Отправь боту команду /pair %ls"), m.pairCode.c_str())
        : fmt(T(L"Открой ссылку на телефоне или отправь боту: /pair %ls"), m.pairCode.c_str());
    text(g, how, RectF(r.X + 20 * S, y, r.Width - 40 * S, 22 * S), 14, cText, 0, 1, 1);
    y += 34 * S;
    float bw = 190 * S, gap = 10 * S;
    int n = wizard ? 2 : 3;
    float bx = r.X + (r.Width - (bw * n + gap * (n - 1))) / 2;
    button(g, ID_PAIR_OPEN, RectF(bx, y, bw, 40 * S), T(L"Открыть в Telegram"), Primary, 0xE8A7, !m.pairLink.empty());
    button(g, ID_PAIR_COPY, RectF(bx + bw + gap, y, bw, 40 * S), T(L"Скопировать ссылку"), Secondary, 0xE8C8, !m.pairLink.empty());
    if (!wizard) button(g, ID_PAIR_CANCEL, RectF(bx + (bw + gap) * 2, y, bw, 40 * S), T(L"Отменить"), Danger);
    return y + 40 * S;
}

static float pageUsers(Graphics& g, float W_, double t) {
    Model& m = g_m;
    float y = pageTitle(g, W_, T(L"Пользователи"), T(L"Кто может управлять этим ПК через Telegram. Добавлять и удалять можно только здесь"));
    float pad = 22 * S;
    // --- add a user
    {
        bool active = m.pairActive;
        float h = active ? 330 * S : (g_newRole == 1 ? 262 * S : 176 * S);
        RectF r(0, y, W_, h);
        card(g, r, active);
        text(g, T(L"Добавить пользователя"), RectF(r.X + pad, r.Y + 16 * S, 500 * S, 28 * S), 17, cText, 0, 0, 1, FONT_SEMI);
        if (!m.online || !m.configured) {
            text(g, !m.online ? T(L"Бот остановлен — запусти его на странице «Обзор»") : T(L"Сначала задай токен бота в настройках"),
                 RectF(r.X + pad, r.Y + 56 * S, r.Width - pad * 2, 22 * S), 13.5f, cMuted);
        } else if (active) {
            std::wstring role = m.pairRole == L"owner" ? std::wstring(T(L"владелец")) : std::wstring(T(L"пользователь"));
            text(g, fmt(T(L"Ждём, когда человек отправит код боту · роль: %ls"), role.c_str()), RectF(r.X + pad, r.Y + 46 * S, r.Width - pad * 2, 22 * S), 13, cMuted);
            pairingCode(g, r, r.Y + 90 * S, false);
        } else {
            settingRow(g, r, r.Y + 56 * S, T(L"Права нового пользователя"), T(L"Выданные права можно поменять в любой момент"), 420);
            segmented(g, ID_PAIR_ROLE, RectF(r.X + r.Width - pad - 380 * S, r.Y + 58 * S, 380 * S, 40 * S), {T(L"Полный доступ"), T(L"Выбранные функции")}, g_newRole);
            float yy = r.Y + 116 * S;
            if (g_newRole == 1) {
                float cx = r.X + pad;
                for (int i = 0; i < PERM_N; i++) {
                    std::wstring name = permTitle(i);
                    float w = measure(g, name, 12.5f, FONT_SEMI) + 30 * S;
                    if (cx + w > r.X + r.Width - pad) { cx = r.X + pad; yy += 40 * S; }
                    chip(g, ID_PAIR_PERM + i, RectF(cx, yy, w, 32 * S), name, g_newPerms.count(PERMS[i]) > 0);
                    cx += w + 8 * S;
                }
                yy += 52 * S;
            }
            text(g, T(L"Появится одноразовый код на 5 минут. Человек отправляет его твоему боту — и получает доступ"),
                 RectF(r.X + pad, yy + 8 * S, r.Width - 260 * S, 22 * S), 12.5f, cMuted);
            button(g, ID_PAIR_START, RectF(r.X + r.Width - pad - 200 * S, yy, 200 * S, 40 * S), T(L"Создать код"), Primary, 0xE710);
        }
        y += h + 16 * S;
    }
    // --- users
    {
        float rowH = 76 * S;
        float h = 62 * S + (m.users.empty() ? 90 * S : 0);
        for (auto& u : m.users) h += rowH + (u.id == g_expandedUser ? 190 * S : 0);
        RectF r(0, y, W_, h + 8 * S);
        card(g, r);
        text(g, fmt(T(L"Есть доступ (%d)"), (int)m.users.size()), RectF(r.X + pad, r.Y + 16 * S, 400 * S, 28 * S), 17, cText, 0, 0, 1, FONT_SEMI);
        if (m.users.empty()) {
            icon(g, 0xE716, RectF(r.X, r.Y + 54 * S, r.Width, 30 * S), 24, alpha(cMuted, 140));
            text(g, T(L"Пока никого нет — бот никому не отвечает"), RectF(r.X, r.Y + 86 * S, r.Width, 24 * S), 14, cMuted, 0, 1, 1);
        }
        float ry = r.Y + 56 * S;
        for (size_t i = 0; i < m.users.size(); i++) {
            const UserRec& u = m.users[i];
            bool owner = u.role == L"owner";
            bool open = u.id == g_expandedUser;
            if (i > 0) { SolidBrush lb(alpha(cBorder, 160)); g.FillRectangle(&lb, RectF(r.X + 20 * S, ry, r.Width - 40 * S, 1)); }
            std::wstring first = u.name.empty() ? L"?" : u.name.substr(0, 1);
            avatar(g, r.X + 46 * S, ry + rowH / 2, 21 * S, first, owner ? cAcc1 : Color(255, 60, 150, 225), cAcc2);
            float nx = r.X + 82 * S;
            std::wstring nm = u.name.empty() ? std::to_wstring(u.id) : u.name;
            text(g, nm, RectF(nx, ry + 12 * S, r.Width - 520 * S, 26 * S), 15, cText, 0, 0, 1, FONT_SEMI);
            float bx = nx + std::min(measure(g, nm, 15, FONT_SEMI), r.Width - 520 * S) + 10 * S;
            badge(g, bx, ry + 13 * S, owner ? T(L"Владелец") : T(L"Пользователь"), owner ? cAcc2 : cMuted, 22);
            std::wstring sub = (u.username.empty() ? L"" : L"@" + u.username + L"  ·  ") + L"ID " + std::to_wstring(u.id);
            if (!u.lastSeen.empty()) sub += T(L"  ·  был(а) ") + u.lastSeen;
            else if (!u.added.empty()) sub += T(L"  ·  добавлен(а) ") + u.added;
            text(g, sub, RectF(nx, ry + 40 * S, r.Width - 420 * S, 22 * S), 12.5f, cMuted);
            // right side: permissions summary + buttons
            std::wstring perms;
            if (owner) perms = T(L"Все функции");
            else {
                int n = 0;
                for (int k = 0; k < PERM_N; k++) if (u.perms.count(PERMS[k])) n++;
                perms = n == 0 ? std::wstring(T(L"Только статус")) : fmt(T(L"Функций: %d из %d"), n, PERM_N);
            }
            text(g, perms, RectF(r.X + r.Width - 400 * S, ry, 120 * S, rowH), 12.5f, cMuted, 0, 2, 1);
            button(g, ID_USER_EXPAND + (int)i, RectF(r.X + r.Width - 268 * S, ry + 19 * S, 120 * S, 38 * S), open ? T(L"Скрыть") : T(L"Права"), Secondary, 0xE70F);
            button(g, ID_USER_DEL + (int)i, RectF(r.X + r.Width - 140 * S, ry + 19 * S, 120 * S, 38 * S), T(L"Удалить"), Danger, 0xE74D);
            ry += rowH;
            if (open) {
                RectF box(r.X + 20 * S, ry, r.Width - 40 * S, 178 * S);
                fillRR(g, box, 14 * S, cCard2);
                text(g, T(L"Роль"), RectF(box.X + 18 * S, box.Y + 14 * S, 300 * S, 22 * S), 14, cText, 0, 0, 1, FONT_SEMI);
                text(g, T(L"Владелец получает все функции и уведомления о запуске ПК"), RectF(box.X + 18 * S, box.Y + 36 * S, box.Width - 380 * S, 20 * S), 12.5f, cMuted);
                segmented(g, ID_USER_ROLE + (int)i * 1000, RectF(box.X + box.Width - 18 * S - 320 * S, box.Y + 14 * S, 320 * S, 38 * S), {T(L"Пользователь"), T(L"Владелец")}, owner ? 1 : 0);
                text(g, T(L"Функции"), RectF(box.X + 18 * S, box.Y + 74 * S, 300 * S, 22 * S), 14, cText, 0, 0, 1, FONT_SEMI);
                float cx = box.X + 18 * S, cy = box.Y + 104 * S;
                for (int k = 0; k < PERM_N; k++) {
                    std::wstring name = permTitle(k);
                    float w = measure(g, name, 12.5f, FONT_SEMI) + 30 * S;
                    if (cx + w > box.X + box.Width - 18 * S) { cx = box.X + 18 * S; cy += 38 * S; }
                    bool on = owner || u.perms.count(PERMS[k]);
                    if (owner) {
                        fillRR(g, RectF(cx, cy, w, 32 * S), 16 * S, alpha(cAcc1, 120));
                        text(g, name, RectF(cx, cy, w, 32 * S), 12.5f, Color(200, 255, 255, 255), 0, 1, 1, FONT_SEMI);
                    } else chip(g, ID_USER_PERM + (int)i * 1000 + k, RectF(cx, cy, w, 32 * S), name, on);
                    cx += w + 8 * S;
                }
                ry += 190 * S;
            }
        }
        y += r.Height;
    }
    return y + 40 * S;
}

static float pageFeatures(Graphics& g, float W_, double t) {
    Model& m = g_m;
    float y = pageTitle(g, W_, T(L"Функции"), T(L"Что вообще разрешено делать с этим ПК через Telegram"));
    float pad = 22 * S, rowH = 62 * S;
    {
        RectF r(0, y, W_, 70 * S + rowH * PERM_N);
        card(g, r);
        text(g, T(L"Возможности бота"), RectF(r.X + pad, r.Y + 16 * S, 500 * S, 28 * S), 17, cText, 0, 0, 1, FONT_SEMI);
        text(g, T(L"Выключенная функция недоступна никому — даже владельцам"), RectF(r.X + pad, r.Y + 42 * S, r.Width - pad * 2, 20 * S), 12.5f, cMuted);
        for (int i = 0; i < PERM_N; i++) {
            float ry = r.Y + 70 * S + i * rowH;
            if (i > 0) { SolidBrush lb(alpha(cBorder, 120)); g.FillRectangle(&lb, RectF(r.X + pad, ry, r.Width - pad * 2, 1)); }
            bool on = m.features.count(PERMS[i]) ? m.features[PERMS[i]] : true;
            fillRR(g, RectF(r.X + pad, ry + 11 * S, 40 * S, 40 * S), 12 * S, alpha(on ? cAcc2 : cMuted, 34));
            icon(g, PERM_ICONS[i], RectF(r.X + pad, ry + 11 * S, 40 * S, 40 * S), 17, on ? cAcc2 : cMuted);
            text(g, permTitle(i), RectF(r.X + pad + 54 * S, ry + 10 * S, r.Width - 200 * S, 22 * S), 14.5f, cText, 0, 0, 1, FONT_SEMI);
            text(g, permDesc(i), RectF(r.X + pad + 54 * S, ry + 32 * S, r.Width - 200 * S, 20 * S), 12.5f, cMuted);
            toggle(g, ID_FEAT + i, r.X + r.Width - pad - 44 * S, ry + 19 * S, on);
        }
        y += r.Height + 16 * S;
    }
    {
        RectF r(0, y, W_, 192 * S);
        card(g, r);
        text(g, T(L"Всегда включено"), RectF(r.X + pad, r.Y + 16 * S, 500 * S, 28 * S), 17, cText, 0, 0, 1, FONT_SEMI);
        settingRow(g, r, r.Y + 58 * S, T(L"Плашка «идёт трансляция» на ПК"), T(L"Видна в углу экрана, пока кто-то смотрит. Скрыть нельзя — так TgDesk нельзя использовать для слежки"), 200);
        lockedPill(g, RectF(r.X + r.Width - pad - 110 * S, r.Y + 62 * S, 110 * S, 30 * S), T(L"Всегда"));
        settingRow(g, r, r.Y + 118 * S, T(L"Журнал доступа и иконка в трее"), T(L"Каждое действие записывается, иконка TgDesk видна в трее, пока работает приложение"), 200);
        lockedPill(g, RectF(r.X + r.Width - pad - 110 * S, r.Y + 122 * S, 110 * S, 30 * S), T(L"Всегда"));
        y += r.Height;
    }
    return y + 40 * S;
}

static float pageSettings(Graphics& g, float W_, double t) {
    Model& m = g_m;
    float y = pageTitle(g, W_, T(L"Настройки"), T(L"Язык, трансляция, запуск, бот и адрес Mini App"));
    float pad = 22 * S;
    // --- language
    {
        RectF r(0, y, W_, 112 * S);
        card(g, r);
        settingRow(g, r, r.Y + 22 * S, T(L"Язык"), T(L"Язык приложения, журнала и плашки. В Telegram у каждого — язык его клиента"), 420);
        int sel = g_uiLang == L"ru" ? 1 : g_uiLang == L"en" ? 2 : 0;
        segmented(g, ID_SET_LANG, RectF(r.X + r.Width - pad - 360 * S, r.Y + 26 * S, 360 * S, 40 * S), {T(L"Как в Windows"), L"Русский", L"English"}, sel);
        y += r.Height + 16 * S;
    }
    // --- streaming
    {
        RectF r(0, y, W_, 192 * S);
        card(g, r);
        text(g, T(L"Трансляция на телефон"), RectF(r.X + pad, r.Y + 16 * S, 400 * S, 28 * S), 17, cText, 0, 0, 1, FONT_SEMI);
        settingRow(g, r, r.Y + 62 * S, T(L"Качество по умолчанию"), T(L"«Авто» подстраивается под скорость мобильной сети"), 420);
        int sel = m.quality == L"eco" ? 0 : m.quality == L"hd" ? 2 : m.quality == L"auto" ? 3 : 1;
        segmented(g, ID_QUAL, RectF(r.X + r.Width - pad - 380 * S, r.Y + 60 * S, 380 * S, 40 * S), {T(L"Эконом"), T(L"Норма"), T(L"Чётко"), T(L"Авто")}, sel);
        settingRow(g, r, r.Y + 124 * S, T(L"Плашка «идёт трансляция» на ПК"), T(L"Видна в углу экрана, пока кто-то смотрит; в самой трансляции её нет. Отключить нельзя"), 200);
        lockedPill(g, RectF(r.X + r.Width - pad - 110 * S, r.Y + 126 * S, 110 * S, 30 * S), T(L"Всегда"));
        y += r.Height + 16 * S;
    }
    // --- startup
    {
        RectF r(0, y, W_, 172 * S);
        card(g, r);
        text(g, T(L"Запуск"), RectF(r.X + pad, r.Y + 16 * S, 400 * S, 28 * S), 17, cText, 0, 0, 1, FONT_SEMI);
        settingRow(g, r, r.Y + 56 * S, T(L"Запускать вместе с Windows"), T(L"Приложение стартует свёрнутым в трей и поднимает бота"));
        toggle(g, ID_TG_AUTOSTART, r.X + r.Width - pad - 44 * S, r.Y + 62 * S, m.autostart);
        settingRow(g, r, r.Y + 112 * S, T(L"Сворачивать в трей при закрытии окна"), T(L"Бот продолжит работать, пока приложение в трее"));
        toggle(g, ID_TG_TRAY, r.X + r.Width - pad - 44 * S, r.Y + 118 * S, g_closeToTray);
        y += r.Height + 16 * S;
    }
    // --- bot
    {
        RectF r(0, y, W_, 128 * S);
        card(g, r);
        text(g, T(L"Бот Telegram"), RectF(r.X + pad, r.Y + 16 * S, 400 * S, 28 * S), 17, cText, 0, 0, 1, FONT_SEMI);
        std::wstring who = m.botUser.empty() ? std::wstring(T(L"Токен не задан")) : L"@" + m.botUser + L"  ·  " + botStateText(m.botState);
        settingRow(g, r, r.Y + 56 * S, who, T(L"Токен хранится зашифрованным (Windows DPAPI) и виден только этому пользователю Windows"));
        button(g, ID_TOKEN_CHANGE, RectF(r.X + r.Width - pad - 190 * S, r.Y + 60 * S, 190 * S, 40 * S), m.configured ? T(L"Сменить токен") : T(L"Задать токен"), Secondary, 0xE8D7, m.online);
        y += r.Height + 16 * S;
    }
    // --- Mini App address
    {
        int sel = g_tunSel >= 0 ? g_tunSel : (m.tunnelMode == L"cloudflare" ? 1 : m.tunnelMode == L"external" ? 2 : 0);
        float h = sel == 0 ? 186 * S : sel == 1 ? 300 * S : 240 * S;
        RectF r(0, y, W_, h);
        card(g, r);
        text(g, T(L"Адрес Mini App"), RectF(r.X + pad, r.Y + 16 * S, 400 * S, 28 * S), 17, cText, 0, 0, 1, FONT_SEMI);
        std::wstring cur = m.tunnelUrl.empty() ? std::wstring(T(L"сейчас не доступен")) : m.tunnelUrl;
        text(g, T(L"Сейчас: ") + modeTitle(m.tunnelMode) + L"  ·  " + cur, RectF(r.X + pad, r.Y + 42 * S, r.Width - pad * 2, 20 * S), 12.5f, cMuted);
        segmented(g, ID_TUN_MODE, RectF(r.X + pad, r.Y + 74 * S, std::min(r.Width - pad * 2, 560 * S), 40 * S), {L"Quick Tunnel", L"Cloudflare Tunnel", T(L"Свой адрес")}, sel);
        float yy = r.Y + 128 * S;
        const wchar_t* desc = sel == 0 ? L"Без регистрации и домена. Адрес меняется при каждом запуске — бот сам обновляет кнопку в Telegram"
                            : sel == 1 ? L"Свой туннель в Cloudflare Zero Trust: укажи токен туннеля и адрес, направленный на http://127.0.0.1:8765"
                                       : L"Любой HTTPS-адрес, который ты сам направил на http://127.0.0.1:8765 этого ПК (свой прокси, другой туннель)";
        text(g, T(desc), RectF(r.X + pad, yy, r.Width - pad * 2, 20 * S), 12.5f, cMuted);
        yy += 34 * S;
        if (sel == 1) {
            inputBox(g, IN_CFTOKEN, RectF(r.X + pad, yy, r.Width - pad * 2, 44 * S),
                     m.tunnelHasToken ? T(L"Токен туннеля сохранён — оставь пустым, чтобы не менять") : T(L"Токен туннеля (eyJ…)"), true);
            yy += 56 * S;
        }
        if (sel >= 1) {
            inputBox(g, IN_URL, RectF(r.X + pad, yy, r.Width - pad * 2 - 210 * S, 44 * S), T(L"https://tgdesk.example.com"), false);
            button(g, ID_TUN_APPLY, RectF(r.X + r.Width - pad - 196 * S, yy + 2 * S, 196 * S, 40 * S), T(L"Применить"), Primary, 0, m.online);
        } else if (m.tunnelMode != L"quick") {
            button(g, ID_TUN_APPLY, RectF(r.X + pad, yy - 4 * S, 260 * S, 40 * S), T(L"Вернуть Quick Tunnel"), Primary, 0, m.online);
        }
        y += r.Height + 16 * S;
    }
    // --- privacy & updates
    {
        RectF r(0, y, W_, 228 * S);
        card(g, r);
        text(g, T(L"Сеть и обновления"), RectF(r.X + pad, r.Y + 16 * S, 400 * S, 28 * S), 17, cText, 0, 0, 1, FONT_SEMI);
        settingRow(g, r, r.Y + 56 * S, T(L"Проверять обновления"), T(L"Раз в 12 часов смотрит GitHub Releases"));
        toggle(g, ID_UPD_CHECK, r.X + r.Width - pad - 44 * S, r.Y + 62 * S, m.updateCheck);
        settingRow(g, r, r.Y + 112 * S, T(L"Устанавливать обновления автоматически"),
                   m.portable ? T(L"В портативной версии обновления ставятся вручную")
                              : T(L"Только подписанные автором; никогда — во время трансляции экрана"));
        if (!m.portable) toggle(g, ID_AUTO_UPD, r.X + r.Width - pad - 44 * S, r.Y + 118 * S, m.autoUpdate && m.updateCheck);
        settingRow(g, r, r.Y + 168 * S, T(L"Город и провайдер по IP в журнале"), T(L"Внешние IP проверяются через сервис ipwho.is"));
        toggle(g, ID_GEOIP, r.X + r.Width - pad - 44 * S, r.Y + 174 * S, m.geoip);
        y += r.Height;
    }
    return y + 40 * S;
}

static float pageAbout(Graphics& g, float W_, double t) {
    Model& m = g_m;
    float y = 0, pad = 22 * S;
    {
        RectF r(0, y, W_, 222 * S);
        card(g, r, true);
        gradRR(g, RectF(r.X + pad, r.Y + 24 * S, 72 * S, 72 * S), 20 * S, cAcc1, cAcc2, 45.f);
        icon(g, 0xE7F4, RectF(r.X + pad, r.Y + 24 * S, 72 * S, 72 * S), 32, Color(255, 255, 255, 255));
        text(g, std::wstring(L"TgDesk ") + APP_VERSION, RectF(r.X + pad + 92 * S, r.Y + 26 * S, 500 * S, 36 * S), 26, cText, 0, 0, 1, FONT_SEMI);
        text(g, T(L"Пульт для своего ПК в Telegram: экран, мышь, клавиатура, файлы, питание"), RectF(r.X + pad + 92 * S, r.Y + 62 * S, r.Width - 140 * S, 22 * S), 13.5f, cMuted);
        std::wstring bv = m.version.empty() ? std::wstring(T(L"бот не запущен")) : T(L"бот ") + m.version;
        text(g, T(L"Приложение на C++ · ") + bv + T(L" · открытый код, лицензия MIT"), RectF(r.X + pad + 92 * S, r.Y + 86 * S, r.Width - 140 * S, 20 * S), 12.5f, cMuted);
        text(g, T(L"Автор: ") + std::wstring(APP_AUTHOR), RectF(r.X + pad + 92 * S, r.Y + 110 * S, r.Width - 140 * S, 22 * S), 13.5f, cAcc2, 0, 0, 1, FONT_SEMI);
        float bx = r.X + pad, by = r.Y + 156 * S;
        button(g, ID_ABOUT_GH, RectF(bx, by, 190 * S, 40 * S), L"GitHub", Primary, 0xE774);
        button(g, ID_ABOUT_ISSUES, RectF(bx + 200 * S, by, 230 * S, 40 * S), T(L"Сообщить о проблеме"), Secondary, 0xE7BA);
        button(g, ID_ABOUT_LICENSE, RectF(bx + 440 * S, by, 150 * S, 40 * S), T(L"Лицензия"), Secondary, 0xE8A5);
        if (api::exists(uninstallerExe()) && r.Width > 800 * S)
            button(g, ID_ABOUT_UNINSTALL, RectF(r.X + r.Width - pad - 190 * S, by, 190 * S, 40 * S), T(L"Удалить TgDesk"), Danger, 0xE74D);
        y += r.Height + 16 * S;
    }
    {
        RectF r(0, y, W_, 150 * S);
        card(g, r);
        text(g, T(L"Папки"), RectF(r.X + pad, r.Y + 16 * S, 400 * S, 28 * S), 17, cText, 0, 0, 1, FONT_SEMI);
        text(g, T(L"Данные: ") + api::dataDir, RectF(r.X + pad, r.Y + 46 * S, r.Width - pad * 2, 20 * S), 12.5f, cMuted);
        text(g, T(L"Программа: ") + api::installDir, RectF(r.X + pad, r.Y + 66 * S, r.Width - pad * 2, 20 * S), 12.5f, cMuted);
        button(g, ID_OPEN_DATA, RectF(r.X + pad, r.Y + 96 * S, 190 * S, 38 * S), T(L"Папка данных"), Secondary, 0xE8B7);
        button(g, ID_OPEN_DIR, RectF(r.X + pad + 200 * S, r.Y + 96 * S, 200 * S, 38 * S), T(L"Папка программы"), Secondary, 0xE8B7);
        button(g, ID_OPEN_BOTLOG, RectF(r.X + pad + 410 * S, r.Y + 96 * S, 230 * S, 38 * S), T(L"Технический журнал"), Secondary, 0xE8A5);
        y += r.Height + 16 * S;
    }
    {
        RectF r(0, y, W_, 128 * S);
        card(g, r);
        text(g, T(L"Честное использование"), RectF(r.X + pad, r.Y + 16 * S, 400 * S, 28 * S), 17, cText, 0, 0, 1, FONT_SEMI);
        textWrap(g, T(L"TgDesk предназначен для управления своим компьютером или компьютером, владелец которого знает об этом и согласен. Приложение видно в трее, ведёт журнал и показывает плашку во время трансляции — эти функции нельзя отключить."),
                 RectF(r.X + pad, r.Y + 50 * S, r.Width - pad * 2, 70 * S), 13, cMuted);
        y += r.Height;
    }
    return y + 40 * S;
}

// ============================================================================ setup wizard
static void wizardStart(int step) {
    g_wizard = true;
    g_wizStep = step;
    g_wizError.clear();
    g_pairRequested = false;
}

// which step the wizard should be on after the model changed
static void wizardSync() {
    Model& m = g_m;
    if (!m.online || !m.gotState) return;
    bool need = !m.configured || m.users.empty();
    if (!g_wizard && need) wizardStart(!m.configured ? 0 : 2);
    if (!g_wizard) return;
    if (g_wizStep == 2 && !m.users.empty()) g_wizStep = 3;
    if (g_wizStep == 1 && m.configured && !g_wizardForced && !g_wizBusy && m.users.empty() && g_wizError.empty()) { /* stay until the user presses Next */ }
}

static void requestOwnerPairing() {
    g_pairRequested = true;
    async([] { api::post(L"/pair/start", "{\"role\":\"owner\"}"); });
}

static void wizardCheckToken() {
    std::wstring tok = inputText(IN_TOKEN);
    if (tok.empty()) { g_wizError = T(L"Вставь токен от @BotFather"); return; }
    g_wizBusy = true;
    g_wizError.clear();
    std::string body = "{\"token\":\"" + jsonEscape(U(tok)) + "\"}";
    async([body] {
        HttpResult r = api::post(L"/setup/token", body, 30000);
        JV j;
        bool ok = r.status == 200 && parseJson(r.body, j) && j["ok"].boolean();
        std::wstring err;
        if (!ok) {
            std::string e = r.status == 200 ? j["error"].str() : "";
            err = e == "format" ? T(L"Это не похоже на токен. Он выглядит так: 123456789:AAE…")
                : e == "invalid" ? T(L"Telegram не принял токен. Проверь, что скопировал его целиком")
                : e == "network" ? T(L"Нет связи с Telegram. Проверь интернет (если Telegram заблокирован — включи VPN) и попробуй ещё раз")
                                 : T(L"Бот не отвечает. Нажми «Запустить» и попробуй ещё раз");
        }
        {
            std::lock_guard<std::mutex> l(g_mx);
            g_wizBusy = false;
            g_wizError = err;
            if (ok) {
                g_m.configured = true;
                g_m.botUser = W(j["username"].str());
                bool hasUsers = !g_m.users.empty();
                if (g_wizardForced && hasUsers) { g_wizard = false; g_wizardForced = false; }
                else { g_wizardForced = false; g_wizStep = hasUsers ? 3 : 2; g_pairRequested = false; }
            }
        }
        if (ok) toast(T(L"Токен сохранён, бот подключается"));
        PostMessage(g_hwnd, WM_APP_MODEL, 0, 0);
    });
}

static void drawWizard(Graphics& g, int Wp, int Hp, double t) {
    Model& m = g_m;
    off = PointF(0, 0);
    viewClip = RectF(0, 0, (float)Wp, (float)Hp);
    // soft glow
    gradRR(g, RectF(-200 * S, -200 * S, Wp * 0.7f, Hp * 0.6f), 400 * S, alpha(cAcc1, 30), alpha(cBg, 0), 45.f);
    float cw = std::min((float)Wp - 80 * S, 760 * S), ch = std::min((float)Hp - 60 * S, 600 * S);
    RectF c((Wp - cw) / 2, (Hp - ch) / 2, cw, ch);
    card(g, c, true);
    float pad = 40 * S, x = c.X + pad, w = c.Width - pad * 2;
    // header
    gradRR(g, RectF(x, c.Y + 32 * S, 44 * S, 44 * S), 13 * S, cAcc1, cAcc2, 45.f);
    icon(g, 0xE7F4, RectF(x, c.Y + 32 * S, 44 * S, 44 * S), 20, Color(255, 255, 255, 255));
    text(g, L"TgDesk", RectF(x + 58 * S, c.Y + 30 * S, 300 * S, 26 * S), 19, cText, 0, 0, 1, FONT_SEMI);
    text(g, T(L"Первая настройка"), RectF(x + 58 * S, c.Y + 55 * S, 300 * S, 20 * S), 12.5f, cMuted);
    // steps
    const wchar_t* steps[] = {L"Начало", L"Бот", L"Привязка", L"Функции"};
    for (int i = 3; i >= 0; i--) {
        float sx = c.X + c.Width - pad - (4 - i) * 92 * S;
        bool done = i < g_wizStep, cur = i == g_wizStep;
        Color col = cur ? cAcc2 : done ? cGreen : alpha(cMuted, 160);
        fillRR(g, RectF(sx, c.Y + 40 * S, 84 * S, 4 * S), 2 * S, alpha(col, cur || done ? 255 : 80));
        text(g, T(steps[i]), RectF(sx, c.Y + 50 * S, 84 * S, 20 * S), 12, col, 0, 0, 1, FONT_SEMI);
    }
    float y = c.Y + 110 * S;
    float by = c.Y + c.Height - 72 * S;

    if (!m.online) {
        icon(g, 0xE895, RectF(x, y + 40 * S, w, 40 * S), 30, alpha(cMuted, 180));
        text(g, T(L"Запускаю бота…"), RectF(x, y + 90 * S, w, 30 * S), 18, cText, 0, 1, 1, FONT_SEMI);
        text(g, T(L"Если это длится дольше минуты — нажми «Запустить» или переустанови TgDesk"), RectF(x, y + 122 * S, w, 22 * S), 13, cMuted, 0, 1, 1);
        button(g, ID_START, RectF(c.X + (c.Width - 200 * S) / 2, y + 170 * S, 200 * S, 42 * S), T(L"Запустить"), Primary, 0xE768);
        return;
    }

    if (g_wizStep == 0) {
        text(g, T(L"Привет! Это TgDesk"), RectF(x, y, w, 40 * S), 28, cText, 0, 0, 1, FONT_SEMI);
        y += 50 * S;
        y += textWrap(g, T(L"TgDesk превращает Telegram в пульт для этого компьютера: экран вживую, мышь и клавиатура, файлы, звук и питание. Настройка займёт пару минут: создадим твоего бота, привяжем твой Telegram и выберем, что можно делать."),
                      RectF(x, y, w, 120 * S), 14.5f, cMuted) + 26 * S;
        text(g, T(L"Язык"), RectF(x, y, 200 * S, 24 * S), 14.5f, cText, 0, 0, 1, FONT_SEMI);
        int sel = g_uiLang == L"ru" ? 1 : g_uiLang == L"en" ? 2 : 0;
        segmented(g, ID_WIZ_LANG, RectF(x, y + 30 * S, std::min(w, 420 * S), 42 * S), {T(L"Как в Windows"), L"Русский", L"English"}, sel);
        y += 96 * S;
        fillRR(g, RectF(x, y, w, 64 * S), 14 * S, alpha(cGreen, 22));
        icon(g, 0xE72E, RectF(x + 12 * S, y, 30 * S, 64 * S), 16, cGreen);
        textWrap(g, T(L"Всё работает через твоего собственного бота. Токен хранится только на этом ПК в зашифрованном виде, бот отвечает только тем, кого ты добавишь."),
                 RectF(x + 50 * S, y + 11 * S, w - 64 * S, 50 * S), 12.5f, cText);
        button(g, ID_WIZ_NEXT, RectF(c.X + c.Width - pad - 200 * S, by, 200 * S, 44 * S), T(L"Начать"), Primary, 0xE72A);
    } else if (g_wizStep == 1) {
        text(g, T(L"Создай бота в Telegram"), RectF(x, y, w, 36 * S), 24, cText, 0, 0, 1, FONT_SEMI);
        y += 46 * S;
        const wchar_t* lines[] = {
            L"Открой @BotFather в Telegram и отправь команду /newbot",
            L"Придумай имя и адрес бота (адрес заканчивается на bot)",
            L"BotFather пришлёт токен вида 123456789:AAE… — скопируй его и вставь ниже",
        };
        for (int i = 0; i < 3; i++) {
            circle(g, x + 13 * S, y + 13 * S, 13 * S, alpha(cAcc1, 60));
            text(g, std::to_wstring(i + 1), RectF(x, y, 26 * S, 26 * S), 13, cText, 0, 1, 1, FONT_SEMI);
            text(g, T(lines[i]), RectF(x + 40 * S, y, w - 40 * S, 26 * S), 14, cText);
            y += 36 * S;
        }
        button(g, ID_WIZ_BOTFATHER, RectF(x + 40 * S, y + 2 * S, 220 * S, 40 * S), T(L"Открыть @BotFather"), Secondary, 0xE8A7);
        y += 62 * S;
        inputBox(g, IN_TOKEN, RectF(x, y, w - 130 * S, 48 * S), T(L"Токен бота"), true);
        button(g, ID_WIZ_SHOW, RectF(x + w - 120 * S, y + 4 * S, 120 * S, 40 * S), g_showSecret ? T(L"Скрыть") : T(L"Показать"), Ghost, g_showSecret ? 0xED1A : 0xE7B3);
        y += 58 * S;
        if (g_wizBusy) text(g, T(L"Проверяю токен в Telegram…"), RectF(x, y, w, 22 * S), 13, cAcc2);
        else if (!g_wizError.empty()) textWrap(g, g_wizError, RectF(x, y, w, 44 * S), 13, cRed);
        else text(g, T(L"Токен — это пароль от бота. Никому его не показывай"), RectF(x, y, w, 22 * S), 12.5f, cMuted);
        if (!g_wizardForced) button(g, ID_WIZ_BACK, RectF(x, by, 140 * S, 44 * S), T(L"Назад"), Ghost, 0xE72B);
        else button(g, ID_WIZ_BACK, RectF(x, by, 140 * S, 44 * S), T(L"Отмена"), Ghost);
        button(g, ID_WIZ_CHECK, RectF(c.X + c.Width - pad - 260 * S, by, 260 * S, 44 * S), T(L"Проверить и сохранить"), Primary, 0xE73E, !g_wizBusy);
    } else if (g_wizStep == 2) {
        text(g, T(L"Привяжи свой Telegram"), RectF(x, y, w, 36 * S), 24, cText, 0, 0, 1, FONT_SEMI);
        y += 42 * S;
        std::wstring sub = m.botUser.empty() ? std::wstring(T(L"Бот подключается к Telegram…"))
                                             : fmt(T(L"Твой бот: @%ls. Ты станешь его владельцем — другие получат доступ, только если ты их добавишь."), m.botUser.c_str());
        textWrap(g, sub, RectF(x, y, w, 44 * S), 14, cMuted);
        y += 60 * S;
        if (m.pairActive) pairingCode(g, c, y, true);
        else {
            if (!g_pairRequested && m.configured) requestOwnerPairing();
            text(g, g_pairRequested ? T(L"Код истёк или ещё создаётся") : T(L"Создаю код…"), RectF(x, y + 30 * S, w, 24 * S), 15, cMuted, 0, 1, 1);
            button(g, ID_WIZ_NEWCODE, RectF(c.X + (c.Width - 200 * S) / 2, y + 70 * S, 200 * S, 42 * S), T(L"Новый код"), Primary, 0xE72C);
        }
        button(g, ID_WIZ_BACK, RectF(x, by, 140 * S, 44 * S), T(L"Назад"), Ghost, 0xE72B);
        statusDot(g, c.X + c.Width - pad - 250 * S, by + 22 * S, 5 * S, cAmber, true, t);
        text(g, T(L"Жду сообщение от тебя…"), RectF(c.X + c.Width - pad - 236 * S, by, 236 * S, 44 * S), 13.5f, cMuted, 0, 0, 1);
    } else {
        text(g, T(L"Готово! Что можно делать с ПК?"), RectF(x, y, w, 36 * S), 24, cText, 0, 0, 1, FONT_SEMI);
        y += 40 * S;
        text(g, T(L"Это можно поменять позже в разделе «Функции»"), RectF(x, y, w, 22 * S), 13.5f, cMuted);
        y += 36 * S;
        float colW = (w - 16 * S) / 2, rowH = 58 * S;
        for (int i = 0; i < PERM_N; i++) {
            float rx = x + (i % 2) * (colW + 16 * S), ry = y + (i / 2) * (rowH + 10 * S);
            RectF b(rx, ry, colW, rowH);
            bool on = m.features.count(PERMS[i]) ? m.features[PERMS[i]] : true;
            fillRR(g, b, 14 * S, cCard2);
            icon(g, PERM_ICONS[i], RectF(b.X + 10 * S, b.Y, 34 * S, b.Height), 16, on ? cAcc2 : cMuted);
            text(g, permTitle(i), RectF(b.X + 50 * S, b.Y + 8 * S, b.Width - 120 * S, 22 * S), 14, cText, 0, 0, 1, FONT_SEMI);
            text(g, permDesc(i), RectF(b.X + 50 * S, b.Y + 30 * S, b.Width - 120 * S, 20 * S), 11.5f, cMuted);
            toggle(g, ID_WIZ_FEAT + i, b.X + b.Width - 60 * S, b.Y + 17 * S, on);
        }
        button(g, ID_WIZ_FINISH, RectF(c.X + c.Width - pad - 220 * S, by, 220 * S, 44 * S), T(L"Начать пользоваться"), Primary, 0xE73E);
    }
}

// ============================================================================ sidebar and frame
static void drawSidebar(Graphics& g, float H, double t) {
    Model& m = g_m;
    float sw = sidebarW();
    SolidBrush sb(cSide);
    g.FillRectangle(&sb, RectF(0, 0, sw, H));
    SolidBrush lb(cBorder);
    g.FillRectangle(&lb, RectF(sw - 1, 0, 1, H));
    gradRR(g, RectF(22 * S, 24 * S, 42 * S, 42 * S), 13 * S, cAcc1, cAcc2, 45.f);
    icon(g, 0xE7F4, RectF(22 * S, 24 * S, 42 * S, 42 * S), 19, Color(255, 255, 255, 255));
    text(g, L"TgDesk", RectF(76 * S, 22 * S, 140 * S, 26 * S), 19, cText, 0, 0, 1, FONT_SEMI);
    text(g, T(L"Пульт для ПК"), RectF(76 * S, 47 * S, 140 * S, 18 * S), 12, cMuted);
    off = PointF(0, 0);
    viewClip = RectF(0, 0, sw, H);
    for (int i = 0; i < P_COUNT; i++) {
        RectF r(12 * S, 94 * S + i * 48 * S, sw - 24 * S, 42 * S);
        int id = ID_NAV + i;
        float h = hover(id);
        float sel = approach(av, 7000 + i, g_page == i ? 1.f : 0.f, 0.28f);
        float k = std::max(sel, h * 0.55f);
        fillRR(g, r, 12 * S, alpha(cCard2, (int)(255 * k)));
        if (sel > 0.02f) gradRR(g, RectF(r.X + 2 * S, r.Y + 11 * S, 3.5f * S, 20 * S), 2 * S, alpha(cAcc1, (int)(255 * sel)), alpha(cAcc2, (int)(255 * sel)), 90.f);
        icon(g, NAV_ICONS[i], RectF(r.X + 12 * S, r.Y, 30 * S, r.Height), 17, lerp(cMuted, cAcc2, sel));
        text(g, T(NAV_NAMES[i]), RectF(r.X + 50 * S, r.Y, r.Width - 60 * S, r.Height), 14.5f, lerp(cMuted, cText, std::max(sel, h)), 0, 0, 1, FONT_SEMI);
        int badgeN = i == P_STREAMS ? (int)m.sessions.size() : 0;
        if (badgeN) {
            RectF bd(r.X + r.Width - 34 * S, r.Y + 10 * S, 22 * S, 22 * S);
            fillRR(g, bd, 11 * S, cRed);
            text(g, std::to_wstring(badgeN), bd, 12, Color(255, 255, 255, 255), 0, 1, 1, FONT_SEMI);
        }
        if (i == P_ABOUT && !m.updVersion.empty()) circle(g, r.X + r.Width - 22 * S, r.Y + r.Height / 2, 4.5f * S, cAcc2);
        addHit(id, r);
    }
    BotSt st = botStatus(m);
    RectF pr(14 * S, H - 74 * S, sw - 28 * S, 56 * S);
    fillRR(g, pr, 14 * S, cCard);
    strokeRR(g, pr, 14 * S, cBorder);
    statusDot(g, pr.X + 22 * S, pr.Y + pr.Height / 2, 5 * S, st.c, st.c.GetValue() == cGreen.GetValue(), t);
    text(g, st.text, RectF(pr.X + 40 * S, pr.Y + 8 * S, pr.Width - 48 * S, 22 * S), 13.5f, cText, 0, 0, 1, FONT_SEMI);
    std::wstring sub2 = !m.online ? std::wstring(T(L"Нажми «Запустить»")) : !m.sessions.empty() ? std::wstring(T(L"Идёт трансляция"))
                      : m.tunnelUrl.empty() ? T(L"Туннель: ") + tunnelStateText(m) : std::wstring(T(L"Никто не смотрит"));
    text(g, sub2, RectF(pr.X + 40 * S, pr.Y + 29 * S, pr.Width - 48 * S, 18 * S), 11.5f, cMuted);
}

static void drawToast(Graphics& g, float cx, float cwid, int Hp) {
    if (GetTickCount64() >= g_toastUntil || g_toast.empty()) return;
    float tw = std::min(cwid - 80 * S, measure(g, g_toast, 14, FONT_SEMI) + 60 * S);
    RectF tr(cx + (cwid - tw) / 2, Hp - 74 * S, tw, 44 * S);
    fillRR(g, RectF(tr.X, tr.Y + 3 * S, tr.Width, tr.Height), 14 * S, Color(70, 0, 0, 0));
    fillRR(g, tr, 14 * S, Color(245, 38, 44, 76));
    strokeRR(g, tr, 14 * S, alpha(cAcc1, 160));
    text(g, g_toast, tr, 14, cText, 0, 1, 1, FONT_SEMI);
    animating = true;
}

static void render(Graphics& g, int Wp, int Hp) {
    std::lock_guard<std::mutex> lk(g_mx);
    hits.clear();
    animating = false;
    for (auto& p : g_inputs) p.second.used = false;
    double t = nowSec();
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
    g.SetPixelOffsetMode(PixelOffsetModeHalf);
    g.Clear(cBg);

    if (g_wizard) {
        drawWizard(g, Wp, Hp, t);
        drawToast(g, 0, (float)Wp, Hp);
        return;
    }
    drawSidebar(g, (float)Hp, t);

    float cx = sidebarW(), cwid = Wp - cx;
    float margin = 34 * S;
    viewClip = RectF(cx, 0, cwid, (float)Hp);
    float scroll = g_scroll[g_page];
    off = PointF(cx + margin, 30 * S - scroll);
    g.SetClip(viewClip);
    g.TranslateTransform(off.X, off.Y);
    float W_ = cwid - margin * 2;
    float contentH = 0;
    switch (g_page) {
        case P_HOME: contentH = pageHome(g, W_, t); break;
        case P_STREAMS: contentH = pageStreams(g, W_, t); break;
        case P_LOG: contentH = pageLog(g, W_, t); break;
        case P_USERS: contentH = pageUsers(g, W_, t); break;
        case P_FEATURES: contentH = pageFeatures(g, W_, t); break;
        case P_SETTINGS: contentH = pageSettings(g, W_, t); break;
        case P_ABOUT: contentH = pageAbout(g, W_, t); break;
    }
    g.ResetTransform();
    g_maxScroll = std::max(0.f, contentH + 30 * S - Hp);
    if (g_maxScroll > 0) {
        float trackH = Hp - 16 * S, th = std::max(40 * S, trackH * Hp / (contentH + 30 * S));
        float ty = 8 * S + (trackH - th) * (scroll / g_maxScroll);
        fillRR(g, RectF(Wp - 9 * S, ty, 4 * S, th), 2 * S, alpha(cMuted, 90));
    }
    g.ResetClip();
    drawToast(g, cx, cwid, Hp);
}

// ============================================================================ click actions
static void setPage(int p) {
    if (p == g_page) return;
    g_page = p;
    g_pageAtomic = p;
    g_scrollT[p] = g_scroll[p] = 0;
    g_tunSel = -1;
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

static std::string permsJson(const std::set<std::string>& s) {
    std::string o = "[";
    for (auto& p : s) o += (o.size() > 1 ? ",\"" : "\"") + p + "\"";
    return o + "]";
}

static void setLang(int sel) {
    g_uiLang = sel == 1 ? L"ru" : sel == 2 ? L"en" : L"auto";
    applyLang();
    saveIni();
    pushLang();
}

static void applyTunnel() {
    int sel;
    std::wstring mode;
    { std::lock_guard<std::mutex> l(g_mx); sel = g_tunSel >= 0 ? g_tunSel : (g_m.tunnelMode == L"cloudflare" ? 1 : g_m.tunnelMode == L"external" ? 2 : 0); }
    mode = sel == 1 ? L"cloudflare" : sel == 2 ? L"external" : L"quick";
    std::string body = "{\"tunnel\":{\"mode\":\"" + U(mode) + "\"";
    if (sel >= 1) {
        std::wstring url = inputText(IN_URL);
        if (url.rfind(L"https://", 0) != 0) { toast(T(L"Адрес должен начинаться с https://")); return; }
        body += ",\"public_url\":\"" + jsonEscape(U(url)) + "\"";
        std::wstring tok = inputText(IN_CFTOKEN);
        if (sel == 1 && !tok.empty()) body += ",\"token\":\"" + jsonEscape(U(tok)) + "\"";
    }
    body += "}}";
    async([body] {
        HttpResult r = api::post(L"/settings", body);
        JV j;
        parseJson(r.body, j);
        if (r.status == 200) { toast(T(L"Адрес Mini App применён, туннель перезапускается")); setInputText(IN_CFTOKEN, L""); }
        else if (j["error"].str() == "no_token") toast(T(L"Укажи токен Cloudflare Tunnel"));
        else if (j["error"].str() == "bad_url") toast(T(L"Нужен адрес вида https://имя.домен"));
        else toast(T(L"Бот не отвечает — настройка не применена"));
    });
    g_tunSel = -1;
}

static void onClick(int id) {
    // ---------------- per-row actions (need the model)
    std::string sidToKick;
    long long uid = 0;
    UserRec urec;
    {
        std::lock_guard<std::mutex> l(g_mx);
        if (id >= ID_KICK && id < ID_KICK + 500 && (size_t)(id - ID_KICK) < g_m.sessions.size()) sidToKick = g_m.sessions[id - ID_KICK].sid;
        int idx = -1;
        if (id >= ID_USER_EXPAND && id < ID_USER_EXPAND + 1000) idx = id - ID_USER_EXPAND;
        else if (id >= ID_USER_DEL && id < ID_USER_DEL + 1000) idx = id - ID_USER_DEL;
        else if (id >= ID_USER_ROLE && id < ID_USER_PERM) idx = (id - ID_USER_ROLE) / 1000;
        else if (id >= ID_USER_PERM && id < ID_USER_PERM + 1000000) idx = (id - ID_USER_PERM) / 1000;
        if (idx >= 0 && (size_t)idx < g_m.users.size()) { urec = g_m.users[idx]; uid = urec.id; }
    }
    if (!sidToKick.empty()) {
        async([sidToKick] { api::post(L"/kick", "{\"sid\":\"" + sidToKick + "\"}"); toast(T(L"Зритель отключён")); });
        return;
    }
    if (uid) {
        if (id >= ID_USER_EXPAND && id < ID_USER_EXPAND + 1000) { g_expandedUser = g_expandedUser == uid ? 0 : uid; return; }
        if (id >= ID_USER_DEL && id < ID_USER_DEL + 1000) {
            std::wstring q = fmt(T(L"Удалить %ls из пользователей?\n\nЕго трансляции будут остановлены, бот перестанет ему отвечать."), (urec.name.empty() ? std::to_wstring(uid) : urec.name).c_str());
            if (MessageBoxW(g_hwnd, q.c_str(), L"TgDesk", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) return;
            async([uid] {
                HttpResult r = api::post(L"/users/remove", "{\"id\":" + std::to_string(uid) + "}");
                toast(r.status == 200 ? T(L"Пользователь удалён") : T(L"Бот не отвечает — настройка не применена"));
            });
            return;
        }
        std::string role = U(urec.role);
        std::set<std::string> perms = urec.perms;
        if (id >= ID_USER_ROLE && id < ID_USER_PERM) {
            int k = (id - ID_USER_ROLE) % 1000;
            if (k > 1) return;
            role = k == 1 ? "owner" : "user";
            if (role == "owner") perms.clear(), perms.insert(PERMS, PERMS + PERM_N);
        } else {
            int k = (id - ID_USER_PERM) % 1000;
            if (k >= PERM_N) return;
            if (perms.count(PERMS[k])) perms.erase(PERMS[k]); else perms.insert(PERMS[k]);
        }
        {   // optimistic update for a snappy UI; the next poll brings the real state
            std::lock_guard<std::mutex> l(g_mx);
            for (auto& u : g_m.users) if (u.id == uid) { u.role = W(role); u.perms = perms; }
        }
        std::string body = "{\"id\":" + std::to_string(uid) + ",\"role\":\"" + role + "\",\"perms\":" + permsJson(perms) + "}";
        async([body] { if (api::post(L"/users/update", body).status != 200) toast(T(L"Бот не отвечает — настройка не применена")); });
        return;
    }

    if (id >= ID_NAV && id < ID_NAV + P_COUNT) { setPage(id - ID_NAV); return; }
    if (id >= ID_CHIP && id < ID_CHIP + 8) { g_logFilter = id - ID_CHIP; g_scrollT[P_LOG] = 0; return; }
    if (id >= ID_QUAL && id < ID_QUAL + 4) {
        const char* q[] = {"eco", "norm", "hd", "auto"};
        { std::lock_guard<std::mutex> l(g_mx); g_m.quality = W(q[id - ID_QUAL]); }
        postSettings(std::string("{\"quality\":\"") + q[id - ID_QUAL] + "\"}", T(L"Качество по умолчанию изменено"));
        return;
    }
    if (id >= ID_WIZ_LANG && id < ID_WIZ_LANG + 3) { setLang(id - ID_WIZ_LANG); return; }
    if (id >= ID_SET_LANG && id < ID_SET_LANG + 3) { setLang(id - ID_SET_LANG); return; }
    if (id >= ID_PAIR_ROLE && id < ID_PAIR_ROLE + 2) { g_newRole = id - ID_PAIR_ROLE; return; }
    if (id >= ID_PAIR_PERM && id < ID_PAIR_PERM + PERM_N) {
        const char* p = PERMS[id - ID_PAIR_PERM];
        if (g_newPerms.count(p)) g_newPerms.erase(p); else g_newPerms.insert(p);
        return;
    }
    if (id >= ID_TUN_MODE && id < ID_TUN_MODE + 3) {
        g_tunSel = id - ID_TUN_MODE;
        std::lock_guard<std::mutex> l(g_mx);
        if (inputText(IN_URL).empty() && !g_m.publicUrl.empty()) setInputText(IN_URL, g_m.publicUrl);
        return;
    }
    if ((id >= ID_FEAT && id < ID_FEAT + PERM_N) || (id >= ID_WIZ_FEAT && id < ID_WIZ_FEAT + PERM_N)) {
        int k = id >= ID_WIZ_FEAT ? id - ID_WIZ_FEAT : id - ID_FEAT;
        bool v;
        {
            std::lock_guard<std::mutex> l(g_mx);
            v = !(g_m.features.count(PERMS[k]) ? g_m.features[PERMS[k]] : true);
            g_m.features[PERMS[k]] = v;
        }
        postSettings(std::string("{\"features\":{\"") + PERMS[k] + "\":" + (v ? "true" : "false") + "}}");
        return;
    }
    switch (id) {
        case ID_START: actStart(); break;
        case ID_STOP: actStop(); break;
        case ID_RESTART: actRestart(); break;
        case ID_TUN_COPY: {
            std::wstring u;
            { std::lock_guard<std::mutex> l(g_mx); u = g_m.tunnelUrl; }
            if (!u.empty()) { copyText(u); toast(T(L"Адрес скопирован")); }
            break;
        }
        case ID_UPD_INSTALL:
            async([] {
                HttpResult r = api::post(L"/update/install", "{}", 5000);
                if (r.status != 200) toast(T(L"Бот не отвечает — настройка не применена"));
                else if (r.body.find("\"ok\":true") == std::string::npos) toast(T(L"Обновление сейчас недоступно — открой страницу релиза"));
                else toast(T(L"Скачиваю обновление…"));
            });
            break;
        case ID_AUTO_UPD: {
            bool v;
            { std::lock_guard<std::mutex> l(g_mx); g_m.autoUpdate = !(g_m.autoUpdate && g_m.updateCheck); v = g_m.autoUpdate; if (v) g_m.updateCheck = true; }
            postSettings(v ? "{\"auto_update\":true,\"update_check\":true}" : "{\"auto_update\":false}",
                         v ? T(L"Автообновление включено") : T(L"Автообновление выключено"));
            break;
        }
        case ID_TUN_RESTART:
            async([] { api::post(L"/tunnel/restart"); toast(T(L"Переподключаю туннель…")); });
            break;
        case ID_OPEN_BOT: {
            std::wstring u;
            { std::lock_guard<std::mutex> l(g_mx); u = g_m.botUser; }
            if (!u.empty()) openUrl(L"https://t.me/" + u);
            break;
        }
        case ID_KICK_ALL:
            async([] { api::post(L"/kick", "{\"sid\":\"*\"}"); toast(T(L"Все зрители отключены")); });
            break;
        case ID_OPEN_WEBLOG: openUrl(L"http://127.0.0.1:" + std::to_wstring(api::logPort) + L"/"); break;
        case ID_CSV: openUrl(L"http://127.0.0.1:" + std::to_wstring(api::logPort) + L"/api/log.csv"); break;
        case ID_TG_AUTOSTART: {
            bool v;
            { std::lock_guard<std::mutex> l(g_mx); g_m.autostart = !g_m.autostart; v = g_m.autostart; }
            postSettings(std::string("{\"autostart\":") + (v ? "true" : "false") + "}", v ? T(L"Автозапуск включён") : T(L"Автозапуск выключен"));
            break;
        }
        case ID_TG_TRAY: g_closeToTray = !g_closeToTray; saveIni(); break;
        case ID_UPD_CHECK: {
            bool v;
            { std::lock_guard<std::mutex> l(g_mx); g_m.updateCheck = !g_m.updateCheck; v = g_m.updateCheck; }
            postSettings(std::string("{\"update_check\":") + (v ? "true" : "false") + "}");
            break;
        }
        case ID_GEOIP: {
            bool v;
            { std::lock_guard<std::mutex> l(g_mx); g_m.geoip = !g_m.geoip; v = g_m.geoip; }
            postSettings(std::string("{\"geoip\":") + (v ? "true" : "false") + "}");
            break;
        }
        case ID_OPEN_DIR: ShellExecuteW(nullptr, L"open", api::installDir.c_str(), nullptr, nullptr, SW_SHOWNORMAL); break;
        case ID_OPEN_DATA: ShellExecuteW(nullptr, L"open", api::dataDir.c_str(), nullptr, nullptr, SW_SHOWNORMAL); break;
        case ID_OPEN_BOTLOG: ShellExecuteW(nullptr, L"open", (api::dataDir + L"\\tgdesk.log").c_str(), nullptr, nullptr, SW_SHOWNORMAL); break;
        case ID_UPD_OPEN: {
            std::wstring u;
            { std::lock_guard<std::mutex> l(g_mx); u = g_m.updUrl; }
            if (u.rfind(std::wstring(REPO_URL) + L"/", 0) == 0) openUrl(u);
            break;
        }
        case ID_ABOUT_GH: openUrl(REPO_URL); break;
        case ID_ABOUT_ISSUES: openUrl(std::wstring(REPO_URL) + L"/issues"); break;
        case ID_ABOUT_LICENSE: {
            std::wstring lic = api::installDir + L"\\LICENSE.txt";
            if (api::exists(lic)) ShellExecuteW(nullptr, L"open", lic.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            else openUrl(std::wstring(REPO_URL) + L"/blob/main/LICENSE");
            break;
        }
        case ID_HOME_USERS: setPage(P_USERS); break;
        case ID_ABOUT_UNINSTALL: {
            // the uninstaller stops the app and the bot itself and asks for confirmation
            std::wstring u = uninstallerExe();
            if (api::exists(u)) ShellExecuteW(nullptr, L"open", u.c_str(), nullptr, api::installDir.c_str(), SW_SHOWNORMAL);
            break;
        }
        case ID_TOKEN_CHANGE: g_wizardForced = true; g_showSecret = false; wizardStart(1); break;
        case ID_TUN_APPLY: applyTunnel(); break;
        case ID_PAIR_START: {
            std::string body = g_newRole == 0 ? std::string("{\"role\":\"user\",\"perms\":") + permsJson(std::set<std::string>(PERMS, PERMS + PERM_N)) + "}"
                                              : std::string("{\"role\":\"user\",\"perms\":") + permsJson(g_newPerms) + "}";
            async([body] { if (api::post(L"/pair/start", body).status != 200) toast(T(L"Бот не отвечает — настройка не применена")); });
            break;
        }
        case ID_PAIR_CANCEL: async([] { api::post(L"/pair/cancel"); toast(T(L"Код отменён")); }); break;
        case ID_PAIR_OPEN:
        case ID_PAIR_COPY: {
            std::wstring link;
            { std::lock_guard<std::mutex> l(g_mx); link = g_m.pairLink; }
            if (link.rfind(L"https://t.me/", 0) != 0) break;
            if (id == ID_PAIR_OPEN) openUrl(link);
            else { copyText(link); toast(T(L"Ссылка скопирована — отправь её тому, кого добавляешь")); }
            break;
        }
        // wizard
        case ID_WIZ_NEXT: g_wizStep = 1; break;
        case ID_WIZ_BACK:
            if (g_wizardForced) { g_wizard = false; g_wizardForced = false; g_wizError.clear(); }
            else if (g_wizStep > 0) { g_wizStep--; g_wizError.clear(); if (g_wizStep < 2) async([] { api::post(L"/pair/cancel"); }); }
            break;
        case ID_WIZ_BOTFATHER: openUrl(L"https://t.me/BotFather"); break;
        case ID_WIZ_SHOW: g_showSecret = !g_showSecret; break;
        case ID_WIZ_CHECK: if (!g_wizBusy) wizardCheckToken(); break;
        case ID_WIZ_NEWCODE: requestOwnerPairing(); break;
        case ID_WIZ_FINISH:
            g_wizard = false;
            g_wizardForced = false;
            g_showSecret = false;
            setInputText(IN_TOKEN, L"");
            setPage(P_HOME);
            toast(T(L"Всё готово! Открой бота в Telegram и нажми «TgDesk»"));
            break;
    }
}

// ============================================================================ tray
static void trayAdd() {
    bool exists = g_nid.cbSize != 0;
    memset(&g_nid, 0, sizeof g_nid);
    g_nid.cbSize = sizeof g_nid;
    g_nid.hWnd = g_hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAY;
    g_nid.hIcon = (HICON)LoadImageW(g_inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    wcscpy(g_nid.szTip, L"TgDesk");
    if (!exists || !Shell_NotifyIconW(NIM_MODIFY, &g_nid)) Shell_NotifyIconW(NIM_ADD, &g_nid);
}
static void trayTip(const std::wstring& tip) {
    wcsncpy(g_nid.szTip, tip.c_str(), 127);
    g_nid.szTip[127] = 0;
    g_nid.uFlags = NIF_TIP;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}
static void trayBalloon(const std::wstring& title, const std::wstring& msg) {
    g_nid.uFlags = NIF_INFO;
    wcsncpy(g_nid.szInfoTitle, title.c_str(), 63);
    g_nid.szInfoTitle[63] = 0;
    wcsncpy(g_nid.szInfo, msg.c_str(), 255);
    g_nid.szInfo[255] = 0;
    g_nid.dwInfoFlags = NIIF_INFO;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}
static void showWindow() {
    ShowWindow(g_hwnd, SW_SHOW);
    if (IsIconic(g_hwnd)) ShowWindow(g_hwnd, SW_RESTORE);
    SetForegroundWindow(g_hwnd);
    InvalidateRect(g_hwnd, nullptr, FALSE);
}
static void trayMenu() {
    bool online;
    { std::lock_guard<std::mutex> l(g_mx); online = g_m.online; }
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, 1, T(L"Открыть TgDesk"));
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    if (online) { AppendMenuW(m, MF_STRING, 2, T(L"Перезапустить бота")); AppendMenuW(m, MF_STRING, 3, T(L"Остановить бота")); }
    else AppendMenuW(m, MF_STRING, 4, T(L"Запустить бота"));
    AppendMenuW(m, MF_STRING, 5, T(L"Журнал доступа в браузере"));
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, 6, T(L"Выйти (бот продолжит работать)"));
    AppendMenuW(m, MF_STRING, 7, T(L"Выйти и остановить бота"));
    POINT p;
    GetCursorPos(&p);
    SetForegroundWindow(g_hwnd);
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, p.x, p.y, 0, g_hwnd, nullptr);
    DestroyMenu(m);
    PostMessage(g_hwnd, WM_NULL, 0, 0);
    switch (cmd) {
        case 1: showWindow(); break;
        case 2: actRestart(); break;
        case 3: actStop(); break;
        case 4: actStart(); break;
        case 5: onClick(ID_OPEN_WEBLOG); break;
        case 6: DestroyWindow(g_hwnd); break;
        case 7: async([] { stopBotSync(); PostMessage(g_hwnd, WM_CLOSE, 1, 0); }); break;
    }
}

// ============================================================================ window
static void applyDpi(HWND h) {
    UINT dpi = GetDpiForWindow(h);
    S = dpi / 96.0f;
    resetFonts();
    makeEditFont();
}

static void paintWindow(HWND h) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(h, &ps);
    RECT rc;
    GetClientRect(h, &rc);
    int w = rc.right, hgt = rc.bottom;
    if (w > 0 && hgt > 0) {
        HDC mem = CreateCompatibleDC(hdc);
        HBITMAP bmp = CreateCompatibleBitmap(hdc, w, hgt);
        HGDIOBJ old = SelectObject(mem, bmp);
        {
            Graphics g(mem);
            render(g, w, hgt);
        }
        BitBlt(hdc, 0, 0, w, hgt, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
        DeleteObject(bmp);
        DeleteDC(mem);
    }
    EndPaint(h, &ps);
    layoutInputs();
}

static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            g_hwnd = h;
            g_editBrush = CreateSolidBrush(RGB(31, 36, 61));
            applyDpi(h);
            BOOL dark = TRUE;
            DwmSetWindowAttribute(h, 20, &dark, sizeof dark);
            COLORREF capc = RGB(10, 12, 23), txt = RGB(238, 241, 255);
            DwmSetWindowAttribute(h, 35, &capc, sizeof capc);
            DwmSetWindowAttribute(h, 34, &capc, sizeof capc);
            DwmSetWindowAttribute(h, 36, &txt, sizeof txt);
            SetTimer(h, 1, 16, nullptr);
            trayAdd();
            return 0;
        }
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: paintWindow(h); return 0;
        case WM_CTLCOLOREDIT: {
            HDC dc = (HDC)wp;
            SetTextColor(dc, RGB(238, 241, 255));
            SetBkColor(dc, RGB(31, 36, 61));
            return (LRESULT)g_editBrush;
        }
        case WM_GETMINMAXINFO: {
            auto* mm = (MINMAXINFO*)lp;
            mm->ptMinTrackSize.x = (LONG)(1060 * S);
            mm->ptMinTrackSize.y = (LONG)(680 * S);
            return 0;
        }
        case WM_DPICHANGED: {
            applyDpi(h);
            RECT* r = (RECT*)lp;
            SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        case WM_SIZE:
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        case WM_TIMER: {
            bool need = ui::animating;
            float& cur = g_scroll[g_page];
            float tgt = std::clamp(g_scrollT[g_page], 0.f, g_maxScroll);
            g_scrollT[g_page] = tgt;
            if (std::fabs(cur - tgt) > 0.4f) { cur += (tgt - cur) * 0.28f; need = true; }
            else if (cur != tgt) { cur = tgt; need = true; }
            if (cur > g_maxScroll) { cur = g_maxScroll; need = true; }
            static ULONGLONG lastSec = 0;   // countdowns (pairing code) tick once a second
            if (GetTickCount64() / 1000 != lastSec) { lastSec = GetTickCount64() / 1000; need = need || g_m.pairActive; }
            if (need && IsWindowVisible(h) && !IsIconic(h)) InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        case WM_MOUSEMOVE: {
            TRACKMOUSEEVENT te{sizeof te, TME_LEAVE, h, 0};
            TrackMouseEvent(&te);
            int id = hitTest((float)GET_X_LPARAM(lp), (float)GET_Y_LPARAM(lp));
            if (id != ui::hot) { ui::hot = id; InvalidateRect(h, nullptr, FALSE); }
            return 0;
        }
        case WM_MOUSELEAVE:
            ui::hot = 0;
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        case WM_SETCURSOR:
            if ((HWND)wp == h && LOWORD(lp) == HTCLIENT) { SetCursor(LoadCursor(nullptr, ui::hot ? IDC_HAND : IDC_ARROW)); return TRUE; }
            break;
        case WM_LBUTTONDOWN:
            SetFocus(h);
            ui::down = ui::hot;
            SetCapture(h);
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        case WM_LBUTTONUP: {
            ReleaseCapture();
            int d = ui::down;
            ui::down = 0;
            int id = hitTest((float)GET_X_LPARAM(lp), (float)GET_Y_LPARAM(lp));
            ui::hot = id;
            if (d && d == id) onClick(id);
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        case WM_MOUSEWHEEL: {
            if (g_wizard) return 0;
            int d = GET_WHEEL_DELTA_WPARAM(wp);
            g_scrollT[g_page] = std::clamp(g_scrollT[g_page] - d * 0.9f * S, 0.f, g_maxScroll);
            return 0;
        }
        case WM_KEYDOWN:
            if (g_wizard) return 0;
            if (wp == VK_NEXT) g_scrollT[g_page] += 500 * S;
            else if (wp == VK_PRIOR) g_scrollT[g_page] -= 500 * S;
            else if (wp == VK_HOME) g_scrollT[g_page] = 0;
            else if (wp == VK_END) g_scrollT[g_page] = g_maxScroll;
            return 0;
        case WM_APP_ENTER:
            if (wp == IN_TOKEN) onClick(ID_WIZ_CHECK);
            else if (wp == IN_URL || wp == IN_CFTOKEN) onClick(ID_TUN_APPLY);
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        case WM_APP_MODEL: {
            std::wstring tip;
            {
                std::lock_guard<std::mutex> l(g_mx);
                wizardSync();
                BotSt st = botStatus(g_m);
                tip = L"TgDesk — " + st.text;
                if (!g_m.sessions.empty()) tip += T(L" · эфир: ") + std::to_wstring(g_m.sessions.size());
            }
            static std::wstring lastTip;
            if (tip != lastTip) { trayTip(tip); lastTip = tip; }
            if (IsWindowVisible(h) && !IsIconic(h)) InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        case WM_APP_TOAST:
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        case WM_APP_NOTIFY: {
            auto* p = (std::pair<std::wstring, std::wstring>*)lp;
            trayBalloon(p->first, p->second);
            delete p;
            return 0;
        }
        case WM_APP_SHOW: showWindow(); return 0;
        case WM_TRAY:
            if (LOWORD(lp) == WM_LBUTTONUP || LOWORD(lp) == WM_LBUTTONDBLCLK) {
                if (IsWindowVisible(h) && !IsIconic(h) && GetForegroundWindow() == h) ShowWindow(h, SW_HIDE); else showWindow();
            } else if (LOWORD(lp) == WM_RBUTTONUP) trayMenu();
            return 0;
        case WM_CLOSE:
            if (g_closeToTray && wp == 0) { ShowWindow(h, SW_HIDE); return 0; }
            DestroyWindow(h);
            return 0;
        case WM_DESTROY:
            g_run = false;
            Shell_NotifyIconW(NIM_DELETE, &g_nid);
            PostQuitMessage(0);
            return 0;
        default:
            if (msg == g_taskbarCreated) { g_nid.cbSize = 0; trayAdd(); return 0; }
    }
    return DefWindowProcW(h, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    g_inst = inst;
    // single instance: a second launch just shows the window of the first one
    HANDLE mtx = CreateMutexW(nullptr, TRUE, L"Local\\TgDeskApp_Singleton");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND other = FindWindowW(L"TgDeskMainWnd", nullptr);
        if (other) PostMessage(other, WM_APP_SHOW, 0, 0);
        return 0;
    }
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    api::resolveDirs();
    CreateDirectoryW(api::dataDir.c_str(), nullptr);
    api::loadPorts();
    loadIni();
    applyLang();
    bool trayOnly = wcsstr(GetCommandLineW(), L"--tray") != nullptr;
    if (const wchar_t* pg = wcsstr(GetCommandLineW(), L"--page=")) {
        int p = _wtoi(pg + 7);
        if (p >= 0 && p < P_COUNT) { g_page = p; g_pageAtomic = p; }
    }
    // first run (no token yet): start with the wizard right away, even before the bot answers
    {
        JV cfg = api::readConfig();
        if (cfg["token_enc"].str().empty()) { g_wizard = true; g_wizStep = 0; trayOnly = false; }
    }

    GdiplusStartupInput gi;
    ULONG_PTR gtok;
    GdiplusStartup(&gtok, &gi, nullptr);
    INITCOMMONCONTROLSEX icc{sizeof icc, ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = L"TgDeskMainWnd";
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hIconSm = wc.hIcon;
    RegisterClassExW(&wc);

    UINT dpi = GetDpiForSystem();
    S = dpi / 96.0f;
    int ww = (int)(1240 * S), wh = (int)(820 * S);
    RECT wa;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    ww = std::min(ww, (int)(wa.right - wa.left));
    wh = std::min(wh, (int)(wa.bottom - wa.top));
    HWND h = CreateWindowExW(0, L"TgDeskMainWnd", L"TgDesk", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, wa.left + (wa.right - wa.left - ww) / 2,
                             wa.top + (wa.bottom - wa.top - wh) / 2, ww, wh, nullptr, nullptr, inst, nullptr);
    if (!trayOnly) ShowWindow(h, SW_SHOW);

    // the bot: start it if it is not running
    async([] {
        Sleep(300);
        if (!botAlive()) startBot();
    });
    std::thread poller(pollLoop);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    g_run = false;
    if (poller.joinable()) poller.join();
    GdiplusShutdown(gtok);
    CloseHandle(mtx);
    return 0;
}
