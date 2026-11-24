// Scroll/swiper/picker/refresher/intersection/selection callbacks.

#include "internal/skyline_runtime_module.hh"
#include "internal/skyline_runtime_gesture.hh"

namespace appservice {


// -- widget materialization --------------------------------------------------

// Yoga metrics are written before widget construction, so allocate every
// node's 9-float buffer up front and register the stable data pointer.
// Native scroll callbacks.  The static page does not need content-size
// estimation or scroll notifications yet, but `newScrollView` calls
// `asFunction()` on every pointer unconditionally, so they must be valid.
const char kScrollVerticalTag[] = "skyline-scroll-vertical";
const char kScrollHorizontalTag[] = "skyline-scroll-horizontal";
const char kSwiperHorizontalTag[] = "skyline-swiper-horizontal";
const char kSwiperVerticalTag[] = "skyline-swiper-vertical";

// GestureType.kVerticalDrag == 6, kHorizontalDrag == 7 in package:skyline.
// The handler id is the node id, so `ShouldGestureResponse(handler_id)` can
// look the node up and the JS gesture-handler proxy (host node viewTag) can
// find its `shouldResponseOnMove` worklet.
NativeGestureRecognizerNative MakeScrollGestureRecognizer(uint32_t handler_id,
                                                          bool horizontal) {
  return NativeGestureRecognizerNative{
      handler_id, horizontal ? 7u : 6u,
      horizontal ? kScrollHorizontalTag : kScrollVerticalTag, nullptr, 0u};
}

NativeGestureRecognizerNative MakeSwiperGestureRecognizer(uint32_t handler_id,
                                                          bool vertical) {
  return NativeGestureRecognizerNative{
      handler_id, vertical ? 6u : 7u,
      vertical ? kSwiperVerticalTag : kSwiperHorizontalTag, nullptr, 0u};
}

extern "C" double ScrollNoopContentSize(intptr_t, int32_t, int32_t, double,
                                       double) {
  return std::nan("");
}

// Shared scroll-report path.  Updates the persisted `scrollTop` so a following
// rebuild (the address-book highlight calls setData on every scroll) keeps the
// list at the user's position, then forwards the event to the addon when one
// is installed.
void DispatchScrollEvent(intptr_t ptr, int32_t phase, double top, double left,
                         double width, double height, double dx, double dy,
                         uint8_t drag, double velocity) {
  if (std::getenv("SKYLINE_APP_SERVICE_DEBUG") != nullptr) {
    if (phase == kScrollPhaseScroll) {
      std::fprintf(stderr,
                   "[skyline-runtime] scroll: top=%g left=%g delta=(%g,%g) "
                   "drag=%u velocity=%g\n",
                   top, left, dx, dy, static_cast<unsigned>(drag), velocity);
    } else {
      std::fprintf(stderr,
                   "[skyline-runtime] scroll start/end: top=%g left=%g "
                   "size=%gx%g drag=%u\n",
                   top, left, width, height, static_cast<unsigned>(drag));
    }
  }
  uint32_t node_id = 0;
  const auto found = g_scroll_view_ids.find(ptr);
  if (found != g_scroll_view_ids.end()) node_id = found->second;
  if (node_id != 0 && g_page != nullptr) {
    AppNode* node = FindNode(node_id);
    if (node != nullptr && !node->released) {
      char buffer[64];
      std::snprintf(buffer, sizeof(buffer), "%g", top);
      node->attrs["scrollTop"] = buffer;
      std::snprintf(buffer, sizeof(buffer), "%g", left);
      node->attrs["scrollLeft"] = buffer;
      // Content extents: the edge check in ShouldGestureResponse compares
      // scrollTop + viewport against these (they are the ScrollEvent's
      // scrollWidth/scrollHeight arguments, not the viewport size).
      std::snprintf(buffer, sizeof(buffer), "%g", width);
      node->attrs["scrollWidth"] = buffer;
      std::snprintf(buffer, sizeof(buffer), "%g", height);
      node->attrs["scrollHeight"] = buffer;
      // Published rects are viewport coordinates; republish so hit testing and
      // selector queries follow the scroll (grid cards must stay tappable).
      PublishNodeRects();
    }
  }
  SkylineScrollEventSink sink = g_scroll_event_sink.load();
  if (sink != nullptr) {
    sink(node_id, phase, top, left, width, height, dx, dy, drag, velocity);
  }
}

extern "C" void ScrollStartEvent(intptr_t ptr, double top, double left,
                                 double width, double height, uint8_t drag) {
  DispatchScrollEvent(ptr, kScrollPhaseStart, top, left, width, height, 0.0,
                      0.0, drag, 0.0);
}
extern "C" void ScrollEndEvent(intptr_t ptr, double top, double left,
                               double width, double height, uint8_t drag) {
  DispatchScrollEvent(ptr, kScrollPhaseEnd, top, left, width, height, 0.0, 0.0,
                      drag, 0.0);
}
extern "C" void ScrollEvent(intptr_t ptr, double top, double left, double width,
                            double height, double dx, double dy, uint8_t drag,
                            double velocity) {
  DispatchScrollEvent(ptr, kScrollPhaseScroll, top, left, width, height, dx, dy,
                      drag, velocity);
}
extern "C" void ScrollNoopPtr(intptr_t) {}

// Register the native callbacks through the `ScrollViewUpdateOnScroll*` FFI
// entry points.  NewScrollView already accepts the same pointers, but the
// appservice pipeline updates scroll callbacks after creation; wiring both
// paths keeps the contracts in sync and matches the official renderer.
void InstallScrollCallbacks(AppNode* node) {
  if (node == nullptr || node->scroll_view_p == nullptr || g_page == nullptr ||
      g_page->holder == nullptr) {
    return;
  }
  g_scroll_view_ids[node->address] = node->id;
  FfiTable* table = g_page->holder->ffi_table;
  Dart_Handle handle = Dart_HandleFromPersistent(node->scroll_view_p);
  bool start = false;
  bool scroll = false;
  bool end = false;
  if (void* callback =
          FindCallback(table, kScrollViewUpdateOnScrollStartHash)) {
    reinterpret_cast<ScrollViewUpdateOnScrollStartFn>(callback)(
        handle, reinterpret_cast<void*>(&ScrollStartEvent));
    start = true;
  }
  if (void* callback = FindCallback(table, kScrollViewUpdateOnScrollHash)) {
    reinterpret_cast<ScrollViewUpdateOnScrollFn>(callback)(
        handle, reinterpret_cast<void*>(&ScrollEvent));
    scroll = true;
  }
  if (void* callback =
          FindCallback(table, kScrollViewUpdateOnScrollEndHash)) {
    reinterpret_cast<ScrollViewUpdateOnScrollEndFn>(callback)(
        handle, reinterpret_cast<void*>(&ScrollEndEvent));
    end = true;
  }
  if (std::getenv("SKYLINE_APP_SERVICE_DEBUG") != nullptr) {
    std::fprintf(stderr,
                 "[skyline-runtime] scroll callbacks installed #%u "
                 "(start=%d scroll=%d end=%d)\n",
                 node->id, start ? 1 : 0, scroll ? 1 : 0, end ? 1 : 0);
  }
}

// -- swiper / picker-view / input native widget channel ----------------------

void EmitWidgetEvent(uint32_t node_id, int32_t kind, double value,
                     double extra, uint8_t flag) {
  SkylineWidgetEventSink sink = g_widget_event_sink.load();
  if (sink != nullptr) sink(node_id, kind, value, extra, flag);
}

uint32_t WidgetNodeId(const std::unordered_map<intptr_t, uint32_t>& map,
                      intptr_t ptr) {
  const auto found = map.find(ptr);
  return found == map.end() ? 0 : found->second;
}

extern "C" void SwiperOnChange(intptr_t ptr, uint32_t page, uint8_t reason) {
  const uint32_t id = WidgetNodeId(g_swiper_ids, ptr);
  if (std::getenv("SKYLINE_APP_SERVICE_DEBUG") != nullptr) {
    std::fprintf(stderr,
                 "[skyline-runtime] swiper change: id=%u page=%u reason=%u\n",
                 id, page, static_cast<unsigned>(reason));
  }
  if (id != 0) {
    AppNode* node = FindNode(id);
    if (node != nullptr && !node->released) {
      char buffer[32];
      std::snprintf(buffer, sizeof(buffer), "%u", page);
      node->attrs["current"] = buffer;
    }
  }
  EmitWidgetEvent(id, kWidgetSwiperChange, static_cast<double>(page),
                  static_cast<double>(reason), 0);
}

extern "C" void SwiperOnScroll(intptr_t ptr, double translate_pixels,
                               uint8_t state, uint32_t current) {
  // 0/1 = drag start/update, 2 = settle.  Rebuilds are held until settle so an
  // image response cannot replace the Swiper while the user is swiping it.
  SetSwiperGestureActive(state != 2);
  const uint32_t id = WidgetNodeId(g_swiper_ids, ptr);
  if (std::getenv("SKYLINE_APP_SERVICE_DEBUG") != nullptr) {
    std::fprintf(stderr,
                 "[skyline-runtime] swiper scroll: id=%u offset=%g state=%u "
                 "current=%u\n",
                 id, translate_pixels, static_cast<unsigned>(state), current);
  }
  EmitWidgetEvent(id, kWidgetSwiperScroll, translate_pixels,
                  static_cast<double>(state),
                  static_cast<uint8_t>(current > 255 ? 255 : current));
}

extern "C" void PickerColumnOnChange(intptr_t ptr, int32_t index,
                                     int32_t reason) {
  const uint32_t id = WidgetNodeId(g_picker_column_ids, ptr);
  if (std::getenv("SKYLINE_APP_SERVICE_DEBUG") != nullptr) {
    std::fprintf(stderr,
                 "[skyline-runtime] picker change: id=%u index=%d reason=%d\n",
                 id, index, reason);
  }
  if (id != 0) {
    AppNode* node = FindNode(id);
    if (node != nullptr && !node->released) {
      char buffer[32];
      std::snprintf(buffer, sizeof(buffer), "%d", index);
      node->attrs["value"] = buffer;
    }
  }
  EmitWidgetEvent(id, kWidgetPickerChange, static_cast<double>(index),
                  static_cast<double>(reason), 0);
}

extern "C" void PickerColumnOnStart(intptr_t ptr) {
  const uint32_t id = WidgetNodeId(g_picker_column_ids, ptr);
  if (std::getenv("SKYLINE_APP_SERVICE_DEBUG") != nullptr) {
    std::fprintf(stderr, "[skyline-runtime] picker start: id=%u\n", id);
  }
  EmitWidgetEvent(id, kWidgetPickerStart, 0.0, 0.0, 0);
}

extern "C" void PickerColumnOnEnd(intptr_t ptr) {
  const uint32_t id = WidgetNodeId(g_picker_column_ids, ptr);
  if (std::getenv("SKYLINE_APP_SERVICE_DEBUG") != nullptr) {
    std::fprintf(stderr, "[skyline-runtime] picker end: id=%u\n", id);
  }
  EmitWidgetEvent(id, kWidgetPickerEnd, 0.0, 0.0, 0);
}

void InstallSwiperCallbacks(AppNode* node) {
  if (node == nullptr || node->swiper_p == nullptr || g_page == nullptr ||
      g_page->holder == nullptr) {
    return;
  }
  g_swiper_ids[node->address] = node->id;
  FfiTable* table = g_page->holder->ffi_table;
  Dart_Handle handle = Dart_HandleFromPersistent(node->swiper_p);
  bool change = false;
  bool scroll = false;
  if (void* callback = FindCallback(table, kSwiperUpdateOnChangeHash)) {
    reinterpret_cast<SwiperUpdateOnChangeFn>(callback)(
        handle, reinterpret_cast<void*>(&SwiperOnChange));
    change = true;
  }
  if (void* callback = FindCallback(table, kSwiperUpdateOnScrollHash)) {
    reinterpret_cast<SwiperUpdateOnScrollFn>(callback)(
        handle, reinterpret_cast<void*>(&SwiperOnScroll));
    scroll = true;
  }
  if (std::getenv("SKYLINE_APP_SERVICE_DEBUG") != nullptr) {
    std::fprintf(stderr,
                 "[skyline-runtime] swiper callbacks installed #%u "
                 "(onChange=%d onScroll=%d)\n",
                 node->id, change ? 1 : 0, scroll ? 1 : 0);
  }
}

void InstallPickerColumnCallbacks(AppNode* node) {
  if (node == nullptr || node->picker_column_p == nullptr ||
      g_page == nullptr || g_page->holder == nullptr) {
    return;
  }
  g_picker_column_ids[node->address] = node->id;
  FfiTable* table = g_page->holder->ffi_table;
  Dart_Handle handle = Dart_HandleFromPersistent(node->picker_column_p);
  bool change = false;
  bool start = false;
  bool end = false;
  if (void* callback =
          FindCallback(table, kPickerViewColumnUpdateOnChangeHash)) {
    reinterpret_cast<PickerViewColumnUpdateOnChangeFn>(callback)(
        handle, reinterpret_cast<void*>(&PickerColumnOnChange));
    change = true;
  }
  if (void* callback =
          FindCallback(table, kPickerViewColumnUpdateOnStartHash)) {
    reinterpret_cast<PickerViewColumnUpdateOnStartFn>(callback)(
        handle, reinterpret_cast<void*>(&PickerColumnOnStart));
    start = true;
  }
  if (void* callback = FindCallback(table, kPickerViewColumnUpdateOnEndHash)) {
    reinterpret_cast<PickerViewColumnUpdateOnEndFn>(callback)(
        handle, reinterpret_cast<void*>(&PickerColumnOnEnd));
    end = true;
  }
  if (std::getenv("SKYLINE_APP_SERVICE_DEBUG") != nullptr) {
    std::fprintf(stderr,
                 "[skyline-runtime] picker callbacks installed #%u "
                 "(onChange=%d onStart=%d onEnd=%d)\n",
                 node->id, change ? 1 : 0, start ? 1 : 0, end ? 1 : 0);
  }
}

// UI-thread programmatic updates.  These run on the engine UI task runner
// (exported wrappers below hop there from the addon thread).
double AttrNumber(const AppNode* node, const char* key, double fallback);

void AnimateSwiperTo(uint32_t node_id, uint32_t page, bool animated) {
  if (g_page == nullptr || g_page->holder == nullptr) return;
  AppNode* node = FindNode(node_id);
  if (node == nullptr || node->swiper_p == nullptr) return;
  // Flutter asserts `duration > Duration.zero` inside DrivenScrollActivity;
  // `<swiper duration="0">` must therefore be a jump, not an animation.
  const bool use_animation =
      animated && AttrNumber(node, "duration", 500.0) > 0.0;
  void* callback = FindCallback(
      g_page->holder->ffi_table,
      use_animation ? kSwiperAnimateToPageHash : kSwiperJumpToPageHash);
  if (callback == nullptr) return;
  Dart_Handle handle = Dart_HandleFromPersistent(node->swiper_p);
  if (use_animation) {
    reinterpret_cast<SwiperAnimateToPageFn>(callback)(handle, page);
  } else {
    reinterpret_cast<SwiperJumpToPageFn>(callback)(handle, page);
  }
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "%u", page);
  node->attrs["current"] = buffer;
  if (std::getenv("SKYLINE_APP_SERVICE_DEBUG") != nullptr) {
    std::fprintf(stderr,
                 "[skyline-runtime] swiperTo #%u page=%u animated=%d\n", node_id,
                 page, use_animation ? 1 : 0);
  }
}

