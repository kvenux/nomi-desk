# Nomi Agent Display

Nomi is an ESP32 external display family for local coding agents. It shows agent state from host integrations over a shared BLE protocol.

## Repository Layout

```text
firmware/
  amoled/   Nomi AMOLED firmware
  rlcd/     Nomi RLCD firmware
  xteink/   XTEINK firmware imported from the CrossPoint-based reference tree

integrations/
  codex/    Codex hook installer and hook-only payload builder

tools/
  nomi-send/  Rust BLE sender used by integrations

docs/
  legacy/   historical notes and mockups
```

## Current Device Status

- `firmware/amoled`: migrated to Nomi v1 BLE names and UUIDs.
- `firmware/rlcd`: migrated to Nomi v1 BLE names and UUIDs.
- `firmware/xteink`: migrated to Nomi v1 BLE names and UUIDs, with e-ink fast-refresh updates after the initial full refresh. Host button mapping is not enabled yet.

## Hook-Only Codex Path

Build the sender:

```powershell
cd tools\nomi-send
cargo build --release
cd ..\..
```

Install Codex hooks:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\integrations\codex\install_global_hooks.ps1 -Force
```

Then open Codex and run `/hooks` once to trust the installed hook commands.

The hook finds `nomi-send` in this order: `NOMI_SEND_PATH`, this repository's Rust build output, then `nomi-send` on `PATH`. Hook diagnostics are written to `%TEMP%\nomi\hook.log`.
It also reads `%USERPROFILE%\.codex\config.toml` and recent session JSONL files so displays can show model effort, context usage, token count, and quota remaining; set `CODEX_HOME` to override that lookup.

## Firmware Build

Use a short PlatformIO core directory and UTF-8 Python output on Windows:

```powershell
$env:PLATFORMIO_CORE_DIR='C:\pio'
$env:PYTHONUTF8='1'
$env:PYTHONIOENCODING='utf-8'
chcp 65001
```

The UTF-8 settings are required for `firmware/xteink` on Windows because its i18n generator can print language names that the default GBK console cannot encode.
The short `C:\pio` core directory also avoids Windows MAX_PATH failures while PlatformIO unpacks the ESP32 Arduino packages when long path support is disabled.

Build firmware:

```powershell
cd firmware\amoled
python -m platformio run

cd ..\rlcd
python -m platformio run

cd ..\xteink
python -m platformio run
```
