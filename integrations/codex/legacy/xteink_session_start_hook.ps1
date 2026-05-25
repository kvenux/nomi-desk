param(
  [switch]$RunWorker,
  [string]$PayloadPath
)

$ErrorActionPreference = "SilentlyContinue"

[Console]::InputEncoding = [System.Text.Encoding]::UTF8
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot "..\..\..")
$bridgeScript = Join-Path $repoRoot "firmware\xteink\tools\codex_ble_bridge.py"
$logPath = Join-Path $repoRoot "status_bridge.out"
$errPath = Join-Path $repoRoot "status_bridge.err"

function Get-CodexConfigValue([string]$Name, [string]$Default) {
  $configPath = Join-Path $HOME ".codex\config.toml"
  if (-not (Test-Path $configPath)) {
    return $Default
  }
  $pattern = '^\s*' + [regex]::Escape($Name) + '\s*=\s*"([^"]+)"'
  $match = Select-String -LiteralPath $configPath -Pattern $pattern | Select-Object -First 1
  if ($match) {
    return $match.Matches[0].Groups[1].Value
  }
  return $Default
}

function Get-GitBranch {
  try {
    $branch = (& git rev-parse --abbrev-ref HEAD 2>$null).Trim()
    if ($LASTEXITCODE -eq 0 -and $branch) {
      return $branch
    }
  } catch {
  }
  return "main"
}

function Invoke-Worker([string]$JsonPath) {
  "[$(Get-Date -Format o)] worker start $JsonPath" | Add-Content -LiteralPath $logPath -Encoding utf8
  if (-not (Test-Path $bridgeScript)) {
    "[$(Get-Date -Format o)] bridge script missing: $bridgeScript" | Add-Content -LiteralPath $errPath -Encoding utf8
    return
  }

  try {
    & python $bridgeScript --json-file $JsonPath --scan-timeout 5 --connect-timeout 8 --wait 0.8 `
      1>> $logPath 2>> $errPath
    "[$(Get-Date -Format o)] worker exit $LASTEXITCODE" | Add-Content -LiteralPath $logPath -Encoding utf8
  } catch {
    "[$(Get-Date -Format o)] $_" | Add-Content -LiteralPath $errPath -Encoding utf8
  } finally {
    Remove-Item -LiteralPath $JsonPath -Force -ErrorAction SilentlyContinue
  }
}

if ($RunWorker) {
  Invoke-Worker $PayloadPath
  exit 0
}

$stdinText = [Console]::In.ReadToEnd()
$cwd = (Get-Location).Path
$project = Split-Path -Leaf $cwd
if (-not $project) {
  $project = "Codex"
}

$model = Get-CodexConfigValue "model" "gpt-5.5"
$reasoning = Get-CodexConfigValue "model_reasoning_effort" "high"
$tier = Get-CodexConfigValue "service_tier" "fast"
$branch = Get-GitBranch
$now = Get-Date

$payload = [ordered]@{
  "model-with-reasoning" = "$model $reasoning $tier"
  "project-name" = $project
  "git-branch" = $branch
  "run-state" = "Ready"
  "context-used" = "0% used"
  "context_pct" = 0
  "used-tokens" = "session started"
  "five-hour-limit" = "5h 0%"
  "five_hour_pct" = 0
  "weekly-limit" = "weekly 0%"
  "weekly_pct" = 0
  "task-progress" = "0/0"
  "goal_text" = ("Session start " + $now.ToString("HH:mm"))
  "hook" = "SessionStart"
  "cwd" = $cwd
}

if ($stdinText) {
  $payload["hook_stdin"] = $stdinText
}

$tempDir = Join-Path ([System.IO.Path]::GetTempPath()) "codex-xteink"
New-Item -ItemType Directory -Force -Path $tempDir | Out-Null
$payloadPath = Join-Path $tempDir ("session-start-" + [guid]::NewGuid().ToString("N") + ".json")
$payloadJson = $payload | ConvertTo-Json -Compress
[System.IO.File]::WriteAllText($payloadPath, $payloadJson, [System.Text.UTF8Encoding]::new($false))

$workerArgs = "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`" -RunWorker -PayloadPath `"$payloadPath`""

try {
  "[$(Get-Date -Format o)] SessionStart enqueue $payloadPath" | Add-Content -LiteralPath $logPath -Encoding utf8
  Start-Process -FilePath "powershell.exe" -ArgumentList $workerArgs -WindowStyle Hidden | Out-Null
} catch {
  "[$(Get-Date -Format o)] failed to start worker: $_" | Add-Content -LiteralPath $errPath -Encoding utf8
}

Write-Output '{"continue":true}'
