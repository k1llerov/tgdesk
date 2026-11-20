// TgDesk Setup — own installer and uninstaller in one exe. Author: k1llerov™.
//
//  TgDesk-Setup-x.y.z.exe          the stub + packed program files appended by pack.exe → installs
//  {app}\Uninstall TgDesk.exe      the same stub without the payload               → uninstalls
//
// Per-user install, no administrator rights:   %LOCALAPPDATA%\Programs\TgDesk
// Command line:  /S  silent  ·  /D=<folder>  install folder  ·  /uninstall  ·  /purge  (with /uninstall: also delete user data)
//                /update  (with /S: started by TgDesk's updater — TgDesk is started again in the tray afterwards)
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <dwmapi.h>
#include <tlhelp32.h>
#include <compressapi.h>
#include <thread>
#include <atomic>
#include <mutex>
#include <cstdint>
#include <cwchar>
#include "../app/ui.h"
#include "version.h"

#ifndef SETUP_VERSION
#define SETUP_VERSION L"1.0.0"
#endif

using namespace ui;

static const wchar_t* APP = L"TgDesk";
static const wchar_t* AUTHOR = L"k1llerov\u2122";
static const wchar_t* REPO = L"https://github.com/k1llerov/tgdesk";
static const wchar_t* UNINST_EXE = L"Uninstall TgDesk.exe";
static const wchar_t* UNINST_KEY = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\TgDesk";
static const wchar_t* RUN_KEY = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

// ============================================================================ state
static bool RU = false;
static const wchar_t* tr(const wchar_t* ru, const wchar_t* en) { return RU ? ru : en; }

enum Mode { M_INSTALL, M_UNINSTALL };
static Mode g_mode = M_INSTALL;
static bool g_silent = false, g_upgrade = false;
static std::wstring g_self, g_dir, g_oldVersion;
static HWND g_hwnd;
static int g_page = 0;   // install: 0 welcome · 1 options · 2 progress · 3 done;  uninstall: 0 confirm · 1 progress · 2 done
static bool optDesktop = true, optStartMenu = true, optAutostart = true, optLaunch = true, optAgree = false, optPurge = false;

static uint64_t g_exeSize = 0, g_payOff = 0, g_paySize = 0, g_total = 0;
static uint32_t g_count = 0;

static std::atomic<float> g_prog{0.f};
static std::atomic<bool> g_busy{false};
static std::mutex g_mx;
static std::wstring g_cur, g_err;
static bool g_ok = false;

enum { WM_APP_PROGRESS = WM_APP + 1 };
enum { ID_CLOSE = 1, ID_MIN, ID_NEXT, ID_BACK, ID_BROWSE, ID_AGREE, ID_T_DESK, ID_T_START, ID_T_AUTO, ID_T_LAUNCH, ID_T_PURGE, ID_SITE };

// ============================================================================ helpers
static std::wstring knownFolder(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    std::wstring r;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p))) r = p;
    CoTaskMemFree(p);
    return r;
}
static bool exists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }
static std::wstring parentOf(const std::wstring& p) { auto i = p.find_last_of(L"\\/"); return i == std::wstring::npos ? L"" : p.substr(0, i); }
static std::wstring fmt(const wchar_t* f, ...) {
    wchar_t b[1024];
    va_list a; va_start(a, f); vswprintf(b, 1024, f, a); va_end(a);
    return b;
}
static std::wstring dataDir() {
    wchar_t b[MAX_PATH * 2];
    DWORD n = GetEnvironmentVariableW(L"TGDESK_HOME", b, MAX_PATH * 2);
    if (n > 0 && n < MAX_PATH * 2) return b;
    return knownFolder(FOLDERID_RoamingAppData) + L"\\TgDesk";
}
static std::wstring regGet(HKEY root, const wchar_t* key, const wchar_t* name) {
    wchar_t b[2048]; DWORD sz = sizeof b;
    if (RegGetValueW(root, key, name, RRF_RT_REG_SZ, nullptr, b, &sz) == ERROR_SUCCESS) return b;
    return L"";
}
static void regSet(HKEY k, const wchar_t* name, const std::wstring& v) {
    RegSetValueExW(k, name, 0, REG_SZ, (const BYTE*)v.c_str(), (DWORD)((v.size() + 1) * sizeof(wchar_t)));
}
static void regSetDw(HKEY k, const wchar_t* name, DWORD v) { RegSetValueExW(k, name, 0, REG_DWORD, (const BYTE*)&v, sizeof v); }

static void setStatus(const std::wstring& s, float p = -1) {
    { std::lock_guard<std::mutex> l(g_mx); g_cur = s; }
    if (p >= 0) g_prog = p;
    if (g_hwnd) PostMessage(g_hwnd, WM_APP_PROGRESS, 0, 0);
}

static std::wstring imagePath(DWORD pid) {
    std::wstring r;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return r;
    wchar_t b[MAX_PATH * 2]; DWORD n = MAX_PATH * 2;
    if (QueryFullProcessImageNameW(h, 0, b, &n)) r.assign(b, n);
    CloseHandle(h);
    return r;
}
static bool startsWithI(const std::wstring& s, const std::wstring& pre) {
    return s.size() >= pre.size() && _wcsnicmp(s.c_str(), pre.c_str(), pre.size()) == 0;
}

/// Stops the desktop app, the bot and the bot's cloudflared (only the one from the TgDesk data folder).
static void stopTgDesk() {
    std::wstring dd = dataDir() + L"\\";
    for (int pass = 0; pass < 2; pass++) {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap == INVALID_HANDLE_VALUE) return;
        PROCESSENTRY32W pe{}; pe.dwSize = sizeof pe;
        for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
            if (pe.th32ProcessID == GetCurrentProcessId()) continue;
            bool kill = _wcsicmp(pe.szExeFile, L"TgDesk.exe") == 0 || _wcsicmp(pe.szExeFile, L"TgDeskBot.exe") == 0;
            if (!kill && _wcsicmp(pe.szExeFile, L"cloudflared.exe") == 0) kill = startsWithI(imagePath(pe.th32ProcessID), dd);
            if (!kill) continue;
            HANDLE h = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pe.th32ProcessID);
            if (h) { TerminateProcess(h, 0); WaitForSingleObject(h, 3000); CloseHandle(h); }
        }
        CloseHandle(snap);
        Sleep(200);
    }
}

static void rmTree(const std::wstring& dir) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            std::wstring n = fd.cFileName;
            if (n == L"." || n == L"..") continue;
            std::wstring p = dir + L"\\" + n;
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) rmTree(p);
            else if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) RemoveDirectoryW(p.c_str());
            else { SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL); DeleteFileW(p.c_str()); }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir.c_str());
}

