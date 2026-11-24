#pragma once
// Replay data model shared by the appservice modules.
#include "skyline_runtime_abi.hh"
#include "skyline_runtime_ffi.hh"

// Page-route configuration passed to CreatePageRoute.  Used by the synthetic
// demo pages (global scope) and the appservice route materializer.
struct SkylinePageConfig {
  uint32_t backgroundColorContent;
  void* pageUrl;
};

// Live DevTools bridge state (defined in appservice_model.cc).
extern std::mutex g_live_ops_mutex;
extern std::string g_pending_live_ops;
extern bool g_live_ready;
extern fml::RefPtr<fml::TaskRunner> g_live_ui_runner;

namespace appservice {

inline constexpr float kInf = 1e30f;

// CSS transition properties modelled by the replay runtime.  The bit mask is
// stored on AppStyle so a style change can start the matching animations.
enum TransitionProperty : uint32_t {
  kTransitionOpacity = 1u << 0,
  kTransitionBackground = 1u << 1,
  kTransitionColor = 1u << 2,
  kTransitionBorderColor = 1u << 3,
  kTransitionTransform = 1u << 4,
};

// CSS gradient function parsed from `background-image` / `background`.
enum CssGradientKind {
  kGradientNone = 0,
  kGradientLinear = 1,
  kGradientRadial = 2,
  kGradientConic = 3,
};

// One axis of a CSS <position> value (`background-position`, `object-position`).
// `value` is a percentage in 0..100 or a px offset; `from_end` implements the
// `right 10px` / `bottom 10px` form.
struct CssPositionAxis {
  float value = 0.0f;
  bool percent = true;
  bool from_end = false;
};

struct AppStyle {
  bool row = false;
  int justify = 0;  // 0 start, 1 center, 2 end, 3 space-between
  int align = 0;    // 0 stretch, 1 start, 2 center, 3 end
  int align_self = -1;  // -1 auto, otherwise same values as `align`
  bool wrap = false;
  int align_content = 1;  // 0 stretch, 1 start, 2 center, 3 end
  int order = 0;
  float padding[4] = {0, 0, 0, 0};  // top, right, bottom, left
  float margin[4] = {0, 0, 0, 0};
  // CSS percentage padding/margin, resolved against the containing block width
  // during measurement.  NaN = not a percentage.
  float padding_percent[4] = {std::nanf(""), std::nanf(""), std::nanf(""),
                              std::nanf("")};
  float margin_percent[4] = {std::nanf(""), std::nanf(""), std::nanf(""),
                             std::nanf("")};
  float gap = 0.0f;
  float row_gap = 0.0f;
  float column_gap = 0.0f;

  // CSS position/insets.  position == 1 absolute, 2 fixed: the node is removed
  // from flex flow; absolute/fixed insets resolve against the parent content box
  // or the viewport respectively.
  int position = 0;
  float inset[4] = {std::nanf(""), std::nanf(""), std::nanf(""),
                    std::nanf("")};  // top, right, bottom, left (px)
  float inset_percent[4] = {std::nanf(""), std::nanf(""), std::nanf(""),
                            std::nanf("")};
  int z_index = 0;
  // CSS transform (column-major 4x4, applied around the element centre).
  bool has_transform = false;
  float transform[16] = {1, 0, 0, 0, 0, 1, 0, 0,
                         0, 0, 1, 0, 0, 0, 0, 1};
  // Percentage translations (`translateY(-50%)`) need the measured border box,
  // so they are kept aside and folded into the matrix when it is built.
  float translate_percent_x = std::nanf("");
  float translate_percent_y = std::nanf("");
  // CSS transform-origin (px or percent; NaN = unset, default 50% 50%).
  float transform_origin_x = std::nanf("");
  float transform_origin_y = std::nanf("");
  float transform_origin_x_percent = std::nanf("");
  float transform_origin_y_percent = std::nanf("");
  // Third transform-origin component (CSS length, default 0px).  Folded into
  // the matrix as T(0,0,z) * M * T(0,0,-z) because the YGNode widget only
  // carries the 2D origin.
  float transform_origin_z = 0.0f;
  // CSS transitions.  `transition_mask` selects the animated properties.
  uint32_t transition_mask = 0;
  double transition_duration = 0.0;  // ms
  double transition_delay = 0.0;     // ms
  int transition_easing = 0;         // 0 linear, 1 ease, 2 ease-in, 3 ease-out
                                     // 4 ease-in-out
  // First CSS box-shadow layer.
  bool has_shadow = false;
  uint32_t shadow_color = 0u;
  float shadow_offset_x = 0.0f;
  float shadow_offset_y = 0.0f;
  float shadow_blur = 0.0f;
  float shadow_spread = 0.0f;
  bool shadow_inset = false;

