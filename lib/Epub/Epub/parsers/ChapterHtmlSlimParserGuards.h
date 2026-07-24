#pragma once

// v18.9.3: BT-aware table guard. When true, ChapterHtmlSlimParser forces
// every table to fall back to paragraph flow -- matches CrossPoint/INX
// behavior which never had structured table rendering. Table buffering
// wants 64 KB free / 40 KB maxAlloc, and NimBLE resident already burns
// ~58 KB of that budget, so books with tables were the most common
// BT+reader OOM class.
//
// This header exists to keep main.cpp (the only writer) free of the full
// ChapterHtmlSlimParser.h dependency graph -- that header pulls in Page.h
// through a std::unique_ptr<Page> in std::function, which requires Page's
// complete type at the point of instantiation.
//
// v18.9.6: guards are now composable from multiple sources:
//   - BT source: main.cpp writes this every loop tick based on
//     BluetoothHIDManager::isEnabled()
//   - Simple-rendering source: Section::createSectionFile writes true
//     around a force-simple retry parse; the reader keeps it true for the
//     whole book once the retry succeeds
// Parser checks the OR of the two.
void setChapterParserSuppressTables(bool suppress);        // BT source (legacy name kept)
void setChapterParserSuppressTablesForSimple(bool suppress); // Simple-rendering source
bool getChapterParserSuppressTables();                     // Returns bt || simple
