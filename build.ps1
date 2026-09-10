<#
    build.ps1 - construccion de A500VP.

        .\build.ps1                 ensambla, compila y genera work\a500vp.adf
        .\build.ps1 run             ademas lo arranca en WinUAE
        .\build.ps1 check           lo arranca y lee la medicion del Hito 0
        .\build.ps1 clean           borra work\

    Rutas por defecto detectadas en esta maquina; se pueden pisar con
    -Vasm, -Gcc, -WinUAE, -Rom.
#>
[CmdletBinding()]
param(
    [ValidateSet('build', 'run', 'check', 'clean')]
    [string]$Task = 'build',

    [string]$Vasm   = 'C:\Users\JC\vbcc\bin\vasmm68k_mot.exe',
    [string]$Gcc    = 'C:\msys64\ucrt64\bin\gcc.exe',
    [string]$WinUAE = 'C:\Program Files\WinUAE\winuae64.exe',
    [string]$Rom    = '',

    # Segundos que dejamos correr el emulador antes de cerrarlo. El Hito 0
    # solo arranca y mide, con lo que unos pocos alcanzan.
    [int]$Timeout = 25
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$work = Join-Path $root 'work'

# El ultimo sector del disquete lleva la medicion que graba el reproductor.
$RESULT_SECTOR = 1759
$SECTOR_SIZE   = 512

function Invoke-Tool {
    param([string]$Exe, [string[]]$Arguments)
    Write-Host "+ $Exe $($Arguments -join ' ')" -ForegroundColor DarkGray
    & $Exe @Arguments
    if ($LASTEXITCODE -ne 0) { throw "fallo: $Exe (codigo $LASTEXITCODE)" }
}

function Get-BE32 {
    param([byte[]]$Bytes, [int]$Offset)
    return ([uint32]$Bytes[$Offset]     -shl 24) -bor
           ([uint32]$Bytes[$Offset + 1] -shl 16) -bor
           ([uint32]$Bytes[$Offset + 2] -shl 8)  -bor
           ([uint32]$Bytes[$Offset + 3])
}

#---------------------------------------------------------------------------
function Build-All {
    if (-not (Test-Path $Vasm)) { throw "No encuentro vasm en '$Vasm'. Pasa -Vasm <ruta>." }
    if (-not (Test-Path $Gcc))  { throw "No encuentro gcc en '$Gcc'. Pasa -Gcc <ruta>." }
    if (-not (Test-Path $work)) { New-Item -ItemType Directory $work | Out-Null }

    $player = Join-Path $root 'player'

    Invoke-Tool $Vasm @('-Fbin', '-m68000', '-no-opt', '-I', $player,
                        '-o', (Join-Path $work 'boot.bin'),
                        (Join-Path $player 'boot.s'))

    Invoke-Tool $Vasm @('-Fbin', '-m68000', '-no-opt', '-I', $player,
                        '-o', (Join-Path $work 'memcheck.bin'),
                        (Join-Path $player 'memcheck.s'))

    Invoke-Tool $Gcc @('-std=c11', '-O2', '-Wall', '-Wextra', '-pedantic',
                       (Join-Path $root 'encoder\adf.c'),
                       (Join-Path $root 'encoder\mkadf.c'),
                       '-o', (Join-Path $work 'mkadf.exe'))

    Invoke-Tool (Join-Path $work 'mkadf.exe') @(
        '--boot',   (Join-Path $work 'boot.bin'),
        '--player', (Join-Path $work 'memcheck.bin'),
        '--out',    (Join-Path $work 'a500vp.adf'))
}

#---------------------------------------------------------------------------
# Genera una copia del .uae del repo con la ROM y el ADF puestos, y lanza
# WinUAE. Devuelve la ruta del ADF que uso (una copia: el reproductor le
# graba encima y no queremos ensuciar el original).
function Start-Emulator {
    param([switch]$Headless)

    if (-not (Test-Path $WinUAE)) { throw "No encuentro WinUAE en '$WinUAE'." }

    $romPath = $Rom
    if (-not $romPath) {
        $candidate = Join-Path $root 'kick12.rom'
        if (Test-Path $candidate) { $romPath = $candidate }
    }
    if (-not $romPath -or -not (Test-Path $romPath)) {
        throw "Falta la ROM de Kickstart. Pasa -Rom <ruta> o deja kick12.rom en la raiz."
    }

    $adf = Join-Path $work 'a500vp.adf'
    if (-not (Test-Path $adf)) { throw "No hay ADF; corre '.\build.ps1' primero." }
    $runAdf = Join-Path $work 'run.adf'
    Copy-Item $adf $runAdf -Force

    $cfgIn  = Join-Path $root 'a500vp.uae'
    $cfgOut = Join-Path $work 'a500vp.run.uae'
    $lines = Get-Content $cfgIn | ForEach-Object {
        if ($_ -like 'kickstart_rom_file=*') { "kickstart_rom_file=$romPath" }
        elseif ($_ -like 'floppy0=*')        { "floppy0=$runAdf" }
        else                                 { $_ }
    }
    if ($Headless) { $lines += 'use_gui=no' }
    Set-Content -Path $cfgOut -Value $lines -Encoding ascii

    Write-Host "ROM : $romPath"
    Write-Host "ADF : $runAdf"
    $proc = Start-Process -FilePath $WinUAE -ArgumentList @('-f', "`"$cfgOut`"") -PassThru
    return @{ Process = $proc; Adf = $runAdf }
}

#---------------------------------------------------------------------------
function Show-Measurement {
    param([string]$Adf)

    $bytes = [System.IO.File]::ReadAllBytes($Adf)
    $off = $RESULT_SECTOR * $SECTOR_SIZE
    $magic = [System.Text.Encoding]::ASCII.GetString($bytes, $off, 4)

    if ($magic -ne 'MEMR') {
        Write-Host ""
        Write-Host "El reproductor no grabo la medicion (magic='$magic')." -ForegroundColor Yellow
        Write-Host "Mira la pantalla del emulador: los valores se muestran igual."
        return $false
    }

    $names = @('CHIP libre', 'CHIP mayor bloque', 'SLOW/FAST libre',
               'SLOW/FAST mayor bloque', 'TOTAL libre')
    Write-Host ""
    Write-Host "Memoria libre medida con AvailMem desde el reproductor:" -ForegroundColor Green
    for ($i = 0; $i -lt 5; $i++) {
        $v = Get-BE32 $bytes ($off + 4 + $i * 4)
        Write-Host ("  {0,-24} {1,10} bytes  {2,7} KB  (0x{3:X8})" -f
                    $names[$i], $v, [math]::Round($v / 1024, 1), $v)
    }
    $s2 = Get-BE32 $bytes ($off + 24)
    $bp = Get-BE32 $bytes ($off + 28)
    Write-Host ("  reproductor cargado en   0x{0:X8}" -f $s2)
    Write-Host ("  bitplane asignado en     0x{0:X8}" -f $bp)
    return $true
}

#---------------------------------------------------------------------------
switch ($Task) {
    'clean' {
        if (Test-Path $work) { Remove-Item $work -Recurse -Force }
        Write-Host "work\ borrado"
    }

    'build' { Build-All }

    'run' {
        Build-All
        $r = Start-Emulator
        Write-Host "WinUAE lanzado. Cerralo vos cuando termines de mirar."
    }

    'check' {
        Build-All
        $r = Start-Emulator -Headless
        Write-Host "Esperando $Timeout s a que arranque y mida..."
        $null = $r.Process.WaitForExit($Timeout * 1000)
        if (-not $r.Process.HasExited) {
            # Cerrar por la ventana, no matarlo: WinUAE mantiene la pista
            # escrita en memoria y solo vuelca el .adf al salir ordenadamente.
            $r.Process.CloseMainWindow() | Out-Null
            if (-not $r.Process.WaitForExit(15000)) {
                Write-Host "WinUAE no cerro solo; lo mato (el ADF puede no tener la medicion)." -ForegroundColor Yellow
                $r.Process.Kill()
                $r.Process.WaitForExit(10000) | Out-Null
            }
        }
        Start-Sleep -Milliseconds 500
        if (-not (Show-Measurement $r.Adf)) { exit 1 }
    }
}
