# Fase 4: vergelijkt de opslaglaag van de ESP32 (/api/debug/storage) met de Pi.
param([string]$Pi = "192.168.2.96", [string]$Esp = "192.168.2.251")

$p = (Invoke-WebRequest -UseBasicParsing -TimeoutSec 15 "http://${Pi}:8080/api/state").Content | ConvertFrom-Json
$e = (Invoke-WebRequest -UseBasicParsing -TimeoutSec 60 "http://${Esp}/api/debug/storage").Content | ConvertFrom-Json

function Compare-Curve($name, $a, $b) {
    $n = 0; $max = 0; $nullMismatch = 0
    for ($i = 0; $i -lt $a.Count; $i++) {
        if (($null -eq $a[$i]) -ne ($null -eq $b[$i])) { $nullMismatch++; continue }
        if ($null -eq $a[$i]) { continue }
        $d = [math]::Abs($a[$i] - $b[$i]); if ($d -gt $max) { $max = $d }; $n++
    }
    "{0,-40} {1,3} waarden, max verschil {2:G4}, null-verschillen {3}" -f $name, $n, $max, $nullMismatch
}

"=== Verbruiksvoorspelling (kWh per kwartier) ==="
Compare-Curve "expected_consumption_curve (vandaag)" $p.expected_consumption_curve $e.expected_consumption_curve
$piTomorrow = $p.expected_consumption_curve_quarterly[96..191]
Compare-Curve "expected_consumption (morgen)" $piTomorrow $e.expected_consumption_curve_tomorrow

"`n=== Zonvoorspelling uit de DB (kW) - Pi en ESP32 halen elk zelf op ==="
Compare-Curve "solar_forecast_curve (vandaag, per uur)" $p.solar_forecast_curve $e.solar_forecast_curve

"`n=== Besparingen (EUR) - ESP32 uit geimporteerde historie, Pi live incl. ongeflusht deel ==="
foreach ($period in "dag", "week", "maand", "jaar", "altijd") {
    foreach ($pool in "batterij", "ems") {
        $k = "savings_${period}_${pool}_eur"
        "{0,-32} Pi {1,9:N4}   ESP32 {2,9:N4}" -f $k, $p.$k, $e.$k
    }
}
"`nshadow_soc_kwh ESP32: {0:N4}   overnight_baseline gisteren: {1}   Pi min_home_w: {2}" -f $e.shadow_soc_kwh, $e.overnight_baseline_yesterday_w, $p.min_home_w
