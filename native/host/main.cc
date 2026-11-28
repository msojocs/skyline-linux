// Headless Linux Flutter embedder used by the phase-2 smoke test.
//
// Lifecycle: FlutterEngineRun -> SendWindowMetricsEvent -> service vsync batons ->
// wait for one software surface present -> FlutterEngineShutdown -> write the RGBA
// frame into /dev/shm/skyline_<key>.dat (docs/contract.md section 4).
//
// The engine is built with `--unoptimized --runtime-mode=debug`; a release engine
// ignores kernel_blob.bin and this host will report a timeout.

#include <flutter_embedder.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include "embedder/embedder.hh"
#include "frame_writer.hh"
#include "skyline/config.h"
#include "skyline/log.h"

// Weak references keep the host usable with an engine built without the
// embedded Skyline runtime: every runtime function is then null.
#define SKYLINE_RUNTIME_API_WEAK
#include "skyline/runtime_api.h"

namespace {

constexpr char kLog[] = "host";

constexpr int kExitOk = 0;
constexpr int kExitEngineRunFailed = 70;
constexpr int kExitTimeout = 71;
constexpr int kExitSharedMemory = 72;
constexpr int kExitBadArguments = 78;

using skyline::embedder::FileExists;
using skyline::embedder::JoinPath;

struct HostState {
  // Vsync batons, platform tasks and accessibility requests; serviced by the
  // main thread, which runs FlutterEngineRun.
  skyline::embedder::PlatformLoop loop{kLog, ""};

  std::mutex mutex;
  // `presented` becomes true once `captured_frames` reaches `target_frames`.
  bool presented = false;
  bool stop_capturing = false;
  uint32_t present_count = 0;
  uint32_t target_frames = 1;
  uint32_t captured_frames = 0;

  uint32_t width = 0;
  uint32_t height = 0;
  std::vector<uint8_t> rgba;

  // Set before FlutterEngineRun; used by the present callback.
  bool rgba_surface = false;
};

// The embedder forwards the framework's semantics tree here.  Under
// SKYLINE_APP_SERVICE_DEBUG each node is logged so automation/tests can assert
// labels/roles without a platform accessibility service.
void OnSemanticsUpdate(const FlutterSemanticsUpdate2* update, void* /*data*/) {
  skyline::embedder::LogSemanticsUpdate(kLog, update);
}

struct Options {
  std::string assets;
  std::string icu;
  int width = 320;
  int height = 240;
  double dpr = 1.0;
  std::string shm_key = "engine_smoke";
  int timeout_ms = 15000;
  int frames = 1;
  std::string pixel_format = "bgra";
  // AOT snapshot (`libapp.so`) for release engines.  When omitted the host
  // looks for `<assets>/libapp.so` and `<assets>/../libapp.so`.
  std::string aot_library;
  // Optional custom Dart entrypoint and its arguments (FlutterProjectArgs).
  std::string entrypoint;
  std::vector<std::string> entrypoint_args;
  // Exit right after a captured frame instead of waiting for engine shutdown.
  // The package:skyline appservice keeps async work alive for a long time, so
  // its embedder shutdown can exceed the test budget.
  bool no_shutdown = false;
  // Timed pointer events used to exercise gestures/scroll without GTK.
  std::string pointer_script;
  // Timed keyboard events used to exercise the key input path without GTK.
  std::string key_script;
  // Timed viewport metric updates used to exercise resize handling.
  std::string resize_script;
  struct ResizeEvent {
    int at_ms = 0;
    double width = 0.0;
    double height = 0.0;
    double dpr = 0.0;  // 0 = keep the initial ratio
  };
  std::vector<ResizeEvent> resize_events;
  struct KeyScriptEvent {
    int at_ms = 0;
    FlutterKeyEventType type = kFlutterKeyEventTypeDown;
    uint64_t physical = 0;
    uint64_t logical = 0;
    std::string character;
  };
  std::vector<KeyScriptEvent> key_events;
  struct PointerScriptEvent {
    int at_ms = 0;
    FlutterPointerPhase phase = kDown;
    double x = 0.0;
    double y = 0.0;
    int32_t device = 1;
    bool mouse = false;
    FlutterPointerSignalKind signal_kind = kFlutterPointerSignalKindNone;
    double scroll_dx = 0.0;
    double scroll_dy = 0.0;
  };
  std::vector<PointerScriptEvent> pointer_events;
};

void OnLogMessage(const char* tag, const char* message, void* /*user_data*/) {
  skyline::embedder::LogEngineMessage("flutter", tag, message);
}

void OnVsync(void* user_data, intptr_t baton) {
  static_cast<HostState*>(user_data)->loop.OnVsync(baton);
}

bool OnSurfacePresent(void* user_data, const void* allocation, size_t row_bytes,
                      size_t height) {
  auto* state = static_cast<HostState*>(user_data);
  if (allocation == nullptr || row_bytes < 4 || height == 0) return true;
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    ++state->present_count;
    if (state->stop_capturing) return true;
    if (state->captured_frames >= state->target_frames) return true;

    const uint32_t width = static_cast<uint32_t>(row_bytes / 4);
    state->rgba.resize(static_cast<size_t>(width) * height * 4);
    skyline::embedder::CopyFrame(allocation, row_bytes, width, height,
                                 state->rgba.data(), state->rgba_surface);
    state->width = width;
    state->height = static_cast<uint32_t>(height);
    ++state->captured_frames;
    if (state->captured_frames >= state->target_frames) {
      state->presented = true;
      state->stop_capturing = true;
    }
  }
  state->loop.Wake();
  return true;
}