static bool writeAll(const std::wstring& path, const void* data, size_t n) {
    for (int attempt = 0; attempt < 12; attempt++) {
        HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE) {
            if (attempt == 2 || attempt == 6) stopTgDesk();
            Sleep(250);
            continue;
        }
        size_t done = 0;
        bool ok = true;
        while (ok && done < n) {
            DWORD w = 0, want = (DWORD)std::min<size_t>(n - done, 1 << 22);
            ok = WriteFile(f, (const BYTE*)data + done, want, &w, nullptr) && w > 0;
            done += w;
        }
        CloseHandle(f);
        if (ok) return true;
    }
    return false;
}

static bool readAt(HANDLE f, uint64_t off, void* b, size_t n) {
    LARGE_INTEGER p; p.QuadPart = (LONGLONG)off;
    if (!SetFilePointerEx(f, p, nullptr, FILE_BEGIN)) return false;
    size_t done = 0;
    while (done < n) {
        DWORD r = 0, want = (DWORD)std::min<size_t>(n - done, 1 << 24);
        if (!ReadFile(f, (BYTE*)b + done, want, &r, nullptr) || r == 0) return false;
        done += r;
    }
    return true;
}

/// Looks for the payload appended to this exe. Without it the exe is the uninstaller.
static bool findPayload() {
    HANDLE f = CreateFileW(g_self.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz; GetFileSizeEx(f, &sz);
    g_exeSize = (uint64_t)sz.QuadPart;
    bool ok = false;
    unsigned char tail[16];
    if (g_exeSize > 64 && readAt(f, g_exeSize - 16, tail, 16) && memcmp(tail + 8, "TGDPAYLD", 8) == 0) {
        uint64_t n; memcpy(&n, tail, 8);
        if (n + 16 < g_exeSize) {
            unsigned char hd[20];
            uint64_t off = g_exeSize - 16 - n;
            if (readAt(f, off, hd, 20) && memcmp(hd, "TGDPACK1", 8) == 0) {
                memcpy(&g_count, hd + 8, 4);
                memcpy(&g_total, hd + 12, 8);
                g_payOff = off; g_paySize = n; g_exeSize = off;
                ok = true;
            }
        }
    }
    CloseHandle(f);
    return ok;
}

static bool createShortcut(const std::wstring& lnk, const std::wstring& target, const std::wstring& args, const std::wstring& desc) {
    IShellLinkW* sl = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW, (void**)&sl))) return false;
    sl->SetPath(target.c_str());
    sl->SetArguments(args.c_str());
    sl->SetWorkingDirectory(parentOf(target).c_str());
    sl->SetDescription(desc.c_str());
    sl->SetIconLocation(target.c_str(), 0);
    IPersistFile* pf = nullptr;
    bool ok = false;
    if (SUCCEEDED(sl->QueryInterface(IID_IPersistFile, (void**)&pf))) { ok = SUCCEEDED(pf->Save(lnk.c_str(), TRUE)); pf->Release(); }
    sl->Release();
    return ok;
}

static std::wstring startMenuLnk() { return knownFolder(FOLDERID_Programs) + L"\\TgDesk.lnk"; }
static std::wstring desktopLnk() { return knownFolder(FOLDERID_Desktop) + L"\\TgDesk.lnk"; }

static std::wstring normDir(std::wstring d) {
    while (d.size() > 3 && (d.back() == L'\\' || d.back() == L'/')) d.pop_back();
    wchar_t b[MAX_PATH * 2];
    DWORD n = GetFullPathNameW(d.c_str(), MAX_PATH * 2, b, nullptr);
    return n > 0 && n < MAX_PATH * 2 ? std::wstring(b) : d;
}

