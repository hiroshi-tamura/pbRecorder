param(
    [Parameter(Mandatory = $true)]
    [string] $ExePath,

    [Parameter(Mandatory = $true)]
    [string] $OutputPath,

    [int] $TimeoutSeconds = 20
)

$ErrorActionPreference = 'Stop'

$exe = (Resolve-Path -LiteralPath $ExePath).Path
$outputDir = Split-Path -Parent $OutputPath
if ($outputDir) {
    New-Item -ItemType Directory -Force -Path $outputDir | Out-Null
}

if (Test-Path -LiteralPath $OutputPath) {
    Remove-Item -LiteralPath $OutputPath -Force
}

$process = Start-Process -FilePath $exe -ArgumentList @('--ui-screenshot', $OutputPath) -PassThru
try {
    Wait-Process -Id $process.Id -Timeout $TimeoutSeconds -ErrorAction SilentlyContinue
    $process.Refresh()
    if (-not $process.HasExited) {
        Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
        throw "UI screenshot test timed out after $TimeoutSeconds seconds."
    }

    if ($process.ExitCode -ne 0) {
        throw "UI screenshot process exited with code $($process.ExitCode)."
    }

    if (-not (Test-Path -LiteralPath $OutputPath)) {
        throw "UI screenshot was not created: $OutputPath"
    }

    $file = Get-Item -LiteralPath $OutputPath
    if ($file.Length -lt 1024) {
        throw "UI screenshot is unexpectedly small: $($file.Length) bytes."
    }

    Write-Host "Created UI screenshot: $($file.FullName) ($($file.Length) bytes)"
} finally {
    if ($process -and -not $process.HasExited) {
        Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
    }
}
