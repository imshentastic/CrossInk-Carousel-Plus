#pragma once

// Section-build profiler. Answers "where do the seconds in a page build
// actually go" with measurements instead of guesses.
//
// Entirely compiled out unless CJK_INDEX_PROFILE is defined, so it costs
// nothing in shipping builds. Enable for a diagnostic binary with:
//   pio run -e tiny-cjk-profile
//
// Buckets NEST -- they are not a partition. Read them as a tree:
//
//   TOTAL                       one parseStep (expat chunk + everything it triggers)
//    +- LAYOUT                  ParsedText::layoutAndExtractLines
//    |   +- MEASURE             calculateWordWidths -> getTextAdvanceX per word
//    |   +- LINEBRK             computeLineBreaks / computeHyphenatedLineBreaks (the DP)
//    |   +- EXTRACT             extractLine -> builds the TextBlock, bakes word x-positions
//    |   \- PAGEOUT             completePageFn -> Section::onPageComplete -> serialize to SD
//    \- IMAGE                   image block decode / measurement during parse
//
// So TOTAL - LAYOUT - IMAGE is expat parsing plus per-chunk overhead, and
// LAYOUT - (MEASURE+LINEBRK+EXTRACT+PAGEOUT) is layout bookkeeping. Whichever
// line dominates is the thing worth optimising.

#ifdef CJK_INDEX_PROFILE

#include <Arduino.h>
#include <Logging.h>

namespace indexProfile {

enum Bucket : uint8_t { TOTAL = 0, LAYOUT, MEASURE, LINEBRK, EXTRACT, PAGEOUT, IMAGE, BUCKET_COUNT };

// Accumulated microseconds and call counts since the last dump. Plain globals
// in DRAM -- 56 bytes total, single-threaded build path, no locking needed.
inline uint32_t gUs[BUCKET_COUNT];
inline uint32_t gCalls[BUCKET_COUNT];
// Words handed to calculateWordWidths since the last dump -- lets the log
// report cost per word, which is the number that matters for CJK.
inline uint32_t gWords;

/// RAII timer. Nesting is fine; each scope charges only its own bucket.
class Scope {
 public:
  explicit Scope(const Bucket bucket) : bucket_(bucket), startUs_(micros()) {}
  ~Scope() {
    gUs[bucket_] += static_cast<uint32_t>(micros() - startUs_);
    ++gCalls[bucket_];
  }
  Scope(const Scope&) = delete;
  Scope& operator=(const Scope&) = delete;

 private:
  Bucket bucket_;
  uint32_t startUs_;
};

/// Emit one line per completed page and reset. Times are milliseconds;
/// the bracketed number is the call count for that bucket.
inline void dumpAndReset(const uint16_t pageNo) {
  LOG_INF("IXPROF",
          "page=%u words=%lu | total=%lu.%02lums layout=%lu.%02lu[%lu] measure=%lu.%02lu[%lu] "
          "linebrk=%lu.%02lu[%lu] extract=%lu.%02lu[%lu] pageout=%lu.%02lu[%lu] image=%lu.%02lu[%lu]",
          pageNo, gWords, gUs[TOTAL] / 1000UL, (gUs[TOTAL] % 1000UL) / 10UL, gUs[LAYOUT] / 1000UL,
          (gUs[LAYOUT] % 1000UL) / 10UL, gCalls[LAYOUT], gUs[MEASURE] / 1000UL, (gUs[MEASURE] % 1000UL) / 10UL,
          gCalls[MEASURE], gUs[LINEBRK] / 1000UL, (gUs[LINEBRK] % 1000UL) / 10UL, gCalls[LINEBRK],
          gUs[EXTRACT] / 1000UL, (gUs[EXTRACT] % 1000UL) / 10UL, gCalls[EXTRACT], gUs[PAGEOUT] / 1000UL,
          (gUs[PAGEOUT] % 1000UL) / 10UL, gCalls[PAGEOUT], gUs[IMAGE] / 1000UL, (gUs[IMAGE] % 1000UL) / 10UL,
          gCalls[IMAGE]);
  gWords = 0;
  for (uint8_t i = 0; i < BUCKET_COUNT; ++i) {
    gUs[i] = 0;
    gCalls[i] = 0;
  }
}

}  // namespace indexProfile

#define IXPROF_SCOPE(bucket) indexProfile::Scope ixprofScope_##bucket(indexProfile::bucket)
#define IXPROF_DUMP(pageNo) indexProfile::dumpAndReset(pageNo)
#define IXPROF_ADD_WORDS(n) (indexProfile::gWords += static_cast<uint32_t>(n))

#else  // !CJK_INDEX_PROFILE

#define IXPROF_SCOPE(bucket) ((void)0)
#define IXPROF_DUMP(pageNo) ((void)0)
#define IXPROF_ADD_WORDS(n) ((void)0)

#endif  // CJK_INDEX_PROFILE
