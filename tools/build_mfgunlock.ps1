[CmdletBinding()]
param(
  [int[]]$ReShadeApi = @(18),
  [string]$RenoDx = "$env:USERPROFILE\Downloads\RenoDX",
  [ValidateSet("Release", "Debug", "RelWithDebInfo")]
  [string]$Configuration = "Release",
  [string]$OutputRoot = "",
  [string]$CacheRoot = "",
  [switch]$RefreshHeaders
)

$ErrorActionPreference = "Stop"

$RepoRoot = Split-Path $PSScriptRoot -Parent
$TargetsPath = Join-Path $PSScriptRoot "reshade-api-targets.json"
$AddonSource = Join-Path $RepoRoot "src\addons\mfgunlock"
$AddonDestination = Join-Path $RenoDx "src\addons\mfgunlock"
$BuildRoot = Join-Path $RenoDx "build.vs"
$NvapiRoot = Join-Path $RenoDx "external\NVAPI"

if ([string]::IsNullOrWhiteSpace($OutputRoot)) {
  $OutputRoot = Join-Path $RepoRoot "dist\mfgunlock"
}
if ([string]::IsNullOrWhiteSpace($CacheRoot)) {
  $CacheRoot = Join-Path $env:LOCALAPPDATA "MFGAmpereUnlock\reshade-api"
}

if (-not (Test-Path $TargetsPath)) {
  throw "Missing API target manifest: $TargetsPath"
}
if (-not (Test-Path $RenoDx)) {
  throw "RenoDX tree not found: $RenoDx"
}
if (-not (Test-Path $AddonSource)) {
  throw "MFGAmpereUnlock source not found: $AddonSource"
}
if (-not (Test-Path $NvapiRoot)) {
  throw "NVAPI dependency not found: $NvapiRoot"
}
if ($ReShadeApi.Count -eq 0) {
  throw "Specify at least one ReShade API version."
}

$Targets = Get-Content $TargetsPath -Raw | ConvertFrom-Json

function Invoke-Checked {
  param(
    [Parameter(Mandatory = $true)]
    [string]$FilePath,
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$Arguments
  )

  & $FilePath @Arguments 2>&1 | Out-Host
  $ExitCode = $LASTEXITCODE
  if ($ExitCode -ne 0) {
    throw "$FilePath failed with exit code $ExitCode"
  }
}

function Get-Target {
  param(
    [Parameter(Mandatory = $true)]
    [int]$Api
  )

  $Key = [string]$Api
  $Property = $Targets.PSObject.Properties[$Key]
  if ($null -eq $Property) {
    $Known = @($Targets.PSObject.Properties.Name | Sort-Object {[int]$_}) -join ", "
    throw "Unsupported ReShade API $Api. Known API targets: $Known"
  }
  return $Property.Value
}

function Get-ReShadeSource {
  param(
    [Parameter(Mandatory = $true)]
    [int]$Api,
    [Parameter(Mandatory = $true)]
    [string]$Ref
  )

  $Path = Join-Path $CacheRoot "api$Api"
  $Marker = Join-Path $Path ".mfgunlock-ref"
  $NeedsFetch = $RefreshHeaders -or -not (Test-Path (Join-Path $Path ".git"))

  if (-not $NeedsFetch -and (Test-Path $Marker)) {
    $NeedsFetch = (Get-Content $Marker -Raw).Trim() -ne $Ref
  }
  elseif (-not $NeedsFetch) {
    $NeedsFetch = $true
  }

  if ($NeedsFetch) {
    Remove-Item $Path -Recurse -Force -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force (Split-Path $Path -Parent) | Out-Null
    Invoke-Checked git clone --depth 1 --branch $Ref https://github.com/crosire/reshade.git $Path
    Invoke-Checked git -C $Path submodule update --init deps/imgui
    Set-Content -Path $Marker -Value $Ref -Encoding ASCII
  }

  $ReShadeHeader = Join-Path $Path "include\reshade.hpp"
  $OverlayHeader = Join-Path $Path "include\reshade_overlay.hpp"
  $ImGuiHeader = Join-Path $Path "deps\imgui\imgui.h"
  if (-not (Test-Path $ReShadeHeader) -or
      -not (Test-Path $OverlayHeader) -or
      -not (Test-Path $ImGuiHeader)) {
    throw "Incomplete ReShade API checkout for API $Api at $Path"
  }

  $HeaderText = Get-Content $ReShadeHeader -Raw
  if ($HeaderText -notmatch '#define\s+RESHADE_API_VERSION\s+(\d+)') {
    throw "Could not read RESHADE_API_VERSION from $ReShadeHeader"
  }
  $HeaderApi = [int]$Matches[1]
  if ($HeaderApi -ne $Api) {
    throw "ReShade ref $Ref exposes API $HeaderApi, expected API $Api"
  }

  $OverlayText = Get-Content $OverlayHeader -Raw
  $ImGuiText = Get-Content $ImGuiHeader -Raw
  if ($OverlayText -notmatch 'IMGUI_VERSION_NUM\s*!=\s*(\d+)') {
    throw "Could not read the ReShade ImGui ABI from $OverlayHeader"
  }
  $ExpectedImGui = [int]$Matches[1]
  if ($ImGuiText -notmatch '#define\s+IMGUI_VERSION_NUM\s+(\d+)') {
    throw "Could not read IMGUI_VERSION_NUM from $ImGuiHeader"
  }
  $ActualImGui = [int]$Matches[1]
  if ($ExpectedImGui -ne $ActualImGui) {
    throw "ReShade $Ref expects ImGui $ExpectedImGui, checkout contains $ActualImGui"
  }

  return $Path
}

