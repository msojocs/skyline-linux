// Resource/font/HTTP platform channels.

#include "internal/skyline_runtime_module.hh"

#ifdef SKYLINE_EMBEDDED
// ---------------------------------------------------------------------------
// Resource loading (platform channel)
//
// `package:skyline` calls these when a font/image resource is requested.  Paths
// resolve against `SKYLINE_RESOURCE_ROOT` (absolute paths and file:// are used
// as-is).  Async results are delivered on the UI task runner by invoking the
// registered `NotifyResourceLoad` callback; calling back into Dart inside the
// original FFI call would violate the VM's callback rules.
// ---------------------------------------------------------------------------

namespace {

std::string ResourcePathFromDart(Dart_Handle value) {
  if (!Dart_IsString(value)) return {};
  const char* text = nullptr;
  if (Dart_IsError(Dart_StringToCString(value, &text)) || text == nullptr) {
    return {};
  }
  return text;
}


Dart_Handle NewResourceData(const std::vector<uint8_t>& bytes) {
  Dart_Handle data =
      Dart_NewTypedData(Dart_TypedData_kUint8,
                        static_cast<intptr_t>(bytes.size()));
  if (Dart_IsError(data)) return data;
  if (bytes.empty()) return data;
  Dart_TypedData_Type type = Dart_TypedData_kInvalid;
  void* raw = nullptr;
  intptr_t length = 0;
  Dart_Handle status = Dart_TypedDataAcquireData(data, &type, &raw, &length);
  if (!Dart_IsError(status) && type == Dart_TypedData_kUint8 &&
      raw != nullptr && length == static_cast<intptr_t>(bytes.size())) {
    std::memcpy(raw, bytes.data(), bytes.size());
  }
  Dart_TypedDataReleaseData(data);
  return data;
}

// Font/resource cache.  `package:skyline` may request the same font or asset
// repeatedly; keep small payloads in memory for the replay lifetime.
std::unordered_map<std::string, std::vector<uint8_t>>& ResourceCache() {
  static auto* cache =
      new std::unordered_map<std::string, std::vector<uint8_t>>();
  return *cache;
}

bool LoadResourceBytes(const std::string& path, std::vector<uint8_t>* out,
                       bool* from_cache) {
  if (from_cache != nullptr) *from_cache = false;
  auto& cache = ResourceCache();
  const auto found = cache.find(path);
  if (found != cache.end()) {
    *out = found->second;
    if (from_cache != nullptr) *from_cache = true;
    return true;
  }
  if (!ReadResourceBytes(path, out)) return false;
  constexpr size_t kMaxCachedBytes = 4u * 1024u * 1024u;
  if (out->size() <= kMaxCachedBytes) cache.emplace(path, *out);
  return true;
}

}  // namespace

SKYLINE_EXPORT extern "C" Dart_Handle LoadResourceSync(void* /*runtime_ptr*/,
                                                       Dart_Handle path) {
  const std::string requested = ResourcePathFromDart(path);
  const std::string resolved = ResolveResourcePath(requested);
  std::vector<uint8_t> bytes;
  bool from_cache = false;
  const bool ok = LoadResourceBytes(resolved, &bytes, &from_cache);
  std::fprintf(stderr,
               "[skyline-runtime] LoadResourceSync: %s -> %s (%s, %zu bytes%s)\n",
               requested.c_str(), resolved.c_str(), ok ? "ok" : "missing",
               bytes.size(), from_cache ? ", cache" : "");
  return NewResourceData(bytes);
}