// ============================================================================ install
static bool doInstall() {
    HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    auto fail = [&](const std::wstring& e) { { std::lock_guard<std::mutex> l(g_mx); g_err = e; } if (SUCCEEDED(co)) CoUninitialize(); return false; };

    setStatus(tr(L"Закрываю запущенный TgDesk…", L"Closing running TgDesk…"), 0.f);
    stopTgDesk();

    if (SHCreateDirectoryExW(nullptr, g_dir.c_str(), nullptr) != ERROR_SUCCESS && !exists(g_dir))
        return fail(fmt(tr(L"Не удалось создать папку\n%ls", L"Cannot create the folder\n%ls"), g_dir.c_str()));

    ULARGE_INTEGER freeB;
    if (GetDiskFreeSpaceExW(g_dir.c_str(), &freeB, nullptr, nullptr) && freeB.QuadPart < g_total + 20 * 1024 * 1024)
        return fail(tr(L"Недостаточно места на диске.", L"Not enough disk space."));

    // old program files are replaced completely (user data lives elsewhere and is kept)
    rmTree(g_dir + L"\\bot");
    rmTree(g_dir + L"\\webapp");

    HANDLE f = CreateFileW(g_self.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return fail(tr(L"Не удалось прочитать установщик.", L"Cannot read the installer."));
    DECOMPRESSOR_HANDLE dc;
    if (!CreateDecompressor(COMPRESS_ALGORITHM_LZMS, nullptr, &dc)) { CloseHandle(f); return fail(L"CreateDecompressor"); }

    std::wstring list;
    uint64_t pos = g_payOff + 20, written = 0;
    std::vector<unsigned char> comp, data;
    bool ok = true;
    std::wstring err;
    for (uint32_t i = 0; ok && i < g_count; i++) {
        uint16_t nl = 0;
        if (!readAt(f, pos, &nl, 2)) { ok = false; break; }
        std::wstring rel(nl, 0);
        uint64_t sizes[2];
        if (!readAt(f, pos + 2, &rel[0], nl * 2) || !readAt(f, pos + 2 + nl * 2, sizes, 16)) { ok = false; break; }
        pos += 2 + nl * 2 + 16;
        if (rel.find(L"..") != std::wstring::npos || rel.find(L':') != std::wstring::npos) { ok = false; break; }
        comp.resize((size_t)sizes[1]);
        data.resize((size_t)sizes[0]);
        if (sizes[1] && !readAt(f, pos, comp.data(), comp.size())) { ok = false; break; }
        pos += sizes[1];
        SIZE_T got = 0;
        if (sizes[0] && (!Decompress(dc, comp.data(), comp.size(), data.data(), data.size(), &got) || got != sizes[0])) { ok = false; break; }
        setStatus(rel, g_total ? 0.04f + 0.86f * (float)written / (float)g_total : 0.5f);
        std::wstring dst = g_dir + L"\\" + rel;
        SHCreateDirectoryExW(nullptr, parentOf(dst).c_str(), nullptr);
        if (!writeAll(dst, data.data(), data.size())) { ok = false; err = fmt(tr(L"Не удалось записать файл\n%ls\n\nЗакрой TgDesk и попробуй ещё раз.", L"Cannot write the file\n%ls\n\nClose TgDesk and try again."), dst.c_str()); break; }
        written += sizes[0];
        list += rel + L"\r\n";
    }
    CloseDecompressor(dc);

    // uninstaller = this exe without the payload
    if (ok) {
        setStatus(tr(L"Создаю деинсталлятор…", L"Creating the uninstaller…"), 0.92f);
        std::vector<unsigned char> stub((size_t)g_exeSize);
        ok = readAt(f, 0, stub.data(), stub.size()) && writeAll(g_dir + L"\\" + UNINST_EXE, stub.data(), stub.size());
        if (!ok) err = tr(L"Не удалось создать деинсталлятор.", L"Cannot create the uninstaller.");
    }
    CloseHandle(f);
    if (!ok) return fail(err.empty() ? tr(L"Установщик повреждён. Скачай его заново.", L"The installer is damaged. Download it again.") : err);

    std::string ul = "\xEF\xBB\xBF";
    { int n = WideCharToMultiByte(CP_UTF8, 0, list.c_str(), (int)list.size(), nullptr, 0, nullptr, nullptr);
      std::string s(n, 0); WideCharToMultiByte(CP_UTF8, 0, list.c_str(), (int)list.size(), &s[0], n, nullptr, nullptr); ul += s; }
    writeAll(g_dir + L"\\uninstall.dat", ul.data(), ul.size());

    setStatus(tr(L"Создаю ярлыки…", L"Creating shortcuts…"), 0.95f);
    std::wstring exe = g_dir + L"\\TgDesk.exe";
    std::wstring desc = tr(L"TgDesk — пульт для своего ПК в Telegram", L"TgDesk — control your PC from Telegram");
    if (optStartMenu) createShortcut(startMenuLnk(), exe, L"", desc); else DeleteFileW(startMenuLnk().c_str());
    if (optDesktop) createShortcut(desktopLnk(), exe, L"", desc); else DeleteFileW(desktopLnk().c_str());

    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) == ERROR_SUCCESS) {
        if (optAutostart) regSet(k, APP, L"\"" + exe + L"\" --tray"); else RegDeleteValueW(k, APP);
        RegCloseKey(k);
    }

    setStatus(tr(L"Регистрирую в «Приложениях»…", L"Registering in Apps…"), 0.98f);
    if (RegCreateKeyExW(HKEY_CURRENT_USER, UNINST_KEY, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) == ERROR_SUCCESS) {
        std::wstring un = L"\"" + g_dir + L"\\" + UNINST_EXE + L"\"";
        SYSTEMTIME st; GetLocalTime(&st);
        regSet(k, L"DisplayName", APP);
        regSet(k, L"DisplayVersion", SETUP_VERSION);
        regSet(k, L"Publisher", AUTHOR);
        regSet(k, L"DisplayIcon", exe + L",0");
        regSet(k, L"InstallLocation", g_dir);
        regSet(k, L"UninstallString", un);
        regSet(k, L"QuietUninstallString", un + L" /S");
        regSet(k, L"URLInfoAbout", REPO);
        regSet(k, L"HelpLink", std::wstring(REPO) + L"/issues");
        regSet(k, L"InstallDate", fmt(L"%04d%02d%02d", st.wYear, st.wMonth, st.wDay));
        regSet(k, L"Comments", tr(L"Пульт для своего ПК в Telegram", L"Control your PC from Telegram"));
        regSetDw(k, L"EstimatedSize", (DWORD)((g_total + g_exeSize) / 1024));
        regSetDw(k, L"NoModify", 1);
        regSetDw(k, L"NoRepair", 1);
        RegCloseKey(k);
    }
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    setStatus(tr(L"Готово", L"Done"), 1.f);
    if (SUCCEEDED(co)) CoUninitialize();
    return true;
}

// ============================================================================ uninstall
static bool doUninstall() {
    setStatus(tr(L"Закрываю TgDesk…", L"Closing TgDesk…"), 0.05f);
    stopTgDesk();

    setStatus(tr(L"Удаляю ярлыки и автозапуск…", L"Removing shortcuts and autostart…"), 0.2f);
    DeleteFileW(startMenuLnk().c_str());
    DeleteFileW(desktopLnk().c_str());
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_SET_VALUE, &k) == ERROR_SUCCESS) { RegDeleteValueW(k, APP); RegCloseKey(k); }

    // only files that the installer wrote (uninstall.dat), so a hand-picked folder with other files stays intact
    std::vector<std::wstring> files;
    {
        HANDLE f = CreateFileW((g_dir + L"\\uninstall.dat").c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            LARGE_INTEGER sz; GetFileSizeEx(f, &sz);
            std::string s((size_t)std::min<LONGLONG>(sz.QuadPart, 4 << 20), 0);
            DWORD r = 0;
            ReadFile(f, &s[0], (DWORD)s.size(), &r, nullptr);
            CloseHandle(f);
            s.resize(r);
            if (s.rfind("\xEF\xBB\xBF", 0) == 0) s.erase(0, 3);
            int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
            std::wstring w(n, 0);
            MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
            size_t a = 0;
            while (a < w.size()) {
                size_t b = w.find(L"\r\n", a);
                if (b == std::wstring::npos) b = w.size();
                std::wstring line = w.substr(a, b - a);
                if (!line.empty() && line.find(L"..") == std::wstring::npos && line.find(L':') == std::wstring::npos) files.push_back(line);
                a = b + 2;
            }
        }
    }
    if (files.empty()) files = {L"TgDesk.exe", L"tgdesk.ico", L"LICENSE.txt", L"bot\\TgDeskBot.exe"};
    std::vector<std::wstring> dirs;
    for (size_t i = 0; i < files.size(); i++) {
        setStatus(files[i], 0.25f + 0.55f * (float)i / (float)files.size());
        std::wstring p = g_dir + L"\\" + files[i];
        SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL);
        DeleteFileW(p.c_str());
        for (std::wstring d = parentOf(files[i]); !d.empty(); d = parentOf(d)) dirs.push_back(d);
    }
    std::sort(dirs.begin(), dirs.end(), [](const std::wstring& a, const std::wstring& b) { return a.size() > b.size(); });
    for (auto& d : dirs) RemoveDirectoryW((g_dir + L"\\" + d).c_str());
    DeleteFileW((g_dir + L"\\uninstall.dat").c_str());
    // the bot's own folders that may be left over (empty after the files above)
    RemoveDirectoryW((g_dir + L"\\webapp").c_str());
    RemoveDirectoryW((g_dir + L"\\bot").c_str());

    if (optPurge) {
        setStatus(tr(L"Удаляю настройки и журналы…", L"Deleting settings and logs…"), 0.85f);
        std::wstring dd = dataDir();
        if (dd.size() > 6) rmTree(dd);
    }

    RegDeleteTreeW(HKEY_CURRENT_USER, UNINST_KEY);
    setStatus(tr(L"Готово", L"Done"), 1.f);
    return true;
}

