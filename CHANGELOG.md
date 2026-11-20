# Changelog

All notable changes are listed here. Format: [Keep a Changelog](https://keepachangelog.com/), versions: [SemVer](https://semver.org/).

## [1.0.0] — 2026-11-19

First public release.

### Added
- Own installer and uninstaller (C++, dark design, author k1llerov™): one `TgDesk-Setup-x.y.z.exe` with everything inside,
  upgrade detection, folder choice, shortcuts, autostart, entry in Windows "Apps", silent mode `/S`, `/D=`, `/purge`.
- "Uninstall TgDesk" button and the author on the About page.
- Clipboard between the phone and the PC: a card on the Remote tab and the `/clip` bot command.
  Writing needs the "mouse & keyboard" permission, reading also "screen"; only the text length is logged.
- `cloudflared` is downloaded ahead of time on the first start, while the setup wizard is still open.
- Setup wizard in the desktop app: bot token check, owner pairing with a one-time code, feature selection.
- Multiple users with per-user permissions (screen, mouse & keyboard, files, processes, media, power) and global feature switches.
- Pairing of new users only from the PC: single-use 6-digit code, 5 minutes, locked after 5 wrong attempts.
- Russian and English in the desktop app, bot (per Telegram client language) and Mini App.
- Bot token and Cloudflare Tunnel token encrypted with Windows DPAPI.
- Mini App address modes: Cloudflare Quick Tunnel, own Cloudflare Tunnel, own HTTPS address.
- `cloudflared` download from the official Cloudflare release with SHA-256 verification.
- Updates from GitHub Releases: "Update now" button and automatic install (on by default, never during a stream).
  Only releases whose `SHA256SUMS.txt` is signed with the author's ECDSA key are installed; the installer restarts
  TgDesk and owners get "updated X → Y" in Telegram. The portable build only reports new versions.
- Installer (per-user, no admin rights), portable mode, GitHub Actions build and release.

### Security
- The "screen is being streamed" badge, the tray icon and the access log cannot be disabled.
- The bot ignores group chats and strangers; the control API rejects browser requests (Origin header).
- Bot tokens are redacted from the technical log.
