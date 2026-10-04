param(
    [Parameter(Mandatory=$true)][string]$ObsSdk,
    [Parameter(Mandatory=$true)][string]$QtPrefix,
    [string]$BuildDirectory = "build",
    [string]$StageDirectory = "stage"
)
$ErrorActionPreference = "Stop"
$ProjectDirectory = Split-Path -Parent $PSScriptRoot
Set-Location $ProjectDirectory
function CheckExit { if ($LASTEXITCODE -ne 0) { throw "Build command failed ($LASTEXITCODE)" } }
cmake -S . -B $BuildDirectory -G "Visual Studio 17 2022" -A x64 "-DCMAKE_PREFIX_PATH=$ObsSdk;$QtPrefix" -DBUILD_TESTING=ON
CheckExit
cmake --build $BuildDirectory --config RelWithDebInfo
CheckExit
ctest --test-dir $BuildDirectory -C RelWithDebInfo --output-on-failure
CheckExit
cmake --install $BuildDirectory --config RelWithDebInfo --prefix $StageDirectory
CheckExit
Write-Output "Built and staged in $StageDirectory. Copy the two staged folders into OBS with OBS closed."
