# Nomi Rust Sender And Protocol Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build the Nomi v1 protocol, Rust hook-only sender, and first firmware migrations for AMOLED and RLCD without relying on a long-lived bridge or Python distribution.

**Architecture:** Add a new Rust CLI in `tools/nomi-send/` as the canonical host sender. Keep source-specific hook scripts in `codex_status/`, and migrate maintained device firmware in `hello_amoled_216/` and `rlcd_status/` to the shared Nomi BLE UUIDs and payload shape. Leave the large XTEINK reference tree untouched in this pass.

**Tech Stack:** Rust, Cargo, `serde`, `serde_json`, `clap`, `fs2`, `btleplug`, PowerShell hooks, Arduino C++ firmware with NimBLE.

---

## File Structure

- Create `tools/nomi-send/Cargo.toml`: Rust CLI package manifest.
- Create `tools/nomi-send/src/main.rs`: CLI entry point and subcommand routing.
- Create `tools/nomi-send/src/protocol.rs`: Nomi UUIDs, payload structs, validation, JSON encoding.
- Create `tools/nomi-send/src/state.rs`: state directory, latest payload file, lock file, address cache, log paths.
- Create `tools/nomi-send/src/worker.rs`: latest-wins worker loop and bounded send attempts.
- Create `tools/nomi-send/src/ble.rs`: BLE transport using `btleplug`.
- Modify `codex_status/codex_status_hook.ps1`: build a compact Nomi payload and call `nomi-send enqueue` asynchronously.
- Modify `codex_status/install_global_hooks.ps1`: install the hook that uses the Rust sender.
- Modify `hello_amoled_216/src/main.cpp`: rename BLE device, UUIDs, parse nested `quota`.
- Modify `rlcd_status/src/main.cpp`: rename BLE device, UUIDs, parse nested `quota`.
- Modify `codex_status/README.md`: document Rust sender and hook-only setup.
- Do not modify `xteink_crosspoint_base/` in this plan.

## Task 1: Scaffold Rust CLI

**Files:**
- Create: `tools/nomi-send/Cargo.toml`
- Create: `tools/nomi-send/src/main.rs`
- Create: `tools/nomi-send/src/protocol.rs`

- [ ] **Step 1: Create the Cargo package manifest**

Create `tools/nomi-send/Cargo.toml`:

```toml
[package]
name = "nomi-send"
version = "0.1.0"
edition = "2021"

[dependencies]
anyhow = "1"
btleplug = "0.11"
chrono = { version = "0.4", default-features = false, features = ["clock"] }
clap = { version = "4", features = ["derive"] }
directories = "5"
fs2 = "0.4"
serde = { version = "1", features = ["derive"] }
serde_json = "1"
tokio = { version = "1", features = ["macros", "rt-multi-thread", "time", "process"] }
uuid = "1"

[dev-dependencies]
tempfile = "3"
```

- [ ] **Step 2: Add protocol constants and payload struct**

Create `tools/nomi-send/src/protocol.rs`:

