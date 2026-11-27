#pragma once

#include <memory>

#include "window_backend.hh"

namespace skyline {

// Linux Flutter Engine backend for the N-API module.  Each window owns a
// platform thread running FlutterEngineRun; presented frames are copied into
// the shared-memory data area supplied to createWindow and reported through the
// window's rasterized callback.
std::unique_ptr<WindowBackend> CreateFlutterWindowBackend();

}  // namespace skyline
