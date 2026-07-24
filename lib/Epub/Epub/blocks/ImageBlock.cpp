#include "ImageBlock.h"

#include <GfxRenderer.h>
#include <Logging.h>
#include <Serialization.h>

#include "Epub/converters/DirectPixelWriter.h"
#include "Epub/converters/ImageDecoderFactory.h"

// Cache file format:
// - uint16_t width
// - uint16_t height
// - uint8_t pixels[...] - 2 bits per pixel, packed (4 pixels per byte), row-major order

ImageBlock::ImageBlock(const std::string& imagePath, int16_t width, int16_t height)
    : imagePath(imagePath), width(width), height(height) {}

bool ImageBlock::imageExists() const { return Storage.exists(imagePath.c_str()); }

namespace {

std::string getCachePath(const std::string& imagePath) {
  // Replace extension with .pxc (pixel cache)
  size_t dotPos = imagePath.rfind('.');
  if (dotPos != std::string::npos) {
    return imagePath.substr(0, dotPos) + ".pxc";
  }
  return imagePath + ".pxc";
}

bool renderFromCache(GfxRenderer& renderer, const std::string& cachePath, int x, int y, int expectedWidth,
                     int expectedHeight) {
  FsFile cacheFile;
  if (!Storage.openFileForRead("IMG", cachePath, cacheFile)) {
    return false;
  }

  uint16_t cachedWidth, cachedHeight;
  if (cacheFile.read(&cachedWidth, 2) != 2 || cacheFile.read(&cachedHeight, 2) != 2) {
    cacheFile.close();
    return false;
  }

  // Verify dimensions are close (allow 1 pixel tolerance for rounding differences)
  int widthDiff = abs(cachedWidth - expectedWidth);
  int heightDiff = abs(cachedHeight - expectedHeight);
  if (widthDiff > 1 || heightDiff > 1) {
    LOG_ERR("IMG", "Cache dimension mismatch: %dx%d vs %dx%d", cachedWidth, cachedHeight, expectedWidth,
            expectedHeight);
    cacheFile.close();
    return false;
  }

  // Use cached dimensions for rendering (they're the actual decoded size)
  expectedWidth = cachedWidth;
  expectedHeight = cachedHeight;

  LOG_DBG("IMG", "Loading from cache: %s (%dx%d)", cachePath.c_str(), cachedWidth, cachedHeight);

  const int screenWidth = renderer.getScreenWidth();
  const int screenHeight = renderer.getScreenHeight();
  int clipXStart = 0;
  int clipYStart = 0;
  int clipXEnd = cachedWidth;
  int clipYEnd = cachedHeight;
  if (x < 0) clipXStart = -x;
  if (y < 0) clipYStart = -y;
  if (screenWidth - x < clipXEnd) clipXEnd = screenWidth - x;
  if (screenHeight - y < clipYEnd) clipYEnd = screenHeight - y;

  if (clipXStart >= clipXEnd || clipYStart >= clipYEnd) {
    LOG_DBG("IMG", "Cached image is outside screen after clipping");
    cacheFile.close();
    return true;
  }

  // Read and render row by row to minimize memory usage
  const int bytesPerRow = (cachedWidth + 3) / 4;  // 2 bits per pixel, 4 pixels per byte
  uint8_t* rowBuffer = (uint8_t*)malloc(bytesPerRow);
  if (!rowBuffer) {
    // v18.9.9.60: bump to visible level. This path was silently skipping
    // images under tight BT heap because bytesPerRow (e.g. 117 B for a
    // 464-wide image) can't fit into a fragmented maxAlloc. Renders as
    // "images disappeared" from the user's perspective with no clue why.
    LOG_ERR("IMG", "Failed to allocate row buffer: needed=%d bytes for %dx%d image at %s",
            bytesPerRow, static_cast<int>(expectedWidth), static_cast<int>(expectedHeight), cachePath.c_str());
    cacheFile.close();
    return false;
  }

  DirectPixelWriter pw;
  pw.init(renderer);

  for (int row = 0; row < cachedHeight; row++) {
    if (cacheFile.read(rowBuffer, bytesPerRow) != bytesPerRow) {
      LOG_ERR("IMG", "Cache read error at row %d", row);
      free(rowBuffer);
      cacheFile.close();
      return false;
    }

    if (row < clipYStart) continue;
    if (row >= clipYEnd) break;

    const int destY = y + row;
    pw.beginRow(destY);
    for (int col = clipXStart; col < clipXEnd; col++) {
      const int byteIdx = col >> 2;            // col / 4
      const int bitShift = 6 - (col & 3) * 2;  // MSB first within byte
      uint8_t pixelValue = (rowBuffer[byteIdx] >> bitShift) & 0x03;

      pw.writePixel(x + col, pixelValue);
    }
  }

  free(rowBuffer);
  cacheFile.close();
  LOG_DBG("IMG", "Cache render complete");
  return true;
}

}  // namespace

