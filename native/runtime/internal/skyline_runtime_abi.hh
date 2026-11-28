#pragma once
// Shared ABI/bootstrap surface for every runtime translation unit.
// Types and helpers that used to live in the monolith's anonymous namespace
// are declared here; definitions live in the entry TU (or the module that
// owns the state once the replay is split).

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifdef SKYLINE_EMBEDDED
// Statically linked into libflutter_engine.so: the full Dart API is available.
#include "third_party/dart/runtime/include/dart_api.h"
#include "rapidjson/document.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/writer.h"
#include <fstream>
#include <iterator>
#include <string>
// Native image drawing: the direct-render paint callback receives a `dart:ui`
// Canvas.  The engine's C++ Canvas/DisplayList classes let us decode with Skia
// and append a DrawImageRect without round-tripping through Dart objects.
#include "flutter/display_list/dl_builder.h"
#include "flutter/display_list/image/dl_image.h"
#include "flutter/display_list/skia/dl_sk_dispatcher.h"
#include "flutter/lib/ui/compositing/scene.h"
#include "flutter/lib/ui/painting/canvas.h"
#include "flutter/lib/ui/painting/display_list_image_gpu.h"
#include "flutter/lib/ui/painting/image.h"
#include "third_party/skia/include/core/SkData.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "third_party/skia/include/core/SkImage.h"
#include "third_party/skia/include/core/SkSurface.h"
#include "third_party/skia/include/encode/SkPngEncoder.h"
#include "third_party/tonic/converter/dart_converter.h"
#include "third_party/tonic/dart_wrappable.h"
// Schedule page construction after the current FFI callback returns; the Dart VM
// prohibits re-entering a native callback synchronously ("Cannot invoke native
// callback when API callbacks are prohibited").
#include "flutter/fml/message_loop.h"
#include "flutter/fml/task_runner.h"
#else
#if __has_include(<dart_api_dl.h>)
#include <dart_api_dl.h>
#endif
#endif

#if defined(_WIN32)
#define SKYLINE_EXPORT __declspec(dllexport)
#elif defined(__GNUC__)
#define SKYLINE_EXPORT __attribute__((visibility("default")))
#else
#define SKYLINE_EXPORT
#endif

// The host/addon-facing exports and their sink types.  Including the public
// declarations here makes the compiler check every definition against them.
#include "skyline/runtime_api.h"
// Environment variables, read once (tools/env-vars.json).
#include "skyline/config.h"
namespace config = skyline::config;
// SKY_LOG; the categories keep the prefixes of the runtime's earlier output.
#include "skyline/log.h"
inline constexpr char kLogRuntime[] = "skyline-runtime";
inline constexpr char kLogOps[] = "ops-debug";

// Global-scope ABI value types returned/consumed by the exported C API.
struct SkylineLayoutSize {
  double width;
  double height;
};

struct SkylineDryLayoutResult {
  float width;
  float height;
};

struct SkylineNativeRect {
  double left;
  double top;
  double right;
  double bottom;
  double width;
  double height;
};

#ifdef SKYLINE_EMBEDDED
using SkylineHandle = Dart_Handle;
#else
using SkylineHandle = void*;
#endif

// stringHash('RunApplication') from spec/kernel-symbols.json.
inline constexpr uint32_t kRunApplicationHash = 548712037u;

// FFI registration table and the runtime/window objects handed back as opaque
// pointers.  Internal to libskyline/libflutter_engine; the library hides these
// symbols (no SKYLINE_EXPORT).
struct FfiEntry {
  uint32_t hash;
  void* function;
};

// `Register` receives `stringHash(name)` values, not dense indices, so keep an
// append-only table.  The official runtime uses a hash lookup; the stub only
// needs to accept registrations for now.
struct FfiTable {
  uint32_t count = 0;
  FfiEntry entries[4096];
};

struct RuntimeHolder {
  FfiTable* ffi_table = nullptr;
  void* dart_isolate = nullptr;
};

struct Runtime {
  RuntimeHolder* holder = nullptr;
};

struct Window {
  Runtime* runtime = nullptr;
  int32_t id = 0;
};

extern std::vector<RuntimeHolder*> g_known_holders;
extern RuntimeHolder g_fallback_holder;
extern bool g_dart_api_initialized;
// Viewport size reported by CreateWindow; the replay lays out the root page.
extern double g_viewport_width;
extern double g_viewport_height;
#ifdef SKYLINE_EMBEDDED
extern Dart_Isolate g_isolate;
#endif

RuntimeHolder* HolderOrFallback(void* pointer);
void LogTwo(skyline::log::Level level, const char* category, SkylineHandle tag,
            SkylineHandle message);
void* FindCallback(FfiTable* table, uint32_t hash);

template <typename T>
intptr_t AsHandle(T* value) {
  return reinterpret_cast<intptr_t>(value);
}

// The live DevTools bridge schedules pending page ops on the UI thread.
void ApplyPendingLiveOpsOnUi();

#ifdef SKYLINE_EMBEDDED
// Synthetic/recorded page entry points defined in embedded_runtime.cc.
void CreateDemoPage(RuntimeHolder* holder);
void CreateTextPage(RuntimeHolder* holder);
void CreateJsonPage(RuntimeHolder* holder, const char* descriptor);
void CreateAppServiceOpsPage(RuntimeHolder* holder, const char* descriptor);
void ScheduleCreateDemoPage(RuntimeHolder* holder);
#endif