bool Presented(HostState* state) {
  std::lock_guard<std::mutex> lock(state->mutex);
  return state->presented;
}

bool ParseInt(const char* value, int* out) {
  if (value == nullptr || *value == '\0') return false;
  char* end = nullptr;
  const long parsed = std::strtol(value, &end, 10);
  if (end == value || *end != '\0') return false;
  *out = static_cast<int>(parsed);
  return true;
}

bool ParseDouble(const char* value, double* out) {
  if (value == nullptr || *value == '\0') return false;
  char* end = nullptr;
  const double parsed = std::strtod(value, &end);
  if (end == value || *end != '\0') return false;
  *out = parsed;
  return true;
}

void PrintUsage(const char* argv0) {
  std::fprintf(stderr,
               "usage: %s --assets DIR --icu FILE [options]\n"
               "  --assets DIR        directory containing kernel_blob.bin / libapp.so\n"
               "  --icu FILE          icudtl.dat\n"
               "  --width N           logical width (default 320)\n"
               "  --height N          logical height (default 240)\n"
               "  --dpr F             device pixel ratio (default 1.0)\n"
               "  --shm-key KEY       /dev/shm/skyline_<KEY>.dat (default engine_smoke)\n"
               "  --timeout-ms N      frame timeout (default 15000)\n"
               "  --pixel-format F    bgra (convert to RGBA) or rgba (default bgra)\n"
               "  --frames N          Nth frame (default 1); 0 = capture until timeout, keep last\n"
               "  --entrypoint NAME   custom Dart entrypoint (default main)\n"
               "  --entrypoint-arg V  Dart entrypoint argument (repeatable)\n"
               "  --aot-library FILE  libapp.so for release engines (default: auto)\n"
               "  --no-shutdown       exit after the first frame (skip engine shutdown)\n"
               "  --pointer-script F  timed pointer events: '<ms> <phase> <x> <y> [device]'\n"
               "                      phase: add/down/move/up/cancel/hover or scroll dx dy;\n"
               "                      prefix with m for mouse (mdown/mmove/mup)\n"
               "  --key-script F      timed key events: '<ms> <down|up|repeat> <physical> <logical> [char]'\n"
               "  --resize-script F   timed viewport updates: '<ms> <width> <height> [dpr]'\n",
               argv0);
}

