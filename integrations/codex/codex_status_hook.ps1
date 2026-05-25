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

function Normalize-NomiField {
  param(
    [string]$Text,
    [int]$MaxBytes
  )

  if ([string]::IsNullOrWhiteSpace($Text)) {
    return $null
  }

  return Limit-NomiUtf8Bytes -Text (($Text -replace "\s+", " ").Trim()) -MaxBytes $MaxBytes
}

function ConvertTo-NomiNumber {
  param($Value)

  if ($null -eq $Value) {
    return $null
  }

  try {
    return [double]$Value
  } catch {
    return $null
  }
}

function Clamp-NomiPercent {
  param($Value)

  $number = ConvertTo-NomiNumber $Value
  if ($null -eq $number) {
    return $null
  }

  $pct = [int][Math]::Round($number)
  if ($pct -lt 0) {
    return 0
  }
  if ($pct -gt 100) {
    return 100
  }
  return $pct
}

function Get-NomiDisplayTokenCount {
  param($Usage)

  if ($null -eq $Usage) {
    return $null
  }

  $inputTokens = ConvertTo-NomiNumber $Usage.input_tokens
  if ($null -ne $inputTokens) {
    $cachedInputTokens = ConvertTo-NomiNumber $Usage.cached_input_tokens
    $outputTokens = ConvertTo-NomiNumber $Usage.output_tokens
    if ($null -eq $cachedInputTokens) {
      $cachedInputTokens = 0
    }
    if ($null -eq $outputTokens) {
      $outputTokens = 0
    }

    $displayTokens = $inputTokens - $cachedInputTokens + $outputTokens
    if ($displayTokens -gt 0) {
      return $displayTokens
    }
  }

  return ConvertTo-NomiNumber $Usage.total_tokens
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

function Get-CodexHome {
  if (-not [string]::IsNullOrWhiteSpace($env:CODEX_HOME)) {
    return $env:CODEX_HOME
  }

  return (Join-Path $HOME ".codex")
}

function Get-CodexConfigValue {
  param(
    [string]$CodexHome,
    [string]$Name
  )

  if ([string]::IsNullOrWhiteSpace($CodexHome)) {
    return $null
  }

  $configPath = Join-Path $CodexHome "config.toml"
  if (-not (Test-Path -LiteralPath $configPath)) {
    return $null
  }

  $pattern = '^\s*' + [regex]::Escape($Name) + '\s*=\s*"([^"]*)"'
  $match = Select-String -LiteralPath $configPath -Pattern $pattern | Select-Object -First 1
  if ($match) {
    return $match.Matches[0].Groups[1].Value
  }
  return $null
}

function Read-NomiTokenSnapshotFromFile {
  param([string]$Path)

  if ([string]::IsNullOrWhiteSpace($Path) -or -not (Test-Path -LiteralPath $Path)) {
    return $null
  }

  $lastTokenCount = $null
  $stream = $null
  $reader = $null
  try {
    $share = [System.IO.FileShare]([System.IO.FileShare]::ReadWrite -bor [System.IO.FileShare]::Delete)
    $stream = [System.IO.File]::Open($Path, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, $share)
    $reader = [System.IO.StreamReader]::new($stream, [System.Text.Encoding]::UTF8, $true)
    while (($line = $reader.ReadLine()) -ne $null) {
      if ([string]::IsNullOrWhiteSpace($line)) {
        continue
      }

      try {
        $row = $line | ConvertFrom-Json -ErrorAction Stop
      } catch {
        continue
      }

      if ($row.type -eq "event_msg" -and $row.payload.type -eq "token_count") {
        $lastTokenCount = $row.payload
      }
    }
  } catch {
    return $null
  } finally {
    if ($null -ne $reader) {
      $reader.Dispose()
    } elseif ($null -ne $stream) {
      $stream.Dispose()
    }
  }

  if ($null -eq $lastTokenCount) {
    return $null
  }

  $info = $lastTokenCount.info
  $windowUsage = $null
  if ($info.last_token_usage) {
    $windowUsage = $info.last_token_usage
  } elseif ($info.total_token_usage) {
    $windowUsage = $info.total_token_usage
  }

  $displayUsage = $windowUsage
  if ($info.total_token_usage) {
    $displayUsage = $info.total_token_usage
  }

  $snapshot = [ordered]@{}
  $displayTokens = Get-NomiDisplayTokenCount $displayUsage
  if ($null -ne $displayTokens -and $displayTokens -gt 0) {
    $snapshot.used_tokens_k = [int][Math]::Round($displayTokens / 1000)
  }

  $windowTokens = ConvertTo-NomiNumber $windowUsage.total_tokens
  $contextWindow = ConvertTo-NomiNumber $info.model_context_window
  if ($null -ne $windowTokens -and $windowTokens -gt 0) {
    if ($null -ne $contextWindow -and $contextWindow -gt 0) {
      $snapshot.context_pct = Clamp-NomiPercent ($windowTokens * 100 / $contextWindow)
    }
  }

  $quota = [ordered]@{}
  $primaryUsed = ConvertTo-NomiNumber $lastTokenCount.rate_limits.primary.used_percent
  if ($null -ne $primaryUsed) {
    $quota.five_hour_left = Clamp-NomiPercent (100 - [int]$primaryUsed)
  }
  $secondaryUsed = ConvertTo-NomiNumber $lastTokenCount.rate_limits.secondary.used_percent
  if ($null -ne $secondaryUsed) {
    $quota.weekly_left = Clamp-NomiPercent (100 - [int]$secondaryUsed)
  }
  if ($quota.Count -gt 0) {
    $snapshot.quota = $quota
  }

  if ($snapshot.Count -eq 0) {
    return $null
  }
  return $snapshot
}

function Get-NomiTokenSnapshot {
  param([string]$CodexHome)

  if ([string]::IsNullOrWhiteSpace($CodexHome)) {
    return $null
  }

  $sessionsDir = Join-Path $CodexHome "sessions"
  if (-not (Test-Path -LiteralPath $sessionsDir)) {
    return $null
  }

  $files = Get-ChildItem -LiteralPath $sessionsDir -Recurse -Filter "*.jsonl" -File |
    Sort-Object LastWriteTimeUtc -Descending |
    Select-Object -First 10

  foreach ($file in $files) {
    $snapshot = Read-NomiTokenSnapshotFromFile -Path $file.FullName
    if ($null -ne $snapshot) {
      return $snapshot
    }
  }

  return $null
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
  $codexHome = Get-CodexHome
  $model = Normalize-NomiField -Text (Get-CodexConfigValue -CodexHome $codexHome -Name "model") -MaxBytes 32
  $effort = Normalize-NomiField -Text (Get-CodexConfigValue -CodexHome $codexHome -Name "model_reasoning_effort") -MaxBytes 16
  $tier = Normalize-NomiField -Text (Get-CodexConfigValue -CodexHome $codexHome -Name "service_tier") -MaxBytes 16
  $tokenSnapshot = Get-NomiTokenSnapshot -CodexHome $codexHome
  $payload = [ordered]@{
    protocol = "nomi-agent-display"
    version = 1
    source = "codex"
    state = Get-NomiState $EventName
    time = (Get-Date).ToString("HH:mm")
    event = $EventName
    session = Normalize-NomiSession (Split-Path -Leaf $cwd)
  }

  if (-not [string]::IsNullOrWhiteSpace($model)) {
    $payload["model"] = $model
  }
  if (-not [string]::IsNullOrWhiteSpace($effort)) {
    $payload["effort"] = $effort
  }
  if (-not [string]::IsNullOrWhiteSpace($tier)) {
    $payload["tier"] = $tier
  }
  if (-not [string]::IsNullOrWhiteSpace($prompt)) {
    $payload["prompt"] = $prompt
  }
  if ($null -ne $tokenSnapshot) {
    foreach ($entry in $tokenSnapshot.GetEnumerator()) {
      $payload[$entry.Key] = $entry.Value
    }
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
