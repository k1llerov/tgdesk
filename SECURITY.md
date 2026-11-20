# Security / Безопасность

## Reporting a vulnerability / Как сообщить об уязвимости

**Please do not open a public issue for security problems.** Use GitHub's private reporting:
[Security → Report a vulnerability](https://github.com/k1llerov/tgdesk/security/advisories/new).
Describe what an attacker can do, how to reproduce it, and the TgDesk version. You will get an answer within a week.

**Не публикуй уязвимости в открытых issue.** Используй приватное сообщение через GitHub:
[Security → Report a vulnerability](https://github.com/k1llerov/tgdesk/security/advisories/new).

Only the latest release is supported with security fixes.

## Design / Как устроена защита

| Threat | Protection |
|---|---|
| A stranger finds the bot | The bot answers only users from `users.json`, only in private chats. Strangers get no reply (attempts are logged). Group chats are ignored. |
| Adding yourself as a user | Users are added only from the desktop app on the PC. The pairing code is 6 digits, single-use, valid 5 minutes and burns after 5 wrong attempts. With no active pairing, codes are rejected. |
| Forged Mini App requests | Every API call and the streaming WebSocket check the Telegram `initData` HMAC signature (max age 12 h) and the user's permissions. 8 failures from one IP within 10 minutes block it for 15 minutes. |
| Host spoofing / other sites | The Mini App port accepts only the configured public host (`*.trycloudflare.com` in Quick Tunnel mode) or `127.0.0.1`. |
| Reaching the log or control API from outside | Both listen on `127.0.0.1` only and refuse requests that came through Cloudflare. The control API also needs a random token (`control.token`, new on every start) and rejects any request with an `Origin` header (browsers). |
| Stolen config file | The bot token and the Cloudflare tunnel token are encrypted with Windows DPAPI (current user). `config.json`, `users.json`, `control.token` and the access log are readable only by the current user and SYSTEM. |
| Token leaks through logs | Bot tokens are redacted from the technical log. |
| Tampered `cloudflared` | Downloaded only from `github.com/cloudflare/cloudflared` releases and checked against the SHA-256 published in the release notes. |
| Malicious update (stolen GitHub account, swapped release files) | Automatic updates are installed only if `SHA256SUMS.txt` carries a valid ECDSA P-256 signature (`SHA256SUMS.txt.sig`) made with the author's private key; the public key is built into the program (`bot/UpdateSignature.cs`). The installer's SHA-256 must match that signed list. Anything else is only reported, never installed. Updates never start during a screen stream. The portable build never updates itself. |
| Covert monitoring | Not supported by design: the streaming badge on the PC, the tray icon and the access log cannot be turned off. |
| Dangerous actions by mistake | Sleep, restart, shut down, Alt+F4 and running programs need confirmation. TgDesk itself and critical Windows processes cannot be killed remotely. |

## Limits / Ограничения

- Anyone who has your **Telegram account** and was added as a user can control the PC. Turn on two-step verification in Telegram.
- Windows does not allow input into elevated (administrator) windows or the lock screen.
- Quick Tunnel addresses are public HTTPS URLs; they are protected by the Telegram signature check, not by obscurity.
- The access log optionally sends remote IPs to `ipwho.is` to show city/ISP. Turn it off in Settings → Network and updates.
