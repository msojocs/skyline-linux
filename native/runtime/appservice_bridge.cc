// Live DevTools bridge exports.

#include "internal/skyline_runtime_module.hh"

// SkylineGlobal.PageContext on the UI task runner.  `ApplyOpsPage` consumes the
// same JSON schema as the recorded appservice fixtures.

// Pending live page ops.  The addon may call SkylineApplyLiveOps from the
// Electron renderer thread before the Dart app has registered its UI task
// runner, so the payload is parked here until NotifyFlutterReady runs on the
// engine UI thread.
void ScheduleLiveOps(std::string payload, int attempt) {
  auto runner = UiTaskRunner();
  if (!runner) return;
  const auto scheduled = std::chrono::steady_clock::now();
  std::fprintf(stderr,
               "[skyline-runtime] live ops scheduled (attempt=%d, bytes=%zu)\n",
               attempt, payload.size());
  // Run the batch immediately; the Dart app may not have registered its
  // callbacks yet, in which case we retry after 500 ms.  (The old code delayed
  // every batch by 500 ms, which alone made every setData visibly late.)
  runner->PostTask([payload, attempt, scheduled, runner]() {
    const auto started = std::chrono::steady_clock::now();
    const auto delay_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                              started - scheduled)
                              .count();
    std::fprintf(stderr,
                 "[skyline-runtime] live ops begin delay=%lldms bytes=%zu\n",
                 static_cast<long long>(delay_ms), payload.size());
    DemoPageApiScope scope;
    if (g_isolate == nullptr || g_fallback_holder.ffi_table == nullptr) {
      if (attempt < 20) {
        runner->PostDelayedTask(
            [payload, attempt]() { ScheduleLiveOps(payload, attempt + 1); },
            fml::TimeDelta::FromMilliseconds(500));
      } else {
        std::fprintf(stderr,
                     "[skyline-runtime] live page: Dart app not ready\n");
      }
      return;
    }
    // Batches share one persistent page: the bridge may flush element
    // creation before the attach and deltas afterwards.
    appservice::ApplyLiveOpsBatch(&g_fallback_holder, payload.c_str());
    const auto finished = std::chrono::steady_clock::now();
    std::fprintf(stderr, "[skyline-runtime] live ops end processing=%lldms\n",
                 static_cast<long long>(
                     std::chrono::duration_cast<std::chrono::milliseconds>(
                         finished - started)
                         .count()));
  });
}

// UI thread: remember the engine task runner and flush any page ops that were
// queued before the Dart app was ready.
void ApplyPendingLiveOpsOnUi() {
  std::string payload;
  {
    std::lock_guard<std::mutex> lock(g_live_ops_mutex);
    g_live_ready = true;
    g_live_ui_runner = UiTaskRunner();
    if (g_pending_live_ops.empty()) return;
    payload = std::move(g_pending_live_ops);
    g_pending_live_ops.clear();
  }
  ScheduleLiveOps(std::move(payload), 0);
}

SKYLINE_EXPORT extern "C" int32_t SkylineGetNodeRect(uint32_t node_id,
                                                     float* out) {
  if (out == nullptr) return 0;
  std::lock_guard<std::mutex> lock(appservice::g_rect_mutex);
  const auto found = appservice::g_rect_snapshot.find(node_id);
  if (found == appservice::g_rect_snapshot.end()) return 0;
  out[0] = found->second.left;
  out[1] = found->second.top;
  out[2] = found->second.width;
  out[3] = found->second.height;
  out[4] = found->second.z_index;
  out[5] = found->second.pointer_events;
  return 1;
}

SKYLINE_EXPORT extern "C" void SkylineSetScrollEventSink(
    appservice::SkylineScrollEventSink sink) {
  appservice::g_scroll_event_sink.store(sink);
}

