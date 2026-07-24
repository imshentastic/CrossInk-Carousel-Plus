#pragma once

// v18.9.9.456 — CrossInk-parity stats backup writer. Copies the current
// global_stats.bin to /.crossink-stats-backup/stats_YYYY-MM-DD[_HHMM].bin
// with atomic temp+rename+sync and keep=N pruning by name-sort.
//
// Backup dir path is FIXED per upstream so users migrating between CrumBLE
// and CrossInk can pick up each other's backups directly.

namespace StatsBackup {

// Fixed directory. Sibling of /.crosspoint, not inside it.
constexpr const char* kBackupDir = "/.crossink-stats-backup";

// Default keep count. Older files removed by pruneBackups.
constexpr int kDefaultKeepCount = 7;

// Manual entry point. Uses HH:MM in the filename so multiple manual backups
// on the same day don't collide. Returns true on write+sync success.
bool writeManualBackup();

// Auto entry point (from sleep-entry hook). Uses date-only filename so
// multiple auto backups on the same day coalesce to one file. Gated on
// SETTINGS.autoBackupStats and clock validity; safe to call unconditionally.
// Returns true if a backup was written, false if skipped or failed.
bool maybeWriteAutoBackup();

// Retain the newest N files matching stats_*.bin, delete older. Runs after
// every write. Files sort chronologically by name (YYYY-MM-DD prefix).
void pruneBackups(int keep = kDefaultKeepCount);

}  // namespace StatsBackup
