param([switch]$AddToPath)

$ErrorActionPreference = 'Stop'
$package = Get-Content -Raw (Join-Path $PSScriptRoot 'WindowsFixtureClang.def') | ConvertFrom-Json
$installer = Join-Path $env:RUNNER_TEMP $package.asset
$destination = Join-Path $env:RUNNER_TEMP $package.destination
try {
    Invoke-WebRequest $package.url -OutFile $installer
    if ((Get-FileHash $installer -Algorithm SHA256).Hash -ne $package.sha256) {
        throw $package.digest_error
    }
    $process = Start-Process -FilePath $installer -ArgumentList @('/S', "/D=$destination") -Wait -PassThru
    if ($process.ExitCode -ne 0) {
        throw "$($package.install_error) ($($process.ExitCode))"
    }
    # The MSVC toolchain also supplies an ASan DLL with this name. Fixtures
    # compiled by this Clang must load its matching runtime, before MSVC's.
    $clang = Join-Path $destination 'bin/clang.exe'
    $runtime = (& $clang "-print-file-name=$($package.asan_runtime)").Trim()
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $runtime -PathType Leaf)) {
        throw $package.runtime_error
    }
    (Split-Path -Parent $runtime) >> $env:GITHUB_PATH
    if ($AddToPath) {
        "$destination\bin" >> $env:GITHUB_PATH
    }
    "NEVERD_SOURCE_CLANG=$destination\bin\clang.exe" >> $env:GITHUB_ENV
} finally {
    if (Test-Path -LiteralPath $installer) {
        Remove-Item -LiteralPath $installer
    }
}
