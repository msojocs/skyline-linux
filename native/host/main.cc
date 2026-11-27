// Headless Linux Flutter embedder used by the phase-2 smoke test.
//
// Lifecycle: FlutterEngineRun -> SendWindowMetricsEvent -> service vsync batons ->
// wait for one software surface present -> FlutterEngineShutdown -> write the RGBA
// frame into /dev/shm/skyline_<key>.dat (docs/contract.md section 4).
//
// The engine is built with `--unoptimized --runtime-mode=debug`; a release engine
// ignores kernel_blob.bin and this host will report a timeout.

#include <flutter_embedder.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "frame_writer.hh"

// Weak references keep the host usable with an engine built without the
// embedded Skyline runtime: every runtime function is then null.
#define SKYLINE_RUNTIME_API_WEAK
#include "skyline/runtime_api.h"

namespace {

constexpr int kExitOk = 0;
constexpr int kExitEngineRunFailed = 70;
constexpr int kExitTimeout = 71;
constexpr int kExitSharedMemory = 72;
constexpr int kExitBadArguments = 78;

struct HostState {
  std::mutex mutex;
  std::condition_variable cv;

  std::deque<intptr_t> batons;
  uint32_t vsync_count = 0;

  // `presented` becomes true once `captured_frames` reaches `target_frames`.
  bool presented = false;
  bool stop_capturing = false;
  uint32_t present_count = 0;
  uint32_t target_frames = 1;
  uint32_t captured_frames = 0;

  uint32_t width = 0;
  uint32_t height = 0;
  size_t row_bytes = 0;
  std::vector<uint8_t> rgba;

  // Set before FlutterEngineRun; used by the callbacks.
  std::string pixel_format = "bgra";

  // Accessibility requests queued by the embedded runtime from the Dart UI
  // thread.  They are applied on the platform thread (the thread that called
  // FlutterEngineRun), which is where the embedder semantics APIs must run.
  std::deque<std::pair<int32_t, int32_t>> accessibility_requests;
};

HostState* g_accessibility_host = nullptr;

void OnAccessibilitySink(int32_t kind, int32_t value) {
  HostState* state = g_accessibility_host;
  if (state == nullptr) return;
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    state->accessibility_requests.emplace_back(kind, value);
  }
  state->cv.notify_all();
}

// The embedder forwards the framework's semantics tree here.  Under
// SKYLINE_APP_SERVICE_DEBUG each node is logged so automation/tests can assert
// labels/roles without a platform accessibility service.
void OnSemanticsUpdate(const FlutterSemanticsUpdate2* update, void* /*data*/) {
  if (update == nullptr || std::getenv("SKYLINE_APP_SERVICE_DEBUG") == nullptr) {
    return;
  }
  for (size_t i = 0; i < update->node_count; i++) {
    const FlutterSemanticsNode2* node = update->nodes[i];
    if (node == nullptr) continue;
    const bool is_button =
        node->flags2 != nullptr && node->flags2->is_button;
    const bool is_text_field =
        node->flags2 != nullptr && node->flags2->is_text_field;
    const bool is_header = node->flags2 != nullptr && node->flags2->is_header;
    std::fprintf(stderr,
                 "[host] semantics node: id=%d label=\"%s\" value=\"%s\" "
                 "button=%d textField=%d header=%d children=%zu "
                 "rect=(%g,%g %gx%g)\n",
                 node->id, node->label != nullptr ? node->label : "",
                 node->value != nullptr ? node->value : "", is_button ? 1 : 0,
                 is_text_field ? 1 : 0, is_header ? 1 : 0, node->child_count,
                 node->rect.left, node->rect.top,
                 node->rect.right - node->rect.left,
                 node->rect.bottom - node->rect.top);
  }
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
  std::fprintf(stderr, "[flutter:%s] %s\n", tag ? tag : "?",
               message ? message : "");
}

void OnVsync(void* user_data, intptr_t baton) {
  auto* state = static_cast<HostState*>(user_data);
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    state->batons.push_back(baton);
    ++state->vsync_count;
  }
  state->cv.notify_all();
}

