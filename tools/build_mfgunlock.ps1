[CmdletBinding()]
param(
  [int[]]$ReShadeApi = @(18),
  [string]$RenoDx = "",
  [ValidateSet("Release", "Debug", "RelWithDebInfo")]
  [string]$Configuration = "Release",
  [string]$OutputRoot = "",
  [string]$CacheRoot = "",
  [switch]$RefreshHeaders,
  [switch]$RefreshDependencies
)

$ErrorActionPreference = "Stop"

$RepoRoot = Split-Path $PSScriptRoot -Parent
$TargetsPath = Join-Path $PSScriptRoot "reshade-api-targets.json"
$DependenciesPath = Join-Path $PSScriptRoot "build-dependencies.json"
$AddonSource = Join-Path $RepoRoot "src\addons\mfgunlock"
$DefaultRenoDx = Join-Path $RepoRoot "external\RenoDX"

if ([string]::IsNullOrWhiteSpace($RenoDx)) {
  $RenoDx = $DefaultRenoDx
}

$RenoDx = [IO.Path]::GetFullPath($RenoDx)
$DefaultRenoDx = [IO.Path]::GetFullPath($DefaultRenoDx)
$ManagedRenoDx = [StringComparer]::OrdinalIgnoreCase.Equals($RenoDx, $DefaultRenoDx)

$AddonDestination = Join-Path $RenoDx "src\addons\mfgunlock"
$BuildRoot = Join-Path $RenoDx "build.vs"
$NvapiRoot = Join-Path $RenoDx "external\NVAPI"
$ReShadeHistoryRoot = Join-Path $RenoDx "external\reshade"
$ReShadeRepositoryUrl = "https://github.com/crosire/reshade.git"

if ([string]::IsNullOrWhiteSpace($OutputRoot)) {
  $OutputRoot = Join-Path $RepoRoot "dist\mfgunlock"
}
if ([string]::IsNullOrWhiteSpace($CacheRoot)) {
  $CacheRoot = Join-Path $RepoRoot "external\reshade-api"
}

$OutputRoot = [IO.Path]::GetFullPath($OutputRoot)
$CacheRoot = [IO.Path]::GetFullPath($CacheRoot)

Write-Host "Build workspace:"
Write-Host "  RenoDX:        $RenoDx"
Write-Host "  ReShade cache: $CacheRoot"
Write-Host "  Output:        $OutputRoot"


function Invoke-Checked {
  param(
    [Parameter(Mandatory = $true)]
    [string]$FilePath,
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$Arguments
  )

  # Windows PowerShell 5.1 turns native stderr into ErrorRecord objects.
  # With the script-wide ErrorActionPreference=Stop, harmless diagnostics from
  # tools such as Git (for example, checkout/reset status messages) would
  # otherwise abort the build even when the native process exits with code 0.
  # Decide native-command success exclusively from its process exit code.
  $PreviousErrorActionPreference = $ErrorActionPreference
  try {
    $ErrorActionPreference = "Continue"
    & $FilePath @Arguments 2>&1 | ForEach-Object {
      if ($_ -is [System.Management.Automation.ErrorRecord]) {
        Write-Host $_.Exception.Message
      }
      else {
        Write-Host $_
      }
    }
    $ExitCode = $LASTEXITCODE
  }
  finally {
    $ErrorActionPreference = $PreviousErrorActionPreference
  }

  if ($ExitCode -ne 0) {
    throw "$FilePath failed with exit code $ExitCode"
  }
}


function Invoke-NativeCapture {
  param(
    [Parameter(Mandatory = $true)]
    [string]$FilePath,
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$Arguments
  )

  $PreviousErrorActionPreference = $ErrorActionPreference
  try {
    $ErrorActionPreference = "Continue"
    $Output = @(& $FilePath @Arguments 2>$null)
    $ExitCode = $LASTEXITCODE
  }
  finally {
    $ErrorActionPreference = $PreviousErrorActionPreference
  }

  if ($ExitCode -ne 0) {
    throw "$FilePath failed with exit code $ExitCode"
  }

  return $Output
}

