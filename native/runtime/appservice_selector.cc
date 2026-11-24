// CSS selectors, stylesheet parsing and style resolution.

#include "internal/skyline_runtime_module.hh"
#include "internal/skyline_runtime_node.hh"
#include "internal/skyline_runtime_selector.hh"

namespace appservice {

struct CompoundSelector {
  std::string element;
  std::vector<std::string> classes;
  std::string id;
};

// A full selector: compounds joined left-to-right by combinators.  `' '` is a
// descendant combinator, `'>'` a child combinator and `'+'`/`'~'` sibling
// combinators.  WXSS pages rely on this: `.anchor-item.selected
// .anchor-item__inner` must only style the child of the *selected* anchor, so
// matching the last compound alone turns every letter green.
struct Selector {
  std::vector<CompoundSelector> compounds;
  // combinators[i] joins compounds[i] and compounds[i + 1].
  std::vector<char> combinators;
};

CompoundSelector ParseCompoundSelector(const std::string& text) {
  CompoundSelector compound;
  std::string token;
  auto flush = [&]() {
    if (token.empty()) return;
    if (token[0] == '.') {
      if (token.size() > 1) compound.classes.push_back(token.substr(1));
    } else if (token[0] == '#') {
      if (token.size() > 1) compound.id = token.substr(1);
    } else if (token != "*") {
      compound.element = token;
    }
    token.clear();
  };
  for (size_t i = 0; i < text.size(); i++) {
    const char c = text[i];
    if (c == '.') {
      flush();
      token = ".";
    } else if (c == '#') {
      flush();
      token = "#";
    } else if (c == '*') {
      flush();
    } else if (c == '[') {
      // Attribute selectors are ignored, but their contents (spaces, quotes,
      // `~=`/`|=`) must not be parsed as element names or combinators.
      flush();
      int depth = 1;
      char quote = '\0';
      for (i++; i < text.size() && depth > 0; i++) {
        const char inner = text[i];
        if (quote != '\0') {
          if (inner == quote) quote = '\0';
        } else if (inner == '"' || inner == '\'') {
          quote = inner;
        } else if (inner == '[') {
          depth++;
        } else if (inner == ']') {
          depth--;
        }
      }
      if (i > 0) i--;  // the for-loop increments past the closing bracket
    } else if (c == ':') {
      flush();
      // Skip the pseudo-class name.
      while (i + 1 < text.size() &&
             (std::isalnum(static_cast<unsigned char>(text[i + 1])) ||
              text[i + 1] == '-' || text[i + 1] == '_')) {
        i++;
      }
      // Skip a functional pseudo-class argument, e.g. `:not(.a)`.
      if (i + 1 < text.size() && text[i + 1] == '(') {
        int depth = 0;
        char quote = '\0';
        for (i++; i < text.size(); i++) {
          const char inner = text[i];
          if (quote != '\0') {
            if (inner == quote) quote = '\0';
          } else if (inner == '"' || inner == '\'') {
            quote = inner;
          } else if (inner == '(') {
            depth++;
          } else if (inner == ')') {
            depth--;
            if (depth == 0) break;
          }
        }
      }
    } else {
      token.push_back(c);
    }
  }
  flush();
  return compound;
}

Selector ParseSelector(const std::string& raw) {
  Selector selector;
  std::string compound;
  // Pending combinator for the next compound; '\0' means none (and a
  // whitespace-separated compound defaults to the descendant combinator).
  char combinator = '\0';
  bool have_left = false;
  char quote = '\0';
  int bracket_depth = 0;
  int paren_depth = 0;
  auto finish_compound = [&]() {
    if (compound.empty()) return;
    if (have_left) {
      selector.combinators.push_back(combinator == '\0' ? ' ' : combinator);
    }
    selector.compounds.push_back(ParseCompoundSelector(compound));
    have_left = true;
    compound.clear();
    combinator = '\0';
  };
  for (const char c : raw) {
    if (quote != '\0') {
      compound.push_back(c);
      if (c == quote) quote = '\0';
      continue;
    }
    if (c == '"' || c == '\'') {
      quote = c;
      compound.push_back(c);
      continue;
    }
    if (c == '[') {
      bracket_depth++;
      compound.push_back(c);
      continue;
    }
    if (c == ']') {
      if (bracket_depth > 0) bracket_depth--;
      compound.push_back(c);
      continue;
    }
    if (c == '(') {
      paren_depth++;
      compound.push_back(c);
      continue;
    }
    if (c == ')') {
      if (paren_depth > 0) paren_depth--;
      compound.push_back(c);
      continue;
    }
    if (bracket_depth > 0 || paren_depth > 0) {
      compound.push_back(c);
      continue;
    }
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f') {
      finish_compound();
      continue;
    }
    if (c == '>' || c == '+' || c == '~') {
      finish_compound();
      combinator = c;
      continue;
    }
    compound.push_back(c);
  }
  finish_compound();
  return selector;
}

