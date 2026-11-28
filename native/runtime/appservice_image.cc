// Image decoding, painting, loading and cancellation.

#include "internal/skyline_runtime_module.hh"
#include "internal/skyline_runtime_image.hh"

namespace appservice {

void DirectRenderImageDispose(intptr_t /*ptr*/) {}
void DirectRenderImageCreate(intptr_t /*ptr*/) {}
void DirectRenderImageLayout(intptr_t /*ptr*/, double /*w*/, double /*h*/) {}

int ImageRepeatMode(const AppNode* node) {
  static const char* kKeys[] = {"repeat", "background-repeat", "backgroundRepeat"};
  for (const char* key : kKeys) {
    const auto found = node->attrs.find(key);
    if (found != node->attrs.end() && !found->second.empty()) {
      return ParseBackgroundRepeat(found->second);
    }
  }
  return node->style.background_repeat;
}

double ResolvePositionOffset(const CssPositionAxis& axis, double free) {
  const double offset = axis.percent ? free * (axis.value / 100.0) : axis.value;
  return axis.from_end ? free - offset : offset;
}

// Draws `image` into a width x height box, applying a CSS-like object-fit,
// repeat mode and background/object position.  Shared by <image>
// direct-render widgets and CSS background images.
void DrawImageInBox(flutter::Canvas* canvas, const sk_sp<flutter::DlImage>& image,
                    double width, double height, int fit, int repeat,
                    double offset_x, double offset_y,
                    const CssPositionAxis& pos_x,
                    const CssPositionAxis& pos_y,
                    bool size_override = false,
                    double override_w = 0.0,
                    double override_h = 0.0) {
  if (!image || !canvas || canvas->builder() == nullptr) return;
  const double image_width = image->width();
  const double image_height = image->height();
  if (image_width <= 0.0 || image_height <= 0.0) return;
  const double target_w = std::max(0.0, width);
  const double target_h = std::max(0.0, height);
  if (target_w <= 0.0 || target_h <= 0.0) return;

  double dest_x = 0.0;
  double dest_y = 0.0;
  double dest_w = target_w;
  double dest_h = target_h;
  if (size_override) {
    dest_w = std::max(0.0, override_w);
    dest_h = std::max(0.0, override_h);
  } else if (fit == 1 || fit == 3 || fit == 4) {
    double scale = fit == 3 ? 1.0 : std::min(target_w / image_width,
                                             target_h / image_height);
    if (fit == 4 && image_width <= target_w && image_height <= target_h) {
      scale = 1.0;
    }
    dest_w = image_width * scale;
    dest_h = image_height * scale;
  } else if (fit == 2) {
    const double scale = std::max(target_w / image_width,
                                  target_h / image_height);
    dest_w = image_width * scale;
    dest_h = image_height * scale;
  }
  if (dest_w <= 0.0 || dest_h <= 0.0) return;

  canvas->save();
  canvas->translate(offset_x, offset_y);
  if (fit == 2 || repeat != 0 || size_override) {
    canvas->clipRect(0.0, 0.0, target_w, target_h,
                     flutter::DlClipOp::kIntersect);
  }
  if (repeat == 0) {
    dest_x = ResolvePositionOffset(pos_x, target_w - dest_w);
    dest_y = ResolvePositionOffset(pos_y, target_h - dest_h);
    canvas->builder()->DrawImageRect(
        image, flutter::DlRect::MakeLTRB(0.0, 0.0, image_width, image_height),
        flutter::DlRect::MakeLTRB(dest_x, dest_y, dest_x + dest_w,
                                  dest_y + dest_h),
        flutter::DlImageSampling::kLinear, nullptr);
  } else {
    // CSS repeat tiles at the natural image size (or the background-size
    // result when a fit was requested).  background-position moves the origin
    // of the tile grid.
    const double tile_w =
        size_override ? dest_w : (fit == 0 ? image_width : dest_w);
    const double tile_h =
        size_override ? dest_h : (fit == 0 ? image_height : dest_h);
    if (tile_w <= 0.0 || tile_h <= 0.0) {
      canvas->restore();
      return;
    }
    const bool tile_x = repeat == 1 || repeat == 2;
    const bool tile_y = repeat == 1 || repeat == 3;
    const double origin_x = ResolvePositionOffset(pos_x, target_w - tile_w);
    const double origin_y = ResolvePositionOffset(pos_y, target_h - tile_h);
    const int columns =
        tile_x ? std::max(1, static_cast<int>(
                                std::ceil((target_w - origin_x) / tile_w)))
               : 1;
    const int rows =
        tile_y ? std::max(1, static_cast<int>(
                                std::ceil((target_h - origin_y) / tile_h)))
               : 1;
    for (int row = 0; row < rows; row++) {
      for (int column = 0; column < columns; column++) {
        const double tx = tile_x ? origin_x + column * tile_w : origin_x;
        const double ty = tile_y ? origin_y + row * tile_h : origin_y;
        canvas->builder()->DrawImageRect(
            image, flutter::DlRect::MakeLTRB(0.0, 0.0, image_width, image_height),
            flutter::DlRect::MakeLTRB(tx, ty, tx + tile_w, ty + tile_h),
            flutter::DlImageSampling::kLinear, nullptr);
      }
    }
  }
  canvas->restore();
}

void DirectRenderImagePaint(intptr_t ptr, Dart_Handle canvas_handle,
                            double width, double height, double offset_x,
                            double offset_y) {
  auto found = g_image_nodes.find(ptr);
  if (found == g_image_nodes.end() || found->second == nullptr) return;
  AppNode* node = found->second;
  if (!node->image_dl) return;

  auto* canvas = tonic::DartConverter<flutter::Canvas*>::FromDart(canvas_handle);
  SKY_LOG(kDebug, kLogRuntime,
          "image paint: ptr=0x%lx canvas=%p %dx%d size=%gx%g at (%g,%g)",
          static_cast<long>(ptr), static_cast<void*>(canvas),
          node->image_dl->width(), node->image_dl->height(), width, height,
          offset_x, offset_y);
  if (canvas == nullptr || canvas->builder() == nullptr) {
    return;
  }

  // Resolve `objectFit` (`mode`/`objectFit` attributes or CSS object-fit).
  int fit = node->style.object_fit;
  static const char* kFitKeys[] = {"objectFit", "object-fit", "mode"};
  for (const char* key : kFitKeys) {
    const auto found = node->attrs.find(key);
    if (found == node->attrs.end() || found->second.empty()) continue;
    const std::string value = found->second;
    if (value == "aspectFit" || value == "contain") {
      fit = 1;
    } else if (value == "aspectFill" || value == "cover") {
      fit = 2;
    } else if (value == "center" || value == "none") {
      fit = 3;
    } else if (value == "scaleToFill" || value == "fill") {
      fit = 0;
    } else if (value == "widthFix" || value == "heightFix") {
      fit = 0;  // the size itself already encodes the preserved aspect ratio
    }
    break;
  }
  const int repeat = ImageRepeatMode(node);
  CssPositionAxis pos_x = node->style.object_position_x;
  CssPositionAxis pos_y = node->style.object_position_y;
  if (repeat != 0 && !node->style.has_object_position) {
    // Tiled images start their tile grid at the top-left unless an explicit
    // object-position was provided.
    pos_x = CssPositionAxis{0.0f, true, false};
    pos_y = CssPositionAxis{0.0f, true, false};
  }
  static const char* kPositionKeys[] = {"objectPosition", "object-position"};
  for (const char* key : kPositionKeys) {
    const auto found = node->attrs.find(key);
    if (found == node->attrs.end() || found->second.empty()) continue;
    ParsePositionPair(found->second, &pos_x, &pos_y);
    break;
  }
  DrawImageInBox(canvas, node->image_dl, width, height, fit, repeat,
                 offset_x, offset_y, pos_x, pos_y);
}

// CSS background-image painting through NewDecoratedBox's onPaint callback.
void BackgroundImageCreate(intptr_t /*ptr*/) {}
void BackgroundImageDisposed(intptr_t /*ptr*/) {}

// Resolves explicit `background-size` values against the background painting
// area.  Returns false when the default fit handling should be used instead.
bool ResolveBackgroundSize(const AppStyle& style,
                           double image_w,
                           double image_h,
                           double box_w,
                           double box_h,
                           double* dest_w,
                           double* dest_h) {
  if (!style.has_background_size || style.background_fit != 0) return false;
  const bool has_w = style.background_size_width >= 0.0f;
  const bool has_h = style.background_size_height >= 0.0f;
  if (!has_w && !has_h) {
    // `background-size: auto` keeps the intrinsic image dimensions.
    *dest_w = image_w;
    *dest_h = image_h;
    return true;
  }
  double w = has_w ? (style.background_size_width_percent
                          ? style.background_size_width * box_w
                          : style.background_size_width)
                   : -1.0;
  double h = has_h ? (style.background_size_height_percent
                          ? style.background_size_height * box_h
                          : style.background_size_height)
                   : -1.0;
  if (w < 0.0 && h >= 0.0) w = image_w * h / image_h;
  if (h < 0.0 && w >= 0.0) h = image_h * w / image_w;
  *dest_w = std::max(0.0, w);
  *dest_h = std::max(0.0, h);
  return true;
}

void BackgroundImagePaint(intptr_t ptr, Dart_Handle canvas_handle, double left,
                          double top, double right, double bottom,
                          Dart_Handle /*path*/) {
  auto found = g_background_image_nodes.find(ptr);
  if (found == g_background_image_nodes.end() || found->second == nullptr) return;
  AppNode* node = found->second;
  if (!node->background_image_dl) return;
  auto* canvas = tonic::DartConverter<flutter::Canvas*>::FromDart(canvas_handle);
  if (canvas == nullptr || canvas->builder() == nullptr) return;
  const double width = std::max(0.0, right - left);
  const double height = std::max(0.0, bottom - top);
  double dest_w = 0.0;
  double dest_h = 0.0;
  const bool size_override = ResolveBackgroundSize(
      node->style, node->background_image_dl->width(),
      node->background_image_dl->height(), width, height, &dest_w, &dest_h);
  DrawImageInBox(canvas, node->background_image_dl, width, height,
                 node->style.background_fit, node->style.background_repeat, left,
                 top, node->style.background_position_x,
                 node->style.background_position_y, size_override, dest_w,
                 dest_h);
}

// `package:skyline` registers this Dart callback; native calls it to perform
// the actual HTTP request (dart:io) and delivers the response through the
// supplied C function pointer.
using RequestHttpResourceFn = void (*)(uint32_t request_context_id,
                                       Dart_Handle url,
                                       void* native_callback);
using CancelRequestHttpResourceFn = void (*)(uint32_t request_context_id);
// `SvgToRgba` renders SVG through package:flutter_svg on the Dart side and
// returns raw RGBA bytes to the native callback.
using SvgToRgbaCallbackFn = void (*)(intptr_t runtime_ptr, intptr_t ptr,
                                      int32_t id, Dart_Handle data,
                                      int32_t width, int32_t height);
using SvgToRgbaFn = void (*)(intptr_t ptr, double pixel_ratio, int32_t id,
                             Dart_Handle url, Dart_Handle raw, double width,
                             double height, void* callback);

constexpr uint32_t kSvgToRgbaHash = 543998557u;
constexpr uint32_t kCancelRequestHttpResourceHash = 653300188u;

sk_sp<flutter::DlImage> WrapSkImage(sk_sp<SkImage> sk_image) {
  if (!sk_image) return nullptr;
  flutter::SkiaGPUObject<SkImage> gpu_image = {std::move(sk_image), nullptr};
  return flutter::DlImageGPU::Make(std::move(gpu_image));
}

sk_sp<flutter::DlImage> DecodeEncodedImage(const uint8_t* data, size_t size) {
  if (data == nullptr || size == 0) return nullptr;
  sk_sp<SkData> sk_data = SkData::MakeWithCopy(data, size);
  return WrapSkImage(SkImages::DeferredFromEncodedData(std::move(sk_data)));
}

sk_sp<flutter::DlImage> DecodeRgbaImage(const uint8_t* data, size_t size,
                                        int width, int height) {
  if (data == nullptr || width <= 0 || height <= 0) return nullptr;
  const size_t expected = static_cast<size_t>(width) * height * 4;
  if (size < expected) return nullptr;
  SkImageInfo info =
      SkImageInfo::Make(width, height, kBGRA_8888_SkColorType,
                        kPremul_SkAlphaType);
  SkBitmap bitmap;
  if (!bitmap.tryAllocPixels(info)) return nullptr;
  std::memcpy(bitmap.getPixels(), data, expected);
  return WrapSkImage(SkImages::RasterFromBitmap(bitmap));
}

bool SourceLooksLikeSvg(const std::string& source, const uint8_t* data,
                        size_t size) {
  std::string path = source;
  const size_t query = path.find('?');
  if (query != std::string::npos) path.resize(query);
  if (EndsWith(LowerAscii(path), ".svg")) return true;
  if (data == nullptr || size == 0) return false;
  size_t index = 0;
  while (index < size && std::isspace(static_cast<unsigned char>(data[index]))) {
    index++;
  }
  if (index + 5 <= size && std::memcmp(data + index, "<?xml", 5) == 0) {
    return true;
  }
  return index + 4 <= size && std::memcmp(data + index, "<svg", 4) == 0;
}

void AssignNodeImage(AppNode* node, const sk_sp<flutter::DlImage>& image,
                     bool background) {
  sk_sp<flutter::DlImage>& target =
      background ? node->background_image_dl : node->image_dl;
  if (target && target != image) {
    RetiredImages().push_back(std::move(target));
  }
  target = image;
  node->image_loaded = true;
  node->image_pending = false;
  SKY_LOG(kDebug, kLogRuntime, "image #%u: %s (%dx%d)", node->id,
          background ? "background image" : "image", image ? image->width() : 0,
          image ? image->height() : 0);
}

void RebuildAppServicePage(RuntimeHolder* holder);

void RepaintAfterImage(RuntimeHolder* holder, bool background,
                       Dart_PersistentHandle decorated_box) {
  auto runner = UiTaskRunner();
  if (!runner) return;
  runner->PostTask([holder, background, decorated_box]() {
    DemoPageApiScope scope;
    if (background && decorated_box != nullptr && holder != nullptr) {
      auto* mark = reinterpret_cast<void (*)(Dart_Handle)>(
          FindCallback(holder->ffi_table, kBackgroundImageMarkNeedPaintHash));
      if (mark != nullptr) {
        mark(Dart_HandleFromPersistent(decorated_box));
        return;
      }
    }
    RebuildAppServicePage(holder);
  });
}

// Marks the existing direct-render image widget dirty.  Unlike
// `RebuildAppServicePage`, this reaches nodes on covered routes too (the
// replay keeps the previous page's widgets alive for non-opaque transitions).
// The lookup happens on the UI thread so the node's current persistent handle
// is used (a queued rebuild may have replaced it in the meantime).
void MarkNodeImageDirty(intptr_t address, RuntimeHolder* holder) {
  if (holder == nullptr || address == 0) return;
  auto runner = UiTaskRunner();
  if (!runner) return;
  runner->PostTask([holder, address]() {
    DemoPageApiScope scope;
    auto found = g_image_nodes.find(address);
    if (found == g_image_nodes.end() || found->second == nullptr) return;
    Dart_PersistentHandle widget = found->second->image_widget_p;
    if (widget == nullptr) return;
    auto* mark = reinterpret_cast<void (*)(Dart_Handle)>(
        FindCallback(holder->ffi_table, kImageMarkNeedPaintHash));
    if (mark != nullptr) mark(Dart_HandleFromPersistent(widget));
  });
}

struct PendingImageRequest {
  RuntimeHolder* holder = nullptr;
  std::string source;
  std::vector<AppNode*> nodes;
  // True while the request is proxied through the DevTools pageframe fetch.
  bool via_shell = false;
};

struct PendingSvgRequest {
  AppNode* node = nullptr;
  RuntimeHolder* holder = nullptr;
  bool background = false;
};

std::unordered_map<uint32_t, PendingImageRequest> g_pending_image_requests;
std::unordered_map<std::string, uint32_t> g_pending_image_by_source;
std::unordered_map<int32_t, PendingSvgRequest> g_pending_svg_requests;
uint32_t g_next_image_request_id = 0x40000000u;
int32_t g_next_svg_request_id = 0x30000000;

void RebuildAppServicePage(RuntimeHolder* holder);

void CancelNetworkImage(AppNode* node) {
  if (!node->image_pending || node->image_request_id == 0) return;
  auto found = g_pending_image_requests.find(node->image_request_id);
  if (found != g_pending_image_requests.end()) {
    auto& nodes = found->second.nodes;
    nodes.erase(std::remove(nodes.begin(), nodes.end(), node), nodes.end());
    if (nodes.empty()) {
      SKY_LOG(kDebug, kLogRuntime, "image request %u cancelled (%s)",
              node->image_request_id, found->second.source.c_str());
      g_pending_image_by_source.erase(found->second.source);
      RuntimeHolder* holder = found->second.holder;
      if (holder != nullptr) {
        auto* cancel = reinterpret_cast<CancelRequestHttpResourceFn>(
            FindCallback(holder->ffi_table, kCancelRequestHttpResourceHash));
        if (cancel != nullptr) cancel(node->image_request_id);
      }
      g_pending_image_requests.erase(found);
    }
  }
  node->image_pending = false;
  node->image_request_id = 0;
}

void CancelPendingImage(AppNode* node) {
  if (node == nullptr) return;
  CancelNetworkImage(node);
  node->image_source.clear();
  node->image_loaded = false;
  node->image_failed = false;
  node->image_pending = false;
  if (node->image_dl) RetiredImages().push_back(std::move(node->image_dl));
}

void OnSvgToRgbaResponse(intptr_t /*runtime_ptr*/, intptr_t /*ptr*/, int32_t id,
                         Dart_Handle data, int32_t width, int32_t height) {
  auto found = g_pending_svg_requests.find(id);
  if (found == g_pending_svg_requests.end()) return;
  PendingSvgRequest pending = found->second;
  g_pending_svg_requests.erase(found);
  AppNode* node = pending.node;
  if (node == nullptr || node->released) return;

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
  sk_sp<flutter::DlImage> image =
      DecodeRgbaImage(bytes.data(), bytes.size(), width, height);
  if (!image) {
    SKY_LOG(kInfo, kLogRuntime,
            "image #%u: SVG payload unusable (%dx%d, %zu bytes)", node->id,
            width, height, bytes.size());
    node->image_failed = true;
    node->image_pending = false;
    return;
  }
  CacheImage(node->image_source, image);
  AssignNodeImage(node, image, pending.background);
  if (!pending.background) MarkNodeImageDirty(node->address, pending.holder);
  RepaintAfterImage(pending.holder, pending.background,
                    pending.background ? node->decorated_box_p : nullptr);
}

// Hands an SVG payload to package:skyline's `SvgToRgba` callback.
bool StartSvgDecode(AppNode* node, const std::string& source,
                    const std::vector<uint8_t>& bytes, bool background,
                    RuntimeHolder* holder) {
  if (holder == nullptr || node == nullptr) return false;
  auto* svg = reinterpret_cast<SvgToRgbaFn>(
      FindCallback(holder->ffi_table, kSvgToRgbaHash));
  if (svg == nullptr) {
    SKY_LOG(kWarning, kLogRuntime, "image #%u: SvgToRgba callback missing",
            node->id);
    return false;
  }
  Dart_Handle raw =
      Dart_NewTypedData(Dart_TypedData_kUint8,
                        static_cast<intptr_t>(bytes.size()));
  if (Dart_IsError(raw)) return false;
  Dart_TypedData_Type type = Dart_TypedData_kInvalid;
  void* data = nullptr;
  intptr_t length = 0;
  if (!Dart_IsError(Dart_TypedDataAcquireData(raw, &type, &data, &length)) &&
      data != nullptr) {
    std::memcpy(data, bytes.data(), bytes.size());
  }
  Dart_TypedDataReleaseData(raw);
  Dart_Handle url = Dart_NewStringFromCString(source.c_str());
  if (Dart_IsError(url)) return false;
  const int32_t id = ++g_next_svg_request_id;
  g_pending_svg_requests[id] = PendingSvgRequest{node, holder, background};
  node->image_pending = true;
  node->image_source = source;
  node->image_target_background = background;
  SKY_LOG(kDebug, kLogRuntime, "image #%u: SVG %s (request=%d)", node->id,
          source.c_str(), id);
  svg(node->address, 1.0, id, url, raw, 0.0, 0.0,
      reinterpret_cast<void*>(&OnSvgToRgbaResponse));
  return true;
}

// Dart's `RequestHttpResource` performs the fallback request (dart:io).  The
// response arrives on the UI task runner; decode once and fan the result out to
// every node waiting on that URL.
void DeliverImagePayload(uint32_t request_id, std::vector<uint8_t> bytes);

extern "C" void OnImageResourceResponse(intptr_t /*runtime_ptr*/,
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
  DeliverImagePayload(request_id, std::move(bytes));
}

void IssueDartImageRequest(RuntimeHolder* holder, uint32_t request_id,
                           const std::string& source) {
  void* request = FindCallback(
      holder != nullptr ? holder->ffi_table : nullptr,
      kRequestHttpResourceHash);
  if (request == nullptr) {
    SKY_LOG(kWarning, kLogRuntime,
            "image request %u: RequestHttpResource missing", request_id);
    DeliverImagePayload(request_id, {});
    return;
  }
  Dart_Handle url = Dart_NewStringFromCString(source.c_str());
  SKY_LOG(kDebug, kLogRuntime, "image request %u starting (dart:io)",
          request_id);
  reinterpret_cast<RequestHttpResourceFn>(request)(
      request_id, url, reinterpret_cast<void*>(&OnImageResourceResponse));
}

void StartDartImageRequest(RuntimeHolder* holder, uint32_t request_id,
                           const std::string& source) {
  auto runner = UiTaskRunner();
  if (!runner) runner = SharedUiRunner();
  if (!runner) {
    DeliverImagePayload(request_id, {});
    return;
  }
  runner->PostTask([holder, request_id, source]() {
    DemoPageApiScope scope;
    IssueDartImageRequest(holder, request_id, source);
  });
}

// Entry point shared by the Dart fallback and the DevTools shell fetch.
void DeliverImagePayload(uint32_t request_id, std::vector<uint8_t> bytes) {
  SKY_DCHECK_UI_THREAD();
  auto found = g_pending_image_requests.find(request_id);
  if (found == g_pending_image_requests.end()) {
    SKY_LOG(kWarning, kLogRuntime, "image response for unknown request %u",
            request_id);
    return;
  }
  PendingImageRequest pending = found->second;
  if (bytes.empty() && pending.via_shell) {
    // The DevTools pageframe fetch can fail on CORS/offline; retry through the
    // in-engine dart:io client so images still render.
    pending.via_shell = false;
    g_pending_image_requests[request_id] = pending;
    SKY_LOG(kInfo, kLogRuntime,
            "image request %u: shell fetch empty, falling back to dart:io",
            request_id);
    StartDartImageRequest(pending.holder, request_id, pending.source);
    return;
  }
  g_pending_image_requests.erase(found);
  g_pending_image_by_source.erase(pending.source);

  if (bytes.empty()) {
    for (AppNode* node : pending.nodes) {
      if (node != nullptr && !node->released && node->image_source == pending.source) {
        node->image_failed = true;
        node->image_pending = false;
      }
    }
    return;
  }
  if (SourceLooksLikeSvg(pending.source, bytes.data(), bytes.size())) {
    // SVG rasterization is per-node (it needs a Dart isolate scope), but the
    // payload is fetched only once.
    for (AppNode* node : pending.nodes) {
      if (node == nullptr || node->released ||
          node->image_source != pending.source) {
        continue;
      }
      node->image_pending = false;
      DemoPageApiScope scope;
      StartSvgDecode(node, pending.source, bytes, node->image_target_background,
                     pending.holder);
    }
    return;
  }

  sk_sp<flutter::DlImage> image = DecodeEncodedImage(bytes.data(), bytes.size());
  if (!image) {
    for (AppNode* node : pending.nodes) {
      if (node != nullptr && !node->released && node->image_source == pending.source) {
        node->image_failed = true;
        node->image_pending = false;
      }
    }
    SKY_LOG(kInfo, kLogRuntime, "network payload unusable (%zu bytes)",
            bytes.size());
    return;
  }
  CacheImage(pending.source, image);
  bool any_background = false;
  Dart_PersistentHandle decorated = nullptr;
  bool any = false;
  for (AppNode* node : pending.nodes) {
    if (node == nullptr || node->released ||
        node->image_source != pending.source) {
      continue;
    }
    AssignNodeImage(node, image, node->image_target_background);
    any = true;
    if (node->image_target_background) {
      any_background = true;
      decorated = node->decorated_box_p;
    } else {
      // Also refresh the widget on routes covered by the current page (the
      // list under a non-opaque custom route); a page rebuild would not.
      MarkNodeImageDirty(node->address, pending.holder);
    }
  }
  SKY_LOG(kInfo, kLogRuntime,
          "network image (%zu bytes) for %zu node(s), rebuilding page",
          bytes.size(), pending.nodes.size());
  if (any) RepaintAfterImage(pending.holder, any_background, decorated);
}

bool StartNetworkImage(AppNode* node, const std::string& source,
                       bool background, RuntimeHolder* holder) {
  if (node->image_pending) return false;
  const bool via_shell = ShellHttpAvailable();
  if (!via_shell &&
      FindCallback(holder != nullptr ? holder->ffi_table : nullptr,
                   kRequestHttpResourceHash) == nullptr) {
    SKY_LOG(kWarning, kLogRuntime, "image #%u: RequestHttpResource missing",
            node->id);
    node->image_failed = true;
    return false;
  }
  node->image_source = source;
  node->image_target_background = background;
  node->image_pending = true;

  auto pending = g_pending_image_by_source.find(source);
  if (pending != g_pending_image_by_source.end()) {
    auto& entry = g_pending_image_requests[pending->second];
    if (std::find(entry.nodes.begin(), entry.nodes.end(), node) ==
        entry.nodes.end()) {
      entry.nodes.push_back(node);
    }
    node->image_request_id = pending->second;
    SKY_LOG(kDebug, kLogRuntime,
            "image #%u: join pending fetch %s (request=%u)", node->id,
            source.c_str(), pending->second);
    return false;
  }

  const uint32_t request_id = ++g_next_image_request_id;
  node->image_request_id = request_id;
  PendingImageRequest entry;
  entry.holder = holder;
  entry.source = source;
  entry.nodes.push_back(node);
  entry.via_shell = via_shell;
  g_pending_image_requests[request_id] = entry;
  g_pending_image_by_source[source] = request_id;
  SKY_LOG(kDebug, kLogRuntime, "image #%u: fetch %s (request=%u)", node->id,
          source.c_str(), request_id);
  // Start the request from a follow-up UI task: the Dart-side
  // `RequestHttpResource` is an async callback and must not be entered from
  // the middle of page construction.
  auto runner = UiTaskRunner();
  if (!runner) runner = SharedUiRunner();
  if (runner) {
    runner->PostTask([holder, request_id, source, via_shell, runner]() {
      if (via_shell) {
        SKY_LOG(kDebug, kLogRuntime, "image request %u via DevTools shell",
                request_id);
        RequestShellHttp(request_id, source);
        // The DevTools fetch callback has no error return path; if it never
        // completes (CORS, pageframe teardown), fall back to the in-engine
        // dart:io client so the image still renders.
        runner->PostDelayedTask(
            [holder, request_id, source]() {
              auto found = g_pending_image_requests.find(request_id);
              if (found == g_pending_image_requests.end() ||
                  !found->second.via_shell) {
                return;
              }
              found->second.via_shell = false;
              SKY_LOG(
                  kWarning, kLogRuntime,
                  "image request %u: shell timeout, falling back to dart:io",
                  request_id);
              StartDartImageRequest(holder, request_id, source);
            },
            fml::TimeDelta::FromMilliseconds(2000));
        return;
      }
      DemoPageApiScope scope;
      IssueDartImageRequest(holder, request_id, source);
    });
  }
  return false;
}

int HexValue(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

bool DecodeBase64(const std::string& input, std::vector<uint8_t>* out) {
  int value = 0;
  int bits = -8;
  for (char c : input) {
    int digit = -1;
    if (c >= 'A' && c <= 'Z') digit = c - 'A';
    else if (c >= 'a' && c <= 'z') digit = c - 'a' + 26;
    else if (c >= '0' && c <= '9') digit = c - '0' + 52;
    else if (c == '+' || c == '-') digit = 62;
    else if (c == '/' || c == '_') digit = 63;
    else if (c == '=') break;
    else continue;  // whitespace / newlines
    value = (value << 6) | digit;
    bits += 6;
    if (bits >= 0) {
      out->push_back(static_cast<uint8_t>((value >> bits) & 0xFF));
      bits -= 8;
    }
  }
  return !out->empty();
}

// RFC 2397 data URIs (base64 or percent-encoded) are used for inline SVG icons
// such as the navigation bar's back chevron.
bool DecodeDataUri(const std::string& uri, std::vector<uint8_t>* out) {
  if (!StartsWith(uri, "data:")) return false;
  const size_t comma = uri.find(',');
  if (comma == std::string::npos) return false;
  const std::string meta = LowerAscii(uri.substr(5, comma - 5));
  const std::string payload = uri.substr(comma + 1);
  if (meta.find(";base64") != std::string::npos) {
    return DecodeBase64(payload, out);
  }
  out->clear();
  out->reserve(payload.size());
  for (size_t i = 0; i < payload.size(); i++) {
    if (payload[i] == '%' && i + 2 < payload.size()) {
      const int hi = HexValue(payload[i + 1]);
      const int lo = HexValue(payload[i + 2]);
      if (hi >= 0 && lo >= 0) {
        out->push_back(static_cast<uint8_t>((hi << 4) | lo));
        i += 2;
        continue;
      }
    }
    out->push_back(static_cast<uint8_t>(payload[i]));
  }
  return !out->empty();
}

bool LoadImageSource(AppNode* node, const std::string& source, bool background,
                     RuntimeHolder* holder) {
  if (source.empty()) {
    SKY_LOG(kInfo, kLogRuntime, "image #%u: no source", node->id);
    node->image_failed = true;
    return false;
  }
  auto cached = ImageCache().find(source);
  if (cached != ImageCache().end() && cached->second) {
    AssignNodeImage(node, cached->second, background);
    SKY_LOG(kDebug, kLogRuntime, "image #%u: cache hit %s", node->id,
            source.c_str());
    return true;
  }

  if (StartsWith(source, "http://") || StartsWith(source, "https://")) {
    return StartNetworkImage(node, source, background, holder);
  }

  if (StartsWith(source, "data:")) {
    std::vector<uint8_t> bytes;
    if (!DecodeDataUri(source, &bytes)) {
      SKY_LOG(kInfo, kLogRuntime, "image #%u: bad data URI", node->id);
      node->image_failed = true;
      return false;
    }
    if (SourceLooksLikeSvg(source, bytes.data(), bytes.size())) {
      return StartSvgDecode(node, source, bytes, background, holder);
    }
    sk_sp<flutter::DlImage> image =
        DecodeEncodedImage(bytes.data(), bytes.size());
    if (!image) {
      SKY_LOG(kWarning, kLogRuntime, "image #%u: cannot decode data URI",
              node->id);
      node->image_failed = true;
      return false;
    }
    CacheImage(source, image);
    AssignNodeImage(node, image, background);
    return true;
  }

  const std::string resolved = ResolveResourcePath(source);
  std::vector<uint8_t> bytes;
  if (!ReadResourceBytes(resolved, &bytes) || bytes.empty()) {
    // Package-local assets are not on the addon's filesystem in the DevTools;
    // route them through the shell resource loader (the JS side reads the
    // project file and completes the same request id).
    if (ShellHttpAvailable()) {
      SKY_LOG(kInfo, kLogRuntime,
              "image #%u: %s not on disk; requesting through the DevTools "
              "resource loader",
              node->id, source.c_str());
      return StartNetworkImage(node, source, background, holder);
    }
    SKY_LOG(kWarning, kLogRuntime, "image #%u: cannot read %s", node->id,
            resolved.c_str());
    node->image_failed = true;
    return false;
  }
  if (SourceLooksLikeSvg(source, bytes.data(), bytes.size())) {
    return StartSvgDecode(node, source, bytes, background, holder);
  }
  sk_sp<flutter::DlImage> image = DecodeEncodedImage(bytes.data(), bytes.size());
  if (!image) {
    SKY_LOG(kWarning, kLogRuntime, "image #%u: cannot decode %s", node->id,
            source.c_str());
    node->image_failed = true;
    return false;
  }
  CacheImage(source, image);
  AssignNodeImage(node, image, background);
  if (background) {
    SKY_LOG(kDebug, kLogRuntime,
            "background image #%u: %s -> %s (%zu bytes, %dx%d)", node->id,
            source.c_str(), resolved.c_str(), bytes.size(), image->width(),
            image->height());
  } else {
    SKY_LOG(kDebug, kLogRuntime, "image #%u: %s -> %s (%zu bytes, %dx%d)",
            node->id, source.c_str(), resolved.c_str(), bytes.size(),
            image->width(), image->height());
  }
  return true;
}

bool LoadImageForNode(AppNode* node) {
  if (node->image_loaded) return true;
  if (node->image_failed || node->image_pending) return false;
  static const char* kSourceKeys[] = {"src", "url", "imageUrl", "image",
                                      "path"};
  std::string source;
  for (const char* key : kSourceKeys) {
    const auto found = node->attrs.find(key);
    if (found != node->attrs.end() && !found->second.empty()) {
      source = found->second;
      break;
    }
  }
  if (source.empty()) {
    SKY_LOG(kInfo, kLogRuntime, "image #%u: no src attribute", node->id);
    node->image_failed = true;
    return false;
  }
  RuntimeHolder* holder = g_page != nullptr ? g_page->holder : nullptr;
  return LoadImageSource(node, source, false, holder);
}

bool LoadBackgroundImageForNode(AppNode* node) {
  if (node == nullptr || node->style.background_image.empty()) return false;
  if (node->background_image_dl != nullptr || node->image_failed ||
      node->image_pending) {
    return node->background_image_dl != nullptr;
  }
  RuntimeHolder* holder = g_page != nullptr ? g_page->holder : nullptr;
  return LoadImageSource(node, node->style.background_image, true, holder);
}

// Builder nodes are slivers.  The Dart side calls back with `(address, index)`
// whenever the sliver needs an item; the callback schedules the mount on the
// UI runner and materializes the captured item node on demand.

}  // namespace appservice

