// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// HTTP transport for RetroAchievements; the only networking in DSperate.
// libcurl is loaded via dlopen, not linked, so a device without it simply
// loses achievements rather than failing to start.
#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>

namespace ds::cheevos {

struct Request {
  std::string url;
  std::string body;           // empty for GET, form-encoded for POST
  std::string content_type;
};

struct Response {
  int status = 0;              // 0 = no status reached (DNS/connect/TLS); rcheevos retries
  std::string body;
  std::string error;           // transport-level reason when status == 0
  bool cancelled = false;      // stopped by `cancel`, not by the network
};

// A blocking HTTP client. Call sites are on the worker thread only.
class Backend {
public:
  virtual ~Backend() = default;
  // `cancel`, when given, is watched during the transfer (about once a second, and on
  // every byte that moves): set, it ends the request at once with `cancelled`.
  virtual Response perform(const Request& req, const char* user_agent,
                           const std::atomic<bool>* cancel = nullptr) = 0;
  virtual const char* name() const = 0;
};

// Null with a reason in `err` if libcurl is missing; not an error to surface loudly.
std::unique_ptr<Backend> make_curl_backend(std::string& err);

} // namespace ds::cheevos
