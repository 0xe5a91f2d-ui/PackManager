param(
    [string]$Version = "0.1.0"
)

$ErrorActionPreference = "Stop"
$distribution = Join-Path $PSScriptRoot "dist"
if (-not (Test-Path (Join-Path $distribution "PackManager.exe"))) {
    throw "Build PackManager first; dist\PackManager.exe was not found."
}

$archive = Join-Path $PSScriptRoot "PackManager-portable-$Version.zip"
Compress-Archive -Path (Join-Path $distribution "*") -DestinationPath $archive -Force
Write-Host "Portable archive created: $archive"
