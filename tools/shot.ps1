<#
    shot.ps1 - arranca el ADF en WinUAE, saca una captura de pantalla y cierra.

        .\tools\shot.ps1 -Adf work\a500vp.adf -Out work\shot.png [-Wait 20]

    Sirve para ver que hace el reproductor sin tener que mirar el monitor:
    la captura queda en un PNG que se puede revisar despues.
#>
[CmdletBinding()]
param(
    [string]$Adf    = 'work\a500vp.adf',
    [string]$Out    = 'work\shot.png',
    [int]$Wait      = 20,
    [string]$WinUAE = 'C:\Program Files\WinUAE\winuae64.exe',
    [string]$Rom    = '',
    [switch]$KeepOpen
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms

$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$work = Join-Path $root 'work'
if (-not (Test-Path $work)) { New-Item -ItemType Directory $work | Out-Null }

if (-not [System.IO.Path]::IsPathRooted($Adf)) { $Adf = Join-Path $root $Adf }
if (-not [System.IO.Path]::IsPathRooted($Out)) { $Out = Join-Path $root $Out }
if (-not $Rom) { $Rom = Join-Path $root 'kick12.rom' }

if (-not (Test-Path $Adf))    { throw "no existe $Adf" }
if (-not (Test-Path $Rom))    { throw "no existe la ROM $Rom" }
if (-not (Test-Path $WinUAE)) { throw "no existe $WinUAE" }

$runAdf = Join-Path $work 'run.adf'
Copy-Item $Adf $runAdf -Force

$cfgOut = Join-Path $work 'shot.uae'
$lines = Get-Content (Join-Path $root 'a500vp.uae') | ForEach-Object {
    if ($_ -like 'kickstart_rom_file=*') { "kickstart_rom_file=$Rom" }
    elseif ($_ -like 'floppy0=*')        { "floppy0=$runAdf" }
    else                                 { $_ }
}
$lines += 'use_gui=no'
Set-Content -Path $cfgOut -Value $lines -Encoding ascii

$proc = Start-Process -FilePath $WinUAE -ArgumentList @('-f', "`"$cfgOut`"") -PassThru
Write-Host "WinUAE pid $($proc.Id); esperando $Wait s..."
Start-Sleep -Seconds $Wait

# Captura de la pantalla completa: la ventana de WinUAE esta encima.
$b = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
$bmp = New-Object System.Drawing.Bitmap $b.Width, $b.Height
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($b.Location, [System.Drawing.Point]::Empty, $b.Size)
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$g.Dispose(); $bmp.Dispose()
Write-Host "captura -> $Out"

if (-not $KeepOpen) {
    if (-not $proc.HasExited) { $proc.Kill(); $proc.WaitForExit(10000) | Out-Null }
}