```rust
use anyhow::{bail, Result};
use serde::{Deserialize, Serialize};

pub const PROTOCOL: &str = "nomi-agent-display";
pub const VERSION: u8 = 1;

pub const SERVICE_UUID: &str = "f4f688c2-613e-56a5-b115-d19a99d1b463";
pub const RX_UUID: &str = "74879a99-7275-5b33-9665-51519f328fa5";
pub const TX_UUID: &str = "830ac719-8dea-541c-8d18-5e8de4cd83dd";
pub const INFO_UUID: &str = "485d9275-a3ad-516d-a524-e284f0aafdb1";

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct Quota {
    #[serde(skip_serializing_if = "Option::is_none")]
    pub five_hour_left: Option<u8>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub weekly_left: Option<u8>,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct NomiPayload {
    pub protocol: String,
    pub version: u8,
    pub source: String,
    pub state: String,
    pub time: String,
    pub event: String,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub model: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub session: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub prompt: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub context_pct: Option<u8>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub used_tokens_k: Option<u32>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub quota: Option<Quota>,
}

impl NomiPayload {
    pub fn validate(&self) -> Result<()> {
        if self.protocol != PROTOCOL {
            bail!("invalid protocol: {}", self.protocol);
        }
        if self.version != VERSION {
            bail!("invalid version: {}", self.version);
        }
        if self.source.trim().is_empty() {
            bail!("source is required");
        }
        if !matches!(
            self.state.as_str(),
            "offline" | "idle" | "active" | "waiting_approval" | "waiting_input" | "failed"
        ) {
            bail!("invalid state: {}", self.state);
        }
        if self.time.trim().is_empty() {
            bail!("time is required");
        }
        if self.event.trim().is_empty() {
            bail!("event is required");
        }
        Ok(())
    }
}

pub fn encode_payload(payload: &NomiPayload) -> Result<Vec<u8>> {
    payload.validate()?;
    Ok(serde_json::to_vec(payload)?)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn validates_minimal_payload() {
        let payload = NomiPayload {
            protocol: PROTOCOL.to_string(),
            version: VERSION,
            source: "codex".to_string(),
            state: "active".to_string(),
            time: "14:25".to_string(),
            event: "UserPromptSubmit".to_string(),
            model: None,
            session: None,
            prompt: None,
            context_pct: None,
            used_tokens_k: None,
            quota: None,
        };
        assert!(payload.validate().is_ok());
        let json = String::from_utf8(encode_payload(&payload).unwrap()).unwrap();
        assert!(json.contains("\"protocol\":\"nomi-agent-display\""));
    }

    #[test]
    fn rejects_unknown_state() {
        let payload = NomiPayload {
            protocol: PROTOCOL.to_string(),
            version: VERSION,
            source: "codex".to_string(),
            state: "busy".to_string(),
            time: "14:25".to_string(),
            event: "UserPromptSubmit".to_string(),
            model: None,
            session: None,
            prompt: None,
            context_pct: None,
            used_tokens_k: None,
            quota: None,
        };
        assert!(payload.validate().is_err());
    }
}
```

- [ ] **Step 3: Add CLI entry point**

Create `tools/nomi-send/src/main.rs`:

```rust
mod protocol;

use anyhow::Result;
use clap::{Parser, Subcommand};
use protocol::{encode_payload, NomiPayload};
use std::fs;
use std::path::PathBuf;

#[derive(Debug, Parser)]
#[command(name = "nomi-send")]
#[command(about = "Send Nomi agent display status payloads")]
struct Cli {
    #[command(subcommand)]
    command: Command,
}

#[derive(Debug, Subcommand)]
enum Command {
    Validate {
        #[arg(long)]
        json_file: PathBuf,
    },
}

fn main() -> Result<()> {
    let cli = Cli::parse();
    match cli.command {
        Command::Validate { json_file } => {
            let raw = fs::read_to_string(json_file)?;
            let payload: NomiPayload = serde_json::from_str(&raw)?;
            let bytes = encode_payload(&payload)?;
            println!("valid {} bytes", bytes.len());
            Ok(())
        }
    }
}
```

- [ ] **Step 4: Run tests**

Run:

```powershell
cd C:\Users\kvenu\playground\mynomi\tools\nomi-send
cargo test
```

Expected:

```text
test protocol::tests::validates_minimal_payload ... ok
test protocol::tests::rejects_unknown_state ... ok
```

## Task 2: Add State, Enqueue, And Worker Lock

**Files:**
- Modify: `tools/nomi-send/src/main.rs`
- Create: `tools/nomi-send/src/state.rs`
- Create: `tools/nomi-send/src/worker.rs`

- [ ] **Step 1: Add state management**

Create `tools/nomi-send/src/state.rs`:

