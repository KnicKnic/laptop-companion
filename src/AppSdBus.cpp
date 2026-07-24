#include "AppSdBus.h"

#include "AppLog.h"

#include <Arduino.h>
#include <BoardConfig.h>
#include <SDCardManager.h>
#include <SPI.h>

bool configureSdSpiForActiveBoard() {
  const auto& profile = BoardConfig::ACTIVE;
  const int8_t sclk = profile.sd.sclk >= 0 ? profile.sd.sclk
                                           : (profile.sd.separateSpi ? -1 : profile.display.sclk);
  const int8_t mosi = profile.sd.mosi >= 0 ? profile.sd.mosi
                                           : (profile.sd.separateSpi ? -1 : profile.display.mosi);
  const int8_t miso = profile.sd.miso;
  const int8_t sdCs = profile.sd.cs;

  if (sdCs >= 0) {
    pinMode(sdCs, OUTPUT);
    digitalWrite(sdCs, HIGH);
  }
  if (!profile.sd.separateSpi && profile.display.cs >= 0) {
    pinMode(profile.display.cs, OUTPUT);
    digitalWrite(profile.display.cs, HIGH);
  }
  if (sclk < 0 || mosi < 0 || miso < 0 || sdCs < 0) {
    return false;
  }

  SPI.begin(sclk, miso, mosi, sdCs);
  return true;
}

void dumpSdDirectory(const char* path) {
  if (!SdMan.ready()) {
    logPrintf("[SD] directory listing skipped for %s: SD is not ready.\n", path);
    return;
  }

  FsFile root = SdMan.open(path, O_RDONLY);
  if (!root) {
    logPrintf("[SD] directory listing failed for %s: open failed.\n", path);
    return;
  }
  if (!root.isDirectory()) {
    logPrintf("[SD] directory listing failed for %s: not a directory.\n", path);
    root.close();
    return;
  }

  logPrintf("[SD] listing %s\n", path);
  uint16_t count = 0;
  for (FsFile entry = root.openNextFile(); entry; entry = root.openNextFile()) {
    char name[96] = {};
    entry.getName(name, sizeof(name));
    if (entry.isDirectory()) {
      logPrintf("[SD]   <DIR> %s\n", name);
    } else {
      logPrintf("[SD]   %lu B %s\n", static_cast<unsigned long>(entry.fileSize()), name);
    }
    entry.close();
    ++count;
  }
  logPrintf("[SD] listing %s complete: %u entries\n", path, count);
  root.close();
}
