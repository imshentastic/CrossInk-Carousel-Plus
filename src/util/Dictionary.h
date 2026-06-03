#pragma once

#include <HalStorage.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

class Dictionary {
 public:
  // Checks if the required StarDict files exist on the SD card
  static bool exists();

  // Looks up a word and returns its definition. Supports progress callbacks.
  static std::string lookup(const std::string& word, const std::function<void(int percent)>& onProgress = nullptr,
                            const std::function<bool()>& shouldCancel = nullptr);

  // Removes punctuation and converts to lowercase
  static std::string cleanWord(const std::string& word);

  // Generates basic English stem variants (e.g., "running" -> "run")
  static std::vector<std::string> getStemVariants(const std::string& word);

  // Finds similar words using Levenshtein distance (Did you mean?)
  static std::vector<std::string> findSimilar(const std::string& word, int maxResults);

  static void freeMemory();

 private:
  static constexpr int SPARSE_INTERVAL = 512;
  static std::vector<uint32_t> sparseOffsets;
  static uint32_t totalWords;
  static bool indexLoaded;

  static bool loadIndex(const std::function<void(int percent)>& onProgress, const std::function<bool()>& shouldCancel);
  static bool loadCachedIndex();
  static void saveCachedIndex();

  static std::string readWord(FsFile& file);
  static std::string readDefinition(uint32_t offset, uint32_t size);
  static int editDistance(const std::string& a, const std::string& b, int maxDist);
};