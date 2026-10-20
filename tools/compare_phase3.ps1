# Fase 3: legt de ruwe ESP32-databronnen naast de Pi's /api/state.
# Gebruik: powershell -File tools\compare_phase3.ps1 [-Pi 192.168.2.96] [-Esp 192.168.2.251]
param([string]$Pi = "192.168.2.96", [string]$Esp = "192.168.2.251")

# Zelfde tarieven als battery_scheduler.apply_import_tariff() op de Pi.
$INKOOP = 0.02; $EB = 0.11085

$p = (Invoke-WebRequest -UseBasicParsing -TimeoutSec 15 "http://${Pi}:8080/api/state").Content | ConvertFrom-Json
$e = (Invoke-WebRequest -UseBasicParsing -TimeoutSec 15 "http://${Esp}/api/state").Content | ConvertFrom-Json

"=== Live (momentopnames, verschillen in tijd zijn normaal) ==="
"{0,-18} {1,12} {2,12}" -f "veld", "Pi", "ESP32"
foreach ($k in "timestamp", "battery_power_w", "battery_state", "battery_soc_pct", "solar_power_w", "grid_power_w") {
    "{0,-18} {1,12} {2,12}" -f $k, $p.$k, $e.$k
}
"ESP32 marstek_mode: {0}, p1: {1} ({2}), leestijd {3} ms" -f $e.probe.marstek_mode, $e.probe.p1_ok, $e.probe.p1_ip, $e.probe.read_ms

"`n=== Prijs vandaag: Pi price_curve (all-in) vs ESP32 EPEX + opslagen ==="
$maxdiff = 0
for ($h = 0; $h -lt 24; $h++) {
    $esp = $e.probe.price_curve_epex[$h] + $INKOOP + $EB
    $d = [math]::Abs($p.price_curve[$h] - $esp)
    if ($d -gt $maxdiff) { $maxdiff = $d }
}
"max verschil over 24 uur: {0:N6} EUR/kWh" -f $maxdiff

"`n=== Kwartierprijzen (dynamisch-tarief, komende kwartieren) vs Pi price_curve_quarterly ==="
$n = 0; $maxq = 0
foreach ($prop in $e.probe.quarter_prices.PSObject.Properties) {
    $q = [int]$prop.Name
    if ($q -ge 192 -or $null -eq $p.price_curve_quarterly[$q]) { continue }
    $d = [math]::Abs($p.price_curve_quarterly[$q] - ($prop.Value + $INKOOP + $EB))
    if ($d -gt $maxq) { $maxq = $d }
    $n++
}
"{0} kwartieren vergeleken, max verschil {1:N6} EUR/kWh" -f $n, $maxq

"`n=== Gas ==="
"Pi gas_enabled={0}; ESP32 gas uur 0 = {1} EUR/m3" -f $p.gas_enabled, $e.probe.gas_price_curve_m3[0]

"`n=== Zon vandaag (kW per uur): Pi solar_forecast_curve vs ESP32 Forecast.Solar x forecast_factor ==="
$factor = $p.settings.solar.forecast_factor
$today = (Get-Date).ToString("yyyy-MM-dd")
$maxs = 0; $cnt = 0
foreach ($r in $e.probe.solar_rows | Where-Object { $_.date -eq $today }) {
    $pi = $p.solar_forecast_curve[$r.hour]
    if ($null -eq $pi) { continue }
    $d = [math]::Abs($pi - $r.watts * $factor / 1000)
    if ($d -gt $maxs) { $maxs = $d }
    $cnt++
}
"{0} uren vergeleken (factor {1}), max verschil {2:N3} kW (Pi haalt elke 3 u op - kleine afwijking kan)" -f $cnt, $factor, $maxs
