# Nextendo Online

ACNHRecomp can use [Nextendo Network](https://nextendo.network) for Animal Crossing’s online features. Nintendo’s servers are never contacted. The recompiled game still speaks NEX, Pia and its own HTTPS services itself; the runtime supplies what a Switch on Nextendo would: sockets, DNS redirects, TLS, the account token and friends.

## What works, and how

Nextendo supports ACNH **3.0.3** fully online: island visits (friends and Dodo Codes), best friends and their messages, Orville’s letters and gifts, Luna’s dreams, Custom and Pro Designs, Happy Home Network and NookLink (see the [game page](https://wiki.nextendo.network/games/animal-crossing-new-horizons)). The runtime pieces behind them:

| Piece | File | What it does |
|---|---|---|
| Account sign-in | `nextendo.cpp` | OAuth 2.0 PKCE in the system browser, loopback redirect on `127.0.0.1`. Tokens refresh before they expire (access token 1 h, game token 24 h). |
| Game identity | `nextendo_token.cpp`, `svc_account.cpp` | `EnsureIdTokenCacheAsync` renews the tokens; `LoadIdTokenCache` returns an RS256 BAAS `id_token` whose `nnex` claim holds the game token from `GET /api/nex-token`. The server checks that token at NEX login and on the island services. |
| Account id | `nextendo.cpp` | The network service account id is the account’s baasUserID (`nsa` from `GET /api/me`), the id a Switch on the same account announces. ACNH matches it against friend lists, so emulator and Switch players recognise each other. |
| Friends | `hle_friends.cpp` | `GET /api/friends`, cached (20 s), mapped to `nn::friends::Friend` with each friend’s baasUserID, presence and favourite flag. |
| Presence | `nextendo.cpp` | `POST /api/presence` every 60 s while online (the server shows a player offline after 90 s), status 2 inside an online session. |
| DNS | `nextendo.cpp`, `hle_hostnet.cpp` | Nintendo names resolve to the Nextendo server; the NAT-check names to the second NAT address. Other names resolve normally. |
| Sockets | `hle_hostnet.cpp` | Host TCP/UDP sockets with the Switch’s ABI: option numbers, errno values, poll, non-blocking I/O. |
| TLS | `hle_ssl.cpp` | `nn::ssl` (libcurl’s C API and NEX’s C++ calls) on [Mbed TLS](https://github.com/Mbed-TLS/mbedtls) 3.6, fetched by CMake. SNI is the host the game named, or the one its address was resolved from. Nextendo presents certificates for Nintendo names, so instead of the certificate chain the server's public key is checked: the SHA-256 of its SubjectPublicKeyInfo must be one of the pins in `hle_ssl.cpp` (or `online.nextendo.tls_pins`), after the handshake and before anything is sent. Mbed TLS is built with its threading layer (`runtime/src/mbedtls/`). ALPN offers `http/1.1` only. |

Not yet on Nextendo’s side (server updates): best friends between an emulator and a real Switch. Island Backup is kept on this computer instead (Nextendo caps cloud saves at 5 MB): its backups go to `user/backups/` (README, *Saves*).

## Modes (`online.mode` in `config.ini`)

| Value | Default | Behaviour |
|---|---|---|
| `nextendo` | yes | Sign in with a Nextendo account. Airport Online uses Nextendo. |
| `local` | | Nextendo off: no account, no online services. |

Local Play peer visits ([LOCAL_PLAY.md](LOCAL_PLAY.md)) are separate and work in both modes when `online.enabled=1`.

## Requirements

- Game version **3.0.3** exactly. Nextendo refuses other versions online (error 2306-0819). Other versions can still use Local mode.
- A Nextendo account in good standing: confirmed e-mail, linked Discord account. A login is refused, with a generic game error, otherwise, or while the account is online elsewhere.
- `curl` on `PATH` (as for the launcher’s update check) for the account API on Windows, macOS and Linux. Android has no curl: the app makes those requests with `HttpURLConnection` (Android's own TLS and certificates), and sign-in opens the phone's browser, which returns to the app on `127.0.0.1`. Keep the game open in the background while signing in: Android may close it if it needs the memory, and then the sign-in has to be started again.

## Setup

1. ACNHRecomp ships with a registered public OAuth (PKCE) application (`nxc_t63scKVq2hF1`). To use your own, register one at [nextendo.network/developers](https://nextendo.network/developers) (public, redirect `http://127.0.0.1/callback`) and set:

```ini
[online]
mode = nextendo
nextendo.client_id = nxc_your_client_id
```

   Or set `NEXTENDO_CLIENT_ID` in the environment.

2. In the launcher or the in-game Online settings, choose **Nextendo Online** and **Sign in**. A browser opens on nextendo.network; approve the permissions (`identity friends presence game.matchmaking`). The password never reaches ACNHRecomp.
3. Launch **3.0.3** and play online as on a Switch linked to Nextendo.

Tokens are stored in `<data root>/nextendo_account.json`. Do not share that file. The pid and the baasUserID are never shown in the UI or logs.

## Environment overrides

| Variable | Purpose | Default |
|---|---|---|
| `NEXTENDO_CLIENT_ID` | OAuth client id | `nxc_t63scKVq2hF1` |
| `NEXTENDO_API` | Account API base. Only `https://*.nextendo.network` or a loopback address is accepted: every request carries the account token. | `https://nextendo.network` |
| `NEXTENDO_AUTH` | Browser authorize host (same rule) | same as `NEXTENDO_API` |
| `NEXTENDO_SERVER_IP` | Address the Nintendo names resolve to | `51.178.29.194` |
| `NEXTENDO_NAT_IP` | Second NAT-check address (must differ from the server’s) | `164.132.111.120` |
| `NEXTENDO_TLS_PINS` (or `online.nextendo.tls_pins`) | More server key pins, comma-separated base64 SHA-256 of the SubjectPublicKeyInfo, for when Nextendo changes a key before a new release. `NEXTENDO_TLS_PINS=any` (the whole value, environment only) turns the check off, for a developer's own test server. | the pins in `hle_ssl.cpp` |
| `online.nextendo.server` / `online.nextendo.nat` (`config.ini`) | Same as `NEXTENDO_SERVER_IP` / `NEXTENDO_NAT_IP`; an address or a host name | as below |
| `NEXTENDO_BAAS_SIGNING_KEY` | RSA key (PEM text or a path) to sign the `id_token`; also read from `<data root>/nextendo_baas.pem` | a key generated for each run |

ACNH only forwards the `id_token`; the server trusts the `nnex` game token inside it, so the generated key is enough.

## Clean-room policy

Client code in this tree is original (MIT). It was written against the public Nextendo documentation ([wiki.nextendo.network](https://wiki.nextendo.network/developers)) and published server behaviour. Do not copy Ryujinx-Nextendo (PolyForm Shield), Citron-Nextendo (GPL), Prelude, or other Nextendo client trees into this project.

## Kill switch

Choose **Local play only**, or **Sign out**: nifm reports offline again and hostname redirects stop. Local Play stays available in both modes when `online.enabled=1`.
