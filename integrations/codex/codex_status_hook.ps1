param(
  [string]$EventName = "Unknown"
)

$ErrorActionPreference = "SilentlyContinue"

[Console]::InputEncoding = [System.Text.Encoding]::UTF8
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8

function Get-NomiState {
  param([string]$Name)

  switch ($Name) {
    "SessionStart" { "idle"; break }
    "UserPromptSubmit" { "active"; break }
    "PermissionRequest" { "waiting_approval"; break }
    "PreToolUse" { "active"; break }
    "PostToolUse" { "active"; break }
    "Stop" { "idle"; break }
    default { "active"; break }
  }
}

function Normalize-NomiPrompt {
  param([string]$Text)

  if ([string]::IsNullOrWhiteSpace($Text)) {
    return $null
  }

  $normalized = ($Text -replace "\s+", " ").Trim()
  return Limit-NomiUtf8Bytes -Text $normalized -MaxBytes 180
}

function Limit-NomiUtf8Bytes {
  param(
    [string]$Text,
    [int]$MaxBytes
  )

  if ([string]::IsNullOrEmpty($Text)) {
    return $Text
  }

  $encoding = [System.Text.Encoding]::UTF8
  if ($encoding.GetByteCount($Text) -le $MaxBytes) {
    return $Text
  }

  $builder = [System.Text.StringBuilder]::new()
  $bytes = 0
  $enumerator = [System.Globalization.StringInfo]::GetTextElementEnumerator($Text)
  while ($enumerator.MoveNext()) {
    $element = [string]$enumerator.Current
    $elementBytes = $encoding.GetByteCount($element)
    if (($bytes + $elementBytes) -gt $MaxBytes) {
      break
    }
    [void]$builder.Append($element)
    $bytes += $elementBytes
  }

  return $builder.ToString()
}

function Normalize-NomiSession {
  param([string]$Text)

  if ([string]::IsNullOrWhiteSpace($Text)) {
    return $null
  }

  return Limit-NomiUtf8Bytes -Text (($Text -replace "\s+", " ").Trim()) -MaxBytes 64
}

function Find-NomiPromptValue {
  param($Value)

  $keys = @("prompt", "user_prompt", "input", "text", "message", "content")

  if ($null -eq $Value) {
    return $null
  }

  if ($Value -is [string]) {
    return $Value
  }

  if ($Value -is [System.Collections.IEnumerable] -and -not ($Value -is [string]) -and -not ($Value -is [System.Collections.IDictionary])) {
    foreach ($child in $Value) {
      $found = Find-NomiPromptValue $child
      if (-not [string]::IsNullOrWhiteSpace($found)) {
        return $found
      }
    }
    return $null
  }

  if ($Value -is [System.Collections.IDictionary]) {
    foreach ($key in $keys) {
      if ($Value.Contains($key)) {
        return Find-NomiPromptValue $Value[$key]
      }
    }
    return $null
  }

  $properties = @($Value.PSObject.Properties)
  foreach ($key in $keys) {
    $property = $properties | Where-Object { $_.Name -eq $key } | Select-Object -First 1
    if ($null -ne $property) {
      return Find-NomiPromptValue $property.Value
    }
  }

  return $null
}

function Get-NomiPrompt {
  param(
    [string]$Name,
    [string]$RawInput
  )

  if ($Name -eq "UserPromptSubmit" -and -not [string]::IsNullOrWhiteSpace($RawInput)) {
    try {
      $json = $RawInput | ConvertFrom-Json
      $prompt = Find-NomiPromptValue $json
      $normalized = Normalize-NomiPrompt $prompt
      if (-not [string]::IsNullOrWhiteSpace($normalized)) {
        return $normalized
      }
    } catch {
      return $null
    }
  }

  return $null
}

function Remove-OldNomiPayloads {
  param([string]$Directory)

  $cutoff = (Get-Date).AddMinutes(-30)
  Get-ChildItem -LiteralPath $Directory -Filter "payload-*.json" -File |
    Where-Object { $_.LastWriteTime -lt $cutoff } |
    Remove-Item -Force
}

function Write-NomiUtf8NoBom {
  param(
    [string]$Path,
    [string]$Value
  )

  [System.IO.File]::WriteAllText($Path, $Value, [System.Text.UTF8Encoding]::new($false))
}

function Write-NomiHookLog {
  param([string]$Message)

  try {
    $nomiTemp = Join-Path $env:TEMP "nomi"
    New-Item -ItemType Directory -Force $nomiTemp | Out-Null
    $line = "$(Get-Date -Format o) $Message"
    Add-Content -LiteralPath (Join-Path $nomiTemp "hook.log") -Value $line -Encoding UTF8
  } catch {
  }
}

function Resolve-NomiSender {
  param([string]$RepoRoot)

  $candidates = @()
  if (-not [string]::IsNullOrWhiteSpace($env:NOMI_SEND_PATH)) {
    $candidates += $env:NOMI_SEND_PATH
  }

  if (-not [string]::IsNullOrWhiteSpace($RepoRoot)) {
    $candidates += (Join-Path $RepoRoot "tools\nomi-send\target\release\nomi-send.exe")
    $candidates += (Join-Path $RepoRoot "tools\nomi-send\target\debug\nomi-send.exe")
  }

  foreach ($candidate in $candidates) {
    if (Test-Path -LiteralPath $candidate) {
      return (Resolve-Path -LiteralPath $candidate).Path
    }
  }

  $command = Get-Command "nomi-send" -CommandType Application -ErrorAction SilentlyContinue
  if ($command) {
    return $command.Source
  }

  $command = Get-Command "nomi-send.exe" -CommandType Application -ErrorAction SilentlyContinue
  if ($command) {
    return $command.Source
  }

  return $null
}

try {
  $stdinText = [Console]::In.ReadToEnd()
  $cwd = (Get-Location).Path
  $prompt = Get-NomiPrompt -Name $EventName -RawInput $stdinText
  $payload = [ordered]@{
    protocol = "nomi-agent-display"
    version = 1
    source = "codex"
    state = Get-NomiState $EventName
    time = (Get-Date).ToString("HH:mm")
    event = $EventName
    session = Normalize-NomiSession (Split-Path -Leaf $cwd)
  }

  if (-not [string]::IsNullOrWhiteSpace($prompt)) {
    $payload.prompt = $prompt
  }

  $nomiTemp = Join-Path $env:TEMP "nomi"
  New-Item -ItemType Directory -Force $nomiTemp | Out-Null
  Remove-OldNomiPayloads -Directory $nomiTemp
  $payloadPath = Join-Path $nomiTemp "payload-$([guid]::NewGuid().ToString()).json"
  Write-NomiUtf8NoBom -Path $payloadPath -Value ($payload | ConvertTo-Json -Compress)

  $repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
  $sender = Resolve-NomiSender -RepoRoot $repoRoot

  if ($sender) {
    Start-Process `
      -FilePath $sender `
      -ArgumentList @("enqueue", "--json-file", $payloadPath) `
      -WindowStyle Hidden | Out-Null
  } else {
    Write-NomiHookLog "nomi-send not found. Build tools\nomi-send, set NOMI_SEND_PATH, or put nomi-send on PATH."
  }
} catch {
  Write-NomiHookLog "hook failed: $($_.Exception.Message)"
}

Write-Output '{"continue":true}'