bool ParsePointerScript(const std::string& path,
                        std::vector<Options::PointerScriptEvent>* out) {
  std::ifstream file(path);
  if (!file) {
    std::fprintf(stderr, "cannot read pointer script: %s\n", path.c_str());
    return false;
  }
  std::string line;
  int line_number = 0;
  while (std::getline(file, line)) {
    ++line_number;
    const size_t comment = line.find('#');
    if (comment != std::string::npos) line.resize(comment);
    std::istringstream stream(line);
    int at_ms = 0;
    std::string phase;
    double x = 0.0;
    double y = 0.0;
    int device = 1;
    if (!(stream >> at_ms >> phase >> x >> y)) continue;
    Options::PointerScriptEvent event;
    if (phase == "scroll") {
      double dx = 0.0;
      double dy = 0.0;
      if (!(stream >> dx >> dy)) {
        std::fprintf(stderr, "pointer script line %d: scroll needs dx dy\n",
                     line_number);
        return false;
      }
      event.phase = kHover;
      event.mouse = true;
      event.device = 0;
      event.signal_kind = kFlutterPointerSignalKindScroll;
      event.scroll_dx = dx;
      event.scroll_dy = dy;
      event.x = x;
      event.y = y;
      event.at_ms = at_ms;
      out->push_back(event);
      continue;
    }
    stream >> device;
    event.at_ms = at_ms;
    event.x = x;
    event.y = y;
    event.device = device;
    // Prefix a phase with `m` to send it as a mouse device (`mdown`, `mmove`,
    // `mup`, ...); useful for exercising desktop drag/wheel behaviour.
    std::string base_phase = phase;
    if (base_phase.size() > 1 && base_phase[0] == 'm') {
      const std::string rest = base_phase.substr(1);
      if (rest == "add" || rest == "down" || rest == "move" ||
          rest == "up" || rest == "cancel" || rest == "hover") {
        base_phase = rest;
        event.mouse = true;
      }
    }
    if (base_phase == "add") event.phase = kAdd;
    else if (base_phase == "down") event.phase = kDown;
    else if (base_phase == "move") event.phase = kMove;
    else if (base_phase == "up") event.phase = kUp;
    else if (base_phase == "cancel") event.phase = kCancel;
    else if (base_phase == "hover") event.phase = kHover;
    else {
      std::fprintf(stderr, "pointer script line %d: unknown phase %s\n",
                   line_number, phase.c_str());
      return false;
    }
    out->push_back(event);
  }
  return true;
}

bool ParseKeyScript(const std::string& path,
                    std::vector<Options::KeyScriptEvent>* out) {
  std::ifstream file(path);
  if (!file) {
    std::fprintf(stderr, "cannot read key script: %s\n", path.c_str());
    return false;
  }
  std::string line;
  int line_number = 0;
  while (std::getline(file, line)) {
    ++line_number;
    const size_t comment = line.find('#');
    if (comment != std::string::npos) line.resize(comment);
    std::istringstream stream(line);
    int at_ms = 0;
    std::string phase;
    unsigned long long physical = 0;
    unsigned long long logical = 0;
    if (!(stream >> at_ms >> phase >> physical >> logical)) continue;
    Options::KeyScriptEvent event;
    event.at_ms = at_ms;
    event.physical = physical;
    event.logical = logical;
    if (phase == "down") {
      event.type = kFlutterKeyEventTypeDown;
    } else if (phase == "up") {
      event.type = kFlutterKeyEventTypeUp;
    } else if (phase == "repeat") {
      event.type = kFlutterKeyEventTypeRepeat;
    } else {
      std::fprintf(stderr, "key script line %d: unknown phase %s\n",
                   line_number, phase.c_str());
      return false;
    }
    std::string character;
    if (stream >> character) {
      if (character == "space") character = " ";
      if (character == "backspace") character.clear();
      event.character = character;
    }
    out->push_back(event);
  }
  return true;
}

bool ParseResizeScript(const std::string& path,
                       std::vector<Options::ResizeEvent>* out) {
  std::ifstream file(path);
  if (!file) {
    std::fprintf(stderr, "cannot read resize script: %s\n", path.c_str());
    return false;
  }
  std::string line;
  int line_number = 0;
  while (std::getline(file, line)) {
    ++line_number;
    const size_t comment = line.find('#');
    if (comment != std::string::npos) line.resize(comment);
    std::istringstream stream(line);
    Options::ResizeEvent event;
    if (!(stream >> event.at_ms >> event.width >> event.height)) continue;
    if (!(event.width > 0.0) || !(event.height > 0.0)) {
      std::fprintf(stderr, "resize script line %d: invalid size\n",
                   line_number);
      return false;
    }
    if (stream >> event.dpr) {
      if (!(event.dpr > 0.0)) {
        std::fprintf(stderr, "resize script line %d: invalid dpr\n",
                     line_number);
        return false;
      }
    }
    out->push_back(event);
  }
  return true;
}

