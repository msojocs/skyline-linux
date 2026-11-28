// Replay data-model state and shared helpers (appservice_model.cc).

#include "internal/skyline_runtime_module.hh"

namespace appservice {



// CSS percentages resolve against a definite containing size.  The generated
// appservice page relies on WXSS for most of its layout; when that stylesheet
// is unavailable (compiled stylesheets are not replayed yet), a `height: 100%`
// inside an auto-sized ancestor used to multiply the kInf sentinel and make the
// whole subtree measure as infinite, so nothing was painted.  Treat a
// percentage against an unbounded extent as `auto` instead.
float ResolvePercentExtent(float available, float percent) {
  if (percent < 0.0f || available <= 0.0f || available >= kInf) return -1.0f;
  return available * percent;
}



// Nodes with a live native image, keyed by their YGNode address.  The paint
// callback only receives that address.
std::unordered_map<intptr_t, AppNode*> g_image_nodes;

// Nodes whose decorated box paints a CSS background-image.
std::unordered_map<intptr_t, AppNode*> g_background_image_nodes;

// In-memory font state.  `g_pending_fonts` tracks families registered by the
// Dart FontFace loader while their @font-face request is in flight; the other
// maps connect LoadFontFace callback ids to families and record the last
// observed status (-1 while loading).
std::unordered_set<std::string> g_pending_fonts;
std::unordered_map<uint32_t, std::string> g_font_callback_families;
std::unordered_map<std::string, int> g_font_status;

// Native views (`<texture>`) keyed by the JS-visible `viewId`.  Pointer events
// arriving from Dart via `HandleNativeViewPointerEvent` carry that id.
std::unordered_map<int, AppNode*> g_texture_nodes;

// Set while materialising a texture node; `ResetNodeHandles` uses it to let
// Dart release the texture channel state.
TextureDisposeFn g_texture_dispose = nullptr;

// Decoded image cache shared by <image>, CSS backgrounds and SVG results.
// Deliberately leaked: DlImageGPU created with a null unref queue asserts when
// destroyed, and the replay runtime lives for the process lifetime.
std::unordered_map<std::string, sk_sp<flutter::DlImage>>& ImageCache() {
  static auto* cache =
      new std::unordered_map<std::string, sk_sp<flutter::DlImage>>();
  return *cache;
}

// Replaced images are retired instead of destroyed: DlImageGPU holds a
// SkiaGPUObject with a null unref queue, and its destructor asserts.
std::vector<sk_sp<flutter::DlImage>>& RetiredImages() {
  static auto* retired = new std::vector<sk_sp<flutter::DlImage>>();
  return *retired;
}

void CacheImage(const std::string& key, const sk_sp<flutter::DlImage>& image) {
  auto& cache = ImageCache();
  if (image && cache.find(key) == cache.end()) cache.emplace(key, image);
}

// Builder nodes, keyed by YGNode address (the Dart callback only receives the
// address).  Declared here because teardown clears the entry.
std::unordered_map<intptr_t, AppNode*> g_builders_by_address;

// `position: fixed` nodes are collected during the page build and appended to
// the page root connector, so an ancestor scroll does not move them.
std::vector<AppNode*> g_deferred_fixed_nodes;
std::unordered_set<AppNode*> g_deferred_fixed_set;
bool g_collect_fixed_nodes = false;
// The attach-root node whose connector receives promoted fixed nodes.
AppNode* g_fixed_root_node = nullptr;

void ResetDeferredFixedNodes() {
  g_deferred_fixed_nodes.clear();
  g_deferred_fixed_set.clear();
  g_fixed_root_node = nullptr;
}

// Returns true when the child was promoted out of its parent's connector.
bool DeferFixedChild(AppNode* child) {
  if (!g_collect_fixed_nodes || child == nullptr ||
      child->style.position != 2) {
    return false;
  }
  if (g_deferred_fixed_set.insert(child).second) {
    g_deferred_fixed_nodes.push_back(child);
  }
  return true;
}



OpPage* g_page = nullptr;

// Scroll views report native callbacks with `SkylineScrollView.ptr` (the
// AppNode address) as their first argument.  Keep the reverse mapping so the
// appservice mirror can be told which view tag scrolled.
std::unordered_map<intptr_t, uint32_t> g_scroll_view_ids;

std::atomic<SkylineScrollEventSink> g_scroll_event_sink{nullptr};

std::atomic<SkylineWidgetEventSink> g_widget_event_sink{nullptr};

// Address -> view tag for the native Dart widgets whose callbacks receive the
// opaque `ptr` we passed at construction.
std::unordered_map<intptr_t, uint32_t> g_swiper_ids;
std::unordered_map<intptr_t, uint32_t> g_picker_column_ids;
std::unordered_map<intptr_t, uint32_t> g_selection_ids;
// YGNode address -> appservice view tag (intersection callbacks use it).
std::unordered_map<intptr_t, uint32_t> g_node_address_ids;

// IntersectionObserver registrations: unique id -> view tag, and per node.
std::unordered_map<uint32_t, uint32_t> g_intersection_unique_nodes;
std::unordered_map<uint32_t, std::unordered_set<uint32_t>>
    g_intersection_node_observers;

// Per-page back interception flag (mirrored into the Dart route config).
std::unordered_map<int32_t, bool> g_navigate_back_interception;

std::atomic<SkylineJsonEventSink> g_json_event_sink{nullptr};

void EmitJsonEvent(const std::string& json) {
  SkylineJsonEventSink sink = g_json_event_sink.load();
  if (sink != nullptr) sink(json.c_str());
}

std::string JsonEscapeString(const std::string& value) {
  std::string out;
  out.reserve(value.size() + 8);
  for (char c : value) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buffer[8];
          std::snprintf(buffer, sizeof(buffer), "\\u%04x",
                        static_cast<unsigned>(c));
          out += buffer;
        } else {
          out.push_back(c);
        }
        break;
    }
  }
  return out;
}

