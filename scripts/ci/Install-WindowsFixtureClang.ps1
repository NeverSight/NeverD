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
    if ($AddToPath) {
        "$destination\bin" >> $env:GITHUB_PATH
    }
    "NEVERD_SOURCE_CLANG=$destination\bin\clang.exe" >> $env:GITHUB_ENV
} finally {
    if (Test-Path -LiteralPath $installer) {
        Remove-Item -LiteralPath $installer
    }
}
