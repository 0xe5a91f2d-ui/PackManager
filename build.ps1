$ErrorActionPreference = "Stop"

$qtPrefix = "C:\Qt\6.7.3\msvc2019_64"
$visualStudioRoot = "C:\Program Files\Microsoft Visual Studio\2022\Community"
$vcVars = Join-Path $visualStudioRoot "VC\Auxiliary\Build\vcvars64.bat"
$cmakeBuild = Join-Path $PSScriptRoot "build"
$outputDirectory = Join-Path $PSScriptRoot "dist"

if (-not (Test-Path (Join-Path $qtPrefix "lib\cmake\Qt6\Qt6Config.cmake"))) {
    throw "Qt 6 Widgets was not found at $qtPrefix. Update qtPrefix in build.ps1."
}
if (-not (Test-Path $vcVars)) {
    throw "Visual Studio C++ build tools were not found at $vcVars."
}

& $env:ComSpec /c "call `"$vcVars`" >nul && cmake -S `"$PSScriptRoot`" -B `"$cmakeBuild`" -A x64 -DCMAKE_PREFIX_PATH=`"$qtPrefix`""
if ($LASTEXITCODE -ne 0) { throw "CMake configuration failed." }

& $env:ComSpec /c "call `"$vcVars`" >nul && cmake --build `"$cmakeBuild`" --config Release"
if ($LASTEXITCODE -ne 0) { throw "C++ build failed." }

$windeployqt = Join-Path $qtPrefix "bin\windeployqt.exe"
$builtExecutable = Join-Path $cmakeBuild "dist\PackManager.exe"
New-Item -ItemType Directory -Force -Path $outputDirectory | Out-Null
Copy-Item -LiteralPath $builtExecutable `
    -Destination (Join-Path $outputDirectory "PackManager.exe") -Force
Copy-Item -LiteralPath (Join-Path $PSScriptRoot "src\packmanager.ico") `
    -Destination (Join-Path $outputDirectory "packmanager.ico") -Force
& $windeployqt --release --dir $outputDirectory (Join-Path $outputDirectory "PackManager.exe")
if ($LASTEXITCODE -ne 0) { throw "Qt runtime deployment failed." }

Write-Host "PackManager.exe and required Qt runtime files are in $outputDirectory"

$innoCompiler = Get-Command ISCC.exe -ErrorAction SilentlyContinue
if (-not $innoCompiler) {
    $defaultInnoCompiler = Join-Path ${env:ProgramFiles(x86)} "Inno Setup 6\ISCC.exe"
    if (Test-Path $defaultInnoCompiler) {
        $innoCompilerPath = $defaultInnoCompiler
    }
} else {
    $innoCompilerPath = $innoCompiler.Source
}

if ($innoCompilerPath) {
    & $innoCompilerPath (Join-Path $PSScriptRoot "installer\PackManager.iss")
    if ($LASTEXITCODE -ne 0) { throw "Inno Setup installer build failed." }
    Write-Host "PackManager installer was created in $outputDirectory"
} else {
    Write-Warning "Inno Setup was not found; the portable application was built, but no installer was created."
}