New-Item -ItemType Directory -Force $AddonDestination | Out-Null
Copy-Item (Join-Path $AddonSource "*") $AddonDestination -Recurse -Force

if (-not (Test-Path $BuildRoot)) {
  Push-Location $RenoDx
  try {
    Invoke-Checked cmake --preset vs-x64
  }
  finally {
    Pop-Location
  }
}

$OriginalCl = $env:CL
$Results = @()

try {
  foreach ($Api in $ReShadeApi) {
    $Target = Get-Target -Api $Api
    $Ref = [string]$Target.reshade_ref
    if ([string]::IsNullOrWhiteSpace($Ref)) {
      throw "ReShade API $Api has no reshade_ref in $TargetsPath"
    }

    $ResizeArg = if ([bool]$Target.swapchain_resize_arg) { 1 } else { 0 }
    $ReShadeSource = Get-ReShadeSource -Api $Api -Ref $Ref
    $ReShadeInclude = Join-Path $ReShadeSource "include"
    $ImGuiInclude = Join-Path $ReShadeSource "deps\imgui"

    $ClOptions = @(
      "/I`"$NvapiRoot`"",
      "/I`"$ImGuiInclude`"",
      "/I`"$ReShadeInclude`"",
      "/DMFGUNLOCK_RESHADE_HEADER_OVERRIDE=1",
      "/DMFGUNLOCK_RESHADE_API=$Api",
      "/DMFGUNLOCK_RESHADE_SWAPCHAIN_RESIZE_ARG=$ResizeArg"
    )
    if (-not [string]::IsNullOrWhiteSpace($OriginalCl)) {
      $ClOptions += $OriginalCl
    }
    $env:CL = $ClOptions -join " "

    Write-Host ""
    Write-Host "===== BUILD MFGUNLOCK FOR RESHADE API $Api ($Ref) ====="
    Invoke-Checked cmake --build $BuildRoot --config $Configuration --target mfgunlock --clean-first

    $Artifact = Join-Path $BuildRoot "$Configuration\renodx-mfgunlock.addon64"
    if (-not (Test-Path $Artifact)) {
      throw "Build succeeded but artifact was not found: $Artifact"
    }

    $ApiOutput = Join-Path $OutputRoot "reshade-api$Api"
    New-Item -ItemType Directory -Force $ApiOutput | Out-Null
    $OutputArtifact = Join-Path $ApiOutput "renodx-mfgunlock.addon64"
    Copy-Item $Artifact $OutputArtifact -Force
    $Hash = Get-FileHash $OutputArtifact -Algorithm SHA256

    $Results += [PSCustomObject]@{
      ReShadeApi = $Api
      ReShadeRef = $Ref
      File = $OutputArtifact
      SHA256 = $Hash.Hash
    }
  }
}
finally {
  if ($null -eq $OriginalCl) {
    Remove-Item Env:CL -ErrorAction SilentlyContinue
  }
  else {
    $env:CL = $OriginalCl
  }
}

Write-Host ""
Write-Host "===== BUILD RESULTS ====="
$Results | Format-Table -AutoSize
