# ESP32 — Elecrow CrowPanel Advance 4.3"

PlatformIO/Arduino-project voor het **Elecrow CrowPanel Advance 4.3"** (ESP32-S3, 800×480 IPS, touch).

## Hardware

| | |
|---|---|
| Board | ESP32 Display-4.3 inch (Advance series), SKU **DIS01943A**, hardware **V1.0** |
| Module | ESP32-S3-WROOM-1, chip rev v0.2 |
| Flash / PSRAM | 16 MB quad flash / 8 MB octal PSRAM (uitgelezen met esptool) |
| Display / touch | 800×480 IPS, ST7265 / GT911 |
| USB | USB-C "USB-5V-IN" via CH340K (WCH-driver), verschijnt als COM-poort |

Hardware V1.0 gebruikt andere backlight-aansturing dan V1.1+; gebruik alleen
Elecrow-code uit de **V1.0**-mappen van
[Elecrow-RD/CrowPanel-Advance-4.3-HMI-ESP32-S3-AI-Powered-IPS-Touch-Screen-800x480](https://github.com/Elecrow-RD/CrowPanel-Advance-4.3-HMI-ESP32-S3-AI-Powered-IPS-Touch-Screen-800x480).

## Omgevingen

| Env | Inhoud |
|---|---|
| `bringup` (default) | Seriële check van chip/flash/PSRAM, geen display |
| `elecrow_lesson03` | Elecrow's V1.0-voorbeeld lesson-03 (LVGL 8, LovyanGFX, GT911-touch, backlight via I/O-expander op 0x18) |

Afwijkingen van Elecrow's V1.0-setup staan als commentaar in `platformio.ini` en in de code:
LovyanGFX 1.2.25 i.p.v. 1.1.16 (core 3.3.x), en tegen flikkering pixelklok 16 MHz en
LVGL-buffers van 40 regels in intern RAM, beide overgenomen uit Elecrow's V1.1–V1.3-code.

## Bouwen en uploaden

```
pio run -e elecrow_lesson03 -t upload
pio device monitor
```

`build_dir` staat lokaal (`%LOCALAPPDATA%\pio-build\ESP32`) omdat de repo op een netwerkschijf staat.
Pas `upload_port`/`monitor_port` in `platformio.ini` aan als het board een ander COM-nummer krijgt.

## Fabrieksfirmware terugzetten

Binaries uit `factory_firmware/4.3factory_firmware-V1.0/` van de Elecrow-repo, in `firmware/elecrow_factory_V1.0/`:

```
python -m esptool --chip esp32s3 --port COM3 -b 921600 write_flash \
  0x0 HMI4.3.ino.bootloader.bin 0x8000 HMI4.3.ino.partitions.bin \
  0xe000 boot_app0.bin 0x10000 HMI4.3.ino.bin
```

Een volledige 16 MB-dump van de oorspronkelijke flash staat lokaal in `backup/` (niet in git);
terugzetten met `write_flash 0x0 backup/factory_flash_16MB_34cdb00a64ec.bin`.
