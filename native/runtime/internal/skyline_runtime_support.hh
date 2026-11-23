// Shared replay support: isolate/scope, UI task runner, resource paths.

#pragma once
#include "skyline_runtime_appservice.hh"

bool ReadResourceBytes(const std::string& path, std::vector<uint8_t>* out);
fml::RefPtr<fml::TaskRunner> UiTaskRunner();
std::string ResolveResourcePath(const std::string& path);
void ReportDartError(const char* what, Dart_Handle handle);

// Enters the root isolate/scope for the posted UI task; released on every exit
// path (including constructor errors) so later tasks can re-enter safely.
class DemoPageApiScope {
 public:
  DemoPageApiScope() {
    if (Dart_CurrentIsolate() == nullptr && g_isolate != nullptr) {
      Dart_EnterIsolate(g_isolate);
      entered_isolate_ = true;
    }
    Dart_EnterScope();
  }

  ~DemoPageApiScope() {
    Dart_ExitScope();
    if (entered_isolate_) {
      Dart_ExitIsolate();
    }
  }

 private:
  bool entered_isolate_ = false;
};

// Runs `fn` on the Dart UI thread once the live bridge is ready.  Exported
// bridge wrappers and async resource callbacks both use it.
template <typename Fn>
void PostToUiIfReady(Fn fn) {
  fml::RefPtr<fml::TaskRunner> runner;
  {
    std::lock_guard<std::mutex> lock(g_live_ops_mutex);
    if (!g_live_ready) return;
    runner = g_live_ui_runner;
  }
  if (!runner) return;
  runner->PostTask([fn]() {
    DemoPageApiScope scope;
    fn();
  });
}