function Assert-Command {
  param(
    [Parameter(Mandatory = $true)]
    [string]$Name
  )

  if (-not (Get-Command $Name -ErrorAction SilentlyContinue)) {
    throw "Required command not found: $Name"
  }
}

function Get-GitHead {
  param(
    [Parameter(Mandatory = $true)]
    [string]$Path
  )

  $Value = & git -C $Path rev-parse HEAD 2>$null
  if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace(($Value -join ""))) {
    throw "Could not read Git HEAD at: $Path"
  }
  return (($Value | Select-Object -First 1).Trim())
}

function Resolve-GitRef {
  param(
    [Parameter(Mandatory = $true)]
    [string]$Path,
    [Parameter(Mandatory = $true)]
    [string]$Ref
  )

  $Value = & git -C $Path rev-parse "$Ref^{commit}" 2>$null
  if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace(($Value -join ""))) {
    return $null
  }
  return (($Value | Select-Object -First 1).Trim())
}

function Set-PinnedCheckout {
  param(
    [Parameter(Mandatory = $true)]
    [string]$Path,
    [Parameter(Mandatory = $true)]
    [string]$Url,
    [Parameter(Mandatory = $true)]
    [string]$Ref,
    [Parameter(Mandatory = $true)]
    [string]$Name,
    [switch]$RecursiveSubmodules
  )

  if (-not (Test-Path $Path)) {
    Write-Host ""
    Write-Host "===== FETCH $Name ====="
    New-Item -ItemType Directory -Force (Split-Path $Path -Parent) | Out-Null
    Invoke-Checked git clone $Url $Path
  }
  elseif (-not (Test-Path (Join-Path $Path ".git"))) {
    throw "$Name path exists but is not a Git checkout: $Path"
  }

  $ResolvedRef = Resolve-GitRef -Path $Path -Ref $Ref
  if ($null -eq $ResolvedRef) {
    Write-Host ""
    Write-Host "===== FETCH $Name REF $Ref ====="
    Invoke-Checked git -C $Path fetch origin
    $ResolvedRef = Resolve-GitRef -Path $Path -Ref $Ref
    if ($null -eq $ResolvedRef) {
      throw "Could not resolve $Name ref $Ref in $Path"
    }
  }

  $CurrentHead = Get-GitHead -Path $Path
  if ($CurrentHead -ne $ResolvedRef) {
    $TrackedChanges = @(& git -C $Path status --porcelain --untracked-files=no)
    if ($LASTEXITCODE -ne 0) {
      throw "Could not inspect $Name worktree: $Path"
    }
    if ($TrackedChanges.Count -gt 0) {
      throw "$Name has tracked local changes; refusing to switch it to pinned ref $Ref. Path: $Path"
    }

    Write-Host ""
    Write-Host "===== PIN $Name -> $Ref ====="
    Invoke-Checked git -C $Path checkout --detach $ResolvedRef
  }

  if ($RecursiveSubmodules) {
    Write-Host ""
    Write-Host "===== SYNC $Name SUBMODULES ====="
    Invoke-Checked git -C $Path submodule sync --recursive
    Invoke-Checked git -C $Path submodule update --init --recursive
  }

  $FinalHead = Get-GitHead -Path $Path
  Write-Host "$Name HEAD: $FinalHead"
}

Assert-Command -Name git
Assert-Command -Name cmake

if (-not (Test-Path $TargetsPath)) {
  throw "Missing API target manifest: $TargetsPath"
}
if (-not (Test-Path $DependenciesPath)) {
  throw "Missing build dependency manifest: $DependenciesPath"
}
if (-not (Test-Path $AddonSource)) {
  throw "MFGAmpereUnlock source not found: $AddonSource"
}
if ($ReShadeApi.Count -eq 0) {
  throw "Specify at least one ReShade API version."
}

