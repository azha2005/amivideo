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

# Captura solo la ventana de WinUAE, con PrintWindow: la ventana se dibuja
# sola en un bitmap. Copiar la pantalla no sirve: si hay otra ventana encima
# se captura esa, que puede ser cualquier cosa del escritorio.
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class A5Shot {
    [StructLayout(LayoutKind.Sequential)]
    public struct RECT { public int L, T, R, B; }
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
}
"@
[A5Shot]::SetProcessDPIAware() | Out-Null
$proc.Refresh()
$h = $proc.MainWindowHandle
if ($h -eq [IntPtr]::Zero) {
    Write-Host "WinUAE no tiene ventana para capturar."
} else {
    $r = New-Object A5Shot+RECT
    [A5Shot]::GetWindowRect($h, [ref]$r) | Out-Null
    $bmp = New-Object System.Drawing.Bitmap ($r.R - $r.L), ($r.B - $r.T)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $hdc = $g.GetHdc()
    $ok = [A5Shot]::PrintWindow($h, $hdc, 2)     # 2 = PW_RENDERFULLCONTENT
    $g.ReleaseHdc($hdc)
    $g.Dispose()
    if ($ok) {
        $bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
        Write-Host "captura -> $Out"
    } else {
        Write-Host "PrintWindow fallo: sin captura."
    }
    $bmp.Dispose()
}

if (-not $KeepOpen) {
    if (-not $proc.HasExited) { $proc.Kill(); $proc.WaitForExit(10000) | Out-Null }
}
