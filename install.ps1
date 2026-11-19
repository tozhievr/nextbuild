[CmdletBinding()]
param(
    [string]$Version = 'latest',
    [string]$BinDir = (Join-Path $env:LOCALAPPDATA 'Nextbuild\bin'),
    [switch]$NoPath
)

$ErrorActionPreference = 'Stop'
$repo = 'https://github.com/tozhievr/nextbuild'
$arch = if ($env:PROCESSOR_ARCHITEW6432) { $env:PROCESSOR_ARCHITEW6432 } else { $env:PROCESSOR_ARCHITECTURE }
if ($arch -ne 'AMD64') { throw "No release binary for Windows $arch. See the source build instructions." }
if ($PSVersionTable.PSEdition -eq 'Desktop') {
    [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
}

if ($Version -eq 'latest') {
    $release = Invoke-RestMethod -Uri 'https://api.github.com/repos/tozhievr/nextbuild/releases/latest' -Headers @{'User-Agent' = 'nextbuild-installer'}
    $Version = $release.tag_name
}
$Version = $Version -creplace '^v', ''
if ($Version -cnotmatch '\A(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\z') {
    throw 'Expected a release version such as 0.2.1.'
}

$asset = "nextbuild-$Version-Windows-X64.tar.gz"
$base = "$repo/releases/download/v$Version"
$work = Join-Path ([IO.Path]::GetTempPath()) ('nextbuild-' + [guid]::NewGuid())
$stage = $null
New-Item -ItemType Directory -Path $work | Out-Null
try {
    $archive = Join-Path $work $asset
    $sums = Join-Path $work 'SHA256SUMS'
    Invoke-WebRequest -UseBasicParsing -Uri "$base/$asset" -OutFile $archive
    Invoke-WebRequest -UseBasicParsing -Uri "$base/SHA256SUMS" -OutFile $sums
    $pattern = '^([0-9a-fA-F]{64})\s+\*?(?:\./)?' + [regex]::Escape($asset) + '$'
    $expected = @(Get-Content -LiteralPath $sums | ForEach-Object {
        if ($_ -cmatch $pattern) { $Matches[1] }
    })
    if ($expected.Count -ne 1 -or (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash -ne $expected[0]) {
        throw "SHA-256 mismatch for $asset."
    }
    & tar.exe -xzf $archive -C $work nextbuild.exe Parsley-LICENSE
    if ($LASTEXITCODE -ne 0) { throw 'Failed to unpack the release archive.' }

    $BinDir = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($BinDir)
    New-Item -ItemType Directory -Path $BinDir -Force | Out-Null
    $destination = Join-Path $BinDir 'nextbuild.exe'
    $stage = Join-Path $BinDir ('.nextbuild-' + [guid]::NewGuid() + '.tmp')
    Copy-Item -LiteralPath (Join-Path $work 'nextbuild.exe') -Destination $stage
    Copy-Item -LiteralPath (Join-Path $work 'Parsley-LICENSE') -Destination (Join-Path $BinDir 'nextbuild-Parsley-LICENSE') -Force
    if ([IO.File]::Exists($destination)) {
        $backup = "$stage.previous"
        [IO.File]::Move($destination, $backup)
        try {
            [IO.File]::Move($stage, $destination)
        } catch {
            [IO.File]::Move($backup, $destination)
            throw
        }
        Remove-Item -LiteralPath $backup -Force
    } else {
        [IO.File]::Move($stage, $destination)
    }

    if (-not $NoPath) {
        $previous = [Environment]::GetEnvironmentVariable('Path', 'User')
        $paths = @($previous -split ';' | Where-Object { $_ })
        if ($paths -notcontains $BinDir) {
            [Environment]::SetEnvironmentVariable('Path', (($paths + $BinDir) -join ';'), 'User')
        }
        if (($env:Path -split ';') -notcontains $BinDir) { $env:Path += ";$BinDir" }
    }
    Write-Host "Installed nextbuild $Version to $destination"
} finally {
    if ($stage -and (Test-Path -LiteralPath $stage)) { Remove-Item -LiteralPath $stage -Force }
    foreach ($file in @($asset, 'SHA256SUMS', 'nextbuild.exe', 'Parsley-LICENSE')) {
        $item = Join-Path $work $file
        if (Test-Path -LiteralPath $item) { Remove-Item -LiteralPath $item -Force }
    }
    Remove-Item -LiteralPath $work -Force
}
