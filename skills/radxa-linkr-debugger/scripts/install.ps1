param(
    [string]$Version = "latest",
    [string]$Repo = $env:REPO,
    [switch]$DryRun
)

$ErrorActionPreference = "Stop"

if ([string]::IsNullOrWhiteSpace($Version)) {
    $Version = "latest"
}
if ([string]::IsNullOrWhiteSpace($Repo)) {
    $Repo = "xzl01/radxa-linkr-debugger"
}

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = [System.IO.Path]::GetFullPath((Join-Path $scriptDir "../../.."))
$installDir = Join-Path $scriptDir "bin"
$target = Join-Path $installDir "radxa-linkr-debuggerctl.exe"
$versionExplicit = $PSBoundParameters.ContainsKey('Version')

function Test-CanBuildFromSource([string]$Root) {
    $hasCargoToml = Test-Path -LiteralPath (Join-Path $Root "cmd-ng/Cargo.toml") -PathType Leaf
    $cargo = Get-Command cargo -ErrorAction SilentlyContinue
    return ($hasCargoToml -and $null -ne $cargo)
}

function Get-AgentLinkr DebuggerArch {
    switch ($env:PROCESSOR_ARCHITECTURE) {
        "AMD64" { "amd64"; return }
        "ARM64" { "arm64"; return }
        default {
            throw "Unsupported CPU architecture: $env:PROCESSOR_ARCHITECTURE"
        }
    }
}

function Get-AgentLinkr DebuggerToken {
    if (-not [string]::IsNullOrWhiteSpace($env:GH_TOKEN)) {
        return $env:GH_TOKEN
    }
    if (-not [string]::IsNullOrWhiteSpace($env:GITHUB_TOKEN)) {
        return $env:GITHUB_TOKEN
    }
    $gh = Get-Command gh -ErrorAction SilentlyContinue
    if ($null -ne $gh) {
        try {
            return (& gh auth token 2>$null)
        } catch {
            return ""
        }
    }
    return ""
}

function Get-ReleaseAssetUrl([string]$Name) {
    if ($Version -eq "latest") {
        return "https://github.com/$Repo/releases/latest/download/$Name"
    }
    return "https://github.com/$Repo/releases/download/$Version/$Name"
}

function Get-ReleaseApiUrl {
    if ($Version -eq "latest") {
        return "https://api.github.com/repos/$Repo/releases/latest"
    }
    return "https://api.github.com/repos/$Repo/releases/tags/$Version"
}

function Invoke-AgentLinkr DebuggerDownload([string]$Url, [string]$OutFile, [string]$Token) {
    $headers = @{}
    if (-not [string]::IsNullOrWhiteSpace($Token)) {
        $headers["Authorization"] = "Bearer $Token"
        $headers["Accept"] = "application/octet-stream"
    }
    Invoke-WebRequest -Uri $Url -OutFile $OutFile -Headers $headers
}

function Invoke-AgentLinkr DebuggerReleaseAssetDownload([string]$Name, [string]$OutFile, [string]$Token) {
    if ([string]::IsNullOrWhiteSpace($Token)) {
        Invoke-AgentLinkr DebuggerDownload (Get-ReleaseAssetUrl $Name) $OutFile ""
        return
    }

    $headers = @{
        Authorization = "Bearer $Token"
        Accept = "application/vnd.github+json"
    }
    $release = Invoke-RestMethod -Uri (Get-ReleaseApiUrl) -Headers $headers
    $assetObject = $release.assets | Where-Object { $_.name -eq $Name } | Select-Object -First 1
    if ($null -eq $assetObject) {
        throw "$Name not found in GitHub release $Version"
    }

    $assetHeaders = @{
        Authorization = "Bearer $Token"
        Accept = "application/octet-stream"
    }
    Invoke-WebRequest -Uri $assetObject.url -OutFile $OutFile -Headers $assetHeaders
}

$canBuild = Test-CanBuildFromSource -Root $repoRoot
$arch = Get-AgentLinkr DebuggerArch
$asset = "radxa-linkr-debuggerctl-rust_windows_$arch.zip"
$assetUrl = Get-ReleaseAssetUrl $asset
$token = Get-AgentLinkr DebuggerToken