bool ParseOptions(int argc, char** argv, Options* opts) {
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto next = [&](const char* name) -> const char* {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "%s requires a value\n", name);
        return nullptr;
      }
      return argv[++i];
    };
    if (arg == "--help" || arg == "-h") {
      PrintUsage(argv[0]);
      return false;
    } else if (arg == "--assets") {
      const char* v = next("--assets");
      if (!v) return false;
      opts->assets = v;
    } else if (arg == "--icu") {
      const char* v = next("--icu");
      if (!v) return false;
      opts->icu = v;
    } else if (arg == "--width") {
      const char* v = next("--width");
      if (!v || !ParseInt(v, &opts->width)) return false;
    } else if (arg == "--height") {
      const char* v = next("--height");
      if (!v || !ParseInt(v, &opts->height)) return false;
    } else if (arg == "--dpr") {
      const char* v = next("--dpr");
      if (!v || !ParseDouble(v, &opts->dpr)) return false;
    } else if (arg == "--shm-key") {
      const char* v = next("--shm-key");
      if (!v) return false;
      opts->shm_key = v;
    } else if (arg == "--timeout-ms") {
      const char* v = next("--timeout-ms");
      if (!v || !ParseInt(v, &opts->timeout_ms)) return false;
    } else if (arg == "--frames") {
      const char* v = next("--frames");
      if (!v || !ParseInt(v, &opts->frames)) return false;
    } else if (arg == "--pixel-format") {
      const char* v = next("--pixel-format");
      if (!v) return false;
      opts->pixel_format = v;
    } else if (arg == "--entrypoint") {
      const char* v = next("--entrypoint");
      if (!v) return false;
      opts->entrypoint = v;
    } else if (arg == "--entrypoint-arg") {
      const char* v = next("--entrypoint-arg");
      if (!v) return false;
      opts->entrypoint_args.emplace_back(v);
    } else if (arg == "--pointer-script") {
      const char* v = next("--pointer-script");
      if (!v) return false;
      opts->pointer_script = v;
    } else if (arg == "--key-script") {
      const char* v = next("--key-script");
      if (!v) return false;
      opts->key_script = v;
    } else if (arg == "--resize-script") {
      const char* v = next("--resize-script");
      if (!v) return false;
      opts->resize_script = v;
    } else if (arg == "--aot-library") {
      const char* v = next("--aot-library");
      if (!v) return false;
      opts->aot_library = v;
    } else if (arg == "--no-shutdown") {
      opts->no_shutdown = true;
    } else {
      std::fprintf(stderr, "unknown argument: %s\n", arg.c_str());
      return false;
    }
  }

  if (opts->assets.empty() || opts->icu.empty()) {
    std::fprintf(stderr, "--assets and --icu are required\n");
    return false;
  }
  if (opts->width <= 0 || opts->height <= 0 || opts->dpr <= 0 ||
      opts->timeout_ms <= 0) {
    std::fprintf(stderr, "width/height/dpr/timeout must be positive\n");
    return false;
  }
  if (!opts->pointer_script.empty() &&
      !ParsePointerScript(opts->pointer_script, &opts->pointer_events)) {
    return false;
  }
  if (!opts->key_script.empty() &&
      !ParseKeyScript(opts->key_script, &opts->key_events)) {
    return false;
  }
  if (!opts->resize_script.empty() &&
      !ParseResizeScript(opts->resize_script, &opts->resize_events)) {
    return false;
  }
  if (opts->pixel_format != "bgra" && opts->pixel_format != "rgba") {
    std::fprintf(stderr, "pixel format must be bgra or rgba\n");
    return false;
  }
  if (opts->frames > 1) {
    std::fprintf(stderr, "note: --frames %d requested; capturing the first frame\n",
                 opts->frames);
  }
  return true;
}

void OnKeyEvent(bool handled, void* /*user_data*/) {
  SKY_LOG(kInfo, kLog, "key event handled=%d", handled ? 1 : 0);
}

}  // namespace

