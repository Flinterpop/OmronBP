# Release build for OmronBP: verifies the version is in lockstep across every file that
# carries it, runs the Python and C++ test suites, builds the static Win32 exe, checks it
# has no VC++ runtime dependency, and zips the artifact into dist\.
#
# Usage:  .\release.ps1            (from the repo root; Windows PowerShell 5.1 or later)
# It never commits, tags or uploads anything.

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
Set-Location $root

# --- 1. version lockstep -----------------------------------------------------------------
$version = (Select-String -Path 'pyproject.toml' -Pattern '^version = "([0-9.]+)"').Matches[0].Groups[1].Value
if (-not $version) { throw 'pyproject.toml has no version line' }
$tuple = ($version -split '\.') -join ','
$checks = @(
    @{ File = 'omron_bp\__init__.py';    Pattern = "__version__ = `"$version`"" },
    @{ File = 'win32\CMakeLists.txt';    Pattern = "project\(OmronBP VERSION $([regex]::Escape($version)) " },
    @{ File = 'win32\src\app.rc';        Pattern = "FILEVERSION $tuple,0" },
    @{ File = 'win32\src\app.rc';        Pattern = "PRODUCTVERSION $tuple,0" },
    @{ File = 'win32\src\app.rc';        Pattern = "`"FileVersion`", `"$([regex]::Escape($version)).0`"" },
    @{ File = 'win32\src\app.rc';        Pattern = "`"ProductVersion`", `"$([regex]::Escape($version)).0`"" },
    @{ File = 'win32\src\app.manifest';  Pattern = "version=`"$([regex]::Escape($version)).0`"" },
    @{ File = 'README.md';               Pattern = "Version $([regex]::Escape($version))" }
)
$bad = @()
foreach ($c in $checks) {
    if (-not (Select-String -Path $c.File -Pattern $c.Pattern -Quiet)) { $bad += "$($c.File): expected /$($c.Pattern)/" }
}
if ($bad.Count -gt 0) { $bad | ForEach-Object { Write-Host "VERSION MISMATCH  $_" -ForegroundColor Red }; throw "version $version is not in lockstep" }
Write-Host "Version $version is in lockstep across all files." -ForegroundColor Green

# --- 2. Python checks ----------------------------------------------------------------------
python -m ruff check .;              if ($LASTEXITCODE -ne 0) { throw 'ruff failed' }
python -m mypy omron_bp tests win32/tools; if ($LASTEXITCODE -ne 0) { throw 'mypy failed' }
python -m pytest -q;                 if ($LASTEXITCODE -ne 0) { throw 'pytest failed' }

# --- 3. C++ build and tests ----------------------------------------------------------------
cmake -S win32 -B win32\build -A x64 -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded
if ($LASTEXITCODE -ne 0) { throw 'cmake configure failed' }
cmake --build win32\build --config Release
if ($LASTEXITCODE -ne 0) { throw 'cmake build failed' }
& win32\build\Release\core_tests.exe
if ($LASTEXITCODE -ne 0) { throw 'core_tests failed' }

# --- 4. static exe check -------------------------------------------------------------------
$exe = Join-Path $root 'win32\build\Release\OmronBP.exe'
$dumpbin = Get-ChildItem 'C:\Program Files\Microsoft Visual Studio\18\Community\VC\Tools\MSVC\*\bin\Hostx64\x64\dumpbin.exe' | Select-Object -Last 1
$deps = & $dumpbin.FullName /dependents $exe | Select-String -Pattern '\.dll' | ForEach-Object { $_.Line.Trim() }
$runtime = $deps | Where-Object { $_ -match '^(VCRUNTIME|MSVCP|api-ms-win-crt)' }
if ($runtime) { throw "exe depends on the VC++ runtime: $($runtime -join ', ')" }
$info = (Get-Item $exe).VersionInfo
if ($info.FileVersion -ne "$version.0") { throw "exe reports FileVersion $($info.FileVersion), expected $version.0" }
Write-Host "OmronBP.exe is static and reports $($info.FileVersion)." -ForegroundColor Green

# --- 5. artifact ---------------------------------------------------------------------------
$dist = Join-Path $root 'dist'
New-Item -ItemType Directory -Force $dist | Out-Null
$stage = Join-Path $dist "OmronBP-v$version-win64"
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force $stage | Out-Null
Copy-Item $exe $stage
Copy-Item (Join-Path $root 'README.md') $stage
Copy-Item (Join-Path $root 'LICENSE') $stage
$zip = "$stage.zip"
if (Test-Path $zip) { Remove-Item -Force $zip }
Compress-Archive -Path "$stage\*" -DestinationPath $zip
Write-Host "Artifact contents:" -ForegroundColor Cyan
Get-ChildItem $stage | ForEach-Object { "  $($_.Name)  $($_.Length) bytes" }
Write-Host "Wrote $zip" -ForegroundColor Green
