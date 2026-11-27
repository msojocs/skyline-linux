/*
 * C ABI of the Skyline runtime embedded in libflutter_engine.so, as used by
 * skyline.node (native/src) and the hosts (native/host).
 *
 * This is the only declaration of these functions: the runtime
 * (native/runtime) includes it, so a definition that drifts from its
 * declaration fails to compile, and native/engine/patch-skyline-runtime.sh
 * generates the engine export list from the SKYLINE_API lines below.
 *
 * The Dart-facing FFI surface (the symbols package:skyline looks up, see
 * spec/kernel-symbols.json) is not declared here; only Dart calls it.
 *
 * Thread annotations:
 *   [any]     callable from any thread; does not enter the Dart isolate.
 *   [any->ui] callable from any thread; the work is posted to the engine UI
 *             task runner and dropped while the Dart app is not ready, unless
 *             the comment says otherwise.
 * Sinks are invoked on the engine UI thread and must not block; passing NULL
 * uninstalls a sink.
 *
 * Hosts that must also run on an engine built without the runtime define
 * SKYLINE_RUNTIME_API_WEAK before including this header: every function is
 * then a weak reference that is NULL when the engine does not export it.
 */
#ifndef SKYLINE_RUNTIME_API_H_
#define SKYLINE_RUNTIME_API_H_

#include <stdint.h>

#if defined(SKYLINE_RUNTIME_API_WEAK)
#define SKYLINE_API __attribute__((weak))
#elif defined(__GNUC__)
#define SKYLINE_API __attribute__((visibility("default")))
#else
#define SKYLINE_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* -- Events -------------------------------------------------------------- */

/* `phase` of SkylineScrollEventSink. */
enum {
  SKYLINE_SCROLL_PHASE_START = 0,
  SKYLINE_SCROLL_PHASE_SCROLL = 1,
  SKYLINE_SCROLL_PHASE_END = 2,
};

/* `kind` of SkylineWidgetEventSink; the bootstrap matches the numbers. */
enum {
  SKYLINE_WIDGET_SWIPER_CHANGE = 0, /* value = page, extra = reason */
  SKYLINE_WIDGET_SWIPER_SCROLL = 1, /* value = translate px, extra = state,
                                       flag = current page */
  SKYLINE_WIDGET_PICKER_CHANGE = 2, /* value = index, extra = reason */
  SKYLINE_WIDGET_PICKER_START = 3,
  SKYLINE_WIDGET_PICKER_END = 4,
  SKYLINE_WIDGET_INPUT_FOCUS = 5,
  SKYLINE_WIDGET_INPUT_BLUR = 6,
  SKYLINE_WIDGET_REFRESHER_PULLING = 8, /* value = dy */
  SKYLINE_WIDGET_REFRESHER_WILL_REFRESH = 9,
  SKYLINE_WIDGET_REFRESHER_REFRESH = 10,
  SKYLINE_WIDGET_REFRESHER_RESTORE = 11,
  SKYLINE_WIDGET_REFRESHER_ABORT = 12,
  SKYLINE_WIDGET_REFRESHER_STATUS = 13, /* value = mode, extra = dy */
};

/* `kind` of SkylineAccessibilitySink: two separate embedder APIs. */
enum {
  SKYLINE_ACCESSIBILITY_FEATURES = 0, /* FlutterEngineUpdateAccessibilityFeatures */
  SKYLINE_ACCESSIBILITY_SEMANTICS = 1, /* FlutterEngineUpdateSemanticsEnabled */
};

/* Native scroll-view callbacks (bindscroll, worklet scroll handlers). */
typedef void (*SkylineScrollEventSink)(uint32_t node_id, int32_t phase,
                                       double scroll_top, double scroll_left,
                                       double scroll_width,
                                       double scroll_height, double delta_x,
                                       double delta_y, uint8_t is_drag,
                                       double boundary_velocity);
/* Swiper / picker / input / refresher events for the appservice mirror. */
typedef void (*SkylineWidgetEventSink)(uint32_t node_id, int32_t kind,
                                       double value, double extra,
                                       uint8_t flag);
/* Rich events (intersection, selection, navigate-back, ...) as JSON. */
typedef void (*SkylineJsonEventSink)(const char* json);
/* Accessibility state the embedder must apply on its platform thread. */
typedef void (*SkylineAccessibilitySink)(int32_t kind, int32_t value);
/* DevTools Network bridge: fetch `url` and answer with
   SkylineNotifyHttpRequestComplete(request_id, ...). */
typedef void (*SkylineHttpRequestSink)(uint32_t request_id, const char* url);
/* Nonzero when the pageframe registered its fetch callback. */
typedef int32_t (*SkylineHasHttpRequestCallbackFn)(void);

/* -- Build identity --------------------------------------------------------- */

/* [any] "SKYLINE_RUNTIME_BUILD_ID=<sha256 of the runtime sources>", or
   "...=unversioned" outside the engine build (tools/build-status.js). */
SKYLINE_API const char* SkylineRuntimeBuildId(void);

/* -- Page operations and queries ---------------------------------------------- */

/* [any->ui] Applies one batch of appservice page ops (JSON, the recorded
   fixture schema).  Batches that arrive before the Dart app is ready are
   kept and applied once it is. */
SKYLINE_API void SkylineApplyLiveOps(const char* json, double window_id);
/* [any] Reads the rect snapshot published by the last layout:
   out = {left, top, width, height, z_index, pointer_events_none}.
   Returns 0 when the node has no rect. */