void SetPickerColumnCurrent(uint32_t node_id, uint32_t current) {
  if (g_page == nullptr || g_page->holder == nullptr) return;
  AppNode* node = FindNode(node_id);
  if (node == nullptr || node->picker_column_p == nullptr) return;
  void* callback = FindCallback(g_page->holder->ffi_table,
                                kPickerViewColumnUpdateCurrentHash);
  if (callback == nullptr) return;
  reinterpret_cast<PickerViewColumnUpdateCurrentFn>(callback)(
      Dart_HandleFromPersistent(node->picker_column_p), current);
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "%u", current);
  node->attrs["value"] = buffer;
  if (std::getenv("SKYLINE_APP_SERVICE_DEBUG") != nullptr) {
    std::fprintf(stderr, "[skyline-runtime] pickerTo #%u current=%u\n",
                 node_id, current);
  }
}

void SetInputFocus(uint32_t node_id, bool focus) {
  if (g_page == nullptr || g_page->holder == nullptr) return;
  AppNode* node = FindNode(node_id);
  if (node == nullptr) return;
  if (focus) {
    g_page->focused_input_id = node_id;
  } else if (g_page->focused_input_id == node_id) {
    g_page->focused_input_id = 0;
  }
  if (node->input_widget_p == nullptr) {
    if (std::getenv("SKYLINE_APP_SERVICE_DEBUG") != nullptr) {
      std::fprintf(stderr,
                   "[skyline-runtime] input %s #%u: no input widget\n",
                   focus ? "focus" : "blur", node_id);
    }
    return;
  }
  void* callback = FindCallback(
      g_page->holder->ffi_table,
      focus ? kInputFocusManuallyHash : kInputBlurManuallyHash);
  if (callback == nullptr) return;
  Dart_Handle handle = Dart_HandleFromPersistent(node->input_widget_p);
  if (focus) {
    reinterpret_cast<InputFocusManuallyFn>(callback)(handle);
  } else {
    reinterpret_cast<InputBlurManuallyFn>(callback)(handle);
  }
  std::fprintf(stderr, "[skyline-runtime] Input%sManually: id=%u\n",
               focus ? "Focus" : "Blur", node_id);
  EmitWidgetEvent(node_id, focus ? kWidgetInputFocus : kWidgetInputBlur, 0.0,
                  0.0, 0);
}