SKYLINE_EXPORT extern "C" void LoadResourceAsync(void* runtime_ptr,
                                                 void* /*window_ptr*/,
                                                 Dart_Handle path,
                                                 int32_t callback_id) {
  const std::string requested = ResourcePathFromDart(path);
  auto* runtime = static_cast<Runtime*>(runtime_ptr);
  RuntimeHolder* holder = runtime != nullptr ? runtime->holder : &g_fallback_holder;
  auto runner = UiTaskRunner();
  if (!runner) return;
  runner->PostTask([holder, requested, callback_id]() {
    DemoPageApiScope scope;
    const std::string resolved = ResolveResourcePath(requested);
    std::vector<uint8_t> bytes;
    bool from_cache = false;
    const bool ok = LoadResourceBytes(resolved, &bytes, &from_cache);
    Dart_Handle payload = NewResourceData(bytes);
    void* callback = FindCallback(holder != nullptr ? holder->ffi_table : nullptr,
                                  kNotifyResourceLoadHash);
    if (callback != nullptr && payload != nullptr && !Dart_IsError(payload)) {
      reinterpret_cast<NotifyResourceLoadFn>(callback)(
          static_cast<uint8_t>(callback_id), payload);
    }
    std::fprintf(stderr,
                 "[skyline-runtime] LoadResourceAsync: %s -> %s (%s, %zu bytes, "
                 "id=%d, notified=%s%s)\n",
                 requested.c_str(), resolved.c_str(), ok ? "ok" : "missing",
                 bytes.size(), callback_id, callback != nullptr ? "yes" : "no",
                 from_cache ? ", cache" : "");
  });
}

// ---------------------------------------------------------------------------
// In-memory font state
//
// `FontFace.load()` (package:skyline/utils/font_face.dart) registers families
// with `AddPendingFont` while the @font-face request is in flight and calls
// `RemovePendingFont` once FontLoader resolves.  Loading proceeds with the
// fallback chain in the meantime; when the family stops being pending we
// re-apply the text widgets that reference it so the newly registered font is
// picked up without rebuilding (and re-loading) the whole page.
// ---------------------------------------------------------------------------

bool TextUsesFontFamily(const appservice::AppNode* node,
                        const std::string& family) {
  if (node == nullptr) return false;
  for (const std::string& candidate : node->style.font_families) {
    if (candidate == family) return true;
  }
  return false;
}

bool PageUsesFontFamily(const std::string& family) {
  if (appservice::g_page == nullptr) return false;
  for (const auto& entry : appservice::g_page->nodes) {
    if (TextUsesFontFamily(entry.second, family)) return true;
  }
  return false;
}

void RelayoutTextForFont(RuntimeHolder* holder, const std::string& family) {
  if (holder == nullptr || appservice::g_page == nullptr) return;
  auto* apply = reinterpret_cast<TextApplyUpdateFn>(
      FindCallback(holder->ffi_table, kTextApplyUpdateHash));
  auto* mark_needs_layout = reinterpret_cast<MarkYGNodeNeedsLayoutFn>(
      FindCallback(holder->ffi_table, kMarkYGNodeNeedsLayoutHash));
  size_t relaid_out = 0;
  for (const auto& entry : appservice::g_page->nodes) {
    appservice::AppNode* node = entry.second;
    if (node == nullptr || node->tag != "text" ||
        !TextUsesFontFamily(node, family)) {
      continue;
    }
    if (apply != nullptr && node->text_widget_p != nullptr) {
      apply(Dart_HandleFromPersistent(node->text_widget_p), 0);
      relaid_out++;
    }
    if (mark_needs_layout != nullptr && node->yoga_node_p != nullptr) {
      mark_needs_layout(Dart_HandleFromPersistent(node->yoga_node_p));
    }
  }
  std::fprintf(stderr,
               "[skyline-runtime] font relayout: %s (%zu text node(s))\n",
               family.c_str(), relaid_out);
}

SKYLINE_EXPORT extern "C" void AddPendingFont(void* /*runtime_ptr*/,
                                              Dart_Handle family_handle) {
  const char* text = nullptr;
  if (Dart_IsError(Dart_StringToCString(family_handle, &text)) ||
      text == nullptr) {
    return;
  }
  const std::string family(text);
  appservice::g_pending_fonts.insert(family);
  appservice::g_font_status[family] = -1;
  std::fprintf(stderr,
               "[skyline-runtime] AddPendingFont: %s (pending=%zu)\n",
               family.c_str(), appservice::g_pending_fonts.size());
}

