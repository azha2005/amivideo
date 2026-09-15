<#
    max_duracion.ps1 - la duracion mas larga de un clip que entra limpia en
    un disco, con colores, --min-hold y tamano fijos, y genera el disco.

    "Limpia" es lo mismo que usa --auto: entra en el disco sin subir el
    umbral de perdida (si lo sube se ve granulado) y ningun frame pasa de 4
    VBL de atraso. Prueba varias duraciones en paralelo y achica el rango
    hasta -Precision segundos.

    Ejemplos:
      .\tools\max_duracion.ps1 -In 'C:\Videos\clip.mp4' -Name clip
      .\tools\max_duracion.ps1 -In 'C:\Videos\clip.mp4' -Name clip -Start 10 -Planes 4 -Hold 3 -Size 80
      .\tools\max_duracion.ps1 -In 'C:\Videos\clip.mp4' -Name clip -Extra '--audio-channel','left'

    Resultado: work\<Name>.adf y work\<Name>_preview.mp4.
    Necesita el build hecho (.\build.ps1).
#>
param(
  [Parameter(Mandatory = $true)][string]$In,
  [Parameter(Mandatory = $true)][string]$Name,
  [double]$Start = 0,          # desde que segundo
  [int]$Planes = 5,            # 3 = 8 colores, 4 = 16, 5 = 32
  [int]$Hold = 2,              # --min-hold: 2 = 12,5 img/s, 3 = 8,3, 4 = 6,2
  [int]$Size = 60,             # --size: la imagen al P % del cuadro
  [string[]]$Extra = @(),      # cualquier otra opcion del encoder
  [double]$Precision = 0.5
)

$In = (Resolve-Path $In).Path       # antes de cambiar de carpeta
# con powershell -File, -Extra '--a','b' llega como un solo texto
$Extra = @($Extra | ForEach-Object { $_ -split ',' } | Where-Object { $_ -ne '' })
$repo = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
Set-Location $repo
$budget = '888832'   # lo que queda en el disco con --adf y --reserve-tail 12

$comun = @('--planes', "$Planes", '--min-hold', "$Hold", '--size', "$Size",
           '--sharpen', '0', '--predict', 'auto', '--stability', '0.02', '--max-late', '4')
if ($Planes -gt 3) { $comun += @('--band-rows', '8') }

$srcdur = [double]::Parse((ffprobe -v error -show_entries format=duration -of csv=p=0 $In),
                          [cultureinfo]::InvariantCulture)
$hi = $srcdur - $Start
if ($hi -le 1) { throw "la fuente dura $srcdur s; no queda nada despues de -Start $Start" }
"fuente: $srcdur s; se busca entre 1 y $([math]::Round($hi, 2)) s desde $Start s"

# Prueba varias duraciones en paralelo y devuelve el resultado de cada una.
function Probar([double[]]$durs) {
  $jobs = foreach ($d in $durs) {
    Start-Job -ArgumentList $repo, $In, $Start, $d, $comun, $Extra, $budget, $Name -ScriptBlock {
      param($repo, $in, $start, $d, $comun, $extra, $budget, $name)
      Set-Location $repo
      $ds = $d.ToString([cultureinfo]::InvariantCulture)
      $ss = $start.ToString([cultureinfo]::InvariantCulture)
      $o = "work\maxdur_${name}_$ds.a5v"
      $a = @('--in', $in, '--start', $ss, '--duration', $ds, '--budget', $budget, '--out', $o) + $comun + $extra
      $r = & .\work\a500vp-enc.exe @a 2>&1 | ForEach-Object { "$_" }
      Get-ChildItem work -Filter "maxdur_${name}_$ds.a5v*" | Remove-Item
      $fits = [bool]($r | Select-String 'presupuesto: \d+ bytes, entra')
      $raised = [bool]($r | Select-String 'se subio el umbral')
      $late = 0
      $m = $r | Select-String 'el peor por (\d+) VBL'
      if ($m) { $late = [int]$m.Matches[0].Groups[1].Value }
      $u = ($r | Select-String 'perdida    : umbral') -replace '.*umbral ', '' -replace ' \(.*', ''
      [pscustomobject]@{ d = $d; ok = ($fits -and -not $raised -and $late -le 4); umbral = $u; late = $late }
    }
  }
  $res = $jobs | Wait-Job | Receive-Job
  $jobs | Remove-Job
  $res | Sort-Object d | ForEach-Object {
    "  {0,6:0.00} s: {1}  (umbral {2}, peor {3} VBL)" -f $_.d, $(if ($_.ok) { 'sirve' } else { 'no' }), $_.umbral, $_.late
  } | Write-Host
  return $res
}

# lo: la mas larga que sirvio; hi: la mas corta que no (o el clip entero).
$lo = 1.0
$total = [math]::Round($hi, 2)
$first = $true
$n = [math]::Min([math]::Max(2, [int]($env:NUMBER_OF_PROCESSORS) / 2), 8)
while ($hi - $lo -gt $Precision) {
  $step = ($hi - $lo) / ($n + 1)
  $durs = @(1..$n | ForEach-Object { [math]::Round($lo + $_ * $step, 2) })
  if ($first) { $durs += $total }          # el clip entero, por si entra
  $first = $false
  "probando: " + (($durs | ForEach-Object { $_.ToString([cultureinfo]::InvariantCulture) }) -join ', ')
  $res = Probar $durs
  foreach ($r in ($res | Sort-Object d)) {
    if ($r.ok -and $r.d -gt $lo) { $lo = $r.d }
  }
  $bad = $res | Where-Object { -not $_.ok -and $_.d -gt $lo } | Sort-Object d | Select-Object -First 1
  if ($bad) { $hi = [math]::Min($hi, $bad.d) }
  if ($lo -ge $total) { $hi = $lo }        # entra entero
}

$best = [math]::Round($lo, 2).ToString([cultureinfo]::InvariantCulture)
if ($lo -ge $total) {
  "entra el clip entero con -Size ${Size}: proba con un -Size mas grande o un -Hold mas bajo"
}
"mejor duracion: $best s. Generando el disco..."
$a = @('--in', $In, '--start', $Start.ToString([cultureinfo]::InvariantCulture), '--duration', $best,
       '--boot', 'work\boot.bin', '--player', 'work\player.bin', '--reserve-tail', '12',
       '--out', "work\$Name.a5v", '--adf', "work\$Name.adf") + $comun + $Extra
& .\work\a500vp-enc.exe @a 2>&1 | ForEach-Object { "$_" } |
  Select-String 'geometria|perdida    :|AVISO|audio auto|audio llena|tiempo real|vs\. fuente|de esos|presupuesto: \d+ bytes,' |
  ForEach-Object { $_.Line }
& .\work\a500vp-dec.exe --in "work\$Name.a5v" --preview "work\${Name}_preview.mp4" |
  Select-String 'VERIFICACION' | ForEach-Object { $_.Line }
"listo: work\$Name.adf y work\${Name}_preview.mp4 ($best s)"
