// Flutter widget materialization (BuildWidget and wrappers).

#include "internal/skyline_runtime_module.hh"
#include "internal/skyline_runtime_widget.hh"

namespace appservice {

void OnBuilderBuildChild(intptr_t address, int32_t index);
void OnBuilderDisposeChild(intptr_t address, int32_t index);

// Flutter's ImageRepeat order is repeat/repeatX/repeatY/noRepeat, whereas the
// CSS parser stores no-repeat/repeat/repeat-x/repeat-y.
uint8_t FlutterImageRepeatIndex(int css_repeat) {
  switch (css_repeat) {
    case 1:
      return 0;  // ImageRepeat.repeat
    case 2:
      return 1;  // ImageRepeat.repeatX
    case 3:
      return 2;  // ImageRepeat.repeatY
    default:
      return 3;  // ImageRepeat.noRepeat
  }
}

// Converts one `background-position` axis into the alignment/offset/edge form
// consumed by NewLinearGradient (`Edge` values: left=1, top=2, right=3,
// bottom=4).
void GradientAxisFromPosition(const CssPositionAxis& pos,
                              uint8_t begin_edge,
                              uint8_t end_edge,
                              double* alignment,
                              double* offset,
                              uint8_t* side) {
  *side = pos.from_end ? end_edge : begin_edge;
  if (pos.percent) {
    *alignment = pos.value / 100.0 * 2.0 - 1.0;
    *offset = 0.0;
  } else {
    *alignment = -1.0;
    *offset = pos.value;
  }
}

// Builds the gradient argument for NewDecoratedBox: linear, radial or conic
// depending on the parsed CSS.
Dart_Handle BuildGradientHandle(const AppStyle& style) {
  if (!style.has_gradient || style.gradient_colors.size() < 2 ||
      g_page == nullptr || g_page->holder == nullptr) {
    return Dart_Null();
  }
  Dart_Handle colors =
      Dart_NewList(static_cast<intptr_t>(style.gradient_colors.size()));
  if (Dart_IsError(colors)) return Dart_Null();
  std::vector<GradientStopNative> stops;
  stops.reserve(style.gradient_colors.size());
  for (size_t i = 0; i < style.gradient_colors.size(); i++) {
    Dart_ListSetAt(colors, static_cast<intptr_t>(i),
                   Dart_NewInteger(
                       static_cast<int64_t>(style.gradient_colors[i])));
    double stop = i < style.gradient_stops.size()
                      ? style.gradient_stops[i]
                      : (style.gradient_colors.size() > 1
                             ? static_cast<double>(i) /
                                   static_cast<double>(style.gradient_colors.size() - 1)
                             : 0.0);
    uint8_t is_percent = i < style.gradient_stop_percent.size()
                             ? style.gradient_stop_percent[i]
                             : 1;
    stops.push_back(GradientStopNative{stop, is_percent});
  }

  // Radial/conic centers are fractions for percentages and px otherwise.
  const double center_x = style.gradient_center_x_percent
                              ? style.gradient_center_x / 100.0
                              : style.gradient_center_x;
  const double center_y = style.gradient_center_y_percent
                              ? style.gradient_center_y / 100.0
                              : style.gradient_center_y;
  const uint8_t center_x_percent = style.gradient_center_x_percent ? 1 : 0;
  const uint8_t center_y_percent = style.gradient_center_y_percent ? 1 : 0;

  if (style.gradient_kind == kGradientRadial) {
    auto* new_radial = reinterpret_cast<NewRadialGradientFn>(
        FindCallback(g_page->holder->ffi_table, kNewRadialGradientHash));
    if (new_radial == nullptr) return Dart_Null();
    return new_radial(center_x, center_y, center_x_percent, center_y_percent,
                      static_cast<double>(style.gradient_radius),
                      static_cast<uint8_t>(style.gradient_radial_size), colors,
                      stops.data());
  }
  if (style.gradient_kind == kGradientConic) {
    auto* new_conic = reinterpret_cast<NewConicGradientFn>(
        FindCallback(g_page->holder->ffi_table, kNewConicGradientHash));
    if (new_conic == nullptr) return Dart_Null();
    // NewConicGradient takes its stops as a Dart list of turns.
    Dart_Handle stop_list =
        Dart_NewList(static_cast<intptr_t>(stops.size()));
    if (Dart_IsError(stop_list)) return Dart_Null();
    for (size_t i = 0; i < stops.size(); i++) {
      Dart_ListSetAt(stop_list, static_cast<intptr_t>(i),
                     Dart_NewDouble(stops[i].value));
    }
    return new_conic(style.gradient_angle, center_x, center_y,
                     center_x_percent, center_y_percent, colors, stop_list);
  }

  auto* new_linear = reinterpret_cast<NewLinearGradientFn>(
      FindCallback(g_page->holder->ffi_table, kNewLinearGradientHash));
  if (new_linear == nullptr) return Dart_Null();
  // `background-size` only affects the linear layer: radial/conic gradients
  // have no fitting-size parameters in their FFI surface.
  double fit_width = -1.0;
  double fit_height = -1.0;
  uint8_t fit_width_percent = 0;
  uint8_t fit_height_percent = 0;
  if (style.has_background_size && style.background_fit == 0) {
    fit_width = style.background_size_width;
    fit_height = style.background_size_height;
    fit_width_percent = style.background_size_width_percent ? 1 : 0;
    fit_height_percent = style.background_size_height_percent ? 1 : 0;
  }
  double alignment_x = -1.0;
  double alignment_y = -1.0;
  double offset_x = 0.0;
  double offset_y = 0.0;
  uint8_t side_x = 1;  // Edge.left
  uint8_t side_y = 2;  // Edge.top
  GradientAxisFromPosition(style.background_position_x, 1, 3, &alignment_x,
                           &offset_x, &side_x);
  GradientAxisFromPosition(style.background_position_y, 2, 4, &alignment_y,
                           &offset_y, &side_y);
  return new_linear(style.gradient_angle, colors, stops.data(), fit_width,
                    fit_height, fit_width_percent, fit_height_percent, side_x,
                    side_y, alignment_x, alignment_y, offset_x, offset_y,
                    FlutterImageRepeatIndex(style.background_repeat));
}

Dart_Handle BuildBoxShadowList(const AppStyle& style) {  if (!style.has_shadow || g_page == nullptr || g_page->holder == nullptr) {
    return Dart_Null();
  }
  auto* new_box_shadow = reinterpret_cast<NewBoxShadowFn>(
      FindCallback(g_page->holder->ffi_table, kNewBoxShadowHash));
  if (new_box_shadow == nullptr) return Dart_Null();
  Dart_Handle shadow = new_box_shadow(
      style.shadow_color, static_cast<double>(style.shadow_offset_x),
      static_cast<double>(style.shadow_offset_y),
      static_cast<double>(style.shadow_blur),
      static_cast<double>(style.shadow_spread), style.shadow_inset ? 1 : 0);
  if (Dart_IsError(shadow) || Dart_IsNull(shadow)) return Dart_Null();
  Dart_Handle list = Dart_NewList(1);
  if (Dart_IsError(list)) return Dart_Null();
  Dart_ListSetAt(list, 0, shadow);
  return list;
}

// -- native input handle -----------------------------------------------------
//
// The replay renders text nodes directly, but the real SkylineInput widget is
// still created (off-tree) so the native Input* callbacks have a valid Dart
// handle.  This is what makes InputHoldKeyboard / InputCursor /
// InputSelectionStart/End observable instead of log-only.
void InputNoopMarkDirty(intptr_t) {}
void InputNoopOnInput(intptr_t, Dart_Handle, uint32_t, uint32_t) {}
void InputNoopOnFocus(intptr_t, Dart_Handle, uint32_t) {}
void InputNoopOnBlur(intptr_t, Dart_Handle, Dart_Handle, Dart_Handle) {}
void InputNoopOnLineChange(intptr_t, uint32_t, uint32_t, uint32_t) {}
void InputNoopOnConfirm(intptr_t, Dart_Handle) {}
void InputNoopOnKeyboardHeightChange(intptr_t, uint32_t, uint32_t) {}
void InputNoopOnSelectionChange(intptr_t, uint32_t, uint32_t) {}
void InputNoopOnComposition(intptr_t, Dart_Handle) {}
void InputNoopOnPageBottomPaddingChange(intptr_t, double) {}

Dart_Handle CreateInputStateWidget(NewInputFn newInput, AppNode* node) {
  if (newInput == nullptr || node == nullptr) return nullptr;
  // The builtin input forwards its resolved props as camelCase attributes on
  // the `native-input` element (`placeholder`, `cursorColor`, `maxlength`, ...).
  std::string value_text = node->text;
  if (value_text.empty()) value_text = AttrText(node, "value", "");
  const std::string type_text = AttrText(node, "type", "text");
  Dart_Handle value = Dart_NewStringFromCString(value_text.c_str());
  Dart_Handle type = Dart_NewStringFromCString(type_text.c_str());
  const std::string placeholder_text = AttrText(node, "placeholder", "");
  Dart_Handle placeholder =
      Dart_NewStringFromCString(placeholder_text.c_str());
  const std::string confirm_text = AttrText(node, "confirmType", "done");
  Dart_Handle confirm = Dart_NewStringFromCString(confirm_text.c_str());
  if (Dart_IsError(value) || Dart_IsError(type) || Dart_IsError(placeholder) ||
      Dart_IsError(confirm)) {
    return nullptr;
  }
  const AppStyle& style = node->style;
  const double line_height =
      style.line_height > 0.0 ? style.line_height / style.font_size
                              : style.line_height_factor;
  const double letter_spacing =
      std::isnan(style.letter_spacing) ? std::nan("") : style.letter_spacing;
  const uint8_t focus =
      g_page != nullptr && g_page->focused_input_id == node->id ? 1 : 0;
  const double placeholder_font_size =
      AttrNumber(node, "placeholderFontSize", style.font_size);
  const uint32_t placeholder_color =
      ParseColorString(AttrText(node, "placeholderColor", "").c_str(),
                       0xFF808080u);
  const uint32_t cursor_color =
      ParseColorString(AttrText(node, "cursorColor", "").c_str(),
                       0xFF000000u);
  const int32_t max_length = static_cast<int32_t>(
      AttrNumber(node, "maxlength", AttrNumber(node, "maxLength", 140.0)));
  return newInput(
      node->address, reinterpret_cast<void*>(&InputNoopMarkDirty), 1, node->id,
      0 /* multiLine */, value, type,
      AttrBool(node, "password", false) ? 1 : 0, placeholder,
      placeholder_font_size, static_cast<uint8_t>(style.font_weight),
      placeholder_color,
      AttrBool(node, "disabled", false) || AttrBool(node, "disable", false)
          ? 1 : 0,
      max_length, static_cast<int32_t>(AttrNumber(node, "cursorSpacing", 0.0)),
      cursor_color,
      focus, AttrBool(node, "autoHeight", false) ? 1 : 0, confirm,
      AttrBool(node, "confirmHold", false) ? 1 : 0,
      node->input_cursor, node->input_selection_start,
      node->input_selection_end,
      AttrBool(node, "showConfirmBar", false) ? 1 : 0,
      AttrBool(node, "adjustPosition", true) ? 1 : 0,
      node->input_hold_keyboard ? 1 : 0, static_cast<uint8_t>(style.text_align),
      style.color, 0 /* backgroundColor */, 0 /* textDecoration */,
      0 /* textDecorationStyle */, 0u /* decorationColor */,
      0.0 /* decorationThickness */, nullptr, 0, style.font_size, line_height,
      static_cast<uint8_t>(style.font_weight),
      static_cast<uint8_t>(style.font_style), Dart_Null(), letter_spacing,
      std::nan(""), Dart_Null(), reinterpret_cast<void*>(&InputNoopOnInput),
      reinterpret_cast<void*>(&InputNoopOnFocus),
      reinterpret_cast<void*>(&InputNoopOnBlur),
      reinterpret_cast<void*>(&InputNoopOnLineChange),
      reinterpret_cast<void*>(&InputNoopOnConfirm),
      reinterpret_cast<void*>(&InputNoopOnKeyboardHeightChange),
      reinterpret_cast<void*>(&InputNoopOnSelectionChange),
      reinterpret_cast<void*>(&InputNoopOnComposition),
      reinterpret_cast<void*>(&InputNoopOnComposition),
      reinterpret_cast<void*>(&InputNoopOnComposition),
      reinterpret_cast<void*>(&InputNoopOnComposition),
      reinterpret_cast<void*>(&InputNoopOnPageBottomPaddingChange));
}

bool IsStickyHeaderNode(const AppNode* node) {
  return node != nullptr &&
         (node->tag == "stickyheader" || node->name == "stickyheader");
}

bool HasStickyFlowChildren(const AppNode* node) {
  if (node == nullptr) return false;
  for (const AppNode* child : node->children) {
    if (child == nullptr || child->released || child->style.display_none) {
      continue;
    }
    // The glass-easel adapter emits dedicated `stickyheader` nodes; older
    // recordings marked them with `position: sticky` instead.
    if (child->style.position == 3 || IsStickyHeaderNode(child)) return true;
  }
  return false;
}

void OnStickyHeaderStickOnTop(intptr_t native_widget_ptr, uint8_t value) {
  if (std::getenv("SKYLINE_APP_SERVICE_DEBUG") == nullptr) return;
  std::fprintf(stderr,
               "[skyline-runtime] sticky header 0x%lx onTop=%u\n",
               static_cast<long>(native_widget_ptr),
               static_cast<unsigned>(value));
}

// ---------------------------------------------------------------------------
// D10: accessibility semantics
//
// The appservice WXML layer carries the W3C accessibility attributes on the
// DOM mirror (`aria-label`, `aria-role`, `aria-hidden`, `aria-disabled`).  The
// official native runtime wraps the element's Flutter widget in
// `SkylineSemantics` (NewSemantics) and keeps it in sync with Semantics*.
// ---------------------------------------------------------------------------

bool IsSnapshotNode(const AppNode* node) {
  return node != nullptr &&
         (node->tag == "snapshot" || node->name == "snapshot");
}

const char* const kSemanticsLabelKeys[] = {"aria-label", "ariaLabel",
                                           "semanticsLabel", "semantics-label"};
const char* const kSemanticsRoleKeys[] = {"aria-role", "ariaRole", "role",
                                          "semanticsRole", "semantics-role"};
const char* const kSemanticsHiddenKeys[] = {
    "aria-hidden", "ariaHidden", "semanticsHidden", "semantics-hidden"};
const char* const kSemanticsDisableKeys[] = {
    "aria-disabled", "ariaDisabled", "semanticsDisabled", "semanticsDisable",
    "semantics-disabled"};

bool FindAttrKey(const AppNode* node, const char* const* keys, size_t count,
                 std::string* value) {
  for (size_t i = 0; i < count; i++) {
    const auto found = node->attrs.find(keys[i]);
    if (found != node->attrs.end()) {
      if (value != nullptr) *value = found->second;
      return true;
    }
  }
  return false;
}

bool IsSemanticsAttribute(const char* name) {
  if (name == nullptr || *name == '\0') return false;
  const struct {
    const char* const* keys;
    size_t count;
  } groups[] = {
      {kSemanticsLabelKeys, sizeof(kSemanticsLabelKeys) / sizeof(char*)},
      {kSemanticsRoleKeys, sizeof(kSemanticsRoleKeys) / sizeof(char*)},
      {kSemanticsHiddenKeys, sizeof(kSemanticsHiddenKeys) / sizeof(char*)},
      {kSemanticsDisableKeys, sizeof(kSemanticsDisableKeys) / sizeof(char*)},
  };
  for (const auto& group : groups) {
    for (size_t i = 0; i < group.count; i++) {
      if (std::strcmp(name, group.keys[i]) == 0) return true;
    }
  }
  return false;
}

struct SemanticsInfo {
  std::string label;
  std::string role;
  bool hidden = false;
  bool disabled = false;
  bool present = false;
};

bool ReadSemanticsInfo(const AppNode* node, SemanticsInfo* info) {
  if (node == nullptr || info == nullptr) return false;
  bool label_present = FindAttrKey(
      node, kSemanticsLabelKeys,
      sizeof(kSemanticsLabelKeys) / sizeof(char*), &info->label);
  bool role_present = FindAttrKey(
      node, kSemanticsRoleKeys,
      sizeof(kSemanticsRoleKeys) / sizeof(char*), &info->role);
  bool hidden_present = FindAttrKey(
      node, kSemanticsHiddenKeys,
      sizeof(kSemanticsHiddenKeys) / sizeof(char*), nullptr);
  bool disable_present = FindAttrKey(
      node, kSemanticsDisableKeys,
      sizeof(kSemanticsDisableKeys) / sizeof(char*), nullptr);
  if (hidden_present) {
    for (size_t i = 0;
         i < sizeof(kSemanticsHiddenKeys) / sizeof(char*); i++) {
      if (node->attrs.find(kSemanticsHiddenKeys[i]) != node->attrs.end()) {
        info->hidden = AttrBool(node, kSemanticsHiddenKeys[i], false);
        break;
      }
    }
  }
  if (disable_present) {
    for (size_t i = 0;
         i < sizeof(kSemanticsDisableKeys) / sizeof(char*); i++) {
      if (node->attrs.find(kSemanticsDisableKeys[i]) != node->attrs.end()) {
        info->disabled = AttrBool(node, kSemanticsDisableKeys[i], false);
        break;
      }
    }
  }
  info->present = label_present || role_present || hidden_present ||
                  disable_present;
  return info->present;
}

Dart_Handle WrapSemantics(AppNode* node, Dart_Handle inner) {
  if (node == nullptr || Dart_IsNull(inner) || g_page == nullptr ||
      g_page->holder == nullptr) {
    return inner;
  }
  SemanticsInfo info;
  if (!ReadSemanticsInfo(node, &info)) return inner;
  auto* new_semantics = reinterpret_cast<NewSemanticsFn>(
      FindCallback(g_page->holder->ffi_table, kNewSemanticsHash));
  if (new_semantics == nullptr) return inner;
  Dart_Handle label = Dart_NewStringFromCString(info.label.c_str());
  Dart_Handle role = Dart_NewStringFromCString(info.role.c_str());
  if (Dart_IsError(label)) label = Dart_NewStringFromCString("");
  if (Dart_IsError(role)) role = Dart_NewStringFromCString("");
  Dart_Handle semantics =
      new_semantics(inner, label, role, info.hidden ? 1 : 0,
                    info.disabled ? 1 : 0);
  if (Dart_IsError(semantics)) {
    ReportDartError("appservice NewSemantics", semantics);
    return inner;
  }
  node->semantics_widget = semantics;
  node->semantics_p = Dart_NewPersistentHandle(semantics);
  std::fprintf(stderr,
               "[skyline-runtime] semantics #%u label=\"%s\" role=\"%s\" "
               "hidden=%d disabled=%d\n",
               node->id, info.label.c_str(), info.role.c_str(),
               info.hidden ? 1 : 0, info.disabled ? 1 : 0);
  return semantics;
}

// Attribute changes reuse the materialized SkylineSemantics widget.  Adding or
// removing the wrapper itself needs a rebuild.
void UpdateNodeSemantics(AppNode* node) {
  if (node == nullptr || g_page == nullptr || g_page->holder == nullptr) return;
  SemanticsInfo info;
  const bool present = ReadSemanticsInfo(node, &info);
  if (node->semantics_p == nullptr) {
    if (present) g_page->force_rebuild = true;
    return;
  }
  if (!present) {
    g_page->force_rebuild = true;
    return;
  }
  Dart_Handle semantics = Dart_HandleFromPersistent(node->semantics_p);
  auto* label = reinterpret_cast<SemanticsTextFn>(
      FindCallback(g_page->holder->ffi_table, kSemanticsLabelHash));
  auto* role = reinterpret_cast<SemanticsTextFn>(
      FindCallback(g_page->holder->ffi_table, kSemanticsRoleHash));
  auto* hidden = reinterpret_cast<SemanticsBoolFn>(
      FindCallback(g_page->holder->ffi_table, kSemanticsHiddenHash));
  auto* disable = reinterpret_cast<SemanticsBoolFn>(
      FindCallback(g_page->holder->ffi_table, kSemanticsDisableHash));
  if (label != nullptr) {
    Dart_Handle text = Dart_NewStringFromCString(info.label.c_str());
    if (!Dart_IsError(text)) label(semantics, text);
  }
  if (role != nullptr) {
    Dart_Handle text = Dart_NewStringFromCString(info.role.c_str());
    if (!Dart_IsError(text)) role(semantics, text);
  }
  if (hidden != nullptr) hidden(semantics, info.hidden ? 1 : 0);
  if (disable != nullptr) disable(semantics, info.disabled ? 1 : 0);
  std::fprintf(stderr,
               "[skyline-runtime] semantics update #%u label=\"%s\" "
               "role=\"%s\" hidden=%d disabled=%d\n",
               node->id, info.label.c_str(), info.role.c_str(),
               info.hidden ? 1 : 0, info.disabled ? 1 : 0);
}

// `<snapshot>` wraps its child in the Dart SkylineSnapshot widget so DevTools
// automation can request per-node snapshots (YGNodeTakeSnapshot also works
// against any YGNode).
Dart_Handle WrapSnapshot(AppNode* node, Dart_Handle inner) {
  if (node == nullptr || Dart_IsNull(inner) || g_page == nullptr ||
      g_page->holder == nullptr) {
    return inner;
  }
  auto* new_snapshot = reinterpret_cast<NewSnapshotFn>(
      FindCallback(g_page->holder->ffi_table, kNewSnapshotHash));
  if (new_snapshot == nullptr) return inner;
  Dart_Handle snapshot = new_snapshot(
      inner, AttrBool(node, "allowSnapshotting", false) ? 1 : 0);
  if (Dart_IsError(snapshot)) {
    ReportDartError("appservice NewSnapshot", snapshot);
    return inner;
  }
  node->snapshot_widget = snapshot;
  node->snapshot_p = Dart_NewPersistentHandle(snapshot);
  return snapshot;
}

// -- share-element Hero flights --------------------------------------------
// The builtin `<share-element>` component forwards its resolved props as
// camelCase attributes (`tag`, `onFrame`, `transitionOnGesture`,
// `rectTweenType`, `shuttleOnPush`, `shuttleOnPop`) through `setAttributes`.
// Native builds a `SkylineHero` around the element so Flutter's Hero machinery
// can fly the shared content between the two routes; the `onFrame-<viewTag>`
// worklet registered by the component is driven by HandleHeroOnFrame.
extern "C" void HeroMarkDirty(intptr_t /*ptr*/) {}

Dart_Handle HeroStringHandle(const std::string& value) {
  return Dart_NewStringFromCString(value.c_str());
}

std::string HeroAttrText(const AppNode* node, const char* camel,
                         const char* kebab, const char* fallback) {
  std::string value = AttrText(node, camel, "");
  if (value.empty()) value = AttrText(node, kebab, "");
  return value.empty() ? std::string(fallback) : value;
}

bool HeroAttrBool(const AppNode* node, const char* camel,
                  const char* kebab, bool fallback) {
  if (node->attrs.count(camel)) return AttrBool(node, camel, fallback);
  if (node->attrs.count(kebab)) return AttrBool(node, kebab, fallback);
  return fallback;
}

Dart_Handle WrapHero(AppNode* node, Dart_Handle inner) {
  if (node == nullptr || Dart_IsNull(inner) || g_page == nullptr ||
      g_page->holder == nullptr) {
    return inner;
  }
  auto* new_hero = reinterpret_cast<NewHeroFn>(
      FindCallback(g_page->holder->ffi_table, kNewHeroHash));
  if (new_hero == nullptr) return inner;
  const std::string tag = HeroAttrText(node, "tag", "key", "");
  if (tag.empty()) return inner;
  const std::string on_frame_attr = HeroAttrText(node, "onFrame", "onframe", "");
  // The component registers its worklet as `onFrame-<viewTag>`; native passes
  // that hash to Dart so the frame callback lands on the right handler.
  const std::string on_frame =
      on_frame_attr.empty() ? "" : "onFrame-" + std::to_string(node->id);
  const bool gesture = HeroAttrBool(node, "transitionOnGesture",
                                    "transition-on-gesture", false);
  const std::string rect_tween_type =
      HeroAttrText(node, "rectTweenType", "rect-tween-type", "");
  const std::string shuttle_on_push =
      HeroAttrText(node, "shuttleOnPush", "shuttle-on-push", "to");
  const std::string shuttle_on_pop =
      HeroAttrText(node, "shuttleOnPop", "shuttle-on-pop", "to");
  Dart_Handle tag_handle = HeroStringHandle(tag);
  Dart_Handle on_frame_handle = HeroStringHandle(on_frame);
  Dart_Handle rect_tween_handle = HeroStringHandle(rect_tween_type);
  Dart_Handle shuttle_push_handle = HeroStringHandle(shuttle_on_push);
  Dart_Handle shuttle_pop_handle = HeroStringHandle(shuttle_on_pop);
  if (Dart_IsError(tag_handle) || Dart_IsError(on_frame_handle) ||
      Dart_IsError(rect_tween_handle) || Dart_IsError(shuttle_push_handle) ||
      Dart_IsError(shuttle_pop_handle)) {
    return inner;
  }
  Dart_Handle hero = new_hero(
      node->address, reinterpret_cast<void*>(&HeroMarkDirty), tag_handle,
      gesture ? 1 : 0, on_frame_handle, rect_tween_handle, shuttle_push_handle,
      shuttle_pop_handle, inner);
  if (Dart_IsError(hero) || Dart_IsNull(hero)) {
    if (Dart_IsError(hero)) ReportDartError("appservice NewHero", hero);
    return inner;
  }
  DeletePersistent(&node->hero_p);
  node->hero_widget = hero;
  node->hero_p = Dart_NewPersistentHandle(hero);
  if (std::getenv("SKYLINE_APP_SERVICE_DEBUG") != nullptr) {
    std::fprintf(stderr,
                 "[skyline-runtime] hero #%u tag=%s gesture=%d onFrame=%s "
                 "tween=%s push=%s pop=%s\n",
                 node->id, tag.c_str(), gesture ? 1 : 0, on_frame.c_str(),
                 rect_tween_type.c_str(), shuttle_on_push.c_str(),
                 shuttle_on_pop.c_str());
  }
  return hero;
}

// Wraps a widget with the element's CSS box painting: background (colour,
// image, gradient), borders, corner radii, shadow, opacity and clip.  Generic
// views run this after building their connector; builtin containers that
// construct their own widget (swiper) call it too, so a CSS background on the
// element is not dropped (`.swiper { background-color: white }`).
Dart_Handle WrapVisualStyle(AppNode* node, Dart_Handle inner,
                            NewDecoratedBoxFn newDecoratedBox,
                            NewOpacityFn newOpacity) {
  const AppStyle& style = node->style;
  const bool wants_background_image = !style.background_image.empty();
  const bool wants_paint_transition =
      (style.transition_mask &
       (kTransitionBackground | kTransitionBorderColor)) != 0;
  const bool has_visual = style.has_background || HasBorder(style) ||
                          HasAnyRadius(style) || style.has_shadow ||
                          wants_background_image || style.has_gradient ||
                          wants_paint_transition;
  if (style.clip && g_page != nullptr && g_page->holder != nullptr) {
    auto* new_clip = reinterpret_cast<NewClipRRectFn>(
        FindCallback(g_page->holder->ffi_table, kNewClipRRectHash));
    if (new_clip != nullptr) {
      const double tl = EffectiveCornerRadius(style, 0);
      const double tr = EffectiveCornerRadius(style, 1);
      const double br = EffectiveCornerRadius(style, 2);
      const double bl = EffectiveCornerRadius(style, 3);
      const uint8_t tl_pct = CornerRadiusPercent(style, 0) ? 1 : 0;
      const uint8_t tr_pct = CornerRadiusPercent(style, 1) ? 1 : 0;
      const uint8_t br_pct = CornerRadiusPercent(style, 2) ? 1 : 0;
      const uint8_t bl_pct = CornerRadiusPercent(style, 3) ? 1 : 0;
      Dart_Handle clip = new_clip(
          0, tl, tl, tr, tr, bl, bl, br, br, tl_pct, tl_pct, tr_pct, tr_pct,
          bl_pct, bl_pct, br_pct, br_pct, EffectiveBorderWidth(style, 3),
          EffectiveBorderWidth(style, 0), EffectiveBorderWidth(style, 1),
          EffectiveBorderWidth(style, 2), inner);
      if (Dart_IsError(clip)) {
        ReportDartError("appservice NewClipRRect", clip);
      } else {
        inner = clip;
        node->clip_widget = clip;
        node->clip_widget_p = Dart_NewPersistentHandle(clip);
      }
    }
  }
  if (has_visual) {
    if (wants_background_image) LoadBackgroundImageForNode(node);
    double widths[4] = {0, 0, 0, 0};
    uint32_t colors[4] = {0u, 0u, 0u, 0u};
    uint8_t styles[4] = {0, 0, 0, 0};
    for (int side = 0; side < 4; side++) {
      widths[side] = EffectiveBorderWidth(style, side);
      if (widths[side] > 0.0) {
        colors[side] = EffectiveBorderColor(style, side);
        styles[side] = 1;
      }
    }
    Dart_Handle shadows = BuildBoxShadowList(style);
    Dart_Handle gradient = BuildGradientHandle(style);
    const double radius_tl = EffectiveCornerRadius(style, 0);
    const double radius_tr = EffectiveCornerRadius(style, 1);
    const double radius_br = EffectiveCornerRadius(style, 2);
    const double radius_bl = EffectiveCornerRadius(style, 3);
    const uint8_t radius_tl_pct = CornerRadiusPercent(style, 0) ? 1 : 0;
    const uint8_t radius_tr_pct = CornerRadiusPercent(style, 1) ? 1 : 0;
    const uint8_t radius_br_pct = CornerRadiusPercent(style, 2) ? 1 : 0;
    const uint8_t radius_bl_pct = CornerRadiusPercent(style, 3) ? 1 : 0;
    Dart_Handle decorated = newDecoratedBox(
        node->address, style.background, widths[3], widths[0], widths[1],
        widths[2], colors[3], colors[0], colors[1], colors[2], styles[3],
        styles[0], styles[1], styles[2], radius_tl, radius_tl, radius_tr,
        radius_tr, radius_bl, radius_bl, radius_br, radius_br, radius_tl_pct,
        radius_tl_pct, radius_tr_pct, radius_tr_pct, radius_bl_pct,
        radius_bl_pct, radius_br_pct, radius_br_pct, Dart_Null(), 0, 0, 0.0,
        0.0, 0.0, 0.0,
        static_cast<uint8_t>(wants_background_image ? style.background_repeat
                                                    : 0),
        static_cast<uint8_t>(wants_background_image ? style.background_fit
                                                    : 0),
        0.0, 0.0, 0, 0, gradient, 0, shadows, 0,
        reinterpret_cast<void*>(&BackgroundImagePaint),
        reinterpret_cast<void*>(&BackgroundImageCreate),
        reinterpret_cast<void*>(&BackgroundImageDisposed), nullptr, inner);
    if (Dart_IsError(decorated)) {
      ReportDartError("appservice NewDecoratedBox", decorated);
    } else {
      inner = decorated;
      node->decorated_box = decorated;
      node->decorated_box_p = Dart_NewPersistentHandle(decorated);
      if (wants_background_image) {
        g_background_image_nodes[node->address] = node;
      }
    }
  }
  if (style.opacity < 0.999 ||
      (style.transition_mask & kTransitionOpacity) != 0) {
    Dart_Handle opacity = newOpacity(style.opacity, inner);
    if (Dart_IsError(opacity)) {
      ReportDartError("appservice NewOpacity", opacity);
    } else {
      inner = opacity;
      node->opacity_widget = opacity;
      node->opacity_widget_p = Dart_NewPersistentHandle(opacity);
    }
  }
  return inner;
}

// The Skyline widget constructors (`YGConnector`, `Swiper`, ...) mutate their
// `children` list (custom routes call `connector.children.clear()` and
// `add(...)` in `buildTransitions`).  `Dart_NewList` creates a fixed-length
// list, so `SkylineMultiChildRenderObjectWidget` (Dart side) copies it into a
// growable list.
Dart_Handle BuildWidget(AppNode* node, NewYGNodeFn newYGNode,
                        NewYGConnectorFn newYGConnector, NewTextFn newText,
                        NewTextSpanFn newTextSpan,
                        NewDecoratedBoxFn newDecoratedBox,
                        NewOpacityFn newOpacity, NewScrollViewFn newScrollView,
                        NewListViewFn newListView, NewGridViewFn newGridView,
                        NewDirectRenderWidgetFn newDirectRenderWidget,
                        NewListBuilderFn newListBuilder,
                        NewGridBuilderFn newGridBuilder, bool is_root) {
  if (node->released || node->style.display_none) {
    return Dart_Null();
  }
  const AppStyle& style = node->style;
  Dart_Handle inner = Dart_Null();

  if (node->tag == "text") {
    if (node->name == "input" && g_page != nullptr &&
        g_page->holder != nullptr) {
      auto* newInput = reinterpret_cast<NewInputFn>(FindCallback(
          g_page->holder->ffi_table, kNewInputHash));
      if (newInput != nullptr) {
        DeletePersistent(&node->input_widget_p);
        Dart_Handle input_widget = CreateInputStateWidget(newInput, node);
        if (input_widget != nullptr && !Dart_IsError(input_widget) &&
            !Dart_IsNull(input_widget)) {
          node->input_widget = input_widget;
          node->input_widget_p = Dart_NewPersistentHandle(input_widget);
        } else if (Dart_IsError(input_widget)) {
          ReportDartError("appservice NewInput", input_widget);
        }
      }
    }
    const std::string text = NodeNormalizedText(node);
    // NewTextSpan's `height` is the Flutter line-height multiplier, while CSS
    // line-height is in pixels; convert back to a multiplier.
    const double line_height =
        style.line_height > 0.0 ? style.line_height / style.font_size
                                : style.line_height_factor;
    Dart_Handle span_handle = nullptr;
    // CSS white-space nowrap/pre disable wrapping; text-overflow is the
    // Flutter TextOverflow index (ellipsis = 2).
    const bool soft_wrap = style.white_space != 1 && style.white_space != 2;
    const bool user_select =
        AttrBool(node, "userSelect", AttrBool(node, "selectable", false)) ||
        InsideSelectionContext(node);
    inner = CreateTextWidget(
        newText, newTextSpan, node->address, text.c_str(), style.font_size,
        style.color, style.font_weight, style.font_style,
        std::isnan(style.letter_spacing) ? std::nan("") : style.letter_spacing,
        line_height, style.text_align, static_cast<int>(style.max_lines),
        style.text_overflow, soft_wrap, user_select, style.font_families,
        &span_handle);
    if (Dart_IsError(inner)) {
      ReportDartError("appservice text widget", inner);
      return Dart_Null();
    }
    node->text_widget = inner;
    node->text_widget_p = Dart_NewPersistentHandle(inner);
    if (span_handle != nullptr) {
      node->text_span_p = Dart_NewPersistentHandle(span_handle);
    }
  } else if (node->name == "native-input" && node->tag == "input" &&
             g_page != nullptr && g_page->holder != nullptr) {
    // Builtin `<input>` renders its native editor here (`native-input` is the
    // input component's shadow child).  The DevTools replay used to keep this
    // widget off-tree, so the grey footer pill had no placeholder text.
    auto* newInput = reinterpret_cast<NewInputFn>(
        FindCallback(g_page->holder->ffi_table, kNewInputHash));
    if (newInput != nullptr) {
      DeletePersistent(&node->input_widget_p);
      Dart_Handle input_widget = CreateInputStateWidget(newInput, node);
      if (input_widget != nullptr && !Dart_IsError(input_widget) &&
          !Dart_IsNull(input_widget)) {
        node->input_widget = input_widget;
        node->input_widget_p = Dart_NewPersistentHandle(input_widget);
        inner = input_widget;
      } else if (Dart_IsError(input_widget)) {
        ReportDartError("appservice NewInput", input_widget);
      }
    }
  } else if (node->tag == "texture") {
    // Native view slot (`wx-canvas`, map/video placeholders, ...).  Dart owns
    // hit-testing and forwards pointer events back through
    // `HandleNativeViewPointerEvent`.
    if (g_page != nullptr && g_page->holder != nullptr) {
      auto* newTexture = reinterpret_cast<NewTextureFn>(
          FindCallback(g_page->holder->ffi_table, kNewTextureHash));
      auto* disposeTexture = reinterpret_cast<TextureDisposeFn>(
          FindCallback(g_page->holder->ffi_table, kTextureDisposeHash));
      if (disposeTexture != nullptr) g_texture_dispose = disposeTexture;
      if (newTexture != nullptr) {
        const int view_id =
            static_cast<int>(AttrNumber(node, "viewId", node->id));
        const std::string type = AttrText(node, "type", "canvas");
        node->texture_view_id = view_id;
        node->texture_type = type;
        Dart_Handle type_handle = Dart_NewStringFromCString(type.c_str());
        if (!Dart_IsError(type_handle)) {
          Dart_Handle texture = newTexture(view_id, type_handle, 1);
          if (Dart_IsError(texture)) {
            ReportDartError("appservice NewTexture", texture);
          } else {
            inner = texture;
            node->texture_widget = texture;
            node->texture_widget_p = Dart_NewPersistentHandle(texture);
            g_texture_nodes[view_id] = node;
            std::fprintf(stderr,
                         "[skyline-runtime] native view #%u: viewId=%d "
                         "type=%s\n",
                         node->id, view_id, type.c_str());
          }
        }
      }
    }
  } else if (IsImageName(node->name) && newDirectRenderWidget != nullptr) {
    // Network images are fetched asynchronously; the widget is installed
    // immediately and repaints after the page is rebuilt on response.
    LoadImageForNode(node);
    Dart_Handle widget = newDirectRenderWidget(
        node->address, reinterpret_cast<void*>(&DirectRenderImagePaint),
        reinterpret_cast<void*>(&DirectRenderImageDispose),
        reinterpret_cast<void*>(&DirectRenderImageCreate),
        reinterpret_cast<void*>(&DirectRenderImageLayout));
    if (Dart_IsError(widget)) {
      ReportDartError("appservice direct render widget", widget);
    } else {
      inner = widget;
      DeletePersistent(&node->image_widget_p);
      node->image_widget_p = Dart_NewPersistentHandle(widget);
      g_image_nodes[node->address] = node;
      // `<image>` has no border/paint parameters of its own: the CSS box
      // (background, border and the border-radius clip) is painted by the
      // shared visual wrappers.  Without it avatars keep square corners.
      inner = WrapVisualStyle(node, inner, newDecoratedBox, newOpacity);
    }
  } else if (IsBuilderName(node->name) &&
             ((IsGridBuilderName(node->name) && newGridBuilder != nullptr) ||
              (IsListBuilderName(node->name) && newListBuilder != nullptr))) {
    // `<list-builder>`/`<grid-builder>`: a virtualized sliver.  Items are not
    // built here; the sliver requests them through onBuildChild.
    node->builder_kind = IsGridBuilderName(node->name) ? 1 : 0;
    node->builder_child_count = static_cast<int>(
        AttrNumber(node, "childCount", static_cast<double>(AttrNumber(
                                           node, "child-count", 0.0))));
    node->builder_child_height =
        static_cast<float>(AttrNumber(node, "childHeight", 0.0));
    node->builder_cross_axis_count = std::max(
        1, static_cast<int>(AttrNumber(node, "crossAxisCount", 2.0)));
    node->builder_main_spacing =
        static_cast<float>(GridMainGap(node));
    node->builder_cross_spacing =
        static_cast<float>(GridCrossGap(node));
    node->builder_max_cross_extent =
        static_cast<float>(AttrNumber(node, "maxCrossAxisExtent", 0.0));
    node->builder_background = style.has_background ? style.background : 0u;
    node->builder_synced_child_count = node->builder_child_count;
    node->builder_synced_child_height = node->builder_child_height;
    node->builder_synced_cross_axis_count = node->builder_cross_axis_count;
    node->builder_synced_main_spacing = node->builder_main_spacing;
    node->builder_synced_cross_spacing = node->builder_cross_spacing;
    node->builder_synced_max_cross_extent = node->builder_max_cross_extent;
    node->builder_synced_background = node->builder_background;
    Dart_Handle empty = Dart_NewList(0);
    Dart_Handle builder = Dart_Null();
    if (node->builder_kind == 1) {
      // GridBuilderType: aligned = 0, masonry = 1 (opposite of GridViewType).
      builder = newGridBuilder(
          node->address, empty,
          static_cast<uint8_t>(GridIsMasonry(node) ? 1 : 0),
          node->builder_child_count, node->builder_cross_axis_count,
          node->builder_main_spacing, node->builder_cross_spacing,
          node->builder_max_cross_extent, style.padding[0], style.padding[1],
          style.padding[2], style.padding[3], node->builder_background,
          reinterpret_cast<void*>(&OnBuilderBuildChild),
          reinterpret_cast<void*>(&OnBuilderDisposeChild));
    } else {
      builder = newListBuilder(
          node->address, empty,
          static_cast<uint8_t>(AttrNumber(node, "type", 0.0)),
          node->builder_child_count, node->builder_child_height,
          style.padding[0], style.padding[1], style.padding[2], style.padding[3],
          node->builder_background,
          reinterpret_cast<void*>(&OnBuilderBuildChild),
          reinterpret_cast<void*>(&OnBuilderDisposeChild));
    }
    if (Dart_IsError(builder) || Dart_IsNull(builder)) {
      ReportDartError("appservice NewListBuilder/NewGridBuilder", builder);
      return Dart_Null();
    }
    node->builder_p = Dart_NewPersistentHandle(builder);
    g_builders_by_address[node->address] = node;
    std::fprintf(stderr,
                 "[skyline-runtime] builder #%u: %s count=%d item=%gx%g\n",
                 node->id, node->builder_kind == 1 ? "grid" : "list",
                 node->builder_child_count,
                 static_cast<double>(node->measured_w),
                 static_cast<double>(node->builder_child_height));
    // Slivers are consumed by the ancestor scroll view; do not wrap in a
    // YGNode (a ParentDataWidget under a sliver is invalid).
    return builder;
  } else if (IsSwiperNode(node)) {
    auto* newSwiper = g_page != nullptr && g_page->holder != nullptr
                          ? reinterpret_cast<NewSwiperFn>(FindCallback(
                                g_page->holder->ffi_table, kNewSwiperHash))
                          : nullptr;
    if (newSwiper == nullptr) return Dart_Null();
    std::vector<Dart_Handle> item_widgets;
    item_widgets.reserve(node->children.size());
    for (AppNode* child : node->children) {
      if (child == nullptr || child->released || child->style.display_none) {
        continue;
      }
      // PageView positions the pages itself; item YGNodes must be laid out at
      // the origin (the page-wide pass positioned them side by side).
      MeasureNode(child, node->measured_w, true, node->measured_h, true);
      std::vector<NodeMetric> item_metrics;
      LayoutNode(child, 0.0f, 0.0f, child->measured_w, child->measured_h,
                 &item_metrics, child->abs_x, child->abs_y);
      child->sliver_layout = true;
      WriteLayoutMetrics(item_metrics);
      Dart_Handle widget =
          BuildWidget(child, newYGNode, newYGConnector, newText, newTextSpan,
                      newDecoratedBox, newOpacity, newScrollView, newListView,
                      newGridView, newDirectRenderWidget, newListBuilder,
                      newGridBuilder, true);
      if (widget != nullptr && !Dart_IsError(widget)) {
        item_widgets.push_back(widget);
      }
    }
    Dart_Handle children =
        Dart_NewList(static_cast<intptr_t>(item_widgets.size()));
    for (size_t i = 0; i < item_widgets.size(); i++) {
      Dart_ListSetAt(children, static_cast<intptr_t>(i), item_widgets[i]);
    }
    const bool vertical = AttrBool(node, "vertical", false);
    uint32_t dot_color =
        ParseColorString(AttrText(node, "indicatorColor", "").c_str(),
                         0x4D000000u);
    uint32_t active_color =
        ParseColorString(AttrText(node, "indicatorActiveColor", "").c_str(),
                         0xFFFFFFFFu);
    double previous_margin = 0.0;
    double next_margin = 0.0;
    {
      const std::string raw = AttrText(node, "previousMargin", "");
      if (!raw.empty()) ParsePxToken(raw, &previous_margin);
    }
    {
      const std::string raw = AttrText(node, "nextMargin", "");
      if (!raw.empty()) ParsePxToken(raw, &next_margin);
    }
    const std::string easing = AttrText(node, "easingFunction", "default");
    Dart_Handle easing_handle = Dart_NewStringFromCString(easing.c_str());
    // The handler id is the node id; package:skyline reports it back through
    // ShouldGestureResponse so the nested scroll view can yield at its edges.
    NativeGestureRecognizerNative recognizer =
        MakeSwiperGestureRecognizer(node->id, vertical);
    Dart_Handle swiper = newSwiper(
        node->address, 0 /* layoutType */, 0 /* transformerType */,
        0 /* indicatorType */, AttrBool(node, "indicatorDots", false) ? 1 : 0,
        0.0f, 0.0f,
        // Unset alignment: SkylineSwiper only accepts [-1, 1]; NaN keeps its
        // default (bottom-centre for horizontal, right-centre for vertical).
        std::nan(""), std::nan(""), 3.0f, 6.0f, 3.0f, 6.0f, 6.0f, dot_color,
        active_color, vertical ? 1 : 0, vertical ? 1 : 3,
        AttrBool(node, "circular", false) ? 1 : 0,
        AttrBool(node, "autoplay", false) ? 1 : 0,
        static_cast<uint32_t>(AttrNumber(node, "interval", 5000.0)),
        static_cast<uint32_t>(AttrNumber(node, "duration", 500.0)),
        easing_handle,
        static_cast<uint32_t>(AttrNumber(node, "current", 0.0)), 0.0f,
        static_cast<float>(std::max(
            1.0, AttrNumber(node, "displayMultipleItems", 1.0))),
        AttrBool(node, "snapToEdge", false) ? 1 : 0,
        static_cast<float>(previous_margin),
        static_cast<float>(next_margin),
        const_cast<NativeGestureRecognizerNative*>(&recognizer), 1,
        reinterpret_cast<void*>(&SwiperOnChange),
        reinterpret_cast<void*>(&SwiperOnScroll), children);
    if (Dart_IsError(swiper)) {
      ReportDartError("appservice NewSwiper", swiper);
      return Dart_Null();
    }
    node->swiper_p = Dart_NewPersistentHandle(swiper);
    InstallSwiperCallbacks(node);
    std::fprintf(stderr,
                 "[skyline-runtime] swiper #%u: vertical=%d current=%g "
                 "items=%zu\n",
                 node->id, vertical ? 1 : 0,
                 AttrNumber(node, "current", 0.0), item_widgets.size());
    inner = swiper;
  } else if (IsPickerViewColumnNode(node)) {
    auto* newPickerColumn =
        g_page != nullptr && g_page->holder != nullptr
            ? reinterpret_cast<NewPickerViewColumnFn>(
                  FindCallback(g_page->holder->ffi_table,
                               kNewPickerViewColumnHash))
            : nullptr;
    if (newPickerColumn == nullptr) return Dart_Null();
    const double indicator_height =
        AttrNumber(node, "indicatorHeight", 50.0);
    std::vector<Dart_Handle> item_widgets;
    item_widgets.reserve(node->children.size());
    for (AppNode* child : node->children) {
      if (child == nullptr || child->released || child->style.display_none) {
        continue;
      }
      // ListWheel positions items at `itemExtent` steps; lay each item out at
      // the origin with the indicator height as its slot.
      MeasureNode(child, node->measured_w, true,
                  static_cast<float>(indicator_height), true);
      std::vector<NodeMetric> item_metrics;
      LayoutNode(child, 0.0f, 0.0f, child->measured_w,
                 static_cast<float>(indicator_height), &item_metrics,
                 child->abs_x, child->abs_y);
      child->sliver_layout = true;
      WriteLayoutMetrics(item_metrics);
      Dart_Handle widget =
          BuildWidget(child, newYGNode, newYGConnector, newText, newTextSpan,
                      newDecoratedBox, newOpacity, newScrollView, newListView,
                      newGridView, newDirectRenderWidget, newListBuilder,
                      newGridBuilder, true);
      if (widget != nullptr && !Dart_IsError(widget)) {
        item_widgets.push_back(widget);
      }
    }
    Dart_Handle children =
        Dart_NewList(static_cast<intptr_t>(item_widgets.size()));
    for (size_t i = 0; i < item_widgets.size(); i++) {
      Dart_ListSetAt(children, static_cast<intptr_t>(i), item_widgets[i]);
    }
    Dart_Handle column = newPickerColumn(
        node->address,
        static_cast<uint32_t>(AttrNumber(node, "value", 0.0)),
        static_cast<float>(indicator_height), 0.0, 0.0, 0.0, 0.0, 0u, 0u, 0u,
        0u, 0, 0, 0, 0, style.has_background ? style.background : 0u,
        AttrBool(node, "immediateChange", false) ? 1 : 0,
        AttrBool(node, "magnifier", true) ? 1 : 0,
        AttrNumber(node, "magnification", 1.0),
        AttrNumber(node, "overAndUnderCenterOpacity", 0.447),
        reinterpret_cast<void*>(&PickerColumnOnChange),
        reinterpret_cast<void*>(&PickerColumnOnStart),
        reinterpret_cast<void*>(&PickerColumnOnEnd), children);
    if (Dart_IsError(column)) {
      ReportDartError("appservice NewPickerViewColumn", column);
      return Dart_Null();
    }
    node->picker_column_p = Dart_NewPersistentHandle(column);
    InstallPickerColumnCallbacks(node);
    std::fprintf(stderr,
                 "[skyline-runtime] picker column #%u: current=%g items=%zu\n",
                 node->id, AttrNumber(node, "value", 0.0),
                 item_widgets.size());
    inner = column;
  } else if (IsSelectionNode(node)) {
    auto* newSelection = g_page != nullptr && g_page->holder != nullptr
                             ? reinterpret_cast<NewSelectionFn>(FindCallback(
                                   g_page->holder->ffi_table, kNewSelectionHash))
                             : nullptr;
    if (newSelection == nullptr) return Dart_Null();
    Dart_Handle child = Dart_Null();
    for (AppNode* item : node->children) {
      if (item == nullptr || item->released || item->style.display_none) {
        continue;
      }
      Dart_Handle widget =
          BuildWidget(item, newYGNode, newYGConnector, newText, newTextSpan,
                      newDecoratedBox, newOpacity, newScrollView, newListView,
                      newGridView, newDirectRenderWidget, newListBuilder,
                      newGridBuilder, true);
      if (widget != nullptr && !Dart_IsError(widget) && !Dart_IsNull(widget)) {
        child = widget;
        break;
      }
    }
    const uint32_t handle_color =
        ParseColorString(AttrText(node, "selectionHandleColor", "").c_str(),
                         0u);
    const uint32_t selection_color =
        ParseColorString(AttrText(node, "selectionColor", "").c_str(), 0u);
    Dart_Handle selection = newSelection(
        node->address, AttrBool(node, "disableContextMenu", false) ? 1 : 0,
        reinterpret_cast<void*>(&SelectionOnChangeCallback), handle_color,
        selection_color, child);
    if (Dart_IsError(selection)) {
      ReportDartError("appservice NewSelection", selection);
      return Dart_Null();
    }
    node->selection_p = Dart_NewPersistentHandle(selection);
    InstallSelectionCallbacks(node);
    inner = selection;
  } else {
    const bool is_scroll_view = IsScrollViewName(node->name) &&
                                newScrollView != nullptr &&
                                !IsScrollViewComponentWrapper(node);
    const bool is_list_view =
        IsListViewName(node->name) && newListView != nullptr;
    const bool is_grid_view =
        IsGridViewName(node->name) && newGridView != nullptr;
    if (is_list_view || is_grid_view) {
      // `<list-view>`/`<grid-view>` produce a virtualized sliver.  Its items
      // are top-level YGNodes (a sliver cannot host a ParentDataWidget).
      std::vector<AppNode*> items;
      CollectListItems(node, &items);
      const int grid_type = GridIsMasonry(node) ? 0 : 1;
      int cross_axis_count = std::max(
          1, static_cast<int>(AttrNumber(node, "crossAxisCount", 2.0)));
      const double grid_main_spacing = GridMainGap(node);
      const double grid_cross_spacing = GridCrossGap(node);
      float slot_width = node->measured_w > 0.0f ? node->measured_w : 0.0f;
      if (is_grid_view) {
        // Match Flutter's delegate (fixed count, or derived from
        // maxCrossAxisExtent) and the page-wide packing pass.
        cross_axis_count = GridColumnCount(node, ContentBoxWidth(node));
        slot_width = GridSlotWidth(node, cross_axis_count);
        if (slot_width <= 0.0f) slot_width = node->measured_w;
      }
      std::vector<Dart_Handle> item_widgets;
      item_widgets.reserve(items.size());
      for (AppNode* item : items) {
        // Flutter's slivers position items themselves, so lay each item's
        // subtree out at the origin (the page-wide pass gave it an absolute y).
        if (slot_width > 0.0f) {
          MeasureNode(item, slot_width, true, kInf, false);
        }
        std::vector<NodeMetric> item_metrics;
        LayoutNode(item, 0.0f, 0.0f, item->measured_w, item->measured_h,
                   &item_metrics, item->abs_x, item->abs_y);
        item->sliver_layout = true;
        WriteLayoutMetrics(item_metrics);
        Dart_Handle widget =
            BuildWidget(item, newYGNode, newYGConnector, newText, newTextSpan,
                        newDecoratedBox, newOpacity, newScrollView, newListView,
                        newGridView, newDirectRenderWidget, newListBuilder,
                        newGridBuilder, true);
        if (widget != nullptr && !Dart_IsError(widget)) {
          item_widgets.push_back(widget);
        }
      }
      Dart_Handle child_list =
          Dart_NewList(static_cast<intptr_t>(item_widgets.size()));
      for (size_t i = 0; i < item_widgets.size(); i++) {
        Dart_ListSetAt(child_list, static_cast<intptr_t>(i), item_widgets[i]);
      }
      Dart_Handle sliver = Dart_Null();
      if (is_list_view) {
        sliver = newListView(node->address, child_list, style.padding[0],
                             style.padding[1], style.padding[2], style.padding[3],
                             style.has_background ? style.background : 0u,
                             nullptr, nullptr);
      } else {
        sliver = newGridView(
            node->address, child_list, static_cast<uint8_t>(grid_type),
            cross_axis_count, static_cast<float>(grid_main_spacing),
            static_cast<float>(grid_cross_spacing),
            static_cast<float>(AttrNumber(node, "maxCrossAxisExtent", 0.0)),
            style.padding[0], style.padding[1], style.padding[2], style.padding[3],
            style.has_background ? style.background : 0u);
      }
      if (Dart_IsError(sliver)) {
        ReportDartError("appservice list/grid view", sliver);
        sliver = Dart_Null();
      }
      inner = sliver;

      const bool inside_scroll = InsideSliverContext(node);
      if (std::getenv("SKYLINE_APP_SERVICE_DEBUG") != nullptr) {
        const float first_h = items.empty() ? -1.0f : items[0]->measured_h;
        const float first_w = items.empty() ? -1.0f : items[0]->measured_w;
        std::fprintf(stderr,
                     "[ops-debug] %s #%u: %zu items inside=%d first=%gx%g\n",
                     is_list_view ? "listview" : "gridview", node->id,
                     item_widgets.size(), inside_scroll ? 1 : 0,
                     static_cast<double>(first_w),
                     static_cast<double>(first_h));
      }
      if (inside_scroll) {
        // The parent scroll view consumes this widget as a sliver directly.
        return inner;
      }
      // Standalone list/grid: host the sliver in a customScrollView so it can
      // scroll like a regular scroll-view.
      if (newScrollView != nullptr && !Dart_IsError(inner)) {
        Dart_Handle children = Dart_NewList(1);
        Dart_ListSetAt(children, 0, inner);
        NativeGestureRecognizerNative recognizer =
            MakeScrollGestureRecognizer(node->id, false);
        Dart_Handle scroll = newScrollView(
            node->address, node->id, 3 /* ScrollViewType.custom */, 0, 1, 0.0,
            0.0, 0, 50.0, 50.0, 0, 0.0, 0u, 0, 0, 0, 0, 0, 0, 0, 150.0, 80.0,
            1,
            1, 0, 0, 0, 0.0,
            const_cast<NativeGestureRecognizerNative*>(&recognizer), 1,
            Dart_Null(), 0, 0.0, style.padding[0], style.padding[1],
            style.padding[2], style.padding[3], 0, 0, 1, 0,
            reinterpret_cast<void*>(&ScrollNoopContentSize),
            reinterpret_cast<void*>(&ScrollStartEvent),
            reinterpret_cast<void*>(&ScrollEvent),
            reinterpret_cast<void*>(&ScrollEndEvent),
            reinterpret_cast<void*>(&ScrollNoopPtr),
            reinterpret_cast<void*>(&ScrollNoopPtr),
            reinterpret_cast<void*>(&RefresherPulling),
            reinterpret_cast<void*>(&RefresherWillRefresh),
            reinterpret_cast<void*>(&RefresherRefresh),
            reinterpret_cast<void*>(&RefresherRestore),
            reinterpret_cast<void*>(&RefresherAbort),
            reinterpret_cast<void*>(&RefresherStatusChange),
            reinterpret_cast<void*>(&ScrollNoopPtr), children);
        if (Dart_IsError(scroll)) {
          ReportDartError("appservice standalone list scroll view", scroll);
        } else {
          inner = scroll;
          node->scroll_view_p = Dart_NewPersistentHandle(scroll);
          InstallScrollCallbacks(node);
        }
      }
    } else {
      std::vector<Dart_Handle> child_widgets;
      std::vector<AppNode*> child_nodes;
      child_widgets.reserve(node->children.size());
      child_nodes.reserve(node->children.size());
      bool has_sliver_child = false;
      bool has_sticky_child = false;
      bool has_sticky_section_child = false;
      NewStickyHeaderFn scroll_sticky_header = nullptr;
      NewStickySectionFn scroll_sticky_section = nullptr;
      if (is_scroll_view && g_page != nullptr && g_page->holder != nullptr) {
        scroll_sticky_header = reinterpret_cast<NewStickyHeaderFn>(
            FindCallback(g_page->holder->ffi_table, kNewStickyHeaderHash));
        scroll_sticky_section = reinterpret_cast<NewStickySectionFn>(
            FindCallback(g_page->holder->ffi_table, kNewStickySectionHash));
      }
      for (AppNode* child : node->children) {
        if (child->released || child->style.display_none) continue;
        // The custom refresher header is passed to the inner `scrollview` as
        // NewScrollView's refresherHeader, not rendered as a wrapper sibling.
        if (IsScrollViewComponentWrapper(node) && IsRefresherHeaderNode(child)) {
          continue;
        }
        // position:fixed is promoted to the page root connector so scrolling
        // ancestors do not move it.
        if (DeferFixedChild(child)) continue;
        if (IsListViewName(child->name) || IsGridViewName(child->name) ||
            IsBuilderName(child->name)) {
          has_sliver_child = true;
        }
        // A section container with sticky children becomes a StickySection
        // sliver: sticky children are pinned headers, the rest are adapters.
        // Scroll views are built normally and handle their own children.
        if (scroll_sticky_header != nullptr && scroll_sticky_section != nullptr &&
            !IsScrollViewName(child->name) && !IsListViewName(child->name) &&
            !IsGridViewName(child->name) && !IsBuilderName(child->name) &&
            HasStickyFlowChildren(child)) {
          std::vector<Dart_Handle> grandchildren;
          std::vector<AppNode*> flow = OrderedFlowChildren(child);
          if (std::getenv("SKYLINE_APP_SERVICE_DEBUG") != nullptr) {
            std::fprintf(stderr,
                         "[ops-debug] sticky section #%u flow=%zu children=",
                         child->id, flow.size());
            for (AppNode* grand : flow) {
              std::fprintf(stderr, "#%u/%s/%s ", grand->id, grand->tag.c_str(),
                           grand->name.c_str());
            }
            std::fprintf(stderr, "\n");
          }
          for (AppNode* grand : flow) {
            // Box children become SliverToBoxAdapters; Flutter stacks them at
            // their own offsets, so re-layout them at the sliver origin.
            if (!IsListViewName(grand->name) &&
                !IsGridViewName(grand->name) && !IsBuilderName(grand->name)) {
              ResetNodeLayoutToOrigin(grand);
            }
            Dart_Handle item =
                BuildWidget(grand, newYGNode, newYGConnector, newText,
                            newTextSpan, newDecoratedBox, newOpacity,
                            newScrollView, newListView, newGridView,
                            newDirectRenderWidget, newListBuilder,
                            newGridBuilder, true);
            if (item == nullptr || Dart_IsError(item) || Dart_IsNull(item)) {
              if (std::getenv("SKYLINE_APP_SERVICE_DEBUG") != nullptr) {
                std::fprintf(stderr,
                             "[ops-debug] sticky grand #%u/%s build failed\n",
                             grand->id, grand->name.c_str());
              }
              continue;
            }
            if (grand->style.position == 3 || IsStickyHeaderNode(grand)) {
              float offset_top = 0.0f;
              if (!std::isnan(grand->style.inset[0])) {
                offset_top = grand->style.inset[0];
              }
              Dart_Handle header = scroll_sticky_header(
                  grand->address, item, 0.0, 0.0, 0.0, 0.0, offset_top, 0,
                  reinterpret_cast<void*>(&OnStickyHeaderStickOnTop));
              if (!Dart_IsError(header)) item = header;
            }
            grandchildren.push_back(item);
          }
          Dart_Handle section_children =
              Dart_NewList(static_cast<intptr_t>(grandchildren.size()));
          for (size_t i = 0; i < grandchildren.size(); i++) {
            Dart_ListSetAt(section_children, static_cast<intptr_t>(i),
                           grandchildren[i]);
          }
          Dart_Handle section = scroll_sticky_section(
              section_children, 1, child->style.padding[0],
              child->style.padding[1], child->style.padding[2],
              child->style.padding[3]);
          if (!Dart_IsError(section)) {
            if (std::getenv("SKYLINE_APP_SERVICE_DEBUG") != nullptr) {
              std::fprintf(stderr,
                           "[ops-debug] sticky section #%u ok n=%zu\n",
                           child->id, grandchildren.size());
            }
            child_widgets.push_back(section);
            child_nodes.push_back(nullptr);
            has_sticky_child = true;
            has_sticky_section_child = true;
            has_sliver_child = true;
            continue;
          }
          ReportDartError("appservice NewStickySection", section);
        }
        // A scroll view's children live under Flutter's ListView, not under a
        // YGConnector, so they must be top-level YGNodes (no ParentDataWidget).
        // Slivers position those boxes themselves, so their layout buffers
        // must be origin-relative rather than page-relative.
        if (is_scroll_view && !IsListViewName(child->name) &&
            !IsGridViewName(child->name) && !IsBuilderName(child->name)) {
          ResetNodeLayoutToOrigin(child);
        }
        Dart_Handle widget =
            BuildWidget(child, newYGNode, newYGConnector, newText, newTextSpan,
                        newDecoratedBox, newOpacity, newScrollView, newListView,
                        newGridView, newDirectRenderWidget, newListBuilder,
                        newGridBuilder, is_scroll_view);
        if (widget == nullptr) continue;
        child_widgets.push_back(widget);
        child_nodes.push_back(child);
        if (child->style.position == 3 || IsStickyHeaderNode(child)) {
          has_sticky_child = true;
        }
      }
      // Fixed nodes were skipped above.  At the page root they become direct
      // children of the root connector and use viewport-relative layout.
      if (node == g_fixed_root_node && g_collect_fixed_nodes) {
        for (size_t i = 0; i < g_deferred_fixed_nodes.size(); i++) {
          AppNode* fixed = g_deferred_fixed_nodes[i];
          if (fixed == nullptr || fixed->released ||
              fixed->style.display_none) {
            continue;
          }
          std::vector<NodeMetric> fixed_metrics;
          fixed_metrics.push_back(NodeMetric{fixed, fixed->abs_x, fixed->abs_y,
                                             fixed->measured_w,
                                             fixed->measured_h});
          WriteLayoutMetrics(fixed_metrics);
          Dart_Handle widget =
              BuildWidget(fixed, newYGNode, newYGConnector, newText,
                          newTextSpan, newDecoratedBox, newOpacity, newScrollView,
                          newListView, newGridView, newDirectRenderWidget,
                          newListBuilder, newGridBuilder, false);
          if (widget != nullptr && !Dart_IsError(widget)) {
            child_widgets.push_back(widget);
          }
        }
      }
      if (is_scroll_view) {
        // Sticky children become pinned-header slivers inside a StickySection,
        // which requires a CustomScrollView (type 3).
        Dart_Handle list = Dart_Null();
        uint8_t scroll_type = has_sliver_child ? 3 : 0;
        if (!has_sliver_child) {
          // The WXML `type` attribute selects how SkylineScrollView consumes
          // its children.  Type `none`/`flex` intentionally renders only the
          // first child for legacy compatibility, so a multi-child
          // `<scroll-view type="list">` must be forwarded as list (2) and a
          // `custom` scroll view as CustomScrollView (3).
          const std::string scroll_type_attr = AttrText(node, "type", "");
          if (scroll_type_attr == "list") {
            scroll_type = 2;
          } else if (scroll_type_attr == "flex") {
            scroll_type = 1;
          } else if (scroll_type_attr == "custom") {
            scroll_type = 3;
          } else if (scroll_type_attr == "nested") {
            scroll_type = 4;
          }
        }
        if (has_sticky_child && !has_sticky_section_child) {
          if (scroll_sticky_header != nullptr &&
              scroll_sticky_section != nullptr) {
            Dart_Handle slivers = Dart_NewList(
                static_cast<intptr_t>(child_widgets.size()));
            for (size_t i = 0; i < child_widgets.size(); i++) {
              Dart_Handle item = child_widgets[i];
              AppNode* child = child_nodes[i];
              if (child != nullptr &&
                  (child->style.position == 3 || IsStickyHeaderNode(child))) {
                float offset_top = 0.0f;
                if (!std::isnan(child->style.inset[0])) {
                  offset_top = child->style.inset[0];
                }
                Dart_Handle header = scroll_sticky_header(
                    child->address, item, 0.0, 0.0, 0.0, 0.0, offset_top, 0,
                    reinterpret_cast<void*>(&OnStickyHeaderStickOnTop));
                if (Dart_IsError(header)) {
                  ReportDartError("appservice NewStickyHeader", header);
                } else {
                  item = header;
                }
              }
              Dart_ListSetAt(slivers, static_cast<intptr_t>(i), item);
            }
            Dart_Handle section =
                scroll_sticky_section(slivers, 1, 0.0, 0.0, 0.0, 0.0);
            if (Dart_IsError(section)) {
              ReportDartError("appservice NewStickySection", section);
            } else {
              list = Dart_NewList(1);
              Dart_ListSetAt(list, 0, section);
              scroll_type = 3;
            }
          }
        } else if (has_sticky_section_child) {
          scroll_type = 3;
        }
        if (Dart_IsNull(list)) {
          list = Dart_NewList(static_cast<intptr_t>(child_widgets.size()));
          for (size_t i = 0; i < child_widgets.size(); i++) {
            Dart_ListSetAt(list, static_cast<intptr_t>(i), child_widgets[i]);
          }
        }
        const bool scroll_x_attr = AttrBool(node, "scrollX", false);
        const bool scroll_y_attr = AttrBool(node, "scrollY", false);
        // List/grid slivers require a CustomScrollView; plain box children are
        // wrapped in SliverToBoxAdapter by SkylineScrollView.
        if (std::getenv("SKYLINE_APP_SERVICE_DEBUG") != nullptr) {
          std::fprintf(stderr,
                       "[ops-debug] scrollview #%u scrollX=%d scrollY=%d "
                       "scrollEnabled=%d type=%u (slivers=%d sticky=%d) "
                       "children=%zu top=%g\n",
                       node->id, scroll_x_attr ? 1 : 0, scroll_y_attr ? 1 : 0,
                       AttrBool(node, "scrollEnabled", true) ? 1 : 0,
                       static_cast<unsigned>(scroll_type),
                       has_sliver_child ? 1 : 0, has_sticky_child ? 1 : 0,
                       child_widgets.size(),
                       AttrNumber(node, "scrollTop", 0.0));
          for (size_t i = 0; i < child_nodes.size() && i < 8; i++) {
            AppNode* child = child_nodes[i];
            std::fprintf(stderr, "[ops-debug]   child[%zu]=%s/%s\n", i,
                         child != nullptr ? child->tag.c_str() : "?",
                         child != nullptr ? child->name.c_str() : "?");
          }
        }
        const std::string refresher_style =
            AttrText(node, "refresherDefaultStyle", "black");
        uint8_t refresher_style_index = 0;
        if (refresher_style == "white") {
          refresher_style_index = 1;
        } else if (refresher_style == "none") {
          refresher_style_index = 2;
        }
        // A custom refresher header (`<view slot="refresher">`) is a sibling of
        // the adapter `scrollview` on the component wrapper.  The base library
        // passes it to NewScrollView as the refresherHeader widget; rendering it
        // as ordinary content pushes the whole page below the header.
        Dart_Handle refresher_header = Dart_Null();
        if (node->parent != nullptr) {
          for (AppNode* sibling : node->parent->children) {
            if (sibling == nullptr || sibling == node || sibling->released ||
                sibling->style.display_none ||
                !IsRefresherHeaderNode(sibling)) {
              continue;
            }
            if (node->measured_w > 0.0f) {
              MeasureNode(sibling, node->measured_w, true, kInf, false);
            }
            std::vector<NodeMetric> header_metrics;
            LayoutNode(sibling, 0.0f, 0.0f, sibling->measured_w,
                       sibling->measured_h, &header_metrics, sibling->abs_x,
                       sibling->abs_y);
            sibling->sliver_layout = true;
            WriteLayoutMetrics(header_metrics);
            Dart_Handle header_widget = BuildWidget(
                sibling, newYGNode, newYGConnector, newText, newTextSpan,
                newDecoratedBox, newOpacity, newScrollView, newListView,
                newGridView, newDirectRenderWidget, newListBuilder,
                newGridBuilder, true);
            if (header_widget != nullptr && !Dart_IsError(header_widget) &&
                !Dart_IsNull(header_widget)) {
              refresher_header = header_widget;
              if (std::getenv("SKYLINE_APP_SERVICE_DEBUG") != nullptr) {
                std::fprintf(stderr,
                             "[ops-debug] scrollview #%u refresher header #%u\n",
                             node->id, sibling->id);
              }
            }
            break;
          }
        }
        NativeGestureRecognizerNative recognizer =
            MakeScrollGestureRecognizer(node->id, scroll_x_attr);
        Dart_Handle scroll = newScrollView(
            node->address, node->id, scroll_type, scroll_x_attr ? 1 : 0,
            scroll_y_attr ? 1 : 0, AttrNumber(node, "scrollTop", 0.0),
            AttrNumber(node, "scrollLeft", 0.0), 0,
            AttrNumber(node, "lowerThreshold", 50.0),
            AttrNumber(node, "upperThreshold", 50.0),
            AttrBool(node, "refresherEnabled", false) ? 1 : 0,
            AttrNumber(node, "refresherThreshold", 50.0),
            ParseColorString(AttrText(node, "refresherBackground", "").c_str(),
                             0u),
            refresher_style_index,
            AttrBool(node, "refresherTriggered", false) ? 1 : 0, 0, 0, 0,
            AttrBool(node, "twoLevelEnabled", false) ? 1 : 0,
            AttrBool(node, "twoLevelTriggered", false) ? 1 : 0,
            AttrNumber(node, "twoLevelThreshold", 150.0),
            AttrNumber(node, "twoLevelCloseThreshold", 80.0),
            AttrBool(node, "scrollEnabled", true) ? 1 : 0,
            AttrBool(node, "bounces", true) ? 1 : 0, 0, 0, 0, 0.0,
            const_cast<NativeGestureRecognizerNative*>(&recognizer), 1,
            refresher_header, 0, 0.0, 0.0, 0.0, 0.0, 0.0, 0,
            0 /* paint order */, 1 /* scrollAnchoring */,
            0 /* innerCorrectionMode */,
            reinterpret_cast<void*>(&ScrollNoopContentSize),
            reinterpret_cast<void*>(&ScrollStartEvent),
            reinterpret_cast<void*>(&ScrollEvent),
            reinterpret_cast<void*>(&ScrollEndEvent),
            reinterpret_cast<void*>(&ScrollNoopPtr),
            reinterpret_cast<void*>(&ScrollNoopPtr),
            reinterpret_cast<void*>(&RefresherPulling),
            reinterpret_cast<void*>(&RefresherWillRefresh),
            reinterpret_cast<void*>(&RefresherRefresh),
            reinterpret_cast<void*>(&RefresherRestore),
            reinterpret_cast<void*>(&RefresherAbort),
            reinterpret_cast<void*>(&RefresherStatusChange),
            reinterpret_cast<void*>(&ScrollNoopPtr), list);
        if (Dart_IsError(scroll)) {
          ReportDartError("appservice NewScrollView", scroll);
        } else {
          inner = scroll;
          node->scroll_view_p = Dart_NewPersistentHandle(scroll);
          InstallScrollCallbacks(node);
        }
      } else if (!child_widgets.empty()) {
        Dart_Handle list =
            Dart_NewList(static_cast<intptr_t>(child_widgets.size()));
        for (size_t i = 0; i < child_widgets.size(); i++) {
          Dart_ListSetAt(list, static_cast<intptr_t>(i), child_widgets[i]);
        }
        // `z-index` only takes effect when the connector paints through
        // StackingRenderBoxObjectMixin; otherwise siblings paint in tree order
        // (e.g. the previewer's white preview list covered its navigation bar).
        bool stacking = false;
        for (AppNode* child : child_nodes) {
          if (child != nullptr && child->style.z_index != 0) {
            stacking = true;
            break;
          }
        }
        Dart_Handle connector = newYGConnector(stacking ? 1 : 0, list);
        if (Dart_IsError(connector)) {
          ReportDartError("appservice NewYGConnector", connector);
        } else {
          inner = connector;
          node->connector = connector;
        }
      }
    }
    inner = WrapVisualStyle(node, inner, newDecoratedBox, newOpacity);
  }

  // SkylineSwiper has no background/border parameters; without this wrapper a
  // CSS background on the swiper element (`.product-list-wrapper{background:
  // white}`) never paints, unlike the official engine.
  if (IsSwiperNode(node)) {
    inner = WrapVisualStyle(node, inner, newDecoratedBox, newOpacity);
  }

  // `concreteWidgetType` drives YGNodeRenderObject.hitTestChildren: containers
  // whose child is a YGConnector must report kYGConnector and pass that
  // connector so shadow hit-testing walks into the child YGNodes.  A plain
  // kYogaNode would stop at the connector's `hitTest` (which only reports
  // `size.contains`), leaving the whole subtree unreachable for gestures.
  constexpr uint32_t kWidgetTypeYogaNode = 1;
  constexpr uint32_t kWidgetTypeYGConnector = 3;
  constexpr uint32_t kWidgetTypeScrollView = 13;
  constexpr uint32_t kWidgetTypeTexture = 15;
  uint32_t concrete_type = kWidgetTypeYogaNode;
  Dart_Handle yoga_connector = Dart_Null();
  if (node->tag != "text") {
    if (node->tag == "texture") {
      concrete_type = kWidgetTypeTexture;
    } else if (node->name == "scrollview" && newScrollView != nullptr) {
      concrete_type = kWidgetTypeScrollView;
    } else if (node->connector != nullptr) {
      concrete_type = kWidgetTypeYGConnector;
      yoga_connector = node->connector;
    }
  }

  if (IsShareElementNode(node)) {
    inner = WrapHero(node, inner);
  }
  if (IsSnapshotNode(node)) {
    inner = WrapSnapshot(node, inner);
  }
  // Accessibility wrapper sits outside the visual wrappers so the semantics
  // node covers the whole element.
  inner = WrapSemantics(node, inner);

  Dart_Handle buffer = node->layout_buffer_p != nullptr
                           ? Dart_HandleFromPersistent(node->layout_buffer_p)
                           : node->layout_buffer;
  if (buffer == nullptr) {
    buffer = Dart_NewTypedData(Dart_TypedData_kFloat32, 9);
    if (Dart_IsError(buffer)) {
      ReportDartError("appservice typed data", buffer);
      return Dart_Null();
    }
  }
  Dart_Handle yoga_node = newYGNode(
      node->address, buffer, is_root ? 1 : 0, 0, 0, 0, concrete_type,
      Dart_Null(), 0.0, 0.0, 0.0, 0.0, 1, 0,
      style.pointer_events_none ? 1 : 0, Dart_Null(), yoga_connector, inner);
  if (style.pointer_events_none &&
      std::getenv("SKYLINE_APP_SERVICE_DEBUG") != nullptr) {
    std::fprintf(stderr, "[skyline-runtime] pointer-events:none id=%u addr=0x%lx\n",
                 node->id, static_cast<long>(node->address));
  }
  if (Dart_IsError(yoga_node)) {
    ReportDartError("appservice NewYGNode", yoga_node);
    return Dart_Null();
  }
  // CSS transform: hand the column-major matrix to the YGNode and rotate/scale
  // around the element centre (CSS transform-origin default).
  if (style.has_transform && g_page != nullptr && g_page->holder != nullptr) {
    auto* setup = reinterpret_cast<YGNodeSetupTransformFn>(
        FindCallback(g_page->holder->ffi_table, kYGNodeSetupTransformHash));
    auto* origin = reinterpret_cast<YGNodeTransformOriginFn>(
        FindCallback(g_page->holder->ffi_table, kYGNodeTransformOriginHash));
    Dart_Handle matrix = Dart_NewTypedData(Dart_TypedData_kFloat64, 16);
    if (setup != nullptr && !Dart_IsError(matrix)) {
      Dart_TypedData_Type data_type = Dart_TypedData_kInvalid;
      void* data = nullptr;
      intptr_t length = 0;
      if (!Dart_IsError(Dart_TypedDataAcquireData(matrix, &data_type, &data,
                                                 &length)) &&
          data_type == Dart_TypedData_kFloat64 && length == 16 &&
          data != nullptr) {
        auto* values = static_cast<double*>(data);
        BuildTransformMatrix(style, node->measured_w, node->measured_h,
                             values);
      }
      Dart_TypedDataReleaseData(matrix);
      setup(yoga_node, matrix);
      if (origin != nullptr) {
        double origin_x = 0.0;
        double origin_y = 0.0;
        ResolveTransformOrigin(style, node->measured_w, node->measured_h,
                               &origin_x, &origin_y);
        origin(yoga_node, origin_x, origin_y, -1.0, -1.0);
      }
      if (std::getenv("SKYLINE_APP_SERVICE_DEBUG") != nullptr) {
        std::fprintf(stderr,
                     "[skyline-runtime] transform #%u matrix=[%g,%g,%g,%g "
                     "%g,%g,%g,%g %g,%g,%g,%g %g,%g,%g,%g]\n",
                     node->id, style.transform[0], style.transform[1],
                     style.transform[2], style.transform[3],
                     style.transform[4], style.transform[5],
                     style.transform[6], style.transform[7],
                     style.transform[8], style.transform[9],
                     style.transform[10], style.transform[11],
                     style.transform[12], style.transform[13],
                     style.transform[14], style.transform[15]);
      }
      node->transform_p = Dart_NewPersistentHandle(matrix);
    }
  }
  node->widget = yoga_node;
  node->yoga_node_p = Dart_NewPersistentHandle(yoga_node);
  return yoga_node;
}

// -- list/grid builder incremental mount -------------------------------------

// Finds the captured node that backs `index`.  Items may declare an explicit
// `index` attribute; otherwise the captured order is the index order.

}  // namespace appservice