void ImageBlock::render(GfxRenderer& renderer, const int x, const int y) {
  LOG_DBG("IMG", "Rendering image at %d,%d: %s (%dx%d)", x, y, imagePath.c_str(), width, height);

  const int screenWidth = renderer.getScreenWidth();
  const int screenHeight = renderer.getScreenHeight();

  if (width <= 0 || height <= 0) {
    LOG_ERR("IMG", "Invalid image size: %dx%d", width, height);
    return;
  }

  // Reject only fully off-screen images. Decoders and cache rendering clip
  // partially visible images to the logical screen bounds.
  if (x >= screenWidth || y >= screenHeight || x + width <= 0 || y + height <= 0) {
    LOG_ERR("IMG", "Invalid render position: (%d,%d) size (%dx%d) screen (%dx%d)", x, y, width, height, screenWidth,
            screenHeight);
    return;
  }
  const bool fullyOnScreen = x >= 0 && y >= 0 && x + width <= screenWidth && y + height <= screenHeight;

  // BT No Images Quick Connect: when image rendering is suppressed (so a BLE
  // remote keeps the full contiguous heap), skip the decode entirely and draw a
  // lightweight placeholder border instead, so the page still shows where an
  // image belongs. Crucially we do NOT markRenderStarved here -- the page is
  // considered fully rendered, so the reader's auto-drop logic won't tear down
  // Bluetooth over an image it was deliberately told to skip.
  if (renderer.suppressImages()) {
    LOG_DBG("IMG", "Image suppressed (BT no-images): placeholder at %d,%d (%dx%d)", x, y, width, height);
    renderer.drawRect(x, y, width, height, true);
    return;
  }

  // Try to render from cache first
  std::string cachePath = getCachePath(imagePath);
  if (renderFromCache(renderer, cachePath, x, y, width, height)) {
    return;  // Successfully rendered from cache
  }

  // No cache - need to decode the image
  // Check if image file exists
  FsFile file;
  if (!Storage.openFileForRead("IMG", imagePath, file)) {
    LOG_ERR("IMG", "Image file not found: %s", imagePath.c_str());
    return;
  }
  size_t fileSize = file.size();
  file.close();

  if (fileSize == 0) {
    LOG_ERR("IMG", "Image file is empty: %s", imagePath.c_str());
    return;
  }

  LOG_DBG("IMG", "Decoding and caching: %s", imagePath.c_str());

  RenderConfig config;
  config.x = x;
  config.y = y;
  config.maxWidth = width;
  config.maxHeight = height;
  config.useGrayscale = true;
  config.useDithering = true;
  config.performanceMode = false;
  config.useExactDimensions = true;  // Use pre-calculated dimensions to avoid rounding mismatches
  if (fullyOnScreen) {
    config.cachePath = cachePath;  // Enable caching during decode
  } else {
    // Partial/off-screen image: we decode but do NOT write a .pxc, so this page
    // would have to decode again on the next repaint. Flag it so the reader won't
    // bring a BLE remote back up over a page that isn't decoder-free to redraw.
    renderer.markImageRepaintUnsafe();
  }

  ImageToFramebufferDecoder* decoder = ImageDecoderFactory::getDecoder(imagePath);
  if (!decoder) {
    LOG_ERR("IMG", "No decoder found for image: %s", imagePath.c_str());
    return;
  }

  LOG_DBG("IMG", "Using %s decoder", decoder->getFormatName());

  bool success = decoder->decodeToFramebuffer(imagePath, renderer, config);
  if (!success) {
    LOG_ERR("IMG", "Failed to decode image: %s", imagePath.c_str());
    // Signal render starvation so the reader can drop Bluetooth for this book
    // (image decode needs a large contiguous block NimBLE's ~58 KB denies).
    renderer.markRenderStarved();
    return;
  }

  LOG_DBG("IMG", "Decode successful");
}

bool ImageBlock::renderIfCached(GfxRenderer& renderer, const int x, const int y) {
  if (width <= 0 || height <= 0) return false;
  const int screenWidth = renderer.getScreenWidth();
  const int screenHeight = renderer.getScreenHeight();
  if (x >= screenWidth || y >= screenHeight || x + width <= 0 || y + height <= 0) return false;
  if (renderer.suppressImages()) {
    renderer.drawRect(x, y, width, height, true);
    return true;
  }
  return renderFromCache(renderer, getCachePath(imagePath), x, y, width, height);
}

bool ImageBlock::serialize(FsFile& file) {
  return serialization::tryWriteString(file, imagePath) && serialization::tryWritePod(file, width) &&
         serialization::tryWritePod(file, height);
}

std::unique_ptr<ImageBlock> ImageBlock::deserialize(FsFile& file) {
  std::string path;
  if (!serialization::tryReadString(file, path)) {
    LOG_ERR("IMG", "Deserialization failed: could not read image path");
    return nullptr;
  }
  int16_t w, h;
  if (!serialization::tryReadPod(file, w) || !serialization::tryReadPod(file, h)) {
    LOG_ERR("IMG", "Deserialization failed: truncated image metadata");
    return nullptr;
  }

  auto* imageBlock = new (std::nothrow) ImageBlock(path, w, h);
  if (!imageBlock) {
    LOG_ERR("IMG", "Deserialization failed: could not allocate ImageBlock");
    return nullptr;
  }
  return std::unique_ptr<ImageBlock>(imageBlock);
}
