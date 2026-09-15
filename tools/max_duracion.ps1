<#
    max_duracion.ps1 - lo maximo que entra limpio en un disco: primero la
    duracion y, si el clip entra entero, el tamano de la imagen. Genera el
    disco.

    "Limpio" es lo mismo que usa --auto: entra en el disco sin subir el
    umbral de perdida (si lo sube se ve granulado) y ningun frame pasa de 4
    VBL de atraso. Las pruebas corren en paralelo.

    1. Con colores, --min-hold y tamano fijos, la duracion mas larga desde
       -Start (hasta -Precision segundos).
    2. Si el clip entra entero y no se dio -Size, el tamano mas grande que
       sigue entrando, de a 2 %, desde -Size hasta 100.

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
  [int]$Size = 60,             # --size: tamano minimo (o fijo, si se da)
  [string[]]$Extra = @(),      # cualquier otra opcion del encoder
  [double]$Precision = 0.5
)

$sizeFixed = $PSBoundParameters.ContainsKey('Size')
$In = (Resolve-Path $In).Path       # antes de cambiar de carpeta
# con powershell -File, -Extra '--a','b' llega como un solo texto
$Extra = @($Extra | ForEach-Object { $_ -split ',' } | Where-Object { $_ -ne '' })
$repo = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
Set-Location $repo
$budget = '888832'   # lo que queda en el disco con --adf y --reserve-tail 12
$inv = [cultureinfo]::InvariantCulture

$comun = @('--planes', "$Planes", '--min-hold', "$Hold",
           '--sharpen', '0', '--predict', 'auto', '--stability', '0.02', '--max-late', '4')
if ($Planes -gt 3) { $comun += @('--band-rows', '8') }

$srcdur = [double]::Parse((ffprobe -v error -show_entries format=duration -of csv=p=0 $In), $inv)
$hi = $srcdur - $Start
if ($hi -le 1) { throw "la fuente dura $srcdur s; no queda nada despues de -Start $Start" }
$total = [math]::Round($hi, 2)
"fuente: $srcdur s; desde $Start s quedan $total s"

# Prueba en paralelo pares (duracion d, tamano s); devuelve cada resultado.
function Probar($casos) {
  $jobs = foreach ($c in $casos) {
    Start-Job -ArgumentList $repo, $In, $Start, $c.d, $c.s, $comun, $Extra, $budget, $Name -ScriptBlock {
      param($repo, $in, $start, $d, $s, $comun, $extra, $budget, $name)
      Set-Location $repo
      $inv = [cultureinfo]::InvariantCulture
      $ds = ([double]$d).ToString($inv)
      $o = "work\maxdur_${name}_${ds}_$s.a5v"
      $a = @('--in', $in, '--start', ([double]$start).ToString($inv), '--duration', $ds, '--size', "$s",
             '--budget', $budget, '--out', $o) + $comun + $extra
      $r = & .\work\a500vp-enc.exe @a 2>&1 | ForEach-Object { "$_" }
      Get-ChildItem work -Filter "maxdur_${name}_${ds}_$s.a5v*" | Remove-Item
      $fits = [bool]($r | Select-String 'presupuesto: \d+ bytes, entra')
      $raised = [bool]($r | Select-String 'se subio el umbral')
      $late = 0
      $m = $r | Select-String 'el peor por (\d+) VBL'
      if ($m) { $late = [int]$m.Matches[0].Groups[1].Value }
      $u = ($r | Select-String 'perdida    : umbral') -replace '.*umbral ', '' -replace ' \(.*', ''
      [pscustomobject]@{ d = [double]$d; s = [int]$s; ok = ($fits -and -not $raised -and $late -le 4); umbral = $u; late = $late }
    }
  }
  $res = $jobs | Wait-Job | Receive-Job
  $jobs | Remove-Job
  $res | Sort-Object s, d | ForEach-Object {
    "  {0,6} s al {1,3} %: {2}  (umbral {3}, peor {4} VBL)" -f $_.d.ToString('0.00', $inv), $_.s,
      $(if ($_.ok) { 'sirve' } else { 'no' }), $_.umbral, $_.late
  } | Write-Host
  return $res
}

$n = [math]::Min([math]::Max(2, [int]($env:NUMBER_OF_PROCESSORS) / 2), 8)

# --- 1. duracion, con el tamano minimo -----------------------------------
# lo: la mas larga que sirvio; hi: la mas corta que no (o el clip entero).
"1. la duracion mas larga con $(1 -shl $Planes) colores, --min-hold $Hold y --size $Size"
$lo = 1.0
$first = $true
while ($hi - $lo -gt $Precision) {
  $step = ($hi - $lo) / ($n + 1)
  $durs = @(1..$n | ForEach-Object { [math]::Round($lo + $_ * $step, 2) })
  if ($first) { $durs += $total }          # el clip entero, por si entra
  $first = $false
  $res = Probar @($durs | ForEach-Object { @{ d = $_; s = $Size } })
  foreach ($r in ($res | Sort-Object d)) { if ($r.ok -and $r.d -gt $lo) { $lo = $r.d } }
  $bad = $res | Where-Object { -not $_.ok -and $_.d -gt $lo } | Sort-Object d | Select-Object -First 1
  if ($bad) { $hi = [math]::Min($hi, $bad.d) }
  if ($lo -ge $total) { $hi = $lo }        # entra entero
}
$dur = [math]::Round($lo, 2)
$size = $Size

# --- 2. si entra entero, el tamano ----------------------------------------
if ($dur -ge $total) {
  if ($sizeFixed) {
    "entra el clip entero con --size ${Size}; se deja ese tamano porque se lo pidio"
  } elseif ($Size -lt 100) {
    "2. entra entero: el tamano mas grande, de $($Size + 2) a 100 %"
    $cands = @()
    for ($s = $Size + 2; $s -le 100; $s += 2) { $cands += $s }
    if ($cands[-1] -ne 100) { $cands += 100 }
    # de a tandas de n, de chico a grande; se corta en la primera tanda
    # donde no sirve ninguno
    for ($i = 0; $i -lt $cands.Count; $i += $n) {
      $tanda = $cands[$i..([math]::Min($i + $n, $cands.Count) - 1)]
      $res = Probar @($tanda | ForEach-Object { @{ d = $total; s = $_ } })
      $okr = $res | Where-Object { $_.ok } | Sort-Object s
      if ($okr) { $size = [math]::Max($size, ($okr | Select-Object -Last 1).s) }
      if (-not $okr -or ($res | Where-Object { -not $_.ok })) { break }
    }
  }
}

$best = $dur.ToString($inv)
"elegido: $best s al $size %. Generando el disco..."
$a = @('--in', $In, '--start', $Start.ToString($inv), '--duration', $best, '--size', "$size",
       '--boot', 'work\boot.bin', '--player', 'work\player.bin', '--reserve-tail', '12',
       '--out', "work\$Name.a5v", '--adf', "work\$Name.adf") + $comun + $Extra
& .\work\a500vp-enc.exe @a 2>&1 | ForEach-Object { "$_" } |
  Select-String 'geometria|perdida    :|AVISO|audio auto|audio llena|tiempo real|vs\. fuente|de esos|presupuesto: \d+ bytes,' |
  ForEach-Object { $_.Line }
& .\work\a500vp-dec.exe --in "work\$Name.a5v" --preview "work\${Name}_preview.mp4" |
  Select-String 'VERIFICACION' | ForEach-Object { $_.Line }
"listo: work\$Name.adf y work\${Name}_preview.mp4 ($best s al $size %)"
