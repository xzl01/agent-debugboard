# SPDX-License-Identifier: LGPL-3.0-or-later

param(
    [string]$Prefix = "",
    [switch]$NoStart,
    [switch]$NoAutostart,
    [switch]$DryRun
)

$ErrorActionPreference = "Stop"
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$sourceCheckout = [IO.Path]::GetFullPath((Join-Path $scriptDir ".."))
if (Test-Path -LiteralPath (Join-Path $sourceCheckout "host-tools\Cargo.toml") -PathType Leaf) {
    $repoRoot = $sourceCheckout
} else {
    $repoRoot = $scriptDir
}
if ([string]::IsNullOrWhiteSpace($Prefix)) {
    $Prefix = Join-Path $env:LOCALAPPDATA "Radxa\LinkrDebugger"
}
$binDir = Join-Path $Prefix "bin"
$webDir = Join-Path $Prefix "share\radxa-linkr-debugger\web"
$startupDir = [Environment]::GetFolderPath([Environment+SpecialFolder]::Startup)
$startupScript = Join-Path $startupDir "RadxaLinkrDebugger.cmd"
$trayDataDir = Join-Path $env:LOCALAPPDATA "Radxa Linkr Debugger"
$trayLock = Join-Path $trayDataDir "tray.lock"
$trayShutdownRequest = Join-Path $trayDataDir "shutdown.request"

if ($DryRun) {
    Write-Host "Radxa Linkr unified installer dry-run"
    Write-Host "source:       $repoRoot"
    Write-Host "install root: $Prefix"
    Write-Host "tray:         $(Join-Path $binDir 'linkr-tray.exe')"
    Write-Host "host:         $(Join-Path $binDir 'linkr-host.exe')"
    Write-Host "CLI:          $(Join-Path $binDir 'radxa-linkr-debuggerctl.exe')"
    Write-Host "Web assets:   $webDir"
    Write-Host "MCP endpoint: http://127.0.0.1:8787/mcp"
    Write-Host "UART archive: enabled by tray (64 MiB segments, 2 GiB quota, 30 days)"
    Write-Host "autostart:    $(if ($NoAutostart) { 'disabled' } else { $startupScript })"
    Write-Host "start now:    $(if ($NoStart) { 'no' } else { 'yes' })"
    exit 0
}

$sourceManifest = Join-Path $repoRoot "host-tools\Cargo.toml"
if (Test-Path -LiteralPath $sourceManifest -PathType Leaf) {
    if ($null -eq (Get-Command npm -ErrorAction SilentlyContinue)) { throw "npm is required" }
    if ($null -eq (Get-Command cargo -ErrorAction SilentlyContinue)) { throw "cargo is required" }

    Write-Host "Building Web UI"
    & npm --prefix (Join-Path $repoRoot "web") ci
    if ($LASTEXITCODE -ne 0) { throw "npm ci failed with exit code $LASTEXITCODE" }
    & npm --prefix (Join-Path $repoRoot "web") run build
    if ($LASTEXITCODE -ne 0) { throw "Web build failed with exit code $LASTEXITCODE" }

    Write-Host "Building Linkr Host, tray and CLI"
    & cargo build --locked --release --manifest-path $sourceManifest
    if ($LASTEXITCODE -ne 0) { throw "Host build failed with exit code $LASTEXITCODE" }
    & cargo build --locked --release --manifest-path (Join-Path $repoRoot "cmd-ng\Cargo.toml")
    if ($LASTEXITCODE -ne 0) { throw "CLI build failed with exit code $LASTEXITCODE" }
    $sourceBin = Join-Path $repoRoot "host-tools\target\release"
    $sourceCli = Join-Path $repoRoot "cmd-ng\target\release\radxa-linkr-debuggerctl.exe"
    $sourceWeb = Join-Path $repoRoot "web\dist"
} elseif ((Test-Path (Join-Path $repoRoot "bin\linkr-host.exe")) -and (Test-Path (Join-Path $repoRoot "bin\linkr-tray.exe"))) {
    $sourceBin = Join-Path $repoRoot "bin"
    $sourceCli = Join-Path $sourceBin "radxa-linkr-debuggerctl.exe"
    $sourceWeb = Join-Path $repoRoot "share\radxa-linkr-debugger\web"
} else {
    throw "Installer is neither inside a source checkout nor a desktop release bundle"
}

