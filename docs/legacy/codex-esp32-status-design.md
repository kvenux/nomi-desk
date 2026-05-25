# Codex ESP32 Status Display Design

## Goal

Use an ESP32 board as a small external status display for Codex. The ESP32 should show whether Codex is idle, active, waiting for approval, waiting for user input, completed, failed, or offline.

The preferred approach is to subscribe to Codex app-server events on the PC, normalize those events in a small bridge process, then sync the compact status to ESP32 over Bluetooth.

## High-Level Architecture

```text
Codex CLI / app-server
        |
        | WebSocket JSON-RPC
        v
PC bridge process
        |
        | BLE GATT notify/write
        v
ESP32 display firmware
```

## Why Use A Bridge

The ESP32 should not talk directly to Codex app-server in the first version.

Reasons:

- Codex app-server uses a JSON-RPC WebSocket protocol.
- The protocol is currently experimental and may change across Codex CLI releases.
- Status events can be detailed, while the display only needs a small state model.
- BLE pairing, reconnection, and display rendering are simpler if ESP32 only receives compact status values.

The bridge keeps Codex-specific logic on the PC and keeps ESP32 firmware small.

## Codex CLI Entry Points

The local CLI version checked during design:

```text
codex-cli 0.130.0
```

Relevant commands:

```powershell
codex app-server --listen ws://127.0.0.1:49300
```

If a Codex TUI should connect to that app-server:

```powershell
codex --remote ws://127.0.0.1:49300
```

The protocol schema can be generated with:

```powershell
codex app-server generate-json-schema --out $env:TEMP\codex-app-schema
```

## App-Server Protocol

The app-server speaks WebSocket JSON-RPC.

Initial request:

```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "initialize",
  "params": {
    "clientInfo": {
      "name": "esp32-status-bridge",
      "version": "0.1.0"
    },
    "capabilities": {
      "experimentalApi": true
    }
  }
}
```

Then send the initialized notification:

```json
{
  "jsonrpc": "2.0",
  "method": "initialized"
}
```

Useful server notifications:

- `thread/status/changed`
- `turn/started`
- `turn/completed`
- `agent/message/delta`
- `item/commandExecution/outputDelta`
- approval-related notifications
- user-input-related notifications
- plan and diff update notifications

## Codex Status Model

From `thread/status/changed`, Codex exposes these thread status types:

- `notLoaded`
- `idle`
- `active`
- `systemError`

When status is `active`, it may include active flags:

- `waitingOnApproval`
- `waitingOnUserInput`

Example notification:

```json
{
  "jsonrpc": "2.0",
  "method": "thread/status/changed",
  "params": {
    "threadId": "example-thread-id",
    "status": {
      "type": "active",
      "activeFlags": []
    }
  }
}
```

## Compact ESP32 Status Protocol

The bridge should reduce Codex events into a compact state that ESP32 can render.

Recommended first version:

| Code | State | Meaning |
| --- | --- | --- |
| 0 | `offline` | Bridge cannot connect to Codex app-server |
| 1 | `idle` | Codex is ready |
| 2 | `active` | Codex is working |
| 3 | `waiting_approval` | Codex needs approval |
| 4 | `waiting_input` | Codex needs user input |
| 5 | `completed` | Last turn completed |
| 6 | `failed` | Last turn failed or system error |

Possible BLE payload:

```json
{
  "s": 2,
  "label": "active",
  "thread": "short-id",
  "ts": 1778918400
}
```

For an even smaller protocol:

```text
S:2
```

## Bridge Responsibilities

The PC bridge should:

- Start or connect to `codex app-server`.
- Open the WebSocket JSON-RPC connection.
- Send `initialize` and `initialized`.
- Listen for Codex notifications.
- Maintain the current display state.
- Expose a BLE GATT service.
- Notify ESP32 when the status changes.
- Send `offline` if the app-server connection drops.
- Reconnect with backoff.

Recommended implementation choices:

- Node.js bridge: good WebSocket and BLE library ecosystem.
- Python bridge: simple for prototyping, but BLE support on Windows can be more uneven.
- Rust bridge: best long-term robustness, more setup cost.

## ESP32 Responsibilities

The ESP32 firmware should:

- Connect to the bridge over BLE.
- Subscribe to a status characteristic.
- Parse compact status updates.
- Render the status on the display.
- Show `offline` when BLE disconnects or no heartbeat is received.

ESP32 should avoid Codex-specific protocol details in the first version.

## Display Suggestions

Useful first screen:

- Large status text.
- Small colored indicator.
- Optional current thread short ID.
- Optional elapsed time since last status change.

Suggested colors:

- Offline: gray
- Idle: green
- Active: blue
- Waiting approval: amber
- Waiting input: purple
- Failed: red

## Risks And Notes

- `app-server` and `remote-control` are experimental in the current CLI.
- Notification names or payloads may change after Codex CLI updates.
- Keep the bridge tolerant of unknown events.
- Generate the schema after CLI upgrades and compare relevant status payloads.
- Prefer event subscription over parsing terminal UI output.

## First Milestone

Build the smallest working loop:

1. Start `codex app-server --listen ws://127.0.0.1:49300`.
2. Run a bridge that connects to the WebSocket.
3. Print normalized statuses in the bridge console.
4. Add BLE notify once the normalization works.
5. Make ESP32 render `offline`, `idle`, `active`, `waiting_approval`, and `failed`.

