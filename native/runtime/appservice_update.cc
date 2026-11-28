// Style diffing, targeted updates, page rebuilds and transitions.

#include "internal/skyline_runtime_module.hh"
#include "internal/skyline_runtime_update.hh"

namespace appservice {

void DebugDumpNode(const AppNode* node, int depth) {
  if (node == nullptr) return;
  std::string dump_text;
  if (node->tag == "text") {
    dump_text = NodeNormalizedText(node);
    for (char& c : dump_text) {
      if (c == '\n' || c == '\r') c = ' ';
    }
  }
  SKY_LOG(kDebug, kLogOps,
          "%*s#%u tag=%s name=%s classes=%zu style(w=%g h=%g pctw=%g pcth=%g "
          "pad=%g,%g,%g,%g margin=%g,%g,%g,%g row=%d justify=%d align=%d "
          "bg=%08x border=%g font=%g) measured=%gx%g address=0x%lx text=\"%s\"",
          depth * 2, "", node->id, node->tag.c_str(), node->name.c_str(),
          node->classes.size(), static_cast<double>(node->style.width),
          static_cast<double>(node->style.height),
          static_cast<double>(node->style.width_percent),
          static_cast<double>(node->style.height_percent),
          static_cast<double>(node->style.padding[0]),
          static_cast<double>(node->style.padding[1]),
          static_cast<double>(node->style.padding[2]),
          static_cast<double>(node->style.padding[3]),
          static_cast<double>(node->style.margin[0]),
          static_cast<double>(node->style.margin[1]),
          static_cast<double>(node->style.margin[2]),
          static_cast<double>(node->style.margin[3]), node->style.row,
          node->style.justify, node->style.align,
          node->style.has_background ? node->style.background : 0u,
          static_cast<double>(node->style.border_width), node->style.font_size,
          static_cast<double>(node->measured_w),
          static_cast<double>(node->measured_h),
          static_cast<long>(node->address), dump_text.c_str());
  for (const AppNode* child : node->children) {
    if (child->released) continue;
    DebugDumpNode(child, depth + 1);
  }
}

// Post-attach updates.
//
// Structural batches (create/append/remove/...) rebuild the root's children and
// swap them through `YGConnectorUpdateChildren`.  Style/text-only batches use
// the fine-grained runtime callbacks (`DecoratedBoxUpdateColor`,
// `DecoratedBoxUpdateBorder`, `OpacityUpdateOpacity`, `TextSpanUpdate*`,
// `TextApplyUpdate`) and re-run the layout pass against the existing buffers.

bool Nearly(float a, float b) {
  return std::fabs(a - b) < 0.01f;
}

bool SameFloatOrNan(float a, float b) {
  if (std::isnan(a) || std::isnan(b)) {
    return std::isnan(a) && std::isnan(b);
  }
  return std::fabs(a - b) < 0.01f;
}

bool SameCssPositionAxis(const CssPositionAxis& a, const CssPositionAxis& b) {
  return std::fabs(a.value - b.value) <= 0.001f && a.percent == b.percent &&
         a.from_end == b.from_end;
}

// Background box properties shared by background images and linear gradients.
bool SameBackgroundBox(const AppStyle& a, const AppStyle& b) {
  return a.background_fit == b.background_fit &&
         a.has_background_size == b.has_background_size &&
         Nearly(a.background_size_width, b.background_size_width) &&
         Nearly(a.background_size_height, b.background_size_height) &&
         a.background_size_width_percent == b.background_size_width_percent &&
         a.background_size_height_percent == b.background_size_height_percent &&
         a.background_repeat == b.background_repeat &&
         SameCssPositionAxis(a.background_position_x, b.background_position_x) &&
         SameCssPositionAxis(a.background_position_y, b.background_position_y);
}

bool SameGradient(const AppStyle& a, const AppStyle& b) {
  if (a.has_gradient != b.has_gradient) return false;
  if (!a.has_gradient) return true;
  if (!SameBackgroundBox(a, b)) return false;
  if (a.gradient_kind != b.gradient_kind) return false;
  if (std::fabs(a.gradient_angle - b.gradient_angle) > 0.001) return false;
  if (a.gradient_colors != b.gradient_colors) return false;
  if (a.gradient_stops.size() != b.gradient_stops.size()) return false;
  if (a.gradient_stop_percent != b.gradient_stop_percent) return false;
  for (size_t i = 0; i < a.gradient_stops.size(); i++) {
    if (std::fabs(a.gradient_stops[i] - b.gradient_stops[i]) > 0.001) {
      return false;
    }
  }
  if (std::fabs(a.gradient_center_x - b.gradient_center_x) > 0.001 ||
      std::fabs(a.gradient_center_y - b.gradient_center_y) > 0.001) {
    return false;
  }
  if (a.gradient_center_x_percent != b.gradient_center_x_percent ||
      a.gradient_center_y_percent != b.gradient_center_y_percent) {
    return false;
  }
  if (std::fabs(a.gradient_radius - b.gradient_radius) > 0.001) return false;
  if (a.gradient_radial_size != b.gradient_radial_size) return false;
  return true;
}

bool StyleNeedsRebuild(const AppNode* node, const AppStyle& before,
                       const AppStyle& after) {
  if (before.display_none != after.display_none) return true;
  if (before.clip != after.clip) return true;
  // softWrap/overflow are baked into the Flutter text widget at creation.
  if (before.white_space != after.white_space ||
      before.text_overflow != after.text_overflow) {
    return true;
  }
  // A different background image URL (or gaining/losing one) needs a fresh
  // decorated-box registration/load; size/position changes repaint in place.
  if (before.background_image != after.background_image) return true;
  // The text widget's fallback chain is fixed at creation time, so a font
  // family change needs a rebuild (font loads themselves use targeted relayout).
  if (before.font_families != after.font_families) return true;
  // Presence changes can be handled in place once the corresponding widget
  // handles already exist (notably during an opacity/background transition).
  if (node->decorated_box_p == nullptr &&
      before.has_background != after.has_background) {
    return true;
  }
  if (node->decorated_box_p == nullptr &&
      HasBorder(before) != HasBorder(after)) {
    return true;
  }
  if (node->opacity_widget_p == nullptr &&
      (before.opacity < 0.999) != (after.opacity < 0.999)) {
    return true;
  }
  if (node->decorated_box_p == nullptr && before.has_shadow != after.has_shadow) {
    return true;
  }
  if (node->decorated_box_p == nullptr &&
      before.has_gradient != after.has_gradient) {
    return true;
  }
  if (node->decorated_box_p == nullptr && RadiiChanged(before, after)) {
    return true;
  }
  return false;
}

void SnapshotNode(AppNode* node) {
  if (node == nullptr) return;
  node->previous_style = node->style;
  node->has_previous_style = true;
  if (node->tag == "text") {
    node->previous_text = NodeNormalizedText(node);
  }
  for (AppNode* child : node->children) SnapshotNode(child);
}

// True when a style diff can change box metrics.  Paint-only changes (colour,
// background, opacity, border colour, shadow, gradient, radius, transform) do
// not require a full layout pass; skipping it keeps class-driven highlight
// updates in the millisecond range on large pages.
bool LayoutAffectingStyleChanged(const AppStyle& a, const AppStyle& b) {
  const auto ne = [](double x, double y) {
    return x != y && !(std::isnan(x) && std::isnan(y));
  };
  if (a.row != b.row || a.justify != b.justify || a.align != b.align ||
      a.align_self != b.align_self || a.wrap != b.wrap ||
      a.align_content != b.align_content || a.order != b.order ||
      a.inline_block != b.inline_block || a.inline_level != b.inline_level ||
      ne(a.gap, b.gap) || ne(a.row_gap, b.row_gap) ||
      ne(a.column_gap, b.column_gap) || a.position != b.position) {
    return true;
  }
  for (int i = 0; i < 4; i++) {
    if (ne(a.padding[i], b.padding[i]) || ne(a.margin[i], b.margin[i]) ||
        ne(a.padding_percent[i], b.padding_percent[i]) ||
        ne(a.margin_percent[i], b.margin_percent[i]) ||
        ne(a.inset[i], b.inset[i]) ||
        ne(a.inset_percent[i], b.inset_percent[i]) ||
        ne(a.side_border_width[i], b.side_border_width[i])) {
      return true;
    }
  }
  if (ne(a.width, b.width) || ne(a.height, b.height) ||
      ne(a.width_percent, b.width_percent) ||
      ne(a.height_percent, b.height_percent) ||
      ne(a.aspect_ratio, b.aspect_ratio) || a.box_sizing != b.box_sizing ||
      ne(a.min_width, b.min_width) || ne(a.max_width, b.max_width) ||
      ne(a.min_height, b.min_height) || ne(a.max_height, b.max_height) ||
      ne(a.flex, b.flex) || a.has_flex != b.has_flex ||
      ne(a.flex_shrink, b.flex_shrink) ||
      a.has_flex_shrink != b.has_flex_shrink ||
      ne(a.flex_basis, b.flex_basis) ||
      ne(a.flex_basis_percent, b.flex_basis_percent) ||
      ne(a.border_width, b.border_width)) {
    return true;
  }
  // Text metrics: content, font size, line height and letter spacing decide
  // the measured text box.
  if (ne(a.font_size, b.font_size) || ne(a.line_height, b.line_height) ||
      ne(a.line_height_factor, b.line_height_factor) ||
      ne(a.letter_spacing, b.letter_spacing) ||
      a.font_families != b.font_families || a.max_lines != b.max_lines ||
      a.white_space != b.white_space) {
    return true;
  }
  return false;
}

bool IsStructuralOpKind(const char* kind) {  return std::strcmp(kind, "createElement") == 0 ||
         std::strcmp(kind, "appendChild") == 0 ||
         std::strcmp(kind, "insertBefore") == 0 ||
         std::strcmp(kind, "removeChild") == 0 ||
         std::strcmp(kind, "replaceChild") == 0 ||
         std::strcmp(kind, "splice") == 0 ||
         std::strcmp(kind, "setScrollTop") == 0 ||
         std::strcmp(kind, "release") == 0;
}

struct TextUpdateCallbacks {
  TextSpanUpdateTextFn text = nullptr;
  TextSpanUpdateTextColorFn color = nullptr;
  TextSpanUpdateFontSizeFn font_size = nullptr;
  TextSpanUpdateFontWeightFn font_weight = nullptr;
  TextSpanUpdateFontStyleFn font_style = nullptr;
  TextSpanUpdateLineHeightFn line_height = nullptr;
  TextSpanUpdateLetterSpacingFn letter_spacing = nullptr;
  TextApplyUpdateFn apply = nullptr;
  TextUpdateTextAlignFn align = nullptr;
  TextUpdateMaxLinesFn max_lines = nullptr;
};

bool ApplyTargetedUpdates(RuntimeHolder* holder) {
  SKY_DCHECK_UI_THREAD();
  if (g_page == nullptr || g_page->root_node == nullptr) return false;
  AppNode* root = FindNode(g_page->root_id);
  if (root == nullptr) return false;

  auto* update_color = reinterpret_cast<DecoratedBoxUpdateColorFn>(
      FindCallback(holder->ffi_table, kDecoratedBoxUpdateColorHash));
  auto* update_border = reinterpret_cast<DecoratedBoxUpdateBorderFn>(
      FindCallback(holder->ffi_table, kDecoratedBoxUpdateBorderHash));
  auto* update_shadow = reinterpret_cast<DecoratedBoxUpdateShadowFn>(
      FindCallback(holder->ffi_table, kDecoratedBoxUpdateShadowHash));
  auto* update_radius = reinterpret_cast<DecoratedBoxUpdateRadiusFn>(
      FindCallback(holder->ffi_table, kDecoratedBoxUpdateBorderSideRadiusHash));
  auto* update_gradient = reinterpret_cast<DecoratedBoxUpdateGradientFn>(
      FindCallback(holder->ffi_table, kDecoratedBoxUpdateGradientHash));
  auto* clear_gradient = reinterpret_cast<DecoratedBoxClearGradientFn>(
      FindCallback(holder->ffi_table, kDecoratedBoxClearGradientHash));
  auto* update_clip_radius = reinterpret_cast<ClipRRectUpdateRadiusFn>(
      FindCallback(holder->ffi_table, kClipRRectUpdateRadiusHash));
  auto* update_opacity = reinterpret_cast<OpacityUpdateFn>(
      FindCallback(holder->ffi_table, kOpacityUpdateHash));
  auto* mark_needs_layout = reinterpret_cast<MarkYGNodeNeedsLayoutFn>(
      FindCallback(holder->ffi_table, kMarkYGNodeNeedsLayoutHash));
  TextUpdateCallbacks text;
  text.text = reinterpret_cast<TextSpanUpdateTextFn>(
      FindCallback(holder->ffi_table, kTextSpanUpdateTextHash));
  text.color = reinterpret_cast<TextSpanUpdateTextColorFn>(
      FindCallback(holder->ffi_table, kTextSpanUpdateTextColorHash));
  text.font_size = reinterpret_cast<TextSpanUpdateFontSizeFn>(
      FindCallback(holder->ffi_table, kTextSpanUpdateFontSizeHash));
  text.font_weight = reinterpret_cast<TextSpanUpdateFontWeightFn>(
      FindCallback(holder->ffi_table, kTextSpanUpdateFontWeightHash));
  text.font_style = reinterpret_cast<TextSpanUpdateFontStyleFn>(
      FindCallback(holder->ffi_table, kTextSpanUpdateFontStyleHash));
  text.line_height = reinterpret_cast<TextSpanUpdateLineHeightFn>(
      FindCallback(holder->ffi_table, kTextSpanUpdateLineHeightHash));
  text.letter_spacing = reinterpret_cast<TextSpanUpdateLetterSpacingFn>(
      FindCallback(holder->ffi_table, kTextSpanUpdateLetterSpacingHash));
  text.apply = reinterpret_cast<TextApplyUpdateFn>(
      FindCallback(holder->ffi_table, kTextApplyUpdateHash));
  text.align = reinterpret_cast<TextUpdateTextAlignFn>(
      FindCallback(holder->ffi_table, kTextUpdateTextAlignHash));
  text.max_lines = reinterpret_cast<TextUpdateMaxLinesFn>(
      FindCallback(holder->ffi_table, kTextUpdateMaxLinesHash));
  if (mark_needs_layout == nullptr) {
    return false;
  }

  ResolveStyles(root, nullptr);
  ComputeInlineWhitespace(root);

  std::vector<Dart_Handle> yoga_nodes;
  bool rebuild = false;
  bool layout_needed = false;
  // Paint-only style updates can still move the published box: animated
  // transforms (half-screen's sheet slides with `translateY`) change the
  // appservice hit-test geometry.  Republish the rect snapshot even when no
  // layout pass is required, otherwise the sheet's hit area stays at its
  // closed position and taps/scroll inside it miss.
  bool rects_dirty = false;

  std::vector<AppNode*> pending;
  pending.push_back(root);
  while (!pending.empty()) {
    AppNode* node = pending.back();
    pending.pop_back();
    for (AppNode* child : node->children) {
      if (!child->released) pending.push_back(child);
    }
    if (!node->has_previous_style) {
      // The node has no baseline to diff against; leave it for the rebuild
      // but keep applying paint updates to the rest of the tree.
      rebuild = true;
      continue;
    }
    if (StyleNeedsRebuild(node, node->previous_style, node->style)) {
      rebuild = true;
      continue;
    }
    const AppStyle& before = node->previous_style;
    const AppStyle& after = node->style;
    if (LayoutAffectingStyleChanged(before, after)) layout_needed = true;

    if (node->tag == "text") {
      const std::string text_value = NodeNormalizedText(node);
      const bool text_changed = text_value != node->previous_text;
      const bool color_changed = before.color != after.color;
      const bool size_changed = std::fabs(before.font_size - after.font_size) > 0.01;
      const bool weight_changed = before.font_weight != after.font_weight;
      const bool style_changed = before.font_style != after.font_style;
      const double before_height =
          before.line_height > 0 ? before.line_height / before.font_size
                                 : before.line_height_factor;
      const double after_height =
          after.line_height > 0 ? after.line_height / after.font_size
                                : after.line_height_factor;
      const bool height_changed = std::fabs(before_height - after_height) > 0.01;
      const bool spacing_changed =
          (std::isnan(before.letter_spacing) != std::isnan(after.letter_spacing)) ||
          (!std::isnan(before.letter_spacing) &&
           !std::isnan(after.letter_spacing) &&
           std::fabs(before.letter_spacing - after.letter_spacing) > 0.01);
      const bool align_changed = before.text_align != after.text_align;
      const bool max_lines_changed = before.max_lines != after.max_lines;
      const bool any = text_changed || color_changed || size_changed ||
                       weight_changed || style_changed || height_changed ||
                       spacing_changed || align_changed || max_lines_changed;
      if (text_changed || size_changed || weight_changed || style_changed ||
          height_changed || spacing_changed) {
        layout_needed = true;
      }
      if (any) {
        if (node->text_span_p == nullptr || node->text_widget_p == nullptr ||
            text.text == nullptr || text.apply == nullptr) {
          // Update the style baseline even though this node has no text widget
          // (raw text runs are painted by their owning <text>).  Without this
          // every transition tick requests another rebuild and the
          // interpolated transform frames are never presented.
          node->previous_style = after;
          node->previous_text = NodeNormalizedText(node);
          rebuild = true;
          continue;
        }
        Dart_Handle span = Dart_HandleFromPersistent(node->text_span_p);
        Dart_Handle widget = Dart_HandleFromPersistent(node->text_widget_p);
        if (text_changed) {
          Dart_Handle value = Dart_NewStringFromCString(text_value.c_str());
          if (!Dart_IsError(value)) text.text(span, value);
        }
        if (color_changed && text.color != nullptr) text.color(span, after.color);
        if (size_changed && text.font_size != nullptr) {
          text.font_size(span, after.font_size);
        }
        if (weight_changed && text.font_weight != nullptr) {
          text.font_weight(span, static_cast<uint8_t>(after.font_weight));
        }
        if (style_changed && text.font_style != nullptr) {
          text.font_style(span, static_cast<uint8_t>(after.font_style));
        }
        if (height_changed && text.line_height != nullptr) {
          text.line_height(span, after_height);
        }
        if (spacing_changed && text.letter_spacing != nullptr) {
          text.letter_spacing(span, std::isnan(after.letter_spacing)
                                        ? std::nan("")
                                        : after.letter_spacing);
        }
        if (align_changed && text.align != nullptr) {
          text.align(widget, static_cast<uint8_t>(after.text_align));
        }
        if (max_lines_changed && text.max_lines != nullptr) {
          text.max_lines(widget, after.max_lines);
        }
        text.apply(widget, 1);
      }
    } else {
      if (node->decorated_box_p != nullptr) {
        Dart_Handle decorated =
            Dart_HandleFromPersistent(node->decorated_box_p);
        // A removed background (e.g. the old A-Z letter losing `.selected`)
        // must clear the existing decorated box; color 0 is transparent.
        if (before.has_background != after.has_background &&
            update_color != nullptr) {
          update_color(decorated, after.has_background ? after.background : 0u);
        } else if (after.has_background &&
                   before.background != after.background &&
                   update_color != nullptr) {
          update_color(decorated, after.background);
        }
        if (BorderSidesChanged(before, after) && update_border != nullptr) {
          double widths[4] = {0, 0, 0, 0};
          uint32_t colors[4] = {0u, 0u, 0u, 0u};
          uint8_t styles[4] = {0, 0, 0, 0};
          for (int side = 0; side < 4; side++) {
            widths[side] = EffectiveBorderWidth(after, side);
            if (widths[side] > 0.0) {
              colors[side] = EffectiveBorderColor(after, side);
              styles[side] = 1;
            }
          }
          update_border(decorated, widths[3], widths[0], widths[1], widths[2],
                        colors[3], colors[0], colors[1], colors[2], styles[3],
                        styles[0], styles[1], styles[2]);
        }
        const bool shadow_changed =
            before.has_shadow != after.has_shadow ||
            before.shadow_color != after.shadow_color ||
            !Nearly(before.shadow_offset_x, after.shadow_offset_x) ||
            !Nearly(before.shadow_offset_y, after.shadow_offset_y) ||
            !Nearly(before.shadow_blur, after.shadow_blur) ||
            !Nearly(before.shadow_spread, after.shadow_spread) ||
            before.shadow_inset != after.shadow_inset;
        if (shadow_changed && update_shadow != nullptr) {
          update_shadow(decorated, BuildBoxShadowList(after));
        }
        if (RadiiChanged(before, after)) {
          if (update_radius == nullptr) {
            rebuild = true;
            continue;
          }
          static const uint8_t kEdges[4] = {5, 6, 8, 7};  // TL,TR,BR,BL
          for (int corner = 0; corner < 4; corner++) {
            const float radius = EffectiveCornerRadius(after, corner);
            const uint8_t is_percent =
                CornerRadiusPercent(after, corner) ? 1 : 0;
            update_radius(decorated, kEdges[corner], radius, radius,
                          is_percent, is_percent);
          }
        }
        if (!SameGradient(before, after)) {
          if (after.has_gradient) {
            if (update_gradient == nullptr) {
              rebuild = true;
              continue;
            }
            update_gradient(decorated, BuildGradientHandle(after));
          } else {
            if (clear_gradient == nullptr) {
              rebuild = true;
              continue;
            }
            clear_gradient(decorated);
          }
        }
        // background-size / position / repeat apply to the natively painted
        // background image; mark it dirty so the onPaint callback re-runs.
        if (!after.background_image.empty() &&
            !SameBackgroundBox(before, after)) {
          auto* mark_paint = reinterpret_cast<void (*)(Dart_Handle)>(
              FindCallback(holder->ffi_table,
                           kBackgroundImageMarkNeedPaintHash));
          if (mark_paint != nullptr) mark_paint(decorated);
        }
      }
      if (node->clip_widget_p != nullptr && RadiiChanged(before, after)) {
        if (update_clip_radius == nullptr) {
          rebuild = true;
          continue;
        }
        const double tl = EffectiveCornerRadius(after, 0);
        const double tr = EffectiveCornerRadius(after, 1);
        const double br = EffectiveCornerRadius(after, 2);
        const double bl = EffectiveCornerRadius(after, 3);
        const uint8_t tl_pct = CornerRadiusPercent(after, 0) ? 1 : 0;
        const uint8_t tr_pct = CornerRadiusPercent(after, 1) ? 1 : 0;
        const uint8_t br_pct = CornerRadiusPercent(after, 2) ? 1 : 0;
        const uint8_t bl_pct = CornerRadiusPercent(after, 3) ? 1 : 0;
        update_clip_radius(Dart_HandleFromPersistent(node->clip_widget_p), tl,
                           tl, tr, tr, bl, bl, br, br, tl_pct, tl_pct, tr_pct,
                           tr_pct, bl_pct, bl_pct, br_pct, br_pct);
      }
      if (node->opacity_widget_p != nullptr && update_opacity != nullptr &&
          std::fabs(before.opacity - after.opacity) > 0.001) {
        Dart_Handle opacity =
            Dart_HandleFromPersistent(node->opacity_widget_p);
        update_opacity(opacity, after.opacity);
      }
      if (before.pointer_events_none != after.pointer_events_none) {
        rects_dirty = true;
        if (node->yoga_node_p != nullptr && g_page != nullptr &&
            g_page->holder != nullptr) {
          auto* update = reinterpret_cast<YGNodeUpdatePointerEventsFn>(
              FindCallback(g_page->holder->ffi_table,
                           kYGNodeUpdatePointerEventsHash));
          if (update != nullptr) {
            update(Dart_HandleFromPersistent(node->yoga_node_p),
                   after.pointer_events_none ? 1 : 0);
          }
        }
      }
      if (before.z_index != after.z_index) rects_dirty = true;
    }
      // CSS transform updates reuse the existing YGNode.  This also lets a
      // `transform` transition animate without rebuilding the page.
      const bool transform_changed =
          before.has_transform != after.has_transform ||
          std::memcmp(before.transform, after.transform,
                      sizeof(before.transform)) != 0 ||
          !SameFloatOrNan(before.translate_percent_x,
                          after.translate_percent_x) ||
          !SameFloatOrNan(before.translate_percent_y,
                          after.translate_percent_y) ||
          !SameFloatOrNan(before.transform_origin_x, after.transform_origin_x) ||
          !SameFloatOrNan(before.transform_origin_y, after.transform_origin_y) ||
          !SameFloatOrNan(before.transform_origin_x_percent,
                          after.transform_origin_x_percent) ||
          !SameFloatOrNan(before.transform_origin_y_percent,
                          after.transform_origin_y_percent) ||
          !SameFloatOrNan(before.transform_origin_z, after.transform_origin_z);
      if (transform_changed) {
        rects_dirty = true;
      }
      if (transform_changed && node->yoga_node_p != nullptr &&
          g_page != nullptr && g_page->holder != nullptr) {
        auto* setup = reinterpret_cast<YGNodeSetupTransformFn>(FindCallback(
            g_page->holder->ffi_table, kYGNodeSetupTransformHash));
        auto* origin = reinterpret_cast<YGNodeTransformOriginFn>(FindCallback(
            g_page->holder->ffi_table, kYGNodeTransformOriginHash));
        if (setup != nullptr) {
          Dart_Handle matrix = Dart_NewTypedData(Dart_TypedData_kFloat64, 16);
          if (!Dart_IsError(matrix)) {
            Dart_TypedData_Type data_type = Dart_TypedData_kInvalid;
            void* data = nullptr;
            intptr_t length = 0;
            if (!Dart_IsError(Dart_TypedDataAcquireData(
                    matrix, &data_type, &data, &length)) &&
                data_type == Dart_TypedData_kFloat64 && length == 16 &&
                data != nullptr) {
              auto* values = static_cast<double*>(data);
              BuildTransformMatrix(after, node->measured_w, node->measured_h,
                                   values);
            }
            Dart_TypedDataReleaseData(matrix);
            Dart_Handle yoga =
                Dart_HandleFromPersistent(node->yoga_node_p);
            setup(yoga, matrix);
            if (origin != nullptr) {
              double origin_x = 0.0;
              double origin_y = 0.0;
              ResolveTransformOrigin(after, node->measured_w, node->measured_h,
                                     &origin_x, &origin_y);
              origin(yoga, origin_x, origin_y, -1.0, -1.0);
            }
            if (node->tag == "text" && node->text_widget_p != nullptr) {
              auto* paragraph_mark =
                  reinterpret_cast<RenderParagraphNativeMarkNeedsLayoutFn>(
                      FindCallback(
                          g_page->holder->ffi_table,
                          kRenderParagraphNativeMarkNeedsLayoutHash));
              if (paragraph_mark != nullptr) {
                paragraph_mark(
                    Dart_HandleFromPersistent(node->text_widget_p));
              }
            }
            DeletePersistent(&node->transform_p);
            node->transform_p = Dart_NewPersistentHandle(matrix);
          }
        }
      }
    if (node->yoga_node_p != nullptr) {
      yoga_nodes.push_back(Dart_HandleFromPersistent(node->yoga_node_p));
    }
    node->previous_style = after;
    if (node->tag == "text") {
      node->previous_text = NodeNormalizedText(node);
    }
  }
  if (rebuild) {
    // A pending rebuild must not postpone a running transform transition:
    // the interpolated matrices are already applied above, so publish them
    // and repaint now.  The deferred full rebuild still settles text metrics.
    if (rects_dirty) PublishNodeRects();
    if (g_page != nullptr && g_page->root_node != nullptr) {
      for (Dart_Handle widget : yoga_nodes) mark_needs_layout(widget);
      mark_needs_layout(Dart_HandleFromPersistent(g_page->root_node));
    }
    return false;
  }
  // Paint-only batch (colour / highlight / opacity): the layout snapshot is
  // still valid, so skip the page-wide measure + layout pass.  An animated
  // transform/pointer-events/z-index change still alters the published rects
  // the appservice hit test reads, so refresh the snapshot in that case.
  if (!layout_needed) {
    if (rects_dirty) {
      PublishNodeRects();
      // Transform changes are paint-only for layout, but the render objects
      // still need to be invalidated or a CSS transform transition only
      // shows its final value after the next full rebuild.
      for (Dart_Handle widget : yoga_nodes) mark_needs_layout(widget);
      if (g_page != nullptr && g_page->root_node != nullptr) {
        mark_needs_layout(Dart_HandleFromPersistent(g_page->root_node));
      }
    }
    return true;
  }

  const float viewport_w =
      g_viewport_width > 0.0 ? static_cast<float>(g_viewport_width) : 320.0f;
  const float viewport_h =
      g_viewport_height > 0.0 ? static_cast<float>(g_viewport_height) : 240.0f;
  MeasureNode(root, viewport_w, true, viewport_h, true);
  root->measured_w = viewport_w;
  root->measured_h = viewport_h;
  std::vector<NodeMetric> metrics;
  LayoutNode(root, 0.0f, 0.0f, viewport_w, viewport_h, &metrics);
  WriteLayoutMetrics(metrics);
  // Sliver-hosted subtrees are positioned by Flutter; restore their
  // origin-relative buffers clobbered by the page-wide pass above.
  RestoreSliverLayoutMetrics();
  PublishNodeRects();
  for (Dart_Handle widget : yoga_nodes) mark_needs_layout(widget);
  Dart_Handle root_handle = Dart_HandleFromPersistent(g_page->root_node);
  mark_needs_layout(root_handle);
  SKY_LOG(kDebug, kLogRuntime, "appservice targeted update: %zu nodes",
          yoga_nodes.size());
  return true;
}

// Rebuilding the page while a swiper drag/animation is in flight replaces the
// Swiper widget mid-gesture, which resets its page and can leave the swiper
// painter blank.  Image responses and setData batches arrive continuously on
// this page, so defer those rebuilds until the swiper reports scroll end.
namespace {
bool g_swiper_gesture_active = false;
bool g_rebuild_after_gesture = false;
}  // namespace

void SetSwiperGestureActive(bool active) {
  g_swiper_gesture_active = active;
  if (active || !g_rebuild_after_gesture) return;
  g_rebuild_after_gesture = false;
  RuntimeHolder* holder = g_page != nullptr ? g_page->holder : nullptr;
  if (holder == nullptr) return;
  auto runner = UiTaskRunner();
  if (!runner) return;
  // Let SwiperOnChange update the node's `current` attribute before the
  // rebuilt Swiper reads its initial page.
  runner->PostDelayedTask(
      [holder]() {
        DemoPageApiScope scope;
        RebuildAppServicePage(holder);
      },
      fml::TimeDelta::FromMilliseconds(120));
}

void RebuildAppServicePage(RuntimeHolder* holder) {
  SKY_DCHECK_UI_THREAD();
  if (g_swiper_gesture_active) {
    g_rebuild_after_gesture = true;
    return;
  }
  ClearTransitions();
  if (g_page == nullptr || g_page->root_node == nullptr ||
      g_page->root_connector == nullptr) {
    return;
  }
  AppNode* root = FindNode(g_page->root_id);
  if (root == nullptr) return;

  auto* newYGNode = reinterpret_cast<NewYGNodeFn>(
      FindCallback(holder->ffi_table, kNewYGNodeHash));
  auto* newYGConnector = reinterpret_cast<NewYGConnectorFn>(
      FindCallback(holder->ffi_table, kNewYGConnectorHash));
  auto* newText =
      reinterpret_cast<NewTextFn>(FindCallback(holder->ffi_table, kNewTextHash));
  auto* newTextSpan = reinterpret_cast<NewTextSpanFn>(
      FindCallback(holder->ffi_table, kNewTextSpanHash));
  auto* newDecoratedBox = reinterpret_cast<NewDecoratedBoxFn>(
      FindCallback(holder->ffi_table, kNewDecoratedBoxHash));
  auto* newOpacity = reinterpret_cast<NewOpacityFn>(
      FindCallback(holder->ffi_table, kNewOpacityHash));
  auto* newScrollView = reinterpret_cast<NewScrollViewFn>(
      FindCallback(holder->ffi_table, kNewScrollViewHash));
  auto* newListView = reinterpret_cast<NewListViewFn>(
      FindCallback(holder->ffi_table, kNewListViewHash));
  auto* newGridView = reinterpret_cast<NewGridViewFn>(
      FindCallback(holder->ffi_table, kNewGridViewHash));
  auto* newDirectRenderWidget = reinterpret_cast<NewDirectRenderWidgetFn>(
      FindCallback(holder->ffi_table, kNewDirectRenderWidgetHash));
  auto* newListBuilder = reinterpret_cast<NewListBuilderFn>(
      FindCallback(holder->ffi_table, kNewListBuilderHash));
  auto* newGridBuilder = reinterpret_cast<NewGridBuilderFn>(
      FindCallback(holder->ffi_table, kNewGridBuilderHash));
  auto* updateChildren = reinterpret_cast<ConnectorUpdateChildrenFn>(
      FindCallback(holder->ffi_table, kYGConnectorUpdateChildrenHash));
  auto* markNeedsLayout = reinterpret_cast<MarkYGNodeNeedsLayoutFn>(
      FindCallback(holder->ffi_table, kMarkYGNodeNeedsLayoutHash));
  if (newYGNode == nullptr || newYGConnector == nullptr || newText == nullptr ||
      newTextSpan == nullptr || newDecoratedBox == nullptr ||
      newOpacity == nullptr || updateChildren == nullptr) {
    SKY_LOG(kWarning, kLogRuntime, "appservice flush: callbacks missing");
    return;
  }

  ResolveStyles(root, nullptr);
  ComputeInlineWhitespace(root);
  ClearSliverLayoutMarks();
  const float viewport_w =
      g_viewport_width > 0.0 ? static_cast<float>(g_viewport_width) : 320.0f;
  const float viewport_h =
      g_viewport_height > 0.0 ? static_cast<float>(g_viewport_height) : 240.0f;
  // Only the page being rebuilt loses its materialized handles.  The node
  // table is shared with the routes kept alive under this one; resetting them
  // would drop the covered page's image registrations and its retained image
  // widgets would paint empty slots while a non-opaque transition reveals it.
  // Released nodes of this page are already detached from the root, so they
  // are excluded by walking the covered pages instead of the current root.
  std::unordered_set<AppNode*> covered;
  for (const auto& entry : g_page->page_stack) {
    CollectSubtreeNodes(FindNode(entry.root_id), &covered);
  }
  // The app-bar overlay lives outside the page route stack; keep its widget
  // handles (image downloads, text widgets) alive across page rebuilds.
  if (g_page->appbar_root_id != 0) {
    CollectSubtreeNodes(FindNode(g_page->appbar_root_id), &covered);
  }
  for (auto& entry : g_page->nodes) {
    if (covered.count(entry.second) != 0) continue;
    ResetNodeHandlesSkipping(entry.second, true, covered);
  }
  PrepareLayoutBuffers(root);
  MeasureNode(root, viewport_w, true, viewport_h, true);
  root->measured_w = viewport_w;
  root->measured_h = viewport_h;
  std::vector<NodeMetric> metrics;
  LayoutNode(root, 0.0f, 0.0f, viewport_w, viewport_h, &metrics);
  WriteLayoutMetrics(metrics);

  std::vector<Dart_Handle> widgets;
  ResetDeferredFixedNodes();
  g_fixed_root_node = root;
  g_collect_fixed_nodes = true;
  for (AppNode* child : root->children) {
    if (child->released || child->style.display_none) continue;
    if (DeferFixedChild(child)) continue;
    Dart_Handle widget = BuildWidget(child, newYGNode, newYGConnector, newText,
                                     newTextSpan, newDecoratedBox, newOpacity,
                                     newScrollView, newListView, newGridView,
                                     newDirectRenderWidget, newListBuilder,
                                     newGridBuilder, false);
    if (widget != nullptr && !Dart_IsError(widget)) widgets.push_back(widget);
  }
  for (size_t i = 0; i < g_deferred_fixed_nodes.size(); i++) {
    AppNode* fixed = g_deferred_fixed_nodes[i];
    if (fixed == nullptr || fixed->released || fixed->style.display_none) {
      continue;
    }
    std::vector<NodeMetric> fixed_metrics;
    fixed_metrics.push_back(NodeMetric{fixed, fixed->abs_x, fixed->abs_y,
                                       fixed->measured_w, fixed->measured_h});
    WriteLayoutMetrics(fixed_metrics);
    Dart_Handle widget =
        BuildWidget(fixed, newYGNode, newYGConnector, newText, newTextSpan,
                    newDecoratedBox, newOpacity, newScrollView, newListView,
                    newGridView, newDirectRenderWidget, newListBuilder,
                    newGridBuilder, false);
    SKY_LOG(kDebug, kLogOps, "rebuild fixed #%u/%s widget=%s", fixed->id,
            fixed->name.c_str(),
            widget != nullptr && !Dart_IsError(widget) ? "ok" : "failed");
    if (widget != nullptr && !Dart_IsError(widget)) widgets.push_back(widget);
  }
  g_collect_fixed_nodes = false;
  ResetDeferredFixedNodes();
  Dart_Handle list = Dart_NewList(static_cast<intptr_t>(widgets.size()));
  for (size_t i = 0; i < widgets.size(); i++) {
    Dart_ListSetAt(list, static_cast<intptr_t>(i), widgets[i]);
  }
  Dart_Handle connector = Dart_HandleFromPersistent(g_page->root_connector);
  updateChildren(connector, 0, list);
  if (markNeedsLayout != nullptr) {
    for (Dart_Handle widget : widgets) markNeedsLayout(widget);
    Dart_Handle root_handle = Dart_HandleFromPersistent(g_page->root_node);
    markNeedsLayout(root_handle);
  }
  PublishNodeRects();
  SnapshotNode(root);
  if (SKY_LOG_ENABLED(kDebug)) {
    DebugDumpNode(root, 0);
    for (const NodeMetric& metric : metrics) {
      SKY_LOG(kDebug, kLogOps, "rebuild metric #%u -> top=%g left=%g w=%g h=%g",
              metric.node->id, static_cast<double>(metric.y),
              static_cast<double>(metric.x), static_cast<double>(metric.w),
              static_cast<double>(metric.h));
    }
  }
  SKY_LOG(kDebug, kLogRuntime, "appservice flush: %zu children",
          widgets.size());
  if (widgets.empty()) {
    SKY_LOG(kDebug, kLogRuntime, "flush root=%u children=%zu connector=%p",
            g_page->root_id, root->children.size(),
            static_cast<void*>(g_page->root_connector));
    for (AppNode* child : root->children) {
      SKY_LOG(kDebug, kLogRuntime,
              "  child #%u tag=%s released=%d display_none=%d", child->id,
              child->tag.c_str(), child->released ? 1 : 0,
              child->style.display_none ? 1 : 0);
    }
  }
}

// Live-op batches that only need a rebuild because a style changed (e.g.
// `display:none` <-> `flex` for the A-Z bubble) are deferred briefly.  A quick
// tap flushes the press and release class updates milliseconds apart; merging
// them into one paint-only update avoids two full page rebuilds (each ~700 ms
// on the address-book page) and lets the selected highlight repaint at once.
// UI thread only.
int g_deferred_rebuild_generation = 0;
bool g_deferred_rebuild_pending = false;

void CancelDeferredRebuild() {
  g_deferred_rebuild_generation += 1;
  g_deferred_rebuild_pending = false;
}

void FlushAppServicePage(RuntimeHolder* holder, bool structural) {
  if (structural) {
    CancelDeferredRebuild();
    RebuildAppServicePage(holder);
    return;
  }
  if (ApplyTargetedUpdates(holder)) {
    CancelDeferredRebuild();
    return;
  }
  const int generation = ++g_deferred_rebuild_generation;
  g_deferred_rebuild_pending = true;
  auto runner = UiTaskRunner();
  if (!runner) {
    g_deferred_rebuild_pending = false;
    RebuildAppServicePage(holder);
    return;
  }
  runner->PostDelayedTask(
      [holder, generation]() {
        if (generation != g_deferred_rebuild_generation ||
            !g_deferred_rebuild_pending) {
          return;
        }
        if (Dart_CurrentIsolate() == nullptr && g_isolate == nullptr) {
          g_deferred_rebuild_pending = false;
          return;
        }
        DemoPageApiScope scope;
        g_deferred_rebuild_pending = false;
        RebuildAppServicePage(holder);
      },
      fml::TimeDelta::FromMilliseconds(300));
}

// -- CSS transitions and animated styles ------------------------------------

struct ActiveTransition {
  AppNode* node = nullptr;
  AppStyle from;
  AppStyle to;
  uint32_t mask = 0;
  double start_ms = 0.0;
  double duration_ms = 1.0;
  int easing = 0;
};

std::vector<ActiveTransition> g_active_transitions;

double SteadyNowMs() {
  using clock = std::chrono::steady_clock;
  static const clock::time_point origin = clock::now();
  return std::chrono::duration<double, std::milli>(clock::now() - origin)
      .count();
}

double EaseTransition(int mode, double t) {
  if (t <= 0.0) return 0.0;
  if (t >= 1.0) return 1.0;
  switch (mode) {
    case 1:  // ease
      return t * t * (3.0 - 2.0 * t);
    case 2:  // ease-in
      return t * t;
    case 3:  // ease-out
      return 1.0 - (1.0 - t) * (1.0 - t);
    case 4:  // ease-in-out
      return t < 0.5 ? 2.0 * t * t : 1.0 - 2.0 * (1.0 - t) * (1.0 - t);
    default:
      return t;
  }
}

double LerpDouble(double a, double b, double t) { return a + (b - a) * t; }

uint32_t LerpColor(uint32_t a, uint32_t b, double t) {
  auto channel = [t](uint32_t x, uint32_t y, int shift) -> uint32_t {
    const double v = LerpDouble((x >> shift) & 0xFF, (y >> shift) & 0xFF, t);
    return static_cast<uint32_t>(std::lround(v)) & 0xFF;
  };
  return (channel(a, b, 24) << 24) | (channel(a, b, 16) << 16) |
         (channel(a, b, 8) << 8) | channel(a, b, 0);
}

uint32_t TransitionChangedMask(const AppStyle& before, const AppStyle& after,
                               uint32_t mask) {
  uint32_t changed = 0;
  if ((mask & kTransitionOpacity) != 0 &&
      std::fabs(before.opacity - after.opacity) > 0.001) {
    changed |= kTransitionOpacity;
  }
  if ((mask & kTransitionBackground) != 0 &&
      (before.has_background != after.has_background ||
       before.background != after.background)) {
    changed |= kTransitionBackground;
  }
  if ((mask & kTransitionColor) != 0 && before.color != after.color) {
    changed |= kTransitionColor;
  }
  if ((mask & kTransitionBorderColor) != 0 &&
      BorderSidesChanged(before, after)) {
    changed |= kTransitionBorderColor;
  }
  if ((mask & kTransitionTransform) != 0 &&
      (before.has_transform != after.has_transform ||
       std::memcmp(before.transform, after.transform,
                   sizeof(before.transform)) != 0)) {
    changed |= kTransitionTransform;
  }
  return changed;
}

void ResetTransitionProperties(AppStyle* style, const AppStyle& from,
                              uint32_t mask) {
  if ((mask & kTransitionOpacity) != 0) style->opacity = from.opacity;
  if ((mask & kTransitionBackground) != 0) {
    style->has_background = from.has_background;
    style->background = from.background;
  }
  if ((mask & kTransitionColor) != 0) style->color = from.color;
  if ((mask & kTransitionBorderColor) != 0) {
    style->border_color = from.border_color;
    style->border_width = from.border_width;
    for (int side = 0; side < 4; side++) {
      style->side_border_width[side] = EffectiveBorderWidth(from, side);
      style->side_border_color[side] = EffectiveBorderColor(from, side);
      style->side_border_color_set[side] = true;
    }
  }
  if ((mask & kTransitionTransform) != 0) {
    style->has_transform = from.has_transform;
    std::memcpy(style->transform, from.transform, sizeof(style->transform));
  }
}

AppStyle InterpolateTransition(const ActiveTransition& transition, double t) {
  AppStyle style = transition.to;
  const AppStyle& from = transition.from;
  const AppStyle& to = transition.to;
  const uint32_t mask = transition.mask;
  if ((mask & kTransitionOpacity) != 0) {
    style.opacity = LerpDouble(from.opacity, to.opacity, t);
  }
  if ((mask & kTransitionBackground) != 0) {
    style.has_background = from.has_background || to.has_background;
    style.background = LerpColor(from.background, to.background, t);
  }
  if ((mask & kTransitionColor) != 0) {
    style.color = LerpColor(from.color, to.color, t);
  }
  if ((mask & kTransitionBorderColor) != 0) {
    style.border_color = LerpColor(from.border_color, to.border_color, t);
    style.border_width =
        static_cast<float>(LerpDouble(from.border_width, to.border_width, t));
    for (int side = 0; side < 4; side++) {
      style.side_border_width[side] = static_cast<float>(LerpDouble(
          EffectiveBorderWidth(from, side), EffectiveBorderWidth(to, side), t));
      style.side_border_color[side] =
          LerpColor(EffectiveBorderColor(from, side),
                    EffectiveBorderColor(to, side), t);
      style.side_border_color_set[side] = true;
    }
  }
  if ((mask & kTransitionTransform) != 0) {
    style.has_transform = from.has_transform || to.has_transform;
    for (int i = 0; i < 16; i++) {
      style.transform[i] = static_cast<float>(
          LerpDouble(from.transform[i], to.transform[i], t));
    }
  }
  return style;
}

void ScheduleTransitionTick(RuntimeHolder* holder);
void AdvanceTransitions(RuntimeHolder* holder);
void SchedulePendingAnimatedStyles(RuntimeHolder* holder);

void ClearTransitions() {
  for (ActiveTransition& transition : g_active_transitions) {
    if (transition.node != nullptr) {
      transition.node->animation_override_active = false;
    }
  }
  g_active_transitions.clear();
}

void StartTransitions(uint32_t root_id) {
  if (g_page == nullptr || root_id == 0) return;
  AppNode* root = FindNode(root_id);
  if (root == nullptr) return;
  ResolveStyles(root, nullptr);

  std::vector<AppNode*> pending;
  pending.push_back(root);
  while (!pending.empty()) {
    AppNode* node = pending.back();
    pending.pop_back();
    for (AppNode* child : node->children) {
      if (!child->released) pending.push_back(child);
    }
    if (!node->has_previous_style || node->animation_override_active) continue;
    const AppStyle& before = node->previous_style;
    const AppStyle& after = node->style;
    if (after.transition_duration <= 0.0 && after.transition_delay <= 0.0) {
      continue;
    }
    const uint32_t mask =
        TransitionChangedMask(before, after, after.transition_mask);
    if (mask == 0) continue;

    ActiveTransition transition;
    transition.node = node;
    transition.from = before;
    transition.to = after;
    transition.mask = mask;
    transition.start_ms = SteadyNowMs() + after.transition_delay;
    transition.duration_ms = std::max(1.0, after.transition_duration);
    transition.easing = after.transition_easing;
    node->animation_override = after;
    ResetTransitionProperties(&node->animation_override, before, mask);
    node->animation_override_active = true;
    g_active_transitions.push_back(transition);
    SKY_LOG(kDebug, kLogRuntime,
            "transition #%u start: mask=0x%x duration=%gms delay=%gms",
            node->id, mask, after.transition_duration, after.transition_delay);
  }
  if (!g_active_transitions.empty() && g_page->holder != nullptr) {
    ScheduleTransitionTick(g_page->holder);
  }
}

void ScheduleTransitionTick(RuntimeHolder* holder) {
  if (g_active_transitions.empty()) return;
  auto runner = UiTaskRunner();
  if (!runner) return;
  runner->PostDelayedTask(
      [holder]() {
        if (Dart_CurrentIsolate() == nullptr && g_isolate == nullptr) return;
        DemoPageApiScope scope;
        AdvanceTransitions(holder);
      },
      fml::TimeDelta::FromMilliseconds(16));
}

void AdvanceTransitions(RuntimeHolder* holder) {
  if (g_active_transitions.empty() || g_page == nullptr) return;
  const double now = SteadyNowMs();
  bool pending_animation = false;
  bool applied = false;
  std::vector<AppNode*> finished;
  for (ActiveTransition& transition : g_active_transitions) {
    if (transition.node == nullptr || transition.node->released) {
      finished.push_back(transition.node);
      continue;
    }
    if (now < transition.start_ms) {
      pending_animation = true;
      continue;
    }
    double raw = (now - transition.start_ms) / transition.duration_ms;
    if (raw < 0.0) raw = 0.0;
    if (raw > 1.0) raw = 1.0;
    const double eased = EaseTransition(transition.easing, raw);
    transition.node->animation_override =
        InterpolateTransition(transition, eased);
    transition.node->animation_override_active = true;
    applied = true;
    SKY_LOG(kDebug, kLogRuntime,
            "transition #%u t=%.3f opacity=%.3f background=%08x",
            transition.node->id, raw,
            transition.node->animation_override.opacity,
            transition.node->animation_override.has_background
                ? transition.node->animation_override.background
                : 0u);
    if (raw >= 1.0) {
      finished.push_back(transition.node);
    } else {
      pending_animation = true;
    }
  }
  if (applied) FlushAppServicePage(holder, false);
  if (!finished.empty() && !g_active_transitions.empty()) {
    for (AppNode* node : finished) {
      if (node != nullptr) node->animation_override_active = false;
    }
    g_active_transitions.erase(
        std::remove_if(g_active_transitions.begin(), g_active_transitions.end(),
                       [&finished](const ActiveTransition& transition) {
                         return std::find(finished.begin(), finished.end(),
                                          transition.node) != finished.end();
                       }),
        g_active_transitions.end());
    for (AppNode* node : finished) {
      if (node != nullptr && !node->released) {
        SKY_LOG(kDebug, kLogRuntime, "transition #%u done", node->id);
      }
    }
    FlushAppServicePage(holder, false);
  }
  if (pending_animation) ScheduleTransitionTick(holder);
}

void SchedulePendingAnimatedStyles(RuntimeHolder* holder) {
  if (g_page == nullptr || holder == nullptr) return;
  auto runner = UiTaskRunner();
  if (!runner) return;
  for (size_t i = g_page->scheduled_animated_styles;
       i < g_page->animated_styles.size(); i++) {
    const PendingAnimatedStyle entry = g_page->animated_styles[i];
    runner->PostDelayedTask(
        [holder, entry]() {
          if (g_page == nullptr) return;
          if (Dart_CurrentIsolate() == nullptr && g_isolate == nullptr) return;
          DemoPageApiScope scope;
          AppNode* node = FindNode(entry.node_id);
          if (node == nullptr || node->released) return;
          AppStyle style = node->style;
          ApplyDeclarations(&style, ParseDeclarations(entry.declarations));
          node->animation_override = style;
          node->animation_override_active = true;
          FlushAppServicePage(holder, false);
          SKY_LOG(kDebug, kLogRuntime, "animated style #%u at %dms: %s",
                  entry.node_id, entry.at_ms, entry.declarations.c_str());
        },
        fml::TimeDelta::FromMilliseconds(std::max(0, entry.at_ms)));
    SKY_LOG(kDebug, kLogRuntime, "animated style scheduled: #%u in %dms",
            entry.node_id, entry.at_ms);
  }
  g_page->scheduled_animated_styles = g_page->animated_styles.size();
}


// -- page routes -------------------------------------------------------------


}  // namespace appservice

