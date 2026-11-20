// Client of the local bot control API (127.0.0.1:CONTROL_PORT, header X-Control-Token).
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#include <shlobj.h>
#include <string>
#include <fstream>
#include <sstream>
#include <mutex>
#include "json.h"

struct HttpResult {
    int status = 0;        // 0 — no connection
    std::string body;
};

namespace api {

inline std::wstring installDir;  // folder of TgDesk.exe
inline std::wstring dataDir;     // %APPDATA%\TgDesk (config.json, control.token, logs)
inline int controlPort = 8767;
inline int logPort = 8766;

inline std::string readFile(const std::wstring& path) {
    std::ifstream f(path.c_str(), std::ios::binary);
    if (!f) return "";
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

inline std::string trim(std::string s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) i++;
    return s.substr(i);
}

inline bool exists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

// Same rules as the bot: TGDESK_HOME → portable.txt next to TgDesk.exe → %APPDATA%\TgDesk
inline void resolveDirs() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    installDir = exe;
    installDir = installDir.substr(0, installDir.find_last_of(L"\\/"));
    wchar_t env[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"TGDESK_HOME", env, MAX_PATH);
    if (n > 0 && n < MAX_PATH) { dataDir = env; return; }
    if (exists(installDir + L"\\portable.txt")) { dataDir = installDir + L"\\data"; return; }
    PWSTR roaming = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &roaming))) {
        dataDir = std::wstring(roaming) + L"\\TgDesk";
        CoTaskMemFree(roaming);
    } else dataDir = installDir + L"\\data";
}

inline JV readConfig() {
    JV j;
    parseJson(readFile(dataDir + L"\\config.json"), j);
    return j;
}

// ports from config.json → "ports"
inline void loadPorts() {
    JV j = readConfig();
    int c = j["ports"]["control"].i(8767), l = j["ports"]["log"].i(8766);
    if (c > 0 && c < 65536) controlPort = c;
    if (l > 0 && l < 65536) logPort = l;
}

inline HttpResult request(const wchar_t* method, const std::wstring& path, const std::string& body = "", int timeoutMs = 4000) {
    HttpResult res;
    std::string token = trim(readFile(dataDir + L"\\control.token"));
    HINTERNET ses = WinHttpOpen(L"TgDesk-App/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY, nullptr, nullptr, 0);
    if (!ses) return res;
    WinHttpSetTimeouts(ses, timeoutMs, timeoutMs, timeoutMs, timeoutMs);
    HINTERNET con = WinHttpConnect(ses, L"127.0.0.1", (INTERNET_PORT)controlPort, 0);
    HINTERNET req = con ? WinHttpOpenRequest(con, method, path.c_str(), nullptr, nullptr, nullptr, 0) : nullptr;
    if (req) {
        std::wstring hdr = L"Content-Type: application/json\r\nX-Control-Token: " + std::wstring(token.begin(), token.end());
        BOOL ok = WinHttpSendRequest(req, hdr.c_str(), (DWORD)-1L, body.empty() ? WINHTTP_NO_REQUEST_DATA : (LPVOID)body.data(),
                                     (DWORD)body.size(), (DWORD)body.size(), 0);
        if (ok) ok = WinHttpReceiveResponse(req, nullptr);
        if (ok) {
            DWORD code = 0, sz = sizeof code;
            WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &code, &sz, nullptr);
            res.status = (int)code;
            DWORD avail = 0;
            while (WinHttpQueryDataAvailable(req, &avail) && avail > 0) {
                std::string chunk(avail, '\0');
                DWORD got = 0;
                if (!WinHttpReadData(req, &chunk[0], avail, &got) || got == 0) break;
                res.body.append(chunk, 0, got);
            }
        }
    }
    if (req) WinHttpCloseHandle(req);
    if (con) WinHttpCloseHandle(con);
    WinHttpCloseHandle(ses);
    return res;
}

// The bot moves to other ports when the defaults are busy (another program may even answer on them),
// and writes them to config.json: re-read the ports whenever that file changes.
inline HttpResult requestRetry(const wchar_t* method, const std::wstring& path, const std::string& body, int timeoutMs) {
    static FILETIME seen{};
    static std::mutex mx;
    std::lock_guard<std::mutex> lock(mx);   // requests come from several threads
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (GetFileAttributesExW((dataDir + L"\\config.json").c_str(), GetFileExInfoStandard, &fa) &&
        CompareFileTime(&fa.ftLastWriteTime, &seen) != 0) {
        seen = fa.ftLastWriteTime;
        loadPorts();
    }
    return request(method, path, body, timeoutMs);
}
inline HttpResult get(const std::wstring& path, int timeoutMs = 4000) { return requestRetry(L"GET", path, "", timeoutMs); }
inline HttpResult post(const std::wstring& path, const std::string& body = "{}", int timeoutMs = 6000) { return requestRetry(L"POST", path, body, timeoutMs); }

} // namespace api