// -- refresher callbacks -----------------------------------------------------

extern "C" void RefresherPulling(intptr_t ptr, double dy) {
  const uint32_t id = WidgetNodeId(g_scroll_view_ids, ptr);
  if (std::getenv("SKYLINE_APP_SERVICE_DEBUG") != nullptr) {
    std::fprintf(stderr, "[skyline-runtime] refresher pulling: id=%u dy=%g\n",
                 id, dy);
  }
  EmitWidgetEvent(id, kWidgetRefresherPulling, dy, 0.0, 0);
}
extern "C" void RefresherWillRefresh(intptr_t ptr, double dy) {
  const uint32_t id = WidgetNodeId(g_scroll_view_ids, ptr);
  if (std::getenv("SKYLINE_APP_SERVICE_DEBUG") != nullptr) {
    std::fprintf(stderr, "[skyline-runtime] refresher willRefresh: id=%u dy=%g\n",
                 id, dy);
  }
  EmitWidgetEvent(id, kWidgetRefresherWillRefresh, dy, 0.0, 0);
}
extern "C" void RefresherRefresh(intptr_t ptr, double dy) {
  const uint32_t id = WidgetNodeId(g_scroll_view_ids, ptr);
  std::fprintf(stderr, "[skyline-runtime] refresher refresh: id=%u dy=%g\n", id,
               dy);
  EmitWidgetEvent(id, kWidgetRefresherRefresh, dy, 0.0, 0);
}
extern "C" void RefresherRestore(intptr_t ptr, double dy) {
  const uint32_t id = WidgetNodeId(g_scroll_view_ids, ptr);
  if (std::getenv("SKYLINE_APP_SERVICE_DEBUG") != nullptr) {
    std::fprintf(stderr, "[skyline-runtime] refresher restore: id=%u dy=%g\n",
                 id, dy);
  }
  EmitWidgetEvent(id, kWidgetRefresherRestore, dy, 0.0, 0);
}
extern "C" void RefresherAbort(intptr_t ptr, double dy) {
  const uint32_t id = WidgetNodeId(g_scroll_view_ids, ptr);
  if (std::getenv("SKYLINE_APP_SERVICE_DEBUG") != nullptr) {
    std::fprintf(stderr, "[skyline-runtime] refresher abort: id=%u dy=%g\n", id,
                 dy);
  }
  EmitWidgetEvent(id, kWidgetRefresherAbort, dy, 0.0, 0);
}
extern "C" void RefresherStatusChange(intptr_t ptr, int32_t mode, double yy) {
  const uint32_t id = WidgetNodeId(g_scroll_view_ids, ptr);
  if (std::getenv("SKYLINE_APP_SERVICE_DEBUG") != nullptr) {
    std::fprintf(stderr,
                 "[skyline-runtime] refresher status: id=%u mode=%d dy=%g\n",
                 id, mode, yy);
  }
  EmitWidgetEvent(id, kWidgetRefresherStatus, static_cast<double>(mode), yy, 0);
}

