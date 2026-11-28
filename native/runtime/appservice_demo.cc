// Synthetic Skyline demo pages (stripes/text/card/JSON descriptor).

#include "internal/skyline_runtime_module.hh"
#include "internal/skyline_runtime_demo.hh"


// ---------------------------------------------------------------------------
// Synthetic Skyline page (embedded build)
//
// With the full Dart API available we can call the real `package:skyline`
// constructors.  This builds a minimal tree:
//
//   CreatePageRoute
//     +- YGNode (root, yoga layout buffer)
//          +- DecoratedBox (solid #FF3366)
//
// The yoga layout callback fills the root's 9-float metrics buffer.
// ---------------------------------------------------------------------------

namespace {

std::unordered_map<intptr_t, float*> g_yoga_metrics;
bool g_demo_page_created = false;


enum class DemoMode { kStripes, kText, kCard };
DemoMode g_demo_mode = DemoMode::kStripes;

// No-op native callbacks passed to NewText.  They are only invoked when the
// text needs a dirty notification / native paragraph; neither affects the
// Flutter-rendered text path (enableNativeParagraph=false).
extern "C" void DemoMarkDirty(intptr_t /*ptr*/) {}
extern "C" void DemoCreateParagraph(intptr_t /*ptr*/, Dart_Handle /*paragraph*/,
                                    float /*maxWidth*/, uint8_t /*isDryLayout*/) {}

// Layout model for the synthetic page: a vertical column of stripes.
struct DemoNode {
  intptr_t address;
  int index;  // 0 = root, 1..n = child stripes (top to bottom)
};
std::vector<DemoNode> g_demo_nodes;

// Descriptor-driven page (`SKYLINE_STUB_PAGE={...}`): nested row/column flex.
struct JsonNode {
  intptr_t address = 0;
  bool row = false;
  float padding = 0.0f;
  float gap = 0.0f;
  float flex = 0.0f;
  float main_size = 0.0f;  // NaN when flexible
  std::vector<JsonNode> children;
};
JsonNode g_json_root;
bool g_json_active = false;

struct PendingColorUpdate {
  Dart_PersistentHandle decorated_box;
  uint32_t color;
  int delay_ms;
};
std::vector<PendingColorUpdate> g_pending_color_updates;

Dart_PersistentHandle g_json_root_connector = nullptr;
struct RootChildRecord {
  Dart_PersistentHandle handle;
  intptr_t address;
};
std::vector<RootChildRecord> g_json_root_child_records;

enum class StyleUpdateKind { kOpacity, kBorder };
struct PendingStyleUpdate {
  Dart_PersistentHandle handle;
  StyleUpdateKind kind;
  double value;
  double width;
  uint32_t color;
  int delay_ms;
};
std::vector<PendingStyleUpdate> g_pending_style_updates;
OpacityUpdateFn g_update_opacity = nullptr;
DecoratedBoxUpdateBorderFn g_update_border = nullptr;
intptr_t g_json_next_address = 0x1100;

void LayoutJsonNode(const JsonNode& node, float left, float top, float width,
                    float height);

void SchedulePendingStyleUpdates() {
  if (g_pending_style_updates.empty()) {
    return;
  }
  auto task_runner = UiTaskRunner();
  if (!task_runner) {
    g_pending_style_updates.clear();
    return;
  }
  for (const PendingStyleUpdate& update : g_pending_style_updates) {
    task_runner->PostDelayedTask(
        [update]() {
          if (Dart_CurrentIsolate() == nullptr && g_isolate == nullptr) {
            return;
          }
          DemoPageApiScope scope;
          Dart_Handle handle = Dart_HandleFromPersistent(update.handle);
          if (update.kind == StyleUpdateKind::kOpacity) {
            if (g_update_opacity != nullptr) {
              g_update_opacity(handle, update.value);
            }
          } else if (g_update_border != nullptr) {
            const uint8_t style = update.width > 0 ? 1 : 0;
            g_update_border(handle, update.width, update.width, update.width,
                            update.width, update.color, update.color,
                            update.color, update.color, style, style, style,
                            style);
          }
          Dart_DeletePersistentHandle(update.handle);
        },
        fml::TimeDelta::FromMilliseconds(update.delay_ms));
  }
  g_pending_style_updates.clear();
}

}  // namespace

