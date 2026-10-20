# Eenmalige historie- en instellingenimport van de Pi naar de ESP32.
# Vereist: SSH-sleutel ~/.ssh/id_ed25519_emsmachine op de Pi en een recente
# /home/pi/emsmachine_export.db (sudo-kopie van /etc/emsmachine/emsmachine.db).
param(
    [string]$Pi = "pi@192.168.2.96",
    [string]$PiHttp = "192.168.2.96:8080",
    [string]$Esp = "192.168.2.251"
)
$ErrorActionPreference = "Stop"
$key = "$env:USERPROFILE\.ssh\id_ed25519_emsmachine"
$root = Split-Path -Parent $PSScriptRoot
$importDir = Join-Path $root "import"
New-Item -ItemType Directory -Force $importDir | Out-Null

# Token = OTA_PASSWORD uit src/ems/secrets.h (wordt niet getoond).
$secrets = Get-Content (Join-Path $root "src\ems\secrets.h") -Raw
$token = ([regex]::Match($secrets, '#define\s+OTA_PASSWORD\s+"((?:[^"\\]|\\.)*)"')).Groups[1].Value -replace '\\(.)', '$1'
$headers = @{ "X-EMS-Token" = $token }

Write-Host "[1/5] Slanke historie-DB maken op de Pi (alleen de 4 historietabellen)"
$py = @'
import os, sqlite3
SRC = "/home/pi/emsmachine_export.db"
DST = "/home/pi/emsmachine_esp32_import.db"
if os.path.exists(DST):
    os.remove(DST)
src = sqlite3.connect(SRC)
dst = sqlite3.connect(DST)
for table in ("consumption_quarterly", "savings_quarterly", "solar_forecast", "battery_pool_state"):
    (create_sql,) = src.execute("SELECT sql FROM sqlite_master WHERE type='table' AND name=?", (table,)).fetchone()
    dst.execute(create_sql)
    rows = src.execute(f"SELECT * FROM {table}").fetchall()
    if rows:
        dst.executemany(f"INSERT INTO {table} VALUES ({','.join('?' * len(rows[0]))})", rows)
    print(table, len(rows))
dst.commit()
dst.execute("VACUUM")
dst.close(); src.close()
print("SIZE", os.path.getsize(DST))
'@
$py | ssh -i $key -o BatchMode=yes $Pi "python3 -"

Write-Host "[2/5] Downloaden naar import\"
scp -q -i $key -o BatchMode=yes "${Pi}:/home/pi/emsmachine_esp32_import.db" (Join-Path $importDir "emsmachine_esp32_import.db")
scp -q -i $key -o BatchMode=yes "${Pi}:/home/pi/default_consumption_profile.csv" (Join-Path $importDir "default_consumption_profile.csv")

Write-Host "[3/5] Instellingen van de Pi overnemen"
$settings = ((Invoke-WebRequest -UseBasicParsing -TimeoutSec 15 "http://$PiHttp/api/state").Content | ConvertFrom-Json).settings
$configs = @{
    "solar_config.json"   = $settings.solar
    "battery_config.json" = $settings.battery
    "gas_config.json"     = $settings.gas
    "marstek.json"        = [pscustomobject]@{ ip = "192.168.2.144" }
}
foreach ($name in $configs.Keys) {
    $body = $configs[$name] | ConvertTo-Json -Depth 5 -Compress
    $r = Invoke-WebRequest -UseBasicParsing -Method Put -Headers $headers -ContentType "application/json" -Body $body "http://$Esp/api/config?name=$name"
    "    $name -> $($r.Content)"
}

Write-Host "[4/5] Bestanden uploaden naar de ESP32"
Add-Type -AssemblyName System.Net.Http
function Upload($localPath, $name) {
    $client = New-Object System.Net.Http.HttpClient
    $client.Timeout = [TimeSpan]::FromMinutes(5)
    $client.DefaultRequestHeaders.Add("X-EMS-Token", $token)
    $content = New-Object System.Net.Http.MultipartFormDataContent
    $bytes = [System.IO.File]::ReadAllBytes($localPath)
    $file = New-Object System.Net.Http.ByteArrayContent(, $bytes)
    $content.Add($file, "file", $name)
    $resp = $client.PostAsync("http://$Esp/api/upload?name=$name", $content).Result
    "    $name ($($bytes.Length) bytes) -> $([int]$resp.StatusCode) $($resp.Content.ReadAsStringAsync().Result)"
    $client.Dispose()
}
Upload (Join-Path $importDir "emsmachine_esp32_import.db") "import.db"
Upload (Join-Path $importDir "default_consumption_profile.csv") "default_consumption_profile.csv"

Write-Host "[5/5] Import uitvoeren"
$r = Invoke-WebRequest -UseBasicParsing -Method Post -Headers $headers -TimeoutSec 120 "http://$Esp/api/import"
"    $($r.Content)"