$Dependencies = Get-Content $DependenciesPath -Raw | ConvertFrom-Json
$RenoDxUrl = [string]$Dependencies.renodx.url
$RenoDxRef = [string]$Dependencies.renodx.ref
$NvapiUrl = [string]$Dependencies.nvapi.url
$NvapiRef = [string]$Dependencies.nvapi.ref
$ShaderTools = @($Dependencies.shader_tools | ForEach-Object { [string]$_ })

foreach ($Value in @($RenoDxUrl, $RenoDxRef, $NvapiUrl, $NvapiRef)) {
  if ([string]::IsNullOrWhiteSpace($Value)) {
    throw "Incomplete dependency manifest: $DependenciesPath"
  }
}

if ($ManagedRenoDx) {
  Set-PinnedCheckout `
    -Path $RenoDx `
    -Url $RenoDxUrl `
    -Ref $RenoDxRef `
    -Name "RenoDX" `
    -RecursiveSubmodules
}
else {
  if (-not (Test-Path $RenoDx)) {
    throw "RenoDX tree not found: $RenoDx"
  }
  if (-not (Test-Path (Join-Path $RenoDx ".git"))) {
    throw "RenoDX path is not a Git checkout: $RenoDx"
  }
  if ($RefreshDependencies) {
    Write-Host ""
    Write-Host "===== REFRESH RENO DX SUBMODULES ====="
    Invoke-Checked git -C $RenoDx submodule sync --recursive
    Invoke-Checked git -C $RenoDx submodule update --init --recursive
  }
  Write-Host "RenoDX HEAD: $(Get-GitHead -Path $RenoDx)"
}

Set-PinnedCheckout `
  -Path $NvapiRoot `
  -Url $NvapiUrl `
  -Ref $NvapiRef `
  -Name "NVAPI"

$SetupDevEnv = Join-Path $RenoDx "scripts\setup-dev-env.ps1"
if (-not (Test-Path $SetupDevEnv)) {
  throw "RenoDX setup helper not found: $SetupDevEnv"
}

$RequiredToolFiles = @(
  "bin\fxc.exe",
  "bin\dxc.exe",
  "bin\slangc.exe",
  "bin\glslang.exe"
)
$MissingToolFiles = @(
  $RequiredToolFiles | Where-Object { -not (Test-Path (Join-Path $RenoDx $_)) }
)

if ($RefreshDependencies -or $MissingToolFiles.Count -gt 0) {
  Write-Host ""
  Write-Host "===== SET UP RENO DX SHADER TOOLS ====="
  Push-Location $RenoDx
  try {
    & $SetupDevEnv -Update -Tools $ShaderTools
  }
  finally {
    Pop-Location
  }
}

$MissingToolFiles = @(
  $RequiredToolFiles | Where-Object { -not (Test-Path (Join-Path $RenoDx $_)) }
)
if ($MissingToolFiles.Count -gt 0) {
  throw @"
RenoDX developer tools are still incomplete after bootstrap.
Missing:
  $($MissingToolFiles -join "`n  ")
Make sure Visual Studio 2022 with Desktop development with C++ and the Windows SDK are installed.
"@
}

if (-not (Test-Path (Join-Path $NvapiRoot "nvapi.h"))) {
  throw "NVAPI checkout is incomplete; nvapi.h not found at: $NvapiRoot"
}

if (-not (Test-Path $ReShadeHistoryRoot)) {
  throw "RenoDX ReShade submodule is missing after dependency bootstrap: $ReShadeHistoryRoot"
}

$Targets = Get-Content $TargetsPath -Raw | ConvertFrom-Json
$script:ReShadeHistoryRefreshed = $false


function Get-ReShadeApiFromRef {
  param(
    [Parameter(Mandatory = $true)]
    [string]$Repository,
    [Parameter(Mandatory = $true)]
    [string]$Ref
  )

  $Spec = "${Ref}:include/reshade.hpp"
  try {
    $Header = (Invoke-NativeCapture git -C $Repository show $Spec) -join "`n"
  }
  catch {
    return $null
  }

  if ($Header -match '(?m)^\s*#define\s+RESHADE_API_VERSION\s+(\d+)\s*$') {
    return [int]$Matches[1]
  }

  return $null
}

