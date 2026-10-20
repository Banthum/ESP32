// Minimale bring-up: bewijst dat de PlatformIO-config (flash/PSRAM/upload/serial)
// klopt. Nog geen display- of touch-code; het scherm blijft daarom donker.
#include <Arduino.h>

void setup() {
  Serial.begin(115200);
  delay(500);

  Serial.println();
  Serial.println("CrowPanel Advance 4.3\" V1.0 - PlatformIO bring-up");
  Serial.printf("Chip:   %s rev %d, %d cores @ %lu MHz\n", ESP.getChipModel(),
                ESP.getChipRevision(), ESP.getChipCores(), ESP.getCpuFreqMHz());
  Serial.printf("Flash:  %lu MB\n", ESP.getFlashChipSize() / (1024 * 1024));
  Serial.printf("PSRAM:  %s, %lu KB totaal, %lu KB vrij\n",
                psramFound() ? "gevonden" : "NIET gevonden",
                ESP.getPsramSize() / 1024, ESP.getFreePsram() / 1024);
  Serial.printf("Heap:   %lu KB vrij\n", ESP.getFreeHeap() / 1024);
}

void loop() {
  static uint32_t n = 0;
  Serial.printf("alive %lu\n", n++);
  delay(1000);
}