```rust
use anyhow::{Context, Result};
use directories::ProjectDirs;
use fs2::FileExt;
use serde::{Deserialize, Serialize};
use std::fs::{self, File, OpenOptions};
use std::io::Write;
use std::path::{Path, PathBuf};

#[derive(Debug, Clone)]
pub struct StatePaths {
    pub root: PathBuf,
    pub latest_payload: PathBuf,
    pub worker_lock: PathBuf,
    pub address_cache: PathBuf,
    pub log_file: PathBuf,
}

#[derive(Debug, Clone, Serialize, Deserialize, Default)]
pub struct AddressCache {
    pub addresses: Vec<String>,
}

impl StatePaths {
    pub fn discover() -> Result<Self> {
        let dirs = ProjectDirs::from("dev", "mynomi", "Nomi")
            .context("could not resolve Nomi state directory")?;
        let root = dirs.data_local_dir().to_path_buf();
        fs::create_dir_all(&root)?;
        Ok(Self {
            latest_payload: root.join("latest.json"),
            worker_lock: root.join("worker.lock"),
            address_cache: root.join("addresses.json"),
            log_file: root.join("nomi-send.log"),
            root,
        })
    }

    pub fn write_latest(&self, payload: &[u8]) -> Result<()> {
        let tmp = self.latest_payload.with_extension("json.tmp");
        fs::write(&tmp, payload)?;
        fs::rename(tmp, &self.latest_payload)?;
        Ok(())
    }

    pub fn read_latest(&self) -> Result<Vec<u8>> {
        Ok(fs::read(&self.latest_payload)?)
    }

    pub fn try_lock_worker(&self) -> Result<Option<File>> {
        let file = OpenOptions::new()
            .create(true)
            .read(true)
            .write(true)
            .open(&self.worker_lock)?;
        match file.try_lock_exclusive() {
            Ok(()) => Ok(Some(file)),
            Err(_) => Ok(None),
        }
    }

    pub fn append_log(&self, message: &str) {
        let _ = append_line(&self.log_file, message);
    }
}

fn append_line(path: &Path, message: &str) -> Result<()> {
    let mut file = OpenOptions::new().create(true).append(true).open(path)?;
    writeln!(file, "{}", message)?;
    Ok(())
}
```

- [ ] **Step 2: Add worker command that uses the lock before BLE is wired**

Create `tools/nomi-send/src/worker.rs`:

```rust
use crate::state::StatePaths;
use anyhow::Result;

pub async fn run_once(paths: &StatePaths) -> Result<bool> {
    let Some(_lock) = paths.try_lock_worker()? else {
        paths.append_log("worker already running");
        return Ok(false);
    };
    let payload = paths.read_latest()?;
    paths.append_log(&format!("worker read {} bytes", payload.len()));
    Ok(true)
}
```

- [ ] **Step 3: Add `enqueue` and `worker` CLI commands**

Modify `tools/nomi-send/src/main.rs`:

```rust
mod protocol;
mod state;
mod worker;

use anyhow::Result;
use clap::{Parser, Subcommand};
use protocol::{encode_payload, NomiPayload};
use state::StatePaths;
use std::fs;
use std::path::PathBuf;
use tokio::process::Command as TokioCommand;

#[derive(Debug, Parser)]
#[command(name = "nomi-send")]
#[command(about = "Send Nomi agent display status payloads")]
struct Cli {
    #[command(subcommand)]
    command: Command,
}

#[derive(Debug, Subcommand)]
enum Command {
    Validate {
        #[arg(long)]
        json_file: PathBuf,
    },
    Enqueue {
        #[arg(long)]
        json_file: PathBuf,
        #[arg(long)]
        no_spawn: bool,
    },
    Worker,
}

#[tokio::main]
async fn main() -> Result<()> {
    let cli = Cli::parse();
    match cli.command {
        Command::Validate { json_file } => {
            let raw = fs::read_to_string(json_file)?;
            let payload: NomiPayload = serde_json::from_str(&raw)?;
            let bytes = encode_payload(&payload)?;
            println!("valid {} bytes", bytes.len());
            Ok(())
        }
        Command::Enqueue { json_file, no_spawn } => {
            let raw = fs::read_to_string(json_file)?;
            let payload: NomiPayload = serde_json::from_str(&raw)?;
            let bytes = encode_payload(&payload)?;
            let paths = StatePaths::discover()?;
            paths.write_latest(&bytes)?;
            paths.append_log(&format!("enqueued {} bytes", bytes.len()));
            if !no_spawn {
                let exe = std::env::current_exe()?;
                TokioCommand::new(exe).arg("worker").spawn()?;
            }
            Ok(())
        }
        Command::Worker => {
            let paths = StatePaths::discover()?;
            worker::run_once(&paths).await?;
            Ok(())
        }
    }
}
```