void CreateDemoPage(RuntimeHolder* holder) {
  const char* ops_descriptor =
      config::IsSet(config::kAppServiceOps)
          ? config::String(config::kAppServiceOps).c_str()
          : nullptr;
  const char* stub_page = config::IsSet(config::kStubPage)
                              ? config::String(config::kStubPage).c_str()
                              : nullptr;
  if (ops_descriptor == nullptr && stub_page == nullptr) {
    return;  // opt-in: synthetic page for tests, not the default appservice UI
  }
  if (g_demo_page_created || holder == nullptr) {
    return;
  }
  if (Dart_CurrentIsolate() == nullptr && g_isolate == nullptr) {
    std::fprintf(stderr, "[skyline-runtime] demo page: no isolate available\n");
    return;
  }
  DemoPageApiScope scope;
  auto* newDecoratedBox = reinterpret_cast<NewDecoratedBoxFn>(
      FindCallback(holder->ffi_table, kNewDecoratedBoxHash));
  auto* newYGNode = reinterpret_cast<NewYGNodeFn>(
      FindCallback(holder->ffi_table, kNewYGNodeHash));
  auto* createPageRoute = reinterpret_cast<CreatePageRouteFn>(
      FindCallback(holder->ffi_table, kCreatePageRouteHash));
  if (newDecoratedBox == nullptr || newYGNode == nullptr ||
      createPageRoute == nullptr) {
    std::fprintf(stderr,
                 "[skyline-runtime] demo page callbacks not registered "
                 "(decorated=%p yg=%p route=%p)\n",
                 reinterpret_cast<void*>(newDecoratedBox),
                 reinterpret_cast<void*>(newYGNode),
                 reinterpret_cast<void*>(createPageRoute));
    return;
  }
  g_demo_page_created = true;
  if (ops_descriptor != nullptr) {
    g_json_active = true;
    CreateAppServiceOpsPage(holder, ops_descriptor);
    return;
  }
  const char* descriptor = config::IsSet(config::kStubPageJson)
                               ? config::String(config::kStubPageJson).c_str()
                               : nullptr;
  if (descriptor == nullptr && stub_page != nullptr &&
      (stub_page[0] == '{' || stub_page[0] == '@')) {
    descriptor = stub_page;
  }
  if (descriptor != nullptr) {
    g_json_active = true;
    CreateJsonPage(holder, descriptor);
    return;
  }
  const char* mode = stub_page;
  if (mode != nullptr && std::strcmp(mode, "text") == 0) {
    g_demo_mode = DemoMode::kText;
    CreateTextPage(holder);
    return;
  }
  if (mode != nullptr && std::strcmp(mode, "card") == 0) {
    g_demo_mode = DemoMode::kCard;
    CreateTextPage(holder);
    return;
  }
  g_demo_mode = DemoMode::kStripes;

  auto* newYGConnector = reinterpret_cast<NewYGConnectorFn>(
      FindCallback(holder->ffi_table, kNewYGConnectorHash));
  if (newYGConnector == nullptr) {
    std::fprintf(stderr, "[skyline-runtime] demo page: NewYGConnector not registered\n");
    return;
  }

  Dart_Handle empty = Dart_NewStringFromCString("");
  constexpr int kChildCount = 3;
  const uint32_t colors[kChildCount] = {0xFFFF3366u, 0xFF33CC66u, 0xFF3366FFu};
  g_demo_nodes.clear();

  Dart_Handle children = Dart_NewList(kChildCount);
  if (Dart_IsError(children)) {
    ReportDartError("Dart_NewList", children);
    return;
  }

  for (int i = 0; i < kChildCount; i++) {
    const intptr_t child_address = 0x1100 + i * 0x100;
    Dart_Handle yoga_layout = Dart_NewTypedData(Dart_TypedData_kFloat32, 9);
    if (Dart_IsError(yoga_layout)) {
      ReportDartError("Dart_NewTypedData(child)", yoga_layout);
      return;
    }
    Dart_Handle decorated = newDecoratedBox(
        child_address, colors[i],
        0.0, 0.0, 0.0, 0.0,
        0, 0, 0, 0,
        0, 0, 0, 0,
        0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0,
        0, 0, 0, 0, 0, 0, 0, 0,
        Dart_Null(), 0, 0,
        0.0, 0.0, 0.0, 0.0,
        0, 0,
        0.0, 0.0,
        0, 0,
        Dart_Null(), 0,
        Dart_Null(), 0,
        nullptr, nullptr, nullptr, nullptr,
        Dart_Null());
    if (Dart_IsError(decorated)) {
      ReportDartError("NewDecoratedBox(child)", decorated);
      return;
    }

    Dart_Handle child_node = newYGNode(
        child_address, yoga_layout,
        0, 0, 0, 0, 1, Dart_Null(),
        0.0, 0.0, 0.0, 0.0,
        1, 0, 0,
        Dart_Null(), Dart_Null(), decorated);
    if (Dart_IsError(child_node)) {
      ReportDartError("NewYGNode(child)", child_node);
      return;
    }
    Dart_Handle set_status = Dart_ListSetAt(children, i, child_node);
    if (Dart_IsError(set_status)) {
      ReportDartError("Dart_ListSetAt", set_status);
      return;
    }

    Dart_TypedData_Type type = Dart_TypedData_kInvalid;
    void* data = nullptr;
    intptr_t length = 0;
    Dart_Handle status =
        Dart_TypedDataAcquireData(yoga_layout, &type, &data, &length);
    if (!Dart_IsError(status) && type == Dart_TypedData_kFloat32 &&
        length == 9 && data != nullptr) {
      g_yoga_metrics[child_address] = static_cast<float*>(data);
      // AcquireData starts a VM "no callback" scope; release immediately.  The
      // YGNode keeps the Float32List alive and typed-data memory does not move.
      Dart_TypedDataReleaseData(yoga_layout);
    } else {
      std::fprintf(stderr, "[skyline-runtime] demo page: bad child yoga buffer\n");
    }
    g_demo_nodes.push_back(DemoNode{child_address, i + 1});
  }

  Dart_Handle connector = newYGConnector(0, children);
  if (Dart_IsError(connector)) {
    ReportDartError("NewYGConnector", connector);
    return;
  }

  const intptr_t root_address = 0x1000;
  Dart_Handle root_layout = Dart_NewTypedData(Dart_TypedData_kFloat32, 9);
  if (Dart_IsError(root_layout)) {
    ReportDartError("Dart_NewTypedData(root)", root_layout);
    return;
  }
  Dart_Handle root = newYGNode(
      root_address, root_layout,
      1, 0, 0, 0, 1, Dart_Null(),
      0.0, 0.0, 0.0, 0.0,
      1, 0, 0,
      Dart_Null(), Dart_Null(), connector);
  if (Dart_IsError(root)) {
    ReportDartError("NewYGNode(root)", root);
    return;
  }
  {
    Dart_TypedData_Type type = Dart_TypedData_kInvalid;
    void* data = nullptr;
    intptr_t length = 0;
    Dart_Handle status =
        Dart_TypedDataAcquireData(root_layout, &type, &data, &length);
    if (!Dart_IsError(status) && type == Dart_TypedData_kFloat32 &&
        length == 9 && data != nullptr) {
      g_yoga_metrics[root_address] = static_cast<float*>(data);
      Dart_TypedDataReleaseData(root_layout);
    } else {
      std::fprintf(stderr, "[skyline-runtime] demo page: bad root yoga buffer\n");
    }
  }
  g_demo_nodes.push_back(DemoNode{root_address, 0});

  static char page_url[] = "pages/index";
  static SkylinePageConfig config;
  config.backgroundColorContent = 0xFFFFFFFFu;
  config.pageUrl = page_url;

  std::fprintf(stderr, "[skyline-runtime] demo page -> CreatePageRoute\n");
  createPageRoute(
      0, 4, 0, 0, 1,
      1, 1, 0, 0,
      0, 0, 0,
      empty,
      1, 1, 0, 0, 0, 0, 0,
      0,
      empty, empty, Dart_Null(), root, &config);
}

