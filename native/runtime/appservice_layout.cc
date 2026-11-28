// Flexbox/grid measurement, layout and sliver metric handling.

#include "internal/skyline_runtime_module.hh"
#include "internal/skyline_runtime_layout.hh"

namespace appservice {

bool IsImageName(const std::string& name);
bool IsListBuilderName(const std::string& name);
bool IsGridBuilderName(const std::string& name);
bool IsScrollViewName(const std::string& name);
bool IsShareElementNode(const AppNode* node);
bool IsSwiperNode(const AppNode* node);
bool LoadImageForNode(AppNode* node);
bool LoadBackgroundImageForNode(AppNode* node);
void CancelPendingImage(AppNode* node);
void ClearTransitions();

bool ImageUsesWidthFix(const AppNode* node) {
  static const char* kKeys[] = {"mode", "objectFit", "object-fit"};
  for (const char* key : kKeys) {
    const auto found = node->attrs.find(key);
    if (found != node->attrs.end() && found->second == "widthFix") return true;
  }
  return false;
}

float BorderBoxExtra(const AppStyle& style, bool horizontal) {
  const float pad = horizontal ? style.padding[1] + style.padding[3]
                               : style.padding[0] + style.padding[2];
  const float border =
      horizontal ? EffectiveBorderWidth(style, 1) + EffectiveBorderWidth(style, 3)
                 : EffectiveBorderWidth(style, 0) + EffectiveBorderWidth(style, 2);
  return pad + border;
}

// `width`/`height`/min/max are CSS content-box values when box-sizing is
// content-box; the layout works in border-box units.
bool IsContentBox(const AppStyle& style) { return style.box_sizing == 1; }

float ContentBoxInset(const AppStyle& style, bool horizontal) {
  return IsContentBox(style) ? BorderBoxExtra(style, horizontal) : 0.0f;
}

float DeclaredWidth(const AppStyle& style) {
  return style.width >= 0.0f ? style.width + ContentBoxInset(style, true)
                             : -1.0f;
}

float DeclaredHeight(const AppStyle& style) {
  return style.height >= 0.0f ? style.height + ContentBoxInset(style, false)
                              : -1.0f;
}

float MinBoxWidth(const AppStyle& style) {
  return style.min_width + ContentBoxInset(style, true);
}

float MaxBoxWidth(const AppStyle& style) {
  return style.max_width + ContentBoxInset(style, true);
}

float MinBoxHeight(const AppStyle& style) {
  return style.min_height + ContentBoxInset(style, false);
}

float MaxBoxHeight(const AppStyle& style) {
  return style.max_height + ContentBoxInset(style, false);
}

// CSS percentage padding/margin (both axes) resolve against the containing
// block's width.  Overwrites the px fields so all layout/paint call sites keep
// using the same arrays.
void ResolveBoxEdges(AppStyle* style, float containing_width) {
  for (int i = 0; i < 4; i++) {
    if (!std::isnan(style->padding_percent[i])) {
      style->padding[i] =
          std::max(0.0f, containing_width * style->padding_percent[i]);
    }
    if (!std::isnan(style->margin_percent[i])) {
      style->margin[i] = containing_width * style->margin_percent[i];
    }
  }
}

SizeF MeasureNode(AppNode* node, float avail_w, bool stretch_w, float avail_h,
                  bool stretch_h, float containing_w) {
  SizeF size{0.0f, 0.0f};
  if (node->released || node->style.display_none) {
    node->measured_w = 0.0f;
    node->measured_h = 0.0f;
    return size;
  }
  static const uint32_t debug_node_id = [] {
    return static_cast<uint32_t>(config::Int(config::kLayoutDebugId));
  }();
  const bool debug_node = debug_node_id != 0 && node->id == debug_node_id;
  if (containing_w < 0.0f) containing_w = avail_w;
  ResolveBoxEdges(&node->style, containing_w);
  const AppStyle& style = node->style;

  const bool is_text = node->tag == "text";
  std::string text = is_text ? NodeNormalizedText(node) : std::string();

  float width = -1.0f;
  if (style.width_percent >= 0.0f) {
    width = ResolvePercentExtent(avail_w, style.width_percent);
    if (width >= 0.0f) width += ContentBoxInset(style, true);
  } else if (style.width >= 0.0f) {
    width = DeclaredWidth(style);
  } else if (is_text) {
    const double intrinsic = TextIntrinsicWidth(text, style);
    // Block-level text stretches to the containing block width; that is what
    // makes white-space:normal wrap and nowrap + ellipsis clip inside the box.
    if (stretch_w && avail_w < kInf) {
      width = avail_w;
    } else {
      width = static_cast<float>(intrinsic) + BorderBoxExtra(style, true);
    }
  } else if (stretch_w && avail_w < kInf) {
    width = avail_w;
  }
  if (width >= 0.0f) {
    if (width < MinBoxWidth(style)) width = MinBoxWidth(style);
    if (width > MaxBoxWidth(style)) width = MaxBoxWidth(style);
  }

  // List/grid builders are slivers: Flutter positions their items, so measure
  // the builder box from its declared child count instead of recursing into
  // the (possibly huge) captured item table.
  if (IsListBuilderName(node->name) || IsGridBuilderName(node->name)) {
    float height = style.height >= 0.0f ? DeclaredHeight(style) : -1.0f;
    if (height < 0.0f && style.height_percent >= 0.0f) {
      height = ResolvePercentExtent(avail_h, style.height_percent);
      if (height >= 0.0f) height += ContentBoxInset(style, false);
    }
    if (height < 0.0f) {
      int child_count = 0;
      double child_height = 0.0;
      const auto count_attr = node->attrs.find("childCount");
      if (count_attr != node->attrs.end()) {
        child_count = std::atoi(count_attr->second.c_str());
      }
      const auto height_attr = node->attrs.find("childHeight");
      if (height_attr != node->attrs.end()) {
        child_height = std::atof(height_attr->second.c_str());
      }
      if (IsGridBuilderName(node->name)) {
        double cross = 1.0;
        const auto cross_attr = node->attrs.find("crossAxisCount");
        if (cross_attr != node->attrs.end()) {
          cross = std::atof(cross_attr->second.c_str());
        }
        const int columns = static_cast<int>(cross > 0 ? cross : 1);
        const int rows = (child_count + columns - 1) / columns;
        height = static_cast<float>(rows * child_height);
      } else {
        height = static_cast<float>(child_count * child_height);
      }
      height += ContentBoxInset(style, false);
    }
    if (width < 0.0f) width = stretch_w ? avail_w : 0.0f;
    node->measured_w = width;
    node->measured_h = height;
    return SizeF{width, height};
  }

  // Images: `widthFix` preserves the intrinsic aspect ratio, so the height is
  // derived from the decoded bitmap rather than the CSS box.
  if (IsImageName(node->name)) {
    LoadImageForNode(node);
    if (width >= 0.0f && ImageUsesWidthFix(node) && node->image_dl != nullptr &&
        node->image_dl->width() > 0) {
      const double ratio = static_cast<double>(node->image_dl->height()) /
                           static_cast<double>(node->image_dl->width());
      const float content_width =
          std::max(0.0f, width - BorderBoxExtra(style, true));
      float height = static_cast<float>(content_width * ratio) +
                     BorderBoxExtra(style, false);
      if (height < MinBoxHeight(style)) height = MinBoxHeight(style);
      if (height > MaxBoxHeight(style)) height = MaxBoxHeight(style);
      node->measured_w = width;
      node->measured_h = height;
      return SizeF{width, height};
    }
  }

  if (is_text) {
    const float content_w = width >= 0.0f ? width - BorderBoxExtra(style, true)
                                          : static_cast<float>(TextIntrinsicWidth(text, style));
    const int lines = CountTextLines(text, content_w, style);
    float height = style.height >= 0.0f
                       ? DeclaredHeight(style)
                       : static_cast<float>(lines * TextLineHeight(style)) +
                             BorderBoxExtra(style, false);
    if (height < MinBoxHeight(style)) height = MinBoxHeight(style);
    if (height > MaxBoxHeight(style)) height = MaxBoxHeight(style);
    if (width < 0.0f) width = content_w + BorderBoxExtra(style, true);
    node->measured_w = width;
    node->measured_h = height;
    return SizeF{width, height};
  }

  const float content_avail_w =
      width >= 0.0f ? std::max(0.0f, width - BorderBoxExtra(style, true))
                    : avail_w;

  auto measure_children = [&]() {
    if (IsInlineFlowContainer(node)) {
      float line_main = 0.0f;
      float line_cross = 0.0f;
      float max_line_main = 0.0f;
      float total_cross = 0.0f;
      for (AppNode* child : OrderedFlowChildren(node)) {
        if (child == nullptr) continue;
        const SizeF measured = MeasureNode(child, content_avail_w, false, kInf, false);
        const float outer_main =
            measured.w + child->style.margin[1] + child->style.margin[3];
        const float outer_cross =
            measured.h + child->style.margin[0] + child->style.margin[2];
        if (line_main > 0.0f && content_avail_w < kInf &&
            line_main + outer_main > content_avail_w + 0.01f) {
          max_line_main = std::max(max_line_main, line_main);
          total_cross += line_cross;
          line_main = 0.0f;
          line_cross = 0.0f;
        }
        line_main += outer_main;
        line_cross = std::max(line_cross, outer_cross);
      }
      max_line_main = std::max(max_line_main, line_main);
      total_cross += line_cross;
      return SizeF{max_line_main, total_cross};
    }
    float main_extent = 0.0f;
    float cross_extent = 0.0f;
    int flow_children = 0;
    const float main_gap = MainGap(style);
    const float cross_gap = style.row ? style.row_gap : style.column_gap;
    float explicit_height = -1.0f;
    if (style.height >= 0.0f) {
      explicit_height = DeclaredHeight(style);
    } else if (style.height_percent >= 0.0f) {
      const float resolved = ResolvePercentExtent(avail_h, style.height_percent);
      if (resolved >= 0.0f) {
        explicit_height = resolved + ContentBoxInset(style, false);
      }
    }
    const float wrap_limit =
        style.wrap
            ? (style.row
                   ? content_avail_w
                   : (explicit_height >= 0.0f
                          ? explicit_height - BorderBoxExtra(style, false)
                          : kInf))
            : kInf;
    float line_main = 0.0f;
    float line_cross = 0.0f;
    float max_line_main = 0.0f;
    float total_cross = 0.0f;
    int lines = 0;
    auto finish_line = [&]() {
      if (line_main <= 0.0f && line_cross <= 0.0f) return;
      max_line_main = std::max(max_line_main, line_main);
      total_cross += line_cross;
      lines++;
      line_main = 0.0f;
      line_cross = 0.0f;
    };
    for (AppNode* child : node->children) {
      if (child != nullptr &&
          (child->released || child->style.display_none)) {
        child->measured_w = 0.0f;
        child->measured_h = 0.0f;
      }
    }
    for (AppNode* child : OrderedFlowChildren(node)) {
      ++flow_children;
      const int child_align = EffectiveAlign(style, child->style);
      // Block-flow text stretches to the parent's content box only when that
      // width is definite.  While measuring an auto-width box for intrinsic
      // sizing (e.g. a flex-row item's `flex-basis: auto`), stretching the
      // child to the available width reports the whole row as the item's
      // max-content size — the detail navigation bar's "关注" pill then ate
      // the row and the `flex: 1` title collapsed to zero.
      const bool child_stretch_w =
          width >= 0.0f && !style.wrap && !style.row && child_align == 0 &&
          !HasExplicitWidth(child->style);
      const bool child_stretch_h =
          !style.wrap && style.row && child_align == 0 &&
          !HasExplicitHeight(child->style);
      const SizeF measured = MeasureNode(child, content_avail_w, child_stretch_w,
                                         kInf, child_stretch_h);
      const float measured_main = style.row ? measured.w : measured.h;
      // Intrinsic sizing uses the content size; flex-basis applies once the
      // container has a definite main size (LayoutNode).
      const float child_main = measured_main;
      const float child_cross = style.row ? measured.h : measured.w;
      const float margin_main =
          style.row ? child->style.margin[1] + child->style.margin[3]
                    : child->style.margin[0] + child->style.margin[2];
      const float margin_cross =
          style.row ? child->style.margin[0] + child->style.margin[2]
                    : child->style.margin[1] + child->style.margin[3];
      const float outer_main = child_main + margin_main;
      const float outer_cross = child_cross + margin_cross;
      if (style.wrap && wrap_limit < kInf) {
        if (line_main > 0.0f &&
            line_main + main_gap + outer_main > wrap_limit + 0.01f) {
          finish_line();
        }
        if (line_main > 0.0f) line_main += main_gap;
        line_main += outer_main;
        line_cross = std::max(line_cross, outer_cross);
      } else {
        main_extent += outer_main;
        cross_extent = std::max(cross_extent, outer_cross);
      }
    }
    if (style.wrap && wrap_limit < kInf) {
      finish_line();
      if (lines > 1) {
        total_cross += cross_gap * static_cast<float>(lines - 1);
      }
      return style.row ? SizeF{max_line_main, total_cross}
                       : SizeF{total_cross, max_line_main};
    }
    const float gaps =
        flow_children > 1
            ? main_gap * static_cast<float>(flow_children - 1)
            : 0.0f;
    main_extent += gaps;
    return style.row ? SizeF{main_extent, cross_extent}
                     : SizeF{cross_extent, main_extent};
  };

  SizeF content = measure_children();
  const float extra_w = BorderBoxExtra(style, true);
  const float extra_h = BorderBoxExtra(style, false);
  if (width < 0.0f) {
    width = content.w + extra_w;
    if (width < MinBoxWidth(style)) width = MinBoxWidth(style);
    if (width > MaxBoxWidth(style)) width = MaxBoxWidth(style);
  }
  float height = -1.0f;
  if (style.height_percent >= 0.0f) {
    height = ResolvePercentExtent(avail_h, style.height_percent);
    if (height >= 0.0f) height += ContentBoxInset(style, false);
  } else if (style.height >= 0.0f) {
    height = DeclaredHeight(style);
  } else if (stretch_h && avail_h < kInf) {
    height = avail_h;
  }
  const bool width_definite =
      HasExplicitWidth(style) || (stretch_w && avail_w < kInf);
  const bool height_definite =
      HasExplicitHeight(style) || (stretch_h && avail_h < kInf);
  if (!is_text && style.aspect_ratio > 0.0f) {
    // CSS aspect-ratio: derive the automatic axis from the definite one.
    // Alignment stretch counts as definite, so a stretched flex/block box uses
    // the used width and derives its height.
    if (!height_definite && width_definite && width >= 0.0f) {
      const float content_w = IsContentBox(style)
                                  ? std::max(0.0f, width - BorderBoxExtra(style, true))
                                  : width;
      float content_h = content_w / style.aspect_ratio;
      height = IsContentBox(style)
                   ? content_h + BorderBoxExtra(style, false)
                   : content_h;
      if (height < MinBoxHeight(style)) height = MinBoxHeight(style);
      if (height > MaxBoxHeight(style)) height = MaxBoxHeight(style);
    } else if (!width_definite && height_definite && height >= 0.0f) {
      const float content_h = IsContentBox(style)
                                  ? std::max(0.0f, height - BorderBoxExtra(style, false))
                                  : height;
      float content_w = content_h * style.aspect_ratio;
      width = IsContentBox(style)
                  ? content_w + BorderBoxExtra(style, true)
                  : content_w;
      if (width < MinBoxWidth(style)) width = MinBoxWidth(style);
      if (width > MaxBoxWidth(style)) width = MaxBoxWidth(style);
    }
  }
  if (height < 0.0f) {
    SizeF remeasured = measure_children();
    height = remeasured.h + extra_h;
    if (height < MinBoxHeight(style)) height = MinBoxHeight(style);
    if (height > MaxBoxHeight(style)) height = MaxBoxHeight(style);
  }
  // A border box can never be smaller than its borders (and padding).  This
  // matters for CSS border triangles: `width:0;height:0;border:20px` is a
  // 40x40 box, not a zero-size one.
  if (width >= 0.0f && width < extra_w) width = extra_w;
  if (height >= 0.0f && height < extra_h) height = extra_h;

  node->measured_w = width;
  node->measured_h = height;
  if (debug_node) {
    std::fprintf(stderr,
                 "[ops-debug] measure #%u tag=%s children=%zu inline=%d "
                 "flow=%zu avail=%.1fx%.1f stretch=%d,%d -> %.1fx%.1f "
                 "content_h=%.1f\n",
                 node->id, node->tag.c_str(), node->children.size(),
                 IsInlineFlowContainer(node) ? 1 : 0,
                 OrderedFlowChildren(node).size(), static_cast<double>(avail_w),
                 static_cast<double>(avail_h), stretch_w ? 1 : 0,
                 stretch_h ? 1 : 0, static_cast<double>(width),
                 static_cast<double>(height), static_cast<double>(content.h));
  }
  return SizeF{width, height};
}


// Resolves the item's flex base size along the main axis.  `measured_main` is
// the intrinsic border-box size used when flex-basis is `auto`.
float ResolveFlexBasis(const AppStyle& style,
                       bool row,
                       float main_extent,
                       float measured_main) {
  float basis = measured_main;
  if (!std::isnan(style.flex_basis_percent)) {
    // Percentages resolve against a definite main size; otherwise CSS treats
    // the basis as auto/content.
    if (std::isfinite(main_extent)) {
      basis = main_extent * style.flex_basis_percent;
    }
  } else if (!std::isnan(style.flex_basis)) {
    basis = style.flex_basis;
  }
  const float min_main = row ? MinBoxWidth(style) : MinBoxHeight(style);
  const float max_main = row ? MaxBoxWidth(style) : MaxBoxHeight(style);
  if (basis < min_main) basis = min_main;
  if (basis > max_main) basis = max_main;
  return std::max(0.0f, basis);
}

// CSS flex items shrink by default (`flex-shrink: 1`) inside a `display:flex`
// container; block children never shrink (the collapsed app bar's block
// content overflows and is clipped by its own overflow:hidden).  Scroll
// content is exempt: its viewport is smaller than the content by design.
float EffectiveFlexShrink(const AppStyle& style, bool in_scroll_view,
                          bool flex_parent) {
  if (in_scroll_view) return 0.0f;
  if (style.has_flex_shrink) return style.flex_shrink;
  return flex_parent ? 1.0f : 0.0f;
}

// Defined with the widget builders below; the page-wide layout pass needs the
// same grid packing Flutter applies so published rects match the painted cards.
bool GridIsMasonry(const AppNode* node);
double GridMainGap(const AppNode* node);
double GridCrossGap(const AppNode* node);
double AttrNumber(const AppNode* node, const char* key, double fallback);
bool IsGridViewName(const std::string& name);
void CollectListItems(AppNode* parent, std::vector<AppNode*>* out);

// Available width inside a node's border box (padding included in the box).
float ContentBoxWidth(const AppNode* node) {
  return std::max(0.0f, node->measured_w - BorderBoxExtra(node->style, true));
}

// `<grid-view>` column count.  Flutter's `SkylineGridView` uses a fixed
// `crossAxisCount` unless `maxCrossAxisExtent` is set, in which case
// SliverSimpleGridDelegateWithMaxCrossAxisExtent selects
// `ceil(crossExtent / (maxExtent + spacing))`.
int GridColumnCount(const AppNode* node, float cross_extent) {
  const double max_extent = AttrNumber(node, "maxCrossAxisExtent", 0.0);
  if (max_extent > 0.0 && cross_extent > 0.0f) {
    const double stride = max_extent + GridCrossGap(node);
    if (stride > 0.0) {
      return std::max(
          1, static_cast<int>(std::ceil(
                 static_cast<double>(cross_extent) / stride)));
    }
  }
  return std::max(1, static_cast<int>(AttrNumber(
                          node, "crossAxisCount",
                          AttrNumber(node, "cross-axis-count", 2.0))));
}

float GridSlotWidth(const AppNode* node, int columns) {
  const float content_w = ContentBoxWidth(node);
  if (columns <= 0 || content_w <= 0.0f) {
    return std::max(0.0f, node->measured_w);
  }
  const float gap = static_cast<float>(GridCrossGap(node));
  const float slot =
      (content_w - gap * static_cast<float>(columns - 1)) /
      static_cast<float>(columns);
  return slot > 0.0f ? slot : node->measured_w;
}

// Collects the final rects instead of writing through cached typed-data
// pointers: Dart's GC may move typed data between API calls, so each buffer is
// acquired only for the duration of its own write.
//
// `abs_x`/`abs_y` are the node's border-box origin relative to the viewport.
// They let `position: fixed` resolve against the viewport instead of the
// parent's content box.
//
// `cb_*` describe the viewport-space containing block used by absolutely
// positioned descendants (the padding box of the nearest positioned ancestor,
// CSS 2.1 §10.1).  A negative width means "not established yet": the starting
// node becomes the containing block.
void LayoutNode(AppNode* node, float x, float y, float w, float h,
                std::vector<NodeMetric>* metrics, float abs_x,
                float abs_y, float cb_abs_x,
                float cb_abs_y, float cb_w,
                float cb_h) {
  node->abs_x = abs_x;
  node->abs_y = abs_y;
  static const uint32_t debug_node_id = [] {
    return static_cast<uint32_t>(config::Int(config::kLayoutDebugId));
  }();
  if (debug_node_id != 0 && node->id == debug_node_id) {
    std::fprintf(stderr,
                 "[ops-debug] layout #%u tag=%s -> x=%g y=%g w=%g h=%g "
                 "measured=%gx%g\n",
                 node->id, node->tag.c_str(), static_cast<double>(x),
                 static_cast<double>(y), static_cast<double>(w),
                 static_cast<double>(h), static_cast<double>(node->measured_w),
                 static_cast<double>(node->measured_h));
  }
  // Keep the measured size in sync with the final laid-out box.  MeasureNode
  // reports intrinsic sizes, so flex-grown children (e.g. the album component
  // host with `height: 0; flex: 1`) would otherwise publish a zero rect and
  // the mirrored hit test could not descend into them.
  node->measured_w = w;
  node->measured_h = h;
  if (metrics != nullptr) {
    metrics->push_back(NodeMetric{node, x, y, w, h});
  }

  const AppStyle& style = node->style;
  // `position: relative` (4) also establishes a containing block for its
  // absolutely positioned descendants even though it stays in flow.
  if (style.position != 0 || cb_w < 0.0f) {
    cb_abs_x = abs_x + EffectiveBorderWidth(style, 3);
    cb_abs_y = abs_y + EffectiveBorderWidth(style, 0);
    cb_w = std::max(
        0.0f, w - EffectiveBorderWidth(style, 3) - EffectiveBorderWidth(style, 1));
    cb_h = std::max(
        0.0f, h - EffectiveBorderWidth(style, 0) - EffectiveBorderWidth(style, 2));
  }

  if (node->children.empty()) return;
  // Builders materialize items on demand; the Flutter sliver owns placement.
  if (IsListBuilderName(node->name) || IsGridBuilderName(node->name)) return;

  const float content_x =
      EffectiveBorderWidth(style, 3) + style.padding[3];
  const float content_y =
      EffectiveBorderWidth(style, 0) + style.padding[0];
  const float content_w =
      std::max(0.0f, w - BorderBoxExtra(style, true));
  const float content_h =
      std::max(0.0f, h - BorderBoxExtra(style, false));

  // Builtin swiper: each item is a full-size page; `current` selects which one
  // sits at the viewport origin.  The parent clips the overflow.
  if (IsSwiperNode(node)) {
    int current = 0;
    const auto current_attr = node->attrs.find("current");
    if (current_attr != node->attrs.end()) {
      current = std::atoi(current_attr->second.c_str());
    }
    if (current < 0) current = 0;
    int index = 0;
    for (AppNode* child : node->children) {
      if (child == nullptr || child->released || child->style.display_none) {
        continue;
      }
      const float offset = static_cast<float>(index - current) * content_w;
      MeasureNode(child, content_w, true, content_h, true);
      const float child_x = content_x + offset;
      LayoutNode(child, child_x, content_y, content_w, content_h, metrics,
                 abs_x + child_x, abs_y + content_y, cb_abs_x, cb_abs_y, cb_w,
                 cb_h);
      index += 1;
    }
    return;
  }

  // Builtin `<grid-view>`: Flutter's SkylineGridView packs the items into
  // masonry columns (each item goes to the shortest column) or aligned rows.
  // The page-wide pass must publish the same per-item rects, otherwise every
  // card is stacked in a single column at x≈0 and the mirrored hit test never
  // reaches the right-hand column.
  if (IsGridViewName(node->name)) {
    std::vector<AppNode*> items;
    CollectListItems(node, &items);
    if (!items.empty()) {
      const bool masonry = GridIsMasonry(node);
      const int columns = GridColumnCount(node, content_w);
      const float cross_gap = static_cast<float>(GridCrossGap(node));
      const float main_gap = static_cast<float>(GridMainGap(node));
      const float slot_w = GridSlotWidth(node, columns);
      std::vector<int> item_column(items.size(), 0);
      std::vector<float> item_main(items.size(), 0.0f);
      std::vector<float> item_height(items.size(), 0.0f);
      for (size_t i = 0; i < items.size(); i++) {
        MeasureNode(items[i], slot_w, true, kInf, false);
        item_height[i] = items[i]->measured_h;
      }
      if (masonry) {
        std::vector<float> column_main(static_cast<size_t>(columns), 0.0f);
        for (size_t i = 0; i < items.size(); i++) {
          // Smallest column, first index wins ties (findSmallestIndexWithMinimumValue).
          int best = 0;
          for (int c = 1; c < columns; c++) {
            if (column_main[static_cast<size_t>(c)] <
                column_main[static_cast<size_t>(best)]) {
              best = c;
            }
          }
          item_column[i] = best;
          item_main[i] = column_main[static_cast<size_t>(best)];
          column_main[static_cast<size_t>(best)] =
              item_main[i] + item_height[i] + main_gap;
        }
      } else {
        float row_main = 0.0f;
        for (size_t start = 0; start < items.size();
             start += static_cast<size_t>(columns)) {
          const size_t end =
              std::min(items.size(), start + static_cast<size_t>(columns));
          float row_height = 0.0f;
          for (size_t i = start; i < end; i++) {
            row_height = std::max(row_height, item_height[i]);
          }
          for (size_t i = start; i < end; i++) {
            item_column[i] = static_cast<int>(i - start);
            item_main[i] = row_main;
          }
          row_main += row_height + main_gap;
        }
      }
      for (size_t i = 0; i < items.size(); i++) {
        AppNode* item = items[i];
        const float item_x =
            content_x + static_cast<float>(item_column[i]) *
                            (slot_w + cross_gap);
        const float item_y = content_y + item_main[i];
        // Item widths/heights were measured above with the slot width; keep the
        // absolute coordinates so descendants publish matching rects.
        LayoutNode(item, item_x, item_y, item->measured_w, item->measured_h,
                   metrics, abs_x + item_x, abs_y + item_y, cb_abs_x, cb_abs_y,
                   cb_w, cb_h);
      }
    }
    return;
  }

  if (IsInlineFlowContainer(node)) {
    struct InlineItem {
      AppNode* node = nullptr;
      float x = 0.0f;
      float w = 0.0f;
      float h = 0.0f;
      float margin_top = 0.0f;
      float margin_bottom = 0.0f;
    };
    std::vector<InlineItem> line;
    float cursor_x = content_x;
    float line_top = content_y;
    float line_height = 0.0f;
    auto flush_line = [&]() {
      // text-align positions each line's inline boxes inside the container.
      const float line_width = cursor_x - content_x;
      float align_offset = 0.0f;
      if (content_w < kInf && line_width < content_w) {
        if (style.text_align == 2) {
          align_offset = (content_w - line_width) / 2.0f;
        } else if (style.text_align == 1) {
          align_offset = content_w - line_width;
        }
      }
      for (const InlineItem& item : line) {
        // inline-block boxes align their bottom margin edge with the line
        // bottom (baseline for the common no-text case).
        const float offset =
            line_height - (item.h + item.margin_top + item.margin_bottom);
        const float child_x = item.x + align_offset;
        const float child_y = line_top + item.margin_top + offset;
        LayoutNode(item.node, child_x, child_y, item.w, item.h, metrics,
                   abs_x + child_x, abs_y + child_y, cb_abs_x, cb_abs_y, cb_w,
                   cb_h);
      }
      line_top += line_height;
      line_height = 0.0f;
      cursor_x = content_x;
      line.clear();
    };
    for (AppNode* child : OrderedFlowChildren(node)) {
      const SizeF measured = MeasureNode(child, content_w, false, kInf, false);
      const float margin_left = child->style.margin[3];
      const float margin_right = child->style.margin[1];
      const float margin_top = child->style.margin[0];
      const float margin_bottom = child->style.margin[2];
      const float outer_w = measured.w + margin_left + margin_right;
      const float outer_h = measured.h + margin_top + margin_bottom;
      if (!line.empty() && content_w < kInf &&
          cursor_x - content_x + outer_w > content_w + 0.01f) {
        flush_line();
      }
      line.push_back({child, cursor_x + margin_left, measured.w, measured.h,
                      margin_top, margin_bottom});
      line_height = std::max(line_height, outer_h);
      cursor_x += outer_w;
    }
    flush_line();
  } else {
  struct ChildLayout {
    AppNode* node = nullptr;
    float w = 0.0f;
    float h = 0.0f;
    float margin_main_start = 0.0f;
    float margin_cross_start = 0.0f;
    float margin_main_end = 0.0f;
    float margin_cross_end = 0.0f;
    float main = 0.0f;
    float cross = 0.0f;
    float base_main = 0.0f;
    float shrink = 0.0f;
  };
  std::vector<ChildLayout> children;
  children.reserve(node->children.size());

  const float main_extent = style.row ? content_w : content_h;
  const bool in_scroll_view = IsScrollViewName(node->name);
  const bool fill_share_wrapper = IsShareElementNode(node) &&
                                  node->children.size() == 1;
  float total_outer_base = 0.0f;
  float grow_sum = 0.0f;
  float shrink_weight_sum = 0.0f;
  for (AppNode* child : OrderedFlowChildren(node)) {
    ChildLayout layout;
    layout.node = child;
    // Re-measure with the final parent content box so percentages resolve.
    const bool stretch_cross =
        !style.wrap && EffectiveAlign(style, child->style) == 0 &&
        (style.row ? !HasExplicitHeight(child->style)
                   : !HasExplicitWidth(child->style));
    SizeF measured =
        MeasureNode(child, content_w, !style.row && stretch_cross, content_h,
                    style.row && stretch_cross);
    if (fill_share_wrapper) measured = SizeF{content_w, content_h};
    layout.w = measured.w;
    layout.h = measured.h;
    if (style.row) {
      layout.w = std::max(layout.w, MinBoxWidth(child->style));
      // Align-items:stretch sizes the item's *margin box* to the line, so the
      // cross-axis margins come out of the stretched height (a `.tab-list`
      // with `margin: 0 24px` must be 342px wide inside a 390px column).
      if (stretch_cross) {
        layout.h = std::max(0.0f, content_h - child->style.margin[0] -
                                      child->style.margin[2]);
      }
      layout.margin_main_start = child->style.margin[3];
      layout.margin_main_end = child->style.margin[1];
      layout.margin_cross_start = child->style.margin[0];
      layout.margin_cross_end = child->style.margin[2];
      layout.cross = layout.h;
    } else {
      if (stretch_cross) {
        layout.w = std::max(0.0f, content_w - child->style.margin[3] -
                                      child->style.margin[1]);
      }
      // The official engine clamps a flex item's cross size to the line even
      // when the item declares a larger width (app-bar's navigation bar:
      // `width:100vw; padding-right` inside `display:flex`).  Block containers
      // let their children overflow, so this only applies to display:flex.
      if (style.flex_container && !in_scroll_view &&
          EffectiveAlign(style, child->style) == 0 &&
          layout.w > content_w) {
        layout.w = content_w;
      }
      layout.margin_main_start = child->style.margin[0];
      layout.margin_main_end = child->style.margin[2];
      layout.margin_cross_start = child->style.margin[3];
      layout.margin_cross_end = child->style.margin[1];
      layout.cross = layout.w;
    }
    const float measured_main = style.row ? layout.w : layout.h;
    layout.base_main =
        ResolveFlexBasis(child->style, style.row, main_extent, measured_main);
    layout.main = layout.base_main;
    if (style.row) {
      layout.w = layout.base_main;
    } else {
      layout.h = layout.base_main;
    }
    layout.shrink = EffectiveFlexShrink(child->style, in_scroll_view,
                                        style.flex_container);
    if (debug_node_id != 0 && child->id == debug_node_id) {
      std::fprintf(stderr,
                   "[ops-debug] child-layout #%u measured=%gx%g layout=%gx%g "
                   "base_main=%g main_extent=%g content=%gx%g in_scroll=%d "
                   "stretch_cross=%d row=%d flex=%d/%g basis=%g pct=%g\n",
                   child->id, static_cast<double>(measured.w),
                   static_cast<double>(measured.h), static_cast<double>(layout.w),
                   static_cast<double>(layout.h),
                   static_cast<double>(layout.base_main),
                   static_cast<double>(main_extent),
                   static_cast<double>(content_w), static_cast<double>(content_h),
                   in_scroll_view ? 1 : 0, stretch_cross ? 1 : 0,
                   style.row ? 1 : 0, child->style.has_flex ? 1 : 0,
                   static_cast<double>(child->style.flex),
                   static_cast<double>(child->style.flex_basis),
                   static_cast<double>(child->style.flex_basis_percent));
    }
    total_outer_base += layout.main + layout.margin_main_start +
                        layout.margin_main_end;
    if (child->style.has_flex && child->style.flex > 0.0f) {
      grow_sum += child->style.flex;
    }
    shrink_weight_sum += layout.shrink * layout.base_main;
    children.push_back(layout);
  }

  if (style.wrap) {
    // Wrapped flex: break children into lines along the main axis, distribute
    // flex growth per line, then align lines with align-content.
    const float main_gap = MainGap(style);
    const float cross_gap = style.row ? style.row_gap : style.column_gap;
    const float cross_extent = style.row ? content_h : content_w;
    struct WrapLine {
      std::vector<size_t> indices;
      float main = 0.0f;
      float cross = 0.0f;
      float flex_sum = 0.0f;
      float shrink_weight = 0.0f;
    };
    std::vector<WrapLine> lines;
    for (size_t i = 0; i < children.size(); i++) {
      ChildLayout& child = children[i];
      const float outer_main =
          child.main + child.margin_main_start + child.margin_main_end;
      if (lines.empty()) lines.push_back(WrapLine());
      WrapLine* line = &lines.back();
      const float needed =
          line->indices.empty() ? outer_main : line->main + main_gap + outer_main;
      if (!line->indices.empty() && needed > main_extent + 0.01f) {
        lines.push_back(WrapLine());
        line = &lines.back();
      }
      if (!line->indices.empty()) line->main += main_gap;
      line->indices.push_back(i);
      line->main += outer_main;
      line->cross = std::max(
          line->cross,
          child.cross + child.margin_cross_start + child.margin_cross_end);
      if (child.node->style.has_flex && child.node->style.flex > 0.0f) {
        line->flex_sum += child.node->style.flex;
      }
      line->shrink_weight += child.shrink * child.base_main;
    }
    for (WrapLine& line : lines) {
      const float remaining = main_extent - line.main;
      if (remaining > 0.0f && line.flex_sum > 0.0f) {
        for (size_t index : line.indices) {
          ChildLayout& child = children[index];
          if (child.node->style.has_flex && child.node->style.flex > 0.0f) {
            child.main +=
                remaining * child.node->style.flex / line.flex_sum;
            if (style.row) {
              child.w = child.main;
            } else {
              child.h = child.main;
            }
          }
        }
      } else if (remaining < 0.0f && line.shrink_weight > 0.0f) {
        const float overflow = -remaining;
        for (size_t index : line.indices) {
          ChildLayout& child = children[index];
          if (child.shrink <= 0.0f) continue;
          const float amount =
              overflow * (child.shrink * child.base_main) / line.shrink_weight;
          const float min_main =
              style.row ? MinBoxWidth(child.node->style)
                        : MinBoxHeight(child.node->style);
          child.main = std::max(min_main, child.main - amount);
          if (style.row) {
            child.w = child.main;
          } else {
            child.h = child.main;
          }
        }
      }
    }
    float total_cross =
        cross_gap * (lines.size() > 1 ? static_cast<float>(lines.size() - 1)
                                      : 0.0f);
    for (const WrapLine& line : lines) total_cross += line.cross;
    float cross_offset = 0.0f;
    if (style.align_content == 2) {
      cross_offset = (cross_extent - total_cross) / 2.0f;
    } else if (style.align_content == 3) {
      cross_offset = cross_extent - total_cross;
    } else if (style.align_content == 0 && !lines.empty() &&
               total_cross < cross_extent) {
      const float extra = (cross_extent - total_cross) / lines.size();
      for (WrapLine& line : lines) line.cross += extra;
    }
    float line_cross_pos = (style.row ? content_y : content_x) + cross_offset;
    for (WrapLine& line : lines) {
      float used_main = main_gap * (line.indices.size() > 1
                                        ? static_cast<float>(line.indices.size() - 1)
                                        : 0.0f);
      for (size_t index : line.indices) {
        const ChildLayout& child = children[index];
        used_main += child.main + child.margin_main_start +
                     child.margin_main_end;
      }
      float leftover = main_extent - used_main;
      if (leftover < 0.0f) leftover = 0.0f;
      float justify_offset = 0.0f;
      float extra_gap = 0.0f;
      if (style.justify == 1) {
        justify_offset = leftover / 2.0f;
      } else if (style.justify == 2) {
        justify_offset = leftover;
      } else if (style.justify == 3 && line.indices.size() > 1) {
        extra_gap = leftover / static_cast<float>(line.indices.size() - 1);
      }
      float cursor = (style.row ? content_x : content_y) + justify_offset;
      for (size_t index : line.indices) {
        ChildLayout& child = children[index];
        const int align = EffectiveAlign(style, child.node->style);
        if (align == 0) {
          if (style.row && !HasExplicitHeight(child.node->style)) {
            child.cross = std::max(0.0f, line.cross -
                                             child.margin_cross_start -
                                             child.margin_cross_end);
            child.h = child.cross;
          } else if (!style.row && !HasExplicitWidth(child.node->style)) {
            child.cross = std::max(0.0f, line.cross -
                                             child.margin_cross_start -
                                             child.margin_cross_end);
            child.w = child.cross;
          }
        }
        float child_x = 0.0f;
        float child_y = 0.0f;
        if (style.row) {
          child_y = line_cross_pos;
          const float cross_space = line.cross - child.cross -
                                    child.margin_cross_start -
                                    child.margin_cross_end;
          if (align == 2) {
            child_y += child.margin_cross_start + cross_space / 2.0f;
          } else if (align == 3) {
            child_y += child.margin_cross_start + cross_space;
          } else {
            child_y += child.margin_cross_start;
          }
          cursor += child.margin_main_start;
          child_x = cursor;
          cursor += child.main + child.margin_main_end + main_gap + extra_gap;
        } else {
          child_x = line_cross_pos;
          const float cross_space = line.cross - child.cross -
                                    child.margin_cross_start -
                                    child.margin_cross_end;
          if (align == 2) {
            child_x += child.margin_cross_start + cross_space / 2.0f;
          } else if (align == 3) {
            child_x += child.margin_cross_start + cross_space;
          } else {
            child_x += child.margin_cross_start;
          }
          cursor += child.margin_main_start;
          child_y = cursor;
          cursor += child.main + child.margin_main_end + main_gap + extra_gap;
        }
        LayoutNode(child.node, child_x, child_y, child.w, child.h, metrics,
                   abs_x + child_x, abs_y + child_y, cb_abs_x, cb_abs_y, cb_w,
                   cb_h);
      }
      line_cross_pos += line.cross + cross_gap;
    }
  } else {
  const float gaps =
      children.size() > 1
          ? MainGap(style) * static_cast<float>(children.size() - 1)
          : 0.0f;
  float free = main_extent - total_outer_base - gaps;
  if (free > 0.0f && grow_sum > 0.0f) {
    for (ChildLayout& child : children) {
      if (child.node->style.has_flex && child.node->style.flex > 0.0f) {
        child.main += free * child.node->style.flex / grow_sum;
        if (style.row) {
          child.w = child.main;
        } else {
          child.h = child.main;
        }
      }
    }
  } else if (free < 0.0f && shrink_weight_sum > 0.0f) {
    const float overflow = -free;
    for (ChildLayout& child : children) {
      if (child.shrink <= 0.0f) continue;
      const float amount =
          overflow * (child.shrink * child.base_main) / shrink_weight_sum;
      const float min_main = style.row ? MinBoxWidth(child.node->style)
                                       : MinBoxHeight(child.node->style);
      child.main = std::max(min_main, child.main - amount);
      if (style.row) {
        child.w = child.main;
      } else {
        child.h = child.main;
      }
    }
  }

  float total_main = gaps;
  for (const ChildLayout& child : children) {
    total_main += child.main + child.margin_main_start + child.margin_main_end;
  }
  float leftover = main_extent - total_main;
  if (leftover < 0.0f) leftover = 0.0f;

  float justify_offset = 0.0f;
  float extra_gap = 0.0f;
  if (style.justify == 1) {
    justify_offset = leftover / 2.0f;
  } else if (style.justify == 2) {
    justify_offset = leftover;
  } else if (style.justify == 3 && children.size() > 1) {
    extra_gap = leftover / static_cast<float>(children.size() - 1);
  }

  float cursor = (style.row ? content_x : content_y) + justify_offset;
  for (ChildLayout& child : children) {
    cursor += child.margin_main_start;
    const int align = EffectiveAlign(style, child.node->style);
    float child_x = 0.0f;
    float child_y = 0.0f;
    if (style.row) {
      child_y = content_y;
      const float cross_space =
          content_h - child.cross - child.margin_cross_start -
          child.margin_cross_end;
      if (align == 2) {
        child_y += child.margin_cross_start + cross_space / 2.0f;
      } else if (align == 3) {
        child_y += child.margin_cross_start + cross_space;
      } else {
        child_y += child.margin_cross_start;
      }
      child_x = cursor;
    } else {
      child_x = content_x;
      const float cross_space =
          content_w - child.cross - child.margin_cross_start -
          child.margin_cross_end;
      if (align == 2) {
        child_x += child.margin_cross_start + cross_space / 2.0f;
      } else if (align == 3) {
        child_x += child.margin_cross_start + cross_space;
      } else {
        child_x += child.margin_cross_start;
      }
      child_y = cursor;
    }
    LayoutNode(child.node, child_x, child_y, child.w, child.h, metrics,
               abs_x + child_x, abs_y + child_y, cb_abs_x, cb_abs_y, cb_w,
               cb_h);
    cursor += child.main + child.margin_main_end + MainGap(style) + extra_gap;
  }
  }  // !style.wrap
  }  // !IsInlineFlowContainer

  // Absolute/fixed children: absolute boxes resolve against the nearest
  // positioned ancestor's padding box (`cb_*`), fixed boxes against the
  // viewport.  Both are excluded from flex sizing above.
  for (AppNode* child : node->children) {
    if (child->released || child->style.display_none ||
        (child->style.position != 1 && child->style.position != 2)) {
      continue;
    }
    const bool fixed = child->style.position == 2;
    const float inset_w =
        fixed ? (g_viewport_width > 0.0
                     ? static_cast<float>(g_viewport_width)
                     : cb_w)
              : cb_w;
    const float inset_h =
        fixed ? (g_viewport_height > 0.0
                     ? static_cast<float>(g_viewport_height)
                     : cb_h)
              : cb_h;
    float left = 0.0f;
    float right = 0.0f;
    float top = 0.0f;
    float bottom = 0.0f;
    const bool has_left = ResolveInset(child->style, 3, inset_w, &left);
    const bool has_right = ResolveInset(child->style, 1, inset_w, &right);
    const bool has_top = ResolveInset(child->style, 0, inset_h, &top);
    const bool has_bottom = ResolveInset(child->style, 2, inset_h, &bottom);

    // Yoga (the official engine) applies margins to absolute boxes differently
    // from CSS 2.1: the leading margin (left/top in LTR) is always added to the
    // resolved position, the trailing margin only shrinks a stretched box.
    // Verified against the Windows engine (`appservice-absolute-margin`):
    // `left:0;margin:0 21px` lands at x=21, `right:0;width:30;margin:0 10px`
    // lands at x=width-30+10, and a bottom-anchored box ignores margin-bottom.
    const float margin_left = child->style.margin[3];
    const float margin_right = child->style.margin[1];
    const float margin_top = child->style.margin[0];
    const float margin_bottom = child->style.margin[2];

    float box_w = -1.0f;
    float box_h = -1.0f;
    if (has_left && has_right && !HasExplicitWidth(child->style)) {
      box_w = std::max(0.0f, inset_w - left - right - margin_left -
                                 margin_right);
    }
    if (has_top && has_bottom && !HasExplicitHeight(child->style)) {
      box_h = std::max(0.0f, inset_h - top - bottom - margin_top -
                                 margin_bottom);
    }
    SizeF measured = MeasureNode(child, box_w >= 0.0f ? box_w : inset_w,
                                 box_w >= 0.0f, box_h >= 0.0f ? box_h : inset_h,
                                 box_h >= 0.0f, inset_w);
    float child_w = box_w >= 0.0f ? box_w : measured.w;
    float child_h = box_h >= 0.0f ? box_h : measured.h;
    child_w = std::max(child_w, MinBoxWidth(child->style));
    child_h = std::max(child_h, MinBoxHeight(child->style));
    child->measured_w = child_w;
    child->measured_h = child_h;

    float child_x = content_x;
    float child_y = content_y;
    if (fixed) {
      // Viewport coordinates, then converted back to the parent's coordinate
      // space because the layout buffer is parent-relative.
      float viewport_x = 0.0f;
      float viewport_y = 0.0f;
      if (has_left) {
        viewport_x = left + margin_left;
      } else if (has_right) {
        viewport_x = inset_w - right - child_w + margin_left;
      } else {
        // No horizontal inset: keep the static position (parent content origin).
        viewport_x = abs_x + content_x + margin_left;
      }
      if (has_top) {
        viewport_y = top + margin_top;
      } else if (has_bottom) {
        viewport_y = inset_h - bottom - child_h + margin_top;
      } else {
        viewport_y = abs_y + content_y + margin_top;
      }
      child_x = viewport_x - abs_x;
      child_y = viewport_y - abs_y;
    } else {
      // `cb_abs_*` is the containing block's padding-box origin, so the child's
      // insets are viewport-relative; convert back to parent-relative.
      if (has_left) {
        child_x = cb_abs_x + left + margin_left - abs_x;
      } else if (has_right) {
        child_x = cb_abs_x + cb_w - right - child_w + margin_left - abs_x;
      } else {
        child_x = content_x + margin_left;
      }
      if (has_top) {
        child_y = cb_abs_y + top + margin_top - abs_y;
      } else if (has_bottom) {
        child_y = cb_abs_y + cb_h - bottom - child_h + margin_top - abs_y;
      } else {
        child_y = content_y + margin_top;
      }
    }
    LayoutNode(child, child_x, child_y, child_w, child_h, metrics,
               abs_x + child_x, abs_y + child_y, cb_abs_x, cb_abs_y, cb_w,
               cb_h);
  }
}

void WriteLayoutMetrics(const std::vector<NodeMetric>& metrics) {
  for (const NodeMetric& metric : metrics) {
    if (metric.node == nullptr || metric.node->layout_buffer_p == nullptr) {
      continue;
    }
    Dart_Handle buffer =
        Dart_HandleFromPersistent(metric.node->layout_buffer_p);
    Dart_TypedData_Type type = Dart_TypedData_kInvalid;
    void* data = nullptr;
    intptr_t length = 0;
    Dart_Handle status =
        Dart_TypedDataAcquireData(buffer, &type, &data, &length);
    if (!Dart_IsError(status) && type == Dart_TypedData_kFloat32 &&
        length == 9 && data != nullptr) {
      float* values = static_cast<float*>(data);
      values[0] = metric.y;
      values[1] = metric.x;
      values[2] = metric.w;
      values[3] = metric.h;
      values[4] = static_cast<float>(metric.node->style.z_index);
      for (int i = 5; i < 9; i++) values[i] = 0.0f;
      Dart_TypedDataReleaseData(buffer);
    }
  }
}

// Flutter places slivers itself, so a sliver child's YGNode buffer must hold
// positions relative to the sliver origin.  The page-wide pass stores them
// relative to the enclosing box; replaying that buffer inside a sliver makes
// YGNodeRenderObject.paint add the offset a second time (a header after a
// 166 px block painted at 332 px).  Re-run the subtree's layout at the origin
// before handing it to NewStickySection / NewScrollView.
void ResetNodeLayoutToOrigin(AppNode* node) {
  if (node == nullptr) return;
  node->sliver_layout = true;
  std::vector<NodeMetric> metrics;
  // Keep the window-absolute origin: only the YGNode paint/hit buffers must be
  // sliver-relative, while selector queries need viewport coordinates.
  LayoutNode(node, 0.0f, 0.0f, node->measured_w, node->measured_h, &metrics,
             node->abs_x, node->abs_y);
  WriteLayoutMetrics(metrics);
}

// Restores origin-relative buffers for every sliver-hosted subtree after a
// page-wide LayoutNode/WriteLayoutMetrics pass (targeted updates, animations).
// Scroll-view children are Flutter slivers: the sliver positions them by their
// widget size and ignores the layout offsets.  Restore the vertical margins in
// the root YGNode's layout buffer so each child reserves `margin-top` above and
// `margin-bottom` below its content (otherwise the list extent is short and the
// last rows cannot scroll fully into view).
void ApplySliverMargins(AppNode* node) {
  if (node == nullptr || node->layout_buffer_p == nullptr) return;
  const AppStyle& style = node->style;
  const float margin_top = style.margin[0];
  const float margin_bottom = style.margin[2];
  const float margin_left = style.margin[3];
  const float margin_right = style.margin[1];
  if (margin_top == 0.0f && margin_bottom == 0.0f && margin_left == 0.0f &&
      margin_right == 0.0f) {
    return;
  }
  Dart_Handle buffer = Dart_HandleFromPersistent(node->layout_buffer_p);
  if (buffer == nullptr || Dart_IsError(buffer) || Dart_IsNull(buffer)) return;
  Dart_TypedData_Type type = Dart_TypedData_kInvalid;
  void* data = nullptr;
  intptr_t length = 0;
  if (Dart_IsError(Dart_TypedDataAcquireData(buffer, &type, &data, &length))) {
    return;
  }
  if (type == Dart_TypedData_kFloat32 && length >= 9 && data != nullptr) {
    float* values = static_cast<float*>(data);
    values[0] = margin_top;                       // paint y offset
    values[1] = margin_left;                      // paint x offset
    values[7] = margin_top + margin_bottom;       // root size margin block
    values[8] = margin_left + margin_right;       // root size margin inline
  }
  Dart_TypedDataReleaseData(buffer);
}

void RestoreSliverLayoutMetrics() {
  if (g_page == nullptr) return;
  for (auto& entry : g_page->nodes) {
    AppNode* node = entry.second;
    if (node == nullptr || node->released || !node->sliver_layout) continue;
    if (node->parent != nullptr && node->parent->sliver_layout) continue;
    std::vector<NodeMetric> metrics;
    LayoutNode(node, 0.0f, 0.0f, node->measured_w, node->measured_h, &metrics,
               node->abs_x, node->abs_y);
    WriteLayoutMetrics(metrics);
    ApplySliverMargins(node);
  }
}

// A structural rebuild re-derives the sliver roots while building widgets.
void ClearSliverLayoutMarks() {
  if (g_page == nullptr) return;
  for (auto& entry : g_page->nodes) {
    if (entry.second != nullptr) entry.second->sliver_layout = false;
  }
}

}  // namespace appservice