- [ ] **Step 4: Test enqueue without BLE**

Run:

```powershell
cd C:\Users\kvenu\playground\mynomi\tools\nomi-send
$payload = "$env:TEMP\nomi-payload.json"
'{"protocol":"nomi-agent-display","version":1,"source":"codex","state":"active","time":"14:25","event":"UserPromptSubmit"}' | Set-Content -LiteralPath $payload -Encoding utf8
cargo run -- enqueue --json-file $payload --no-spawn
cargo run -- worker
```

Expected:

```text
Finished dev ...
```

No blocking BLE scan should run in this task.

## Task 3: Implement BLE Transport

**Files:**
- Create: `tools/nomi-send/src/ble.rs`
- Modify: `tools/nomi-send/src/worker.rs`
- Modify: `tools/nomi-send/src/main.rs`

- [ ] **Step 1: Add BLE sender**

Create `tools/nomi-send/src/ble.rs`:

```rust
use anyhow::{anyhow, Context, Result};
use btleplug::api::{
    Central, CharPropFlags, Manager as _, Peripheral as _, ScanFilter, WriteType,
};
use btleplug::platform::{Adapter, Manager, Peripheral};
use std::time::Duration;
use tokio::time::timeout;
use uuid::Uuid;

use crate::protocol::{RX_UUID, SERVICE_UUID};

#[derive(Debug, Clone)]
pub struct SendOptions {
    pub explicit_addresses: Vec<String>,
    pub scan_timeout: Duration,
    pub connect_timeout: Duration,
}

impl Default for SendOptions {
    fn default() -> Self {
        Self {
            explicit_addresses: Vec::new(),
            scan_timeout: Duration::from_secs(4),
            connect_timeout: Duration::from_secs(5),
        }
    }
}

pub async fn send_payload(payload: &[u8], options: &SendOptions) -> Result<usize> {
    let adapter = first_adapter().await?;
    let targets = if options.explicit_addresses.is_empty() {
        scan_nomi_devices(&adapter, options.scan_timeout).await?
    } else {
        peripherals_by_address(&adapter, &options.explicit_addresses).await?
    };

    let mut sent = 0usize;
    for peripheral in targets {
        if send_to_peripheral(&peripheral, payload, options.connect_timeout).await.is_ok() {
            sent += 1;
        }
    }
    if sent == 0 {
        return Err(anyhow!("no Nomi devices accepted payload"));
    }
    Ok(sent)
}

async fn first_adapter() -> Result<Adapter> {
    let manager = Manager::new().await?;
    let adapters = manager.adapters().await?;
    adapters.into_iter().next().context("no Bluetooth adapter found")
}

async fn scan_nomi_devices(adapter: &Adapter, scan_timeout: Duration) -> Result<Vec<Peripheral>> {
    let service_uuid = Uuid::parse_str(SERVICE_UUID)?;
    adapter.start_scan(ScanFilter { services: vec![service_uuid] }).await?;
    tokio::time::sleep(scan_timeout).await;
    let peripherals = adapter.peripherals().await?;
    let mut matches = Vec::new();
    for peripheral in peripherals {
        if let Ok(Some(props)) = peripheral.properties().await {
            let name_match = props
                .local_name
                .as_deref()
                .map(|name| name.starts_with("Nomi "))
                .unwrap_or(false);
            let service_match = props.services.iter().any(|uuid| *uuid == service_uuid);
            if name_match || service_match {
                matches.push(peripheral);
            }
        }
    }
    Ok(matches)
}

async fn peripherals_by_address(adapter: &Adapter, addresses: &[String]) -> Result<Vec<Peripheral>> {
    let peripherals = adapter.peripherals().await?;
    let wanted: Vec<String> = addresses.iter().map(|addr| addr.to_uppercase()).collect();
    Ok(peripherals
        .into_iter()
        .filter(|p| wanted.iter().any(|addr| p.address().to_string().to_uppercase() == *addr))
        .collect())
}

async fn send_to_peripheral(peripheral: &Peripheral, payload: &[u8], connect_timeout: Duration) -> Result<()> {
    timeout(connect_timeout, peripheral.connect()).await??;
    peripheral.discover_services().await?;
    let rx_uuid = Uuid::parse_str(RX_UUID)?;
    let characteristic = peripheral
        .characteristics()
        .into_iter()
        .find(|c| c.uuid == rx_uuid && c.properties.intersects(CharPropFlags::WRITE | CharPropFlags::WRITE_WITHOUT_RESPONSE))
        .context("Nomi RX characteristic not found")?;
    peripheral.write(&characteristic, payload, WriteType::WithResponse).await?;
    let _ = peripheral.disconnect().await;
    Ok(())
}
```