void CreateTextPage(RuntimeHolder* holder) {
  auto* newYGNode = reinterpret_cast<NewYGNodeFn>(
      FindCallback(holder->ffi_table, kNewYGNodeHash));
  auto* newYGConnector = reinterpret_cast<NewYGConnectorFn>(
      FindCallback(holder->ffi_table, kNewYGConnectorHash));
  auto* createPageRoute = reinterpret_cast<CreatePageRouteFn>(
      FindCallback(holder->ffi_table, kCreatePageRouteHash));
  auto* newText = reinterpret_cast<NewTextFn>(
      FindCallback(holder->ffi_table, kNewTextHash));
  auto* newTextSpan = reinterpret_cast<NewTextSpanFn>(
      FindCallback(holder->ffi_table, kNewTextSpanHash));
  auto* newDecoratedBox = reinterpret_cast<NewDecoratedBoxFn>(
      FindCallback(holder->ffi_table, kNewDecoratedBoxHash));
  if (newYGNode == nullptr || newYGConnector == nullptr ||
      createPageRoute == nullptr || newText == nullptr ||
      newTextSpan == nullptr ||
      (g_demo_mode == DemoMode::kCard && newDecoratedBox == nullptr)) {
    std::fprintf(stderr, "[skyline-runtime] text page: callbacks not registered\n");
    return;
  }

  g_demo_nodes.clear();
  Dart_Handle empty = Dart_NewStringFromCString("");
  Dart_Handle text_handle = Dart_NewStringFromCString("Skyline on Linux");
  Dart_Handle no_children = Dart_NewList(0);
  if (Dart_IsError(text_handle) || Dart_IsError(no_children)) {
    std::fprintf(stderr, "[skyline-runtime] text page: cannot create strings\n");
    return;
  }

  const intptr_t text_node_address = 0x2000;
  const double nan = std::nan("");
  Dart_Handle span = newTextSpan(
      text_node_address, text_handle, 0xFF000000u, 0,
      0, 0, 0, 0.0,
      nullptr, 0,
      32.0, 3 /* FontWeight.w400 */, 0 /* FontStyle.normal */,
      Dart_Null(), nan, nan, nan,
      0 /* WordBreak.normal */, Dart_Null(), no_children);
  if (Dart_IsError(span)) {
    std::fprintf(stderr, "[skyline-runtime] text page: NewTextSpan failed\n");
    return;
  }
  Dart_Handle spans = Dart_NewList(1);
  Dart_ListSetAt(spans, 0, span);
  Dart_Handle text_widget = newText(
      text_node_address,
      reinterpret_cast<void*>(&DemoMarkDirty),
      reinterpret_cast<void*>(&DemoCreateParagraph),
      0 /* TextAlign.left */, 1 /* TextDirection.ltr */, 0 /* clip */,
      1 /* softWrap */, 0 /* maxLines */, 0 /* userSelect */,
      0 /* selectOnGesture */, 0 /* enableNativeParagraph */, spans);
  if (Dart_IsError(text_widget)) {
    std::fprintf(stderr, "[skyline-runtime] text page: NewText failed\n");
    return;
  }

  Dart_Handle node_child = text_widget;
  if (g_demo_mode == DemoMode::kCard) {
    node_child = newDecoratedBox(
        text_node_address, 0xFFF5F5F5u,
        3.0, 3.0, 3.0, 3.0,
        0xFF3366FFu, 0xFF3366FFu, 0xFF3366FFu, 0xFF3366FFu,
        1, 1, 1, 1,
        16.0, 16.0, 16.0, 16.0, 16.0, 16.0, 16.0, 16.0,
        0, 0, 0, 0, 0, 0, 0, 0,
        Dart_Null(), 0, 0,
        0.0, 0.0, 0.0, 0.0,
        0, 0,
        0.0, 0.0,
        0, 0,
        Dart_Null(), 0,
        Dart_Null(), 0,
        nullptr, nullptr, nullptr, nullptr,
        text_widget);
    if (Dart_IsError(node_child)) {
      std::fprintf(stderr, "[skyline-runtime] card page: NewDecoratedBox failed\n");
      return;
    }
  }

  Dart_Handle text_layout = Dart_NewTypedData(Dart_TypedData_kFloat32, 9);
  Dart_Handle text_node = newYGNode(
      text_node_address, text_layout,
      0, 0, 0, 0, 1, Dart_Null(),
      0.0, 0.0, 0.0, 0.0,
      1, 0, 0,
      Dart_Null(), Dart_Null(), node_child);
  if (Dart_IsError(text_node)) {
    std::fprintf(stderr, "[skyline-runtime] text page: text YGNode failed\n");
    return;
  }
  {
    Dart_TypedData_Type type = Dart_TypedData_kInvalid;
    void* data = nullptr;
    intptr_t length = 0;
    Dart_Handle status =
        Dart_TypedDataAcquireData(text_layout, &type, &data, &length);
    if (!Dart_IsError(status) && type == Dart_TypedData_kFloat32 &&
        length == 9 && data != nullptr) {
      g_yoga_metrics[text_node_address] = static_cast<float*>(data);
      Dart_TypedDataReleaseData(text_layout);
    }
  }
  g_demo_nodes.push_back(DemoNode{text_node_address, 1});

  Dart_Handle children = Dart_NewList(1);
  Dart_ListSetAt(children, 0, text_node);
  Dart_Handle connector = newYGConnector(0, children);
  if (Dart_IsError(connector)) {
    std::fprintf(stderr, "[skyline-runtime] text page: NewYGConnector failed\n");
    return;
  }

  const intptr_t root_address = 0x1000;
  Dart_Handle root_layout = Dart_NewTypedData(Dart_TypedData_kFloat32, 9);
  Dart_Handle root = newYGNode(
      root_address, root_layout,
      1, 0, 0, 0, 1, Dart_Null(),
      0.0, 0.0, 0.0, 0.0,
      1, 0, 0,
      Dart_Null(), Dart_Null(), connector);
  if (Dart_IsError(root)) {
    std::fprintf(stderr, "[skyline-runtime] text page: root YGNode failed\n");
    return;
  }
  {
    Dart_TypedData_Type type = Dart_TypedData_kInvalid;
    void* data = nullptr;
    intptr_t length = 0;
    Dart_Handle status =
        Dart_TypedDataAcquireData(root_layout, &type, &data, &length);
    if (!Dart_IsError(status) && type == Dart_TypedData_kFloat32 &&
        length == 9 && data != nullptr) {
      g_yoga_metrics[root_address] = static_cast<float*>(data);
      Dart_TypedDataReleaseData(root_layout);
    }
  }
  g_demo_nodes.push_back(DemoNode{root_address, 0});

  static char page_url_text[] = "pages/text";
  static char page_url_card[] = "pages/card";
  static SkylinePageConfig config;
  config.backgroundColorContent = 0xFFFFFFFFu;
  config.pageUrl = g_demo_mode == DemoMode::kCard ? page_url_card : page_url_text;

  std::fprintf(stderr, "[skyline-runtime] %s page -> CreatePageRoute\n",
               g_demo_mode == DemoMode::kCard ? "card" : "text");
  createPageRoute(
      0, 4, 0, 0, 1,
      1, 1, 0, 0,
      0, 0, 0,
      empty,
      1, 1, 0, 0, 0, 0, 0,
      0,
      empty, empty, Dart_Null(), root, &config);
}


uint32_t ParseColorString(const char* value, uint32_t fallback) {
  if (value == nullptr || *value == '\0') {
    return fallback;
  }
  const char* p = value;
  if (*p == '#') {
    p++;
  }
  char* end = nullptr;
  const unsigned long parsed = std::strtoul(p, &end, 16);
  if (end == p) {
    return fallback;
  }
  if (std::strlen(p) <= 6) {
    return 0xFF000000u | static_cast<uint32_t>(parsed & 0xFFFFFFu);
  }
  return static_cast<uint32_t>(parsed);
}

