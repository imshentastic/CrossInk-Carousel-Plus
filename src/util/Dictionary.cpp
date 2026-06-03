#include "Dictionary.h"

#include <HalStorage.h>

#include <algorithm>
#include <cctype>
#include <cstring>

// Standard StarDict file paths on the SD root
static const char* DICT_FILE = "/dictionary.dict";
static const char* IDX_FILE = "/dictionary.idx";
static const char* CACHE_FILE = "/.crosspoint/dict_idx.cache";

// Static member initialization
std::vector<uint32_t> Dictionary::sparseOffsets;
uint32_t Dictionary::totalWords = 0;
bool Dictionary::indexLoaded = false;

// Helper to convert Big-Endian uint32 to ESP32 Little-Endian
static uint32_t swap32(uint32_t val) {
  return ((val << 24) & 0xFF000000) | ((val << 8) & 0x00FF0000) | ((val >> 8) & 0x0000FF00) |
         ((val >> 24) & 0x000000FF);
}

bool Dictionary::exists() { return Storage.exists(DICT_FILE) && Storage.exists(IDX_FILE); }

std::string Dictionary::cleanWord(const std::string& word) {
  std::string clean;
  for (char c : word) {
    if (std::isalpha(c)) {
      clean += std::tolower(c);
    }
  }
  return clean;
}

std::vector<std::string> Dictionary::getStemVariants(const std::string& word) {
  std::vector<std::string> variants;
  if (word.length() < 4) return variants;

  // Simple English stemming heuristics
  if (word.back() == 's') {
    variants.push_back(word.substr(0, word.length() - 1));

    if (word.substr(word.length() - 2) == "es") {
      variants.push_back(word.substr(0, word.length() - 2));
    }
  }
  if (word.substr(word.length() - 2) == "ed") {
    variants.push_back(word.substr(0, word.length() - 1));  // e.g., baked -> bake
    variants.push_back(word.substr(0, word.length() - 2));  // e.g., started -> start
  }
  if (word.substr(word.length() - 3) == "ing") {
    variants.push_back(word.substr(0, word.length() - 3));        // e.g., playing -> play
    variants.push_back(word.substr(0, word.length() - 3) + "e");  // e.g., making -> make
  }
  return variants;
}

bool Dictionary::loadCachedIndex() {
  FsFile f;
  if (!Storage.openFileForRead("DICT", CACHE_FILE, f)) {
    return false;
  }

  uint32_t count = 0;
  if (f.read(reinterpret_cast<uint8_t*>(&count), sizeof(count)) != sizeof(count)) {
    f.close();
    return false;
  }

  totalWords = count;
  uint32_t sparseSize = (count + SPARSE_INTERVAL - 1) / SPARSE_INTERVAL;
  sparseOffsets.resize(sparseSize);

  if (f.read(reinterpret_cast<uint8_t*>(sparseOffsets.data()), sparseSize * sizeof(uint32_t)) !=
      sparseSize * sizeof(uint32_t)) {
    sparseOffsets.clear();
    f.close();
    return false;
  }

  f.close();
  indexLoaded = true;
  return true;
}

void Dictionary::saveCachedIndex() {
  Storage.mkdir("/.crosspoint");
  FsFile f;
  if (Storage.openFileForWrite("DICT", CACHE_FILE, f)) {
    f.write(reinterpret_cast<const uint8_t*>(&totalWords), sizeof(totalWords));
    f.write(reinterpret_cast<const uint8_t*>(sparseOffsets.data()), sparseOffsets.size() * sizeof(uint32_t));
    f.close();
  }
}

std::string Dictionary::readWord(FsFile& file) {
  std::string word;
  char c;
  while (file.read(reinterpret_cast<uint8_t*>(&c), 1) == 1 && c != '\0') {
    word += c;
  }
  return word;
}

bool Dictionary::loadIndex(const std::function<void(int percent)>& onProgress,
                           const std::function<bool()>& shouldCancel) {
  if (loadCachedIndex()) return true;

  FsFile idxFile;
  if (!Storage.openFileForRead("DICT", IDX_FILE, idxFile)) return false;

  uint32_t fileSize = idxFile.size();
  uint32_t currentOffset = 0;
  totalWords = 0;
  sparseOffsets.clear();

  while (currentOffset < fileSize) {
    if (shouldCancel && shouldCancel()) {
      idxFile.close();
      return false;
    }

    if (totalWords % SPARSE_INTERVAL == 0) {
      sparseOffsets.push_back(currentOffset);
      if (onProgress) {
        onProgress((currentOffset * 100) / fileSize);
      }
    }

    // Read word string
    char c;
    while (idxFile.read(reinterpret_cast<uint8_t*>(&c), 1) == 1 && c != '\0') {
      currentOffset++;
    }
    currentOffset++;  // For the '\0'

    // Skip offset and size (2 * 4 bytes)
    idxFile.seek(currentOffset + 8);
    currentOffset += 8;
    totalWords++;
  }

  idxFile.close();
  indexLoaded = true;
  saveCachedIndex();

  if (onProgress) onProgress(100);
  return true;
}

std::string Dictionary::readDefinition(uint32_t offset, uint32_t size) {
  FsFile dictFile;
  if (!Storage.openFileForRead("DICT", DICT_FILE, dictFile)) return "";

  dictFile.seek(offset);
  std::string definition;
  definition.resize(size);

  if (dictFile.read(reinterpret_cast<uint8_t*>(definition.data()), size) == size) {
    dictFile.close();
    return definition;
  }

  dictFile.close();
  return "";
}

