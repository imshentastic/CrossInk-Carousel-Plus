#pragma once
#include <HalStorage.h>

#include <memory>
#include <string>

#include "Block.h"

class ImageBlock final : public Block {
 public:
  ImageBlock(const std::string& imagePath, int16_t width, int16_t height);
  ~ImageBlock() override = default;

  const std::string& getImagePath() const { return imagePath; }
  int16_t getWidth() const { return width; }
  int16_t getHeight() const { return height; }

  bool imageExists() const;

  BlockType getType() override { return IMAGE_BLOCK; }
  bool isEmpty() override { return false; }

  void render(GfxRenderer& renderer, const int x, const int y);
  // v18.9.9.57: cache-only render entry for the streamed/BT path. Blits from
  // the .pxc cache if present, otherwise skips entirely (no JPEG-decoder
  // fallback, no markImageRepaintUnsafe). suppressImages() still draws the
  // placeholder rect. Returns true iff something was drawn (cache hit or
  // placeholder); false iff the image was silently skipped.
  bool renderIfCached(GfxRenderer& renderer, const int x, const int y);
  bool serialize(FsFile& file);
  static std::unique_ptr<ImageBlock> deserialize(FsFile& file);

 private:
  std::string imagePath;
  int16_t width;
  int16_t height;
};
