# Agent Notes

This repository is **Nomi Desk**, an ESP32 external display project for local coding agents.

The current product direction is:

- Product name: `Nomi Desk`
- Repository name: `nomi-desk`
- Future user-facing CLI: `nomi`
- Current Rust sender crate/binary: `nomi-send`

Do not rename the project back to `codex_status`, `Clawdmeter`, or other source-specific names. Codex is one integration, not the product boundary.

## Repository Shape

```text
firmware/
  amoled/   Nomi AMOLED firmware
  rlcd/     Nomi RLCD firmware
  xteink/   XTEINK firmware imported from the CrossPoint reference tree

integrations/
  codex/    Supported Codex hook-only integration

tools/
  nomi-send/  Rust BLE sender used by integrations

docs/
  legacy/   Historical notes, plans, and bridge-era references
```

Keep new supported paths out of `docs/legacy/`. Anything under `docs/legacy/` may contain old names, old paths, or bridge-era assumptions.

## Supported Device Status

Current device state:

- `firmware/amoled`
  - BLE local name: `Nomi AMOLED`
  - Status: migrated to Nomi v1 BLE identity and payload.
- `firmware/rlcd`
  - BLE local name: `Nomi RLCD`
  - Status: migrated to Nomi v1 BLE identity and payload.
- `firmware/xteink`
  - Status: imported and build-verified, but **not yet migrated** to Nomi v1 BLE identity.
  - Current old local name in code: `Codex XTEINK`.
  - Treat protocol migration for XTEINK as a separate task.

Strictly speaking, `nomi-send` can directly target AMOLED and RLCD today. XTEINK is present for build integration and future migration.

## Main Host Path

The supported Codex path is hook-only:

```text
Codex hook -> integrations/codex/codex_status_hook.ps1 -> tools/nomi-send -> BLE device
```

Do not reintroduce a long-running Python bridge as the default path. Legacy bridge files are kept only under `integrations/codex/legacy/` for reference and hardware experiments.

The hook must return quickly and must not block Codex. BLE scan/connect/write happens in the spawned `nomi-send enqueue` process.

Hook diagnostics go to:

```text
%TEMP%\nomi\hook.log
```

`codex_status_hook.ps1` resolves the sender in this order:

1. `NOMI_SEND_PATH`
2. This repository's Rust build output
3. `nomi-send` / `nomi-send.exe` on `PATH`

## Build Commands

Rust sender:

```powershell
cd tools\nomi-send
cargo fmt -- --check
cargo test
cargo clippy --all-targets -- -D warnings
cargo build --release
cd ..\..
```

Firmware builds on Windows should use a short PlatformIO core directory and UTF-8 Python output:

```powershell
$env:PLATFORMIO_CORE_DIR=(Join-Path $env:LOCALAPPDATA 'nomi\pio-core')
$env:PYTHONUTF8='1'
$env:PYTHONIOENCODING='utf-8'
chcp 65001
```

Then:

```powershell
cd firmware\amoled
python -m platformio run

cd ..\rlcd
python -m platformio run

cd ..\xteink
python -m platformio run
```

## Known Build Pitfalls

### XTEINK Windows GBK Failure

Observed failure during `firmware/xteink` PlatformIO build:

```text
UnicodeEncodeError: 'gbk' codec can't encode character '\u010c'
```

Root cause: Windows default GBK console encoding cannot print some language names emitted by `firmware/xteink/scripts/gen_i18n.py`.

Fix:

```powershell
$env:PYTHONUTF8='1'
$env:PYTHONIOENCODING='utf-8'
chcp 65001
python -m platformio run
```

With those settings, XTEINK was build-verified successfully. The prior successful XTEINK build produced `firmware.bin`, `bootloader.bin`, and `partitions.bin` under `.pio/build/default/`.

### PlatformIO Build Time

Full firmware builds can be slow after cache cleanup:

- RLCD previously took around 6-7 minutes after cache reset.
- AMOLED previously took around 2-3 minutes after cache reset.
- XTEINK previously took around 2 minutes once the UTF-8 issue was fixed.

Do not assume a build is hung just because PlatformIO is quiet for a while. Check active `python.exe` / `tool-scons` processes and verbose logs before killing it.

## Installation Behavior

`integrations/codex/install_global_hooks.ps1` must preserve existing user configuration. It reads the existing `hooks.json`, keeps unrelated top-level properties and unrelated hook events, and only replaces the Nomi-managed Codex events:

- `SessionStart`
- `UserPromptSubmit`
- `PermissionRequest`
- `PreToolUse`
- `PostToolUse`
- `Stop`

Do not change this back to wholesale overwrite behavior.

## Files To Avoid Committing

The repo should not commit generated or local runtime outputs:

- `firmware/**/.pio/`
- `firmware/**/.cache/`
- `tools/nomi-send/target/`
- `.codex/`
- `*.log`
- `*.out`
- `*.err`
- `__pycache__/`
- `*.pyc`

Temporary XTEINK debug logs such as `build-xteink.log` and `build-xteink-utf8.log` were intentionally removed and should stay out of the repository.

## Review Status

The repository refactor was reviewed with subagents until Critical/Important findings converged.

Fixed during review:

- Hook errors are logged instead of being fully silent.
- Sender lookup supports `NOMI_SEND_PATH`, repo-local builds, and `PATH`.
- Codex hook installation preserves existing user hook configuration.
- Main README and integration README no longer use machine-specific absolute paths.
- Legacy Superpowers plans were moved under `docs/legacy/superpowers`.

Remaining non-blocking follow-up work:

- Migrate `firmware/xteink` to Nomi v1 BLE identity and payload.
- Add future Claude Code integration without making the core protocol Codex-specific.
- Eventually consolidate `nomi-send` into a user-facing `nomi` CLI if packaging through npm.