std::mutex g_rect_mutex;
std::unordered_map<uint32_t, NodeRectSnapshot> g_rect_snapshot;

// Scroll views translate their content at paint time; the C++ layout keeps
// content coordinates.  Selector queries and the mirror hit test need viewport
// coordinates, so subtract every scrolling ancestor's persisted offset (kept
// current by the native scroll callbacks).  `position: fixed` boxes and sticky
// headers are anchored differently, so leave them in their laid-out frame.
bool IsScrollAnchored(const AppNode* node) {
  for (const AppNode* current = node; current != nullptr;
       current = current->parent) {
    if (current->style.position == 2 || current->style.position == 3) {
      return true;
    }
  }
  return false;
}

void ScrollAncestorOffset(const AppNode* node, float* dx, float* dy) {
  *dx = 0.0f;
  *dy = 0.0f;
  for (const AppNode* ancestor = node->parent; ancestor != nullptr;
       ancestor = ancestor->parent) {
    // Only real scroll widgets translate their content.  A `<scroll-view>`
    // component wrapper can mirror the same `scrollTop` attribute without
    // owning a scrollable; counting it would shift the rect twice.
    if (ancestor->scroll_view_p == nullptr) continue;
    const auto top = ancestor->attrs.find("scrollTop");
    if (top != ancestor->attrs.end()) {
      *dy += static_cast<float>(std::atof(top->second.c_str()));
    }
    const auto left = ancestor->attrs.find("scrollLeft");
    if (left != ancestor->attrs.end()) {
      *dx += static_cast<float>(std::atof(left->second.c_str()));
    }
  }
}

namespace {

// Compact, stable number formatting for layout snapshots ("12.5", "3").
std::string SnapshotNumber(double value) {
  if (!std::isfinite(value)) return "null";
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "%.2f", value);
  std::string text(buffer);
  while (text.back() == '0') text.pop_back();
  if (text.back() == '.') text.pop_back();
  if (text == "-0") text = "0";
  return text;
}

std::string SnapshotColor(uint32_t argb) {
  char buffer[16];
  std::snprintf(buffer, sizeof(buffer), "\"#%08x\"", argb);
  return buffer;
}

