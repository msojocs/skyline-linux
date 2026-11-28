#pragma once
// Runtime state shared between the engine UI thread and the threads that call
// the skyline/runtime_api.h exports (the addon's JS thread, host threads).
//
// Everything else in the replay (appservice::g_page and the node maps, image,
// font and animation state) belongs to the UI thread: the exports post their
// work there, and the functions that touch it start with
// SKY_DCHECK_UI_THREAD().
#include "skyline_runtime_abi.hh"

struct RuntimeState {
  // The engine UI task runner and the live-ops handoff, guarded by `mutex`.
  std::mutex mutex;
  fml::RefPtr<fml::TaskRunner> ui_runner;
  // Set once the Dart app runs (NotifyFlutterReady); bridge calls are posted
  // to `ui_runner` only from then on.
  bool live_ready = false;
  // The batch that arrived before `live_ready`, applied when it is set.
  std::string pending_live_ops;

  // Sinks installed through runtime_api.h.
  std::atomic<SkylineScrollEventSink> scroll_sink{nullptr};
  std::atomic<SkylineWidgetEventSink> widget_sink{nullptr};
  std::atomic<SkylineJsonEventSink> json_sink{nullptr};
  std::atomic<SkylineAccessibilitySink> accessibility_sink{nullptr};
  // DevTools shell HTTP bridge: the addon proxies `setHttpRequestCallback`
  // (window.fetch in the pageframe renderer) so network requests show up in
  // the DevTools Network panel instead of an in-engine dart:io socket.
  std::atomic<SkylineHttpRequestSink> http_request_sink{nullptr};
  std::atomic<SkylineHasHttpRequestCallbackFn> http_available{nullptr};

  // Safe-area insets in logical px (top, right, bottom, left), pushed by
  // SkylineShell.setSafeAreaEdgeInsets and read by CSS env().
  std::atomic<float> safe_area[4] = {};
};

RuntimeState& SharedState();

// A copy of the UI task runner; null until the runtime has run on the UI
// thread.
fml::RefPtr<fml::TaskRunner> SharedUiRunner();
// UI thread: records the current task runner as the UI runner.
void RememberUiRunner();
// Whether the caller runs on the UI thread (true while it is not known yet).
bool OnUiThread();

// Debug engines report a UI-thread-only function that runs elsewhere, once
// per function, as an error log line (tests fail on it).
#ifndef NDEBUG
void DcheckUiThread(const char* function);
#define SKY_DCHECK_UI_THREAD() DcheckUiThread(__func__)
#else
#define SKY_DCHECK_UI_THREAD() static_cast<void>(0)
#endif