- [ ] **Step 2: Wire BLE into worker**

Modify `tools/nomi-send/src/worker.rs`:

```rust
use crate::ble::{send_payload, SendOptions};
use crate::state::StatePaths;
use anyhow::Result;

pub async fn run_once(paths: &StatePaths, options: &SendOptions) -> Result<bool> {
    let Some(_lock) = paths.try_lock_worker()? else {
        paths.append_log("worker already running");
        return Ok(false);
    };
    let payload = paths.read_latest()?;
    paths.append_log(&format!("worker sending {} bytes", payload.len()));
    match send_payload(&payload, options).await {
        Ok(count) => {
            paths.append_log(&format!("sent payload to {} device(s)", count));
            Ok(true)
        }
        Err(err) => {
            paths.append_log(&format!("send failed: {err:#}"));
            Ok(false)
        }
    }
}
```

- [ ] **Step 3: Add CLI address and timeout options**

Modify `tools/nomi-send/src/main.rs` imports and worker call:

```rust
mod ble;
mod protocol;
mod state;
mod worker;

use anyhow::Result;
use ble::SendOptions;
use clap::{Parser, Subcommand};
use protocol::{encode_payload, NomiPayload};
use state::StatePaths;
use std::fs;
use std::path::PathBuf;
use std::time::Duration;
use tokio::process::Command as TokioCommand;
```

Change the `Worker` variant:

```rust
    Worker {
        #[arg(long = "address")]
        addresses: Vec<String>,
        #[arg(long, default_value_t = 4)]
        scan_timeout_secs: u64,
        #[arg(long, default_value_t = 5)]
        connect_timeout_secs: u64,
    },
```

Change the match arm:

```rust
        Command::Worker { addresses, scan_timeout_secs, connect_timeout_secs } => {
            let paths = StatePaths::discover()?;
            let options = SendOptions {
                explicit_addresses: addresses,
                scan_timeout: Duration::from_secs(scan_timeout_secs),
                connect_timeout: Duration::from_secs(connect_timeout_secs),
            };
            worker::run_once(&paths, &options).await?;
            Ok(())
        }
```

- [ ] **Step 4: Compile BLE sender**

Run:

```powershell
cd C:\Users\kvenu\playground\mynomi\tools\nomi-send
cargo build
```

Expected:

```text
Finished dev ...
```

## Task 4: Update Codex Hook To Enqueue Asynchronously

**Files:**
- Modify: `codex_status/codex_status_hook.ps1`
- Modify: `codex_status/install_global_hooks.ps1`

- [ ] **Step 1: Replace hook body with payload write and non-blocking enqueue**

Modify `codex_status/codex_status_hook.ps1` to:

