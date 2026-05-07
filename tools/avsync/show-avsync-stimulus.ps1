param(
    [int] $InitialDelayMs = 1500,
    [int] $DurationMs = 7500,
    [int] $FlashMs = 100,
    [int[]] $EventOffsetsMs = @(1000, 2500, 4000, 5500),
    [int] $ToneHz = 1000,
    [int] $SampleRate = 48000
)

$ErrorActionPreference = 'Stop'

Add-Type -AssemblyName PresentationCore, PresentationFramework, WindowsBase

function New-ToneBytes {
    param(
        [int] $Frequency,
        [int] $DurationMs,
        [int] $SampleRate
    )

    $samples = [int]($SampleRate * $DurationMs / 1000)
    $dataBytes = $samples * 2
    $stream = New-Object IO.MemoryStream
    $writer = New-Object IO.BinaryWriter($stream)

    $writer.Write([Text.Encoding]::ASCII.GetBytes('RIFF'))
    $writer.Write([int](36 + $dataBytes))
    $writer.Write([Text.Encoding]::ASCII.GetBytes('WAVE'))
    $writer.Write([Text.Encoding]::ASCII.GetBytes('fmt '))
    $writer.Write([int]16)
    $writer.Write([int16]1)
    $writer.Write([int16]1)
    $writer.Write([int]$SampleRate)
    $writer.Write([int]($SampleRate * 2))
    $writer.Write([int16]2)
    $writer.Write([int16]16)
    $writer.Write([Text.Encoding]::ASCII.GetBytes('data'))
    $writer.Write([int]$dataBytes)

    for ($i = 0; $i -lt $samples; $i++) {
        $t = $i / $SampleRate
        $env = 1.0
        $ramp = [int]($SampleRate * 0.005)
        if ($i -lt $ramp) {
            $env = $i / [double]$ramp
        } elseif ($i -gt ($samples - $ramp)) {
            $env = ($samples - $i) / [double]$ramp
        }
        $value = [int16]([math]::Sin(2.0 * [math]::PI * $Frequency * $t) * 24000.0 * $env)
        $writer.Write($value)
    }

    $writer.Flush()
    $stream.Position = 0
    return $stream.ToArray()
}

$toneBytes = New-ToneBytes -Frequency $ToneHz -DurationMs $FlashMs -SampleRate $SampleRate
$toneStream = New-Object IO.MemoryStream(,$toneBytes)
$player = New-Object Media.SoundPlayer($toneStream)
$player.Load()

$window = New-Object Windows.Window
$window.WindowStyle = 'None'
$window.ResizeMode = 'NoResize'
$window.WindowState = 'Maximized'
$window.Topmost = $true
$window.Background = [Windows.Media.Brushes]::Black
$window.Content = New-Object Windows.Controls.Grid

$stopwatch = [Diagnostics.Stopwatch]::StartNew()
$events = @{}
foreach ($offset in $EventOffsetsMs) {
    $events[$InitialDelayMs + $offset] = $false
}

$timer = New-Object Windows.Threading.DispatcherTimer
$timer.Interval = [TimeSpan]::FromMilliseconds(2)
$timer.Add_Tick({
    $elapsed = $stopwatch.ElapsedMilliseconds
    foreach ($key in @($events.Keys)) {
        if (-not $events[$key] -and $elapsed -ge $key) {
            $events[$key] = $true
            $window.Background = [Windows.Media.Brushes]::White
            $toneStream.Position = 0
            $player.Play()

            $offTimer = New-Object Windows.Threading.DispatcherTimer
            $offTimer.Interval = [TimeSpan]::FromMilliseconds($FlashMs)
            $offTimer.Add_Tick({
                $this.Stop()
                $window.Background = [Windows.Media.Brushes]::Black
            })
            $offTimer.Start()
        }
    }

    if ($elapsed -ge ($InitialDelayMs + $DurationMs)) {
        $timer.Stop()
        $window.Close()
    }
})

$window.Add_Loaded({ $timer.Start() })
$null = $window.ShowDialog()
$player.Dispose()
$toneStream.Dispose()