SKYLINE_EXPORT extern "C" void SkylineSetWidgetEventSink(
    appservice::SkylineWidgetEventSink sink) {
  appservice::g_widget_event_sink.store(sink);
}

SKYLINE_EXPORT extern "C" void SkylineSetJsonEventSink(
    appservice::SkylineJsonEventSink sink) {
  appservice::g_json_event_sink.store(sink);
}

SKYLINE_EXPORT extern "C" void SkylineAddIntersectionObserver(
    uint32_t node_id, uint32_t unique_id, uint32_t root_id, double margin_top,
    double margin_right, double margin_bottom, double margin_left,
    const double* thresholds, int32_t threshold_count) {
  std::vector<double> list;
  if (thresholds != nullptr && threshold_count > 0) {
    list.assign(thresholds, thresholds + threshold_count);
  }
  if (list.empty()) list.push_back(0.0);
  PostToUiIfReady([node_id, unique_id, root_id, margin_top, margin_right,
                   margin_bottom, margin_left, list]() {
    appservice::RegisterIntersectionObserver(node_id, unique_id, root_id,
                                             margin_top, margin_right,
                                             margin_bottom, margin_left, list);
  });
}

SKYLINE_EXPORT extern "C" void SkylineRemoveIntersectionObserver(
    uint32_t node_id, uint32_t unique_id) {
  PostToUiIfReady([node_id, unique_id]() {
    appservice::UnregisterIntersectionObserver(node_id, unique_id);
  });
}

SKYLINE_EXPORT extern "C" void SkylineSelectionSetRange(
    uint32_t selection_id, uint32_t start_id, int32_t start_offset,
    uint32_t end_id, int32_t end_offset) {
  PostToUiIfReady([selection_id, start_id, start_offset, end_id, end_offset]() {
    appservice::SetSelectionRange(selection_id, start_id, start_offset, end_id,
                                  end_offset);
  });
}

SKYLINE_EXPORT extern "C" void SkylineRefresherTriggered(
    uint32_t node_id, uint8_t open, uint8_t two_level) {
  PostToUiIfReady([node_id, open, two_level]() {
    appservice::SetRefresherTriggered(node_id, open != 0, two_level != 0);
  });
}

SKYLINE_EXPORT extern "C" void SkylineSetNavigateBackInterception(
    int32_t page_id, uint8_t enabled) {
  PostToUiIfReady([page_id, enabled]() {
    appservice::SetPageNavigateBackInterception(page_id, enabled != 0);
  });
}

// ---------------------------------------------------------------------------
// D10: accessibility + DevTools automation bridge (addon -> embedded runtime)
// ---------------------------------------------------------------------------

SKYLINE_EXPORT extern "C" void SkylineSetAccessibilitySink(
    SkylineAccessibilitySink sink) {
  g_accessibility_sink.store(sink);
  // Replay the state the Dart app reported before the host installed its sink.
  if (sink != nullptr) {
    if (g_accessibility_enabled.load() != 0) sink(1, 1);
  }
}

SKYLINE_EXPORT extern "C" int32_t SkylineGetAccessibilityState() {
  return (g_accessibility_enabled.load() != 0 ? 1 : 0) |
         (g_accessibility_bold_text.load() != 0 ? 2 : 0);
}

// Programmatic accessibility toggle (DevTools automation / appservice ops).
SKYLINE_EXPORT extern "C" void SkylineRequestAccessibility(uint8_t enabled) {
  g_accessibility_enabled.store(enabled != 0 ? 1 : 0);
  appservice::NotifyAccessibilitySink(1, enabled != 0 ? 1 : 0);
  appservice::NotifyAccessibilitySink(0, enabled != 0 ? 1 : 0);
}

SKYLINE_EXPORT extern "C" void SkylineDumpElementTree() {
  PostToUiIfReady([]() { appservice::DumpTree(true); });
}

SKYLINE_EXPORT extern "C" void SkylineDumpRenderTree() {
  PostToUiIfReady([]() { appservice::DumpTree(false); });
}