/// The uninstaller cannot delete its own exe while running: a hidden cmd does it a moment after we exit.
static void scheduleSelfDelete() {
    std::wstring self = g_self;
    if (_wcsnicmp(parentOf(self).c_str(), g_dir.c_str(), g_dir.size()) != 0) return;
    wchar_t sys[MAX_PATH]; GetSystemDirectoryW(sys, MAX_PATH);
    std::wstring cmd = L"\"" + std::wstring(sys) + L"\\cmd.exe\" /d /c ping -n 3 127.0.0.1 >nul & del /f /q \"" + self +
                       L"\" & rmdir \"" + g_dir + L"\"";
    STARTUPINFOW si{}; si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, sys, &si, &pi)) {
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    }
}

static void launchApp(const wchar_t* args = nullptr) {
    std::wstring exe = g_dir + L"\\TgDesk.exe";
    ShellExecuteW(nullptr, L"open", exe.c_str(), args, g_dir.c_str(), SW_SHOWNORMAL);
}

static void runWork() {
    g_busy = true;
    g_page = g_mode == M_INSTALL ? 2 : 1;
    InvalidateRect(g_hwnd, nullptr, FALSE);
    std::thread([] {
        bool ok = g_mode == M_INSTALL ? doInstall() : doUninstall();
        g_ok = ok;
        Sleep(350);
        g_busy = false;
        g_page = g_mode == M_INSTALL ? 3 : 2;
        PostMessage(g_hwnd, WM_APP_PROGRESS, 0, 0);
    }).detach();
}

static void browseFolder() {
    IFileOpenDialog* d = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_IFileOpenDialog, (void**)&d))) return;
    DWORD o; d->GetOptions(&o);
    d->SetOptions(o | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    d->SetTitle(tr(L"Папка для TgDesk", L"Folder for TgDesk"));
    IShellItem* cur = nullptr;
    std::wstring start = exists(g_dir) ? g_dir : parentOf(g_dir);
    if (SUCCEEDED(SHCreateItemFromParsingName(start.c_str(), nullptr, IID_PPV_ARGS(&cur)))) { d->SetFolder(cur); cur->Release(); }
    if (SUCCEEDED(d->Show(g_hwnd))) {
        IShellItem* it = nullptr;
        if (SUCCEEDED(d->GetResult(&it))) {
            PWSTR p = nullptr;
            if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
                std::wstring s = normDir(p);
                // a folder of its own, like every program: ...\TgDesk
                std::wstring last = s.substr(s.find_last_of(L'\\') + 1);
                if (_wcsicmp(last.c_str(), L"TgDesk") != 0) s += L"\\TgDesk";
                g_dir = s;
                CoTaskMemFree(p);
            }
            it->Release();
        }
    }
    d->Release();
}

// ============================================================================ drawing
static const Color cWhite(255, 255, 255, 255);

static void logo(Graphics& g, RectF r) {
    gradRR(g, r, r.Width * 0.28f, cAcc1, cAcc2, 45.f);
    icon(g, 0xE7F4, r, r.Width / S * 0.44f, cWhite);
}

static void sidePanel(Graphics& g, float H) {
    RectF L(0, 0, 268 * S, H);
    { LinearGradientBrush br(L, Color(255, 46, 30, 120), Color(255, 10, 54, 92), 65.f, FALSE); g.FillRectangle(&br, L); }
    // soft decorative glows (kept inside the panel)
    g.SetClip(L);
    circle(g, 230 * S, 70 * S, 120 * S, Color(28, 124, 92, 255));
    circle(g, 30 * S, H - 60 * S, 150 * S, Color(30, 34, 211, 238));
    circle(g, 200 * S, H - 170 * S, 46 * S, Color(22, 255, 255, 255));
    g.ResetClip();

    logo(g, RectF(28 * S, 34 * S, 58 * S, 58 * S));
    text(g, L"TgDesk", RectF(100 * S, 36 * S, 160 * S, 30 * S), 23, cWhite, 0, 0, 1, FONT_SEMI);
    text(g, (g_mode == M_INSTALL ? std::wstring(tr(L"Установка ", L"Setup ")) : std::wstring(tr(L"Удаление ", L"Uninstall "))) + SETUP_VERSION,
         RectF(100 * S, 64 * S, 160 * S, 22 * S), 12.5f, Color(200, 255, 255, 255));

    std::vector<std::wstring> steps = g_mode == M_INSTALL
        ? std::vector<std::wstring>{tr(L"Приветствие", L"Welcome"), tr(L"Параметры", L"Options"), tr(L"Установка", L"Installing"), tr(L"Готово", L"Finish")}
        : std::vector<std::wstring>{tr(L"Подтверждение", L"Confirm"), tr(L"Удаление", L"Removing"), tr(L"Готово", L"Finish")};
    float y = 140 * S;
    for (size_t i = 0; i < steps.size(); i++) {
        bool cur = (int)i == g_page, done = (int)i < g_page || (g_page == (int)steps.size() - 1 && g_ok && cur);
        float cx = 44 * S, cy = y + 16 * S;
        if (i + 1 < steps.size()) {
            SolidBrush lb(Color(60, 255, 255, 255));
            g.FillRectangle(&lb, cx - 1 * S, cy + 13 * S, 2 * S, 30 * S);
        }
        if (cur) circle(g, cx, cy, 16 * S, Color(50, 255, 255, 255));
        circle(g, cx, cy, 12 * S, done ? cGreen : cur ? cWhite : Color(60, 255, 255, 255));
        if (done) icon(g, 0xE73E, RectF(cx - 12 * S, cy - 12 * S, 24 * S, 24 * S), 11, Color(255, 10, 40, 30));
        else text(g, std::to_wstring(i + 1), RectF(cx - 12 * S, cy - 12 * S, 24 * S, 24 * S), 11.5f, cur ? Color(255, 60, 40, 140) : cWhite, 0, 1, 1, FONT_SEMI);
        text(g, steps[i], RectF(72 * S, y, 180 * S, 32 * S), 14, cur ? cWhite : Color(190, 255, 255, 255), 0, 0, 1, cur ? FONT_SEMI : L"");
        y += 52 * S;
    }

    // author
    float by = H - 74 * S;
    text(g, tr(L"Автор", L"Author"), RectF(28 * S, by, 200 * S, 18 * S), 11.5f, Color(170, 255, 255, 255));
    text(g, AUTHOR, RectF(28 * S, by + 18 * S, 220 * S, 26 * S), 17, cWhite, 0, 0, 1, FONT_SEMI);
    text(g, L"\u00A9 2026 \u00B7 MIT License", RectF(28 * S, by + 44 * S, 220 * S, 18 * S), 11, Color(150, 255, 255, 255));
}

