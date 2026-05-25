# Codex Status Display Design

## Goal

Show the active Codex session status on an ESP32 AMOLED board over BLE.

The display payload includes:

- model, reasoning effort, and service tier, for example `gpt-5.5 medium fast`
- run state: ready/active/waiting/failed/offline
- context usage percentage and used token count
- 5-hour and weekly quota remaining percentage
- latest lifecycle event

## Data Sources

The generic path is:

1. Codex hooks publish lifecycle events to a local HTTP endpoint.
2. A local bridge process connects to `codex app-server` and reads richer state:
   - `config/read` for model, reasoning effort, service tier
   - `account/rateLimits/read` for 5-hour and weekly usage
   - JSON-RPC notifications such as `thread/status/changed`, `turn/started`, `turn/completed`, `thread/tokenUsage/updated`, `account/rateLimits/updated`
3. The bridge writes compact JSON to the ESP32 over BLE GATT.

psmux can read the TUI status line, but it is not portable because users may not run Codex inside psmux.

## Hook Feasibility

Hooks are feasible for lifecycle events, but Codex requires hook trust. After installing `~/.codex/hooks.json`, open Codex and run `/hooks`, then trust the installed hooks.

The hook script should not block Codex. The implementation posts to the local bridge and always returns:

```json
{"continue":true}
```

## Implemented Files

- `hello_amoled_216/src/main.cpp`: ESP32 AMOLED + NimBLE firmware
- `hello_amoled_216/platformio.ini`: firmware dependencies and board config
- `codex_status/bridge.py`: Codex app-server + hook HTTP + BLE bridge
- `codex_status/codex_status_hook.ps1`: global hook command
- `codex_status/install_global_hooks.ps1`: installs `~/.codex/hooks.json`
- `codex_status/README.md`: runbook

## Payload

The bridge sends this JSON shape:

```json
{
  "state": "active",
  "model": "gpt-5.5",
  "effort": "medium",
  "tier": "fast",
  "event": "UserPromptSubmit",
  "context_pct": 0,
  "used_tokens_k": 0,
  "five_left": 94,
  "weekly_left": 12,
  "ts": 1778893480
}
```

## BLE Protocol

- Device name: `Codex Status`
- Service UUID: `4c41555a-4465-7669-6365-000000000001`
- RX/write UUID: `4c41555a-4465-7669-6365-000000000002`
- TX/notify UUID: `4c41555a-4465-7669-6365-000000000003`

Windows may discover the service UUID without a local name, so the bridge scans by both name and service UUID.

## Verified

- Firmware builds with PlatformIO.
- Firmware flashed to ESP32-S3 on `COM5` using direct esptool.
- Serial boot log shows `Codex Status Display boot` and `BLE advertising`.
- Bridge connects to BLE address `A4:CB:8F:D7:6A:3D`.
- Bridge reads Codex app-server snapshot: `gpt-5.5 medium fast`, 5-hour and weekly remaining percentages.
- Hook script posts `UserPromptSubmit` to the bridge.
- Bridge writes the final compact JSON payload to the ESP32.

## Important Windows Note

PlatformIO upload appeared to hang because esptool 5.2 hit a GBK stdout encoding error while printing its progress bar. Use:

```powershell
$env:PYTHONIOENCODING='utf-8'
```

before upload/flash commands on this machine.