SKYLINE_EXPORT extern "C" void SkylineCollectGarbage() {
  PostToUiIfReady([]() { appservice::CollectGarbage(); });
}

SKYLINE_EXPORT extern "C" void SkylineSendMessageToRunner(const char* message) {
  std::string text = message != nullptr ? message : "";
  PostToUiIfReady([text = std::move(text)]() {
    appservice::SendMessageToRunner(text);
  });
}

SKYLINE_EXPORT extern "C" void SkylineTakeAppSnapshot(
    uint32_t request_id, double scale, uint8_t compressed) {
  PostToUiIfReady([request_id, scale, compressed]() {
    appservice::TakeAppSnapshot(request_id, scale, compressed != 0);
  });
}

SKYLINE_EXPORT extern "C" void SkylineSnapshotNode(uint32_t node_id,
                                                   uint8_t format,
                                                   uint32_t request_id) {
  PostToUiIfReady([node_id, format, request_id]() {
    appservice::SnapshotNode(node_id, format, request_id);
  });
}

SKYLINE_EXPORT extern "C" void SkylineSetUserGesture(uint8_t in_progress) {
  PostToUiIfReady([in_progress]() {
    appservice::SetUserGesture(in_progress != 0);
  });
}

SKYLINE_EXPORT extern "C" void SkylineRelaunchApp(uint8_t open_type) {
  PostToUiIfReady([open_type]() { appservice::RelaunchApp(open_type); });
}

SKYLINE_EXPORT extern "C" void SkylineShutdownApplication() {
  PostToUiIfReady([]() { appservice::ShutdownApplication(); });
}

// Dart reports the platform accessibility state (`SkylineApplication
// .notifyAccessibilityChanged`).  Persist it, drive the embedder's semantics
// tree, and query the registered AccessibilityBoldTextEnabled callback.
SKYLINE_EXPORT extern "C" void UpdateAccessibility(void* /*runtime_ptr*/,
                                                   uint8_t enabled) {
  g_accessibility_enabled.store(enabled != 0 ? 1 : 0);
  std::fprintf(stderr, "[skyline-runtime] UpdateAccessibility: enabled=%u\n",
               static_cast<unsigned>(enabled != 0));
  appservice::NotifyAccessibilitySink(1, enabled != 0 ? 1 : 0);
  appservice::NotifyAccessibilitySink(0, enabled != 0 ? 1 : 0);
#ifdef SKYLINE_EMBEDDED
  auto runner = UiTaskRunner();
  if (runner) {
    runner->PostTask([]() {
      DemoPageApiScope scope;
      const bool bold = appservice::QueryAccessibilityBoldText();
      g_accessibility_bold_text.store(bold ? 1 : 0);
      std::fprintf(stderr,
                   "[skyline-runtime] AccessibilityBoldTextEnabled: %d\n",
                   bold ? 1 : 0);
    });
  }
#endif
}

// Called by the framework when the platform accessibility features change.
SKYLINE_EXPORT extern "C" void OnAccessibilityFeaturesChanged(
    void* /*runtime_ptr*/, uint8_t bold_text) {
  g_accessibility_bold_text.store(bold_text != 0 ? 1 : 0);
  std::fprintf(stderr,
               "[skyline-runtime] OnAccessibilityFeaturesChanged: boldText=%u\n",
               static_cast<unsigned>(bold_text != 0));
  char buffer[96];
  std::snprintf(buffer, sizeof(buffer),
                "{\"type\":\"accessibilityFeatures\",\"boldText\":%s}",
                bold_text != 0 ? "true" : "false");
  appservice::EmitJsonEvent(buffer);
}