  // CSS background image (url(...)); painted behind the node's children.
  std::string background_image;
  int background_repeat = 0;  // 0 no-repeat, 1 repeat, 2 repeat-x, 3 repeat-y
  int background_fit = 0;     // 0 fill, 1 contain, 2 cover
  // CSS background-size explicit values (`cover`/`contain` stay in
  // `background_fit`).  -1 = auto; percentages are 0..1.
  bool has_background_size = false;
  float background_size_width = -1.0f;
  float background_size_height = -1.0f;
  bool background_size_width_percent = false;
  bool background_size_height_percent = false;
  // CSS background-position (default 0% 0%).
  CssPositionAxis background_position_x;
  CssPositionAxis background_position_y;
  // First CSS gradient layer.  `gradient_kind` decides which of the parameter
  // groups below is meaningful; `gradient_angle` is the linear direction or the
  // conic start angle (radians).
  bool has_gradient = false;
  int gradient_kind = kGradientNone;
  double gradient_angle = 0.0;
  double gradient_center_x = 50.0;  // 0..100 percent, or px
  double gradient_center_y = 50.0;
  bool gradient_center_x_percent = true;
  bool gradient_center_y_percent = true;
  float gradient_radius = -1.0f;    // radial explicit radius (px)
  int gradient_radial_size = 0;     // RadialGradientSize enum
  std::vector<uint32_t> gradient_colors;
  std::vector<double> gradient_stops;
  // 1 = stop value is already a fraction (percentage); 0 = px length.
  std::vector<uint8_t> gradient_stop_percent;

  float width = -1.0f;          // px, -1 = auto
  float height = -1.0f;
  float width_percent = -1.0f;  // 0..1
  float height_percent = -1.0f;
  // `width`/`height` were declared by CSS (including `auto`).  Builtin default
  // sizes (e.g. `swiper{height:150px}`) must not override an explicit `auto`.
  bool width_declared = false;
  bool height_declared = false;
  // CSS aspect-ratio (width / height); -1 = none.  The automatic axis is
  // derived from the definite one during layout.
  float aspect_ratio = -1.0f;
  // 0 = border-box (Skyline's effective default), 1 = content-box.
  int box_sizing = 0;
  float min_width = 0.0f;
  float max_width = kInf;
  float min_height = 0.0f;
  float max_height = kInf;
  float flex = 0.0f;
  bool has_flex = false;
  // CSS flex-shrink (default 1) and flex-basis.  `has_flex_shrink` records an
  // explicit declaration so the replay's historical non-shrinking layout stays
  // stable for pages that never ask for it.
  float flex_shrink = 1.0f;
  bool has_flex_shrink = false;
  float flex_basis = std::nanf("");          // px; NaN = auto
  float flex_basis_percent = std::nanf("");  // 0..1; NaN = not a percentage

  uint32_t background = 0u;
  bool has_background = false;
  uint32_t border_color = 0u;
  float border_width = 0.0f;
  // Per-side overrides (top, right, bottom, left); -1 = use the uniform value.
  float side_border_width[4] = {-1.0f, -1.0f, -1.0f, -1.0f};
  uint32_t side_border_color[4] = {0u, 0u, 0u, 0u};
  bool side_border_color_set[4] = {false, false, false, false};
  float radius = 0.0f;
  // Per-corner radii (TL, TR, BR, BL); -1 = use the uniform `radius`.
  float corner_radius[4] = {-1.0f, -1.0f, -1.0f, -1.0f};
  bool corner_radius_percent[4] = {false, false, false, false};
  bool corner_radius_ellipse[4] = {false, false, false, false};
  double opacity = 1.0;