// True when the (whitespace-normalized) run consists only of emoji
// codepoints, ZWJ/skin-tone/keycap modifiers and spaces.  The FFI has no
// primary `fontFamily` argument, so emoji-only runs get a marker-prefixed
// color-emoji family that app_compat/ffi-registry.patch turns into TextStyle.fontFamily
// (the SkParagraph FontCollection patch lets that family win over DejaVu's
// monochrome U+2764).  Mixed text keeps the previous default-font behaviour.
bool IsEmojiOnlyText(const char* value) {
  if (value == nullptr || *value == '\0') return false;
  const unsigned char* p = reinterpret_cast<const unsigned char*>(value);
  bool saw_emoji = false;
  while (*p != 0) {
    uint32_t codepoint = *p;
    if (codepoint < 0x80) {
      ++p;
      if (codepoint == ' ' || codepoint == '\t' || codepoint == '\n' ||
          codepoint == '\r') {
        continue;
      }
      return false;
    }
    size_t length = 0;
    if ((codepoint & 0xE0) == 0xC0) {
      length = 2;
      codepoint &= 0x1Fu;
    } else if ((codepoint & 0xF0) == 0xE0) {
      length = 3;
      codepoint &= 0x0Fu;
    } else if ((codepoint & 0xF8) == 0xF0) {
      length = 4;
      codepoint &= 0x07u;
    } else {
      return false;
    }
    for (size_t i = 1; i < length; ++i) {
      if ((p[i] & 0xC0) != 0x80) return false;
      codepoint = (codepoint << 6) | (p[i] & 0x3Fu);
    }
    p += length;
    const bool emoji =
        (codepoint >= 0x00A9 && codepoint <= 0x00AE) ||
        codepoint == 0x203C || codepoint == 0x2049 ||
        codepoint == 0x2122 || codepoint == 0x2139 ||
        (codepoint >= 0x2190 && codepoint <= 0x21FF) ||
        (codepoint >= 0x2300 && codepoint <= 0x23FF) ||
        (codepoint >= 0x25A0 && codepoint <= 0x25FF) ||
        (codepoint >= 0x2600 && codepoint <= 0x27BF) ||
        (codepoint >= 0x2900 && codepoint <= 0x297F) ||
        (codepoint >= 0x2B00 && codepoint <= 0x2BFF) ||
        codepoint == 0x3030 || codepoint == 0x303D ||
        codepoint == 0x3297 || codepoint == 0x3299 ||
        codepoint == 0x200D || codepoint == 0x20E3 ||
        codepoint == 0xFE0E || codepoint == 0xFE0F ||
        (codepoint >= 0x1F000 && codepoint <= 0x1FAFF) ||
        (codepoint >= 0x1F1E6 && codepoint <= 0x1F1FF) ||
        (codepoint >= 0x1F3FB && codepoint <= 0x1F3FF);
    if (!emoji) return false;
    saw_emoji = true;
  }
  return saw_emoji;
}

Dart_Handle CreateTextWidget(NewTextFn newText,
                             NewTextSpanFn newTextSpan,
                             intptr_t address,
                             const char* value,
                             double fontSize,
                             uint32_t color,
                             int fontWeight,
                             int fontStyle,
                             double letterSpacing,
                             double lineHeight,
                             int textAlign,
                             int maxLines,
                             int overflow,
                             bool softWrap,
                             bool userSelect,
                             const std::vector<std::string>& fontFamilies,
                             Dart_Handle* out_span) {
  Dart_Handle text_handle = Dart_NewStringFromCString(value);
  Dart_Handle no_children = Dart_NewList(0);
  if (Dart_IsError(text_handle) || Dart_IsError(no_children)) {
    return Dart_IsError(text_handle) ? text_handle : no_children;
  }
  // Public engines do not apply the private TextStyle flags, but they do honour
  // the public `fontFamilyFallback` list.  The CSS families come first (the
  // first is the preferred family); common CJK/system families follow so
  // Chinese text does not fall back to tofu when the host has them.
  Dart_Handle fallback = Dart_Null();
  {
    static const char* kFallbackFamilies[] = {"Noto Sans CJK SC",
                                              "Noto Sans CJK",
                                              "Noto Sans",
                                              "DejaVu Sans"};
    std::vector<std::string> families;
    // A leading U+0001 marks a primary family (the FFI has no fontFamily
    // argument); app_compat/ffi-registry.patch strips the marker into TextStyle's
    // fontFamily and keeps the rest as the fallback chain.
    if (IsEmojiOnlyText(value)) {
      families.push_back(std::string(1, '\x01') + "Noto Color Emoji");
    }
    for (const std::string& family : fontFamilies) {
      if (family.empty()) continue;
      if (std::find(families.begin(), families.end(), family) ==
          families.end()) {
        families.push_back(family);
      }
    }
    for (const char* family : kFallbackFamilies) {
      if (std::find(families.begin(), families.end(), family) ==
          families.end()) {
        families.push_back(family);
      }
    }
    Dart_Handle list = Dart_NewList(static_cast<intptr_t>(families.size()));
    if (!Dart_IsError(list)) {
      for (size_t i = 0; i < families.size(); i++) {
        Dart_Handle family = Dart_NewStringFromCString(families[i].c_str());
        if (!Dart_IsError(family)) {
          Dart_ListSetAt(list, static_cast<intptr_t>(i), family);
        }
      }
      fallback = list;
    }
  }
  const double nan = std::nan("");
  Dart_Handle span = newTextSpan(
      address, text_handle, color, 0, 0, 0, 0, 0.0, nullptr, 0, fontSize,
      static_cast<uint8_t>(fontWeight), static_cast<uint8_t>(fontStyle),
      fallback, std::isnan(lineHeight) ? nan : lineHeight,
      std::isnan(letterSpacing) ? nan : letterSpacing, nan,
      // Skyline's default WordBreak is breakAll (the official engine breaks
      // long English words at the character that overflows).
      1, Dart_Null(), no_children);
  if (Dart_IsError(span)) {
    return span;
  }
  if (out_span != nullptr) *out_span = span;
  Dart_Handle spans = Dart_NewList(1);
  Dart_ListSetAt(spans, 0, span);
  return newText(address, reinterpret_cast<void*>(&DemoMarkDirty),
                 reinterpret_cast<void*>(&DemoCreateParagraph),
                 static_cast<uint8_t>(textAlign), 1 /* ltr */,
                 static_cast<uint8_t>(overflow), softWrap ? 1 : 0,
                 static_cast<uint32_t>(maxLines), userSelect ? 1 : 0,
                 userSelect ? 1 : 0,
                 0 /* Flutter text */, spans);
}

bool ReadDescriptor(const char* descriptor, std::string* out) {  if (descriptor[0] != '@') {
    *out = descriptor;
    return true;
  }
  std::ifstream file(descriptor + 1);
  if (!file) {
    return false;
  }
  out->assign(std::istreambuf_iterator<char>(file),
              std::istreambuf_iterator<char>());
  return true;
}

float MemberNumber(const rapidjson::Value& object,
                   const char* key,
                   float fallback) {
  const auto member = object.FindMember(key);
  if (member == object.MemberEnd()) return fallback;
  if (member->value.IsBool()) return member->value.GetBool() ? 1.0f : 0.0f;
  if (!member->value.IsNumber()) return fallback;
  return static_cast<float>(member->value.GetDouble());
}

const char* MemberString(const rapidjson::Value& object,
                         const char* key,
                         const char* fallback) {
  const auto member = object.FindMember(key);
  if (member == object.MemberEnd() || !member->value.IsString()) {
    return fallback;
  }
  return member->value.GetString();
}