// a long path is shortened in the middle: C:\Users\…\Programs\TgDesk
static void pathText(Graphics& g, const std::wstring& s, RectF r) {
    StringFormat sf(StringFormatFlagsNoWrap);
    sf.SetLineAlignment(StringAlignmentCenter);
    sf.SetTrimming(StringTrimmingEllipsisPath);
    SolidBrush b(cText);
    g.DrawString(s.c_str(), -1, font(13), r, &sf, &b);
}

static void toggleRow(Graphics& g, int id, RectF r, const std::wstring& title, const std::wstring& sub, bool on, wchar_t glyph) {
    float h = hover(id);
    fillRR(g, r, 12 * S, lerp(cCard, Color(255, 28, 33, 56), h));
    strokeRR(g, r, 12 * S, cBorder);
    float tx = r.X + 16 * S;
    if (glyph) { icon(g, glyph, RectF(r.X + 12 * S, r.Y, 28 * S, r.Height), 16, cAcc2); tx = r.X + 50 * S; }
    if (sub.empty()) text(g, title, RectF(tx, r.Y, r.Width - (tx - r.X) - 70 * S, r.Height), 14, cText, 0, 0, 1, FONT_SEMI);
    else {
        text(g, title, RectF(tx, r.Y + 9 * S, r.Width - (tx - r.X) - 70 * S, 22 * S), 14, cText, 0, 0, 1, FONT_SEMI);
        text(g, sub, RectF(tx, r.Y + 31 * S, r.Width - (tx - r.X) - 70 * S, 18 * S), 12, cMuted);
    }
    addHit(id, r);
    toggle(g, id, r.X + r.Width - 60 * S, r.Y + (r.Height - 24 * S) / 2, on);
}

static void windowButtons(Graphics& g, float W) {
    RectF rc(W - 46 * S, 0, 46 * S, 34 * S), rm(W - 92 * S, 0, 46 * S, 34 * S);
    float hc = hover(ID_CLOSE), hm = hover(ID_MIN);
    if (hc > 0.01f) { SolidBrush b(alpha(cRed, (int)(200 * hc))); g.FillRectangle(&b, rc); }
    if (hm > 0.01f) { SolidBrush b(alpha(cCard2, (int)(255 * hm))); g.FillRectangle(&b, rm); }
    bool canClose = !g_busy;
    icon(g, 0xE8BB, rc, 10, canClose ? lerp(cMuted, cWhite, hc) : alpha(cMuted, 80));
    icon(g, 0xE921, rm, 10, lerp(cMuted, cWhite, hm));
    if (canClose) addHit(ID_CLOSE, rc);
    addHit(ID_MIN, rm);
}