void SetRefresherTriggered(uint32_t node_id, bool open, bool two_level) {
  if (g_page == nullptr || g_page->holder == nullptr) return;
  AppNode* node = FindNode(node_id);
  if (node == nullptr || node->scroll_view_p == nullptr) return;
  void* callback = FindCallback(
      g_page->holder->ffi_table,
      two_level ? kScrollViewRequestRefresherTwoLevelHash
                : kScrollViewRequestRefresherRefreshHash);
  if (callback == nullptr) return;
  Dart_Handle easing = Dart_NewStringFromCString("default");
  reinterpret_cast<ScrollViewRequestRefresherFn>(callback)(
      Dart_HandleFromPersistent(node->scroll_view_p), open ? 1 : 0, 300, easing);
  char buffer[16];
  std::snprintf(buffer, sizeof(buffer), "%d", open ? 1 : 0);
  node->attrs["refresherTriggered"] = buffer;
  std::fprintf(stderr,
               "[skyline-runtime] refresher trigger: id=%u open=%d twoLevel=%d\n",
               node_id, open ? 1 : 0, two_level ? 1 : 0);
}

// -- IntersectionObserver ----------------------------------------------------

void RegisterIntersectionObserver(uint32_t node_id, uint32_t unique_id,
                                  uint32_t root_node_id, double top,
                                  double right, double bottom, double left,
                                  std::vector<double> thresholds) {
  if (g_page == nullptr || g_page->holder == nullptr) return;
  AppNode* node = FindNode(node_id);
  if (node == nullptr || node->yoga_node_p == nullptr) {
    std::fprintf(stderr,
                 "[skyline-runtime] intersection add: no yoga node #%u\n",
                 node_id);
    return;
  }
  void* callback = FindCallback(g_page->holder->ffi_table,
                                kYGNodeAddIntersectionObserverHash);
  if (callback == nullptr) return;
  Dart_Handle yoga = Dart_HandleFromPersistent(node->yoga_node_p);
  Dart_Handle root = Dart_Null();
  AppNode* root_node = root_node_id != 0 ? FindNode(root_node_id) : nullptr;
  if (root_node != nullptr && root_node->yoga_node_p != nullptr) {
    root = Dart_HandleFromPersistent(root_node->yoga_node_p);
  }
  Dart_Handle list = Dart_NewList(static_cast<intptr_t>(thresholds.size()));
  for (size_t i = 0; i < thresholds.size(); i++) {
    Dart_ListSetAt(list, static_cast<intptr_t>(i), Dart_NewDouble(thresholds[i]));
  }
  reinterpret_cast<YGNodeAddIntersectionObserverFn>(callback)(
      yoga, unique_id, root, top, right, bottom, left, list);
  g_intersection_unique_nodes[unique_id] = node_id;
  g_intersection_node_observers[node_id].insert(unique_id);
  std::fprintf(stderr,
               "[skyline-runtime] intersection add: node=#%u id=%u root=#%u "
               "thresholds=%zu\n",
               node_id, unique_id, root_node_id, thresholds.size());
}

