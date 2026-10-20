# Fase 5: draait de ESP32-planner op dezelfde invoer als de Pi en vergelijkt
# het resultaat met de battery_plan_*-velden van de Pi.
# Let op: de Pi plant elke 5 min met de SOC van dat moment; kleine verschillen
# bij een net verstreken kwartier of veranderde SOC zijn daardoor normaal.
param([string]$Pi = "192.168.2.96", [string]$Esp = "192.168.2.251")

$TARIFF = 0.02 + 0.11085 # apply_import_tariff()-opslag, om terug naar EPEX te rekenen

$p = (Invoke-WebRequest -UseBasicParsing -TimeoutSec 15 "http://${Pi}:8080/api/state").Content | ConvertFrom-Json
$b = $p.settings.battery

function Epex($v) { if ($null -eq $v) { $null } else { $v - $TARIFF } }

$quarter = @(); foreach ($v in $p.price_curve_quarterly) { $quarter += , (Epex $v) }
$hourly = @(); for ($h = 0; $h -lt 24; $h++) { $hourly += , (Epex $p.price_curve[$h]) }
for ($h = 0; $h -lt 24; $h++) { $hourly += , (Epex $p.price_curve_quarterly[96 + 4 * $h]) }
$solar = @(); for ($h = 0; $h -lt 48; $h++) { $solar += , $p.solar_forecast_curve_quarterly[4 * $h] }

$body = [ordered]@{
    now_epoch       = [int][double]::Parse((Get-Date -UFormat %s))
    soc_pct         = $p.battery_soc_pct
    capacity_kwh    = $b.capacity_kwh
    max_power_w     = $b.max_power_w
    min_soc_pct     = $b.min_soc_pct
    max_soc_pct     = $b.max_soc_pct
    energy_loss_pct = $b.energy_loss_pct
    price_hourly    = $hourly
    quarter_prices  = $quarter
    consumption     = $p.expected_consumption_curve_quarterly
    solar_hourly    = $solar
} | ConvertTo-Json -Depth 4 -Compress

$sw = [Diagnostics.Stopwatch]::StartNew()
$e = (Invoke-WebRequest -UseBasicParsing -Method Post -TimeoutSec 300 -ContentType "application/json" -Body $body "http://${Esp}/api/debug/plan").Content | ConvertFrom-Json
"ESP32 plan: {0} ms rekentijd op het board ({1:N1} s incl. HTTP), {2} scenario's, actie nu: {3}" -f $e.compute_ms, $sw.Elapsed.TotalSeconds, $e.scenarios_tried, $e.action
"ESP32 laadvensters:  {0}" -f ($e.windows_chosen -join ", ")
"ESP32 blokvensters:  {0}" -f ($e.block_windows_chosen -join ", ")
"Pi SOC nu: {0}%" -f $p.battery_soc_pct

function Compare-Curve($name, $a, $b, $unit) {
    $n = 0; $max = 0; $nullMismatch = 0; $diffQ = @()
    for ($i = 0; $i -lt 192; $i++) {
        if (($null -eq $a[$i]) -ne ($null -eq $b[$i])) { $nullMismatch++; continue }
        if ($null -eq $a[$i]) { continue }
        $d = [math]::Abs($a[$i] - $b[$i]); if ($d -gt $max) { $max = $d }
        if ($d -gt 1) { $diffQ += $i }
        $n++
    }
    "{0,-36} {1,3} kwartieren, max verschil {2,8:N3} {3}, null-verschillen {4}, kwartieren met >1 verschil: {5}" -f $name, $n, $max, $unit, $nullMismatch, $diffQ.Count
}
""
Compare-Curve "battery_plan_actief_gestuurd_wh" $p.battery_plan_actief_gestuurd_wh $e.battery_plan_actief_gestuurd_wh "Wh"
Compare-Curve "battery_plan_eigen_zon_wh" $p.battery_plan_eigen_zon_wh $e.battery_plan_eigen_zon_wh "Wh"
Compare-Curve "battery_plan_verwacht_verbruik_wh" $p.battery_plan_verwacht_verbruik_wh $e.battery_plan_verwacht_verbruik_wh "Wh"
Compare-Curve "battery_plan_geblokkeerd_wh" $p.battery_plan_geblokkeerd_wh $e.battery_plan_geblokkeerd_wh "Wh"
Compare-Curve "battery_soc_forecast_pct" $p.battery_soc_forecast_pct $e.battery_soc_forecast_pct "%"
