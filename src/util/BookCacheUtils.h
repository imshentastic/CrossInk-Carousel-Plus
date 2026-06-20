#pragma once

#include <string>

// Clears the reading cache for a book file if its extension is recognised
// (EPUB, XTC, or TXT). Does nothing for other file types.
void clearBookCache(const std::string& path);

// CrumBLE 4.4 (ported from CrossInk v1.3.3): clears a known book cache
// directory while preserving per-book reading stats (stats.bin). Used by
// "Clear Reading Cache" so per-book streaks/totals aren't lost. Note: takes
// a *cache* path (e.g. /.crosspoint/epub_<hash>), not a book file path.
bool clearBookCacheDirectoryPreservingStats(const std::string& cachePath);

// Returns true if the directory name matches a book cache entry.
bool isBookCacheDirectoryName(const char* name);