void UnregisterIntersectionObserver(uint32_t node_id, uint32_t unique_id) {
  if (g_page == nullptr || g_page->holder == nullptr) return;
  AppNode* node = FindNode(node_id);
  void* callback = FindCallback(g_page->holder->ffi_table,
                                kYGNodeRemoveIntersectionObserverHash);
  if (node != nullptr && node->yoga_node_p != nullptr && callback != nullptr) {
    reinterpret_cast<YGNodeRemoveIntersectionObserverFn>(callback)(
        Dart_HandleFromPersistent(node->yoga_node_p), unique_id);
  }
  g_intersection_unique_nodes.erase(unique_id);
  auto found = g_intersection_node_observers.find(node_id);
  if (found != g_intersection_node_observers.end()) {
    found->second.erase(unique_id);
    if (found->second.empty()) g_intersection_node_observers.erase(found);
  }
  std::fprintf(stderr,
               "[skyline-runtime] intersection remove: node=#%u id=%u\n",
               node_id, unique_id);
}

void EmitIntersection(uint32_t node_id, uint32_t unique_id, double b_top,
                      double b_right, double b_bottom, double b_left,
                      double i_top, double i_right, double i_bottom,
                      double i_left, double root_top, double root_right,
                      double root_bottom, double root_left, double ratio,
                      bool intersecting) {
  char buffer[1024];
  std::snprintf(
      buffer, sizeof(buffer),
      "{\"type\":\"intersection\",\"nodeId\":%u,\"uniqueId\":%u,"
      "\"boundingClientRect\":{\"top\":%.6g,\"right\":%.6g,\"bottom\":%.6g,"
      "\"left\":%.6g},\"intersectionRect\":{\"top\":%.6g,\"right\":%.6g,"
      "\"bottom\":%.6g,\"left\":%.6g},\"rootBounds\":{\"top\":%.6g,"
      "\"right\":%.6g,\"bottom\":%.6g,\"left\":%.6g},"
      "\"intersectionRatio\":%.6g,\"isIntersecting\":%s}",
      node_id, unique_id, b_top, b_right, b_bottom, b_left, i_top, i_right,
      i_bottom, i_left, root_top, root_right, root_bottom, root_left, ratio,
      intersecting ? "true" : "false");
  if (std::getenv("SKYLINE_APP_SERVICE_DEBUG") != nullptr) {
    std::fprintf(stderr,
                 "[skyline-runtime] intersection trigger: node=#%u id=%u "
                 "ratio=%g intersecting=%d\n",
                 node_id, unique_id, ratio, intersecting ? 1 : 0);
  }
  EmitJsonEvent(buffer);
}