function Stop-ExistingLinkrTray {
    if (-not (Test-Path -LiteralPath $trayLock -PathType Leaf)) { return }
    $lockLine = Get-Content -LiteralPath $trayLock -TotalCount 1 -ErrorAction SilentlyContinue
    if ([string]::IsNullOrWhiteSpace($lockLine)) { return }
    $trayProcessId = 0
    $pidText = ($lockLine -split '\s+')[0]
    if (-not [int]::TryParse($pidText, [ref]$trayProcessId)) { return }
    $trayProcess = Get-Process -Id $trayProcessId -ErrorAction SilentlyContinue
    if ($null -eq $trayProcess) { return }
    if ($trayProcess.ProcessName -ne "linkr-tray") {
        throw "Refusing to stop PID $trayProcessId from stale tray lock ($($trayProcess.ProcessName))"
    }

    Write-Host "Stopping the existing Radxa Linkr tray"
    try {
        Set-Content -LiteralPath $trayShutdownRequest -Value "installer" -Encoding ASCII
        if (-not $trayProcess.WaitForExit(12000)) {
            throw "Existing tray did not stop cleanly; installation aborted"
        }
    } finally {
        Remove-Item -LiteralPath $trayShutdownRequest -Force -ErrorAction SilentlyContinue
    }
}

Stop-ExistingLinkrTray

New-Item -ItemType Directory -Path $binDir -Force | Out-Null
New-Item -ItemType Directory -Path $webDir -Force | Out-Null
$installedHost = Join-Path $binDir "linkr-host.exe"
$runningInstalledHosts = @(Get-Process -Name "linkr-host" -ErrorAction SilentlyContinue | Where-Object {
    try { $_.Path -eq $installedHost } catch { $false }
})
if ($runningInstalledHosts.Count -gt 0) {
    throw "The installed linkr-host is still running after tray shutdown; installation aborted"
}
Copy-Item (Join-Path $sourceBin "linkr-host.exe") $binDir -Force
Copy-Item (Join-Path $sourceBin "linkr-tray.exe") $binDir -Force
Copy-Item $sourceCli $binDir -Force
Copy-Item (Join-Path $sourceWeb "*") $webDir -Recurse -Force

$mcpInfo = [ordered]@{
    name = "radxa-linkr-debugger"
    transport = "streamable-http"
    url = "http://127.0.0.1:8787/mcp"
    stdio_compatibility = [ordered]@{
        command = (Join-Path $binDir "linkr-host.exe")
        args = @("mcp", "--no-autostart")
    }
}
$mcpInfo | ConvertTo-Json -Depth 4 | Set-Content (Join-Path $Prefix "mcp-endpoint.json") -Encoding UTF8

if (-not $NoAutostart) {
    New-Item -ItemType Directory -Path $startupDir -Force | Out-Null
    $trayPath = Join-Path $binDir "linkr-tray.exe"
    "@start `"`" /B `"$trayPath`"" | Set-Content $startupScript -Encoding ASCII
}

if (-not $NoStart) {
    Start-Process -FilePath (Join-Path $binDir "linkr-tray.exe")
}

Write-Host "Installed Radxa Linkr desktop stack to $Prefix"
Write-Host "Web console: http://127.0.0.1:8787/"
Write-Host "MCP endpoint: http://127.0.0.1:8787/mcp"
Write-Host "UART archive: enabled by tray; manage it from the Web serial console"
Write-Host "CLI: $(Join-Path $binDir 'radxa-linkr-debuggerctl.exe')"