  double font_size = 16.0;
  uint32_t color = 0xFF000000u;
  int font_weight = 3;  // FontWeight.w400
  int font_style = 0;   // normal
  // CSS `font-family` list (first entry is the preferred family).  The public
  // NewTextSpan surface has no primary family parameter, so these are placed at
  // the front of `fontFamilyFallback`.
  std::vector<std::string> font_families;
  double line_height = -1.0;         // absolute px, -1 = use factor
  double line_height_factor = 1.2;   // multiplier
  double letter_spacing = std::nan("");
  int text_align = 0;
  uint32_t max_lines = 0;
  // CSS white-space: 0 normal, 1 nowrap, 2 pre, 3 pre-wrap, 4 pre-line.
  int white_space = 0;
  // Flutter TextOverflow index (0 clip, 1 fade, 2 ellipsis, 3 visible).
  // Skyline does not clip glyph ink to the text box: the native runtime sizes
  // the node from the real paragraph, and text that overflows the declared box
  // (e.g. a fixed height) paints outside it, like CSS.  Default to `visible`
  // so an approximate replay measurement never cuts descenders or emoji.
  int text_overflow = 3;
  bool display_none = false;
  // True when a rule or inline style declared `display`.  Skyline's native
  // text/span tags default to inline, so this distinguishes an author override
  // (`display:block`) from the built-in default.
  bool has_display = false;
  // CSS display: inline-block / inline participates in the parent's inline
  // formatting context instead of stacking as a block.
  bool inline_block = false;
  bool inline_level = false;
  // CSS pointer-events: none.  Passed to NewYGNode as PointerEvents.none; the
  // YGNode hit test then skips this node and its subtree.
  bool pointer_events_none = false;
  bool clip = false;  // overflow: hidden/clip
  // Image fit: 0 fill, 1 contain, 2 cover, 3 none, 4 scale-down.
  int object_fit = 0;
  // CSS object-position (default 50% 50%).
  CssPositionAxis object_position_x = {50.0f, true, false};
  CssPositionAxis object_position_y = {50.0f, true, false};
  bool has_object_position = false;
};
struct AppNode {
  uint32_t id = 0;
  std::string tag;
  std::string name;
  std::string text;
  std::vector<std::string> classes;
  std::string inline_style;
  std::unordered_map<std::string, std::string> attrs;
  std::vector<AppNode*> children;
  AppNode* parent = nullptr;
  bool released = false;
  // CSS inline-flow whitespace collapsing: a raw text run between two inline
  // boxes keeps one collapsed space on the side that has adjacent content.
  // Set by ComputeInlineWhitespace after ResolveStyles.
  bool inline_preserve_leading = false;
  bool inline_preserve_trailing = false;
  // True when this node is a sliver-hosted root (sticky-section child, scroll
  // view child, or list/grid item).  Its layout buffer is origin-relative and
  // must be restored after any page-wide LayoutNode pass.
  bool sliver_layout = false;

  AppStyle style;
  float measured_w = 0.0f;
  float measured_h = 0.0f;
  // Absolute border-box origin relative to the viewport (set by LayoutNode).
  float abs_x = 0.0f;
  float abs_y = 0.0f;
  intptr_t address = 0;
  Dart_Handle widget = nullptr;
  Dart_Handle layout_buffer = nullptr;
  // Widget handles produced during the current materialization (local scope).
  Dart_Handle connector = nullptr;
  Dart_Handle decorated_box = nullptr;
  Dart_Handle text_widget = nullptr;
  Dart_Handle texture_widget = nullptr;
  Dart_Handle opacity_widget = nullptr;
  Dart_Handle clip_widget = nullptr;

  // Native view (`tag == "texture"`) identity and pointer-event routing.
  int texture_view_id = -1;
  std::string texture_type;