int SelectorSpecificity(const Selector& selector) {
  int specificity = 0;
  for (const CompoundSelector& compound : selector.compounds) {
    specificity += static_cast<int>(compound.classes.size()) * 10 +
                   (compound.element.empty() ? 0 : 1) +
                   (compound.id.empty() ? 0 : 100);
  }
  return specificity;
}

bool NodeHasClass(const AppNode* node, const std::string& name) {
  for (const std::string& klass : node->classes) {
    if (klass == name) return true;
  }
  return false;
}

// WXML custom elements are hyphenated (`sticky-header`, `scroll-view`) while
// the adapter's shadow nodes drop the hyphen (`stickyheader`).  Compare
// element selectors with hyphens/underscores removed so CSS rules such as
// `sticky-header { position: sticky }` reach their nodes.
std::string NormalizeElementName(const std::string& name) {
  std::string out;
  out.reserve(name.size());
  for (char c : name) {
    if (c == '-' || c == '_') continue;
    out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  }
  return out;
}

bool CompoundSelectorMatches(const CompoundSelector& selector,
                             const AppNode* node) {
  if (node == nullptr) return false;
  if (!selector.element.empty() && selector.element != node->tag &&
      selector.element != node->name) {
    const std::string wanted = NormalizeElementName(selector.element);
    if (wanted != NormalizeElementName(node->tag) &&
        wanted != NormalizeElementName(node->name)) {
      return false;
    }
  }
  if (!selector.id.empty()) {
    const auto found = node->attrs.find("id");
    if (found == node->attrs.end() || found->second != selector.id) return false;
  }
  for (const std::string& klass : selector.classes) {
    if (!NodeHasClass(node, klass)) return false;
  }
  return true;
}

bool SelectorMatches(const Selector& selector, const AppNode* node) {
  if (selector.compounds.empty() || node == nullptr) return false;
  size_t index = selector.compounds.size() - 1;
  const AppNode* current = node;
  if (!CompoundSelectorMatches(selector.compounds[index], current)) {
    return false;
  }
  while (index > 0) {
    const char combinator =
        index - 1 < selector.combinators.size()
            ? selector.combinators[index - 1]
            : ' ';
    const CompoundSelector& wanted = selector.compounds[index - 1];
    if (combinator == '>') {
      current = current->parent;
      if (!CompoundSelectorMatches(wanted, current)) return false;
    } else if (combinator == '+' || combinator == '~') {
      const AppNode* parent = current->parent;
      if (parent == nullptr) return false;
      const auto found =
          std::find(parent->children.begin(), parent->children.end(), current);
      if (found == parent->children.end()) return false;
      const size_t position =
          static_cast<size_t>(found - parent->children.begin());
      bool matched = false;
      if (combinator == '+') {
        if (position > 0) {
          current = parent->children[position - 1];
          matched = CompoundSelectorMatches(wanted, current);
        }
      } else {
        for (size_t i = position; i-- > 0;) {
          if (CompoundSelectorMatches(wanted, parent->children[i])) {
            current = parent->children[i];
            matched = true;
            break;
          }
        }
      }
      if (!matched) return false;
    } else {
      const AppNode* ancestor = current->parent;
      while (ancestor != nullptr &&
             !CompoundSelectorMatches(wanted, ancestor)) {
        ancestor = ancestor->parent;
      }
      if (ancestor == nullptr) return false;
      current = ancestor;
    }
    index--;
  }
  return true;
}

// Index of the `}` matching the `{` at `open`, skipping nested blocks.  WXSS
// @media/@supports bodies contain their own rules, so the old "first closing
// brace" scan merged an inner rule into the at-rule selector and leaked the
// rest of the block out as top-level rules.
size_t FindMatchingBrace(const std::string& css, size_t open) {
  int depth = 0;
  for (size_t i = open; i < css.size(); i++) {
    if (css[i] == '{') {
      depth++;
    } else if (css[i] == '}') {
      depth--;
      if (depth == 0) return i;
    }
  }
  return std::string::npos;
}

