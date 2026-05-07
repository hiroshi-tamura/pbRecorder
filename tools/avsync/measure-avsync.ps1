param(
    [Parameter(Mandatory = $true)]
    [string] $InputPath,

    [string] $FfmpegPath = "",

    [string] $WorkDir = "",

    [switch] $Json
)

$ErrorActionPreference = 'Stop'

if (-not $FfmpegPath) {
    $cmd = Get-Command ffmpeg -ErrorAction SilentlyContinue
    if (-not $cmd) {
        throw "ffmpeg was not found on PATH. Pass -FfmpegPath to use a specific ffmpeg.exe."
    }
    $FfmpegPath = $cmd.Source
}

$inputFull = (Resolve-Path -LiteralPath $InputPath).Path
if (-not $WorkDir) {
    $WorkDir = Split-Path -Parent $inputFull
}
New-Item -ItemType Directory -Force -Path $WorkDir | Out-Null

$tag = [IO.Path]::GetFileNameWithoutExtension($inputFull)
$videoRaw = Join-Path $WorkDir "$tag.video.gray"
$audioRaw = Join-Path $WorkDir "$tag.audio.s16le"

& $FfmpegPath -hide_banner -y -v error -i $inputFull `
    -map 0:v:0 -vf "fps=60,scale=1:1,format=gray" -f rawvideo $videoRaw
& $FfmpegPath -hide_banner -y -v error -i $inputFull `
    -map 0:a:0 -ac 1 -ar 48000 -f s16le $audioRaw

$video = [IO.File]::ReadAllBytes((Resolve-Path -LiteralPath $videoRaw).Path)
$flashFrames = New-Object System.Collections.Generic.List[int]
for ($i = 0; $i -lt $video.Length; $i++) {
    if ($video[$i] -gt 180) {
        $flashFrames.Add($i)
    }
}

$flashEvents = @()
$start = $null
$end = $null
foreach ($frame in $flashFrames) {
    if ($null -eq $start -or $frame -gt ($end + 1)) {
        if ($null -ne $start) {
            $flashEvents += [pscustomobject]@{
                start = [math]::Round($start / 60.0, 4)
                end = [math]::Round($end / 60.0, 4)
            }
        }
        $start = $frame
        $end = $frame
    } else {
        $end = $frame
    }
}
if ($null -ne $start) {
    $flashEvents += [pscustomobject]@{
        start = [math]::Round($start / 60.0, 4)
        end = [math]::Round($end / 60.0, 4)
    }
}

$audio = [IO.File]::ReadAllBytes((Resolve-Path -LiteralPath $audioRaw).Path)
$peakTimes = New-Object System.Collections.Generic.List[double]
$window = 240 # 5 ms
for ($pos = 0; $pos + 1 -lt $audio.Length; $pos += 2 * $window) {
    $max = 0
    for ($j = 0; $j -lt $window -and ($pos + 2 * $j + 1) -lt $audio.Length; $j++) {
        $sample = [BitConverter]::ToInt16($audio, $pos + 2 * $j)
        $abs = [math]::Abs([int]$sample)
        if ($abs -gt $max) {
            $max = $abs
        }
    }
    if ($max -gt 5000) {
        $peakTimes.Add([math]::Round(($pos / 2) / 48000.0, 4))
    }
}

$audioEvents = @()
$start = $null
$end = $null
foreach ($time in $peakTimes) {
    if ($null -eq $start -or $time -gt ($end + 0.02)) {
        if ($null -ne $start) {
            $audioEvents += [pscustomobject]@{ start = $start; end = $end }
        }
        $start = $time
        $end = $time
    } else {
        $end = $time
    }
}
if ($null -ne $start) {
    $audioEvents += [pscustomobject]@{ start = $start; end = $end }
}

$pairs = @()
foreach ($flash in $flashEvents) {
    $nearest = $audioEvents |
        Sort-Object @{ Expression = { [math]::Abs($_.start - $flash.start) } } |
        Select-Object -First 1
    if ($nearest) {
        $pairs += [pscustomobject]@{
            video = $flash.start
            audio = $nearest.start
            deltaMs = [math]::Round(($nearest.start - $flash.start) * 1000.0, 1)
        }
    }
}

$result = [pscustomobject]@{
    input = $inputFull
    flash = $flashEvents
    audio = $audioEvents
    pairs = $pairs
}

if ($Json) {
    $result | ConvertTo-Json -Depth 5
} else {
    $result
}