function Update-ReShadeHistory {
  if (-not (Test-Path $ReShadeHistoryRoot)) {
    throw "RenoDX ReShade submodule not found: $ReShadeHistoryRoot"
  }

  Write-Host ""
  Write-Host "===== REFRESH RESHADE HISTORY ====="
  Invoke-Checked git -C $ReShadeHistoryRoot fetch --force --prune --tags origin "+refs/heads/*:refs/remotes/origin/*"
  $script:ReShadeHistoryRefreshed = $true
}

function Resolve-ReShadeApiRefCore {
  param(
    [Parameter(Mandatory = $true)]
    [int]$Api
  )

  $Key = [string]$Api
  $PinnedProperty = $Targets.PSObject.Properties[$Key]
  if ($null -ne $PinnedProperty) {
    $PinnedRef = [string]$PinnedProperty.Value.reshade_ref
    if (-not [string]::IsNullOrWhiteSpace($PinnedRef)) {
      $PinnedApi = Get-ReShadeApiFromRef -Repository $ReShadeHistoryRoot -Ref $PinnedRef
      if ($PinnedApi -eq $Api) {
        return [PSCustomObject]@{
          Ref = $PinnedRef
          Source = "pinned"
        }
      }
    }
  }

  # Prefer an official ReShade tag when one exists for the requested API.
  $Tags = @(Invoke-NativeCapture git -C $ReShadeHistoryRoot tag --sort=-version:refname)
  foreach ($Tag in $Tags) {
    $Tag = [string]$Tag
    if ([string]::IsNullOrWhiteSpace($Tag)) {
      continue
    }

    $TagApi = Get-ReShadeApiFromRef -Repository $ReShadeHistoryRoot -Ref $Tag
    if ($TagApi -eq $Api) {
      return [PSCustomObject]@{
        Ref = $Tag
        Source = "tag"
      }
    }
  }

  # Some ReShade API revisions existed between release tags. Resolve those
  # directly from the upstream main-branch history. Pick the last commit that
  # still exposed the requested API before the next API-version transition.
  if ($null -eq (Resolve-GitRef -Path $ReShadeHistoryRoot -Ref "origin/main")) {
    return $null
  }

  $TransitionCommits = @(
    Invoke-NativeCapture git -C $ReShadeHistoryRoot log --reverse --format=%H -G RESHADE_API_VERSION origin/main -- include/reshade.hpp
  )

  $Transitions = @()
  $PreviousApi = $null
  foreach ($Commit in $TransitionCommits) {
    $Commit = [string]$Commit
    if ([string]::IsNullOrWhiteSpace($Commit)) {
      continue
    }

    $CommitApi = Get-ReShadeApiFromRef -Repository $ReShadeHistoryRoot -Ref $Commit
    if ($null -eq $CommitApi) {
      continue
    }

    if ($null -eq $PreviousApi -or $CommitApi -ne $PreviousApi) {
      $Transitions += [PSCustomObject]@{
        Commit = $Commit
        Api = [int]$CommitApi
      }
      $PreviousApi = [int]$CommitApi
    }
  }

  for ($Index = 0; $Index -lt $Transitions.Count; ++$Index) {
    if ($Transitions[$Index].Api -ne $Api) {
      continue
    }

    if ($Index + 1 -lt $Transitions.Count) {
      $NextCommit = [string]$Transitions[$Index + 1].Commit
      $Candidate = ((Invoke-NativeCapture git -C $ReShadeHistoryRoot rev-parse "${NextCommit}^1") | Select-Object -First 1).Trim()
      $CandidateApi = Get-ReShadeApiFromRef -Repository $ReShadeHistoryRoot -Ref $Candidate
      if ($CandidateApi -eq $Api) {
        return [PSCustomObject]@{
          Ref = $Candidate
          Source = "history"
        }
      }
    }

    $TransitionCommit = [string]$Transitions[$Index].Commit
    return [PSCustomObject]@{
      Ref = $TransitionCommit
      Source = "history"
    }
  }

  return $null
}