SKYLINE_EXPORT extern "C" void RemovePendingFont(void* /*runtime_ptr*/,
                                                 Dart_Handle family_handle) {
  const char* text = nullptr;
  if (Dart_IsError(Dart_StringToCString(family_handle, &text)) ||
      text == nullptr) {
    return;
  }
  const std::string family(text);
  appservice::g_pending_fonts.erase(family);
  const bool used = PageUsesFontFamily(family);
  const auto status_it = appservice::g_font_status.find(family);
  const int last_status =
      status_it != appservice::g_font_status.end() ? status_it->second : -2;
  std::fprintf(stderr,
               "[skyline-runtime] RemovePendingFont: %s (pending=%zu, used=%d, "
               "status=%d)\n",
               family.c_str(), appservice::g_pending_fonts.size(),
               used ? 1 : 0, last_status);
  if (!used || appservice::g_page == nullptr ||
      appservice::g_page->holder == nullptr) {
    return;
  }
  RuntimeHolder* holder = appservice::g_page->holder;
  auto runner = UiTaskRunner();
  if (!runner) return;
  runner->PostTask([holder, family]() {
    DemoPageApiScope scope;
    RelayoutTextForFont(holder, family);
  });
}

SKYLINE_EXPORT extern "C" void NotifyFontFaceLoad(void* /*runtime_ptr*/,
                                                  int32_t callback_id,
                                                  int32_t status) {
  std::string family;
  const auto found =
      appservice::g_font_callback_families.find(callback_id);
  if (found != appservice::g_font_callback_families.end()) {
    family = found->second;
    appservice::g_font_status[family] = status;
    appservice::g_font_callback_families.erase(found);
  }
  std::fprintf(stderr,
               "[skyline-runtime] NotifyFontFaceLoad: id=%d status=%d family=%s "
               "(pending=%zu)\n",
               callback_id, status, family.c_str(),
               appservice::g_pending_fonts.size());
}

// ---------------------------------------------------------------------------
// HTTP platform channel
//
// `HttpForwarder.httpGet` (package:skyline/utils/http_forwarder.dart) calls the
// `ForwardHttpGet` export.  The actual request is performed by Dart's own
// `RequestHttpResource` implementation (`dart:io` HttpClient for DevTools), so
// the native side only bridges the two: it hands Dart a C callback and later
// forwards the payload to `OnForwardHttpGetResponse` on the UI runner.
// ---------------------------------------------------------------------------