std::string Dictionary::lookup(const std::string& rawWord, const std::function<void(int percent)>& onProgress,
                               const std::function<bool()>& shouldCancel) {
  if (!exists()) return "";
  if (!indexLoaded && !loadIndex(onProgress, shouldCancel)) return "";

  std::string targetWord = cleanWord(rawWord);
  if (targetWord.empty()) return "";

  FsFile idxFile;
  if (!Storage.openFileForRead("DICT", IDX_FILE, idxFile)) return "";

  // Binary search over the sparse index
  int low = 0;
  int high = sparseOffsets.size() - 1;
  int closestSparseIdx = 0;

  while (low <= high) {
    int mid = low + (high - low) / 2;
    idxFile.seek(sparseOffsets[mid]);
    std::string currentWord = cleanWord(readWord(idxFile));

    if (currentWord == targetWord) {
      closestSparseIdx = mid;
      break;
    } else if (currentWord < targetWord) {
      closestSparseIdx = mid;  // Might be in this chunk
      low = mid + 1;
    } else {
      high = mid - 1;
    }
  }

  // Linear scan within the identified chunk
  idxFile.seek(sparseOffsets[closestSparseIdx]);
  uint32_t chunkEnd = (closestSparseIdx + 1 < static_cast<int>(sparseOffsets.size()))
                          ? sparseOffsets[closestSparseIdx + 1]
                          : idxFile.size();

  while (idxFile.position() < chunkEnd) {
    if (shouldCancel && shouldCancel()) break;

    std::string word = readWord(idxFile);
    std::string clean = cleanWord(word);

    uint32_t dataOffset, dataSize;
    idxFile.read(reinterpret_cast<uint8_t*>(&dataOffset), sizeof(uint32_t));
    idxFile.read(reinterpret_cast<uint8_t*>(&dataSize), sizeof(uint32_t));

    // Convert from Big-Endian to Little-Endian
    dataOffset = swap32(dataOffset);
    dataSize = swap32(dataSize);

    if (clean == targetWord) {
      idxFile.close();
      return readDefinition(dataOffset, dataSize);
    }
  }

  idxFile.close();
  return "";
}

int Dictionary::editDistance(const std::string& a, const std::string& b, int maxDist) {
  if (a.empty()) return b.length();
  if (b.empty()) return a.length();

  std::vector<int> v0(b.length() + 1);
  std::vector<int> v1(b.length() + 1);

  for (size_t i = 0; i <= b.length(); i++) v0[i] = i;

  for (size_t i = 0; i < a.length(); i++) {
    v1[0] = i + 1;
    int minRowDist = v1[0];

    for (size_t j = 0; j < b.length(); j++) {
      int cost = (a[i] == b[j]) ? 0 : 1;
      v1[j + 1] = std::min({v1[j] + 1, v0[j + 1] + 1, v0[j] + cost});
      minRowDist = std::min(minRowDist, v1[j + 1]);
    }
    v0 = v1;
    if (minRowDist > maxDist) return maxDist + 1;  // Early exit
  }
  return v0[b.length()];
}

std::vector<std::string> Dictionary::findSimilar(const std::string& rawWord, int maxResults) {
  std::vector<std::string> results;
  if (!indexLoaded || rawWord.empty()) return results;

  std::string targetWord = cleanWord(rawWord);
  FsFile idxFile;
  if (!Storage.openFileForRead("DICT", IDX_FILE, idxFile)) return results;

  struct Suggestion {
    std::string word;
    int distance;
    bool operator<(const Suggestion& other) const { return distance < other.distance; }
  };
  std::vector<Suggestion> candidates;

  // We limit the search to a few chunks around the estimated position to avoid freezing
  // For a complete implementation, a more complex heuristic is needed.
  // For now, we scan 2000 words starting from the closest sparse index block.

  int low = 0;
  int high = sparseOffsets.size() - 1;
  int closestSparseIdx = 0;

  while (low <= high) {
    int mid = low + (high - low) / 2;
    idxFile.seek(sparseOffsets[mid]);
    std::string currentWord = cleanWord(readWord(idxFile));

    if (currentWord == targetWord) {
      closestSparseIdx = mid;
      break;
    } else if (currentWord < targetWord) {
      closestSparseIdx = mid;
      low = mid + 1;
    } else {
      high = mid - 1;
    }
  }

  // Scan backwards slightly and forwards to find similar prefixes
  int startChunk = std::max(0, closestSparseIdx - 2);
  idxFile.seek(sparseOffsets[startChunk]);

  int wordsScanned = 0;
  while (idxFile.available() && wordsScanned < 3000) {
    std::string word = readWord(idxFile);
    idxFile.seek(idxFile.position() + 8);  // Skip offset and size
    wordsScanned++;

    std::string clean = cleanWord(word);
    // Ignore completely unrelated words (first letter must match to save CPU)
    if (clean.empty() || clean[0] != targetWord[0]) continue;

    int dist = editDistance(targetWord, clean, 3);
    if (dist <= 3) {
      candidates.push_back({word, dist});
    }
  }
  idxFile.close();

  std::sort(candidates.begin(), candidates.end());

  for (const auto& c : candidates) {
    if (results.size() >= maxResults) break;
    if (std::find(results.begin(), results.end(), c.word) == results.end()) {
      results.push_back(c.word);
    }
  }

  return results;
}

void Dictionary::freeMemory() {
  sparseOffsets.clear();
  sparseOffsets.shrink_to_fit();
  indexLoaded = false;
  totalWords = 0;
}
