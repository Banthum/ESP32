# Maakt van elk dashboard-tabblad een screenshot (PNG) via /api/debug/tab en
# /api/screenshot. Gebruik: powershell -File tools\screenshots.ps1 [-Out map]
param([string]$Esp = "192.168.2.251", [string]$Out = "$env:TEMP\ems-screens")
New-Item -ItemType Directory -Force $Out | Out-Null
Add-Type -AssemblyName System.Drawing
$names = "0-energiestromen", "1-kosten", "2-verbruik", "3-zon", "4-batterij", "5-besparing"
for ($i = 0; $i -lt $names.Count; $i++) {
    Invoke-WebRequest -UseBasicParsing -TimeoutSec 15 "http://$Esp/api/debug/tab?i=$i" | Out-Null
    Start-Sleep -Milliseconds 1500
    $bmp = Join-Path $Out "$($names[$i]).bmp"
    Invoke-WebRequest -UseBasicParsing -TimeoutSec 60 "http://$Esp/api/screenshot" -OutFile $bmp
    $img = [System.Drawing.Image]::FromFile($bmp)
    $img.Save((Join-Path $Out "$($names[$i]).png"), [System.Drawing.Imaging.ImageFormat]::Png)
    $img.Dispose()
    Remove-Item $bmp
}
Invoke-WebRequest -UseBasicParsing -TimeoutSec 15 "http://$Esp/api/debug/tab?i=0" | Out-Null
Get-ChildItem $Out -Filter *.png | Select-Object -ExpandProperty FullName