namespace {

using RequestHttpResourceFn = void (*)(uint32_t request_context_id,
                                       Dart_Handle url,
                                       void* native_callback);
using OnForwardHttpGetResponseFn = void (*)(Dart_Handle data,
                                            Dart_Handle callback);

struct PendingHttpGet {
  Dart_PersistentHandle callback = nullptr;
  RuntimeHolder* holder = nullptr;
  std::string url;
  bool via_shell = false;
};

std::unordered_map<uint32_t, PendingHttpGet> g_pending_http_gets;
uint32_t g_next_http_get_id = 0x80000000u;

void DeliverPendingHttpGet(uint32_t request_id, std::vector<uint8_t> bytes);

extern "C" void OnHttpResourceResponse(intptr_t /*runtime_ptr*/,
                                       uint32_t request_id, Dart_Handle data,
                                       uint8_t /*from_cache*/);

void IssueDartHttpGet(RuntimeHolder* holder, uint32_t request_id,
                      const std::string& url) {
  void* request = FindCallback(
      holder != nullptr ? holder->ffi_table : nullptr,
      kRequestHttpResourceHash);
  if (request == nullptr) {
    std::fprintf(stderr,
                 "[skyline-runtime] ForwardHttpGet: RequestHttpResource "
                 "callback missing\n");
    DeliverPendingHttpGet(request_id, {});
    return;
  }
  Dart_Handle url_handle = Dart_NewStringFromCString(url.c_str());
  reinterpret_cast<RequestHttpResourceFn>(request)(
      request_id, url_handle,
      reinterpret_cast<void*>(&OnHttpResourceResponse));
}

void StartDartHttpGet(RuntimeHolder* holder, uint32_t request_id,
                      const std::string& url) {
  auto runner = UiTaskRunner();
  if (!runner) {
    DeliverPendingHttpGet(request_id, {});
    return;
  }
  runner->PostTask([holder, request_id, url]() {
    DemoPageApiScope scope;
    IssueDartHttpGet(holder, request_id, url);
  });
}

extern "C" void OnHttpResourceResponse(intptr_t /*runtime_ptr*/,
                                       uint32_t request_id, Dart_Handle data,
                                       uint8_t /*from_cache*/) {
  std::vector<uint8_t> bytes;
  if (data != nullptr && !Dart_IsNull(data) && !Dart_IsError(data)) {
    Dart_TypedData_Type type = Dart_TypedData_kInvalid;
    void* raw = nullptr;
    intptr_t length = 0;
    if (!Dart_IsError(Dart_TypedDataAcquireData(data, &type, &raw, &length)) &&
        raw != nullptr && length > 0) {
      bytes.assign(static_cast<uint8_t*>(raw),
                   static_cast<uint8_t*>(raw) + length);
    }
    Dart_TypedDataReleaseData(data);
  }
  DeliverPendingHttpGet(request_id, std::move(bytes));
}

void DeliverPendingHttpGet(uint32_t request_id, std::vector<uint8_t> bytes) {
  auto found = g_pending_http_gets.find(request_id);
  if (found == g_pending_http_gets.end()) return;
  PendingHttpGet pending = found->second;
  if (bytes.empty() && pending.via_shell) {
    pending.via_shell = false;
    g_pending_http_gets[request_id] = pending;
    std::fprintf(stderr,
                 "[skyline-runtime] ForwardHttpGet %u: shell fetch empty, "
                 "falling back to dart:io\n",
                 request_id);
    StartDartHttpGet(pending.holder, request_id, pending.url);
    return;
  }
  g_pending_http_gets.erase(found);
  auto runner = UiTaskRunner();
  if (!runner) {
    if (pending.callback != nullptr) Dart_DeletePersistentHandle(pending.callback);
    return;
  }
  runner->PostTask([pending, bytes = std::move(bytes)]() {
    DemoPageApiScope scope;
    Dart_Handle payload = Dart_Null();
    if (!bytes.empty()) {
      payload = Dart_NewTypedData(Dart_TypedData_kUint8,
                                  static_cast<intptr_t>(bytes.size()));
      if (!Dart_IsError(payload)) {
        Dart_TypedData_Type type = Dart_TypedData_kInvalid;
        void* raw = nullptr;
        intptr_t length = 0;
        if (!Dart_IsError(Dart_TypedDataAcquireData(payload, &type, &raw,
                                                   &length)) &&
            raw != nullptr) {
          std::memcpy(raw, bytes.data(),
                      std::min<size_t>(bytes.size(),
                                       static_cast<size_t>(length)));
          Dart_TypedDataReleaseData(payload);
        }
      }
    }
    void* callback = FindCallback(
        pending.holder != nullptr ? pending.holder->ffi_table : nullptr,
        kOnForwardHttpGetResponseHash);
    if (callback != nullptr && pending.callback != nullptr) {
      Dart_Handle dart_callback =
          Dart_HandleFromPersistent(pending.callback);
      reinterpret_cast<OnForwardHttpGetResponseFn>(callback)(payload,
                                                             dart_callback);
    }
    if (pending.callback != nullptr) Dart_DeletePersistentHandle(pending.callback);
  });
}

}  // namespace

