# Nomi Agent Display Protocol Design

Date: 2026-05-24

## Goal

Refactor the current Codex-specific status display protocol into a neutral Nomi device protocol that can support Codex now and Claude Code later.

The new design should:

- Treat Nomi as the device family name, not as a Codex-specific product.
- Use device-specific BLE names, such as `Nomi AMOLED`, instead of source-specific names like `Codex Status`.
- Use one shared BLE GATT protocol across the maintained Nomi firmware targets first, with room for future controller devices.
- Make hook-only sending the default user path so users do not need to run a long-lived bridge process.
- Preserve the existing bridge concept only as an optional debug or advanced mode.
- Use Rust for the production host sender so distribution does not depend on Python, pip, or long-lived services.

## Current Problems

The current implementation has three protocol shapes:

- AMOLED and RLCD use device names `Codex Status` and `Codex RLCD`, with service UUID `4c41555a-4465-7669-6365-000000000001`.
- XTEINK uses device name `Codex XTEINK`, with service UUID `6f30d210-2f6d-4a8c-9f78-42d8d2f04201`.
- Clawdmeter uses `Claude Controller`, sharing the older `4c41555a...` service UUID but with a Claude quota-oriented payload.

This creates three issues:

- The device name is tied to the current source application, Codex.
- XTEINK cannot be driven by the main `codex_status/bridge.py` protocol without special handling.
- Users must currently understand whether they need the bridge, a session-start hook, or a device-specific sender.

## Naming

Use `Nomi` as the device family name.

BLE local names:

```text
Nomi AMOLED
Nomi RLCD
Nomi XTEINK
Nomi Controller
```

Rules:

- Device names describe the physical device or role.
- Source names such as `codex` or `claude-code` appear only in the status payload.
- Host-side scanning should match either the service UUID or BLE names beginning with `Nomi `.

## BLE GATT Protocol

Use one shared Nomi Agent Display v1 BLE service.

```text
Service:  f4f688c2-613e-56a5-b115-d19a99d1b463
RX/write: 74879a99-7275-5b33-9665-51519f328fa5
TX/notify: 830ac719-8dea-541c-8d18-5e8de4cd83dd
Info/read: 485d9275-a3ad-516d-a524-e284f0aafdb1
```

Characteristics:

- `RX/write`: host writes compact JSON status payloads to the device.
- `TX/notify`: device emits acknowledgements, parse errors, and button/control events.
- `Info/read`: host reads static device capability metadata.

Expected `Info/read` value:

```json
{
  "protocol": "nomi-agent-display",
  "version": 1,
  "device": "rlcd",
  "width": 400,
  "height": 300
}
```

## Status Payload

Use a source-neutral JSON payload. The device should not need to know whether the payload came from Codex or Claude Code beyond displaying the optional `source` label.

```json
{
  "protocol": "nomi-agent-display",
  "version": 1,
  "source": "codex",
  "state": "active",
  "model": "gpt-5.5",
  "session": "current session",
  "prompt": "latest user prompt",
  "context_pct": 84,
  "used_tokens_k": 218,
  "quota": {
    "five_hour_left": 95,
    "weekly_left": 98
  },
  "time": "14:25",
  "event": "UserPromptSubmit"
}
```

Required fields:

- `protocol`
- `version`
- `source`
- `state`
- `time`
- `event`

Optional display fields:

- `model`
- `session`
- `prompt`
- `context_pct`
- `used_tokens_k`
- `quota.five_hour_left`
- `quota.weekly_left`

State values:

```text
offline
idle
active
waiting_approval
waiting_input
failed
```

Future Claude Code support uses the same payload shape with:

```json
{
  "source": "claude-code"
}
```

## Sending Model

The default path is hook-only.

```text
Codex or Claude Code hook
  -> source-specific hook script
  -> short-lived Nomi CLI enqueue command
  -> latest payload file + single worker lock
  -> hook returns immediately

background Nomi worker
  -> cached-address connect, or BLE scan/connect/write
  -> worker exits
```

The sender should:

- Keep the hook path asynchronous. BLE scanning and connection must never run in the blocking hook process.
- Write the latest payload to a local state directory.
- Start at most one background worker.
- Use latest-wins semantics when multiple hook events arrive quickly.
- Try cached BLE addresses first.
- Scan for devices advertising the Nomi service UUID if cached addresses fail.
- Accept explicit BLE addresses for debugging.
- Connect to each discovered Nomi device.
- Write the same payload to each device's `RX/write` characteristic.
- Subscribe briefly to `TX/notify` only long enough to capture ack/error responses.
- Exit after a bounded timeout.