// Platform colour scheme used to evaluate `prefers-color-scheme` media
// queries.  The simulator defaults to light; `UpdatePlatformBrightness`
// (Brightness enum: 1 light, 2 dark) can switch it before the next replay.
bool g_media_dark = false;

bool MediaQueryMatches(const std::string& condition) {
  if (condition.find("prefers-color-scheme") != std::string::npos) {
    if (condition.find("dark") != std::string::npos) return g_media_dark;
    if (condition.find("light") != std::string::npos) return !g_media_dark;
  }
  // Unsupported features are treated as matching (best effort).
  return true;
}

void ParseStyleSheet(const std::string& css) {
  std::string cleaned;
  cleaned.reserve(css.size());
  for (size_t i = 0; i < css.size();) {
    if (css[i] == '/' && i + 1 < css.size() && css[i + 1] == '*') {
      const size_t end = css.find("*/", i + 2);
      i = end == std::string::npos ? css.size() : end + 2;
      continue;
    }
    cleaned.push_back(css[i++]);
  }

  size_t cursor = 0;
  while (cursor < cleaned.size()) {
    const size_t open = cleaned.find('{', cursor);
    if (open == std::string::npos) break;
    const size_t close = FindMatchingBrace(cleaned, open);
    if (close == std::string::npos) break;
    const std::string selectors = TrimAscii(cleaned.substr(cursor, open - cursor));
    const std::string body = cleaned.substr(open + 1, close - open - 1);
    if (!selectors.empty() && selectors.front() == '@') {
      const std::string at_rule = LowerAscii(selectors);
      if (at_rule.rfind("@media", 0) == 0) {
        if (MediaQueryMatches(at_rule)) ParseStyleSheet(body);
      } else if (at_rule.rfind("@supports", 0) == 0) {
        ParseStyleSheet(body);
      } else if (at_rule.rfind("@font-face", 0) == 0) {
        const auto declarations = ParseDeclarations(body);
        FontFaceRequest font;
        for (const auto& decl : declarations) {
          const std::string key = LowerAscii(TrimAscii(decl.first));
          if (key == "font-family") {
            std::string family = TrimAscii(decl.second);
            if (family.size() >= 2 &&
                (family.front() == '"' || family.front() == '\'') &&
                family.back() == family.front()) {
              family = family.substr(1, family.size() - 2);
            }
            font.family = family;
          } else if (key == "src") {
            const size_t url_at = decl.second.find("url(");
            if (url_at != std::string::npos) {
              const size_t begin = url_at + 4;
              const size_t end = decl.second.find(')', begin);
              if (end != std::string::npos) {
                std::string url =
                    TrimAscii(decl.second.substr(begin, end - begin));
                if (url.size() >= 2 &&
                    (url.front() == '"' || url.front() == '\'') &&
                    url.back() == url.front()) {
                  url = url.substr(1, url.size() - 2);
                }
                font.url = url;
              }
            }
          }
        }
        if (!font.family.empty() && !font.url.empty() && g_page != nullptr) {
          g_page->fonts.push_back(font);
        }
      }
      // @keyframes/@-webkit-keyframes are not replayed; skipping them keeps
      // their `0%`/`from` selectors from becoming bogus element rules.
      cursor = close + 1;
      continue;
    }
    const auto declarations = ParseDeclarations(body);
    for (const std::string& selector_text : SplitChar(selectors, ',')) {
      if (selector_text.empty()) continue;
      CssRule rule;
      rule.selector = selector_text;
      rule.parsed_selector =
          std::make_shared<Selector>(ParseSelector(selector_text));
      rule.specificity = SelectorSpecificity(*rule.parsed_selector);
      rule.declarations = declarations;
      rule.order = g_next_rule_order++;
      if (g_page != nullptr) g_page->rules.push_back(rule);
    }
    cursor = close + 1;
  }
}

bool IsSwiperNode(const AppNode* node);

