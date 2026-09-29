# Builds CapView, checks the exe is safe to hand to other people, and zips it into dist\.
#   powershell -ExecutionPolicy Bypass -File package.ps1
# -Ci skips the launch test (build servers have no display, GPU or capture card).
param([switch]$Ci)
$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot
$failed = $false
function Check($ok, $what) {
    if ($ok) { Write-Host "  [ok]   $what" -ForegroundColor Green }
    else { Write-Host "  [FAIL] $what" -ForegroundColor Red; $script:failed = $true }
}

Write-Host "Building..."
cmd /c "`"$PSScriptRoot\build.bat`"" | Out-Null
if ($LASTEXITCODE -ne 0) { throw "build failed (run build.bat to see errors)" }
$exe = Get-Item "build\CapView.exe"
$version = $exe.VersionInfo.ProductVersion

Write-Host "Checking build\CapView.exe (v$version)..."

# 1. Only depends on DLLs that ship with Windows (no VC++ redistributable etc.).
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$dumpbin = Get-ChildItem "$vs\VC\Tools\MSVC\*\bin\Hostx64\x64\dumpbin.exe" | Select-Object -First 1
$deps = & $dumpbin.FullName /nologo /dependents $exe.FullName | Where-Object { $_ -match '^\s+\S+\.dll\s*$' } | ForEach-Object { $_.Trim() }
foreach ($dll in $deps) {
    $system = Test-Path "$env:WINDIR\System32\$dll"
    $redist = $dll -match '^(vcruntime|msvcp|concrt|vccorlib|api-ms-win-crt)'
    Check ($system -and -not $redist) "depends on $dll (built into Windows)"
}

# 2. 64-bit, with version info (unsigned exes without it look more suspicious to antivirus).
$headers = & $dumpbin.FullName /nologo /headers $exe.FullName
Check ($headers -match 'machine \(x64\)') "64-bit executable"
Check ([bool]$version) "has version info"

# 3. Starts, stays up, and closes cleanly.
if ($Ci) {
    Write-Host "  [skip] smoke test (CI)" -ForegroundColor Yellow
} elseif (Get-Process CapView -ErrorAction SilentlyContinue) {
    Write-Host "  [skip] smoke test: close the running CapView first" -ForegroundColor Yellow
} else {
    $p = Start-Process $exe.FullName -PassThru
    Start-Sleep 4
    $p.Refresh()
    Check (-not $p.HasExited -and $p.Responding) "launches and stays responsive"
    if (-not $p.HasExited) {
        $p.CloseMainWindow() | Out-Null
        $closed = $p.WaitForExit(5000)
        Check $closed "closes within 5 s"
        if (-not $closed) { $p | Stop-Process -Force }
    }
    $log = Get-Content "$env:LOCALAPPDATA\CapView\capview.log" -Raw
    Check ($log -match 'render: GPU') "Direct3D started (see log)"
}

if ($failed) { throw "Some checks failed - not packaging." }

# 4. Zip it with a short readme for friends.
New-Item -ItemType Directory -Force dist | Out-Null
$stage = "build\package"
Remove-Item $stage -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory $stage | Out-Null
Copy-Item $exe.FullName $stage
Copy-Item dist-readme.txt "$stage\README.txt"
$zip = "dist\CapView-$version.zip"
Compress-Archive -Path "$stage\*" -DestinationPath $zip -Force
$hash = (Get-FileHash $zip -Algorithm SHA256).Hash
Write-Host ""
Write-Host "Packaged $zip  ($([int]((Get-Item $zip).Length / 1KB)) KB)" -ForegroundColor Cyan
Write-Host "SHA-256: $hash"