```powershell
param(
  [string]$EventName = "Unknown"
)

$ErrorActionPreference = "SilentlyContinue"

[Console]::InputEncoding = [System.Text.Encoding]::UTF8
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$sender = Join-Path $repoRoot "tools\nomi-send\target\release\nomi-send.exe"
if (-not (Test-Path $sender)) {
  $sender = Join-Path $repoRoot "tools\nomi-send\target\debug\nomi-send.exe"
}

function Get-State([string]$EventName) {
  switch ($EventName) {
    "SessionStart" { "idle"; break }
    "UserPromptSubmit" { "active"; break }
    "PermissionRequest" { "waiting_approval"; break }
    "PreToolUse" { "active"; break }
    "PostToolUse" { "active"; break }
    "Stop" { "idle"; break }
    default { "active"; break }
  }
}

function Find-PromptValue($Value) {
  if ($null -eq $Value) { return "" }
  if ($Value -is [string]) { return $Value }
  if ($Value -is [System.Collections.IDictionary]) {
    foreach ($key in @("prompt", "user_prompt", "input", "text", "message", "content")) {
      if ($Value.Contains($key)) {
        $found = Find-PromptValue $Value[$key]
        if ($found) { return $found }
      }
    }
    foreach ($child in $Value.Values) {
      $found = Find-PromptValue $child
      if ($found) { return $found }
    }
  }
  if ($Value -is [System.Collections.IEnumerable]) {
    foreach ($child in $Value) {
      $found = Find-PromptValue $child
      if ($found) { return $found }
    }
  }
  return ""
}

$stdinText = [Console]::In.ReadToEnd()
$prompt = ""
if ($EventName -eq "UserPromptSubmit" -and $stdinText) {
  try {
    $parsed = $stdinText | ConvertFrom-Json
    $prompt = Find-PromptValue $parsed
  } catch {
    $prompt = $stdinText
  }
  $prompt = (($prompt -replace "\s+", " ").Trim())
  if ($prompt.Length -gt 96) {
    $prompt = $prompt.Substring(0, 96)
  }
}

$payload = [ordered]@{
  protocol = "nomi-agent-display"
  version = 1
  source = "codex"
  state = Get-State $EventName
  time = (Get-Date).ToString("HH:mm")
  event = $EventName
  session = Split-Path -Leaf (Get-Location).Path
}

if ($prompt) {
  $payload.prompt = $prompt
}

$tempDir = Join-Path ([System.IO.Path]::GetTempPath()) "nomi"
New-Item -ItemType Directory -Force -Path $tempDir | Out-Null
$payloadPath = Join-Path $tempDir ("payload-" + [guid]::NewGuid().ToString("N") + ".json")
$payloadJson = $payload | ConvertTo-Json -Compress -Depth 10
[System.IO.File]::WriteAllText($payloadPath, $payloadJson, [System.Text.UTF8Encoding]::new($false))

if (Test-Path $sender) {
  try {
    Start-Process -FilePath $sender -ArgumentList @("enqueue", "--json-file", $payloadPath) -WindowStyle Hidden | Out-Null
  } catch {
  }
}

Write-Output '{"continue":true}'
```

- [ ] **Step 2: Build Rust sender for hook testing**

Run:

```powershell
cd C:\Users\kvenu\playground\mynomi\tools\nomi-send
cargo build --release
```

Expected:

```text
Finished release ...
```

- [ ] **Step 3: Smoke-test hook**

Run:

```powershell
cd C:\Users\kvenu\playground\mynomi
'{"prompt":"hello from hook"}' | powershell -NoProfile -ExecutionPolicy Bypass -File codex_status\codex_status_hook.ps1 UserPromptSubmit
```

Expected:

```json
{"continue":true}
```

The command should return quickly even if no BLE device is nearby.

## Task 5: Migrate AMOLED Firmware To Nomi v1

**Files:**
- Modify: `hello_amoled_216/src/main.cpp`

- [ ] **Step 1: Change BLE identity and UUIDs**

In `hello_amoled_216/src/main.cpp`, replace:

```cpp
#define DEVICE_NAME "Codex Status"
#define SERVICE_UUID "4c41555a-4465-7669-6365-000000000001"
#define RX_CHAR_UUID "4c41555a-4465-7669-6365-000000000002"
#define TX_CHAR_UUID "4c41555a-4465-7669-6365-000000000003"
```