void ResolveStyles(AppNode* node, const AppStyle* parent) {
  AppStyle style;
  if (parent != nullptr) {
    // CSS inheritance for text properties.
    style.font_size = parent->font_size;
    style.color = parent->color;
    style.font_weight = parent->font_weight;
    style.font_style = parent->font_style;
    style.line_height = parent->line_height;
    style.line_height_factor = parent->line_height_factor;
    style.letter_spacing = parent->letter_spacing;
    style.text_align = parent->text_align;
    style.font_families = parent->font_families;
    style.white_space = parent->white_space;
    // `text-overflow` is declared on the block container in WXSS (e.g.
    // `<view class="ellipsis">text</view>`), while the replay renders the
    // container's raw text as a child text node.  Carry the value down so the
    // text widget applies the ellipsis; a child declaration still wins.
    style.text_overflow = parent->text_overflow;
  }

  if (g_page != nullptr) {
    struct Match {
      int specificity;
      int order;
      const CssRule* rule;
    };
    std::vector<Match> matches;
    for (const CssRule& rule : g_page->rules) {
      const Selector* selector = rule.parsed_selector.get();
      if (selector == nullptr) continue;
      if (SelectorMatches(*selector, node)) {
        matches.push_back(
            Match{rule.specificity, rule.order, &rule});
      }
    }
    std::sort(matches.begin(), matches.end(),
              [](const Match& a, const Match& b) {
                if (a.specificity != b.specificity) {
                  return a.specificity < b.specificity;
                }
                return a.order < b.order;
              });
    for (const Match& match : matches) {
      ApplyDeclarations(&style, match.rule->declarations);
    }
  }
  if (!node->inline_style.empty()) {
    ApplyDeclarations(&style, ParseDeclarations(node->inline_style));
  }
  // Skyline's text containers are inline-level by default: `span` is the
  // inline mixing element for text/image/navigator, and `<text>` mirrors the
  // WebView renderer's inline `wx-text`.  The DevTools host does not serve the
  // compiled `WASkylineStyle` base sheet, so these native defaults live here.
  // Raw text nodes (tag `text`, empty name) are measured as text runs and are
  // unaffected; only the element wrappers take part in inline formatting.
  if (!style.has_display) {
    const bool is_text_container =
        node->name == "text" || (node->tag == "text" && !node->name.empty());
    const bool is_span_container =
        node->name == "span" || node->tag == "span";
    if (is_text_container || is_span_container) {
      style.inline_level = true;
    }
  }
  // `<root-portal>` children are hoisted out of their ancestor's layout and
  // clipping: they render against the page viewport.  Treat the portal box as
  // a viewport-sized fixed container so absolutely positioned children (the
  // usual pattern, e.g. an A-Z side index) resolve their insets correctly and
  // are promoted to the page root by DeferFixedChild.  The detached
  // `root-portal-content` node carries the actual subtree.
  if (node->name == "root-portal" || node->name == "rootportal" ||
      node->name == "root-portal-content" ||
      node->name == "rootportalcontent" || node->tag == "root-portal" ||
      node->tag == "rootportal") {
    style.position = 2;  // fixed
    for (int side = 0; side < 4; side++) {
      style.inset[side] = 0.0f;
      style.inset_percent[side] = std::nanf("");
    }
  }
  // Builtin swiper defaults from the base stylesheet (`wx-swiper{height:150px}`)
  // plus the pager's mandatory clipping.  A CSS `height: auto` deliberately
  // clears the builtin size (`.swiper{height:auto; flex:1}`), so only fall back
  // to 150px when the page never declared a height at all.
  if (IsSwiperNode(node)) {
    style.clip = true;
    if (!style.height_declared && style.height < 0.0f &&
        style.height_percent < 0.0f) {
      style.height = 150.0f;
    }
  }
  // Builtin scroll containers lay their children along the scroll axis.  The
  // renderer creates the inner `scrollview` element without the user's flex
  // styles (those live on the outer `scroll-view` component wrapper), so a
  // horizontal scroll-view would otherwise fall back to Skyline's default
  // column flow and stack its items vertically.
  if (IsScrollViewName(node->name)) {
    if (AttrBool(node, "scrollX", false)) {
      style.row = true;
    } else if (AttrBool(node, "scrollY", false)) {
      style.row = false;
    }
  }
  node->style = style;
  // A running CSS transition writes an interpolated style over the cascade
  // result.  Children inherit from the animated style so text colour/opacity
  // transitions stay in sync with their container.
  if (node->animation_override_active) {
    node->style = node->animation_override;
  }
  for (AppNode* child : node->children) ResolveStyles(child, &node->style);
}

// -- node table / operations -------------------------------------------------


}  // namespace appservice