// One node of a layout snapshot: identity, laid-out border box (content
// coordinates, before transforms and scrolling), a summary of the resolved
// style (defaults omitted) and the widgets BuildWidget materialized for it.
void AppendSnapshotNode(std::string& out, const AppNode* node) {
  const AppStyle& s = node->style;
  out += "{\"id\":" + std::to_string(node->id);
  out += ",\"parent\":" + std::to_string(node->parent ? node->parent->id : 0);
  out += ",\"tag\":\"" + JsonEscapeString(node->tag) + "\"";
  if (!node->name.empty()) out += ",\"name\":\"" + JsonEscapeString(node->name) + "\"";
  if (!node->classes.empty()) {
    std::string classes;
    for (const std::string& name : node->classes) {
      if (!classes.empty()) classes += ' ';
      classes += name;
    }
    out += ",\"class\":\"" + JsonEscapeString(classes) + "\"";
  }
  if (!node->text.empty()) out += ",\"text\":\"" + JsonEscapeString(node->text) + "\"";
  out += ",\"rect\":[" + SnapshotNumber(node->abs_x) + "," + SnapshotNumber(node->abs_y) +
         "," + SnapshotNumber(node->measured_w) + "," + SnapshotNumber(node->measured_h) + "]";

  std::string style;
  auto add = [&style](const char* key, const std::string& value) {
    if (!style.empty()) style += ',';
    style += '"';
    style += key;
    style += "\":";
    style += value;
  };
  const char* display = s.display_none ? "none"
                        : s.inline_block ? "inline-block"
                        : s.inline_level ? "inline"
                        : s.flex_container ? (s.row ? "flex-row" : "flex-column")
                        : nullptr;
  if (display != nullptr) add("display", std::string("\"") + display + "\"");
  if (s.position != 0) add("position", std::to_string(s.position));
  if (s.z_index != 0) add("z", std::to_string(s.z_index));
  if (s.has_background) add("background", SnapshotColor(s.background));
  if (s.color != 0xFF000000u) add("color", SnapshotColor(s.color));
  if (s.font_size != 16.0) add("fontSize", SnapshotNumber(s.font_size));
  if (s.font_weight != 3) add("fontWeight", std::to_string(s.font_weight));
  if (s.opacity != 1.0) add("opacity", SnapshotNumber(s.opacity));
  if (s.border_width > 0.0f) {
    add("border", "[" + SnapshotNumber(s.border_width) + "," + SnapshotColor(s.border_color) + "]");
  }
  if (s.radius > 0.0f) add("radius", SnapshotNumber(s.radius));
  if (s.clip) add("clip", "true");
  if (s.has_transform) add("transform", "true");
  if (s.has_shadow) add("shadow", "true");
  if (s.has_gradient) add("gradient", std::to_string(s.gradient_kind));
  if (!style.empty()) out += ",\"style\":{" + style + "}";

  std::string widgets;
  auto widget = [&widgets](bool present, const char* name) {
    if (!present) return;
    widgets += widgets.empty() ? "\"" : ",\"";
    widgets += name;
    widgets += '"';
  };
  widget(node->decorated_box_p != nullptr, "box");
  widget(node->opacity_widget_p != nullptr, "opacity");
  widget(node->clip_widget_p != nullptr, "clip");
  widget(node->text_span_p != nullptr, "text");
  widget(node->image_widget_p != nullptr, "image");
  widget(node->texture_widget_p != nullptr, "texture");
  widget(node->scroll_view_p != nullptr, "scroll");
  widget(node->swiper_p != nullptr, "swiper");
  widget(node->picker_column_p != nullptr, "picker");
  widget(node->selection_p != nullptr, "selection");
  widget(node->input_widget_p != nullptr, "input");
  widget(node->builder_p != nullptr, "builder");
  widget(node->semantics_p != nullptr, "semantics");
  widget(node->hero_p != nullptr, "hero");
  if (!widgets.empty()) out += ",\"widgets\":[" + widgets + "]";
  out += '}';
}

}  // namespace

// Test hook: SKYLINE_LAYOUT_DUMP=<file> appends one JSON line per distinct
// layout state of the page (test/golden/layout compares them), so refactors
// can be checked structurally instead of through pixels.  Called whenever node
// rects are published and at the end of every applied op batch.
void DumpLayoutSnapshot() {
  const std::string& path = config::String(config::kLayoutDump);
  if (path.empty() || g_page == nullptr) return;
  std::vector<const AppNode*> nodes;
  for (const auto& entry : g_page->nodes) {
    if (entry.second != nullptr && !entry.second->released) nodes.push_back(entry.second);
  }
  std::sort(nodes.begin(), nodes.end(),
            [](const AppNode* a, const AppNode* b) { return a->id < b->id; });
  std::string body;
  for (const AppNode* node : nodes) {
    if (!body.empty()) body += ',';
    AppendSnapshotNode(body, node);
  }
  static std::string last;
  if (body == last) return;
  last = body;
  static int sequence = 0;
  FILE* file = std::fopen(path.c_str(), "a");
  if (file == nullptr) return;
  std::fprintf(file, "{\"seq\":%d,\"nodes\":[%s]}\n", sequence++, body.c_str());
  std::fclose(file);
}