with:

```cpp
#define DEVICE_NAME "Nomi AMOLED"
#define SERVICE_UUID "f4f688c2-613e-56a5-b115-d19a99d1b463"
#define RX_CHAR_UUID "74879a99-7275-5b33-9665-51519f328fa5"
#define TX_CHAR_UUID "830ac719-8dea-541c-8d18-5e8de4cd83dd"
#define INFO_CHAR_UUID "485d9275-a3ad-516d-a524-e284f0aafdb1"
```

- [ ] **Step 2: Parse nested quota**

In `parse_status_json`, replace:

```cpp
    status.five_left = doc["five_left"] | -1;
    status.weekly_left = doc["weekly_left"] | -1;
```

with:

```cpp
    JsonObject quota = doc["quota"].as<JsonObject>();
    status.five_left = quota["five_hour_left"] | doc["five_left"] | -1;
    status.weekly_left = quota["weekly_left"] | doc["weekly_left"] | -1;
```

- [ ] **Step 3: Add Info characteristic**

After creating `tx_char`, add:

```cpp
    NimBLECharacteristic *info_char = svc->createCharacteristic(
        INFO_CHAR_UUID,
        NIMBLE_PROPERTY::READ
    );
    info_char->setValue("{\"protocol\":\"nomi-agent-display\",\"version\":1,\"device\":\"amoled\",\"width\":480,\"height\":480}");
```

- [ ] **Step 4: Compile AMOLED firmware**

Run:

```powershell
cd C:\Users\kvenu\playground\mynomi\hello_amoled_216
python -m platformio run
```

Expected:

```text
SUCCESS
```

## Task 6: Migrate RLCD Firmware To Nomi v1

**Files:**
- Modify: `rlcd_status/src/main.cpp`

- [ ] **Step 1: Change BLE identity and UUIDs**

In `rlcd_status/src/main.cpp`, replace:

```cpp
#define DEVICE_NAME  "Codex RLCD"
#define SERVICE_UUID "4c41555a-4465-7669-6365-000000000001"
#define RX_CHAR_UUID "4c41555a-4465-7669-6365-000000000002"
#define TX_CHAR_UUID "4c41555a-4465-7669-6365-000000000003"
```

with:

```cpp
#define DEVICE_NAME  "Nomi RLCD"
#define SERVICE_UUID "f4f688c2-613e-56a5-b115-d19a99d1b463"
#define RX_CHAR_UUID "74879a99-7275-5b33-9665-51519f328fa5"
#define TX_CHAR_UUID "830ac719-8dea-541c-8d18-5e8de4cd83dd"
#define INFO_CHAR_UUID "485d9275-a3ad-516d-a524-e284f0aafdb1"
```

- [ ] **Step 2: Parse nested quota**

In `parse_status_json`, replace:

```cpp
    status.five_left = doc["five_left"] | -1;
    status.weekly_left = doc["weekly_left"] | -1;
```

with:

```cpp
    JsonObject quota = doc["quota"].as<JsonObject>();
    status.five_left = quota["five_hour_left"] | doc["five_left"] | -1;
    status.weekly_left = quota["weekly_left"] | doc["weekly_left"] | -1;
```

- [ ] **Step 3: Add Info characteristic**

After creating `tx_char`, add:

```cpp
    NimBLECharacteristic *info_char = svc->createCharacteristic(
        INFO_CHAR_UUID,
        NIMBLE_PROPERTY::READ
    );
    info_char->setValue("{\"protocol\":\"nomi-agent-display\",\"version\":1,\"device\":\"rlcd\",\"width\":400,\"height\":300}");
```

- [ ] **Step 4: Compile RLCD firmware**

Run:

```powershell
cd C:\Users\kvenu\playground\mynomi\rlcd_status
python -m platformio run
```

Expected:

```text
SUCCESS
```

## Task 7: Documentation And Migration Notes

**Files:**
- Modify: `README.md`
- Modify: `codex_status/README.md`

- [ ] **Step 1: Update root README**