void TriggerIntersectionForNode(uint32_t node_id, int32_t unique_id,
                                double b_top, double b_right, double b_bottom,
                                double b_left, double i_top, double i_right,
                                double i_bottom, double i_left, double root_top,
                                double root_right, double root_bottom,
                                double root_left, double ratio,
                                bool intersecting) {
  auto found = g_intersection_node_observers.find(node_id);
  if (found == g_intersection_node_observers.end()) return;
  if (unique_id >= 0) {
    const uint32_t uid = static_cast<uint32_t>(unique_id);
    if (found->second.count(uid) != 0) {
      EmitIntersection(node_id, uid, b_top, b_right, b_bottom, b_left, i_top,
                       i_right, i_bottom, i_left, root_top, root_right,
                       root_bottom, root_left, ratio, intersecting);
    }
    return;
  }
  for (uint32_t uid : found->second) {
    EmitIntersection(node_id, uid, b_top, b_right, b_bottom, b_left, i_top,
                     i_right, i_bottom, i_left, root_top, root_right,
                     root_bottom, root_left, ratio, intersecting);
  }
}

// -- Selection ---------------------------------------------------------------

extern "C" void SelectionOnChangeCallback(intptr_t ptr, double left, double top,
                                          double right, double bottom,
                                          Dart_Handle selected) {
  const uint32_t id = WidgetNodeId(g_selection_ids, ptr);
  std::string text;
#ifdef SKYLINE_EMBEDDED
  if (selected != nullptr && Dart_IsString(selected)) {
    const char* utf8 = nullptr;
    if (!Dart_IsError(Dart_StringToCString(selected, &utf8)) &&
        utf8 != nullptr) {
      text = utf8;
    }
  }
#else
  (void)selected;
#endif
  std::fprintf(stderr,
               "[skyline-runtime] selection change: id=%u rect=(%g,%g,%g,%g) "
               "text=\"%s\"\n",
               id, left, top, right, bottom, text.c_str());
  std::string json =
      "{\"type\":\"selection\",\"nodeId\":" + std::to_string(id) +
      ",\"left\":" + std::to_string(left) + ",\"top\":" + std::to_string(top) +
      ",\"right\":" + std::to_string(right) +
      ",\"bottom\":" + std::to_string(bottom) +
      ",\"selectedString\":\"" + JsonEscapeString(text) + "\"}";
  EmitJsonEvent(json);
}

