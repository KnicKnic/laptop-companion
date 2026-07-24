#include "SleepImage.h"

#include "AppLog.h"
#include "AppSdBus.h"
#include "Settings.h"

#include <Arduino.h>
#include <SDCardManager.h>
#include <new>
#include <string.h>

namespace {

constexpr uint32_t kBmpCompressionRgb = 0;
constexpr uint32_t kBmpCompressionBitfields = 3;
constexpr bool kUseAtkinsonDither = true;

bool readFully(FsFile& file, uint8_t* buffer, size_t size) {
  return file.read(buffer, size) == static_cast<int>(size);
}

uint16_t le16(const uint8_t* p) {
  return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

uint32_t le32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

int32_t sle32(const uint8_t* p) {
  return static_cast<int32_t>(le32(p));
}

uint8_t scaledMaskedChannel(uint32_t pixel, uint32_t mask) {
  if (mask == 0) return 0;
  uint8_t shift = 0;
  while (((mask >> shift) & 0x01) == 0 && shift < 31) {
    ++shift;
  }
  const uint32_t value = (pixel & mask) >> shift;
  const uint32_t maxValue = mask >> shift;
  return maxValue == 0 ? 0 : static_cast<uint8_t>((value * 255UL) / maxValue);
}

uint8_t luminanceFromRgb(uint8_t r, uint8_t g, uint8_t b) {
  return static_cast<uint8_t>((static_cast<uint16_t>(r) * 77U + static_cast<uint16_t>(g) * 150U +
                               static_cast<uint16_t>(b) * 29U) >>
                              8);
}

uint8_t quantizeSimple(int gray) {
  if (gray < 45) return 0;   // black
  if (gray < 70) return 1;   // dark gray
  if (gray < 140) return 2;  // light gray
  return 3;                        // white
}

class AtkinsonDitherer {
 public:
  explicit AtkinsonDitherer(int width) : width_(width) {
    row0_ = static_cast<int16_t*>(calloc(static_cast<size_t>(width_) + 4, sizeof(int16_t)));
    row1_ = static_cast<int16_t*>(calloc(static_cast<size_t>(width_) + 4, sizeof(int16_t)));
    row2_ = static_cast<int16_t*>(calloc(static_cast<size_t>(width_) + 4, sizeof(int16_t)));
  }

  ~AtkinsonDitherer() {
    free(row0_);
    free(row1_);
    free(row2_);
  }

  bool ready() const { return row0_ != nullptr && row1_ != nullptr && row2_ != nullptr; }

  uint8_t processPixel(int gray, int x) {
    int adjusted = gray + row0_[x + 2];
    if (adjusted < 0) adjusted = 0;
    if (adjusted > 255) adjusted = 255;

    uint8_t quantized = 0;
    int quantizedValue = 15;
    if (adjusted < 30) {
      quantized = 0;
      quantizedValue = 15;
    } else if (adjusted < 50) {
      quantized = 1;
      quantizedValue = 30;
    } else if (adjusted < 140) {
      quantized = 2;
      quantizedValue = 80;
    } else {
      quantized = 3;
      quantizedValue = 210;
    }

    const int error = (adjusted - quantizedValue) >> 3;
    row0_[x + 3] += error;
    row0_[x + 4] += error;
    row1_[x + 1] += error;
    row1_[x + 2] += error;
    row1_[x + 3] += error;
    row2_[x + 2] += error;
    return quantized;
  }

  void nextRow() {
    int16_t* temp = row0_;
    row0_ = row1_;
    row1_ = row2_;
    row2_ = temp;
    memset(row2_, 0, (static_cast<size_t>(width_) + 4) * sizeof(int16_t));
  }

 private:
  int width_ = 0;
  int16_t* row0_ = nullptr;
  int16_t* row1_ = nullptr;
  int16_t* row2_ = nullptr;
};

void setPanelPixel(EInkDisplay& display, int32_t x, int32_t y, bool black) {
  if (x < 0 || y < 0 || x >= display.getDisplayWidth() || y >= display.getDisplayHeight()) return;

  uint8_t* frame = display.getFrameBuffer();
  if (frame == nullptr) return;
  const uint32_t offset = static_cast<uint32_t>(y) * display.getDisplayWidthBytes() + (static_cast<uint32_t>(x) >> 3);
  const uint8_t mask = static_cast<uint8_t>(0x80 >> (x & 7));
  if (black) {
    frame[offset] &= static_cast<uint8_t>(~mask);
  } else {
    frame[offset] |= mask;
  }
}

bool imageToPanel(EInkDisplay& display, int32_t imageX, int32_t imageY, bool portraitToPanel, int32_t& panelX,
                  int32_t& panelY) {
  if (portraitToPanel) {
    panelX = imageY;
    panelY = static_cast<int32_t>(display.getDisplayHeight()) - 1 - imageX;
  } else {
    panelX = imageX;
    panelY = imageY;
  }
  return panelX >= 0 && panelY >= 0 && panelX < display.getDisplayWidth() && panelY < display.getDisplayHeight();
}

void setPlanePixel(EInkDisplay& display, uint8_t* plane, int32_t x, int32_t y, bool setBit) {
  if (plane == nullptr || x < 0 || y < 0 || x >= display.getDisplayWidth() || y >= display.getDisplayHeight()) return;

  const uint32_t offset = static_cast<uint32_t>(y) * display.getDisplayWidthBytes() + (static_cast<uint32_t>(x) >> 3);
  const uint8_t mask = static_cast<uint8_t>(0x80 >> (x & 7));
  if (setBit) {
    plane[offset] |= mask;
  } else {
    plane[offset] &= static_cast<uint8_t>(~mask);
  }
}

bool loadIndexedPalette(FsFile& file, uint32_t paletteOffset, uint32_t colorCount, uint8_t* luminanceByIndex) {
  if (colorCount == 0 || colorCount > 256 || !file.seekSet(paletteOffset)) return false;

  uint8_t bgra[4];
  for (uint32_t i = 0; i < colorCount; ++i) {
    if (!readFully(file, bgra, sizeof(bgra))) return false;
    luminanceByIndex[i] = luminanceFromRgb(bgra[2], bgra[1], bgra[0]);
  }
  return true;
}

uint32_t bmpRowStride(int32_t width, uint16_t bpp) {
  return ((static_cast<uint32_t>(width) * bpp + 31) / 32) * 4;
}

bool paletteIsNativeGrayscale(uint16_t bpp, uint32_t colorCount, const uint8_t* luminanceByIndex) {
  if (bpp <= 2) return true;
  if (colorCount == 0) return false;

  for (uint32_t i = 0; i < colorCount; ++i) {
    const uint8_t lum = luminanceByIndex[i];
    const uint8_t level = lum >> 6;
    const uint8_t reconstructed = static_cast<uint8_t>(level * 85);
    if (lum > reconstructed + 21 || lum + 21 < reconstructed) {
      return false;
    }
  }
  return true;
}

uint8_t rowPixelLuminance(const uint8_t* row, int32_t x, uint16_t bpp, const uint8_t* luminanceByIndex,
                          uint32_t redMask, uint32_t greenMask, uint32_t blueMask, bool bitfields) {
  if (bpp == 1) {
    const uint8_t value = (row[x >> 3] >> (7 - (x & 7))) & 0x01;
    return luminanceByIndex[value];
  }
  if (bpp == 2) {
    const uint8_t value = (row[x >> 2] >> (6 - ((x & 3) * 2))) & 0x03;
    return luminanceByIndex[value];
  }
  if (bpp == 4) {
    const uint8_t packed = row[x >> 1];
    const uint8_t value = (x & 1) == 0 ? (packed >> 4) : (packed & 0x0F);
    return luminanceByIndex[value];
  }
  if (bpp == 8) {
    return luminanceByIndex[row[x]];
  }
  if (bpp == 16) {
    const uint8_t* px = row + static_cast<uint32_t>(x) * 2;
    const uint32_t rgb = static_cast<uint32_t>(px[0]) | (static_cast<uint32_t>(px[1]) << 8);
    return luminanceFromRgb(scaledMaskedChannel(rgb, redMask), scaledMaskedChannel(rgb, greenMask),
                            scaledMaskedChannel(rgb, blueMask));
  }
  if (bpp == 32 && bitfields) {
    const uint8_t* px = row + static_cast<uint32_t>(x) * 4;
    const uint32_t rgb = static_cast<uint32_t>(px[0]) | (static_cast<uint32_t>(px[1]) << 8) |
                         (static_cast<uint32_t>(px[2]) << 16) | (static_cast<uint32_t>(px[3]) << 24);
    return luminanceFromRgb(scaledMaskedChannel(rgb, redMask), scaledMaskedChannel(rgb, greenMask),
                            scaledMaskedChannel(rgb, blueMask));
  }
  if (bpp == 24) {
    const uint8_t* bgr = row + static_cast<uint32_t>(x) * 3;
    return luminanceFromRgb(bgr[2], bgr[1], bgr[0]);
  }

  const uint8_t* bgra = row + static_cast<uint32_t>(x) * 4;
  return luminanceFromRgb(bgra[2], bgra[1], bgra[0]);
}

uint8_t packed2BitPixel(const uint8_t* row, int32_t x) {
  return (row[x >> 2] >> (6 - ((x & 3) * 2))) & 0x03;
}

bool readPacked2BitRow(FsFile& file, uint8_t* sourceRow, uint8_t* packedRow, uint32_t sourceRowBytes, int32_t width,
                       uint16_t bpp, const uint8_t* luminanceByIndex, uint32_t redMask, uint32_t greenMask,
                       uint32_t blueMask, bool bitfields, bool nativePalette, AtkinsonDitherer* ditherer) {
  if (!readFully(file, sourceRow, sourceRowBytes)) return false;

  memset(packedRow, 0, (static_cast<uint32_t>(width) + 3) / 4);
  for (int32_t x = 0; x < width; ++x) {
    const uint8_t luminance =
        rowPixelLuminance(sourceRow, x, bpp, luminanceByIndex, redMask, greenMask, blueMask, bitfields);
    uint8_t level = 0;
    if (nativePalette) {
      level = luminance >> 6;
    } else if (ditherer != nullptr) {
      level = ditherer->processPixel(luminance, x);
    } else {
      level = quantizeSimple(luminance);
    }

    packedRow[x >> 2] |= static_cast<uint8_t>((level & 0x03) << (6 - ((x & 3) * 2)));
  }

  if (ditherer != nullptr) ditherer->nextRow();
  return true;
}

enum class SleepBmpPass : uint8_t { Base, GrayscaleLsb, GrayscaleMsb };

bool renderBmpPass(FsFile& file, EInkDisplay& display, SleepBmpPass pass, uint32_t pixelOffset, int32_t width,
                   int32_t height, bool topDown, bool portraitToPanel, uint16_t bpp,
                   const uint8_t* luminanceByIndex, uint32_t redMask, uint32_t greenMask, uint32_t blueMask,
                   bool bitfields, bool nativePalette, uint32_t* levelCounts, bool* hasGrayscale) {
  const uint32_t rowStride = bmpRowStride(width, bpp);
  const uint32_t packedRowBytes = (static_cast<uint32_t>(width) + 3) / 4;
  uint8_t* sourceRow = static_cast<uint8_t*>(malloc(rowStride));
  uint8_t* packedRow = static_cast<uint8_t*>(malloc(packedRowBytes));
  AtkinsonDitherer* ditherer = nullptr;
  if (!nativePalette && kUseAtkinsonDither) {
    ditherer = new (std::nothrow) AtkinsonDitherer(width);
    if (ditherer != nullptr && !ditherer->ready()) {
      delete ditherer;
      ditherer = nullptr;
    }
  }

  if (sourceRow == nullptr || packedRow == nullptr || (!nativePalette && kUseAtkinsonDither && ditherer == nullptr)) {
    logPrintf("Sleep BMP rejected: cannot allocate row conversion buffers (%lu + %lu bytes).\n",
              static_cast<unsigned long>(rowStride), static_cast<unsigned long>(packedRowBytes));
    free(sourceRow);
    free(packedRow);
    delete ditherer;
    return false;
  }

  if (!file.seekSet(pixelOffset)) {
    logPrintf("Sleep BMP rejected: cannot seek to pixel data at offset %lu.\n", static_cast<unsigned long>(pixelOffset));
    free(sourceRow);
    free(packedRow);
    delete ditherer;
    return false;
  }

  display.clearScreen(pass == SleepBmpPass::Base ? 0xFF : 0x00);
  for (int32_t bmpY = 0; bmpY < height; ++bmpY) {
    if (!readPacked2BitRow(file, sourceRow, packedRow, rowStride, width, bpp, luminanceByIndex, redMask, greenMask,
                           blueMask, bitfields, nativePalette, ditherer)) {
      logPrintf("Sleep BMP rejected: row %ld unreadable.\n", static_cast<long>(bmpY));
      free(sourceRow);
      free(packedRow);
      delete ditherer;
      return false;
    }

    const int32_t imageY = topDown ? bmpY : (height - 1 - bmpY);
    for (int32_t x = 0; x < width; ++x) {
      const uint8_t level = packed2BitPixel(packedRow, x);
      if (levelCounts != nullptr && level < 4) ++levelCounts[level];
      if (hasGrayscale != nullptr && (level == 1 || level == 2)) *hasGrayscale = true;

      int32_t panelX = 0;
      int32_t panelY = 0;
      if (!imageToPanel(display, x, imageY, portraitToPanel, panelX, panelY)) continue;

      if (pass == SleepBmpPass::Base) {
        setPanelPixel(display, panelX, panelY, level < 3);
      } else {
        const bool setBit = pass == SleepBmpPass::GrayscaleMsb ? (level == 1 || level == 2) : (level == 1);
        if (setBit) {
          setPlanePixel(display, display.getFrameBuffer(), panelX, panelY, true);
        }
      }
    }
  }

  free(sourceRow);
  free(packedRow);
  delete ditherer;
  return true;
}

bool drawBmp(FsFile& file, EInkDisplay& display, bool* alreadyPresented) {
  if (alreadyPresented != nullptr) *alreadyPresented = false;

  uint8_t fileHeader[14];
  uint8_t dibHeader[40];
  if (!readFully(file, fileHeader, sizeof(fileHeader))) {
    logPrintf("Sleep BMP header read failed.\n");
    return false;
  }
  if (fileHeader[0] != 'B' || fileHeader[1] != 'M') {
    logPrintf("Sleep BMP signature invalid: 0x%02X 0x%02X.\n", fileHeader[0], fileHeader[1]);
    return false;
  }

  const uint32_t pixelOffset = le32(fileHeader + 10);
  if (!readFully(file, dibHeader, sizeof(dibHeader))) {
    logPrintf("Sleep BMP DIB header read failed.\n");
    return false;
  }

  const uint32_t dibSize = le32(dibHeader);
  const int32_t width = sle32(dibHeader + 4);
  const int32_t signedHeight = sle32(dibHeader + 8);
  const uint16_t planes = le16(dibHeader + 12);
  const uint16_t bpp = le16(dibHeader + 14);
  const uint32_t compression = le32(dibHeader + 16);
  const uint32_t colorsUsed = le32(dibHeader + 32);
  const bool topDown = signedHeight < 0;
  const int32_t height = topDown ? -signedHeight : signedHeight;

  logPrintf("Sleep BMP header: dib=%lu offset=%lu size=%ldx%ld bpp=%u compression=%lu colors=%lu file=%lu B.\n",
            static_cast<unsigned long>(dibSize), static_cast<unsigned long>(pixelOffset), static_cast<long>(width),
            static_cast<long>(height), bpp, static_cast<unsigned long>(compression),
            static_cast<unsigned long>(colorsUsed), static_cast<unsigned long>(file.fileSize()));

  if (dibSize < 40 || width <= 0 || height <= 0 || planes != 1) {
    logPrintf("Sleep BMP rejected: invalid DIB fields.\n");
    return false;
  }
  const bool bitfields = compression == kBmpCompressionBitfields && (bpp == 16 || bpp == 32);
  if (compression != kBmpCompressionRgb && !bitfields) {
    logPrintf("Sleep BMP rejected: unsupported compression %lu.\n", static_cast<unsigned long>(compression));
    return false;
  }
  const bool exactPanelSize = width == display.getDisplayWidth() && height == display.getDisplayHeight();
  const bool portraitPanelSize = width == display.getDisplayHeight() && height == display.getDisplayWidth();
  if (!exactPanelSize && !portraitPanelSize) {
    logPrintf("Sleep BMP size %ldx%ld does not match display %ux%u.\n", static_cast<long>(width),
              static_cast<long>(height), display.getDisplayWidth(), display.getDisplayHeight());
    return false;
  }
  if (portraitPanelSize) {
    logPrintf("Sleep BMP is portrait; rotating into panel framebuffer.\n");
  }
  if (bpp != 1 && bpp != 2 && bpp != 4 && bpp != 8 && bpp != 16 && bpp != 24 && bpp != 32) {
    logPrintf("Sleep BMP rejected: unsupported bpp %u.\n", bpp);
    return false;
  }

  uint32_t redMask = bpp == 16 ? 0xF800 : 0x00FF0000;
  uint32_t greenMask = bpp == 16 ? 0x07E0 : 0x0000FF00;
  uint32_t blueMask = bpp == 16 ? 0x001F : 0x000000FF;
  if (bitfields) {
    uint8_t masks[12];
    if (!file.seekSet(14 + 40) || !readFully(file, masks, sizeof(masks))) {
      logPrintf("Sleep BMP rejected: bitfield masks unreadable.\n");
      return false;
    }
    redMask = le32(masks);
    greenMask = le32(masks + 4);
    blueMask = le32(masks + 8);
    logPrintf("Sleep BMP masks: R=0x%08lX G=0x%08lX B=0x%08lX.\n", static_cast<unsigned long>(redMask),
              static_cast<unsigned long>(greenMask), static_cast<unsigned long>(blueMask));
  }

  uint8_t luminanceByIndex[256] = {};
  uint32_t colorCount = 0;
  if (bpp <= 8) {
    colorCount = colorsUsed != 0 ? colorsUsed : (1UL << bpp);
    if (!loadIndexedPalette(file, 14 + dibSize, colorCount, luminanceByIndex)) {
      logPrintf("Sleep BMP rejected: palette unreadable, colors=%lu.\n", static_cast<unsigned long>(colorCount));
      return false;
    }
  }

  const bool nativePalette = bpp <= 8 && paletteIsNativeGrayscale(bpp, colorCount, luminanceByIndex);
  const bool grayCapable = display.supportsStripGrayscale();
  bool hasGrayscale = false;
  uint32_t levelCounts[4] = {};
  if (!renderBmpPass(file, display, SleepBmpPass::Base, pixelOffset, width, height, topDown, portraitPanelSize, bpp,
                     luminanceByIndex, redMask, greenMask, blueMask, bitfields, nativePalette, levelCounts,
                     &hasGrayscale)) {
    return false;
  }

  logPrintf("Sleep BMP levels: black=%lu dark=%lu light=%lu white=%lu grayCapable=%d nativePalette=%d dither=%d.\n",
            static_cast<unsigned long>(levelCounts[0]), static_cast<unsigned long>(levelCounts[1]),
            static_cast<unsigned long>(levelCounts[2]), static_cast<unsigned long>(levelCounts[3]),
            static_cast<int>(grayCapable), static_cast<int>(nativePalette),
            static_cast<int>(!nativePalette && kUseAtkinsonDither));

  if (hasGrayscale && grayCapable) {
    logPrintf("Sleep BMP contains grayscale; using grayscale display pipeline.\n");
    display.displayGrayscaleBase(EInkDisplay::HALF_REFRESH);
    if (!renderBmpPass(file, display, SleepBmpPass::GrayscaleLsb, pixelOffset, width, height, topDown,
                       portraitPanelSize, bpp, luminanceByIndex, redMask, greenMask, blueMask, bitfields,
                       nativePalette, nullptr, nullptr)) {
      if (alreadyPresented != nullptr) *alreadyPresented = true;
      return true;
    }
    display.copyGrayscaleLsbBuffers(display.getFrameBuffer());
    if (!renderBmpPass(file, display, SleepBmpPass::GrayscaleMsb, pixelOffset, width, height, topDown,
                       portraitPanelSize, bpp, luminanceByIndex, redMask, greenMask, blueMask, bitfields,
                       nativePalette, nullptr, nullptr)) {
      if (alreadyPresented != nullptr) *alreadyPresented = true;
      return true;
    }
    display.copyGrayscaleMsbBuffers(display.getFrameBuffer());
    display.displayGrayBuffer(false);
    if (alreadyPresented != nullptr) *alreadyPresented = true;
  }
  return true;
}

}  // namespace

bool drawSleepImageFromSd(EInkDisplay& display, bool* alreadyPresented) {
  if (alreadyPresented != nullptr) *alreadyPresented = false;
  if (!SdMan.ready()) {
    logPrintf("Sleep BMP skipped: SD is not ready.\n");
    return false;
  }
  configureSdSpiForActiveBoard();

  const CompanionSettings settings = copySettings();
  const String imagePath = settings.system.sleep.imagePath;
  FsFile file = SdMan.open(imagePath.c_str(), O_RDONLY);
  if (!file) {
    logPrintf("Sleep BMP not found: %s\n", imagePath.c_str());
    return false;
  }

  const bool ok = drawBmp(file, display, alreadyPresented);
  file.close();
  logPrintf("Sleep BMP %s: %s\n", imagePath.c_str(), ok ? "loaded" : "unsupported or unreadable");
  return ok;
}
