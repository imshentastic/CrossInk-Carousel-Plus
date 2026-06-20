#pragma once
#include <string>

// CrumBLE: persistent "do not retry" marker for cover-thumbnail generation
// that has failed on a given book at a given W x H. Some EPUBs ship a
// cover image we can't decode on-device (PNG with palette quirks,
// malformed JPEGs, OPF that points at a missing path, etc.). Without
// this gate, every visit to the home carousel or a collection grid
// re-attempts generation, briefly flashes a Loading popup, fails again,
// and finally renders the placeholder -- turning what should be a
// snappy scroll into a stutter.
//
// The marker is a zero-byte file at
// <book cachePath>/thumb_failed_v3_<W>x<H>.marker so it persists across
// reboots (lives on SD card alongside the book's other cache state).
// It's cleared whenever a generation attempt for the SAME size succeeds.
//
// Per-size scoping (v3) fixes the case where a transient memory-pressure
// failure at one size (e.g. Collections at 100x150) would otherwise
// permanently block that book's cover from being regenerated -- even
// though another size (e.g. Carousel at 296x468) had already cached a
// usable thumb. Previously markers were global per book, which meant
// one bad attempt poisoned every future attempt regardless of size.
namespace CoverThumbStatus {

// True when we've previously failed to generate a cover thumbnail for
// the book at `bookPath` at the requested size. Call before any
// generateThumbBmp*() to skip the doomed-to-fail attempt and fall
// through to the placeholder render. Size MUST match the W,H that the
// caller plans to request (otherwise markers are silently ignored,
// which is the conservative behaviour for a UI cache hint).
bool isMarkedFailed(const std::string& bookPath, int width, int height);

// Persist the "give up" marker so future visits stop retrying at this
// size. Call after a generateThumbBmp*() returns false for the same W,H.
void markFailed(const std::string& bookPath, int width, int height);

// Drop the marker (idempotent). Call after a successful regeneration in
// case the book had been previously marked but is now decodable (build
// improvement, or user replaced the file). Cheap no-op when the marker
// doesn't exist. Size-scoped to the same W,H the caller just succeeded
// at; pass the same dimensions that were used in the gen attempt.
void clearFailed(const std::string& bookPath, int width, int height);

// CrumBLE 4.4: wipe ALL thumb_failed_v3_*.marker files across every
// per-book cache dir under /.crosspoint/. Used by:
//   1. Boot-time auto-sweep after a firmware-version change so users
//      whose covers broke on a fixed bug (e.g. the EOCD scan window
//      bump) get their covers back without manual intervention.
//   2. Settings > System > Retry Failed Cover Thumbnails -- manual
//      lever for users who want to re-attempt without waiting for the
//      next update (e.g. they freed heap, replaced a book file, etc.).
// Returns the count of markers removed. Walks the /.crosspoint/ tree
// once; cost scales with number of cache subdirs (~10-200 typical).
int sweepAllMarkers();

}  // namespace CoverThumbStatus