SKYLINE_EXPORT extern "C" void SkylineSetHttpRequestSink(
    SkylineHttpRequestSink sink, SkylineHasHttpRequestCallbackFn has) {
  g_shell_http_request.store(sink);
  g_shell_http_available.store(has);
}

// Addon JS thread -> runtime: the DevTools pageframe finished a shell fetch.
SKYLINE_EXPORT extern "C" void SkylineNotifyHttpRequestComplete(
    uint32_t request_id, const uint8_t* data, int32_t length,
    uint8_t /*from_cache*/) {
  std::vector<uint8_t> bytes;
  if (data != nullptr && length > 0) {
    bytes.assign(data, data + length);
  }
  // Called from the addon JS thread: hop onto the engine UI task runner.
  PostToUiIfReady([request_id, bytes = std::move(bytes)]() {
    if (request_id >= 0x80000000u) {
      DeliverPendingHttpGet(request_id, bytes);
    } else {
      appservice::DeliverImagePayload(request_id, std::move(bytes));
    }
  });
}

SKYLINE_EXPORT extern "C" void ForwardHttpGet(void* runtime_ptr,
                                              void* /*window_ptr*/,
                                              Dart_Handle url,
                                              Dart_Handle callback) {
  if (callback == nullptr || Dart_IsNull(callback)) return;
  auto* runtime = static_cast<Runtime*>(runtime_ptr);
  RuntimeHolder* holder = runtime != nullptr ? runtime->holder : &g_fallback_holder;
  const std::string requested = ResourcePathFromDart(url);
  const uint32_t request_id = ++g_next_http_get_id;
  const bool via_shell = ShellHttpAvailable();
  PendingHttpGet pending;
  pending.callback = Dart_NewPersistentHandle(callback);
  pending.holder = holder;
  pending.url = requested;
  pending.via_shell = via_shell;
  g_pending_http_gets[request_id] = pending;

  auto runner = UiTaskRunner();
  if (!runner) {
    g_pending_http_gets.erase(request_id);
    Dart_DeletePersistentHandle(pending.callback);
    return;
  }
  std::fprintf(stderr,
               "[skyline-runtime] ForwardHttpGet: id=%u url=%s%s\n",
               request_id, requested.c_str(),
               via_shell ? " (shell)" : "");
  runner->PostTask([holder, request_id, requested, via_shell, runner]() {
    if (via_shell) {
      RequestShellHttp(request_id, requested);
      runner->PostDelayedTask(
          [holder, request_id, requested]() {
            auto found = g_pending_http_gets.find(request_id);
            if (found == g_pending_http_gets.end() ||
                !found->second.via_shell) {
              return;
            }
            found->second.via_shell = false;
            std::fprintf(stderr,
                         "[skyline-runtime] ForwardHttpGet %u: shell timeout, "
                         "falling back to dart:io\n",
                         request_id);
            StartDartHttpGet(holder, request_id, requested);
          },
          fml::TimeDelta::FromMilliseconds(2000));
      return;
    }
    DemoPageApiScope scope;
    IssueDartHttpGet(holder, request_id, requested);
  });
}
#endif  // SKYLINE_EMBEDDED
#ifdef SKYLINE_EMBEDDED
SKYLINE_EXPORT extern "C" void HandleGestureEvent(void* /*runtime_ptr*/,
                                                  int32_t handler_id,
                                                  int32_t gesture_type,
                                                  void* /*data*/,
                                                  int32_t /*bytes*/) {
  if (std::getenv("SKYLINE_APP_SERVICE_DEBUG") != nullptr) {
    std::fprintf(stderr,
                 "[skyline-runtime] HandleGestureEvent: handler=%d type=%d\n",
                 handler_id, gesture_type);
  }
}
#endif