bool OnSurfacePresent(void* user_data, const void* allocation, size_t row_bytes,
                      size_t height) {
  auto* state = static_cast<HostState*>(user_data);
  if (allocation == nullptr || row_bytes < 4 || height == 0) return true;

  std::lock_guard<std::mutex> lock(state->mutex);
  ++state->present_count;
  if (state->stop_capturing) return true;
  if (state->captured_frames >= state->target_frames) return true;

  const uint32_t width = static_cast<uint32_t>(row_bytes / 4);
  const size_t frame_size = static_cast<size_t>(width) * height * 4;
  state->rgba.resize(frame_size);

  const uint8_t* src = static_cast<const uint8_t*>(allocation);
  uint8_t* dst = state->rgba.data();
  if (state->pixel_format == "rgba") {
    for (size_t y = 0; y < height; ++y) {
      std::memcpy(dst + y * width * 4, src + y * row_bytes,
                  static_cast<size_t>(width) * 4);
    }
  } else {
    // Skia's N32 software surface is BGRA on little-endian Linux; the contract
    // wants RGBA8.  Swap R/B and keep A.
    for (size_t y = 0; y < height; ++y) {
      const uint8_t* row_src = src + y * row_bytes;
      uint8_t* row_dst = dst + y * width * 4;
      for (uint32_t x = 0; x < width; ++x) {
        row_dst[x * 4 + 0] = row_src[x * 4 + 2];
        row_dst[x * 4 + 1] = row_src[x * 4 + 1];
        row_dst[x * 4 + 2] = row_src[x * 4 + 0];
        row_dst[x * 4 + 3] = row_src[x * 4 + 3];
      }
    }
  }

  state->width = width;
  state->height = static_cast<uint32_t>(height);
  state->row_bytes = row_bytes;
  ++state->captured_frames;
  if (state->captured_frames >= state->target_frames) {
    state->presented = true;
    state->stop_capturing = true;
  }
  state->cv.notify_all();
  return true;
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

std::string JoinPath(const std::string& dir, const std::string& name) {
  if (!dir.empty() && dir.back() == '/') return dir + name;
  return dir + "/" + name;
}

bool FileExists(const std::string& path) {
  std::FILE* f = std::fopen(path.c_str(), "rb");
  if (f == nullptr) return false;
  std::fclose(f);
  return true;
}

// Returns the batons that are currently queued.
std::deque<intptr_t> TakeBatons(HostState* state) {
  std::lock_guard<std::mutex> lock(state->mutex);
  std::deque<intptr_t> result;
  result.swap(state->batons);
  return result;
}

void ReturnBatons(FlutterEngine engine, const std::deque<intptr_t>& batons) {
  for (intptr_t baton : batons) {
    const uint64_t now = FlutterEngineGetCurrentTime();
    const uint64_t target = now + 16666667;  // 60 Hz hint.
    (void)FlutterEngineOnVsync(engine, baton, now, target);
  }
}

void OnKeyEvent(bool handled, void* /*user_data*/) {
  std::fprintf(stderr, "[host] key event handled=%d\n", handled ? 1 : 0);
}

}  // namespace

