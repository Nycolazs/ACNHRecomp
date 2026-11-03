# Nextendo Online

ACNHRecomp can use [Nextendo Network](https://nextendo.network) for Animal Crossing’s **Online** features (Dodo Codes and Search for a friend). Nintendo’s servers are never contacted. The recompiled game still speaks NEX/Pia itself; the runtime supplies sockets, DNS redirects, account tokens, and friends like a Switch on Nextendo.

## Modes (`online.mode` in `config.ini`)

| Value | Default | Behaviour |
|---|---|---|
| `nextendo` | yes | Sign in with a Nextendo account. Airport Online uses Nextendo. |
| `local` | | Peer visits only ([LOCAL_PLAY.md](LOCAL_PLAY.md)): no Nextendo account, no Nintendo Online path. |

## Requirements

- Game version **3.0.3** (Nextendo’s ACNH gate). Other versions can still use Local mode.
- A Nextendo account and a registered **public OAuth (PKCE)** application for ACNHRecomp.
- `curl` on `PATH` (same as the launcher’s update check) for HTTPS to the account API.

## Setup

1. ACNHRecomp ships with a registered public PKCE app (`nxc_t63scKVq2hF1`). To use your own app instead, create one at [nextendo.network/developers](https://nextendo.network/developers) (public / PKCE, loopback `http://127.0.0.1:*/callback`) and set:

```ini
[online]
mode = nextendo
nextendo.client_id = nxc_your_client_id
```

   Or set `NEXTENDO_CLIENT_ID` in the environment. Leave `nextendo.client_id` unset to use the built-in id.

3. In the launcher or in-game Online settings, choose **Nextendo Online** and **Sign in**. A browser opens; the password never reaches ACNHRecomp.
4. Launch **3.0.3** and use Dodo Codes / Search for a friend as on a Switch linked to Nextendo.

Session tokens are stored in `<data root>/nextendo_account.json`. Do not share that file (it holds bearer tokens). The Network ID (pid) is never shown in the UI or logs.

## Environment overrides

| Variable | Purpose | Default |
|---|---|---|
| `NEXTENDO_CLIENT_ID` | OAuth client id | `nxc_t63scKVq2hF1` |
| `NEXTENDO_API` | Account + token API base | `https://nextendo.network` |
| `NEXTENDO_AUTH` | Browser authorize host | same as `NEXTENDO_API` |
| `NEXTENDO_SERVER_IP` | Main hostname redirect target | `51.178.29.194` |
| `NEXTENDO_NAT_IP` | Second NAT-check vantage | `164.132.111.120` |
| `NEXTENDO_BAAS_SIGNING_KEY` | Optional PEM path/contents for local BAAS `id_token` signing | (use `/api/nex-token` instead) |

## Clean-room policy

Client code in this tree is original (MIT). It was written against public Nextendo documentation and published **server** behaviour. Do not copy Ryujinx-Nextendo (PolyForm Shield), Citron-Nextendo (GPL), Prelude, or other Nextendo client trees into this project.

## Kill switch

Choose **Local play only**, or **Sign out**: nifm reports offline again and hostname redirects stop. Local LDN peer play remains available under Local mode when `online.enabled=1`.
