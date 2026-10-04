// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "cheevos/cheevos_http.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <dlfcn.h>
#include <sys/stat.h>

namespace ds::cheevos {
namespace {

// libcurl's ABI, declared rather than included (loaded via dlopen). Option
// numbers are permanent (type base + index) and safe to hard-code.
using CURL = void;
constexpr long CURL_GLOBAL_ALL = 3;
constexpr int  CURLE_OK = 0;

enum CurlOpt : int {
  CURLOPT_WRITEDATA        = 10001,   // CBPOINT 1
  CURLOPT_URL              = 10002,   // STRINGPOINT 2
  CURLOPT_WRITEFUNCTION    = 20011,   // FUNCTIONPOINT 11
  CURLOPT_POSTFIELDS       = 10015,   // OBJECTPOINT 15
  CURLOPT_USERAGENT        = 10018,   // STRINGPOINT 18
  CURLOPT_HTTPHEADER       = 10023,   // SLISTPOINT 23
  CURLOPT_POST             = 47,      // LONG 47
  CURLOPT_FOLLOWLOCATION   = 52,      // LONG 52
  CURLOPT_POSTFIELDSIZE    = 60,      // LONG 60
  CURLOPT_CAINFO           = 10065,   // STRINGPOINT 65
  CURLOPT_NOSIGNAL         = 99,      // LONG 99
  CURLOPT_ACCEPT_ENCODING  = 10102,   // STRINGPOINT 102
  CURLOPT_TIMEOUT_MS       = 155,     // LONG 155
  CURLOPT_CONNECTTIMEOUT_MS = 156,    // LONG 156
};
constexpr int CURLINFO_RESPONSE_CODE = 0x200000 + 2;   // CURLINFO_LONG + 2

struct curl_slist;

struct Api {
  int   (*global_init)(long);
  CURL* (*easy_init)();
  int   (*easy_setopt)(CURL*, int, ...);
  int   (*easy_perform)(CURL*);
  int   (*easy_getinfo)(CURL*, int, ...);
  void  (*easy_cleanup)(CURL*);
  const char* (*easy_strerror)(int);
  curl_slist* (*slist_append)(curl_slist*, const char*);
  void  (*slist_free_all)(curl_slist*);
};

// Fallback trust stores, consulted only when curl's own compiled-in store is absent.
const char* const kCaPaths[] = {
  "/etc/ssl/certs/ca-certificates.crt",
  "/etc/pki/tls/certs/ca-bundle.crt",
  "/etc/ssl/cert.pem",
  "/mnt/SDCARD/spruce/etc/ca-certificates.crt",
};

bool exists(const char* path) {
  struct stat st{};
  return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

constexpr size_t MAX_BODY = 8u << 20;   // caps a runaway response

struct Sink {
  std::string* out;
  bool overflowed = false;
};

size_t write_cb(char* data, size_t size, size_t nmemb, void* user) {
  Sink* s = static_cast<Sink*>(user);
  const size_t n = size * nmemb;
  if (s->out->size() + n > MAX_BODY) { s->overflowed = true; return 0; }  // 0 aborts the transfer
  s->out->append(data, n);
  return n;
}

class CurlBackend final : public Backend {
public:
  CurlBackend(void* handle, const Api& api, const char* from)
      : handle_(handle), api_(api), name_(std::string("libcurl (") + from + ")") {
    if (const char* ca = std::getenv("DS_CHEEVOS_CAINFO")) {
      if (*ca) { ca_ = ca; return; }
    }
    for (const char* p : kCaPaths) {
      if (exists(p)) { ca_ = p; break; }
    }
  }
  // Handle deliberately leaked, curl_global_cleanup deliberately not called:
  // tearing down process-wide curl/OpenSSL state at exit risks a crash.
  ~CurlBackend() override { (void)handle_; }

  const char* name() const override { return name_.c_str(); }

  Response perform(const Request& req, const char* user_agent) override {
    Response res;
    CURL* c = api_.easy_init();
    if (!c) { res.error = "curl_easy_init failed"; return res; }

    Sink sink{&res.body};
    api_.easy_setopt(c, CURLOPT_URL, req.url.c_str());
    api_.easy_setopt(c, CURLOPT_WRITEFUNCTION, &write_cb);
    api_.easy_setopt(c, CURLOPT_WRITEDATA, &sink);
    api_.easy_setopt(c, CURLOPT_USERAGENT, user_agent);
    api_.easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    api_.easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");
    // curl's default DNS timeout is alarm-based and not thread-safe.
    api_.easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    api_.easy_setopt(c, CURLOPT_CONNECTTIMEOUT_MS, 10000L);
    api_.easy_setopt(c, CURLOPT_TIMEOUT_MS, 30000L);
    if (!ca_.empty()) api_.easy_setopt(c, CURLOPT_CAINFO, ca_.c_str());

    curl_slist* headers = nullptr;
    if (!req.body.empty()) {
      api_.easy_setopt(c, CURLOPT_POST, 1L);
      api_.easy_setopt(c, CURLOPT_POSTFIELDS, req.body.c_str());
      api_.easy_setopt(c, CURLOPT_POSTFIELDSIZE, static_cast<long>(req.body.size()));
      if (!req.content_type.empty()) {
        const std::string h = "Content-Type: " + req.content_type;
        headers = api_.slist_append(headers, h.c_str());
      }
      headers = api_.slist_append(headers, "Expect:");  // suppress 100-continue stalls
      if (headers) api_.easy_setopt(c, CURLOPT_HTTPHEADER, headers);
    }

    const int rc = api_.easy_perform(c);
    if (rc == CURLE_OK) {
      long status = 0;
      api_.easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
      res.status = static_cast<int>(status);
    } else if (sink.overflowed) {
      res.error = "response larger than 8 MB";
    } else {
      const char* m = api_.easy_strerror ? api_.easy_strerror(rc) : nullptr;
      res.error = m ? m : ("curl error " + std::to_string(rc));
    }

    if (headers) api_.slist_free_all(headers);
    api_.easy_cleanup(c);
    return res;
  }

private:
  void* handle_;
  Api api_;
  std::string name_, ca_;
};

} // namespace

std::unique_ptr<Backend> make_curl_backend(std::string& err) {
  err.clear();

  // DS_CHEEVOS_LIBCURL overrides the soname for a firmware that hides it from the loader.
  const char* explicit_path = std::getenv("DS_CHEEVOS_LIBCURL");
  if (explicit_path && !*explicit_path) explicit_path = nullptr;
  const char* what = explicit_path ? explicit_path : "libcurl.so.4";
  void* h = dlopen(what, RTLD_NOW | RTLD_LOCAL);
  if (!h) {
    const char* e = dlerror();
    err = e ? e : "libcurl.so.4 not found";
    return nullptr;
  }

  Api api{};
  struct Sym { const char* name; void** slot; };
  const Sym syms[] = {
    {"curl_global_init",   reinterpret_cast<void**>(&api.global_init)},
    {"curl_easy_init",     reinterpret_cast<void**>(&api.easy_init)},
    {"curl_easy_setopt",   reinterpret_cast<void**>(&api.easy_setopt)},
    {"curl_easy_perform",  reinterpret_cast<void**>(&api.easy_perform)},
    {"curl_easy_getinfo",  reinterpret_cast<void**>(&api.easy_getinfo)},
    {"curl_easy_cleanup",  reinterpret_cast<void**>(&api.easy_cleanup)},
    {"curl_easy_strerror", reinterpret_cast<void**>(&api.easy_strerror)},
    {"curl_slist_append",  reinterpret_cast<void**>(&api.slist_append)},
    {"curl_slist_free_all", reinterpret_cast<void**>(&api.slist_free_all)},
  };
  for (const Sym& s : syms) {
    *s.slot = dlsym(h, s.name);
    if (!*s.slot) {
      err = std::string("libcurl.so.4 has no ") + s.name;
      dlclose(h);
      return nullptr;
    }
  }

  // Must run before any easy handle exists: curl's lazy init is not thread-safe.
  if (api.global_init(CURL_GLOBAL_ALL) != CURLE_OK) {
    err = "curl_global_init failed";
    dlclose(h);
    return nullptr;
  }
  return std::make_unique<CurlBackend>(h, api, what);
}

} // namespace ds::cheevos
