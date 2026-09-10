<#
    get-vasm.ps1 - descarga y compila vasm (vasmm68k_mot) para Windows.

    No necesita permisos de administrador ni 'make': compila directamente con
    el gcc de MSYS2 (UCRT64) que ya esta instalado.

    Resultado: tools\vasm\vasmm68k_mot.exe
    Todo el arbol tools\vasm\ esta ignorado por git.
#>
[CmdletBinding()]
param(
    # Compilador C nativo de Windows. Por defecto el gcc de MSYS2/UCRT64.
    [string]$Gcc = 'C:\msys64\ucrt64\bin\gcc.exe',
    [string]$Url = 'http://sun.hasenbraten.de/vasm/release/vasm.tar.gz',
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
$toolsDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$tarball  = Join-Path $toolsDir 'vasm.tar.gz'
$srcDir   = Join-Path $toolsDir 'vasm'
$exe      = Join-Path $srcDir 'vasmm68k_mot.exe'

if ((Test-Path $exe) -and -not $Force) {
    Write-Host "vasm ya compilado: $exe"
    & $exe -? 2>&1 | Select-Object -First 1
    exit 0
}

if (-not (Test-Path $Gcc)) { throw "No encuentro gcc en '$Gcc'. Pasa -Gcc <ruta>." }

if (-not (Test-Path $tarball)) {
    Write-Host "Descargando $Url ..."
    Invoke-WebRequest -Uri $Url -OutFile $tarball -UseBasicParsing
}

if (-not (Test-Path $srcDir)) {
    Write-Host "Extrayendo ..."
    # tar viene con Windows 10 (bsdtar).
    & tar.exe -xzf $tarball -C $toolsDir
    if ($LASTEXITCODE -ne 0) { throw "tar fallo" }
}

# Mismos formatos de salida que make.rules del propio vasm.
$outfmts = @(
    '-DOUTAOF','-DOUTAOUT','-DOUTBIN','-DOUTCOFF','-DOUTELF','-DOUTGST',
    '-DOUTHANS','-DOUTHUNK','-DOUTIHEX','-DOUTO65','-DOUTPAP','-DOUTSREC',
    '-DOUTTOS','-DOUTVOBJ','-DOUTWOZ','-DOUTXFIL'
)

# Todos los .c de la raiz menos vobjdump.c (es otra herramienta), mas el
# backend de CPU m68k y la sintaxis Motorola.
$sources = @(Get-ChildItem -Path $srcDir -Filter '*.c' |
             Where-Object { $_.Name -ne 'vobjdump.c' } |
             ForEach-Object { $_.FullName })
$sources += (Join-Path $srcDir 'cpus\m68k\cpu.c')
$sources += (Join-Path $srcDir 'syntax\mot\syntax.c')

$args = @('-O2','-w','-std=c99') + $outfmts +
        @("-I$srcDir", "-I$srcDir\cpus\m68k", "-I$srcDir\syntax\mot") +
        $sources + @('-o', $exe, '-lm')

Write-Host "Compilando vasm con $Gcc ..."
& $Gcc @args
if ($LASTEXITCODE -ne 0) { throw "La compilacion de vasm fallo" }

Write-Host "OK -> $exe"
& $exe -? 2>&1 | Select-Object -First 1