This keeps the user-facing setup to hook installation and trust. No long-lived bridge process is required for normal use.

## Optional Bridge Mode

Keep bridge mode as an advanced tool, not the main path.

Bridge mode remains useful for:

- Debugging BLE connection stability.
- Long-lived button/control callbacks.
- Frequent updates that should avoid repeated BLE scanning.
- Comparing hook-only behavior with app-server enriched behavior.

The bridge should use the same Nomi BLE protocol and payload as hook-only sending.

## Host Components

Introduce a production Rust CLI:

```text
tools/nomi-send/
```

Responsibilities:

- Provide `nomi-send enqueue`, `nomi-send worker`, `nomi-send send-file`, and `nomi-send doctor`.
- Build or accept a complete Nomi payload.
- Store latest payload, lock state, BLE address cache, and logs in a local Nomi state directory.
- Discover Nomi devices.
- Send payloads over the unified BLE protocol.
- Return a clear success/failure exit code.

Python BLE scripts remain reference or development tools only. They are not the production user path.

Codex-specific hook logic remains separate:

```text
codex_status/codex_status_hook.ps1
```

The Codex payload builder can read:

- Hook event name and stdin.
- Current working directory.
- `~/.codex/config.toml` for model, reasoning effort, and service tier when available.
- Current session jsonl token counts when a session path can be found.

Claude Code support should later add a separate source-specific payload builder rather than changing the BLE protocol.

## Distribution

Primary developer build:

```text
cargo build --release -p nomi-send
```

Primary user distribution can be added as an npm package that ships prebuilt Rust binaries, but npm is a packaging channel, not the BLE runtime.

```text
nomi npm package
  -> platform-specific nomi-send binary
  -> install-hooks command/wrapper
```

Users should not need Rust, Python, or PlatformIO for host-side status sending.

## Device Firmware Changes

AMOLED:

- Rename BLE device to `Nomi AMOLED`.
- Replace old UUIDs with Nomi v1 UUIDs.
- Accept the new source-neutral payload.
- Preserve button notifications on `TX/notify`.

RLCD:

- Rename BLE device to `Nomi RLCD`.
- Replace old UUIDs with Nomi v1 UUIDs.
- Accept the new source-neutral payload.

XTEINK:

- XTEINK is excluded from the first implementation pass because the current directory is a large reference GitHub project.
- Rename BLE device to `Nomi XTEINK`.
- Replace the separate `6f30d210...` protocol with Nomi v1 UUIDs.
- Map existing field names to the new payload fields.
- Keep button events on `TX/notify`.

Controller:

- Controller migration is not part of the first implementation phase.
- If Clawdmeter is migrated later, rename it to `Nomi Controller`.
- Keep controller-specific HID behavior separate from display status fields.
- Use `TX/notify` for control events and `RX/write` for status/config payloads.

## Compatibility

During migration, sender code can add old device names and UUIDs behind a compatibility flag if needed, but the default targets only Nomi v1.

Recommended default behavior:

- New firmware advertises only Nomi names and Nomi v1 UUIDs.
- New hook-only sender scans only for Nomi v1.
- Old bridge and old device firmware remain available in the repo until the migration is verified.
- Large reference projects, including the current XTEINK reference tree, are not modified or deleted in the first pass.

## Error Handling

Hook scripts must never block normal Codex or Claude Code work.

Rules:

- Hook script exits successfully even if no Nomi device is available.
- Sender logs BLE failures to a local log file.
- Sender uses bounded scan/connect/write timeouts.
- Hook enqueue path should complete in tens of milliseconds under normal conditions and must not wait for BLE.
- Device returns `{"ack":true}` after accepting a payload.
- Device returns `{"err":true,"message":"..."}` after rejecting a payload.

## Testing

Verification should cover:

- Payload builder creates valid Nomi JSON for Codex hook events.
- Hook enqueue returns quickly while a background worker handles BLE.
- Sender can connect by cached address, scan by Nomi service UUID, and use explicit addresses.
- Sender writes the same payload to multiple connected devices.
- AMOLED parses the new payload and updates UI.
- RLCD parses the new payload and updates UI.
- Hook-only path returns quickly when no BLE device is present.
- Legacy bridge remains optional and uses the new protocol when enabled.

## Out of Scope

- Full Claude Code payload extraction.
- Cloud sync or remote status forwarding.
- A persistent GUI setup app.
- Redesigning the visual UI of each device beyond field mapping required by the protocol.
- Migrating or deleting the XTEINK reference GitHub project in the first implementation pass.