namespace {

struct BuiltJsonNode {
  JsonNode layout;
  Dart_Handle widget = nullptr;
};

BuiltJsonNode BuildJsonNode(const rapidjson::Value& entry,
                            int depth,
                            intptr_t address,
                            NewYGNodeFn newYGNode,
                            NewYGConnectorFn newYGConnector,
                            NewTextFn newText,
                            NewTextSpanFn newTextSpan,
                            NewDecoratedBoxFn newDecoratedBox,
                            NewOpacityFn newOpacity,
                            NewClipRRectFn newClipRRect,
                            NewLinearGradientFn newLinearGradient,
                            NewBoxShadowFn newBoxShadow,
                            intptr_t* next_address) {
  BuiltJsonNode built;
  built.layout.address = address;
  built.layout.row =
      std::strcmp(MemberString(entry, "direction", "column"), "row") == 0;
  built.layout.padding = MemberNumber(entry, "padding", 0.0f);
  built.layout.gap = MemberNumber(entry, "gap", 0.0f);

  Dart_Handle connector = nullptr;
  const auto children_member = entry.FindMember("children");
  if (children_member != entry.MemberEnd() && children_member->value.IsArray() &&
      children_member->value.Size() > 0) {
    const rapidjson::Value& entries = children_member->value;
    Dart_Handle list = Dart_NewList(static_cast<intptr_t>(entries.Size()));
    if (Dart_IsError(list)) {
      std::fprintf(stderr, "[skyline-runtime] json page: Dart_NewList failed\n");
      return BuiltJsonNode();
    }
    for (rapidjson::SizeType i = 0; i < entries.Size(); i++) {
      const rapidjson::Value& child_entry = entries[i];
      if (!child_entry.IsObject()) {
        std::fprintf(stderr, "[skyline-runtime] json page: child %u is not an object\n",
                     static_cast<unsigned>(i));
        return BuiltJsonNode();
      }
      const intptr_t child_address = *next_address;
      *next_address += 0x100;
      BuiltJsonNode child = BuildJsonNode(
          child_entry, depth + 1, child_address, newYGNode, newYGConnector,
          newText, newTextSpan, newDecoratedBox, newOpacity, newClipRRect,
          newLinearGradient, newBoxShadow, next_address);
      if (child.widget == nullptr) {
        return BuiltJsonNode();
      }
      if (depth == 0) {
        g_json_root_child_records.push_back(
            RootChildRecord{Dart_NewPersistentHandle(child.widget),
                            child.layout.address});
      }
      built.layout.children.push_back(child.layout);
      built.layout.children.back().flex =
          MemberNumber(child_entry, "flex", 0.0f);
      const auto size_member = child_entry.FindMember("size");
      built.layout.children.back().main_size =
          (size_member != child_entry.MemberEnd() &&
           size_member->value.IsNumber())
              ? static_cast<float>(size_member->value.GetDouble())
              : std::nan("");
      if (Dart_IsError(Dart_ListSetAt(list, static_cast<intptr_t>(i), child.widget))) {
        std::fprintf(stderr, "[skyline-runtime] json page: Dart_ListSetAt failed\n");
        return BuiltJsonNode();
      }
    }
    connector = newYGConnector(0, list);
    if (Dart_IsError(connector)) {
      std::fprintf(stderr, "[skyline-runtime] json page: NewYGConnector failed\n");
      return BuiltJsonNode();
    }
    if (depth == 0) {
      g_json_root_connector = Dart_NewPersistentHandle(connector);
    }
  }

  Dart_Handle inner = connector != nullptr ? connector : Dart_Null();
  const char* type = MemberString(entry, "type", "view");
  if (std::strcmp(type, "opacity") == 0) {
    inner = newOpacity(MemberNumber(entry, "opacity", 1.0f), inner);
    if (Dart_IsError(inner)) {
      std::fprintf(stderr, "[skyline-runtime] json page: NewOpacity failed\n");
      return BuiltJsonNode();
    }
    const auto opacity_update = entry.FindMember("updateOpacity");
    if (opacity_update != entry.MemberEnd() &&
        opacity_update->value.IsObject()) {
      g_pending_style_updates.push_back(PendingStyleUpdate{
          Dart_NewPersistentHandle(inner), StyleUpdateKind::kOpacity,
          MemberNumber(opacity_update->value, "value", 0.5f), 0.0, 0u,
          static_cast<int>(
              MemberNumber(opacity_update->value, "afterMs", 300.0f))});
    }
  } else if (std::strcmp(type, "clip") == 0) {
    const double radius = MemberNumber(entry, "radius", 0.0f);
    const uint8_t disable_clip =
        MemberNumber(entry, "disableClip", 0.0f) > 0 ? 1 : 0;
    inner = newClipRRect(
        disable_clip,
        radius, radius, radius, radius, radius, radius, radius, radius,
        0, 0, 0, 0, 0, 0, 0, 0,
        0.0, 0.0, 0.0, 0.0,
        inner);
    if (Dart_IsError(inner)) {
      std::fprintf(stderr, "[skyline-runtime] json page: NewClipRRect failed\n");
      return BuiltJsonNode();
    }
  } else if (std::strcmp(type, "text") == 0) {
    std::vector<std::string> families;
    std::string family = MemberString(entry, "fontFamily", "");
    if (family.empty()) family = MemberString(entry, "font-family", "");
    if (!family.empty()) families.push_back(family);
    inner = CreateTextWidget(
        newText, newTextSpan, address, MemberString(entry, "text", "Text"),
        MemberNumber(entry, "fontSize", 20.0f),
        ParseColorString(MemberString(entry, "color", "#000000"), 0xFF000000u),
        static_cast<int>(MemberNumber(entry, "fontWeight", 3.0f)),
        static_cast<int>(MemberNumber(entry, "fontStyle", 0.0f)),
        MemberNumber(entry, "letterSpacing", std::nan("")),
        MemberNumber(entry, "lineHeight", std::nan("")),
        static_cast<int>(MemberNumber(entry, "textAlign", 0.0f)),
        static_cast<int>(MemberNumber(entry, "maxLines", 0.0f)), 0, true,
        false, families);
    if (Dart_IsError(inner)) {
      std::fprintf(stderr, "[skyline-runtime] json page: text widget failed\n");
      return BuiltJsonNode();
    }
  } else {
    const bool has_visual =
        entry.HasMember("color") || entry.HasMember("borderColor") ||
        entry.HasMember("borderWidth") || entry.HasMember("radius") ||
        entry.HasMember("gradient") || entry.HasMember("shadow");

    Dart_Handle gradient_handle = Dart_Null();
    const auto gradient_member = entry.FindMember("gradient");
    if (gradient_member != entry.MemberEnd() &&
        gradient_member->value.IsObject()) {
      const rapidjson::Value& gradient = gradient_member->value;
      const auto colors_member = gradient.FindMember("colors");
      if (colors_member != gradient.MemberEnd() &&
          colors_member->value.IsArray() &&
          colors_member->value.Size() > 0) {
        const rapidjson::Value& colors = colors_member->value;
        Dart_Handle color_list =
            Dart_NewList(static_cast<intptr_t>(colors.Size()));
        std::vector<GradientStopNative> stops;
        const auto stops_member = gradient.FindMember("stops");
        for (rapidjson::SizeType i = 0; i < colors.Size(); i++) {
          const char* color_string =
              colors[i].IsString() ? colors[i].GetString() : "#000000";
          Dart_ListSetAt(color_list, static_cast<intptr_t>(i),
                         Dart_NewInteger(static_cast<int64_t>(
                             ParseColorString(color_string, 0xFF000000u))));
          double value = colors.Size() > 1
                             ? static_cast<double>(i) / (colors.Size() - 1)
                             : 0.0;
          if (stops_member != gradient.MemberEnd() &&
              stops_member->value.IsArray() &&
              i < stops_member->value.Size() &&
              stops_member->value[i].IsNumber()) {
            value = stops_member->value[i].GetDouble();
          }
          stops.push_back(GradientStopNative{value, 1});
        }
        gradient_handle = newLinearGradient(
            MemberNumber(gradient, "angle", 0.0f), color_list, stops.data(),
            -1.0, -1.0 /* FittingSize.none -> fill the box */, 0, 0, 0, 0,
            0.0, 0.0, 0.0, 0.0, 0);
        if (Dart_IsError(gradient_handle)) {
          std::fprintf(stderr, "[skyline-runtime] json page: NewLinearGradient failed\n");
          return BuiltJsonNode();
        }
      }
    }

    Dart_Handle shadows_handle = Dart_Null();
    const auto shadow_member = entry.FindMember("shadow");
    if (shadow_member != entry.MemberEnd() && shadow_member->value.IsObject()) {
      const rapidjson::Value& shadow = shadow_member->value;
      Dart_Handle box_shadow = newBoxShadow(
          ParseColorString(MemberString(shadow, "color", "#00000080"),
                           0x80000000u),
          MemberNumber(shadow, "offsetX", 0.0f),
          MemberNumber(shadow, "offsetY", 0.0f),
          MemberNumber(shadow, "blur", 0.0f),
          MemberNumber(shadow, "spread", 0.0f),
          MemberNumber(shadow, "inset", 0.0f) > 0 ? 1 : 0);
      if (Dart_IsError(box_shadow)) {
        std::fprintf(stderr, "[skyline-runtime] json page: NewBoxShadow failed\n");
        return BuiltJsonNode();
      }
      Dart_Handle shadow_list = Dart_NewList(1);
      Dart_ListSetAt(shadow_list, 0, box_shadow);
      shadows_handle = shadow_list;
    }

    if (has_visual) {
      const uint32_t background =
          ParseColorString(MemberString(entry, "color", "#00000000"), 0u);
      const uint32_t border =
          ParseColorString(MemberString(entry, "borderColor", ""), 0u);
      const double border_width = MemberNumber(entry, "borderWidth", 0.0f);
      const double radius = MemberNumber(entry, "radius", 0.0f);
      const uint8_t border_style = border_width > 0 ? 1 : 0;
      inner = newDecoratedBox(
          address, background,
          border_width, border_width, border_width, border_width,
          border, border, border, border,
          border_style, border_style, border_style, border_style,
          radius, radius, radius, radius, radius, radius, radius, radius,
          0, 0, 0, 0, 0, 0, 0, 0,
          Dart_Null(), 0, 0,
          0.0, 0.0, 0.0, 0.0,
          0, 0,
          0.0, 0.0,
          0, 0,
          gradient_handle, 0,
          shadows_handle, 0,
          nullptr, nullptr, nullptr, nullptr,
          inner);
      if (Dart_IsError(inner)) {
        std::fprintf(stderr, "[skyline-runtime] json page: DecoratedBox failed\n");
        return BuiltJsonNode();
      }
      const auto update_member = entry.FindMember("updateColor");
      if (update_member != entry.MemberEnd() && update_member->value.IsString()) {
        g_pending_color_updates.push_back(PendingColorUpdate{
            Dart_NewPersistentHandle(inner),
            ParseColorString(update_member->value.GetString(), 0xFFFFFFFFu),
            static_cast<int>(MemberNumber(entry, "updateAfterMs", 300.0f))});
      }
      const auto border_update = entry.FindMember("updateBorder");
      if (border_update != entry.MemberEnd() &&
          border_update->value.IsObject()) {
        g_pending_style_updates.push_back(PendingStyleUpdate{
            Dart_NewPersistentHandle(inner), StyleUpdateKind::kBorder, 0.0,
            MemberNumber(border_update->value, "width", 4.0f),
            ParseColorString(
                MemberString(border_update->value, "color", "#FFFF00"),
                0xFFFFFF00u),
            static_cast<int>(
                MemberNumber(border_update->value, "afterMs", 300.0f))});
      }
    }
  }

  Dart_Handle buffer = Dart_NewTypedData(Dart_TypedData_kFloat32, 9);
  if (Dart_IsError(buffer)) {
    std::fprintf(stderr, "[skyline-runtime] json page: typed data failed\n");
    return BuiltJsonNode();
  }
  Dart_Handle node = newYGNode(
      address, buffer, depth == 0 ? 1 : 0, 0, 0, 0, 1, Dart_Null(), 0.0, 0.0,
      0.0, 0.0, 1, 0, 0, Dart_Null(), Dart_Null(), inner);
  if (Dart_IsError(node)) {
    std::fprintf(stderr, "[skyline-runtime] json page: NewYGNode failed\n");
    return BuiltJsonNode();
  }

  Dart_TypedData_Type data_type = Dart_TypedData_kInvalid;
  void* data = nullptr;
  intptr_t length = 0;
  Dart_Handle status =
      Dart_TypedDataAcquireData(buffer, &data_type, &data, &length);
  if (!Dart_IsError(status) && data_type == Dart_TypedData_kFloat32 &&
      length == 9 && data != nullptr) {
    g_yoga_metrics[address] = static_cast<float*>(data);
    Dart_TypedDataReleaseData(buffer);
  }
  built.widget = node;
  return built;
}

void LayoutJsonNode(const JsonNode& node,
                    float left,
                    float top,
                    float width,
                    float height) {
  const auto found = g_yoga_metrics.find(node.address);
  if (found != g_yoga_metrics.end() && found->second != nullptr) {
    float* m = found->second;
    m[0] = top;
    m[1] = left;
    m[2] = width;
    m[3] = height;
    for (int k = 4; k < 9; k++) m[k] = 0.0f;
  }
  if (node.children.empty()) {
    return;
  }
  // Child offsets are stored relative to this node (the YGNode render object
  // writes boxParentData.offset from the buffer), not relative to the viewport.
  const float content_left = node.padding;
  const float content_top = node.padding;
  const float content_width =
      width - 2 * node.padding > 0 ? width - 2 * node.padding : 0.0f;
  const float content_height =
      height - 2 * node.padding > 0 ? height - 2 * node.padding : 0.0f;
  const size_t count = node.children.size();
  const float main_extent =
      (node.row ? content_width : content_height) -
      (count > 1 ? node.gap * static_cast<float>(count - 1) : 0.0f);
  float fixed_sum = 0.0f;
  float flex_sum = 0.0f;
  for (const JsonNode& child : node.children) {
    if (std::isnan(child.main_size)) {
      flex_sum += child.flex;
    } else {
      fixed_sum += child.main_size;
    }
  }
  float remaining = main_extent - fixed_sum;
  if (remaining < 0.0f) remaining = 0.0f;
  float cursor = node.row ? content_left : content_top;
  for (const JsonNode& child : node.children) {
    const float main = std::isnan(child.main_size)
                           ? (flex_sum > 0.0f ? remaining * child.flex / flex_sum : 0.0f)
                           : child.main_size;
    if (node.row) {
      LayoutJsonNode(child, cursor, content_top, main, content_height);
    } else {
      LayoutJsonNode(child, content_left, cursor, content_width, main);
    }
    cursor += main + node.gap;
  }
}

}  // namespace

