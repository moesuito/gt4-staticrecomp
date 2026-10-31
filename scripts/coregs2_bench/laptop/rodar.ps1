# CoreGS 2.0 benchmark for a portable GT4Recomp folder (the owner's i5-7200U / HD 620 laptop, 2026-10-30).
# Copy this folder as <portable folder>\bench with a test memory card as card-base.ps2, then run RODAR.bat. It plays the
# same scripted Arcade night race as scripts/coregs2_bench/arcade-run.sh, so the numbers compare with the desktop's.
# Nothing in user\ or data\ is written: the card is a copy, the logs go to bench\logs.
param(
    [string]$Root = (Split-Path $PSScriptRoot -Parent),
    [string]$Exe = '',
    [string]$Elf = '',
    [string]$Disc = '',
    [int]$Frames = 11500,
    [string[]]$Only = @(),
    [int]$Set = 1
)
$ErrorActionPreference = 'Stop'
$Only = @($Only | ForEach-Object { $_ -split ',' })
if (!$Exe) { $Exe = Join-Path $PSScriptRoot 'gt4.exe' }
if (!$Elf) { $Elf = Join-Path $Root 'data\games\gt4\prepared\SCUS_973.28' }
if (!$Disc) { $Disc = Join-Path $Root 'data\games\gt4\disc\SCUS-97328.iso' }
foreach ($path in @($Exe, $Elf, $Disc, "$PSScriptRoot\card-base.ps2")) {
    if (!(Test-Path -LiteralPath $path)) { Write-Host "Faltando: $path" -ForegroundColor Red; exit 2 }
}
if (Get-Process gt4,gt4-spec2,GT4Recomp -ErrorAction SilentlyContinue) {
    Write-Host 'Feche o launcher e o jogo antes de rodar.' -ForegroundColor Yellow
    exit 3
}
$logs = Join-Path $PSScriptRoot ('logs\' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Force $logs | Out-Null

# The machine: real clock under load, power source and plan.
$info = @()
$info += (Get-CimInstance Win32_Processor | Select-Object Name, NumberOfCores, NumberOfLogicalProcessors, MaxClockSpeed | Format-List | Out-String)
$info += (Get-CimInstance Win32_PhysicalMemory | Select-Object Capacity, ConfiguredClockSpeed, DeviceLocator | Format-Table | Out-String)
$info += 'Battery status (2 = on AC): ' + ((Get-CimInstance Win32_Battery -ErrorAction SilentlyContinue | Select-Object -ExpandProperty BatteryStatus) -join ',')
$info += (& powercfg /getactivescheme | Out-String)
$info | Set-Content (Join-Path $logs 'machine.txt') -Encoding UTF8

# Counter names are localized on Windows, the WMI classes are not: the real clock every 5 s, for the whole benchmark.
$clockFile = Join-Path $logs 'clock.csv'
$clockCommand = "'time,mhz,performance_pct,cpu_pct,gpu_pct' | Set-Content '$clockFile'; while (`$true) { `$p = Get-CimInstance Win32_PerfFormattedData_Counters_ProcessorInformation | Where-Object { `$_.Name -eq '_Total' }; `$g = (Get-CimInstance Win32_PerfFormattedData_GPUPerformanceCounters_GPUEngine -ErrorAction SilentlyContinue | Where-Object { `$_.Name -like '*engtype_3D' } | Measure-Object -Property UtilizationPercentage -Sum).Sum; ('{0},{1},{2},{3},{4}' -f (Get-Date -Format 'HH:mm:ss'), `$p.ProcessorFrequency, `$p.PercentProcessorPerformance, `$p.PercentProcessorTime, `$g) | Add-Content '$clockFile'; Start-Sleep 5 }"
$clockSampler = Start-Process powershell -ArgumentList '-NoProfile', '-Command', $clockCommand -PassThru -WindowStyle Hidden

$press = '600:start:30 900:cross:30 1500:cross:30 2300:down:10 2450:cross:10 3500:right:10 3700:right:10 3900:cross:10 4100:cross:10 6500:down:8 6650:cross:10 7400:cross:10 7600:cross:10 7800:cross:10 8700:cross:10 9200:cross:7000'
$pressArgs = @(); foreach ($p in $press.Split(' ')) { $pressArgs += '--press'; $pressArgs += $p }

$sampler = Join-Path $logs 'sampler.bin'
$tests = @(
    @{ name='1-aquecimento'; text='aquecendo (compila os shaders que faltam)'; scale=1; frames=$Frames; window=$false; env=@{} },
    @{ name='2-1x'; text='1x, memoria nova'; scale=1; frames=$Frames; window=$false; env=@{ GT4_CPU_SAMPLER=$sampler; GT4_CPU_SAMPLER_FROM='9400'; GT4_CPU_SAMPLER_TO='11000' } },
    @{ name='3-1x-memoria-antiga'; text='1x, memoria antiga'; scale=1; frames=$Frames; window=$false; env=@{ GT4_CORE2_CACHED_UPLOAD='0' } },
    @{ name='4-2x'; text='2x'; scale=2; frames=$Frames; window=$false; env=@{} },
    @{ name='5-1x-janela-fsr'; text='1x com janela, FSR na tela'; scale=1; frames=10500; window=$true; env=@{} },
    @{ name='6-1x-janela-bilinear'; text='1x com janela, filtro simples'; scale=1; frames=10500; window=$true; env=@{ GT4_CORE2_PRESENT_FILTER='bilinear' } }
)
if ($Set -eq 2) {
    # Second round: the fill-rate benchmark of each way of writing a pixel, then the new build.
    $fill = Join-Path $PSScriptRoot 'fill'
    Write-Host 'Medindo o custo de cada jeito de desenhar ...' -ForegroundColor Cyan
    Push-Location $fill
    foreach ($size in @(@(640, 448), @(1280, 896))) {
        foreach ($shared in @($false, $true)) {
            $name = 'fill-{0}x{1}{2}.txt' -f $size[0], $size[1], $(if ($shared) { '-shared' } else { '' })
            if ($shared) { $env:FB_SHARED = '1' } else { Remove-Item env:FB_SHARED -ErrorAction SilentlyContinue }
            & (Join-Path $fill 'fillbench.exe') $size[0] $size[1] 20 2>&1 | Out-File (Join-Path $logs $name) -Encoding UTF8
        }
    }
    Remove-Item env:FB_SHARED -ErrorAction SilentlyContinue
    Pop-Location
    $tests = @(
        @{ name='1-1x'; text='1x, EE sem esperar o GS'; scale=1; frames=$Frames; window=$false; env=@{} },
        @{ name='2-1x-sem-interlock'; text='1x sem interlock'; scale=1; frames=$Frames; window=$false; env=@{ GT4_HW_INTERLOCK='0' } },
        @{ name='3-2x'; text='2x'; scale=2; frames=$Frames; window=$false; env=@{} },
        @{ name='4-1x-janela'; text='1x com janela, filtro simples'; scale=1; frames=10500; window=$true; env=@{} }
    )
}
if ($Set -eq 3) {
    # Third round: textures through the shader against the hardware sampler, and where the GPU's time goes in the game.
    $fill = Join-Path $PSScriptRoot 'fill'
    Write-Host 'Medindo textura e desenho ...' -ForegroundColor Cyan
    Push-Location $fill
    $env:FB_ROUND3 = '1'; $env:FB_SHARED = '1'
    foreach ($size in @(@(640, 448), @(1280, 896))) {
        & (Join-Path $fill 'fillbench.exe') $size[0] $size[1] 20 2>&1 | Out-File (Join-Path $logs ('fill3-{0}x{1}.txt' -f $size[0], $size[1])) -Encoding UTF8
    }
    Remove-Item env:FB_ROUND3, env:FB_SHARED -ErrorAction SilentlyContinue
    Pop-Location
    $tests = @(
        @{ name='1-mapa-1x'; text='mapa da GPU em 1x'; scale=1; frames=10900; window=$false; env=@{ GT4_CORE2_GPU_PROFILE_AT='10800'; GT4_CORE2_GPU_STATISTICS='1' } },
        @{ name='2-mapa-2x'; text='mapa da GPU em 2x'; scale=2; frames=10900; window=$false; env=@{ GT4_CORE2_GPU_PROFILE_AT='10800'; GT4_CORE2_GPU_STATISTICS='1' } }
    )
}
if ($Set -eq 4) {
    # Fourth round: draws that read nothing back through colour attachments (IN-481), on and off.
    $tests = @(
        @{ name='1-aquecimento'; text='aquecendo (compila os shaders novos)'; scale=1; frames=$Frames; window=$false; env=@{} },
        @{ name='2-1x-novo'; text='1x, caminho novo'; scale=1; frames=$Frames; window=$false; env=@{} },
        @{ name='3-1x-antigo'; text='1x, caminho antigo'; scale=1; frames=$Frames; window=$false; env=@{ GT4_CORE2_ROP='0' } },
        @{ name='4-2x-novo'; text='2x, caminho novo'; scale=2; frames=$Frames; window=$false; env=@{} },
        @{ name='5-mapa-2x'; text='mapa da GPU em 2x'; scale=2; frames=10900; window=$false; env=@{ GT4_CORE2_GPU_PROFILE_AT='10800' } }
    )
}
if ($Set -eq 6) {
    # Reference: GSdx on this machine, Direct3D 11 as the launcher's Performance Mode sets it, on the same scene.
    $gsExe = Join-Path $Root 'data\gs\1.2.1\gt4-gs.exe'
    $gsSha = (Get-FileHash -LiteralPath $gsExe -Algorithm SHA256).Hash.ToLowerInvariant()
    $gsTests = @()
    foreach ($scale in 1, 2) {
        $ini = Join-Path $logs "gsdx-$($scale)x.ini"
        @('[EmuCore/GS]', 'Renderer = 3', "upscale_multiplier = $scale", 'accurate_blending_unit = 0', 'deinterlace_mode = 0', 'AspectRatio = 16:9',
          'VsyncEnable = false', 'DisableMailboxPresentation = true', 'HWDownloadMode = 2', 'LoadTextureReplacements = false', '',
          '[Folders]', ('Cache = ' + (Join-Path $Root 'data\gs\cache')), ('Textures = ' + (Join-Path $Root 'data\Textures')), '',
          '[GT4GS]', ('DataRoot = ' + (Join-Path $Root 'data\gs\data')), 'ProhibitDynamicCode = false') | Set-Content -LiteralPath $ini -Encoding ascii
        $gsTests += @{ name="gsdx-$($scale)x"; text="GSdx em $($scale)x"; scale=1; frames=$Frames; window=$true;
                       env=@{ GT4_RENDERER='pcsx2'; PS2_GS='gpu'; GT4_GS_PROCESS=$gsExe; GT4_GS_PROCESS_SHA256=$gsSha; GT4_GS_CONFIG=$ini; GT4_FLARE_LIMITS='1'; GT4_PERF='1' } }
    }
    # GSdx cannot start without the logged-in desktop, so this set runs from RODAR5.bat, with the window. The first test
    # shows what presenting costs CoreGS 2.0 (the CPU profile with a window).
    $tests = @(
        @{ name='coregs-1x-janela'; text='CoreGS 2.0 em 1x com janela'; scale=1; frames=10500; window=$true; env=@{ GT4_CPU_SAMPLER=$sampler; GT4_CPU_SAMPLER_FROM='9400'; GT4_CPU_SAMPLER_TO='10400'; GT4_PERF='1' } },
        @{ name='coregs-2x-janela'; text='CoreGS 2.0 em 2x com janela'; scale=2; frames=10500; window=$true; env=@{ GT4_PERF='1' } }
    ) + $gsTests
}
if ($Set -eq 7) {
    # A light scene: Time Trial on the Nordschleife by day, alone on the track (the car stays against the rail on the first
    # bend, so the second half is a still view of the track).
    $press = '600:start:30 900:cross:30 1300:right:10 1500:cross:30 2450:cross:10 3900:cross:10 4100:cross:10 6500:down:8 6650:cross:10 7400:cross:10 7600:cross:10 7800:cross:10 8700:cross:10 9200:cross:7000'
    $pressArgs = @(); foreach ($p in $press.Split(' ')) { $pressArgs += '--press'; $pressArgs += $p }
    $tests = @(
        @{ name='1-dia-aquecimento'; text='dia, aquecendo'; scale=1; frames=11000; window=$false; env=@{} },
        @{ name='2-dia-1x'; text='dia, 1x'; scale=1; frames=11000; window=$false; env=@{} },
        @{ name='3-dia-2x'; text='dia, 2x'; scale=2; frames=11000; window=$false; env=@{} }
    )
}
$env:GT4_RENDERER = 'hw'; $env:GT4_HW_CORE = '2'; $env:GT4_FORCE_480P = '1'; $env:GT4_FORCE_WIDESCREEN = '1'
$env:GT4_CORE2_PERFORMANCE = '1'; $env:GT4_MTVU_REPORT_EVERY = '600'; $env:GT4_HW_PROFILE_EVERY = '600'
$number = 0
foreach ($test in $tests) {
    $number++
    if ($Only.Count -and $Only -notcontains $test.name) { continue }
    Write-Host ("[{0}/{1}] {2} ... {3}" -f $number, $tests.Count, $test.text, (Get-Date -Format 'HH:mm:ss')) -ForegroundColor Cyan
    $dir = Join-Path $logs $test.name
    New-Item -ItemType Directory -Force $dir | Out-Null
    Copy-Item -LiteralPath "$PSScriptRoot\card-base.ps2" -Destination "$dir\card.ps2"
    $env:GT4_HW_SCALE = "$($test.scale)"
    foreach ($key in $test.env.Keys) { Set-Item "env:$key" $test.env[$key] }
    $arguments = @($Elf, '--ntsc', '--disc', $Disc, '--frames', "$($test.frames)", '--memcard', "$dir\card.ps2") + $pressArgs
    if ($test.window) {
        $env:GT4_PRESENT_MODE = 'immediate'; $env:GT4_FULLSCREEN = '1'
        $arguments += '--window'
    }
    $started = Get-Date
    $quoted = $arguments | ForEach-Object { if ($_ -match '\s') { '"' + $_ + '"' } else { $_ } }
    $process = Start-Process -FilePath $Exe -ArgumentList $quoted -WorkingDirectory $dir -RedirectStandardError "$dir\log.txt" -RedirectStandardOutput "$dir\stdout.txt" -PassThru -WindowStyle Hidden
    $process.WaitForExit()
    Write-Host ("    terminou em {0:N1} min, codigo {1}" -f ((Get-Date) - $started).TotalMinutes, $process.ExitCode)
    foreach ($key in $test.env.Keys) { Remove-Item "env:$key" -ErrorAction SilentlyContinue }
    Remove-Item env:GT4_PRESENT_MODE, env:GT4_FULLSCREEN -ErrorAction SilentlyContinue
    Remove-Item "$dir\card.ps2" -ErrorAction SilentlyContinue
}
Stop-Process -Id $clockSampler.Id -ErrorAction SilentlyContinue
Write-Host "Pronto. Resultados em $logs" -ForegroundColor Green