if ($DryRun) {
    if ($canBuild -and -not $versionExplicit) {
        Write-Host "radxa-linkr-debuggerctl skill install dry-run"
        Write-Host "mode:        build Rust cmd-ng from source"
        Write-Host "repo root:   $repoRoot"
        Write-Host "output:      $target"
    } else {
        $tokenState = if ([string]::IsNullOrWhiteSpace($token)) { "no" } else { "yes" }
        Write-Host "radxa-linkr-debuggerctl skill install dry-run"
        Write-Host "mode:        download release"
        Write-Host "repo:        $Repo"
        Write-Host "version:     $Version"
        Write-Host "platform:    windows/$arch"
        Write-Host "asset:       $asset"
        Write-Host "install dir: $installDir"
        Write-Host "auth token:  $tokenState"
        Write-Host "asset URL:   $assetUrl"
    }
    exit 0
}

New-Item -ItemType Directory -Path $installDir -Force | Out-Null

if ($canBuild -and -not $versionExplicit) {
    Write-Host "Building skill-local Rust radxa-linkr-debuggerctl at $target"
    Push-Location $repoRoot
    try {
        & cargo build --release --manifest-path cmd-ng/Cargo.toml
        if ($LASTEXITCODE -ne 0) {
            throw "cargo build failed with exit code $LASTEXITCODE"
        }
        Copy-Item -Path (Join-Path $repoRoot "cmd-ng/target/release/radxa-linkr-debuggerctl.exe") -Destination $target -Force
    } finally {
        Pop-Location
    }
    Write-Host "Installed radxa-linkr-debuggerctl to $target"
    $versionOutput = & $target --version 2>$null
    if ($LASTEXITCODE -eq 0) {
        Write-Host $versionOutput
    }
    exit 0
}

$tempRoot = Join-Path ([IO.Path]::GetTempPath()) ("radxa-linkr-debuggerctl-" + [Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $tempRoot | Out-Null

try {
    $assetPath = Join-Path $tempRoot $asset
    $sumsPath = Join-Path $tempRoot "SHA256SUMS.txt"
    $extractPath = Join-Path $tempRoot "extract"

    Write-Host "Downloading $asset"
    try {
        Invoke-AgentLinkr DebuggerReleaseAssetDownload $asset $assetPath $token
        Invoke-AgentLinkr DebuggerReleaseAssetDownload "SHA256SUMS.txt" $sumsPath $token
    } catch {
        throw "Download failed. For private repositories, set GH_TOKEN or run gh auth login. $($_.Exception.Message)"
    }

    $expected = $null
    foreach ($line in Get-Content $sumsPath) {
        $parts = $line -split "\s+"
        if ($parts.Length -ge 2 -and $parts[1] -eq $asset) {
            $expected = $parts[0]
            break
        }
    }
    if ([string]::IsNullOrWhiteSpace($expected)) {
        throw "Checksum for $asset not found in SHA256SUMS.txt"
    }

    $actual = (Get-FileHash -Algorithm SHA256 $assetPath).Hash.ToLowerInvariant()
    if ($expected.ToLowerInvariant() -ne $actual) {
        throw "Checksum mismatch for $asset. Expected $expected, got $actual"
    }

    New-Item -ItemType Directory -Path $extractPath | Out-Null
    Expand-Archive -Path $assetPath -DestinationPath $extractPath -Force
    $binary = Get-ChildItem -Path $extractPath -Recurse -Filter "radxa-linkr-debuggerctl.exe" | Select-Object -First 1
    if ($null -eq $binary) {
        throw "radxa-linkr-debuggerctl.exe not found in archive"
    }

    Copy-Item -Path $binary.FullName -Destination $target -Force
    Write-Host "Installed radxa-linkr-debuggerctl to $target"
    $versionOutput = & $target --version 2>$null
    if ($LASTEXITCODE -eq 0) {
        Write-Host $versionOutput
    }
} finally {
    Remove-Item -Recurse -Force $tempRoot -ErrorAction SilentlyContinue
}