void CreateJsonPage(RuntimeHolder* holder, const char* descriptor) {
  auto* newYGNode = reinterpret_cast<NewYGNodeFn>(
      FindCallback(holder->ffi_table, kNewYGNodeHash));
  auto* newYGConnector = reinterpret_cast<NewYGConnectorFn>(
      FindCallback(holder->ffi_table, kNewYGConnectorHash));
  auto* createPageRoute = reinterpret_cast<CreatePageRouteFn>(
      FindCallback(holder->ffi_table, kCreatePageRouteHash));
  auto* newText = reinterpret_cast<NewTextFn>(
      FindCallback(holder->ffi_table, kNewTextHash));
  auto* newTextSpan = reinterpret_cast<NewTextSpanFn>(
      FindCallback(holder->ffi_table, kNewTextSpanHash));
  auto* newDecoratedBox = reinterpret_cast<NewDecoratedBoxFn>(
      FindCallback(holder->ffi_table, kNewDecoratedBoxHash));
  auto* newOpacity = reinterpret_cast<NewOpacityFn>(
      FindCallback(holder->ffi_table, kNewOpacityHash));
  auto* newClipRRect = reinterpret_cast<NewClipRRectFn>(
      FindCallback(holder->ffi_table, kNewClipRRectHash));
  auto* newLinearGradient = reinterpret_cast<NewLinearGradientFn>(
      FindCallback(holder->ffi_table, kNewLinearGradientHash));
  auto* newBoxShadow = reinterpret_cast<NewBoxShadowFn>(
      FindCallback(holder->ffi_table, kNewBoxShadowHash));
  auto* updateColor = reinterpret_cast<DecoratedBoxUpdateColorFn>(
      FindCallback(holder->ffi_table, kDecoratedBoxUpdateColorHash));
  g_update_opacity = reinterpret_cast<OpacityUpdateFn>(
      FindCallback(holder->ffi_table, kOpacityUpdateHash));
  g_update_border = reinterpret_cast<DecoratedBoxUpdateBorderFn>(
      FindCallback(holder->ffi_table, kDecoratedBoxUpdateBorderHash));
  auto* updateChildren = reinterpret_cast<ConnectorUpdateChildrenFn>(
      FindCallback(holder->ffi_table, kYGConnectorUpdateChildrenHash));
  auto* markNeedsLayout = reinterpret_cast<MarkYGNodeNeedsLayoutFn>(
      FindCallback(holder->ffi_table, kMarkYGNodeNeedsLayoutHash));
  if (newYGNode == nullptr || newYGConnector == nullptr ||
      createPageRoute == nullptr || newText == nullptr ||
      newTextSpan == nullptr || newDecoratedBox == nullptr ||
      newOpacity == nullptr || newClipRRect == nullptr ||
      newLinearGradient == nullptr || newBoxShadow == nullptr) {
    std::fprintf(stderr, "[skyline-runtime] json page: callbacks not registered\n");
    return;
  }

  g_pending_color_updates.clear();
  g_pending_style_updates.clear();
  g_json_root_connector = nullptr;
  g_json_root_child_records.clear();

  std::string json;
  if (!ReadDescriptor(descriptor, &json)) {
    std::fprintf(stderr, "[skyline-runtime] json page: cannot read %s\n", descriptor);
    return;
  }
  rapidjson::Document doc;
  doc.Parse(json.c_str());
  if (doc.HasParseError() || !doc.IsObject()) {
    std::fprintf(stderr, "[skyline-runtime] json page: parse error at %zu\n",
                 static_cast<size_t>(doc.GetErrorOffset()));
    return;
  }

  intptr_t next_address = 0x1100;
  BuiltJsonNode root = BuildJsonNode(doc, 0, 0x1000, newYGNode, newYGConnector,
                                     newText, newTextSpan, newDecoratedBox,
                                     newOpacity, newClipRRect, newLinearGradient,
                                     newBoxShadow, &next_address);
  if (root.widget == nullptr) {
    std::fprintf(stderr, "[skyline-runtime] json page: build failed\n");
    return;
  }
  g_json_root = root.layout;
  g_json_next_address = next_address;

  static char page_url[] = "pages/json";
  static SkylinePageConfig config;
  config.backgroundColorContent = 0xFFFFFFFFu;
  config.pageUrl = page_url;
  Dart_Handle empty = Dart_NewStringFromCString("");

  std::fprintf(stderr, "[skyline-runtime] json page -> CreatePageRoute\n");
  createPageRoute(0, 4, 0, 0, 1, 1, 1, 0, 0, 0, 0, 0, empty, 1, 1, 0, 0, 0, 0,
                  0, 0, empty, empty, Dart_Null(), root.widget, &config);
  SchedulePendingStyleUpdates();

  if (updateColor != nullptr && !g_pending_color_updates.empty()) {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    auto task_runner = fml::MessageLoop::GetCurrent().GetTaskRunner();
#pragma GCC diagnostic pop
    if (task_runner) {
      for (const PendingColorUpdate& update : g_pending_color_updates) {
        std::fprintf(stderr,
                     "[skyline-runtime] json page: color update in %d ms\n",
                     update.delay_ms);
        task_runner->PostDelayedTask(
            [updateColor, update]() {
              if (Dart_CurrentIsolate() == nullptr && g_isolate == nullptr) {
                return;
              }
              DemoPageApiScope scope;
              Dart_Handle decorated =
                  Dart_HandleFromPersistent(update.decorated_box);
              updateColor(decorated, update.color);
              Dart_DeletePersistentHandle(update.decorated_box);
            },
            fml::TimeDelta::FromMilliseconds(update.delay_ms));
      }
    }
  }

  const auto ops_member = doc.FindMember("ops");
  if (ops_member != doc.MemberEnd() && ops_member->value.IsArray()) {
    auto task_runner = UiTaskRunner();
    if (task_runner) {
      const rapidjson::Value& ops = ops_member->value;
      for (rapidjson::SizeType i = 0; i < ops.Size(); i++) {
        const rapidjson::Value& op = ops[i];
        if (!op.IsObject()) {
          continue;
        }
        const char* kind = MemberString(op, "op", "");
        const int delay_ms =
            static_cast<int>(MemberNumber(op, "afterMs", 300.0f));
        if (std::strcmp(kind, "append") == 0 && updateChildren != nullptr) {
          const auto child_member = op.FindMember("child");
          if (child_member == op.MemberEnd() || !child_member->value.IsObject()) {
            continue;
          }
          rapidjson::StringBuffer buffer;
          rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
          child_member->value.Accept(writer);
          const std::string child_json(buffer.GetString(), buffer.GetLength());
          task_runner->PostDelayedTask(
              [child_json, updateChildren, newYGNode, newYGConnector, newText,
               newTextSpan, newDecoratedBox, newOpacity, newClipRRect,
               newLinearGradient, newBoxShadow, markNeedsLayout]() {
                if (g_json_root_connector == nullptr ||
                    (Dart_CurrentIsolate() == nullptr && g_isolate == nullptr)) {
                  return;
                }
                DemoPageApiScope scope;
                rapidjson::Document doc;
                doc.Parse(child_json.c_str());
                if (doc.HasParseError() || !doc.IsObject()) {
                  std::fprintf(stderr,
                               "[skyline-runtime] append op: bad child json\n");
                  return;
                }
                const intptr_t address = g_json_next_address;
                g_json_next_address += 0x100;
                BuiltJsonNode child = BuildJsonNode(
                    doc, 1, address, newYGNode, newYGConnector, newText,
                    newTextSpan, newDecoratedBox, newOpacity, newClipRRect,
                    newLinearGradient, newBoxShadow, &g_json_next_address);
                if (child.widget == nullptr) {
                  return;
                }
                child.layout.flex =
                    static_cast<float>(MemberNumber(doc, "flex", 0.0f));
                const auto size_member = doc.FindMember("size");
                child.layout.main_size =
                    (size_member != doc.MemberEnd() &&
                     size_member->value.IsNumber())
                        ? static_cast<float>(size_member->value.GetDouble())
                        : std::nan("");
                SchedulePendingStyleUpdates();
                Dart_PersistentHandle persistent =
                    Dart_NewPersistentHandle(child.widget);
                g_json_root.children.push_back(child.layout);
                g_json_root_child_records.push_back(
                    RootChildRecord{persistent, child.layout.address});
                Dart_Handle list = Dart_NewList(
                    static_cast<intptr_t>(g_json_root_child_records.size()));
                for (intptr_t i = 0;
                     i < static_cast<intptr_t>(g_json_root_child_records.size());
                     i++) {
                  Dart_ListSetAt(
                      list, i,
                      Dart_HandleFromPersistent(
                          g_json_root_child_records[i].handle));
                }
                Dart_Handle connector =
                    Dart_HandleFromPersistent(g_json_root_connector);
                updateChildren(connector, 0, list);
                if (markNeedsLayout != nullptr) {
                  for (const RootChildRecord& item :
                       g_json_root_child_records) {
                    markNeedsLayout(Dart_HandleFromPersistent(item.handle));
                  }
                }
              },
              fml::TimeDelta::FromMilliseconds(delay_ms));
          std::fprintf(stderr,
                       "[skyline-runtime] json page: append child in %d ms\n",
                       delay_ms);
        } else if (std::strcmp(kind, "remove") == 0 &&
                   updateChildren != nullptr) {
          const int index =
              static_cast<int>(MemberNumber(op, "index", 0.0f));
          if (index < 0 ||
              index >= static_cast<int>(g_json_root_child_records.size())) {
            continue;
          }
          const intptr_t address = g_json_root_child_records[index].address;
          task_runner->PostDelayedTask(
              [updateChildren, markNeedsLayout, address]() {
                if (g_json_root_connector == nullptr ||
                    (Dart_CurrentIsolate() == nullptr && g_isolate == nullptr)) {
                  return;
                }
                DemoPageApiScope scope;
                const auto record = std::find_if(
                    g_json_root_child_records.begin(),
                    g_json_root_child_records.end(),
                    [address](const RootChildRecord& item) {
                      return item.address == address;
                    });
                if (record == g_json_root_child_records.end()) {
                  return;
                }
                const size_t remaining =
                    g_json_root_child_records.size() - 1;
                Dart_Handle list =
                    Dart_NewList(static_cast<intptr_t>(remaining));
                intptr_t index = 0;
                for (const RootChildRecord& item : g_json_root_child_records) {
                  if (item.address == address) {
                    continue;
                  }
                  Dart_ListSetAt(list, index++,
                                 Dart_HandleFromPersistent(item.handle));
                }
                Dart_PersistentHandle persistent = record->handle;
                g_json_root.children.erase(
                    std::remove_if(
                        g_json_root.children.begin(), g_json_root.children.end(),
                        [address](const JsonNode& node) {
                          return node.address == address;
                        }),
                    g_json_root.children.end());
                g_json_root_child_records.erase(record);
                Dart_DeletePersistentHandle(persistent);
                Dart_Handle connector =
                    Dart_HandleFromPersistent(g_json_root_connector);
                updateChildren(connector, 0, list);
                if (markNeedsLayout != nullptr) {
                  for (const RootChildRecord& item :
                       g_json_root_child_records) {
                    markNeedsLayout(Dart_HandleFromPersistent(item.handle));
                  }
                }
              },
              fml::TimeDelta::FromMilliseconds(delay_ms));
          std::fprintf(stderr,
                       "[skyline-runtime] json page: remove child in %d ms\n",
                       delay_ms);
        }
      }
    }
  }
}

