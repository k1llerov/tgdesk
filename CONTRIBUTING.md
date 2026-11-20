# Contributing / Как помочь проекту

Спасибо за интерес! Issues и pull requests приветствуются — на русском или английском.

## Before you start

- For bigger changes open an issue first to agree on the approach.
- Keep the fair-use guarantees: the streaming badge, the tray icon and the access log must stay impossible to disable. PRs that hide TgDesk or add covert monitoring will not be accepted.

## Development setup

- **.NET 10 SDK**, **MinGW-w64 g++** (`winget install BrechtSanders.WinLibs.POSIX.UCRT`), **Python 3**, optionally **Inno Setup 6**.
- `.\scripts\build.ps1` builds everything into `dist\`. Run `dist\TgDesk.exe`.
- To keep test data away from your real setup, set `TGDESK_HOME` to a temporary folder before starting the app and the bot.
- Use a separate test bot from @BotFather — one bot token can be used by only one running bot.

## Code

- `bot/` — C#: keep the code simple and synchronous where possible; every action that touches the PC goes through a permission check (`Users.Can`) and the access log (`App.Alog`).
- `app/` — C++17 with Win32 + GDI+ only, no extra libraries.
- `webapp/` — plain HTML/JS, no frameworks or build step.
- Write user-visible texts in Russian in the code and add English to `i18n/*.en.json`, then run `python scripts/i18n.py` (see [docs/TRANSLATING.md](docs/TRANSLATING.md)).

## Checks

```powershell
python scripts/i18n.py --check
dotnet test tests/TgDesk.Tests/TgDesk.Tests.csproj
.\scripts\build.ps1
```

## Releasing (maintainers)

1. Update `VERSION`, `Cfg.Version` in `bot/Core.cs`, `APP_VERSION` in `app/main.cpp`, `<Version>` in `bot/TgDeskBot.csproj`, `app/app.rc` and `CHANGELOG.md`.
2. Commit, tag `vX.Y.Z`, push the tag. GitHub Actions builds the installer, portable zip and checksums, signs the checksums and creates a draft release.
3. Check the draft (it must contain `SHA256SUMS.txt.sig`) and publish it. Installed copies pick it up within 12 hours.

### Update signing key

Installed TgDesk installs an update only if `SHA256SUMS.txt` is signed with the key whose public part is in
`bot/UpdateSignature.cs`. The private key belongs to the maintainer only:

- it lives outside the repository (`%USERPROFILE%\.tgdesk-release\update-signing-key.txt`) — **back it up offline**;
- on GitHub it is the repository secret `UPDATE_SIGNING_KEY` (Settings → Secrets and variables → Actions);
- a tag build without the secret fails on purpose;
- a new key: `dotnet run --project tools/UpdateSign -- keygen <file>`, then put the printed public key into
  `bot/UpdateSignature.cs`. Copies with the old key will not auto-update to builds signed with the new one —
  they have to be updated by hand once. Never publish the private key; if it leaks, rotate it and say so in the release notes.