static void paint(Graphics& g, float W, float H) {
    sidePanel(g, H);
    float x = 268 * S + 40 * S, cw = W - x - 40 * S;
    float by = H - 72 * S;   // bottom buttons
    RectF next(W - 40 * S - 170 * S, by, 170 * S, 44 * S), back(next.X - 12 * S - 130 * S, by, 130 * S, 44 * S);

    if (g_mode == M_INSTALL && g_page == 0) {
        bool same = g_oldVersion == SETUP_VERSION;
        text(g, !g_upgrade ? tr(L"Добро пожаловать!", L"Welcome!") : same ? tr(L"Переустановка TgDesk", L"Reinstall TgDesk") : tr(L"Обновление TgDesk", L"Update TgDesk"),
             RectF(x, 46 * S, cw, 40 * S), 27, cText, 0, 0, 1, FONT_SEMI);
        std::wstring sub = !g_upgrade ? std::wstring(tr(L"Пульт для своего Windows-ПК прямо в Telegram.", L"A remote for your own Windows PC, right in Telegram."))
                         : same ? fmt(tr(L"Версия %ls уже установлена — файлы программы будут заменены. Настройки сохранятся.", L"Version %ls is already installed: program files will be replaced. Settings are kept."), SETUP_VERSION)
                                : fmt(tr(L"Установлена версия %ls, будет установлена %ls. Настройки сохранятся.", L"Version %ls is installed, %ls will be installed. Settings are kept."),
                                      g_oldVersion.empty() ? L"?" : g_oldVersion.c_str(), SETUP_VERSION);
        text(g, sub, RectF(x, 86 * S, cw, 24 * S), 13.5f, cMuted);
        struct F { wchar_t g; const wchar_t *t, *d; } feats[] = {
            {0xE7F4, tr(L"Экран вживую", L"Live screen"), tr(L"Трансляция, мышь и клавиатура с телефона", L"Streaming, mouse and keyboard from your phone")},
            {0xE8B7, tr(L"Файлы и процессы", L"Files and processes"), tr(L"Скачивание, загрузка, просмотр медиа", L"Download, upload, preview media")},
            {0xE72E, tr(L"Всё под контролем", L"You stay in control"), tr(L"Журнал доступа, права пользователей, плашка трансляции", L"Access log, user permissions, streaming badge")},
        };
        float y = 130 * S;
        for (auto& f : feats) {
            fillRR(g, RectF(x, y, 40 * S, 40 * S), 11 * S, alpha(cAcc1, 50));
            icon(g, f.g, RectF(x, y, 40 * S, 40 * S), 17, cAcc2);
            text(g, f.t, RectF(x + 54 * S, y, cw - 54 * S, 20 * S), 14, cText, 0, 0, 1, FONT_SEMI);
            text(g, f.d, RectF(x + 54 * S, y + 20 * S, cw - 54 * S, 20 * S), 12.5f, cMuted);
            y += 54 * S;
        }
        RectF fr(x, y + 6 * S, cw, 108 * S);
        fillRR(g, fr, 14 * S, alpha(cAmber, 20));
        strokeRR(g, fr, 14 * S, alpha(cAmber, 90));
        textWrap(g, tr(L"TgDesk даёт управление этим компьютером через Telegram. Устанавливай его только на свой ПК или с согласия владельца. Во время трансляции на ПК всегда видна плашка, а все действия пишутся в журнал.",
                       L"TgDesk gives control of this computer through Telegram. Install it only on your own PC or with its owner's consent. While the screen is streamed, a badge is always visible on the PC and every action is logged."),
                 RectF(fr.X + 16 * S, fr.Y + 10 * S, fr.Width - 32 * S, 54 * S), 12, Color(255, 250, 225, 160));
        RectF ar(fr.X + 10 * S, fr.Y + 66 * S, fr.Width - 20 * S, 34 * S);
        float ha = hover(ID_AGREE);
        fillRR(g, ar, 9 * S, alpha(cAmber, (int)(25 * ha)));
        RectF box(ar.X + 6 * S, ar.Y + 7 * S, 20 * S, 20 * S);
        float t = approach(av, ID_AGREE, optAgree ? 1.f : 0.f, 0.3f);
        fillRR(g, box, 6 * S, lerp(cCard2, cAcc1, t));
        strokeRR(g, box, 6 * S, lerp(cBorder, cAcc1, t));
        if (t > 0.5f) icon(g, 0xE73E, box, 11, cWhite);
        text(g, tr(L"Я устанавливаю TgDesk на свой ПК или с согласия владельца", L"I install TgDesk on my own PC or with the owner's consent"),
             RectF(ar.X + 36 * S, ar.Y, ar.Width - 40 * S, ar.Height), 13, cText, 0, 0, 1, FONT_SEMI);
        addHit(ID_AGREE, ar);
        button(g, ID_NEXT, next, tr(L"Далее", L"Next"), Primary, 0, optAgree);
        button(g, ID_BACK, back, tr(L"Отмена", L"Cancel"), Ghost);
    } else if (g_mode == M_INSTALL && g_page == 1) {
        text(g, tr(L"Параметры установки", L"Install options"), RectF(x, 46 * S, cw, 40 * S), 27, cText, 0, 0, 1, FONT_SEMI);
        text(g, tr(L"Права администратора не нужны: TgDesk ставится только для тебя.", L"No administrator rights needed: TgDesk is installed just for you."), RectF(x, 86 * S, cw, 24 * S), 13.5f, cMuted);
        text(g, tr(L"Папка программы", L"Program folder"), RectF(x, 126 * S, cw, 20 * S), 12.5f, cMuted, 0, 0, 1, FONT_SEMI);
        RectF pr(x, 150 * S, cw, 50 * S);
        fillRR(g, pr, 12 * S, cCard2);
        strokeRR(g, pr, 12 * S, cBorder);
        icon(g, 0xE8B7, RectF(pr.X + 12 * S, pr.Y, 28 * S, pr.Height), 16, cAcc2);
        pathText(g, g_dir, RectF(pr.X + 48 * S, pr.Y, pr.Width - 48 * S - 132 * S, pr.Height));
        button(g, ID_BROWSE, RectF(pr.X + pr.Width - 124 * S, pr.Y + 7 * S, 116 * S, 36 * S), tr(L"Изменить…", L"Change…"), Secondary);
        text(g, fmt(tr(L"Нужно места: %.0f МБ · данные и настройки хранятся в %%APPDATA%%\\TgDesk", L"Space required: %.0f MB · data and settings live in %%APPDATA%%\\TgDesk"),
                    (g_total + g_exeSize) / 1048576.0), RectF(x, 206 * S, cw, 20 * S), 12, cMuted);
        float y = 240 * S;
        toggleRow(g, ID_T_DESK, RectF(x, y, cw, 46 * S), tr(L"Ярлык на рабочем столе", L"Desktop shortcut"), L"", optDesktop, 0xE7F8); y += 54 * S;
        toggleRow(g, ID_T_START, RectF(x, y, cw, 46 * S), tr(L"Ярлык в меню «Пуск»", L"Start menu shortcut"), L"", optStartMenu, 0xE700); y += 54 * S;
        toggleRow(g, ID_T_AUTO, RectF(x, y, cw, 46 * S), tr(L"Запускать вместе с Windows (в трее)", L"Start with Windows (in the tray)"), L"", optAutostart, 0xE7E8);
        button(g, ID_NEXT, next, g_upgrade ? tr(L"Обновить", L"Update") : tr(L"Установить", L"Install"), Primary, 0xE896);
        button(g, ID_BACK, back, tr(L"Назад", L"Back"), Secondary);
    } else if ((g_mode == M_INSTALL && g_page == 2) || (g_mode == M_UNINSTALL && g_page == 1)) {
        bool ins = g_mode == M_INSTALL;
        text(g, ins ? tr(L"Устанавливаю TgDesk…", L"Installing TgDesk…") : tr(L"Удаляю TgDesk…", L"Removing TgDesk…"), RectF(x, 46 * S, cw, 40 * S), 27, cText, 0, 0, 1, FONT_SEMI);
        text(g, tr(L"Это займёт несколько секунд.", L"This takes a few seconds."), RectF(x, 86 * S, cw, 24 * S), 13.5f, cMuted);
        float p = approach(av, 9999, g_prog.load(), 0.18f);
        RectF card_(x, 170 * S, cw, 130 * S);
        card(g, card_, true);
        text(g, fmt(L"%d%%", (int)std::lround(p * 100)), RectF(card_.X + 22 * S, card_.Y + 18 * S, 200 * S, 44 * S), 34, cText, 0, 0, 1, FONT_SEMI);
        bar(g, RectF(card_.X + 22 * S, card_.Y + 72 * S, card_.Width - 44 * S, 10 * S), p, ins ? cAcc1 : cRed, ins ? cAcc2 : cAmber);
        std::wstring cur; { std::lock_guard<std::mutex> l(g_mx); cur = g_cur; }
        text(g, cur, RectF(card_.X + 22 * S, card_.Y + 92 * S, card_.Width - 44 * S, 22 * S), 12, cMuted, 0, 0, 1, FONT_MONO);
        animating = animating || g_busy;
    } else if (g_mode == M_INSTALL && g_page == 3) {
        float cy = 120 * S;
        circle(g, x + 36 * S, cy, 36 * S, alpha(g_ok ? cGreen : cRed, 40));
        circle(g, x + 36 * S, cy, 26 * S, g_ok ? cGreen : cRed);
        icon(g, g_ok ? 0xE73E : 0xE711, RectF(x, cy - 36 * S, 72 * S, 72 * S), 22, Color(255, 10, 20, 30));
        if (g_ok) {
            text(g, g_upgrade ? tr(L"TgDesk обновлён", L"TgDesk is updated") : tr(L"TgDesk установлен", L"TgDesk is installed"), RectF(x, 176 * S, cw, 40 * S), 27, cText, 0, 0, 1, FONT_SEMI);
            textWrap(g, tr(L"При первом запуске откроется мастер настройки: он поможет создать бота в @BotFather и привязать твой Telegram. Нужные компоненты (туннель cloudflared) TgDesk скачает сам в свою папку.",
                           L"On first launch the setup wizard opens: it helps you create a bot in @BotFather and link your Telegram. TgDesk downloads the components it needs (the cloudflared tunnel) into its own folder by itself."),
                     RectF(x, 222 * S, cw, 80 * S), 13.5f, cMuted);
            toggleRow(g, ID_T_LAUNCH, RectF(x, 316 * S, cw, 46 * S), tr(L"Запустить TgDesk", L"Launch TgDesk"), L"", optLaunch, 0xE768);
            button(g, ID_NEXT, next, tr(L"Готово", L"Finish"), Primary);
        } else {
            text(g, tr(L"Установка не удалась", L"Installation failed"), RectF(x, 176 * S, cw, 40 * S), 27, cText, 0, 0, 1, FONT_SEMI);
            std::wstring e; { std::lock_guard<std::mutex> l(g_mx); e = g_err; }
            textWrap(g, e, RectF(x, 222 * S, cw, 140 * S), 13.5f, cMuted);
            button(g, ID_NEXT, next, tr(L"Закрыть", L"Close"), Secondary);
        }
    } else if (g_mode == M_UNINSTALL && g_page == 0) {
        text(g, tr(L"Удаление TgDesk", L"Uninstall TgDesk"), RectF(x, 46 * S, cw, 40 * S), 27, cText, 0, 0, 1, FONT_SEMI);
        text(g, tr(L"Будут удалены программа, ярлыки и автозапуск.", L"The program, shortcuts and autostart will be removed."), RectF(x, 86 * S, cw, 24 * S), 13.5f, cMuted);
        RectF pr(x, 130 * S, cw, 50 * S);
        fillRR(g, pr, 12 * S, cCard2);
        strokeRR(g, pr, 12 * S, cBorder);
        icon(g, 0xE8B7, RectF(pr.X + 12 * S, pr.Y, 28 * S, pr.Height), 16, cAcc2);
        pathText(g, g_dir, RectF(pr.X + 48 * S, pr.Y, pr.Width - 60 * S, pr.Height));
        toggleRow(g, ID_T_PURGE, RectF(x, 196 * S, cw, 60 * S), tr(L"Удалить также настройки, пользователей и журналы", L"Also delete settings, users and logs"),
                  dataDir(), optPurge, 0xE74D);
        if (optPurge)
            text(g, tr(L"Токен бота и список пользователей будут удалены без возможности восстановления.", L"The bot token and the user list will be deleted permanently."),
                 RectF(x, 264 * S, cw, 20 * S), 12, cAmber);
        button(g, ID_NEXT, next, tr(L"Удалить", L"Uninstall"), Danger, 0xE74D);
        button(g, ID_BACK, back, tr(L"Отмена", L"Cancel"), Secondary);
    } else if (g_mode == M_UNINSTALL && g_page == 2) {
        float cy = 120 * S;
        circle(g, x + 36 * S, cy, 36 * S, alpha(cGreen, 40));
        circle(g, x + 36 * S, cy, 26 * S, cGreen);
        icon(g, 0xE73E, RectF(x, cy - 36 * S, 72 * S, 72 * S), 22, Color(255, 10, 20, 30));
        text(g, tr(L"TgDesk удалён", L"TgDesk is removed"), RectF(x, 176 * S, cw, 40 * S), 27, cText, 0, 0, 1, FONT_SEMI);
        textWrap(g, optPurge ? tr(L"Программа и все данные удалены. Спасибо, что пользовался TgDesk!", L"The program and all data are removed. Thanks for using TgDesk!")
                             : tr(L"Программа удалена. Настройки остались в папке данных — при повторной установке TgDesk их подхватит. Спасибо, что пользовался TgDesk!",
                                  L"The program is removed. Settings stay in the data folder and will be picked up if you install TgDesk again. Thanks for using TgDesk!"),
                 RectF(x, 222 * S, cw, 80 * S), 13.5f, cMuted);
        text(g, tr(L"Не забудь удалить своего бота в @BotFather, если он больше не нужен.", L"Don't forget to delete your bot in @BotFather if you no longer need it."),
             RectF(x, 300 * S, cw, 20 * S), 12, cMuted);
        button(g, ID_NEXT, next, tr(L"Закрыть", L"Close"), Primary);
    }
    windowButtons(g, W);
}

