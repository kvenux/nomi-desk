param(
  [switch]$Force
)

$ErrorActionPreference = "Stop"

$hookScript = (Resolve-Path (Join-Path $PSScriptRoot "codex_status_hook.ps1")).Path
$codexHome = if ($env:CODEX_HOME) { $env:CODEX_HOME } else { Join-Path $HOME ".codex" }
$hooksPath = Join-Path $codexHome "hooks.json"

New-Item -ItemType Directory -Force $codexHome | Out-Null

if ((Test-Path $hooksPath) -and -not $Force) {
  $backup = "$hooksPath.bak.$(Get-Date -Format yyyyMMdd-HHmmss)"
  Copy-Item -LiteralPath $hooksPath -Destination $backup
  Write-Host "Existing hooks.json backed up to $backup"
}

function New-HookGroup($eventName) {
  return ,@(
    @{
      matcher = $null
      hooks = @(
        @{
          type = "command"
          command = "powershell -NoProfile -ExecutionPolicy Bypass -File `"$hookScript`" $eventName"
          timeoutSec = 10
        }
      )
    }
  )
}

function Set-JsonProperty {
  param(
    [Parameter(Mandatory = $true)]$Object,
    [Parameter(Mandatory = $true)][string]$Name,
    [Parameter(Mandatory = $true)]$Value
  )

  $property = $Object.PSObject.Properties[$Name]
  if ($property) {
    $property.Value = $Value
  } else {
    $Object | Add-Member -MemberType NoteProperty -Name $Name -Value $Value
  }
}

$config = [pscustomobject]@{}
if (Test-Path -LiteralPath $hooksPath) {
  try {
    $existingJson = Get-Content -LiteralPath $hooksPath -Raw
    if (-not [string]::IsNullOrWhiteSpace($existingJson)) {
      $config = $existingJson | ConvertFrom-Json
    }
  } catch {
    throw "Failed to parse existing hooks.json at $hooksPath. Fix or remove it before installing Nomi hooks. $($_.Exception.Message)"
  }
}

if ($null -eq $config.PSObject.Properties["hooks"]) {
  Set-JsonProperty -Object $config -Name "hooks" -Value ([pscustomobject]@{})
} elseif ($null -eq $config.hooks) {
  Set-JsonProperty -Object $config -Name "hooks" -Value ([pscustomobject]@{})
}

$eventNames = @(
  "SessionStart",
  "UserPromptSubmit",
  "PermissionRequest",
  "PreToolUse",
  "PostToolUse",
  "Stop"
)

foreach ($eventName in $eventNames) {
  Set-JsonProperty -Object $config.hooks -Name $eventName -Value (New-HookGroup $eventName)
}

$json = $config | ConvertTo-Json -Depth 20
if ($PSVersionTable.PSVersion.Major -ge 6) {
  Set-Content -LiteralPath $hooksPath -Value $json -Encoding utf8NoBOM
} else {
  [System.IO.File]::WriteAllText($hooksPath, $json, [System.Text.UTF8Encoding]::new($false))
}

Write-Host "Installed global Codex hooks to $hooksPath"
Write-Host "Open Codex and run /hooks once, then trust the new hooks before expecting them to run."
