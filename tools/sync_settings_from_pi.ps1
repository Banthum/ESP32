# Zet de instellingen (zon/batterij/gas) van de ESP32 gelijk aan die van de Pi,
# via het `settings`-blok in de Pi's /api/state. De ESP32 leest ze direct in.
param([string]$PiHttp = "192.168.2.96:8080", [string]$Esp = "192.168.2.251")
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$secrets = Get-Content (Join-Path $root "src\ems\secrets.h") -Raw
$token = ([regex]::Match($secrets, '#define\s+OTA_PASSWORD\s+"((?:[^"\\]|\\.)*)"')).Groups[1].Value -replace '\\(.)', '$1'
$headers = @{ "X-BMS-Token" = $token }

$settings = ((Invoke-WebRequest -UseBasicParsing -TimeoutSec 15 "http://$PiHttp/api/state").Content | ConvertFrom-Json).settings
$configs = [ordered]@{
    "solar_config.json"   = $settings.solar
    "battery_config.json" = $settings.battery
    "gas_config.json"     = $settings.gas
}
foreach ($name in $configs.Keys) {
    if ($null -eq $configs[$name]) { "    $name -> overgeslagen (op de Pi niet ingesteld)"; continue }
    $body = $configs[$name] | ConvertTo-Json -Depth 5 -Compress
    $r = Invoke-WebRequest -UseBasicParsing -Method Put -Headers $headers -ContentType "application/json" -Body $body "http://$Esp/api/config?name=$name"
    "    $name -> $($r.Content)"
}