int main(int argc, char** argv) {
  Options opts;
  if (!ParseOptions(argc, argv, &opts)) return kExitBadArguments;

  const std::string kernel = JoinPath(opts.assets, "kernel_blob.bin");
  const bool has_kernel = FileExists(kernel);
  const bool force_aot = !opts.aot_library.empty();
  bool use_aot = force_aot || !has_kernel;
  std::string aot_library = opts.aot_library;
  if (aot_library.empty()) {
    const std::string candidates[] = {
        JoinPath(opts.assets, "libapp.so"),
        JoinPath(opts.assets, "../libapp.so"),
    };
    for (const std::string& candidate : candidates) {
      if (FileExists(candidate)) {
        aot_library = candidate;
        break;
      }
    }
  }
  if (!has_kernel && (!use_aot || aot_library.empty() ||
                      !FileExists(aot_library))) {
    std::fprintf(stderr,
                 "missing kernel blob and AOT snapshot: %s / libapp.so\n",
                 kernel.c_str());
    return kExitBadArguments;
  }
  if (use_aot && (aot_library.empty() || !FileExists(aot_library))) {
    std::fprintf(stderr, "missing AOT snapshot: %s\n", aot_library.c_str());
    return kExitBadArguments;
  }
  if (!FileExists(opts.icu)) {
    std::fprintf(stderr, "missing ICU data: %s\n", opts.icu.c_str());
    return kExitBadArguments;
  }

  HostState state;
  state.pixel_format = opts.pixel_format;
  state.target_frames =
      opts.frames > 0 ? static_cast<uint32_t>(opts.frames) : UINT32_MAX;

  FlutterRendererConfig renderer = {};
  renderer.type = kSoftware;
  renderer.software.struct_size = sizeof(FlutterSoftwareRendererConfig);
  renderer.software.surface_present_callback = &OnSurfacePresent;

  FlutterProjectArgs args = {};
  args.struct_size = sizeof(FlutterProjectArgs);
  args.assets_path = opts.assets.c_str();
  args.icu_data_path = opts.icu.c_str();
  FlutterEngineAOTData aot_data = nullptr;
  if (use_aot && !aot_library.empty() && FileExists(aot_library)) {
    FlutterEngineAOTDataSource source = {};
    source.type = kFlutterEngineAOTDataSourceTypeElfPath;
    source.elf_path = aot_library.c_str();
    if (FlutterEngineCreateAOTData(&source, &aot_data) != kSuccess ||
        aot_data == nullptr) {
      std::fprintf(stderr, "cannot load AOT snapshot: %s\n",
                   aot_library.c_str());
      return kExitEngineRunFailed;
    }
    args.aot_data = aot_data;
    std::fprintf(stderr, "[host] AOT snapshot: %s\n", aot_library.c_str());
  }
  args.vsync_callback = &OnVsync;
  args.log_message_callback = &OnLogMessage;
  args.log_tag = "skyline_host";
  args.shutdown_dart_vm_when_done = true;
  // D10: capture the framework semantics tree (only produced once the runtime
  // enables semantics through the accessibility sink below).
  args.update_semantics_callback2 = &OnSemanticsUpdate;
  if (!opts.entrypoint.empty()) {
    args.custom_dart_entrypoint = opts.entrypoint.c_str();
  }
  std::vector<const char*> dart_entrypoint_argv;
  dart_entrypoint_argv.reserve(opts.entrypoint_args.size());
  for (const std::string& value : opts.entrypoint_args) {
    dart_entrypoint_argv.push_back(value.c_str());
  }
  if (!dart_entrypoint_argv.empty()) {
    args.dart_entrypoint_argc = static_cast<int>(dart_entrypoint_argv.size());
    args.dart_entrypoint_argv = dart_entrypoint_argv.data();
  }
  const char* flutter_argv[] = {"skyline_engine_host", "--disable-vm-service"};
  args.command_line_argc = 2;
  args.command_line_argv = flutter_argv;

  FlutterEngine engine = nullptr;
  const FlutterEngineResult run_result =
      FlutterEngineRun(FLUTTER_ENGINE_VERSION, &renderer, &args, &state, &engine);
  if (run_result != kSuccess || engine == nullptr) {
    std::fprintf(stderr, "FlutterEngineRun failed: %d\n",
                 static_cast<int>(run_result));
    return kExitEngineRunFailed;
  }
  // The embedded runtime (linked into libflutter_engine.so) may have queued an
  // accessibility state before the host existed.  Install the sink now; its
  // setter replays semantics-enabled state, and the ops below drive features.
  g_accessibility_host = &state;
  if (SkylineSetAccessibilitySink != nullptr) {
    SkylineSetAccessibilitySink(&OnAccessibilitySink);
  }
  auto collect_aot = [&aot_data]() {
    if (aot_data != nullptr) {
      FlutterEngineCollectAOTData(aot_data);
      aot_data = nullptr;
    }
  };

  FlutterWindowMetricsEvent metrics = {};
  metrics.struct_size = sizeof(FlutterWindowMetricsEvent);
  metrics.width = static_cast<size_t>(std::ceil(opts.width * opts.dpr));
  metrics.height = static_cast<size_t>(std::ceil(opts.height * opts.dpr));
  metrics.pixel_ratio = opts.dpr;
  metrics.view_id = 0;  // kFlutterImplicitViewId
  const FlutterEngineResult metrics_result =
      FlutterEngineSendWindowMetricsEvent(engine, &metrics);
  if (metrics_result != kSuccess) {
    std::fprintf(stderr, "FlutterEngineSendWindowMetricsEvent failed: %d\n",
                 static_cast<int>(metrics_result));
    FlutterEngineShutdown(engine);
    collect_aot();
    return kExitEngineRunFailed;
  }

  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(opts.timeout_ms);
  const uint64_t pointer_start_ns = FlutterEngineGetCurrentTime();
  size_t next_pointer = 0;
  size_t next_key = 0;
  size_t next_resize = 0;
  bool pointer_error_reported = false;
  bool got_frame = false;
  {
    std::unique_lock<std::mutex> lock(state.mutex);
    while (!state.presented) {
      // Dispatch any pointer events whose script time has elapsed.  Keep the
      // lock released while calling into the engine: the present callback may
      // take the same mutex.
      while (next_pointer < opts.pointer_events.size()) {
        const auto& scripted = opts.pointer_events[next_pointer];
        const uint64_t now_ns = FlutterEngineGetCurrentTime();
        const uint64_t due_ns =
            pointer_start_ns + static_cast<uint64_t>(scripted.at_ms) * 1000000ull;
        if (now_ns < due_ns) break;
        FlutterPointerEvent event = {};
        event.struct_size = sizeof(event);
        event.phase = scripted.phase;
        event.timestamp = now_ns;
        event.x = scripted.x * opts.dpr;
        event.y = scripted.y * opts.dpr;
        event.device = scripted.device;
        event.device_kind = scripted.mouse ? kFlutterPointerDeviceKindMouse
                                           : kFlutterPointerDeviceKindTouch;
        event.buttons =
            scripted.mouse &&
                    (scripted.phase == kDown || scripted.phase == kMove)
                ? kFlutterPointerButtonMousePrimary
                : 0;
        event.signal_kind = scripted.signal_kind;
        event.scroll_delta_x = scripted.scroll_dx;
        event.scroll_delta_y = scripted.scroll_dy;
        const char* view_id_env = std::getenv("SKYLINE_POINTER_VIEW_ID");
        event.view_id = view_id_env != nullptr ? std::atoi(view_id_env) : 0;
        lock.unlock();
        const FlutterEngineResult pointer_result =
            FlutterEngineSendPointerEvent(engine, &event, 1);
        lock.lock();
        if (pointer_result != kSuccess && !pointer_error_reported) {
          std::fprintf(stderr, "FlutterEngineSendPointerEvent failed: %d\n",
                       static_cast<int>(pointer_result));
          pointer_error_reported = true;
        }
        ++next_pointer;
      }

      // Dispatch scripted key events.  Both the embedder path
      // (`FlutterEngineSendKeyEvent`, delivered to the Flutter framework) and
      // the embedded runtime channel (synthetic appservice input) are fed.
      while (next_key < opts.key_events.size()) {
        const auto& scripted = opts.key_events[next_key];
        const uint64_t now_ns = FlutterEngineGetCurrentTime();
        const uint64_t due_ns =
            pointer_start_ns + static_cast<uint64_t>(scripted.at_ms) * 1000000ull;
        if (now_ns < due_ns) break;
        FlutterKeyEvent event = {};
        event.struct_size = sizeof(event);
        event.timestamp = static_cast<double>(now_ns);
        event.type = scripted.type;
        event.physical = scripted.physical;
        event.logical = scripted.logical;
        event.character =
            scripted.character.empty() ? nullptr : scripted.character.c_str();
        event.synthesized = false;
        event.device_type = kFlutterKeyEventDeviceTypeKeyboard;
        lock.unlock();
        const FlutterEngineResult key_result =
            FlutterEngineSendKeyEvent(engine, &event, &OnKeyEvent, nullptr);
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
          message.message =
              reinterpret_cast<const uint8_t*>(payload.data());
          message.message_size = payload.size();
          message.response_handle = nullptr;
          const FlutterEngineResult message_result =
              FlutterEngineSendPlatformMessage(engine, &message);
          if (message_result != kSuccess) {
            std::fprintf(stderr,
                         "FlutterEngineSendPlatformMessage failed: %d\n",
                         static_cast<int>(message_result));
          }
        } else {
          std::fprintf(stderr,
                       "[host] DispatchSkylineKeyEvent unavailable\n");
        }
        lock.lock();
        std::fprintf(stderr, "[host] key event sent: at=%d type=%d logical=%llu\n",
                     scripted.at_ms, static_cast<int>(scripted.type),
                     static_cast<unsigned long long>(scripted.logical));
        ++next_key;
      }

      // Apply scripted viewport updates (resize path).
      while (next_resize < opts.resize_events.size()) {
        const auto& scripted = opts.resize_events[next_resize];
        const uint64_t now_ns = FlutterEngineGetCurrentTime();
        const uint64_t due_ns =
            pointer_start_ns + static_cast<uint64_t>(scripted.at_ms) * 1000000ull;
        if (now_ns < due_ns) break;
        const double dpr = scripted.dpr > 0.0 ? scripted.dpr : opts.dpr;
        FlutterWindowMetricsEvent metrics = {};
        metrics.struct_size = sizeof(FlutterWindowMetricsEvent);
        metrics.width =
            static_cast<size_t>(std::ceil(scripted.width * dpr));
        metrics.height =
            static_cast<size_t>(std::ceil(scripted.height * dpr));
        metrics.pixel_ratio = dpr;
        metrics.view_id = 0;
        lock.unlock();
        const FlutterEngineResult resize_result =
            FlutterEngineSendWindowMetricsEvent(engine, &metrics);
        lock.lock();
        if (resize_result != kSuccess) {
          std::fprintf(stderr,
                       "FlutterEngineSendWindowMetricsEvent (resize) failed: %d\n",
                       static_cast<int>(resize_result));
        }
        std::fprintf(stderr,
                     "[host] resize: at=%d %gx%g dpr=%g -> %zux%zu\n",
                     scripted.at_ms, scripted.width, scripted.height, dpr,
                     metrics.width, metrics.height);
        ++next_resize;
      }

      const bool pending_pointers = next_pointer < opts.pointer_events.size();
      const bool pending_keys = next_key < opts.key_events.size();
      const bool pending_resizes = next_resize < opts.resize_events.size();
      const bool pending_events =
          pending_pointers || pending_keys || pending_resizes;
      const std::cv_status status =
          pending_events
              ? state.cv.wait_for(lock, std::chrono::milliseconds(5))
              : state.cv.wait_until(lock, deadline);

      // The engine posts semantic updates (and other platform callbacks) to the
      // platform task runner.  This host does not run an fml message loop, so
      // drain the pending tasks explicitly on every iteration.
      lock.unlock();
      __FlutterEngineFlushPendingTasksNow();
      lock.lock();

      // Apply accessibility requests on the platform thread (the embedder API
      // requires the FlutterEngineRun thread).
      std::deque<std::pair<int32_t, int32_t>> accessibility;
      accessibility.swap(state.accessibility_requests);
      if (!accessibility.empty()) {
        lock.unlock();
        for (const auto& request : accessibility) {
          if (request.first == 0) {
            const FlutterAccessibilityFeature features =
                request.second != 0
                    ? kFlutterAccessibilityFeatureAccessibleNavigation
                    : static_cast<FlutterAccessibilityFeature>(0);
            const FlutterEngineResult result =
                FlutterEngineUpdateAccessibilityFeatures(engine, features);
            std::fprintf(stderr,
                         "[host] accessibility features: %d (result=%d)\n",
                         static_cast<int>(features),
                         static_cast<int>(result));
          } else {
            const FlutterEngineResult result =
                FlutterEngineUpdateSemanticsEnabled(engine,
                                                    request.second != 0);
            std::fprintf(stderr,
                         "[host] semantics enabled: %d (result=%d)\n",
                         request.second != 0 ? 1 : 0,
                         static_cast<int>(result));
          }
        }
        lock.lock();
      }

      if (!state.presented && std::chrono::steady_clock::now() >= deadline) {
        break;
      }
      if (!pending_events && status == std::cv_status::timeout &&
          !state.presented) {
        break;
      }
      if (!state.batons.empty()) {
        std::deque<intptr_t> batons;
        batons.swap(state.batons);
        lock.unlock();
        ReturnBatons(engine, batons);
        lock.lock();
      }
    }
    got_frame = state.captured_frames > 0;
    state.stop_capturing = true;
  }

  std::fprintf(stderr,
               "host wait finished: got_frame=%d vsync=%u present=%u batons=%zu\n",
               got_frame ? 1 : 0, state.vsync_count, state.present_count,
               state.batons.size());
  std::fflush(stderr);

  if (got_frame) {
    // Return outstanding batons so shutdown does not leak frame requests.
    const auto drain_until =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(150);
    while (std::chrono::steady_clock::now() < drain_until) {
      {
        std::unique_lock<std::mutex> lock(state.mutex);
        if (!state.batons.empty()) {
          std::deque<intptr_t> batons;
          batons.swap(state.batons);
          lock.unlock();
          ReturnBatons(engine, batons);
          continue;
        }
        state.cv.wait_for(lock, std::chrono::milliseconds(20));
      }
      std::deque<intptr_t> pending = TakeBatons(&state);
      if (pending.empty()) {
        // Nothing pending; small grace period before shutdown.
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      } else {
        ReturnBatons(engine, pending);
      }
    }
    // Drain one final time in case a baton arrived during the grace period.
    ReturnBatons(engine, TakeBatons(&state));
  }

  if (!got_frame) {
    std::fprintf(stderr,
                 "timeout after %d ms: no software frame (vsync=%u present=%u)\n",
                 opts.timeout_ms, state.vsync_count, state.present_count);
    FlutterEngineShutdown(engine);
    collect_aot();
    return kExitTimeout;
  }

  const uint32_t width = state.width;
  const uint32_t height = state.height;
  const size_t frame_size = static_cast<size_t>(width) * height * 4;
  if (frame_size == 0 || state.rgba.size() != frame_size) {
    std::fprintf(stderr, "invalid captured frame\n");
    FlutterEngineShutdown(engine);
    collect_aot();
    return kExitSharedMemory;
  }

  skyline::SharedFrameWriter writer(opts.shm_key);
  if (!writer.Valid()) {
    std::fprintf(stderr, "cannot open shared memory for key %s\n",
                 opts.shm_key.c_str());
    FlutterEngineShutdown(engine);
    collect_aot();
    return kExitSharedMemory;
  }
  std::string error;
  if (!writer.WriteFrame(1, state.rgba.data(), frame_size, &error)) {
    std::fprintf(stderr, "cannot write shared memory: %s\n", error.c_str());
    FlutterEngineShutdown(engine);
    collect_aot();
    return kExitSharedMemory;
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
      fourth, state.vsync_count, state.present_count, writer.path().c_str(),
      static_cast<size_t>(8 + frame_size * 4));
  std::fflush(stdout);

  if (opts.no_shutdown) {
    std::fflush(nullptr);
    std::_Exit(kExitOk);
  }

  // FlutterEngineShutdown must run on the platform (creation) thread.  The
  // appservice kernel keeps timers/async work alive and can make shutdown slow;
  // use --no-shutdown in smoke tests that only need the frame.
  g_accessibility_host = nullptr;
  const FlutterEngineResult shutdown_result = FlutterEngineShutdown(engine);
  if (shutdown_result != kSuccess) {
    std::fprintf(stderr, "FlutterEngineShutdown failed: %d\n",
                 static_cast<int>(shutdown_result));
  }
  collect_aot();
  return kExitOk;
}