int main(int argc, char** argv) {
  Options opts;
  if (!ParseOptions(argc, argv, &opts)) return kExitBadArguments;

  const std::string kernel = JoinPath(opts.assets, "kernel_blob.bin");
  const bool has_kernel = FileExists(kernel);
  const bool use_aot = !opts.aot_library.empty() || !has_kernel;
  const std::string aot_library =
      !opts.aot_library.empty()
          ? opts.aot_library
          : skyline::embedder::FindAotLibrary(JoinPath(opts.assets, ".."),
                                              opts.assets);
  const bool has_aot = !aot_library.empty() && FileExists(aot_library);
  if (!has_kernel && !has_aot) {
    std::fprintf(stderr,
                 "missing kernel blob and AOT snapshot: %s / libapp.so\n",
                 kernel.c_str());
    return kExitBadArguments;
  }
  if (use_aot && !has_aot) {
    std::fprintf(stderr, "missing AOT snapshot: %s\n", aot_library.c_str());
    return kExitBadArguments;
  }
  if (!FileExists(opts.icu)) {
    std::fprintf(stderr, "missing ICU data: %s\n", opts.icu.c_str());
    return kExitBadArguments;
  }

  HostState state;
  state.rgba_surface = opts.pixel_format == "rgba";
  state.target_frames =
      opts.frames > 0 ? static_cast<uint32_t>(opts.frames) : UINT32_MAX;

  skyline::embedder::EngineConfig config;
  config.assets = opts.assets;
  config.icu = opts.icu;
  if (use_aot) config.aot_library = aot_library;
  config.entrypoint = opts.entrypoint;
  config.entrypoint_args = opts.entrypoint_args;
  config.program = "skyline_engine_host";
  config.present = &OnSurfacePresent;
  config.vsync = &OnVsync;
  // D10: capture the framework semantics tree (only produced once the runtime
  // enables semantics through the accessibility sink below).
  config.semantics = &OnSemanticsUpdate;
  config.log_message = &OnLogMessage;
  config.user_data = &state;

  std::unique_ptr<skyline::embedder::RunningEngine> engine;
  try {
    engine = std::make_unique<skyline::embedder::RunningEngine>(config);
  } catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what());
    return kExitEngineRunFailed;
  }
  if (use_aot) {
    SKY_LOG(kInfo, kLog, "AOT snapshot: %s", aot_library.c_str());
  }
  // The embedded runtime (linked into libflutter_engine.so) may have queued an
  // accessibility state before the host existed.  Install the sink now; its
  // setter replays semantics-enabled state, and the ops below drive features.
  skyline::embedder::SetAccessibilityTarget(&state.loop);
  if (SkylineSetAccessibilitySink != nullptr) {
    SkylineSetAccessibilitySink(&skyline::embedder::RouteAccessibility);
  }
  // Shuts the engine down on the platform (creation) thread.
  auto finish = [&](int code) {
    skyline::embedder::ClearAccessibilityTarget(&state.loop);
    const FlutterEngineResult result = engine->Shutdown();
    if (code == kExitOk && result != kSuccess) {
      std::fprintf(stderr, "FlutterEngineShutdown failed: %d\n",
                   static_cast<int>(result));
    }
    return code;
  };

  const FlutterEngineResult metrics_result =
      engine->SendMetrics(opts.width, opts.height, opts.dpr);
  if (metrics_result != kSuccess) {
    std::fprintf(stderr, "FlutterEngineSendWindowMetricsEvent failed: %d\n",
                 static_cast<int>(metrics_result));
    return finish(kExitEngineRunFailed);
  }

  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(opts.timeout_ms);
  const uint64_t script_start_ns = FlutterEngineGetCurrentTime();
  auto due = [script_start_ns](int at_ms) {
    return FlutterEngineGetCurrentTime() >=
           script_start_ns + static_cast<uint64_t>(at_ms) * 1000000ull;
  };
  size_t next_pointer = 0;
  size_t next_key = 0;
  size_t next_resize = 0;
  bool pointer_error_reported = false;
  while (!Presented(&state)) {
    // Dispatch the scripted events whose time has come.  This thread ran
    // FlutterEngineRun, so it may call the input embedder APIs directly.
    while (next_pointer < opts.pointer_events.size() &&
           due(opts.pointer_events[next_pointer].at_ms)) {
      const auto& scripted = opts.pointer_events[next_pointer];
      FlutterPointerEvent event = {};
      event.struct_size = sizeof(event);
      event.phase = scripted.phase;
      event.timestamp = FlutterEngineGetCurrentTime();
      event.x = scripted.x * opts.dpr;
      event.y = scripted.y * opts.dpr;
      event.device = scripted.device;
      event.device_kind = scripted.mouse ? kFlutterPointerDeviceKindMouse
                                         : kFlutterPointerDeviceKindTouch;
      event.buttons =
          scripted.mouse && (scripted.phase == kDown || scripted.phase == kMove)
              ? kFlutterPointerButtonMousePrimary
              : 0;
      event.signal_kind = scripted.signal_kind;
      event.scroll_delta_x = scripted.scroll_dx;
      event.scroll_delta_y = scripted.scroll_dy;
      event.view_id = static_cast<FlutterViewId>(
          skyline::config::Int(skyline::config::kPointerViewId));
      const FlutterEngineResult pointer_result =
          FlutterEngineSendPointerEvent(engine->get(), &event, 1);
      if (pointer_result != kSuccess && !pointer_error_reported) {
        std::fprintf(stderr, "FlutterEngineSendPointerEvent failed: %d\n",
                     static_cast<int>(pointer_result));
        pointer_error_reported = true;
      }
      ++next_pointer;
    }

    // Scripted key events feed both the embedder path
    // (`FlutterEngineSendKeyEvent`, delivered to the Flutter framework) and
    // the embedded runtime channel (synthetic appservice input).
    while (next_key < opts.key_events.size() &&
           due(opts.key_events[next_key].at_ms)) {
      const auto& scripted = opts.key_events[next_key];
      FlutterKeyEvent event = {};
      event.struct_size = sizeof(event);
      event.timestamp = static_cast<double>(FlutterEngineGetCurrentTime());
      event.type = scripted.type;
      event.physical = scripted.physical;
      event.logical = scripted.logical;
      event.character =
          scripted.character.empty() ? nullptr : scripted.character.c_str();
      event.synthesized = false;
      event.device_type = kFlutterKeyEventDeviceTypeKeyboard;
      const FlutterEngineResult key_result =
          FlutterEngineSendKeyEvent(engine->get(), &event, &OnKeyEvent, nullptr);
      if (key_result != kSuccess) {
        std::fprintf(stderr, "FlutterEngineSendKeyEvent failed: %d\n",
                     static_cast<int>(key_result));
      }
      if (DispatchSkylineKeyEvent != nullptr) {
        int32_t runtime_kind = 0;
        if (scripted.type == kFlutterKeyEventTypeUp) {
          runtime_kind = 1;
        } else if (scripted.type == kFlutterKeyEventTypeRepeat) {
          runtime_kind = 2;
        }
        // Deliver on the engine's UI thread: the sample appservice registers
        // a 'skyline/keyboard' message handler that calls the embedded
        // runtime export.  Calling it directly here would touch the Dart
        // isolate from the wrong OS thread.
        std::string payload = std::to_string(runtime_kind) + " " +
                              std::to_string(scripted.logical) + " " +
                              scripted.character;
        FlutterPlatformMessage message = {};
        message.struct_size = sizeof(message);
        message.channel = "skyline/keyboard";
        message.message = reinterpret_cast<const uint8_t*>(payload.data());
        message.message_size = payload.size();
        message.response_handle = nullptr;
        const FlutterEngineResult message_result =
            FlutterEngineSendPlatformMessage(engine->get(), &message);
        if (message_result != kSuccess) {
          std::fprintf(stderr, "FlutterEngineSendPlatformMessage failed: %d\n",
                       static_cast<int>(message_result));
        }
      } else {
        SKY_LOG(kWarning, kLog, "DispatchSkylineKeyEvent unavailable");
      }
      SKY_LOG(kInfo, kLog, "key event sent: at=%d type=%d logical=%llu",
              scripted.at_ms, static_cast<int>(scripted.type),
              static_cast<unsigned long long>(scripted.logical));
      ++next_key;
    }

    // Scripted viewport updates (resize path).
    while (next_resize < opts.resize_events.size() &&
           due(opts.resize_events[next_resize].at_ms)) {
      const auto& scripted = opts.resize_events[next_resize];
      const double dpr = scripted.dpr > 0.0 ? scripted.dpr : opts.dpr;
      const FlutterEngineResult resize_result =
          engine->SendMetrics(scripted.width, scripted.height, dpr);
      if (resize_result != kSuccess) {
        std::fprintf(stderr,
                     "FlutterEngineSendWindowMetricsEvent (resize) failed: %d\n",
                     static_cast<int>(resize_result));
      }
      SKY_LOG(kInfo, kLog, "resize: at=%d %gx%g dpr=%g -> %zux%zu",
              scripted.at_ms, scripted.width, scripted.height, dpr,
              skyline::embedder::PhysicalPixels(scripted.width, dpr),
              skyline::embedder::PhysicalPixels(scripted.height, dpr));
      ++next_resize;
    }

    const bool pending_events = next_pointer < opts.pointer_events.size() ||
                                next_key < opts.key_events.size() ||
                                next_resize < opts.resize_events.size();
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) break;
    state.loop.RunOnce(
        engine->get(),
        pending_events ? std::chrono::milliseconds(5)
                       : std::chrono::duration_cast<std::chrono::milliseconds>(
                             deadline - now));
  }

  bool got_frame = false;
  uint32_t present_count = 0;
  {
    std::lock_guard<std::mutex> lock(state.mutex);
    got_frame = state.captured_frames > 0;
    present_count = state.present_count;
    state.stop_capturing = true;
  }

  std::fprintf(stderr,
               "host wait finished: got_frame=%d vsync=%u present=%u batons=%zu\n",
               got_frame ? 1 : 0, state.loop.vsync_count(), present_count,
               state.loop.pending_batons());
  std::fflush(stderr);

  if (!got_frame) {
    std::fprintf(stderr,
                 "timeout after %d ms: no software frame (vsync=%u present=%u)\n",
                 opts.timeout_ms, state.loop.vsync_count(), present_count);
    return finish(kExitTimeout);
  }
  // Return outstanding batons so shutdown does not leak frame requests.
  state.loop.DrainVsync(engine->get(), std::chrono::milliseconds(150));

  const uint32_t width = state.width;
  const uint32_t height = state.height;
  const size_t frame_size = static_cast<size_t>(width) * height * 4;
  if (frame_size == 0 || state.rgba.size() != frame_size) {
    std::fprintf(stderr, "invalid captured frame\n");
    return finish(kExitSharedMemory);
  }

  skyline::SharedFrameWriter writer(opts.shm_key);
  if (!writer.Valid()) {
    std::fprintf(stderr, "cannot open shared memory for key %s\n",
                 opts.shm_key.c_str());
    return finish(kExitSharedMemory);
  }
  std::string error;
  if (!writer.WriteFrame(1, state.rgba.data(), frame_size, &error)) {
    std::fprintf(stderr, "cannot write shared memory: %s\n", error.c_str());
    return finish(kExitSharedMemory);
  }

  const uint32_t first = state.rgba[0];
  const uint32_t second = state.rgba[1];
  const uint32_t third = state.rgba[2];
  const uint32_t fourth = state.rgba[3];
  std::printf(
      "SKYLINE_HOST_RESULT {\"frameId\":1,\"width\":%u,\"height\":%u,"
      "\"frameSize\":%zu,\"pixelFormat\":\"%s\",\"firstPixel\":[%u,%u,%u,%u],"
      "\"vsyncCount\":%u,\"presentCount\":%u,\"shmPath\":\"%s\","
      "\"shmDataSize\":%zu}\n",
      width, height, frame_size, opts.pixel_format.c_str(), first, second, third,
      fourth, state.loop.vsync_count(), present_count, writer.path().c_str(),
      writer.data_size());
  std::fflush(stdout);

  if (opts.no_shutdown) {
    std::fflush(nullptr);
    std::_Exit(kExitOk);
  }

  // The appservice kernel keeps timers/async work alive and can make shutdown
  // slow; use --no-shutdown in smoke tests that only need the frame.
  return finish(kExitOk);
}
