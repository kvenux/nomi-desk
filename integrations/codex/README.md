# Codex Integration

This directory contains the supported Codex hook-only integration for Nomi.

## Files

- `codex_status_hook.ps1`: builds a compact `nomi-agent-display` payload from Codex hook input and launches `nomi-send enqueue` asynchronously.
- `install_global_hooks.ps1`: writes the global Codex `hooks.json` entries.
- `legacy/`: old Python bridge scripts kept for reference and hardware experiments.

## Install

Build the Rust sender first:

```powershell
cd tools\nomi-send
cargo build --release
cd ..\..
```

Install hooks:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\integrations\codex\install_global_hooks.ps1 -Force
```

Open Codex and run `/hooks` once to trust the generated commands.

The hook finds `nomi-send` in this order: `NOMI_SEND_PATH`, this repository's Rust build output, then `nomi-send` on `PATH`. Hook diagnostics are written to `%TEMP%\nomi\hook.log`.

## Runtime Behavior

The hook always returns:

```json
{"continue":true}
```

BLE scan/connect/write happens outside the blocking Codex hook path. If no Nomi device is nearby, Codex continues normally and `nomi-send` records the failure in its local state log.

The payload includes model, reasoning effort, and service tier from `config.toml`, plus the latest `token_count` snapshot from Codex session JSONL when available. That snapshot drives context percentage, used tokens, and 5-hour/weekly quota remaining. Set `CODEX_HOME` to override the default `%USERPROFILE%\.codex` lookup.

## Nomi v1 BLE Identity

```text
AMOLED local name: Nomi AMOLED
RLCD local name:   Nomi RLCD
XTEINK local name: Nomi XTEINK

Service UUID:      f4f688c2-613e-56a5-b115-d19a99d1b463
RX/write UUID:     74879a99-7275-5b33-9665-51519f328fa5
TX/notify UUID:    830ac719-8dea-541c-8d18-5e8de4cd83dd
Info/read UUID:    485d9275-a3ad-516d-a524-e284f0aafdb1
```

`firmware/xteink` accepts the shared display payload and refreshes the e-ink screen on status updates. XTEINK host button mapping is intentionally not enabled yet.