  // Persistent handles keep the attached tree addressable across update tasks.
  Dart_PersistentHandle layout_buffer_p = nullptr;
  Dart_PersistentHandle decorated_box_p = nullptr;
  Dart_PersistentHandle opacity_widget_p = nullptr;
  Dart_PersistentHandle clip_widget_p = nullptr;
  Dart_PersistentHandle text_widget_p = nullptr;
  Dart_PersistentHandle text_span_p = nullptr;
  Dart_PersistentHandle texture_widget_p = nullptr;
  Dart_PersistentHandle yoga_node_p = nullptr;
  Dart_PersistentHandle scroll_view_p = nullptr;
  Dart_PersistentHandle swiper_p = nullptr;
  Dart_PersistentHandle picker_column_p = nullptr;
  Dart_PersistentHandle selection_p = nullptr;
  // D10: accessibility wrapper (`<view aria-label=...>`) and snapshot widgets.
  Dart_Handle semantics_widget = nullptr;
  Dart_PersistentHandle semantics_p = nullptr;
  Dart_Handle snapshot_widget = nullptr;
  Dart_PersistentHandle snapshot_p = nullptr;
  // `<share-element>` Hero widget (SkylineHero).  The frame callback drives
  // the `onFrame-<viewTag>` worklet while a route transition animates.
  Dart_Handle hero_widget = nullptr;
  Dart_PersistentHandle hero_p = nullptr;
  // Float64List(16) backing YGNodeSetupTransform while the Matrix4 reads it.
  Dart_PersistentHandle transform_p = nullptr;
  // Decoded image backing a `<image>` node's direct-render paint callback.
  sk_sp<flutter::DlImage> image_dl;
  // Direct-render widget currently materialized for this node.  Kept so a
  // late image response can mark the *existing* widget dirty; a page rebuild
  // only refreshes the current route, so covered pages would keep their white
  // placeholders (visible while a non-opaque route transitions).
  Dart_PersistentHandle image_widget_p = nullptr;
  // Decoded CSS background-image (painted by the decorated box).
  sk_sp<flutter::DlImage> background_image_dl;
  std::string image_source;
  uint32_t image_request_id = 0;
  bool image_target_background = false;
  bool image_loaded = false;
  bool image_failed = false;
  bool image_pending = false;

  // Synthetic input state for `name == "input"` nodes.  Indices are UTF-8
  // codepoint offsets (the replay text model, not Dart UTF-16 offsets).
  int input_cursor = 0;
  int input_selection_start = 0;
  int input_selection_end = 0;
  bool input_hold_keyboard = false;
  std::string composition_text;
  int composition_start = 0;
  int composition_end = 0;
  bool composing = false;
  std::string composition_base_text;
  Dart_Handle input_widget = nullptr;
  Dart_PersistentHandle input_widget_p = nullptr;

  // CSS transition override.  When active, ResolveStyles keeps the animated
  // style instead of the cascade target until the transition completes.
  bool animation_override_active = false;
  AppStyle animation_override;

  // List/grid builder state (NewListBuilder/NewGridBuilder).  Items are
  // materialized on demand from the captured node table when Flutter's sliver
  // asks for an index via the onBuildChild callback.
  Dart_PersistentHandle builder_p = nullptr;
  uint8_t builder_kind = 0;  // 0 = list, 1 = grid
  int builder_child_count = 0;
  int builder_cross_axis_count = 1;
  float builder_child_height = 0.0f;
  float builder_main_spacing = 0.0f;
  float builder_cross_spacing = 0.0f;
  float builder_max_cross_extent = 0.0f;
  uint32_t builder_background = 0u;
  // index -> materialized SkylineListBuilderItem/SkylineGridBuilderItem.
  std::unordered_map<int, Dart_PersistentHandle> builder_items;
  std::unordered_map<int, AppNode*> builder_item_nodes;
  std::unordered_set<int> builder_pending;
  // Last synced metadata and content signature, used for fine-grained updates.
  int builder_synced_child_count = 0;
  float builder_synced_child_height = 0.0f;
  int builder_synced_cross_axis_count = 1;
  float builder_synced_main_spacing = 0.0f;
  float builder_synced_cross_spacing = 0.0f;
  float builder_synced_max_cross_extent = 0.0f;
  uint32_t builder_synced_background = 0u;
  std::unordered_map<int, uint64_t> builder_item_signatures;


  AppStyle previous_style;
  bool has_previous_style = false;
  std::string previous_text;
};
// Parsed once per rule when the stylesheet is loaded; `ResolveStyles` runs for
// every node, so re-parsing here was the dominant cost of a full restyle.
struct Selector;

struct CssRule {
  std::string selector;
  std::shared_ptr<Selector> parsed_selector;
  int specificity = 0;
  std::vector<std::pair<std::string, std::string>> declarations;
  int order = 0;
};

struct FontFaceRequest {
  std::string family;
  std::string url;
};

// One sampled frame of a JS `addAnimatedStyle` worklet.  The recorder stores
// declarations (CSS syntax) so the replay runtime can apply them without a V8.
struct PendingAnimatedStyle {
  uint32_t node_id = 0;
  int at_ms = 0;
  std::string declarations;
};
struct OpPage {
  std::unordered_map<uint32_t, AppNode*> nodes;
  std::vector<CssRule> rules;
  std::vector<FontFaceRequest> fonts;
  std::vector<PendingAnimatedStyle> animated_styles;
  size_t scheduled_animated_styles = 0;
  uint32_t root_id = 0;
  int applied_ops = 0;
  bool started = false;
  bool force_rebuild = false;

