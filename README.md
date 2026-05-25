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
- `firmware/xteink`: imported into the project and kept build-isolated; Nomi v1 protocol migration is still a separate step.

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

## Firmware Build

Use a short PlatformIO core directory and UTF-8 Python output on Windows:

```powershell
$env:PLATFORMIO_CORE_DIR=(Join-Path $env:LOCALAPPDATA 'nomi\pio-core')
$env:PYTHONUTF8='1'
$env:PYTHONIOENCODING='utf-8'
chcp 65001
```

The UTF-8 settings are required for `firmware/xteink` on Windows because its i18n generator can print language names that the default GBK console cannot encode.

Build firmware:

```powershell
cd firmware\amoled
python -m platformio run

cd ..\rlcd
python -m platformio run

cd ..\xteink
python -m platformio run
```