void InstallSelectionCallbacks(AppNode* node) {
  if (node == nullptr || node->selection_p == nullptr || g_page == nullptr ||
      g_page->holder == nullptr) {
    return;
  }
  g_selection_ids[node->address] = node->id;
  void* callback = FindCallback(g_page->holder->ffi_table,
                                kSelectionUpdateOnSelectionChangeHash);
  if (callback != nullptr) {
    reinterpret_cast<SelectionUpdateOnSelectionChangeFn>(callback)(
        Dart_HandleFromPersistent(node->selection_p), node->address,
        reinterpret_cast<void*>(&SelectionOnChangeCallback));
  }
  std::fprintf(stderr, "[skyline-runtime] selection callbacks installed #%u\n",
               node->id);
}

void SetSelectionRange(uint32_t selection_id, uint32_t start_id,
                       int32_t start_offset, uint32_t end_id,
                       int32_t end_offset) {
  if (g_page == nullptr || g_page->holder == nullptr) return;
  AppNode* selection = FindNode(selection_id);
  AppNode* start = FindNode(start_id);
  AppNode* end = FindNode(end_id != 0 ? end_id : start_id);
  if (selection == nullptr || selection->selection_p == nullptr ||
      start == nullptr || end == nullptr) {
    std::fprintf(stderr,
                 "[skyline-runtime] selection setRange: missing node "
                 "(selection=#%u start=#%u end=#%u)\n",
                 selection_id, start_id, end_id);
    return;
  }
  void* callback =
      FindCallback(g_page->holder->ffi_table, kSelectionSetRangeHash);
  if (callback == nullptr) return;
  reinterpret_cast<SelectionSetRangeFn>(callback)(
      Dart_HandleFromPersistent(selection->selection_p), start->address,
      start_offset, end->address, end_offset);
  std::fprintf(stderr,
               "[skyline-runtime] selection setRange: selection=#%u start=#%u:%d "
               "end=#%u:%d\n",
               selection_id, start_id, start_offset, end_id != 0 ? end_id
                                                                 : start_id,
               end_offset);
}