#ifdef SKYLINE_EMBEDDED
// `SkylineSnapshot.takeSnapshotAsync` builds an offscreen Scene and hands it
// to the native runtime (the private dart:ui has no `Scene.toImage`).  Flatten
// the layer tree into a CPU display list, rasterize it with Skia and call the
// Dart `OnSnapshotCallbackNative` with a Uint8List.  `format` follows
// ui.ImageByteFormat: 4 = png, other values = raw RGBA.
SKYLINE_EXPORT extern "C" void SceneToImage(Dart_Handle scene_handle,
                                            int32_t width, int32_t height,
                                            int32_t format,
                                            intptr_t callback_ptr) {
  std::vector<uint8_t> bytes;
  if (scene_handle != nullptr && !Dart_IsError(scene_handle) &&
      width > 0 && height > 0) {
    auto* scene =
        tonic::DartConverter<flutter::Scene*>::FromDart(scene_handle);
    if (scene != nullptr) {
      auto layer_tree = scene->takeLayerTree(static_cast<uint64_t>(width),
                                             static_cast<uint64_t>(height));
      if (layer_tree) {
        auto display_list = layer_tree->Flatten(
            flutter::DlRect::MakeLTRB(0.0, 0.0, static_cast<double>(width),
                                      static_cast<double>(height)),
            nullptr, nullptr);
        if (display_list) {
          const SkImageInfo info =
              SkImageInfo::Make(width, height, kRGBA_8888_SkColorType,
                                kPremul_SkAlphaType);
          auto surface = SkSurfaces::Raster(info);
          if (surface != nullptr) {
            flutter::DlSkCanvasDispatcher dispatcher(surface->getCanvas());
            display_list->Dispatch(dispatcher);
            if (format == 4) {  // ui.ImageByteFormat.png
              auto image = surface->makeImageSnapshot();
              sk_sp<SkData> encoded =
                  image != nullptr
                      ? SkPngEncoder::Encode(nullptr, image.get(), {})
                      : nullptr;
              if (encoded != nullptr) {
                bytes.assign(encoded->bytes(),
                             encoded->bytes() + encoded->size());
              }
            } else {
              bytes.resize(static_cast<size_t>(width) * height * 4);
              if (!surface->readPixels(info, bytes.data(),
                                       static_cast<size_t>(width) * 4, 0, 0)) {
                bytes.clear();
              }
            }
          }
        }
      }
    }
  }
  Dart_Handle data = Dart_NewTypedData(
      Dart_TypedData_kUint8, static_cast<intptr_t>(bytes.size()));
  if (!Dart_IsError(data) && !bytes.empty()) {
    Dart_TypedData_Type type = Dart_TypedData_kInvalid;
    void* out = nullptr;
    intptr_t length = 0;
    if (!Dart_IsError(
            Dart_TypedDataAcquireData(data, &type, &out, &length)) &&
        out != nullptr) {
      std::memcpy(out, bytes.data(),
                  std::min<size_t>(bytes.size(), static_cast<size_t>(length)));
      Dart_TypedDataReleaseData(data);
    }
  }
  std::fprintf(stderr,
               "[skyline-runtime] SceneToImage: %dx%d format=%d bytes=%zu\n",
               width, height, format, bytes.size());
  auto callback = reinterpret_cast<NodeSnapshotCallback>(callback_ptr);
  if (callback != nullptr) {
    callback(g_pending_node_snapshot_request.load(), data);
  }
}
#endif  // SKYLINE_EMBEDDED

// Called by the Dart route while a back gesture is intercepted.
SKYLINE_EXPORT extern "C" void OnNavigateBackIntercept(intptr_t /*runtime*/,
                                                       int32_t page_id) {
  appservice::TriggerNavigateBackIntercept(page_id);
}

