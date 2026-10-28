// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// Achievement badges: the cache shared with DSlot, the media host behind it, and the
// decoded picture (docs/retroachievements.md). Blocking; the client runs it on its
// worker thread, after every request that matters more.
#pragma once

#include <atomic>
#include <string>
#include <vector>

#include "core/types.h"

namespace ds::cheevos {

class Backend;

// An achievement's badge, decoded: 0xAARRGGBB, rows top to bottom.
struct Badge {
  std::string name;
  int w = 0, h = 0;
  std::vector<u32> px;
};

// $XDG_CACHE_HOME or ~/.cache, then retroachievements/badges, made if missing.
// Empty when there is no home to put it in.
std::string badge_cache_dir();

// The name is a file name in the cache: the service's are digits (and _lock), so
// nothing but letters, digits, '_' and '-' gets in.
bool valid_badge_name(const std::string& name);

enum class BadgeResult : u8 {
  Ok,
  NotFetched,    // not cached and the media host did not give it: worth asking again later
  Undecodable,   // the bytes are not a PNG we read: asking again gives the same bytes
};

// The cache, else the media host through `http` (into the cache on a 200), then decoded.
// `cancel` aborts a download in progress.
BadgeResult load_badge(const std::string& name, Backend& http, const char* user_agent,
                       const std::atomic<bool>* cancel, Badge& out, std::string& err);

} // namespace ds::cheevos