// -- navigate-back interception ---------------------------------------------

void SetPageNavigateBackInterception(int32_t page_id, bool enabled) {
  g_navigate_back_interception[page_id] = enabled;
  if (g_page != nullptr && g_page->holder != nullptr) {
    void* callback = FindCallback(g_page->holder->ffi_table,
                                  kSetNavigateBackInterceptionHash);
    if (callback != nullptr) {
      reinterpret_cast<SetNavigateBackInterceptionFn>(callback)(
          page_id, enabled ? 1 : 0);
    }
  }
  std::fprintf(stderr,
               "[skyline-runtime] navigate back interception: page=%d "
               "enabled=%d\n",
               page_id, enabled ? 1 : 0);
}

void TriggerNavigateBackIntercept(int32_t page_id) {
  bool enabled = false;
  const auto found = g_navigate_back_interception.find(page_id);
  if (found != g_navigate_back_interception.end()) enabled = found->second;
  std::fprintf(stderr,
               "[skyline-runtime] OnNavigateBackIntercept: page=%d enabled=%d\n",
               page_id, enabled ? 1 : 0);
  char buffer[256];
  std::snprintf(buffer, sizeof(buffer),
                "{\"type\":\"navigateBack\",\"pageId\":%d,\"enabled\":%s}",
                page_id, enabled ? "true" : "false");
  EmitJsonEvent(buffer);
}


}  // namespace appservice

