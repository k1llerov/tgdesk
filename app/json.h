// Minimal JSON parser for the local bot API responses.
#pragma once
#include <string>
#include <vector>
#include <utility>
#include <cstdlib>

struct JV {
    enum T { Null, Bool, Num, Str, Arr, Obj } t = Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<JV> a;
    std::vector<std::pair<std::string, JV>> o;

    const JV& operator[](const char* k) const {
        static const JV nul;
        for (auto& p : o) if (p.first == k) return p.second;
        return nul;
    }
    const JV& at(size_t i) const {
        static const JV nul;
        return i < a.size() ? a[i] : nul;
    }
    size_t size() const { return t == Arr ? a.size() : o.size(); }
    bool isNull() const { return t == Null; }
    std::string str(const std::string& d = "") const { return t == Str ? s : d; }
    double num(double d = 0) const { return t == Num ? n : d; }
    int i(int d = 0) const { return t == Num ? (int)n : d; }
    bool boolean(bool d = false) const { return t == Bool ? b : d; }
};

class JsonParser {
    const std::string& s;
    size_t p = 0;
    bool ok = true;

    void ws() { while (p < s.size() && (s[p] == ' ' || s[p] == '\t' || s[p] == '\n' || s[p] == '\r')) p++; }

    static void utf8(std::string& out, unsigned cp) {
        if (cp < 0x80) out += (char)cp;
        else if (cp < 0x800) { out += (char)(0xC0 | (cp >> 6)); out += (char)(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { out += (char)(0xE0 | (cp >> 12)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
        else { out += (char)(0xF0 | (cp >> 18)); out += (char)(0x80 | ((cp >> 12) & 0x3F)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
    }

    unsigned hex4() {
        unsigned v = 0;
        for (int k = 0; k < 4 && p < s.size(); k++, p++) {
            char c = s[p];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= c - '0';
            else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
            else ok = false;
        }
        return v;
    }

    std::string str() {
        std::string out;
        p++; // "
        while (p < s.size() && s[p] != '"') {
            char c = s[p++];
            if (c != '\\') { out += c; continue; }
            if (p >= s.size()) break;
            char e = s[p++];
            switch (e) {
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'r': out += '\r'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'u': {
                    unsigned cp = hex4();
                    if (cp >= 0xD800 && cp < 0xDC00 && p + 1 < s.size() && s[p] == '\\' && s[p + 1] == 'u') {
                        p += 2;
                        unsigned lo = hex4();
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    utf8(out, cp);
                    break;
                }
                default: out += e;
            }
        }
        p++; // "
        return out;
    }

    JV val() {
        JV v;
        ws();
        if (p >= s.size()) { ok = false; return v; }
        char c = s[p];
        if (c == '{') {
            v.t = JV::Obj; p++; ws();
            if (p < s.size() && s[p] == '}') { p++; return v; }
            while (ok && p < s.size()) {
                ws();
                if (s[p] != '"') { ok = false; break; }
                std::string k = str();
                ws();
                if (p >= s.size() || s[p] != ':') { ok = false; break; }
                p++;
                v.o.emplace_back(k, val());
                ws();
                if (p < s.size() && s[p] == ',') { p++; continue; }
                if (p < s.size() && s[p] == '}') { p++; break; }
                ok = false;
            }
        } else if (c == '[') {
            v.t = JV::Arr; p++; ws();
            if (p < s.size() && s[p] == ']') { p++; return v; }
            while (ok && p < s.size()) {
                v.a.push_back(val());
                ws();
                if (p < s.size() && s[p] == ',') { p++; continue; }
                if (p < s.size() && s[p] == ']') { p++; break; }
                ok = false;
            }
        } else if (c == '"') {
            v.t = JV::Str; v.s = str();
        } else if (s.compare(p, 4, "true") == 0) { v.t = JV::Bool; v.b = true; p += 4; }
        else if (s.compare(p, 5, "false") == 0) { v.t = JV::Bool; v.b = false; p += 5; }
        else if (s.compare(p, 4, "null") == 0) { p += 4; }
        else {
            char* end = nullptr;
            v.n = strtod(s.c_str() + p, &end);
            if (end == s.c_str() + p) { ok = false; return v; }
            v.t = JV::Num; p = end - s.c_str();
        }
        return v;
    }

public:
    explicit JsonParser(const std::string& src) : s(src) {}
    bool parse(JV& out) {
        out = val();
        return ok;
    }
};

inline bool parseJson(const std::string& s, JV& out) {
    JsonParser jp(s);
    return jp.parse(out);
}

// Escaping a string for a JSON request body
inline std::string jsonEscape(const std::string& in) {
    std::string o;
    for (unsigned char c : in) {
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o += b; }
                else o += (char)c;
        }
    }
    return o;
}