// Dart's IntersectionObserver layer reports threshold crossings here; route
// them to the JS observer registered for the YGNode address.
SKYLINE_EXPORT extern "C" void YogaTriggerIntersectionObservers(
    intptr_t node_address, int32_t unique_id, float b_top, float b_right,
    float b_bottom, float b_left, float i_top, float i_right, float i_bottom,
    float i_left, float root_top, float root_right, float root_bottom,
    float root_left, float ratio, uint8_t is_intersecting) {
  const auto found = appservice::g_node_address_ids.find(node_address);
  const uint32_t node_id =
      found == appservice::g_node_address_ids.end() ? 0 : found->second;
  appservice::TriggerIntersectionForNode(
      node_id, unique_id, b_top, b_right, b_bottom, b_left, i_top, i_right,
      i_bottom, i_left, root_top, root_right, root_bottom, root_left, ratio,
      is_intersecting != 0);
}

SKYLINE_EXPORT extern "C" void YogaTriggerIntersection(
    intptr_t node_address, float b_top, float b_right, float b_bottom,
    float b_left, float i_top, float i_right, float i_bottom, float i_left,
    float root_top, float root_right, float root_bottom, float root_left,
    float ratio, uint8_t is_intersecting) {
  const auto found = appservice::g_node_address_ids.find(node_address);
  const uint32_t node_id =
      found == appservice::g_node_address_ids.end() ? 0 : found->second;
  appservice::TriggerIntersectionForNode(
      node_id, -1, b_top, b_right, b_bottom, b_left, i_top, i_right, i_bottom,
      i_left, root_top, root_right, root_bottom, root_left, ratio,
      is_intersecting != 0);
}

// Route callbacks driven from the appservice worklet runtime
// (`workletUIModule.navigatorPop` / `startUserGesture` / `stopUserGesture` /
// `updateAnimationController`).  They arrive on the addon's JS thread, so the
// Dart FFI callbacks must be invoked on the UI task runner.
SKYLINE_EXPORT extern "C" void SkylineNavigatorPop(int32_t page_id) {
  PostToUiIfReady([page_id]() {
    if (appservice::g_page == nullptr || appservice::g_page->holder == nullptr) {
      return;
    }
    auto* callback = reinterpret_cast<NavigatorPopFn>(FindCallback(
        appservice::g_page->holder->ffi_table, kNavigatorPopHash));
    if (callback != nullptr) {
      std::fprintf(stderr, "[skyline-runtime] NavigatorPop page=%d\n", page_id);
      callback(page_id);
    }
  });
}

SKYLINE_EXPORT extern "C" void SkylineStartUserGesture() {
  PostToUiIfReady([]() {
    if (appservice::g_page == nullptr || appservice::g_page->holder == nullptr) {
      return;
    }
    auto* callback = reinterpret_cast<UserGestureCallbackFn>(FindCallback(
        appservice::g_page->holder->ffi_table, kStartUserGestureHash));
    if (callback != nullptr) callback();
  });
}

SKYLINE_EXPORT extern "C" void SkylineStopUserGesture() {
  PostToUiIfReady([]() {
    if (appservice::g_page == nullptr || appservice::g_page->holder == nullptr) {
      return;
    }
    auto* callback = reinterpret_cast<UserGestureCallbackFn>(FindCallback(
        appservice::g_page->holder->ffi_table, kStopUserGestureHash));
    if (callback != nullptr) callback();
  });
}

SKYLINE_EXPORT extern "C" void SkylineUpdateAnimationController(int32_t page_id,
                                                                double value) {
  PostToUiIfReady([page_id, value]() {
    if (appservice::g_page == nullptr || appservice::g_page->holder == nullptr) {
      return;
    }
    auto* callback = reinterpret_cast<UpdateAnimationControllerFn>(FindCallback(
        appservice::g_page->holder->ffi_table, kUpdateAnimationControllerHash));
    if (callback != nullptr) {
      callback(page_id, value);
    }
  });
}

SKYLINE_EXPORT extern "C" void SkylineSwiperTo(uint32_t node_id, uint32_t page,
                                               uint8_t animated) {
  PostToUiIfReady([node_id, page, animated]() {
    appservice::AnimateSwiperTo(node_id, page, animated != 0);
  });
}

