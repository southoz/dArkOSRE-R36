// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// HTTP transport for RetroAchievements; the only networking in DSperate.
// libcurl is loaded via dlopen, not linked, so a device without it simply
// loses achievements rather than failing to start.
#pragma once

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
};

// A blocking HTTP client. Call sites are on the worker thread only.
class Backend {
public:
  virtual ~Backend() = default;
  virtual Response perform(const Request& req, const char* user_agent) = 0;
  virtual const char* name() const = 0;
};

// Null with a reason in `err` if libcurl is missing; not an error to surface loudly.
std::unique_ptr<Backend> make_curl_backend(std::string& err);

} // namespace ds::cheevos
