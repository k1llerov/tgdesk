# Translating TgDesk / Перевод

Russian is the source language: user-visible texts are written in Russian directly in the code.
English (and any future language) is a table "Russian text → translation":

| File | Used by |
|---|---|
| `i18n/bot.en.json` | the bot (`bot/*.cs`) → generated `bot/Strings.cs` |
| `i18n/app.en.json` | the desktop app (`app/main.cpp`) → generated `app/i18n_en.h` |
| `i18n/web.en.json` | the Mini App and the log page → table inside `webapp/index.html` and `webapp/logs.html` |

## Changing a text

1. Edit the Russian text in the code.
   - C#: `L.T("…", lang)`, `L.F("… {0}", lang, x)`, PC side: `L.Pc("…")`.
   - C++: `T(L"…")`, with printf placeholders: `fmt(T(L"… %d"), n)`.
   - Web: static HTML text is translated automatically; in JS use `t('…')` or `t('… {0}', x)`.
2. Run `python scripts/i18n.py --missing` — it prints the texts without translation.
3. Add them to the right `i18n/*.en.json` file.
4. Run `python scripts/i18n.py` to regenerate the tables. CI runs `--check` and fails on missing texts.

Keep placeholders identical (`{0}`, `%d`, `%ls`) — the script checks it.

## Adding a new language

The code currently switches between Russian and English. To add a language, add `i18n/*.<lang>.json` files, extend the generator in `scripts/i18n.py` and the language choice in the app (`applyLang`), the bot (`Cfg.TgLang`, `L.T`) and the Mini App (`LANG`). Open an issue first — happy to help.