SKYLINE_EXPORT extern "C" void YogaCalculateLayout(intptr_t /*nodeAddress*/,
                                                     double maxWidth,
                                                     double maxHeight) {
#ifdef SKYLINE_EMBEDDED
  if (g_json_active) {
    LayoutJsonNode(g_json_root, 0.0f, 0.0f, static_cast<float>(maxWidth),
                   static_cast<float>(maxHeight));
    return;
  }
  if (g_demo_nodes.empty()) {
    return;
  }
  const size_t child_count = g_demo_nodes.size() - 1;
  const float stripe_height =
      child_count == 0 ? static_cast<float>(maxHeight)
                       : static_cast<float>(maxHeight) / child_count;
  const float inset = 20.0f;
  const float text_width =
      static_cast<float>(maxWidth) - 2 * inset > 0 ? static_cast<float>(maxWidth) - 2 * inset : 0.0f;
  const float text_height =
      static_cast<float>(maxHeight) - 2 * inset > 0 ? static_cast<float>(maxHeight) - 2 * inset : 0.0f;
  for (const DemoNode& node : g_demo_nodes) {
    const auto found = g_yoga_metrics.find(node.address);
    if (found == g_yoga_metrics.end() || found->second == nullptr) {
      continue;
    }
    float* metrics = found->second;
    // [top, left, width, height, zIndex, isLayoutDirty, displayNone,
    //  marginBlock, marginInline] -- see package:skyline/widgets/yoga.dart.
    if (node.index == 0) {
      metrics[0] = 0.0f;
      metrics[1] = 0.0f;
      metrics[2] = static_cast<float>(maxWidth);
      metrics[3] = static_cast<float>(maxHeight);
    } else if (g_demo_mode == DemoMode::kText) {
      metrics[0] = inset;
      metrics[1] = inset;
      metrics[2] = text_width;
      metrics[3] = text_height;
    } else if (g_demo_mode == DemoMode::kCard) {
      constexpr float card_inset = 40.0f;
      const float card_width =
          static_cast<float>(maxWidth) - 2 * card_inset > 0
              ? static_cast<float>(maxWidth) - 2 * card_inset
              : 0.0f;
      const float card_height =
          static_cast<float>(maxHeight) - 2 * card_inset > 0
              ? static_cast<float>(maxHeight) - 2 * card_inset
              : 0.0f;
      metrics[0] = card_inset;
      metrics[1] = card_inset;
      metrics[2] = card_width;
      metrics[3] = card_height;
    } else {
      metrics[0] = static_cast<float>(node.index - 1) * stripe_height;
      metrics[1] = 0.0f;
      metrics[2] = static_cast<float>(maxWidth);
      metrics[3] = stripe_height;
    }
    metrics[4] = 0.0f;
    metrics[5] = 0.0f;
    metrics[6] = 0.0f;
    metrics[7] = 0.0f;
    metrics[8] = 0.0f;
  }
#endif
}