void PublishNodeRects() {
  std::lock_guard<std::mutex> lock(g_rect_mutex);
  g_rect_snapshot.clear();
  if (g_page == nullptr) return;
  const bool debug = SKY_LOG_ENABLED(kDebug);
  for (auto& entry : g_page->nodes) {
    const AppNode* node = entry.second;
    if (node == nullptr || node->released) continue;
    // Publish the painted position: translation transforms move the box, and
    // the appservice hit test / selector queries must see the same geometry a
    // browser's getBoundingClientRect would (the album image is centred with
    // `translate(-50%, -50%)`, so its layout box alone misses taps).
    const AppStyle& style = node->style;
    float tx = style.transform[12];
    float ty = style.transform[13];
    if (!std::isnan(style.translate_percent_x)) {
      tx += style.translate_percent_x * node->measured_w;
    }
    if (!std::isnan(style.translate_percent_y)) {
      ty += style.translate_percent_y * node->measured_h;
    }
    float scroll_x = 0.0f;
    float scroll_y = 0.0f;
    if (!IsScrollAnchored(node)) {
      ScrollAncestorOffset(node, &scroll_x, &scroll_y);
    }
    const float left = node->abs_x + tx - scroll_x;
    const float top = node->abs_y + ty - scroll_y;
    g_rect_snapshot[node->id] = NodeRectSnapshot{
        left, top, node->measured_w, node->measured_h,
        static_cast<float>(style.z_index),
        style.pointer_events_none ? 1.0f : 0.0f};
    if (debug) {
      SKY_LOG(kDebug, kLogOps, "rect #%u left=%g top=%g w=%g h=%g", node->id,
              static_cast<double>(left), static_cast<double>(top),
              static_cast<double>(node->measured_w),
              static_cast<double>(node->measured_h));
    }
  }
  DumpLayoutSnapshot();
}
// Captured on the UI thread; the host keyboard channel is called from the
// embedder thread and must marshal work back onto the UI runner.
fml::RefPtr<fml::TaskRunner> g_ui_task_runner;
std::unique_ptr<rapidjson::Document> g_ops_document;
int g_next_rule_order = 0;
intptr_t g_next_address = 0x5000;

// -- text helpers ------------------------------------------------------------

std::string TrimAscii(const std::string& value) {
  size_t begin = 0;
  size_t end = value.size();
  while (begin < end && std::isspace(static_cast<unsigned char>(value[begin]))) begin++;
  while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1]))) end--;
  return value.substr(begin, end - begin);
}

std::string LowerAscii(const std::string& value) {
  std::string out = value;
  for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

bool EndsWith(const std::string& value, const std::string& suffix) {
  return value.size() >= suffix.size() &&
         value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool StartsWith(const std::string& value, const std::string& prefix) {
  return value.size() >= prefix.size() &&
         value.compare(0, prefix.size(), prefix) == 0;
}

std::vector<std::string> SplitWhitespace(const std::string& value) {
  std::vector<std::string> out;
  size_t i = 0;
  while (i < value.size()) {
    while (i < value.size() && std::isspace(static_cast<unsigned char>(value[i]))) i++;
    const size_t begin = i;
    while (i < value.size() && !std::isspace(static_cast<unsigned char>(value[i]))) i++;
    if (i > begin) out.push_back(value.substr(begin, i - begin));
  }
  return out;
}

std::vector<std::string> SplitChar(const std::string& value, char separator) {
  std::vector<std::string> out;
  std::string current;
  for (char c : value) {
    if (c == separator) {
      out.push_back(TrimAscii(current));
      current.clear();
    } else {
      current.push_back(c);
    }
  }
  out.push_back(TrimAscii(current));
  return out;
}



}  // namespace appservice

// Live DevTools bridge state.  The addon may call SkylineApplyLiveOps from the
// Electron renderer thread before the Dart app has registered its UI task
// runner, so payloads are parked until NotifyFlutterReady runs.
std::mutex g_live_ops_mutex;
std::string g_pending_live_ops;
bool g_live_ready = false;
fml::RefPtr<fml::TaskRunner> g_live_ui_runner;
