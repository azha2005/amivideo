<#
    lote.ps1 - codifica una lista de clips con --auto, uno detras de otro,
    genera la preview de cada uno y deja un resumen.

    La lista es un archivo de datos de PowerShell (por defecto work\clips.psd1,
    fuera de git porque tiene rutas a videos propios):

      @{ Clips = @(
          @{ n = 'caniggia'; in = 'C:\Videos\caniggia.mp4'; x = @('--start', '10', '--duration', '25') }
          @{ n = 'house';    in = 'C:\Videos\house.mp4';    x = @('--duration', '27.35', '--audio-channel', 'left') }
      ) }

    n es el nombre de las salidas, in la fuente y x las opciones propias del
    clip (tramo, canal de audio, --auto-sizes...).

    Ejemplos:
      .\tools\lote.ps1                         todos los de work\clips.psd1
      .\tools\lote.ps1 -Solo caniggia,house    solo esos
      .\tools\lote.ps1 -Lista work\otros.psd1

    Salidas en work\: <n>_auto.adf, <n>_auto_preview.mp4, logs\<n>_auto.txt y
    el resumen de todos en lote_resumen.txt. Necesita el build hecho.
#>
param(
  [string]$Lista = 'work\clips.psd1',
  [string[]]$Solo = @()
)

# con powershell -File, -Solo a,b llega como un solo texto
$Solo = @($Solo | ForEach-Object { $_ -split ',' } | Where-Object { $_ -ne '' })
$repo = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
# la lista relativa se busca donde se esta parado y, si no, en el repo
if (-not [System.IO.Path]::IsPathRooted($Lista)) {
  $aqui = Join-Path (Get-Location) $Lista
  $Lista = if (Test-Path $aqui) { $aqui } else { Join-Path $repo $Lista }
}
Set-Location $repo
if (-not (Test-Path $Lista)) { throw "no esta la lista $Lista (ver el comentario al principio de este script)" }
$clips = (Import-PowerShellDataFile $Lista).Clips

# Opciones comunes: las que dejan los discos limpios (DECISIONS.md, 2026-09-15).
$comun = @('--auto', '--sharpen', '0', '--predict', 'auto', '--stability', '0.02',
           '--max-late', '4', '--boot', 'work\boot.bin', '--player', 'work\player.bin',
           '--reserve-tail', '12')

New-Item -ItemType Directory -Force work\logs | Out-Null
$resumen = 'work\lote_resumen.txt'
"lote  $(Get-Date -Format 'yyyy-MM-dd HH:mm')  ($Lista)" | Out-File -Encoding utf8 $resumen
$pat = '^auto       : elegida|^geometria|^perdida    :|AVISO|^audio auto|^audio llena|^tiempo real|^  cortes|^vs\. fuente|^  de esos|^presupuesto: \d+ bytes,'

foreach ($c in $clips) {
  if ($Solo.Count -gt 0 -and $Solo -notcontains $c.n) { continue }
  $name = "$($c.n)_auto"
  if (-not (Test-Path $c.in)) {
    "=== ${name}: no esta la fuente $($c.in)" | Tee-Object -FilePath $resumen -Append
    continue
  }
  $t0 = Get-Date
  $x = if ($c.x) { @($c.x) } else { @() }
  $a = $comun + @('--in', $c.in) + $x + @('--out', "work\$name.a5v", '--adf', "work\$name.adf")
  $log = "work\logs\$name.txt"
  & .\work\a500vp-enc.exe @a 2>&1 | ForEach-Object { "$_" } | Out-File -Encoding utf8 $log
  $dec = & .\work\a500vp-dec.exe --in "work\$name.a5v" --preview "work\${name}_preview.mp4" 2>&1 | ForEach-Object { "$_" }
  $dec | Out-File -Encoding utf8 -Append $log
  $secs = [int]((Get-Date) - $t0).TotalSeconds
  "=== $name  ($secs s)" | Tee-Object -FilePath $resumen -Append
  Get-Content $log | Select-String -Pattern $pat | ForEach-Object { '  ' + $_.Line.Trim() } | Tee-Object -FilePath $resumen -Append
  $dec | Select-String 'VERIFICACION' | ForEach-Object { '  ' + $_.Line.Trim() } | Tee-Object -FilePath $resumen -Append
}
"fin  $(Get-Date -Format 'yyyy-MM-dd HH:mm')" | Tee-Object -FilePath $resumen -Append