SKYLINE_EXPORT extern "C" void SkylinePickerColumnSetCurrent(
    uint32_t node_id, uint32_t current) {
  PostToUiIfReady(
      [node_id, current]() { appservice::SetPickerColumnCurrent(node_id, current); });
}

SKYLINE_EXPORT extern "C" void SkylineSetInputFocus(uint32_t node_id,
                                                    uint8_t focus) {
  PostToUiIfReady(
      [node_id, focus]() { appservice::SetInputFocus(node_id, focus != 0); });
}

SKYLINE_EXPORT extern "C" void SkylineScrollNodeTo(uint32_t node_id,
                                                   double top) {
  fml::RefPtr<fml::TaskRunner> runner;
  {
    std::lock_guard<std::mutex> lock(g_live_ops_mutex);
    if (!g_live_ready) return;
    runner = g_live_ui_runner;
  }
  if (!runner) return;
  runner->PostTask([node_id, top]() {
    DemoPageApiScope scope;
    appservice::ScrollNodeTo(node_id, top);
  });
}

SKYLINE_EXPORT extern "C" void SkylineApplyLiveOps(const char* json,
                                                   double /*window_id*/) {
  std::string payload(json != nullptr ? json : "");
  if (payload.empty()) return;
  fml::RefPtr<fml::TaskRunner> runner;
  {
    std::lock_guard<std::mutex> lock(g_live_ops_mutex);
    if (!g_live_ready) {
      g_pending_live_ops = std::move(payload);
      return;
    }
    runner = g_live_ui_runner;
  }
  if (!runner) return;
  // Called from the Electron renderer thread: hop onto the engine UI thread
  // before touching the fml message loop or the Dart isolate.
  runner->PostTask([payload = std::move(payload)]() mutable {
    DemoPageApiScope scope;
    ScheduleLiveOps(std::move(payload), 0);
  });
}

SKYLINE_EXPORT extern "C" void DispatchSkylineKeyEvent(int32_t kind,
                                                       uint32_t logical,
                                                       const char* text) {
  std::fprintf(stderr,
               "[skyline-runtime] DispatchSkylineKeyEvent: kind=%d logical=%u "
               "text=%s\n",
               static_cast<int>(kind), logical, text != nullptr ? text : "");
  if (appservice::g_page == nullptr || appservice::g_page->holder == nullptr) {
    return;
  }
  const std::string value = text != nullptr ? text : "";
  if (Dart_CurrentIsolate() != nullptr) {
    // Called with the isolate already entered (e.g. from a Dart callback).
    DemoPageApiScope scope;
    appservice::HandleKeyInput(appservice::g_page->holder, kind, logical,
                               value);
    return;
  }
  RuntimeHolder* holder = appservice::g_page->holder;
  auto runner = UiTaskRunner();
  if (!runner) runner = appservice::g_ui_task_runner;
  if (!runner) return;
  // The headless embedder runs the UI tasks on the same thread that drives the
  // engine, so run inline instead of deadlocking behind the host's wait loop.
  fml::TaskRunner::RunNowOrPostTask(
      runner, [holder, kind, logical, value]() {
        DemoPageApiScope scope;
        appservice::HandleKeyInput(holder, kind, logical, value);
      });
}


void CreateAppServiceOpsPage(RuntimeHolder* holder, const char* descriptor) {
  appservice::ApplyOpsPage(holder, descriptor);
}


void ScheduleCreateDemoPage(RuntimeHolder* holder) {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
  auto task_runner = fml::MessageLoop::GetCurrent().GetTaskRunner();
#pragma GCC diagnostic pop
  if (!task_runner) {
    std::fprintf(stderr, "[skyline-runtime] demo page: no UI task runner\n");
    return;
  }
  task_runner->PostTask([holder]() { CreateDemoPage(holder); });
}