SKYLINE_API int32_t SkylineGetNodeRect(uint32_t node_id, float out[6]);
/* [any->ui] Scrolls a materialized scroll view to a vertical offset. */
SKYLINE_API void SkylineScrollNodeTo(uint32_t node_id, double top);
/* [any->ui] Scrolls `scroll_node_id` until `target_node_id` is visible. */
SKYLINE_API void SkylineScrollIntoView(uint32_t scroll_node_id,
                                       uint32_t target_node_id,
                                       uint8_t within_extent,
                                       uint8_t animated, double offset,
                                       uint8_t alignment);
/* [any->ui] Programmatic swiper paging. */
SKYLINE_API void SkylineSwiperTo(uint32_t node_id, uint32_t page,
                                 uint8_t animated);
/* [any->ui] Sets the selected row of a picker-view column. */
SKYLINE_API void SkylinePickerColumnSetCurrent(uint32_t node_id,
                                               uint32_t current);
/* [any->ui] Programmatic input focus (1) or blur (0). */
SKYLINE_API void SkylineSetInputFocus(uint32_t node_id, uint8_t focus);
/* [any->ui] IntersectionObserver registration; `thresholds` is copied
   (an empty list means {0}). */
SKYLINE_API void SkylineAddIntersectionObserver(
    uint32_t node_id, uint32_t unique_id, uint32_t root_id, double margin_top,
    double margin_right, double margin_bottom, double margin_left,
    const double* thresholds, int32_t threshold_count);
SKYLINE_API void SkylineRemoveIntersectionObserver(uint32_t node_id,
                                                   uint32_t unique_id);
/* [any->ui] <selection> range in (node, text offset) coordinates. */
SKYLINE_API void SkylineSelectionSetRange(uint32_t selection_id,
                                          uint32_t start_id,
                                          int32_t start_offset,
                                          uint32_t end_id,
                                          int32_t end_offset);
/* [any->ui] scroll-view refresher-triggered property. */
SKYLINE_API void SkylineRefresherTriggered(uint32_t node_id, uint8_t open,
                                           uint8_t two_level);
/* [any->ui] Page-level back-gesture interception. */
SKYLINE_API void SkylineSetNavigateBackInterception(int32_t page_id,
                                                    uint8_t enabled);
/* [any] Safe-area insets for CSS env(safe-area-inset-*), logical px. */
SKYLINE_API void SkylineSetSafeAreaInsets(double top, double right,
                                          double bottom, double left);

/* -- Event sinks ------------------------------------------------------------ */

/* [any] */
SKYLINE_API void SkylineSetScrollEventSink(SkylineScrollEventSink sink);
/* [any] */
SKYLINE_API void SkylineSetWidgetEventSink(SkylineWidgetEventSink sink);
/* [any] */
SKYLINE_API void SkylineSetJsonEventSink(SkylineJsonEventSink sink);
/* [any] Installing a sink replays the semantics state the Dart app reported
   before the embedder existed. */
SKYLINE_API void SkylineSetAccessibilitySink(SkylineAccessibilitySink sink);
/* [any] */
SKYLINE_API void SkylineSetHttpRequestSink(
    SkylineHttpRequestSink sink, SkylineHasHttpRequestCallbackFn has_callback);
/* [any->ui] Completes a SkylineHttpRequestSink request; `data` is copied. */
SKYLINE_API void SkylineNotifyHttpRequestComplete(uint32_t request_id,
                                                  const uint8_t* data,
                                                  int32_t length,
                                                  uint8_t from_cache);

/* -- Accessibility and DevTools automation ---------------------------------- */

/* [any] Bit 0: accessibility enabled; bit 1: bold text. */
SKYLINE_API int32_t SkylineGetAccessibilityState(void);
/* [any] Programmatic accessibility toggle; notifies the accessibility sink. */
SKYLINE_API void SkylineRequestAccessibility(uint8_t enabled);
/* [any->ui] */
SKYLINE_API void SkylineDumpElementTree(void);
/* [any->ui] */
SKYLINE_API void SkylineDumpRenderTree(void);
/* [any->ui] */
SKYLINE_API void SkylineCollectGarbage(void);
/* [any->ui] `message` is copied. */
SKYLINE_API void SkylineSendMessageToRunner(const char* message);
/* [any->ui] The result arrives as a JSON event. */
SKYLINE_API void SkylineTakeAppSnapshot(uint32_t request_id, double scale,
                                        uint8_t compressed);
/* [any->ui] The result arrives as a JSON event. */
SKYLINE_API void SkylineSnapshotNode(uint32_t node_id, uint8_t format,
                                     uint32_t request_id);
/* [any->ui] */
SKYLINE_API void SkylineSetUserGesture(uint8_t in_progress);
/* [any->ui] */
SKYLINE_API void SkylineRelaunchApp(uint8_t open_type);
/* [any->ui] */
SKYLINE_API void SkylineShutdownApplication(void);

/* -- Custom-route gesture worklets (workletUIModule) ------------------------ */

/* [any->ui] */
SKYLINE_API void SkylineNavigatorPop(int32_t page_id);
/* [any->ui] */
SKYLINE_API void SkylineStartUserGesture(void);
/* [any->ui] */
SKYLINE_API void SkylineStopUserGesture(void);
/* [any->ui] */
SKYLINE_API void SkylineUpdateAnimationController(int32_t page_id,
                                                  double value);

/* -- Keyboard ---------------------------------------------------------------- */

/* [any->ui] Synthetic key input for the focused <input>: kind 0 down, 1 up,
   2 repeat.  Runs inline when the caller is already inside the isolate (the
   'skyline/keyboard' channel handler that skyline_host drives). */
SKYLINE_API void DispatchSkylineKeyEvent(int32_t kind, uint32_t logical,
                                         const char* text);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif  /* SKYLINE_RUNTIME_API_H_ */
