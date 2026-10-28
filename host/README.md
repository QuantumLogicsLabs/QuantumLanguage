# Quantum compiler host

The small server behind `https://quantum-compiler.quantumlogicslimited.com`. It is written in Quantum
(`server.sa`) and stores and serves the compiler releases:

- `quantum version.sa` (on the dev PC) builds a release and **uploads** it here.
- `quantum update` (on any user's PC) **downloads** the newest compiler from here.
- The website reads `latest.json` and links the installer from here.

```
dev PC:  quantum version.sa ──PUT──▶  Caddy (HTTPS :443) ──▶ quantum-host.exe (HTTP :8080, local only) ──▶ data\
users:   quantum update / website ──GET──▶  the same
```

## Endpoints

| Request | Does |
| --- | --- |
| `GET /health` | `ok` |
| `GET /latest.json` | the current version and every file's path and SHA-256 |
| `GET /releases/<version>/<file>` | one stored file: `QuantumSetup.exe`, `quantum.exe`, `qrun.exe`, `quantum_stub.exe`, `qpm.exe` |
| `GET /download/QuantumSetup.exe` | the newest installer (a stable link for the website) |
| `PUT /api/publish/<version>/<file>` | store a file. Needs `Authorization: Bearer <publish password>` |
| `PUT /api/publish/latest.json` | make a release current. Refused unless all five files of that version are stored |

Only those file names and `MAJOR.MINOR.PATCH` versions are accepted, so a path cannot escape `data\`.
One connection is served at a time, which is plenty for updates.

## Build and run locally

From `compiler\`:

```bat
seed\qrun.exe --native host\server.sa host\quantum-host.exe

set QUANTUM_HOST_PORT=8080
set QUANTUM_PUBLISH_TOKEN=some-test-password
host\quantum-host.exe
```

Settings (environment variables):

| Variable | Default | Meaning |
| --- | --- | --- |
| `QUANTUM_HOST_PORT` | `8080` | port to listen on |
| `QUANTUM_HOST_BIND` | `127.0.0.1` | address to listen on (keep it local; Caddy is the public face) |
| `QUANTUM_HOST_DATA` | `data` | where releases and `latest.json` are stored |
| `QUANTUM_PUBLISH_TOKEN` | *(unset)* | the publish password. Unset = publishing is disabled |

Try a release against it without the internet:

```bat
set QUANTUM_HOST_URL=http://localhost:8080
set QUANTUM_PUBLISH_TOKEN=some-test-password
quantum version.sa
quantum update --check
```

## Put it on the 24/7 Windows machine

1. **Publish password.** Make a long random string once and keep it safe:
   `powershell -Command "-join ((1..32) | ForEach-Object { '{0:x2}' -f (Get-Random -Maximum 256) })"`
   Put the same string on the dev PC as the environment variable `QUANTUM_PUBLISH_TOKEN`
   (or in the file `%USERPROFILE%\.quantum\publish_token`). Never commit it.
2. **Files.** Copy this `host\` folder to the machine and put `quantum-host.exe` (built as above) and
   [`caddy.exe`](https://caddyserver.com/download) (Windows amd64) in it.
3. **DNS (Vercel).** Vercel → Domains → `quantumlogicslimited.com` → DNS Records → add an **A** record:
   name `quantum-compiler`, value = the machine's **public IP address**.
4. **Network.** On the router forward TCP **80** and **443** to the machine (give it a fixed local IP). Allow
   `caddy.exe` through Windows Firewall. Do **not** open 8080. If the public IP changes, update the A record
   (or use a dynamic-DNS updater). If the machine is behind carrier-grade NAT, the same folder works on a cheap VPS instead.
5. **pm2.** In this folder, in a shell where the password is set:
   ```bat
   npm install -g pm2
   set QUANTUM_PUBLISH_TOKEN=<the password>
   pm2 start ecosystem.config.js
   pm2 save
   ```
   pm2 has no boot support on Windows by itself; for start-at-boot install `pm2-windows-startup`
   (`npm i -g pm2-windows-startup`, then `pm2-startup install`) after `pm2 save`.
6. **Check.** `https://quantum-compiler.quantumlogicslimited.com/health` answers `ok`.

## Releasing

On the dev PC, change the number in `compiler\version.sa` and run `quantum version.sa`. It builds the compiler and
installer (into `build-release\` and `release\<version>\`), uploads them, and switches `latest.json` last.
Run it again with the same number and it says the version is already published. Options:
`--no-publish` (build only), `--skip-build` (upload an already built `release\<version>\`), `--force`.

The host's `data\` folder is the only state; back it up if you care about old releases.
