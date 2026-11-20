// Interface language. Russian is the source language: T(L"…") returns the English text from i18n_en.h when English is on.
// To add a language see docs/TRANSLATING.md.
#pragma once
#include <string>
#include <unordered_map>

namespace i18n {

inline bool english = false;

inline const std::unordered_map<std::wstring, const wchar_t*>& table() {
    static const std::unordered_map<std::wstring, const wchar_t*> t = {
#include "i18n_en.h"
    };
    return t;
}

} // namespace i18n

inline const wchar_t* T(const wchar_t* ru) {
    if (!i18n::english) return ru;
    auto& t = i18n::table();
    auto it = t.find(ru);
    return it == t.end() ? ru : it->second;
}
inline std::wstring T(const std::wstring& ru) { return T(ru.c_str()); }