function Resolve-ReShadeApiRef {
  param(
    [Parameter(Mandatory = $true)]
    [int]$Api
  )

  if ($Api -le 0) {
    throw "ReShade API must be a positive integer, got: $Api"
  }

  if ($RefreshHeaders -and -not $script:ReShadeHistoryRefreshed) {
    Update-ReShadeHistory
  }

  $Resolved = Resolve-ReShadeApiRefCore -Api $Api
  if ($null -ne $Resolved) {
    return $Resolved
  }

  # The local RenoDX submodule may have been cloned before a newer ReShade API
  # existed. Refresh remote refs once before declaring the API unknown.
  if (-not $script:ReShadeHistoryRefreshed) {
    Update-ReShadeHistory
  }
  $Resolved = Resolve-ReShadeApiRefCore -Api $Api
  if ($null -ne $Resolved) {
    return $Resolved
  }

  $CurrentApi = Get-ReShadeApiFromRef -Repository $ReShadeHistoryRoot -Ref "origin/main"
  throw "ReShade API $Api was not found in upstream ReShade history. Current upstream API: $CurrentApi"
}

function Get-SwapchainResizeArg {
  param(
    [Parameter(Mandatory = $true)]
    [string]$ReShadeSource
  )

  $EventsHeader = Join-Path $ReShadeSource "include\reshade_events.hpp"
  if (-not (Test-Path $EventsHeader)) {
    throw "ReShade events header not found: $EventsHeader"
  }

  $EventsText = Get-Content $EventsHeader -Raw
  $InitPattern = 'RESHADE_DEFINE_ADDON_EVENT_TRAITS\s*\(\s*addon_event::init_swapchain\s*,\s*void\s*,\s*api::swapchain\s*\*\s*swapchain(?<resize>\s*,\s*bool\s+resize)?\s*\)\s*;'
  $DestroyPattern = 'RESHADE_DEFINE_ADDON_EVENT_TRAITS\s*\(\s*addon_event::destroy_swapchain\s*,\s*void\s*,\s*api::swapchain\s*\*\s*swapchain(?<resize>\s*,\s*bool\s+resize)?\s*\)\s*;'

  $InitMatch = [regex]::Match($EventsText, $InitPattern)
  $DestroyMatch = [regex]::Match($EventsText, $DestroyPattern)
  if (-not $InitMatch.Success -or -not $DestroyMatch.Success) {
    throw "Could not determine ReShade swapchain callback ABI from: $EventsHeader"
  }

  $InitHasResize = $InitMatch.Groups["resize"].Success
  $DestroyHasResize = $DestroyMatch.Groups["resize"].Success
  if ($InitHasResize -ne $DestroyHasResize) {
    throw "Inconsistent ReShade swapchain callback ABI in: $EventsHeader"
  }

  return $(if ($InitHasResize) { 1 } else { 0 })
}