  // Route stack replay: `notifyNavigateTo`/`notifySwitchTab` record the open
  // type used by the next `attach`; each attach materializes one page route.
  uint8_t pending_open_type = 0;
  int32_t pending_page_id = 0;
  int32_t pending_callback_id = 0;
  // Root id that was active before the current `attach` op replaced root_id.
  uint32_t attach_previous_root_id = 0;
  // Custom routes attach a `host` wrapper that contains the page root.  The
  // host is the animation carrier handed to `CreatePageRoute`; the content
  // root stays the node that incremental updates rebuild.
  uint32_t attach_host_id = 0;
  // `attachCustomRoute` precedes `attach`; the config configures the
  // CreatePageRoute call for the page being attached.
  struct CustomRouteConfig {
    bool pending = false;
    bool is_custom = false;
    bool opaque = true;
    bool maintain_state = true;
    int transition_ms = 0;
    int reverse_transition_ms = 0;
    bool barrier_dismissible = false;
    bool has_barrier_color = false;
    uint32_t barrier_color = 0;
    std::string barrier_label;
    bool can_transition_to = true;
    bool can_transition_from = true;
    std::string animation_hash;
    std::string secondary_animation_hash;
  };
  CustomRouteConfig custom_route;
  int pending_back_delta = 1;
  bool pending_back_animated = true;
  int page_count = 0;
  // Page id of the route currently materialized (used to ignore a second
  // didPop for the same gesture).
  int32_t current_page_id = 0;
  // Synthetic input focus used by the host keyboard channel.
  uint32_t focused_input_id = 0;
  double keyboard_height = 0.0;