// ============================================================================ window
static void onClick(int id) {
    switch (id) {
        case ID_CLOSE: if (!g_busy) DestroyWindow(g_hwnd); break;
        case ID_MIN: ShowWindow(g_hwnd, SW_MINIMIZE); break;
        case ID_AGREE: optAgree = !optAgree; break;
        case ID_T_DESK: optDesktop = !optDesktop; break;
        case ID_T_START: optStartMenu = !optStartMenu; break;
        case ID_T_AUTO: optAutostart = !optAutostart; break;
        case ID_T_LAUNCH: optLaunch = !optLaunch; break;
        case ID_T_PURGE: optPurge = !optPurge; break;
        case ID_BROWSE: browseFolder(); break;
        case ID_BACK:
            if (g_mode == M_INSTALL && g_page == 1) g_page = 0;
            else DestroyWindow(g_hwnd);
            break;
        case ID_NEXT:
            if (g_mode == M_INSTALL) {
                if (g_page == 0 && optAgree) g_page = 1;
                else if (g_page == 1) runWork();
                else if (g_page == 3) { if (g_ok && optLaunch) launchApp(); DestroyWindow(g_hwnd); }
            } else {
                if (g_page == 0) runWork();
                else if (g_page == 2) DestroyWindow(g_hwnd);
            }
            break;
    }
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(h, &ps);
            RECT rc; GetClientRect(h, &rc);
            int w = rc.right, ht = rc.bottom;
            if (w > 0 && ht > 0) {
                Bitmap bmp(w, ht, PixelFormat32bppPARGB);
                Graphics g(&bmp);
                g.SetSmoothingMode(SmoothingModeAntiAlias);
                g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
                g.SetPixelOffsetMode(PixelOffsetModeHalf);
                g.Clear(cBg);
                hits.clear();
                animating = false;
                off = PointF(0, 0);
                viewClip = RectF(0, 0, (REAL)w, (REAL)ht);
                paint(g, (float)w, (float)ht);
                Graphics out(dc);
                out.DrawImage(&bmp, 0, 0);
            }
            EndPaint(h, &ps);
            if (animating) SetTimer(h, 1, 15, nullptr); else KillTimer(h, 1);
            return 0;
        }
        case WM_ERASEBKGND: return 1;
        case WM_TIMER: InvalidateRect(h, nullptr, FALSE); return 0;
        case WM_APP_PROGRESS: InvalidateRect(h, nullptr, FALSE); return 0;
        case WM_MOUSEMOVE: {
            int nh = hitTest((float)GET_X_LPARAM(lp), (float)GET_Y_LPARAM(lp));
            if (nh != hot) { hot = nh; InvalidateRect(h, nullptr, FALSE); }
            SetCursor(LoadCursor(nullptr, hot ? IDC_HAND : IDC_ARROW));
            TRACKMOUSEEVENT t{sizeof t, TME_LEAVE, h, 0};
            TrackMouseEvent(&t);
            return 0;
        }
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT) { SetCursor(LoadCursor(nullptr, hot ? IDC_HAND : IDC_ARROW)); return TRUE; }
            break;
        case WM_MOUSELEAVE: hot = 0; InvalidateRect(h, nullptr, FALSE); return 0;
        case WM_LBUTTONDOWN: down = hot; SetCapture(h); InvalidateRect(h, nullptr, FALSE); return 0;
        case WM_LBUTTONUP: {
            ReleaseCapture();
            int d = down; down = 0;
            int nh = hitTest((float)GET_X_LPARAM(lp), (float)GET_Y_LPARAM(lp));
            if (d && d == nh) onClick(d);
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        case WM_NCHITTEST: {
            LRESULT r = DefWindowProc(h, msg, wp, lp);
            if (r != HTCLIENT) return r;
            POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(h, &p);
            if (p.y < 110 * S && hitTest((float)p.x, (float)p.y) == 0) return HTCAPTION;
            return HTCLIENT;
        }
        case WM_KEYDOWN:
            if (wp == VK_RETURN) {
                if ((g_mode == M_INSTALL && (g_page == 1 || g_page == 3 || (g_page == 0 && optAgree))) || (g_mode == M_UNINSTALL && g_page == 2)) onClick(ID_NEXT);
            } else if (wp == VK_ESCAPE && !g_busy) DestroyWindow(h);
            return 0;
        case WM_CLOSE: if (!g_busy) DestroyWindow(h); return 0;
        case WM_DPICHANGED: {
            S = HIWORD(wp) / 96.f;
            resetFonts();
            RECT* r = (RECT*)lp;
            SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProc(h, msg, wp, lp);
}

// ============================================================================ entry
static bool argHas(int argc, wchar_t** argv, const wchar_t* a) {
    for (int i = 1; i < argc; i++) if (_wcsicmp(argv[i], a) == 0) return true;
    return false;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    LANGID l = GetUserDefaultUILanguage();
    WORD p = PRIMARYLANGID(l);
    RU = p == LANG_RUSSIAN || p == LANG_UKRAINIAN || p == LANG_BELARUSIAN;

    wchar_t self[MAX_PATH * 2];
    GetModuleFileNameW(nullptr, self, MAX_PATH * 2);
    g_self = self;

    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    g_silent = argHas(argc, argv, L"/S");
    bool isUpdate = argHas(argc, argv, L"/update");   // started by TgDesk itself: start it again afterwards
    bool hasPayload = findPayload();
    g_mode = (!hasPayload || argHas(argc, argv, L"/uninstall")) ? M_UNINSTALL : M_INSTALL;
    optPurge = argHas(argc, argv, L"/purge");
    std::wstring dirArg;
    for (int i = 1; i < argc; i++) if (_wcsnicmp(argv[i], L"/D=", 3) == 0) dirArg = argv[i] + 3;
    LocalFree(argv);

    // only one installer/uninstaller at a time
    HANDLE mtx = CreateMutexW(nullptr, TRUE, L"Local\\TgDeskSetup_Singleton");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND other = FindWindowW(L"TgDeskSetupWnd", nullptr);
        if (other) { ShowWindow(other, SW_RESTORE); SetForegroundWindow(other); }
        return 1;
    }

    std::wstring reg = normDir(regGet(HKEY_CURRENT_USER, UNINST_KEY, L"InstallLocation"));
    g_oldVersion = regGet(HKEY_CURRENT_USER, UNINST_KEY, L"DisplayVersion");
    if (g_mode == M_INSTALL) {
        g_upgrade = !regGet(HKEY_CURRENT_USER, UNINST_KEY, L"InstallLocation").empty() && exists(reg + L"\\TgDesk.exe");
        g_dir = !dirArg.empty() ? normDir(dirArg) : g_upgrade ? reg : knownFolder(FOLDERID_LocalAppData) + L"\\Programs\\TgDesk";
        if (g_upgrade) {
            // keep what the user chose last time
            optDesktop = exists(desktopLnk());
            optStartMenu = exists(startMenuLnk());
            optAutostart = !regGet(HKEY_CURRENT_USER, RUN_KEY, APP).empty();
        }
    } else {
        g_dir = normDir(parentOf(g_self));
        if (!exists(g_dir + L"\\TgDesk.exe") && !reg.empty()) g_dir = reg;
        // let the folder be removed: do not keep it as our current directory
        wchar_t tmp[MAX_PATH]; GetTempPathW(MAX_PATH, tmp);
        SetCurrentDirectoryW(tmp);
    }

    if (g_silent) {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        bool ok = g_mode == M_INSTALL ? doInstall() : doUninstall();
        if (g_mode == M_UNINSTALL) scheduleSelfDelete();
        // after an update TgDesk comes back in the tray (it starts the bot); a failed update restarts the old version
        if (g_mode == M_INSTALL && isUpdate && exists(g_dir + L"\\TgDesk.exe")) launchApp(L"--tray");
        CloseHandle(mtx);
        return ok ? 0 : 1;
    }

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    GdiplusStartupInput gsi;
    ULONG_PTR gtok;
    GdiplusStartup(&gtok, &gsi, nullptr);

    UINT dpi = GetDpiForSystem();
    S = dpi / 96.f;
    {
        FontFamily fluent(L"Segoe Fluent Icons");
        if (fluent.IsAvailable()) FONT_ICON = L"Segoe Fluent Icons";
    }

    WNDCLASSEXW wc{sizeof wc};
    wc.style = CS_DROPSHADOW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hIconSm = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"TgDeskSetupWnd";
    RegisterClassExW(&wc);

    int w = (int)(860 * S), ht = (int)(540 * S);
    RECT wa; SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    int x = wa.left + (wa.right - wa.left - w) / 2, y = wa.top + (wa.bottom - wa.top - ht) / 2;
    std::wstring title = g_mode == M_INSTALL ? std::wstring(tr(L"Установка TgDesk", L"TgDesk Setup")) : std::wstring(tr(L"Удаление TgDesk", L"Uninstall TgDesk"));
    g_hwnd = CreateWindowExW(WS_EX_APPWINDOW, wc.lpszClassName, title.c_str(), WS_POPUP | WS_MINIMIZEBOX | WS_SYSMENU,
                             x, y, w, ht, nullptr, nullptr, inst, nullptr);
    BOOL dark = TRUE;
    DwmSetWindowAttribute(g_hwnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof dark);
    int corner = 2; /*DWMWCP_ROUND*/
    DwmSetWindowAttribute(g_hwnd, 33 /*DWMWA_WINDOW_CORNER_PREFERENCE*/, &corner, sizeof corner);
    MARGINS m{0, 0, 0, 1};
    DwmExtendFrameIntoClientArea(g_hwnd, &m);
    ShowWindow(g_hwnd, SW_SHOW);
    SetForegroundWindow(g_hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) { TranslateMessage(&msg); DispatchMessageW(&msg); }

    if (g_mode == M_UNINSTALL && g_ok) scheduleSelfDelete();
    resetFonts();
    GdiplusShutdown(gtok);
    CoUninitialize();
    CloseHandle(mtx);
    return 0;
}