function Get-ReShadeSource {
  param(
    [Parameter(Mandatory = $true)]
    [int]$Api,
    [Parameter(Mandatory = $true)]
    [string]$Ref
  )

  $ResolvedCommit = ((Invoke-NativeCapture git -C $ReShadeHistoryRoot rev-parse "${Ref}^{commit}") | Select-Object -First 1).Trim()
  if ([string]::IsNullOrWhiteSpace($ResolvedCommit)) {
    throw "Could not resolve ReShade ref: $Ref"
  }

  $Path = Join-Path $CacheRoot "api$Api"
  $Marker = Join-Path $Path ".mfgunlock-ref"
  $NeedsFetch = $RefreshHeaders -or -not (Test-Path (Join-Path $Path ".git"))

  if (-not $NeedsFetch -and (Test-Path $Marker)) {
    $NeedsFetch = (Get-Content $Marker -Raw).Trim() -ne $ResolvedCommit
  }
  elseif (-not $NeedsFetch) {
    $NeedsFetch = $true
  }

  if ($NeedsFetch) {
    Remove-Item $Path -Recurse -Force -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force (Split-Path $Path -Parent) | Out-Null
    Invoke-Checked git clone --reference-if-able $ReShadeHistoryRoot --no-checkout $ReShadeRepositoryUrl $Path
    Invoke-Checked git -C $Path checkout --detach $ResolvedCommit
    Invoke-Checked git -C $Path submodule update --init deps/imgui
    Set-Content -Path $Marker -Value $ResolvedCommit -Encoding ASCII
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
  $ImGuiRoot = Join-Path $Path "deps\imgui"

  # The gitlink stored by the selected ReShade commit is the authoritative
  # ImGui dependency for that source revision, including old ReShade releases
  # which did not yet publish an IMGUI_VERSION_NUM compatibility guard in
  # reshade_overlay.hpp.
  $ImGuiTreeLine = @(
    Invoke-NativeCapture git -C $Path ls-tree $ResolvedCommit -- deps/imgui
  ) | Select-Object -First 1
  $ImGuiTreeLine = [string]$ImGuiTreeLine
  if ($ImGuiTreeLine -notmatch '^160000\s+commit\s+([0-9a-fA-F]{40})\s+deps/imgui$') {
    throw "Could not read the ReShade ImGui gitlink for $Ref"
  }
  $ExpectedImGuiCommit = $Matches[1].ToLowerInvariant()
  $ActualImGuiCommit = (Get-GitHead -Path $ImGuiRoot).ToLowerInvariant()

  if ($ActualImGuiCommit -ne $ExpectedImGuiCommit) {
    Write-Host ""
    Write-Host "===== REPAIR RESHADE API $Api IMGUI SUBMODULE ====="
    Invoke-Checked git -C $Path submodule update --init --force deps/imgui
    $ActualImGuiCommit = (Get-GitHead -Path $ImGuiRoot).ToLowerInvariant()
  }

  if ($ActualImGuiCommit -ne $ExpectedImGuiCommit) {
    throw "ReShade $Ref requires ImGui commit $ExpectedImGuiCommit, checkout contains $ActualImGuiCommit"
  }

  # Newer ReShade revisions additionally publish one or more accepted Dear
  # ImGui ABI numbers in reshade_overlay.hpp. Validate those when present, but
  # do not reject legacy revisions merely because that guard did not exist yet.
  $AllowedImGuiMatches = [regex]::Matches(
    $OverlayText,
    'IMGUI_VERSION_NUM\s*!=\s*(\d+)'
  )
  $AllowedImGui = @(
    $AllowedImGuiMatches |
      ForEach-Object { [int]$_.Groups[1].Value } |
      Sort-Object -Unique
  )

  if ($ImGuiText -notmatch '#define\s+IMGUI_VERSION_NUM\s+(\d+)') {
    throw "Could not read IMGUI_VERSION_NUM from $ImGuiHeader"
  }
  $ActualImGui = [int]$Matches[1]

  if ($AllowedImGui.Count -gt 0) {
    if ($ActualImGui -notin $AllowedImGui) {
      $AllowedImGuiText = $AllowedImGui -join ", "
      throw "ReShade $Ref accepts ImGui ABI(s) $AllowedImGuiText, checkout contains $ActualImGui"
    }
  }
  else {
    Write-Host "ReShade $Ref has no explicit ImGui ABI guard; verified pinned ImGui commit $ActualImGuiCommit (ABI $ActualImGui)"
  }

  $ResizeArg = Get-SwapchainResizeArg -ReShadeSource $Path

  # ReShade evolved several source-level helper APIs independently of the
  # numeric add-on API. Detect the selected checkout instead of maintaining a
  # second version table in MFGAmpereUnlock.
  $LogStyle = 0
  if ($HeaderText -match 'namespace\s+log\s*\{' -and
      $HeaderText -match 'enum\s+class\s+level') {
    $LogStyle = 2
  }
  elseif ($HeaderText -match 'enum\s+class\s+log_level') {
    $LogStyle = 1
  }

  $ConfigStyle = if ($HeaderText -match 'inline\s+bool\s+get_config_value\s*\(') { 1 } else { 0 }
  $HasConfigArray = if ($HeaderText -match 'ReShadeSetConfigArray' -or
      $HeaderText -match 'set_config_value\s*\([^\)]*size_t\s+value_size') { 1 } else { 0 }

  $DeviceHeader = Join-Path $Path "include\reshade_api_device.hpp"
  $HasColorSpace = 0
  if (Test-Path $DeviceHeader) {
    $DeviceHeaderText = Get-Content $DeviceHeader -Raw
    if ($DeviceHeaderText -match 'get_color_space\s*\(') {
      $HasColorSpace = 1
    }
  }

  return [PSCustomObject]@{
    Path = $Path
    Commit = $ResolvedCommit
    SwapchainResizeArg = $ResizeArg
    LogStyle = $LogStyle
    ConfigStyle = $ConfigStyle
    HasConfigArray = $HasConfigArray
    HasColorSpace = $HasColorSpace
  }
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
    $Target = Resolve-ReShadeApiRef -Api $Api
    $Ref = [string]$Target.Ref
    $ReShadeCheckout = Get-ReShadeSource -Api $Api -Ref $Ref
    $ReShadeSource = [string]$ReShadeCheckout.Path
    $ReShadeCommit = [string]$ReShadeCheckout.Commit
    $ResizeArg = [int]$ReShadeCheckout.SwapchainResizeArg
    $LogStyle = [int]$ReShadeCheckout.LogStyle
    $ConfigStyle = [int]$ReShadeCheckout.ConfigStyle
    $HasConfigArray = [int]$ReShadeCheckout.HasConfigArray
    $HasColorSpace = [int]$ReShadeCheckout.HasColorSpace
    $ReShadeInclude = Join-Path $ReShadeSource "include"
    $ImGuiInclude = Join-Path $ReShadeSource "deps\imgui"

    $ClOptions = @(
      "/I`"$NvapiRoot`"",
      "/I`"$ImGuiInclude`"",
      "/I`"$ReShadeInclude`"",
      "/DMFGUNLOCK_RESHADE_HEADER_OVERRIDE=1",
      "/DMFGUNLOCK_RESHADE_API=$Api",
      "/DMFGUNLOCK_RESHADE_SWAPCHAIN_RESIZE_ARG=$ResizeArg",
      "/DMFGUNLOCK_RESHADE_LOG_STYLE=$LogStyle",
      "/DMFGUNLOCK_RESHADE_CONFIG_STYLE=$ConfigStyle",
      "/DMFGUNLOCK_RESHADE_HAS_CONFIG_ARRAY=$HasConfigArray",
      "/DMFGUNLOCK_RESHADE_HAS_COLOR_SPACE=$HasColorSpace"
    )
    if (-not [string]::IsNullOrWhiteSpace($OriginalCl)) {
      $ClOptions += $OriginalCl
    }
    $env:CL = $ClOptions -join " "

    Write-Host ""
    Write-Host "===== BUILD MFGUNLOCK FOR RESHADE API $Api ($Ref -> $($ReShadeCommit.Substring(0, 12))) ====="
    Write-Host "ReShade compatibility: log=$LogStyle config=$ConfigStyle config_array=$HasConfigArray color_space=$HasColorSpace resize_arg=$ResizeArg"
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
      ReShadeCommit = $ReShadeCommit
      SwapchainResizeArg = $ResizeArg
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
Write-Host "Output root: $OutputRoot"

foreach ($Result in $Results) {
  Write-Host ""
  Write-Host "ReShade API $($Result.ReShadeApi)"
  Write-Host "  ReShade ref:    $($Result.ReShadeRef)"
  Write-Host "  ReShade commit: $($Result.ReShadeCommit)"
  Write-Host "  Binary:         $($Result.File)"
  Write-Host "  SHA256:         $($Result.SHA256)"
}
