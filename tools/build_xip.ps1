# Bouwt (en flasht optioneel) de [env:ems_xip]-firmware - de versie met stil beeld.
#
#   powershell -File tools\build_xip.ps1                 # alleen bouwen
#   powershell -File tools\build_xip.ps1 -Flash usb      # bouwen + flashen via USB (COM3)
#   powershell -File tools\build_xip.ps1 -Flash ota      # bouwen + flashen via WiFi (/api/ota)
#
# Waarom zo omslachtig:
# - pioarduino (nodig voor custom_sdkconfig) draait in een eigen PlatformIO-map,
#   zodat het de officiele espressif32-installatie van andere projecten niet
#   overschrijft.
# - ESP-IDF/CMake kan niet bouwen vanaf de netwerkschijf (UNC-pad met spatie),
#   dus het project wordt eerst naar een lokale map gespiegeld.
# - ESP-IDF weigert te bouwen vanuit Git Bash (MSYS); gebruik PowerShell.
# - "pio run -t upload" is stuk in pioarduino; we flashen firmware.factory.bin
#   zelf met esptool (en wissen otadata, zodat app-slot 0 start).
param(
    [ValidateSet("none", "usb", "ota")] [string]$Flash = "none",
    [string]$Port = "COM3",
    [string]$Board = "emsmachine-esp32.local"  # of het IP van het bord
)
$ErrorActionPreference = "Stop"

$repo = Split-Path -Parent $PSScriptRoot
$mirror = Join-Path $env:LOCALAPPDATA "ems-xip-src"
$coreDir = Join-Path $env:LOCALAPPDATA "pio-core-xip"
$pio = Join-Path $env:USERPROFILE ".platformio\penv\Scripts\pio.exe"
$buildDir = Join-Path $env:LOCALAPPDATA "pio-build\ESP32\ems_xip"

robocopy $repo $mirror /MIR /XD .pio .git .vscode backup firmware import .dummy managed_components `
    /XF CMakeLists.txt dependencies.lock sdkconfig.defaults sdkconfig.ems_xip /NFL /NDL /NJH /NJS /NP | Out-Null
if ($LASTEXITCODE -ge 8) { throw "robocopy mislukt ($LASTEXITCODE)" }

$env:PLATFORMIO_CORE_DIR = $coreDir
Remove-Item Env:MSYSTEM -ErrorAction SilentlyContinue
Push-Location $mirror
try {
    & $pio run -e ems_xip
    if ($LASTEXITCODE -ne 0) { throw "build mislukt" }
} finally {
    Pop-Location
}

$image = Join-Path $buildDir "firmware.factory.bin"
if ($Flash -eq "usb") {
    $py = Join-Path $coreDir "penv\Scripts\python.exe"
    $esptool = Join-Path $coreDir "packages\tool-esptoolpy\esptool.py"
    & $py $esptool --chip esp32s3 --port $Port --baud 921600 erase-region 0xe000 0x2000
    & $py $esptool --chip esp32s3 --port $Port --baud 921600 write-flash -z 0x0 $image
} elseif ($Flash -eq "ota") {
    # Alleen de app (firmware.bin); het token is het OTA_PASSWORD uit src/ems/secrets.h.
    $secrets = Get-Content (Join-Path $repo "src\ems\secrets.h") -Raw
    $token = [regex]::Match($secrets, '#define OTA_PASSWORD "(.*)"').Groups[1].Value
    curl.exe -s -m 300 -H "X-EMS-Token: $token" -F "fw=@$(Join-Path $buildDir 'firmware.bin')" "http://$Board/api/ota"
}