  // Attached page identity, used to push incremental updates.
  Dart_PersistentHandle root_node = nullptr;
  Dart_PersistentHandle root_connector = nullptr;
  // Previously attached pages, most recent last.  Skyline keeps covered pages
  // alive while `navigateTo` pushes a new route; `navigateBack` restores the
  // covered page's root/connector so rebuilds target the route that becomes
  // visible again (without this the top page's connector is reused and the
  // revealed page stays blank).
  struct PageEntry {
    uint32_t root_id = 0;
    Dart_PersistentHandle root_node = nullptr;
    Dart_PersistentHandle root_connector = nullptr;
    int32_t page_id = 0;
  };
  std::vector<PageEntry> page_stack;
  RuntimeHolder* holder = nullptr;
};
// The DevTools addon installs a sink that forwards scroll callbacks to the
// appservice JS (`bindscroll`, worklet handlers).  The standalone host leaves
// it null and only writes diagnostics.
enum : int32_t {
  kScrollPhaseStart = 0,
  kScrollPhaseScroll = 1,
  kScrollPhaseEnd = 2,
};
using SkylineScrollEventSink = void (*)(
    uint32_t node_id, int32_t phase, double scroll_top, double scroll_left,
    double scroll_width, double scroll_height, double delta_x, double delta_y,
    uint8_t is_drag, double boundary_velocity);
// Native widget events (swiper change/transition, picker column change/start/
// end, programmatic input focus/blur) use a second sink so the addon can
// dispatch them to the appservice mirror.
enum : int32_t {
  kWidgetSwiperChange = 0,   // value = page, extra = reason
  kWidgetSwiperScroll = 1,   // value = translatePixels, extra = state, flag = current
  kWidgetPickerChange = 2,   // value = index, extra = reason
  kWidgetPickerStart = 3,
  kWidgetPickerEnd = 4,
  kWidgetInputFocus = 5,
  kWidgetInputBlur = 6,
  kWidgetRefresherPulling = 8,
  kWidgetRefresherWillRefresh = 9,
  kWidgetRefresherRefresh = 10,
  kWidgetRefresherRestore = 11,
  kWidgetRefresherAbort = 12,
  kWidgetRefresherStatus = 13,
};
using SkylineWidgetEventSink = void (*)(uint32_t node_id, int32_t kind,
                                        double value, double extra,
                                        uint8_t flag);
// Rich events (intersection/selection/navigate-back) are forwarded as JSON to
// the addon JS thread; the bootstrap parses and dispatches them.
using SkylineJsonEventSink = void (*)(const char* json);
// Layout geometry published for the appservice selector-query APIs
// (`createSelectorQuery().boundingClientRect`).  The appservice JS queries node
// geometry synchronously from the NAPI thread while the UI thread may be
// building a page, so keep an immutable, mutex-protected snapshot instead of
// walking the live tree.
struct NodeRectSnapshot {
  float left = 0.0f;
  float top = 0.0f;
  float width = 0.0f;
  float height = 0.0f;
  // Stacking order among siblings; the appservice hit test uses it to pick the
  // visually topmost target (e.g. a navigation bar above a full-screen image).
  float z_index = 0.0f;
  // 1 when CSS pointer-events: none; the appservice mirror hit test must skip
  // this node and its subtree.
  float pointer_events = 0.0f;
};
struct NodeMetric {
  AppNode* node = nullptr;
  float x = 0.0f;
  float y = 0.0f;
  float w = 0.0f;
  float h = 0.0f;
};

// Measured box size returned by the measurement pass.
struct SizeF {
  float w = 0.0f;
  float h = 0.0f;
};

// Shared process-global state.  Definitions live in appservice_model.cc; a
// module owns state that only it uses instead of listing it here.
extern std::unordered_map<intptr_t, AppNode*> g_image_nodes;
extern std::unordered_map<intptr_t, AppNode*> g_background_image_nodes;
extern std::unordered_set<std::string> g_pending_fonts;
extern std::unordered_map<uint32_t, std::string> g_font_callback_families;
extern std::unordered_map<std::string, int> g_font_status;
extern std::unordered_map<int, AppNode*> g_texture_nodes;
extern TextureDisposeFn g_texture_dispose;
extern std::unordered_map<intptr_t, AppNode*> g_builders_by_address;
extern std::vector<AppNode*> g_deferred_fixed_nodes;
extern std::unordered_set<AppNode*> g_deferred_fixed_set;
extern bool g_collect_fixed_nodes;
extern AppNode* g_fixed_root_node;

extern OpPage* g_page;
extern std::unordered_map<intptr_t, uint32_t> g_scroll_view_ids;
extern std::atomic<SkylineScrollEventSink> g_scroll_event_sink;
extern std::atomic<SkylineWidgetEventSink> g_widget_event_sink;
extern std::unordered_map<intptr_t, uint32_t> g_swiper_ids;
extern std::unordered_map<intptr_t, uint32_t> g_picker_column_ids;
extern std::unordered_map<intptr_t, uint32_t> g_selection_ids;
extern std::unordered_map<intptr_t, uint32_t> g_node_address_ids;
extern std::unordered_map<uint32_t, uint32_t> g_intersection_unique_nodes;
extern std::unordered_map<uint32_t, std::unordered_set<uint32_t>>
    g_intersection_node_observers;
extern std::unordered_map<int32_t, bool> g_navigate_back_interception;
extern std::atomic<SkylineJsonEventSink> g_json_event_sink;
extern std::mutex g_rect_mutex;
extern std::unordered_map<uint32_t, NodeRectSnapshot> g_rect_snapshot;
extern fml::RefPtr<fml::TaskRunner> g_ui_task_runner;
extern std::unique_ptr<rapidjson::Document> g_ops_document;
extern int g_next_rule_order;
extern intptr_t g_next_address;


// -- shared state helpers (defined in appservice_model.cc) -------------------
bool DeferFixedChild(AppNode* child);
bool EndsWith(const std::string& value, const std::string& suffix);
bool StartsWith(const std::string& value, const std::string& prefix);
float ResolvePercentExtent(float available, float percent);
std::string JsonEscapeString(const std::string& value);
std::string LowerAscii(const std::string& value);
std::string TrimAscii(const std::string& value);
std::unordered_map<std::string, sk_sp<flutter::DlImage>>& ImageCache();
std::vector<sk_sp<flutter::DlImage>>& RetiredImages();
std::vector<std::string> SplitChar(const std::string& value, char separator);
std::vector<std::string> SplitWhitespace(const std::string& value);
void CacheImage(const std::string& key, const sk_sp<flutter::DlImage>& image);
void EmitJsonEvent(const std::string& json);
void PublishNodeRects();
void ResetDeferredFixedNodes();

}  // namespace appservice
