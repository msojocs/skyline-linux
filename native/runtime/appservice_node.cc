// Node tree operations, attributes, text metrics and node predicates.

#include "internal/skyline_runtime_module.hh"
#include "internal/skyline_runtime_node.hh"

namespace appservice {

AppNode* NewNode(uint32_t id, const std::string& tag, const std::string& name) {
  AppNode* node = new AppNode();
  node->id = id;
  node->tag = tag;
  node->name = name;
  node->address = g_next_address;
  g_next_address += 0x100;
  g_node_address_ids[node->address] = id;
  if (g_page != nullptr) g_page->nodes[id] = node;
  return node;
}

AppNode* FindNode(uint32_t id) {
  if (g_page == nullptr) return nullptr;
  const auto found = g_page->nodes.find(id);
  return found == g_page->nodes.end() ? nullptr : found->second;
}

void DetachFromParent(AppNode* node) {
  if (node == nullptr || node->parent == nullptr) return;
  auto& siblings = node->parent->children;
  siblings.erase(std::remove(siblings.begin(), siblings.end(), node),
                 siblings.end());
  node->parent = nullptr;
}

void AppendChild(AppNode* parent, AppNode* child) {
  if (parent == nullptr || child == nullptr) return;
  DetachFromParent(child);
  parent->children.push_back(child);
  child->parent = parent;
}

void InsertBefore(AppNode* parent, AppNode* child, AppNode* before) {
  if (parent == nullptr || child == nullptr) return;
  DetachFromParent(child);
  auto& siblings = parent->children;
  const auto found = std::find(siblings.begin(), siblings.end(), before);
  const auto position = found == siblings.end() ? siblings.end() : found;
  siblings.insert(position, child);
  child->parent = parent;
}

void RemoveChild(AppNode* parent, AppNode* child) {
  if (parent == nullptr || child == nullptr) return;
  if (child->parent != parent) return;
  DetachFromParent(child);
}

void SetClasses(AppNode* node, const std::vector<std::string>& classes) {
  node->classes = classes;
}

std::vector<std::string> SplitClasses(const std::string& names) {
  std::vector<std::string> classes;
  for (const std::string& name : SplitWhitespace(names)) {
    if (!name.empty()) classes.push_back(name);
  }
  return classes;
}

void Splice(AppNode* parent, int position, int count,
            const std::vector<AppNode*>& items) {
  if (parent == nullptr) return;
  auto& children = parent->children;
  if (position < 0 || position > static_cast<int>(children.size())) {
    position = static_cast<int>(children.size());
  }
  if (count < 0) count = 0;
  if (position + count > static_cast<int>(children.size())) {
    count = static_cast<int>(children.size()) - position;
  }
  for (int i = 0; i < count; i++) {
    AppNode* removed = children[position + i];
    removed->parent = nullptr;
  }
  children.erase(children.begin() + position, children.begin() + position + count);
  int insert_at = position;
  for (AppNode* item : items) {
    if (item == nullptr) continue;
    DetachFromParent(item);
    if (item->tag == "fragment") {
      std::vector<AppNode*> nested = item->children;
      item->children.clear();
      for (AppNode* child : nested) {
        child->parent = parent;
        children.insert(children.begin() + insert_at++, child);
      }
      continue;
    }
    item->parent = parent;
    children.insert(children.begin() + insert_at++, item);
  }
}

void CancelPendingImage(AppNode* node);

void ApplyAttribute(AppNode* node, const std::string& name,
                    const std::string& value) {
  if (name == "class") {
    SetClasses(node, SplitClasses(value));
  } else if (name == "style") {
    if (!node->inline_style.empty() && node->inline_style.back() != ';') {
      node->inline_style.push_back(';');
    }
    node->inline_style += value;
  } else if (name == "src" || name == "url" || name == "imageUrl" ||
             name == "image" || name == "path") {
    const auto previous = node->attrs.find(name);
    if (previous == node->attrs.end() || previous->second != value) {
      CancelPendingImage(node);
    }
    node->attrs[name] = value;
  } else {
    node->attrs[name] = value;
  }
}

// -- measurement / layout ----------------------------------------------------

const char* NodeText(const AppNode* node, std::string* storage) {
  if (!node->text.empty()) return node->text.c_str();
  storage->clear();
  for (const AppNode* child : node->children) {
    if (child->released || child->style.display_none) continue;
    std::string nested;
    storage->append(NodeText(child, &nested));
  }
  return storage->c_str();
}

// Variation selectors (U+FE0E / U+FE0F) only request a presentation form and
// have no advance width.  When the selected text font lacks the emoji ligature
// it paints/meters them as a second full-width glyph, doubling the advance of
// `❤️`/`⭐️` (26px emoji measured 52px).  The replayed text carries no useful
// variation-selector information, so drop them before measuring/rendering.
std::string StripVariationSelectors(const std::string& raw) {
  std::string out;
  out.reserve(raw.size());
  for (size_t i = 0; i < raw.size();) {
    if (i + 2 < raw.size() &&
        static_cast<unsigned char>(raw[i]) == 0xEF &&
        static_cast<unsigned char>(raw[i + 1]) == 0xB8 &&
        (static_cast<unsigned char>(raw[i + 2]) == 0x8E ||
         static_cast<unsigned char>(raw[i + 2]) == 0x8F)) {
      i += 3;
      continue;
    }
    out.push_back(raw[i++]);
  }
  return out;
}

// WXML text follows CSS `white-space: normal`: whitespace runs collapse to a
// single space and leading/trailing whitespace is dropped.  The SDK hands us
// the raw template text (indentation and newlines included), which otherwise
// measures as a huge multiline block (e.g. the A-Z popup letter).
//
// `preserve_leading`/`preserve_trailing` implement CSS whitespace collapsing
// across inline boxes: a raw text run that follows/precedes another inline box
// keeps one collapsed space, while the same whitespace at the edge of a line
// is dropped (ComputeInlineWhitespace sets the flags).
std::string NormalizeTextWhitespace(const std::string& raw_input,
                                    int white_space = 0,
                                    bool preserve_leading = false,
                                    bool preserve_trailing = false) {
  const std::string raw = StripVariationSelectors(raw_input);
  const auto is_space = [](unsigned char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
  };
  const bool leading_ws =
      !raw.empty() && is_space(static_cast<unsigned char>(raw.front()));
  const bool trailing_ws =
      !raw.empty() && is_space(static_cast<unsigned char>(raw.back()));
  // `pre`/`pre-wrap` preserve spaces and newlines verbatim.
  if (white_space == 2 || white_space == 3) return raw;
  std::string out;
  out.reserve(raw.size());
  if (white_space == 4) {
    // `pre-line` collapses runs of spaces/tabs but preserves newlines.
    bool pending_space = false;
    for (const unsigned char c : raw) {
      if (c == '\n') {
        out.push_back('\n');
        pending_space = false;
      } else if (is_space(c)) {
        pending_space = !out.empty() && out.back() != '\n';
      } else {
        if (pending_space) out.push_back(' ');
        pending_space = false;
        out.push_back(static_cast<char>(c));
      }
    }
  } else {
    // normal/nowrap: whitespace runs collapse to a single space and
    // leading/trailing whitespace is dropped.
    bool pending_space = false;
    for (const unsigned char c : raw) {
      if (is_space(c)) {
        pending_space = !out.empty();
        continue;
      }
      if (pending_space) {
        out.push_back(' ');
        pending_space = false;
      }
      out.push_back(static_cast<char>(c));
    }
  }
  if (preserve_leading && leading_ws) out.insert(out.begin(), ' ');
  if (preserve_trailing && trailing_ws) out.push_back(' ');
  return out;
}

// Node-aware text normalization: keeps the inline-flow edge spaces computed by
// ComputeInlineWhitespace.
std::string NodeNormalizedText(const AppNode* node) {
  std::string storage;
  const std::string raw = NodeText(node, &storage);
  return NormalizeTextWhitespace(raw, node->style.white_space,
                                 node->inline_preserve_leading,
                                 node->inline_preserve_trailing);
}

// Skyline's default WordBreak is breakAll; Flutter's public paragraph builder
// does not expose that switch, so the wrapping path below uses Flutter's
// default word wrapping.

double GlyphWidth(uint32_t codepoint, double font_size) {
  if (codepoint == ' ') return 0.30 * font_size;
  if (codepoint == '\t') return 1.20 * font_size;
  if (codepoint < 0x20) return 0.0;
  if (codepoint < 0x80) {
    if ((codepoint >= '0' && codepoint <= '9') ||
        (codepoint >= 'A' && codepoint <= 'Z') ||
        (codepoint >= 'a' && codepoint <= 'z')) {
      return 0.56 * font_size;
    }
    if (codepoint == '.' || codepoint == ',' || codepoint == ':' ||
        codepoint == '\'' || codepoint == ';') {
      return 0.28 * font_size;
    }
    return 0.50 * font_size;
  }
  return font_size;  // CJK / full width
}

// Decodes UTF-8 lazily; invalid bytes are treated as single-byte glyphs.
uint32_t NextCodepoint(const std::string& text, size_t* index) {
  const unsigned char c = static_cast<unsigned char>(text[*index]);
  if (c < 0x80) {
    (*index)++;
    return c;
  }
  int extra = 0;
  uint32_t value = 0;
  if ((c & 0xE0) == 0xC0) {
    extra = 1;
    value = c & 0x1F;
  } else if ((c & 0xF0) == 0xE0) {
    extra = 2;
    value = c & 0x0F;
  } else if ((c & 0xF8) == 0xF0) {
    extra = 3;
    value = c & 0x07;
  } else {
    (*index)++;
    return c;
  }
  (*index)++;
  for (int i = 0; i < extra && *index < text.size(); i++) {
    const unsigned char next = static_cast<unsigned char>(text[(*index)++]);
    if ((next & 0xC0) != 0x80) return '?';
    value = (value << 6) | (next & 0x3F);
  }
  return value;
}

double TextLineHeight(const AppStyle& style) {
  if (style.line_height > 0) return style.line_height;
  return style.font_size * style.line_height_factor;
}

double TextIntrinsicWidth(const std::string& text, const AppStyle& style) {
  size_t index = 0;
  double width = 0.0;
  while (index < text.size()) {
    const uint32_t codepoint = NextCodepoint(text, &index);
    if (codepoint == '\n') continue;
    width += GlyphWidth(codepoint, style.font_size);
    if (!std::isnan(style.letter_spacing)) width += style.letter_spacing;
  }
  return width;
}

int CountTextLines(const std::string& text, double max_width,
                   const AppStyle& style) {
  // white-space:nowrap collapses newlines to spaces and never wraps.
  if (style.white_space == 1) return 1;
  // white-space:pre keeps hard newlines but never soft-wraps.
  if (style.white_space == 2) {
    int lines = 1;
    for (char c : text) {
      if (c == '\n') lines++;
    }
    return lines;
  }
  if (max_width <= 0.0 || max_width >= kInf) {
    int lines = 1;
    for (char c : text) {
      if (c == '\n') lines++;
    }
    return lines;
  }
  int lines = 1;
  double line_width = 0.0;
  size_t index = 0;
  while (index < text.size()) {
    const uint32_t codepoint = NextCodepoint(text, &index);
    if (codepoint == '\n') {
      lines++;
      line_width = 0.0;
      continue;
    }
    const double glyph = GlyphWidth(codepoint, style.font_size);
    if (line_width > 0.0 && line_width + glyph > max_width + 0.01) {
      lines++;
      line_width = 0.0;
    }
    line_width += glyph;
    if (!std::isnan(style.letter_spacing)) line_width += style.letter_spacing;
  }
  return lines;
}

bool HasExplicitWidth(const AppStyle& style) {
  return style.width >= 0.0f || style.width_percent >= 0.0f;
}

bool HasExplicitHeight(const AppStyle& style) {
  return style.height >= 0.0f || style.height_percent >= 0.0f;
}

// Main-axis gap: row flex uses column-gap, column flex uses row-gap.
float MainGap(const AppStyle& style) {
  if (style.row) {
    return style.column_gap > 0.0f ? style.column_gap : style.gap;
  }
  return style.row_gap > 0.0f ? style.row_gap : style.gap;
}

int EffectiveAlign(const AppStyle& parent, const AppStyle& child) {
  return child.align_self >= 0 ? child.align_self : parent.align;
}

// Flow children in CSS `order` order (stable for equal order values).
std::vector<AppNode*> OrderedFlowChildren(AppNode* node) {
  std::vector<AppNode*> flow;
  for (AppNode* child : node->children) {
    if (child == nullptr || child->released || child->style.display_none ||
        (child->style.position != 0 && child->style.position != 3 &&
         child->style.position != 4)) {
      continue;
    }
    flow.push_back(child);
  }
  std::stable_sort(flow.begin(), flow.end(),
                   [](const AppNode* a, const AppNode* b) {
                     return a->style.order < b->style.order;
                   });
  return flow;
}

// A block container whose in-flow children are all inline-level (text or
// display:inline-block/inline).  CSS wraps them into line boxes; the official
// engine bottom-aligns inline-block children on the line.  Pure text-only
// containers keep the existing stretched-text path so text-align still applies
// across the full container width.
bool IsInlineFlowContainer(AppNode* node) {
  if (node == nullptr || node->style.row || node->style.wrap) return false;
  const std::vector<AppNode*> flow = OrderedFlowChildren(node);
  // A block whose only flow content is one inline text element behaves like a
  // text block (`.good-title > text`): keep the stretched-text path so the
  // text wraps at the container width.  Measuring it as an inline box would
  // shrink-wrap it to its intrinsic width, which then overflows or clips the
  // card (and wraps around an underestimate of the real glyph widths).
  if (flow.size() == 1) {
    const AppNode* only = flow.front();
    const bool is_text_element =
        only->name == "text" || only->name == "span" ||
        (!only->name.empty() && (only->tag == "text" || only->tag == "span"));
    if (is_text_element) return false;
  }
  bool has_inline_element = false;
  for (AppNode* child : flow) {
    // Raw text nodes (tag "text", no element name) are inline runs, but a
    // text-only container keeps the stretched-text path so text-align applies
    // across the full container width.  `<text>` element wrappers carry a
    // name and are inline boxes themselves (Skyline's default display).
    if (child->tag == "text" && child->name.empty()) continue;
    if (child->style.inline_block || child->style.inline_level) {
      has_inline_element = true;
      continue;
    }
    return false;
  }
  return has_inline_element;
}

// CSS whitespace collapsing across inline boxes.  A raw text run between two
// inline boxes keeps one collapsed space on the side that has adjacent content
// (`<text>⭐️</text>\n  723` renders as "⭐️ 723"); whitespace at the start/end
// of the inline flow is dropped.  Runs in the page-wide measure/build passes
// after ResolveStyles so the flags are available to text measurement and
// widget creation.
void ComputeInlineWhitespace(AppNode* node) {
  if (node == nullptr) return;
  node->inline_preserve_leading = false;
  node->inline_preserve_trailing = false;
  // Recurse first: a child resets its own flags on entry, so the containing
  // inline flow must assign them afterwards.
  for (AppNode* child : node->children) ComputeInlineWhitespace(child);
  if (IsInlineFlowContainer(node)) {
    const std::vector<AppNode*> flow = OrderedFlowChildren(node);
    const auto is_space = [](unsigned char c) {
      return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
    };
    for (size_t i = 0; i < flow.size(); ++i) {
      AppNode* child = flow[i];
      if (child->tag != "text") continue;
      std::string storage;
      const std::string raw = NodeText(child, &storage);
      if (raw.empty()) continue;
      if (i > 0 && is_space(static_cast<unsigned char>(raw.front()))) {
        child->inline_preserve_leading = true;
      }
      if (i + 1 < flow.size() &&
          is_space(static_cast<unsigned char>(raw.back()))) {
        child->inline_preserve_trailing = true;
      }
    }
  }
}

// glass-easel splices a fragment into its parent before appending its children
// (e.g. the sticky-section/list-view structure), so the insert-time flattening
// in Splice/AppendChild only sees an empty fragment.  Flatten any remaining
// live fragments in place before layout/widget building.
bool FlattenFragments(AppNode* node) {
  if (node == nullptr) return false;
  bool changed = false;
  std::vector<AppNode*> flat;
  flat.reserve(node->children.size());
  for (AppNode* child : node->children) {
    if (child == nullptr) continue;
    if (child->released) continue;
    if (child->tag == "fragment") {
      changed = true;
      FlattenFragments(child);
      for (AppNode* grand : child->children) {
        if (grand == nullptr) continue;
        grand->parent = node;
        flat.push_back(grand);
      }
      child->children.clear();
      child->parent = nullptr;
    } else {
      // Keep the explicit `stickysection` wrapper: it becomes a proper
      // MultiSliver section with pinned headers.  Flattening it into the
      // scroll view used to work around missing pixels, but it breaks the
      // pinned-header paint math at large scroll offsets (headers stack up).
      if (FlattenFragments(child)) changed = true;
      flat.push_back(child);
    }
  }
  if (changed) node->children.swap(flat);
  return changed;
}

bool IsRootPortalContentName(const std::string& name) {
  return name == "root-portal-content" || name == "rootportalcontent" ||
         name == "root-portal" || name == "rootportal";
}

// The appservice root-portal adapter creates a detached `root-portal-content`
// node and (in the webview renderer) appends it to the document root.  In the
// Skyline op stream the node and its subtree are created but never attached,
// so the A-Z side index in the address-book sample disappears.  Hoist those
// detached portals to the page root; their style is fixed/full-viewport (see
// ResolveStyles), so they behave like the CSS `fixed` overlay they replace.
bool AttachDetachedPortalContent(AppNode* root) {
  if (root == nullptr || g_page == nullptr) return false;
  bool changed = false;
  for (auto& entry : g_page->nodes) {
    AppNode* node = entry.second;
    if (node == nullptr || node == root || node->released ||
        !IsRootPortalContentName(node->name)) {
      continue;
    }
    if (node->parent != nullptr) {
      // The root-portal adapter attaches its content to the PageContext's
      // `empty/root` node (getRootNode) rather than the page element on later
      // data updates; render it at the page root instead.
      if (node->parent->name == "root") {
        DetachFromParent(node);
      } else {
        if (std::getenv("SKYLINE_APP_SERVICE_DEBUG") != nullptr) {
          std::fprintf(stderr,
                       "[skyline-runtime] portal #%u kept under #%u/%s%s\n",
                       node->id, node->parent->id, node->parent->name.c_str(),
                       node->parent->released ? " (released)" : "");
        }
        continue;
      }
    }
    AppendChild(root, node);
    changed = true;
    if (std::getenv("SKYLINE_APP_SERVICE_DEBUG") != nullptr) {
      std::fprintf(stderr, "[skyline-runtime] attached detached portal #%u (%s)\n",
                   node->id, node->name.c_str());
    }
  }
  return changed;
}

// Resolves a CSS inset (top/right/bottom/left) against the parent content box.
// Returns false when the inset is `auto`.
bool ResolveInset(const AppStyle& style, int index, float content_extent,
                  float* out) {
  const float percent = style.inset_percent[index];
  if (!std::isnan(percent) && percent >= 0.0f) {
    *out = content_extent * percent;
    return true;
  }
  const float px = style.inset[index];
  if (!std::isnan(px)) {
    *out = px;
    return true;
  }
  return false;
}

// Forward declarations: image/builder helpers live with the widget
// materialization code, but measurement needs them.
bool AttrBool(const AppNode* node, const char* key, bool fallback) {
  const auto found = node->attrs.find(key);
  if (found == node->attrs.end()) return fallback;
  const std::string& value = found->second;
  return value == "1" || value == "true" || value == "True";
}

double AttrNumber(const AppNode* node, const char* key, double fallback) {
  const auto found = node->attrs.find(key);
  if (found == node->attrs.end()) return fallback;
  double value = fallback;
  if (!ParseNumber(found->second, &value)) return fallback;
  return value;
}

std::string AttrText(const AppNode* node,
                     const char* key,
                     const char* fallback) {
  const auto found = node->attrs.find(key);
  if (found == node->attrs.end() || found->second.empty()) return fallback;
  return found->second;
}

// `<grid-view>` properties arrive as camelCase attributes: `type` is the
// string "masonry"/"aligned" (the SDK sends `type: "masonry"` in a bulk
// setAttributes op), and the gaps are `mainAxisGap`/`crossAxisGap`.  The older
// numeric `0 = masonry, 1 = aligned` form is still accepted for recorded
// fixtures.
bool GridIsMasonry(const AppNode* node) {
  const std::string type = AttrText(node, "type", "");
  if (type == "masonry") return true;
  if (type == "aligned") return false;
  return AttrNumber(node, "type", 1.0) == 0.0;
}

double GridMainGap(const AppNode* node) {
  return AttrNumber(node, "mainAxisGap",
                    AttrNumber(node, "mainAxisSpacing", 0.0));
}

double GridCrossGap(const AppNode* node) {
  return AttrNumber(node, "crossAxisGap",
                    AttrNumber(node, "crossAxisSpacing", 0.0));
}

void DeletePersistent(Dart_PersistentHandle* handle) {
  if (*handle != nullptr) {
    Dart_DeletePersistentHandle(*handle);
    *handle = nullptr;
  }
}

// Drops all persistent widget handles for a subtree.  Called before a coarse
// rebuild; `include_layout` also releases the 9-float buffers so new ones can
// be allocated for the rebuilt widgets.
void ResetNodeHandlesSelf(AppNode* node, bool include_layout) {
  DeletePersistent(&node->decorated_box_p);
  DeletePersistent(&node->opacity_widget_p);
  DeletePersistent(&node->clip_widget_p);
  DeletePersistent(&node->text_widget_p);
  DeletePersistent(&node->text_span_p);
  if (node->texture_widget_p != nullptr) {
    // Let Dart clear SkylineTextureChannel state before the handle goes away.
    if (g_texture_dispose != nullptr) {
      Dart_Handle texture = Dart_HandleFromPersistent(node->texture_widget_p);
      if (!Dart_IsError(texture)) g_texture_dispose(texture);
    }
    DeletePersistent(&node->texture_widget_p);
    node->texture_widget = nullptr;
  }
  if (node->texture_view_id >= 0) {
    if (std::getenv("SKYLINE_APP_SERVICE_DEBUG") != nullptr) {
      std::fprintf(stderr,
                   "[skyline-runtime] native view dispose: viewId=%d\n",
                   node->texture_view_id);
    }
    g_texture_nodes.erase(node->texture_view_id);
    node->texture_view_id = -1;
  }
  DeletePersistent(&node->yoga_node_p);
  DeletePersistent(&node->scroll_view_p);
  DeletePersistent(&node->transform_p);
  DeletePersistent(&node->input_widget_p);
  DeletePersistent(&node->semantics_p);
  DeletePersistent(&node->snapshot_p);
  DeletePersistent(&node->hero_p);
  node->semantics_widget = nullptr;
  node->snapshot_widget = nullptr;
  node->hero_widget = nullptr;
  node->input_widget = nullptr;
  if (node->builder_p != nullptr || !node->builder_items.empty()) {
    g_builders_by_address.erase(node->address);
    for (auto& entry : node->builder_items) {
      Dart_DeletePersistentHandle(entry.second);
    }
    node->builder_items.clear();
    node->builder_item_nodes.clear();
    node->builder_item_signatures.clear();
    node->builder_pending.clear();
    DeletePersistent(&node->builder_p);
  }
  g_image_nodes.erase(node->address);
  DeletePersistent(&node->image_widget_p);
  g_background_image_nodes.erase(node->address);
  if (include_layout) DeletePersistent(&node->layout_buffer_p);
  node->decorated_box = nullptr;
  node->opacity_widget = nullptr;
  node->text_widget = nullptr;
  node->connector = nullptr;
}

void ResetNodeHandles(AppNode* node, bool include_layout) {
  if (node == nullptr) return;
  ResetNodeHandlesSelf(node, include_layout);
  for (AppNode* child : node->children) {
    ResetNodeHandles(child, include_layout);
  }
}

// Like `ResetNodeHandles`, but never descends into a covered route's subtree.
void ResetNodeHandlesSkipping(AppNode* node, bool include_layout,
                              const std::unordered_set<AppNode*>& covered) {
  if (node == nullptr || covered.count(node) != 0) return;
  ResetNodeHandlesSelf(node, include_layout);
  for (AppNode* child : node->children) {
    ResetNodeHandlesSkipping(child, include_layout, covered);
  }
}

// Mark every node reachable from `node` (used to protect covered routes from
// the current page's handle reset).
void CollectSubtreeNodes(AppNode* node, std::unordered_set<AppNode*>* out) {
  if (node == nullptr || out->count(node) != 0) return;
  out->insert(node);
  for (AppNode* child : node->children) CollectSubtreeNodes(child, out);
}

void PrepareLayoutBuffers(AppNode* node) {
  if (node == nullptr || node->released || node->style.display_none) return;
  if (node->layout_buffer_p == nullptr) {
    Dart_Handle buffer = Dart_NewTypedData(Dart_TypedData_kFloat32, 9);
    if (!Dart_IsError(buffer)) {
      Dart_TypedData_Type data_type = Dart_TypedData_kInvalid;
      void* data = nullptr;
      intptr_t length = 0;
      Dart_Handle status =
          Dart_TypedDataAcquireData(buffer, &data_type, &data, &length);
      if (!Dart_IsError(status) && data_type == Dart_TypedData_kFloat32 &&
          length == 9 && data != nullptr) {
        Dart_TypedDataReleaseData(buffer);
        node->layout_buffer = buffer;
        node->layout_buffer_p = Dart_NewPersistentHandle(buffer);
      }
    }
  }
  for (AppNode* child : node->children) PrepareLayoutBuffers(child);
}

// Appends the virtualized item nodes of a list/grid subtree.  Builder elements
// (`listbuilder`/`gridbuilder`) are transparent containers; their items are the
// actual children.  Item wrappers (`*builderitem`) are real items.
void CollectListItems(AppNode* parent, std::vector<AppNode*>* out) {
  for (AppNode* child : parent->children) {
    if (child->released || child->style.display_none) continue;
    if (child->name == "listbuilder" || child->name == "gridbuilder") {
      CollectListItems(child, out);
    } else {
      out->push_back(child);
    }
  }
}

bool IsListViewName(const std::string& name) {
  return name == "listview" || name == "list-view";
}

bool IsGridViewName(const std::string& name) {
  return name == "gridview" || name == "grid-view";
}

bool IsListBuilderName(const std::string& name) {
  return name == "listbuilder" || name == "list-builder";
}

bool IsGridBuilderName(const std::string& name) {
  return name == "gridbuilder" || name == "grid-builder";
}

// True when `node` is a list/grid builder or lives inside one.
bool IsBuilderName(const std::string& name);
bool IsBuilderSubtree(const AppNode* node) {
  for (const AppNode* current = node; current != nullptr;
       current = current->parent) {
    if (IsBuilderName(current->name)) return true;
  }
  return false;
}

bool IsBuilderName(const std::string& name) {
  return IsListBuilderName(name) || IsGridBuilderName(name);
}

bool IsScrollViewName(const std::string& name) {
  return name == "scrollview" || name == "scroll-view";
}

// The builtin `share-element` template wraps its slotted child in an anonymous
// `<view><slot/></view>` that the base stylesheet (WASkylineStyleV2, not served
// by the DevTools) sizes to `width/height:100%`.  Without it the wrapper stays
// 0-high and both painting and the mirrored hit test lose the content.
bool IsShareElementNode(const AppNode* node) {
  return node != nullptr &&
         (node->tag == "share-element" || node->tag == "shareelement" ||
          node->name == "share-element" || node->name == "shareelement");
}

// `swiper` is a builtin native component: its `swiper-item` children are
// full-viewport pages laid out horizontally and clipped, with `current`
// selecting the visible page.
bool IsSwiperNode(const AppNode* node) {
  return node != nullptr &&
         (node->tag == "swiper" || node->name == "swiper");
}

// `picker-view-column` is the native Cupertino wheel; `<picker-view>` itself
// is a plain box that lays the columns out horizontally.
bool IsPickerViewColumnNode(const AppNode* node) {
  return node != nullptr &&
         (node->tag == "picker-view-column" ||
          node->tag == "pickerviewcolumn" ||
          node->name == "picker-view-column" ||
          node->name == "pickerviewcolumn");
}

// Skyline's `<selection>` wrapper provides long-press text selection over its
// subtree (and a programmatic range API).
bool IsSelectionNode(const AppNode* node) {
  return node != nullptr &&
         (node->tag == "selection" || node->name == "selection");
}

bool InsideSelectionContext(const AppNode* node) {
  for (const AppNode* parent = node != nullptr ? node->parent : nullptr;
       parent != nullptr; parent = parent->parent) {
    if (IsSelectionNode(parent)) return true;
  }
  return false;
}

// glass-easel wraps the real adapter node (`scrollview`, which carries the
// scroll-y / scroll-x / show-scrollbar attributes) in a `wx-x scroll-view`
// component host.  Building the scroll view on the wrapper loses those
// attributes (both axes read as false, so the recognizer disables scrolling);
// the wrapper must stay a plain box and the inner node becomes the scroll view.
const AppNode* AdapterScrollViewChild(const AppNode* node) {
  if (node == nullptr || node->name != "scroll-view") return nullptr;
  for (const AppNode* child : node->children) {
    if (child != nullptr && !child->released && child->name == "scrollview") {
      return child;
    }
  }
  return nullptr;
}

bool IsScrollViewComponentWrapper(const AppNode* node) {
  return AdapterScrollViewChild(node) != nullptr;
}

// Sliver containers (sticky sections) sit between a scroll view and the
// list/grid slivers they host.  A list/grid under one of those is already a
// sliver and must not be wrapped in its own scroll view.
bool IsTransparentSliverContainer(const AppNode* node) {
  if (node == nullptr) return false;
  return node->tag == "fragment" || node->name == "empty" ||
         node->name == "stickysection" || node->name == "stickyheader" ||
         node->name == "listbuilder" || node->name == "gridbuilder";
}

bool InsideSliverContext(const AppNode* node) {
  for (const AppNode* ancestor = node != nullptr ? node->parent : nullptr;
       ancestor != nullptr; ancestor = ancestor->parent) {
    if (IsScrollViewName(ancestor->name)) return true;
    if (!IsTransparentSliverContainer(ancestor)) return false;
  }
  return false;
}

bool IsImageName(const std::string& name) {
  return name == "image" || name == "wx-image" || name == "img" ||
         name == "cover-image" || name == "wx-cover-image";
}

}  // namespace appservice

