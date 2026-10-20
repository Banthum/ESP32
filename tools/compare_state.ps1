# Veld-voor-veld vergelijking van /api/state: ESP32 tegen de Pi (referentie).
# Live waarden (vermogen/SOC) en tijdstempels verschillen per definitie een
# beetje (andere pollmomenten); curves en instellingen horen (bijna) gelijk.
param([string]$Pi = "192.168.2.96", [string]$Esp = "192.168.2.251")

$p = (Invoke-WebRequest -UseBasicParsing -TimeoutSec 15 "http://${Pi}:8080/api/state").Content | ConvertFrom-Json
$e = (Invoke-WebRequest -UseBasicParsing -TimeoutSec 30 "http://${Esp}/api/state").Content | ConvertFrom-Json

function Describe-Array($a, $b) {
    if ($null -eq $a -and $null -eq $b) { return "beide null" }
    if ($null -eq $a) { return "Pi null, ESP32 lengte $($b.Count)" }
    if ($null -eq $b) { return "ESP32 null, Pi lengte $($a.Count)" }
    $n = [math]::Min($a.Count, $b.Count); $max = 0; $nulls = 0; $cmp = 0
    for ($i = 0; $i -lt $n; $i++) {
        if (($null -eq $a[$i]) -ne ($null -eq $b[$i])) { $nulls++; continue }
        if ($null -eq $a[$i]) { continue }
        $d = [math]::Abs([double]$a[$i] - [double]$b[$i]); if ($d -gt $max) { $max = $d }; $cmp++
    }
    "lengte Pi {0} / ESP32 {1}, {2} vergeleken, max verschil {3:G4}, null-verschillen {4}" -f $a.Count, $b.Count, $cmp, $max, $nulls
}

$keys = @($p.PSObject.Properties.Name) + @($e.PSObject.Properties.Name) | Select-Object -Unique
foreach ($k in $keys) {
    $inP = $p.PSObject.Properties.Name -contains $k
    $inE = $e.PSObject.Properties.Name -contains $k
    if (-not $inP) { "{0,-38} alleen op ESP32: {1}" -f $k, ($e.$k | ConvertTo-Json -Compress -Depth 2); continue }
    if (-not $inE) { "{0,-38} ONTBREEKT op ESP32" -f $k; continue }
    $a = $p.$k; $b = $e.$k
    if ($k -eq "power_history") {
        foreach ($s in "solar", "battery", "grid") { "{0,-38} {1}" -f "power_history.$s", (Describe-Array $a.$s $b.$s) }
    } elseif ($k -eq "settings") {
        $same = ($a | ConvertTo-Json -Depth 4 -Compress) -eq ($b | ConvertTo-Json -Depth 4 -Compress)
        "{0,-38} {1}" -f $k, $(if ($same) { "identiek" } else { "VERSCHILLEND`n    Pi:    $($a | ConvertTo-Json -Depth 4 -Compress)`n    ESP32: $($b | ConvertTo-Json -Depth 4 -Compress)" })
    } elseif ($a -is [array] -or $b -is [array]) {
        "{0,-38} {1}" -f $k, (Describe-Array $a $b)
    } else {
        "{0,-38} Pi {1,-28} ESP32 {2}" -f $k, $a, $b
    }
}