Replace the opening description with:

```markdown
# Nomi Agent Display

Nomi is an ESP32 external status display family for local coding agents.

Current maintained firmware targets:

- `hello_amoled_216/`: Nomi AMOLED firmware.
- `rlcd_status/`: Nomi RLCD firmware.
- `codex_status/`: Codex hook installer and host-side sender integration.

The default host path is hook-only: Codex hooks enqueue a compact Nomi status payload, and the Rust `nomi-send` worker sends it to nearby Nomi BLE devices asynchronously.

The large `xteink_crosspoint_base/` directory is retained as a reference tree and is not part of the first Nomi v1 migration.
```

- [ ] **Step 2: Update `codex_status/README.md` run commands**

Add this section near the bridge section:

```markdown
## Nomi hook-only sender

Build the Rust sender:

```powershell
cd C:\Users\kvenu\playground\mynomi\tools\nomi-send
cargo build --release
```

Install global Codex hooks:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File C:\Users\kvenu\playground\mynomi\codex_status\install_global_hooks.ps1 -Force
```

The hook returns immediately. BLE scanning and writes run in a background `nomi-send worker` process. If no Nomi device is nearby, Codex is not blocked.
```

- [ ] **Step 3: Verify docs mention XTEINK reference status**

Run:

```powershell
rg -n "xteink_crosspoint_base|Nomi|nomi-send|hook-only" README.md codex_status\README.md docs\superpowers\specs\2026-05-24-nomi-agent-display-protocol-design.md
```

Expected:

```text
README.md:...
codex_status\README.md:...
docs\superpowers\specs\...
```

## Task 8: End-To-End Verification

**Files:**
- No source changes unless earlier tasks fail.

- [ ] **Step 1: Run Rust tests**

Run:

```powershell
cd C:\Users\kvenu\playground\mynomi\tools\nomi-send
cargo test
```

Expected:

```text
test result: ok
```

- [ ] **Step 2: Run Rust release build**

Run:

```powershell
cd C:\Users\kvenu\playground\mynomi\tools\nomi-send
cargo build --release
```

Expected:

```text
Finished release ...
```

- [ ] **Step 3: Verify hook returns quickly without BLE device**

Run:

```powershell
Measure-Command {
  '{"prompt":"latency test"}' | powershell -NoProfile -ExecutionPolicy Bypass -File C:\Users\kvenu\playground\mynomi\codex_status\codex_status_hook.ps1 UserPromptSubmit | Out-Null
}
```

Expected:

```text
TotalMilliseconds less than 1000
```

- [ ] **Step 4: Compile maintained firmware**

Run:

```powershell
cd C:\Users\kvenu\playground\mynomi\hello_amoled_216
python -m platformio run
cd C:\Users\kvenu\playground\mynomi\rlcd_status
python -m platformio run
```

Expected:

```text
SUCCESS
SUCCESS
```

- [ ] **Step 5: Optional hardware test**

With a flashed Nomi device advertising nearby, run:

```powershell
cd C:\Users\kvenu\playground\mynomi\tools\nomi-send
$payload = "$env:TEMP\nomi-live.json"
'{"protocol":"nomi-agent-display","version":1,"source":"codex","state":"active","time":"14:25","event":"ManualTest","model":"gpt-5.5","prompt":"manual BLE test","quota":{"five_hour_left":95,"weekly_left":98}}' | Set-Content -LiteralPath $payload -Encoding utf8
.\target\release\nomi-send.exe enqueue --json-file $payload
```

Expected:

```text
The command returns quickly, and the device updates within the worker timeout.
```

## Self-Review Notes

- Spec coverage: Rust sender, asynchronous hook, unified BLE UUIDs, Nomi naming, AMOLED/RLCD firmware migration, and XTEINK reference exclusion are covered.
- Placeholder scan: no TODO/TBD placeholders are intentionally left in the plan.
- Type consistency: `NomiPayload`, `Quota`, `SendOptions`, and CLI subcommands are consistently named across tasks.
- Repository note: the workspace root is not a git repository, so commit steps are omitted.
