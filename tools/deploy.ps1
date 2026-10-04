# Copies a packaged plugin folder into an OBS install, or restores the known-good binary.
# It auto-elevates via UAC and refuses to run while OBS is open.
#
#   .\tools\deploy.ps1 -From stage        # install the current build (with meme morphs)
#   .\tools\deploy.ps1 -From known-good   # roll back to the known-good binary
param(
    [Parameter(Mandatory = $true)][ValidateSet('stage', 'known-good')][string]$From,
    [string]$Obs = "C:\Program Files\obs-studio"
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$src = Join-Path $root $From

# Re-launch elevated if we are not administrator (one UAC prompt).
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
    Write-Host "Requesting administrator rights (UAC prompt)..."
    Start-Process -FilePath "powershell.exe" -Verb RunAs -ArgumentList @(
        "-NoProfile", "-ExecutionPolicy", "Bypass", "-NoExit",
        "-File", "`"$PSCommandPath`"", "-From", $From, "-Obs", "`"$Obs`""
    )
    return
}

if (Get-Process obs64 -ErrorAction SilentlyContinue) {
    throw "OBS is running. Close OBS completely, then run this again."
}
if (-not (Test-Path $src)) { throw "missing source folder: $src" }
if (-not (Test-Path $Obs)) { throw "OBS not found: $Obs" }

$dll = Join-Path $src 'obs-plugins\64bit\obs-parameter-animator.dll'
if (-not (Test-Path $dll)) { throw "missing DLL: $dll" }

Copy-Item $dll (Join-Path $Obs 'obs-plugins\64bit\') -Force
Copy-Item (Join-Path $src 'data\obs-plugins\obs-parameter-animator\*') `
          (Join-Path $Obs 'data\obs-plugins\obs-parameter-animator\') -Force
Write-Host "Deployed '$From' into $Obs"
Write-Host "SHA256: $((Get-FileHash $dll -Algorithm SHA256).Hash)"
