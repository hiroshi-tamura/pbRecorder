param(
    [Parameter(Mandatory = $true)]
    [string] $ExePath,

    [Parameter(Mandatory = $true)]
    [string] $OutputPath,

    [int] $TimeoutSeconds = 20
)

$ErrorActionPreference = 'Stop'

$exe = (Resolve-Path -LiteralPath $ExePath).Path
$appDir = Split-Path -Parent $exe
$outputFullPath = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($OutputPath)
$outputDir = Split-Path -Parent $outputFullPath
if ($outputDir) {
    New-Item -ItemType Directory -Force -Path $outputDir | Out-Null
}

if (Test-Path -LiteralPath $outputFullPath) {
    Remove-Item -LiteralPath $outputFullPath -Force
}

$oldPath = $env:PATH
try {
    # Keep only Windows system paths. This catches missing Qt/MinGW DLLs that
    # would otherwise be hidden by the developer machine's PATH.
    $env:PATH = 'C:\Windows\System32;C:\Windows;C:\Windows\System32\Wbem'

    $process = Start-Process `
        -FilePath $exe `
        -WorkingDirectory $appDir `
        -ArgumentList @('--ui-screenshot', $outputFullPath) `
        -PassThru

    Wait-Process -Id $process.Id -Timeout $TimeoutSeconds -ErrorAction SilentlyContinue
    $process.Refresh()
    if (-not $process.HasExited) {
        Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
        throw "Runtime launch timed out after $TimeoutSeconds seconds. A missing-DLL dialog may be blocking startup."
    }

    if ($process.ExitCode -ne 0) {
        throw "Runtime launch exited with code $($process.ExitCode)."
    }

    if (-not (Test-Path -LiteralPath $outputFullPath)) {
        throw "Runtime launch did not create screenshot: $outputFullPath"
    }

    $file = Get-Item -LiteralPath $outputFullPath
    if ($file.Length -lt 1024) {
        throw "Runtime launch screenshot is unexpectedly small: $($file.Length) bytes."
    }

    Write-Host "Runtime launch OK: $($file.FullName) ($($file.Length) bytes)"
} finally {
    $env:PATH = $oldPath
    if ($process -and -not $process.HasExited) {
        Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
    }
}
