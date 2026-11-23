// CSS value parsing and declaration application.

#include "internal/skyline_runtime_module.hh"
#include "internal/skyline_runtime_css.hh"

namespace appservice {

bool ParseNumber(const std::string& value, double* out) {
  if (value.empty()) return false;
  char* end = nullptr;
  const double parsed = std::strtod(value.c_str(), &end);
  if (end == value.c_str()) return false;
  *out = parsed;
  return true;
}

// Parses "12", "12px" or "0".  Percentages are reported separately.
bool ParseLength(const std::string& raw, float* px, float* percent) {
  const std::string value = TrimAscii(raw);
  if (value.empty()) return false;
  if (value == "auto") {
    *px = -1.0f;
    *percent = -1.0f;
    return true;
  }
  if (EndsWith(value, "%")) {
    double number = 0.0;
    if (!ParseNumber(value.substr(0, value.size() - 1), &number)) return false;
    *percent = static_cast<float>(number / 100.0);
    *px = -1.0f;
    return true;
  }
  // Viewport-relative and rpx units are resolved against the logical window
  // size captured at CreateWindow.  The runtime previously rejected them,
  // which silently dropped `height: 100vh` / `width: 100vw` from page WXSS and
  // collapsed the appservice page layout.
  if (EndsWith(value, "vw") || EndsWith(value, "vh")) {
    const bool width_unit = value.back() == 'w';
    double number = 0.0;
    if (!ParseNumber(value.substr(0, value.size() - 2), &number)) return false;
    const double base = width_unit ? g_viewport_width : g_viewport_height;
    if (base > 0.0) {
      *px = static_cast<float>(number * base / 100.0);
      *percent = -1.0f;
    } else {
      *px = -1.0f;
      *percent = static_cast<float>(number / 100.0);
    }
    return true;
  }
  if (EndsWith(value, "rpx")) {
    double number = 0.0;
    if (!ParseNumber(value.substr(0, value.size() - 3), &number)) return false;
    // Skyline's design width is 750; the DevTools window is already logical px.
    *px = static_cast<float>(
        number * (g_viewport_width > 0.0 ? g_viewport_width : 375.0) / 750.0);
    *percent = -1.0f;
    return true;
  }
  double number = 0.0;
  std::string numeric = value;
  if (EndsWith(numeric, "px")) numeric = numeric.substr(0, numeric.size() - 2);
  if (!ParseNumber(numeric, &number)) return false;
  *px = static_cast<float>(number);
  *percent = -1.0f;
  return true;
}

uint32_t NamedColor(const std::string& name) {
  if (name == "transparent") return 0x00000000u;
  if (name == "black") return 0xFF000000u;
  if (name == "white") return 0xFFFFFFFFu;
  if (name == "red") return 0xFFFF0000u;
  if (name == "green") return 0xFF008000u;
  if (name == "blue") return 0xFF0000FFu;
  if (name == "yellow") return 0xFFFFFF00u;
  if (name == "orange") return 0xFFFFA500u;
  if (name == "gray" || name == "grey") return 0xFF808080u;
  if (name == "pink") return 0xFFFFC0CBu;
  if (name == "purple") return 0xFF800080u;
  if (name == "cyan") return 0xFF00FFFFu;
  if (name == "magenta") return 0xFFFF00FFu;
  return 0u;
}

bool ParseCssColor(const std::string& raw, uint32_t* out) {
  const std::string value = TrimAscii(raw);
  if (value.empty()) return false;
  if (value[0] == '#') {
    std::string hex = value.substr(1);
    if (hex.size() == 3) {
      hex = {hex[0], hex[0], hex[1], hex[1], hex[2], hex[2]};
    }
    if (hex.size() == 6 || hex.size() == 8) {
      char* end = nullptr;
      const unsigned long parsed = std::strtoul(hex.c_str(), &end, 16);
      if (end == hex.c_str()) return false;
      *out = hex.size() == 6 ? (0xFF000000u | static_cast<uint32_t>(parsed))
                             : static_cast<uint32_t>(parsed);
      return true;
    }
    return false;
  }
  if (StartsWith(value, "rgb(") || StartsWith(value, "rgba(")) {
    const size_t open = value.find('(');
    const size_t close = value.find(')');
    if (open == std::string::npos || close == std::string::npos) return false;
    const std::vector<std::string> parts =
        SplitChar(value.substr(open + 1, close - open - 1), ',');
    if (parts.size() < 3) return false;
    double r = 0, g = 0, b = 0, a = 255;
    if (!ParseNumber(parts[0], &r) || !ParseNumber(parts[1], &g) ||
        !ParseNumber(parts[2], &b)) {
      return false;
    }
    if (parts.size() > 3) ParseNumber(parts[3], &a);
    if (a <= 1.0) a *= 255.0;
    auto clamp = [](double v) {
      if (v < 0) return 0.0;
      if (v > 255) return 255.0;
      return v;
    };
    *out = (static_cast<uint32_t>(clamp(a)) << 24) |
           (static_cast<uint32_t>(clamp(r)) << 16) |
           (static_cast<uint32_t>(clamp(g)) << 8) |
           static_cast<uint32_t>(clamp(b));
    return true;
  }
  const uint32_t named = NamedColor(LowerAscii(value));
  if (named != 0u || LowerAscii(value) == "transparent") {
    *out = named;
    return true;
  }
  return false;
}

int ParseJustify(const std::string& value) {
  if (value == "center") return 1;
  if (value == "flex-end" || value == "end") return 2;
  if (value == "space-between") return 3;
  return 0;
}

int ParseAlign(const std::string& value) {
  if (value == "flex-start" || value == "start") return 1;
  if (value == "center") return 2;
  if (value == "flex-end" || value == "end") return 3;
  return 0;  // stretch
}

// -- CSS transform -----------------------------------------------------------

constexpr double kPi = 3.14159265358979323846;

void MatrixIdentity(float* m) {
  for (int i = 0; i < 16; i++) m[i] = 0.0f;
  m[0] = m[5] = m[10] = m[15] = 1.0f;
}

// out = a * b, both column-major 4x4.
void MatrixMultiply(const float* a, const float* b, float* out) {
  float result[16];
  for (int col = 0; col < 4; col++) {
    for (int row = 0; row < 4; row++) {
      float sum = 0.0f;
      for (int k = 0; k < 4; k++) {
        sum += a[k * 4 + row] * b[col * 4 + k];
      }
      result[col * 4 + row] = sum;
    }
  }
  std::memcpy(out, result, sizeof(result));
}

bool ParsePxToken(const std::string& raw, double* out) {
  std::string value = TrimAscii(raw);
  if (EndsWith(value, "px")) value.resize(value.size() - 2);
  return ParseNumber(value, out);
}

// `translate`/`translateX`/`translateY` accept percentages, which resolve
// against the element's own border box once it has been measured.
bool ParseTranslateToken(const std::string& raw, double* out,
                         double* percent) {
  std::string value = TrimAscii(raw);
  if (EndsWith(value, "%")) {
    value.resize(value.size() - 1);
    double number = 0.0;
    if (!ParseNumber(value, &number)) return false;
    *percent = number / 100.0;
    return true;
  }
  return ParsePxToken(value, out);
}

bool ParseAngleToken(const std::string& raw, double* radians) {
  std::string value = TrimAscii(raw);
  double number = 0.0;
  if (EndsWith(value, "deg")) {
    value.resize(value.size() - 3);
    if (!ParseNumber(value, &number)) return false;
    *radians = number * kPi / 180.0;
    return true;
  }
  if (EndsWith(value, "rad")) {
    value.resize(value.size() - 3);
    if (!ParseNumber(value, &number)) return false;
    *radians = number;
    return true;
  }
  if (EndsWith(value, "turn")) {
    value.resize(value.size() - 4);
    if (!ParseNumber(value, &number)) return false;
    *radians = number * 2.0 * kPi;
    return true;
  }
  if (!ParseNumber(value, &number)) return false;
  *radians = number * kPi / 180.0;  // unitless follows CSS angles
  return true;
}

std::vector<std::string> TransformArguments(const std::string& args) {
  std::vector<std::string> out;
  std::string current;
  for (char c : args) {
    if (c == ',' || c == ' ' || c == '\t') {
      if (!current.empty()) {
        out.push_back(current);
        current.clear();
      }
    } else {
      current.push_back(c);
    }
  }
  if (!current.empty()) out.push_back(current);
  return out;
}

// Parses `transform: translate(...) rotate(...) scale(...) ...` into a
// column-major matrix.  Percentage translations are kept in
// `translate_percent_*` and resolved against the measured border box.
void ParseTransform(const std::string& raw, AppStyle* style) {
  float matrix[16];
  MatrixIdentity(matrix);
  style->translate_percent_x = std::nanf("");
  style->translate_percent_y = std::nanf("");
  size_t index = 0;
  bool any = false;
  while (index < raw.size()) {
    while (index < raw.size() &&
           (std::isspace(static_cast<unsigned char>(raw[index])) ||
            raw[index] == ',')) {
      index++;
    }
    const size_t name_start = index;
    while (index < raw.size() && raw[index] != '(') index++;
    if (index >= raw.size()) break;
    std::string name = LowerAscii(TrimAscii(raw.substr(name_start,
                                                       index - name_start)));
    const size_t close = raw.find(')', index + 1);
    if (close == std::string::npos) break;
    const std::string args =
        raw.substr(index + 1, close - index - 1);
    index = close + 1;
    const std::vector<std::string> values = TransformArguments(args);
    float local[16];
    MatrixIdentity(local);
    if (name == "translate" || name == "translate3d") {
      double tx = 0.0;
      double ty = 0.0;
      double pct_x = std::nan("");
      double pct_y = std::nan("");
      if (values.empty() ||
          !ParseTranslateToken(values[0], &tx, &pct_x)) {
        continue;
      }
      if (values.size() > 1) ParseTranslateToken(values[1], &ty, &pct_y);
      double tz = 0.0;
      if (values.size() > 2) ParsePxToken(values[2], &tz);
      if (!std::isnan(pct_x)) style->translate_percent_x = pct_x;
      if (!std::isnan(pct_y)) style->translate_percent_y = pct_y;
      local[12] = static_cast<float>(tx);
      local[13] = static_cast<float>(ty);
      local[14] = static_cast<float>(tz);
    } else if (name == "translatex") {
      double tx = 0.0;
      double pct_x = std::nan("");
      if (values.empty() || !ParseTranslateToken(values[0], &tx, &pct_x)) {
        continue;
      }
      if (!std::isnan(pct_x)) style->translate_percent_x = pct_x;
      local[12] = static_cast<float>(tx);
    } else if (name == "translatey") {
      double ty = 0.0;
      double pct_y = std::nan("");
      if (values.empty() || !ParseTranslateToken(values[0], &ty, &pct_y)) {
        continue;
      }
      if (!std::isnan(pct_y)) style->translate_percent_y = pct_y;
      local[13] = static_cast<float>(ty);
    } else if (name == "translatez") {
      double tz = 0.0;
      if (values.empty() || !ParsePxToken(values[0], &tz)) continue;
      local[14] = static_cast<float>(tz);
    } else if (name == "scale" || name == "scale3d") {
      double sx = 1.0;
      double sy = 1.0;
      double sz = 1.0;
      if (values.empty() || !ParseNumber(values[0], &sx)) continue;
      sy = sx;
      if (values.size() > 1) ParseNumber(values[1], &sy);
      if (values.size() > 2) ParseNumber(values[2], &sz);
      local[0] = static_cast<float>(sx);
      local[5] = static_cast<float>(sy);
      local[10] = static_cast<float>(sz);
    } else if (name == "scalex") {
      double sx = 1.0;
      if (values.empty() || !ParseNumber(values[0], &sx)) continue;
      local[0] = static_cast<float>(sx);
    } else if (name == "scaley") {
      double sy = 1.0;
      if (values.empty() || !ParseNumber(values[0], &sy)) continue;
      local[5] = static_cast<float>(sy);
    } else if (name == "scalez") {
      double sz = 1.0;
      if (values.empty() || !ParseNumber(values[0], &sz)) continue;
      local[10] = static_cast<float>(sz);
    } else if (name == "rotate" || name == "rotatez") {
      double radians = 0.0;
      if (values.empty() || !ParseAngleToken(values[0], &radians)) continue;
      const float c = static_cast<float>(std::cos(radians));
      const float s = static_cast<float>(std::sin(radians));
      local[0] = c;
      local[1] = s;
      local[4] = -s;
      local[5] = c;
    } else if (name == "rotatex") {
      double radians = 0.0;
      if (values.empty() || !ParseAngleToken(values[0], &radians)) continue;
      const float c = static_cast<float>(std::cos(radians));
      const float s = static_cast<float>(std::sin(radians));
      // Column-major rotation about X.
      local[5] = c;
      local[6] = s;
      local[9] = -s;
      local[10] = c;
    } else if (name == "rotatey") {
      double radians = 0.0;
      if (values.empty() || !ParseAngleToken(values[0], &radians)) continue;
      const float c = static_cast<float>(std::cos(radians));
      const float s = static_cast<float>(std::sin(radians));
      // Column-major rotation about Y.
      local[0] = c;
      local[2] = -s;
      local[8] = s;
      local[10] = c;
    } else if (name == "skew" || name == "skewx" || name == "skewy") {
      double ax = 0.0;
      double ay = 0.0;
      if (name == "skew") {
        if (values.empty() || !ParseAngleToken(values[0], &ax)) continue;
        if (values.size() > 1) ParseAngleToken(values[1], &ay);
      } else if (name == "skewx") {
        if (values.empty() || !ParseAngleToken(values[0], &ax)) continue;
      } else {
        if (values.empty() || !ParseAngleToken(values[0], &ay)) continue;
      }
      local[1] = static_cast<float>(std::tan(ay));
      local[4] = static_cast<float>(std::tan(ax));
      if (std::getenv("SKYLINE_APP_SERVICE_DEBUG") != nullptr) {
        std::fprintf(stderr,
                     "[skyline-runtime] transform %s: ax=%g ay=%g m1=%g m4=%g\n",
                     name.c_str(), ax, ay, local[1], local[4]);
      }
    } else if (name == "perspective") {
      double depth = 0.0;
      if (values.empty() || !ParsePxToken(values[0], &depth) ||
          std::fabs(depth) < 1e-6) {
        continue;
      }
      local[11] = static_cast<float>(-1.0 / depth);
    } else if (name == "matrix") {
      if (values.size() < 6) continue;
      double v[6] = {0, 0, 0, 0, 0, 0};
      bool ok = true;
      for (int i = 0; i < 6; i++) {
        if (!ParseNumber(values[static_cast<size_t>(i)], &v[i])) {
          ok = false;
          break;
        }
      }
      if (!ok) continue;
      local[0] = static_cast<float>(v[0]);   // a
      local[1] = static_cast<float>(v[1]);   // b
      local[4] = static_cast<float>(v[2]);   // c
      local[5] = static_cast<float>(v[3]);   // d
      local[12] = static_cast<float>(v[4]);  // e
      local[13] = static_cast<float>(v[5]);  // f
    } else if (name == "matrix3d") {
      if (values.size() < 16) continue;
      bool ok = true;
      for (size_t i = 0; i < 16; i++) {
        double value = 0.0;
        if (!ParseNumber(values[i], &value)) {
          ok = false;
          break;
        }
        local[i] = static_cast<float>(value);
      }
      if (!ok) continue;
    } else {
      continue;  // unsupported transform function
    }
    any = true;
    MatrixMultiply(matrix, local, matrix);
  }
  if (!any) return;
  std::memcpy(style->transform, matrix, sizeof(matrix));
  style->has_transform = true;
}

// Splits CSS function arguments at top-level commas (rgba(0, 0, 0, .5) is one
// layer, not four).
std::vector<std::string> SplitTopLevelComma(const std::string& value) {
  std::vector<std::string> out;
  std::string current;
  int depth = 0;
  for (char c : value) {
    if (c == '(') depth++;
    if (c == ')' && depth > 0) depth--;
    if (c == ',' && depth == 0) {
      out.push_back(TrimAscii(current));
      current.clear();
    } else {
      current.push_back(c);
    }
  }
  if (!TrimAscii(current).empty()) out.push_back(TrimAscii(current));
  return out;
}

// CSS time values (`250ms`, `0.3s`).  Bare numbers are treated as ms.
bool ParseTimeMs(const std::string& raw, double* out) {
  std::string value = TrimAscii(raw);
  if (value.empty()) return false;
  double number = 0.0;
  if (EndsWith(value, "ms")) {
    value.resize(value.size() - 2);
    if (!ParseNumber(value, &number)) return false;
    *out = number;
    return true;
  }
  if (EndsWith(value, "s")) {
    value.resize(value.size() - 1);
    if (!ParseNumber(value, &number)) return false;
    *out = number * 1000.0;
    return true;
  }
  if (!ParseNumber(value, &number)) return false;
  *out = number;
  return true;
}

uint32_t TransitionPropertyBit(const std::string& raw) {
  const std::string name = LowerAscii(TrimAscii(raw));
  if (name == "all" || name.empty()) {
    return kTransitionOpacity | kTransitionBackground | kTransitionColor |
           kTransitionBorderColor | kTransitionTransform;
  }
  if (name == "opacity") return kTransitionOpacity;
  if (name == "background" || name == "background-color") {
    return kTransitionBackground;
  }
  if (name == "color") return kTransitionColor;
  if (name == "border" || name == "border-color") {
    return kTransitionBorderColor;
  }
  if (name == "transform") return kTransitionTransform;
  return 0u;
}

int TransitionEasing(const std::string& raw) {
  const std::string value = LowerAscii(TrimAscii(raw));
  if (value == "ease") return 1;
  if (value == "ease-in") return 2;
  if (value == "ease-out") return 3;
  if (value == "ease-in-out") return 4;
  if (StartsWith(value, "cubic-bezier")) return 1;  // approximate with ease
  return 0;                                          // linear
}

bool IsTransitionTimingKeyword(const std::string& raw) {
  const std::string value = LowerAscii(TrimAscii(raw));
  return value == "linear" || value == "ease" || value == "ease-in" ||
         value == "ease-out" || value == "ease-in-out" ||
         StartsWith(value, "cubic-bezier") || StartsWith(value, "steps");
}

void ParseTransitionShorthand(AppStyle* style, const std::string& raw) {
  const std::vector<std::string> layers = SplitTopLevelComma(raw);
  uint32_t mask = 0;
  double duration = 0.0;
  double delay = 0.0;
  int easing = 0;
  bool have_duration = false;
  bool have_delay = false;
  for (const std::string& layer : layers) {
    // CSS allows one duration/delay/timing per comma-separated transition;
    // reset per layer so a second layer's duration is not mistaken for delay.
    double layer_duration = 0.0;
    double layer_delay = 0.0;
    bool layer_has_duration = false;
    bool layer_has_delay = false;
    int layer_easing = 0;
    uint32_t layer_mask = 0;
    for (const std::string& token : SplitWhitespace(layer)) {
      double time = 0.0;
      if (ParseTimeMs(token, &time)) {
        if (!layer_has_duration) {
          layer_duration = time;
          layer_has_duration = true;
        } else if (!layer_has_delay) {
          layer_delay = time;
          layer_has_delay = true;
        }
      } else if (IsTransitionTimingKeyword(token)) {
        layer_easing = TransitionEasing(token);
      } else {
        layer_mask |= TransitionPropertyBit(token);
      }
    }
    mask |= layer_mask != 0 ? layer_mask : TransitionPropertyBit("all");
    if (!have_duration && layer_has_duration) {
      duration = layer_duration;
      have_duration = true;
    }
    if (!have_delay && layer_has_delay) {
      delay = layer_delay;
      have_delay = true;
    }
    if (easing == 0) easing = layer_easing;
  }
  style->transition_mask = mask != 0 ? mask : TransitionPropertyBit("all");
  style->transition_duration = duration;
  style->transition_delay = delay;
  style->transition_easing = easing;
}

// Extracts `url(...)` from a CSS background value.
std::string ParseCssUrl(const std::string& raw) {
  const size_t start = raw.find("url(");
  if (start == std::string::npos) return std::string();
  size_t value_start = start + 4;
  while (value_start < raw.size() &&
         std::isspace(static_cast<unsigned char>(raw[value_start]))) {
    value_start++;
  }
  // Quoted URLs may contain `)` and `;` (data URIs); end at the quote.
  if (value_start < raw.size() &&
      (raw[value_start] == '"' || raw[value_start] == '\'')) {
    const char quote = raw[value_start];
    const size_t quote_end = raw.find(quote, value_start + 1);
    if (quote_end == std::string::npos) return std::string();
    return raw.substr(value_start + 1, quote_end - value_start - 1);
  }
  const size_t end = raw.find(')', value_start);
  if (end == std::string::npos) return std::string();
  std::string value = TrimAscii(raw.substr(value_start, end - value_start));
  if (value.size() >= 2 && (value.front() == '"' || value.front() == '\'') &&
      value.back() == value.front()) {
    value = value.substr(1, value.size() - 2);
  }
  return value;
}

int ParseBackgroundRepeat(const std::string& raw) {
  const std::string value = LowerAscii(TrimAscii(raw));
  if (value == "repeat" || value == "repeat repeat") return 1;
  if (value == "repeat-x") return 2;
  if (value == "repeat-y") return 3;
  return 0;
}

// Parses CSS `background-size`: `cover`/`contain`, or one/two explicit values
// (`auto`, length or percentage).
void ParseBackgroundSize(AppStyle* style, const std::string& raw) {
  const std::vector<std::string> layers = SplitTopLevelComma(raw);
  const std::string value =
      LowerAscii(TrimAscii(layers.empty() ? raw : layers[0]));
  if (value.empty()) return;
  auto reset_explicit = [style]() {
    style->has_background_size = false;
    style->background_size_width = -1.0f;
    style->background_size_height = -1.0f;
    style->background_size_width_percent = false;
    style->background_size_height_percent = false;
  };
  if (value == "cover") {
    style->background_fit = 2;
    reset_explicit();
    return;
  }
  if (value == "contain") {
    style->background_fit = 1;
    reset_explicit();
    return;
  }
  const std::vector<std::string> parts = SplitWhitespace(value);
  if (parts.empty() || parts.size() > 2) return;
  auto parse = [](const std::string& token, float* out, bool* percent) {
    if (LowerAscii(token) == "auto") {
      *out = -1.0f;
      *percent = false;
      return true;
    }
    float px = -1.0f;
    float pct = -1.0f;
    if (!ParseLength(token, &px, &pct)) return false;
    if (pct >= 0.0f) {
      *out = pct;
      *percent = true;
    } else {
      *out = px;
      *percent = false;
    }
    return true;
  };
  float width = -1.0f;
  float height = -1.0f;
  bool width_percent = false;
  bool height_percent = false;
  if (!parse(parts[0], &width, &width_percent)) return;
  if (parts.size() > 1 && !parse(parts[1], &height, &height_percent)) return;
  style->background_fit = 0;
  style->has_background_size = true;
  style->background_size_width = width;
  style->background_size_height = height;
  style->background_size_width_percent = width_percent;
  style->background_size_height_percent = height_percent;
}

// Parses CSS `transform-origin` (one to three components; the optional third
// component is the z offset).
void ParseTransformOrigin(AppStyle* style, const std::string& raw) {
  const std::vector<std::string> parts = SplitWhitespace(raw);
  if (parts.empty()) return;
  style->transform_origin_z = 0.0f;
  bool x_set = false;
  bool y_set = false;
  float x_px = std::nanf("");
  float y_px = std::nanf("");
  float x_percent = std::nanf("");
  float y_percent = std::nanf("");
  auto set_x_percent = [&](float value) {
    x_percent = value;
    x_px = std::nanf("");
    x_set = true;
  };
  auto set_y_percent = [&](float value) {
    y_percent = value;
    y_px = std::nanf("");
    y_set = true;
  };
  for (const std::string& part : parts) {
    const std::string value = LowerAscii(TrimAscii(part));
    if (value == "left") {
      set_x_percent(0.0f);
      continue;
    }
    if (value == "right") {
      set_x_percent(1.0f);
      continue;
    }
    if (value == "top") {
      set_y_percent(0.0f);
      continue;
    }
    if (value == "bottom") {
      set_y_percent(1.0f);
      continue;
    }
    if (value == "center") {
      if (!x_set) {
        set_x_percent(0.5f);
      } else if (!y_set) {
        set_y_percent(0.5f);
      }
      continue;
    }
    float px = 0.0f;
    float percent = -1.0f;
    if (!ParseLength(part, &px, &percent)) continue;
    if (!x_set) {
      if (percent >= 0.0f) {
        set_x_percent(percent);
      } else {
        x_px = px;
        x_percent = std::nanf("");
        x_set = true;
      }
    } else if (!y_set) {
      if (percent >= 0.0f) {
        set_y_percent(percent);
      } else {
        y_px = px;
        y_percent = std::nanf("");
        y_set = true;
      }
    } else {
      // Third component is the z offset; only lengths are valid.
      if (percent < 0.0f && value != "auto") {
        style->transform_origin_z = px;
      }
      break;
    }
  }
  if (x_set && !y_set) set_y_percent(0.5f);
  if (y_set && !x_set) set_x_percent(0.5f);
  style->transform_origin_x = x_px;
  style->transform_origin_y = y_px;
  style->transform_origin_x_percent = x_percent;
  style->transform_origin_y_percent = y_percent;
}

// Defined after the gradient parser but used to resolve `at <position>`.
bool ParsePositionPair(const std::string& raw,
                       CssPositionAxis* x,
                       CssPositionAxis* y);

// Locates the first gradient function (`linear`/`radial`/`conic`) in a CSS
// background value and returns its comma-separated argument text.
bool FindCssGradient(const std::string& raw,
                     CssGradientKind* kind,
                     std::string* args) {
  struct GradientFunction {
    const char* name;
    CssGradientKind kind;
  };
  static const GradientFunction kFunctions[] = {
      {"linear-gradient(", kGradientLinear},
      {"radial-gradient(", kGradientRadial},
      {"conic-gradient(", kGradientConic},
  };
  size_t best = std::string::npos;
  const GradientFunction* match = nullptr;
  for (const GradientFunction& fn : kFunctions) {
    const size_t pos = raw.find(fn.name);
    if (pos != std::string::npos && pos < best) {
      best = pos;
      match = &fn;
    }
  }
  if (match == nullptr) return false;
  size_t cursor = best + strlen(match->name);
  int depth = 1;
  const size_t args_start = cursor;
  while (cursor < raw.size() && depth > 0) {
    if (raw[cursor] == '(') depth++;
    if (raw[cursor] == ')') depth--;
    cursor++;
  }
  if (depth != 0) return false;
  *args = raw.substr(args_start, cursor - args_start - 1);
  *kind = match->kind;
  return true;
}

// Gradient arguments are color stops unless the leading token is a shape/size
// or `from`/`at` descriptor.
bool LooksLikeGradientColor(const std::string& raw) {
  const std::string value = LowerAscii(TrimAscii(raw));
  if (value.empty()) return false;
  if (value[0] == '#') return true;
  if (StartsWith(value, "rgb(") || StartsWith(value, "rgba(") ||
      StartsWith(value, "hsl(") || StartsWith(value, "hsla(")) {
    return true;
  }
  uint32_t color = 0u;
  return ParseCssColor(value, &color);
}

void ParseGradientPosition(const std::vector<std::string>& tokens,
                           size_t at,
                           AppStyle* style) {
  std::string position;
  for (size_t i = at + 1; i < tokens.size(); i++) {
    if (!position.empty()) position += ' ';
    position += tokens[i];
  }
  CssPositionAxis x;
  CssPositionAxis y;
  if (!ParsePositionPair(position, &x, &y)) return;
  style->gradient_center_x = x.value;
  style->gradient_center_y = y.value;
  style->gradient_center_x_percent = x.percent;
  style->gradient_center_y_percent = y.percent;
}

// Parses one color-stop position (percentage, px, or conic angle).
bool ParseGradientStopToken(const std::string& raw,
                            bool conic,
                            double* value,
                            uint8_t* is_percent) {
  const std::string token = TrimAscii(raw);
  if (conic) {
    const std::string lower = LowerAscii(token);
    // Angles must be detected before lengths: ParseNumber keeps the leading
    // numeric prefix, so "180deg" would otherwise parse as 180px.
    if (EndsWith(lower, "deg") || EndsWith(lower, "rad") ||
        EndsWith(lower, "turn")) {
      double radians = 0.0;
      if (!ParseAngleToken(token, &radians)) return false;
      *value = radians / (2.0 * kPi);
      *is_percent = 1;
      return true;
    }
    if (token == "0") {
      *value = 0.0;
      *is_percent = 1;
      return true;
    }
  }
  float px = 0.0f;
  float percent = -1.0f;
  if (ParseLength(token, &px, &percent)) {
    if (percent >= 0.0f) {
      *value = percent;
      *is_percent = 1;
    } else {
      *value = px;
      *is_percent = 0;
    }
    return true;
  }
  return false;
}

// Parses one gradient's color stops.  `conic` also accepts angle positions,
// which are normalized to 0..1 turns.  A colour may carry two positions
// (`red 0deg 90deg`), producing two hard-stop entries.
void ParseGradientColorStops(const std::vector<std::string>& layers,
                             size_t first,
                             bool conic,
                             std::vector<uint32_t>* colors,
                             std::vector<double>* stops,
                             std::vector<uint8_t>* stop_percent) {
  for (size_t i = first; i < layers.size(); i++) {
    const std::string part = TrimAscii(layers[i]);
    if (part.empty()) continue;
    std::string color_text;
    std::string stop_text;
    const std::string lower = LowerAscii(part);
    if (StartsWith(lower, "rgb(") || StartsWith(lower, "rgba(")) {
      const size_t close = part.find(')');
      if (close == std::string::npos) continue;
      color_text = part.substr(0, close + 1);
      stop_text = TrimAscii(part.substr(close + 1));
    } else {
      const std::vector<std::string> tokens = SplitWhitespace(part);
      if (tokens.empty()) continue;
      color_text = tokens[0];
      // At most two position tokens (`red 0deg 90deg`).
      for (size_t s = 1; s < tokens.size() && s <= 2; s++) {
        if (!stop_text.empty()) stop_text += ' ';
        stop_text += tokens[s];
      }
    }
    uint32_t color = 0u;
    if (!ParseCssColor(color_text, &color)) continue;

    std::vector<double> positions;
    std::vector<uint8_t> percents;
    for (const std::string& token : SplitWhitespace(stop_text)) {
      if (positions.size() >= 2) break;
      double value = 0.0;
      uint8_t is_percent = 1;
      if (ParseGradientStopToken(token, conic, &value, &is_percent)) {
        positions.push_back(value);
        percents.push_back(is_percent);
      }
    }
    if (positions.empty()) {
      positions.push_back(colors->empty() ? 0.0 : -1.0);
      percents.push_back(1);
    }
    for (size_t p = 0; p < positions.size(); p++) {
      colors->push_back(color);
      stops->push_back(positions[p]);
      stop_percent->push_back(percents[p]);
    }
  }
  // Distribute unspecified stops evenly.
  double previous = 0.0;
  for (size_t i = 0; i < stops->size(); i++) {
    if ((*stops)[i] < 0.0) {
      (*stops)[i] = static_cast<double>(i) /
                    static_cast<double>(stops->size() - 1);
      if ((*stops)[i] < previous) (*stops)[i] = previous;
      (*stop_percent)[i] = 1;
    }
    previous = (*stops)[i];
  }
}

// Parses the first CSS `linear-gradient(...)`, `radial-gradient(...)` or
// `conic-gradient(...)` layer into AppStyle.
void ParseCssGradient(AppStyle* style, const std::string& raw) {
  CssGradientKind kind = kGradientNone;
  std::string args;
  if (!FindCssGradient(raw, &kind, &args)) return;
  const std::vector<std::string> layers = SplitTopLevelComma(args);
  if (layers.empty()) return;

  size_t first_color = 0;
  std::vector<uint32_t> colors;
  std::vector<double> stops;
  std::vector<uint8_t> stop_percent;

  if (kind == kGradientLinear) {
    double angle = 0.0;  // CSS 0deg = to top; NewLinearGradient uses radians.
    const std::string head = LowerAscii(TrimAscii(layers[0]));
    if (StartsWith(head, "to ")) {
      const std::string direction = TrimAscii(head.substr(3));
      if (direction == "right") angle = kPi / 2.0;
      else if (direction == "bottom") angle = kPi;
      else if (direction == "left") angle = -kPi / 2.0;
      else angle = 0.0;  // to top
      first_color = 1;
    } else {
      double parsed = 0.0;
      if (ParseAngleToken(head, &parsed)) {
        angle = parsed;
        first_color = 1;
      }
    }
    ParseGradientColorStops(layers, first_color, false, &colors, &stops,
                            &stop_percent);
    if (colors.size() < 2) return;
    style->gradient_angle = angle;
  } else if (kind == kGradientRadial) {
    // RadialGradientSize: 0 farthest-corner (default), 1 closest-side,
    // 2 closest-corner, 3 farthest-side, 4 explicit length.
    int size = 0;
    float radius = -1.0f;
    if (!LooksLikeGradientColor(layers[0])) {
      first_color = 1;
      const std::vector<std::string> tokens = SplitWhitespace(layers[0]);
      for (size_t i = 0; i < tokens.size(); i++) {
        const std::string token = LowerAscii(tokens[i]);
        if (token == "closest-side") {
          size = 1;
          continue;
        }
        if (token == "closest-corner") {
          size = 2;
          continue;
        }
        if (token == "farthest-side") {
          size = 3;
          continue;
        }
        if (token == "farthest-corner") {
          size = 0;
          continue;
        }
        if (token == "at") {
          ParseGradientPosition(tokens, i, style);
          break;
        }
        float px = 0.0f;
        float percent = -1.0f;
        if (ParseLength(tokens[i], &px, &percent) && percent < 0.0f &&
            px >= 0.0f) {
          radius = px;
          size = 4;
        }
      }
    }
    ParseGradientColorStops(layers, first_color, false, &colors, &stops,
                            &stop_percent);
    if (colors.size() < 2) return;
    style->gradient_radius = radius;
    style->gradient_radial_size = size;
  } else if (kind == kGradientConic) {
    double start_angle = 0.0;
    if (!LooksLikeGradientColor(layers[0])) {
      first_color = 1;
      const std::vector<std::string> tokens = SplitWhitespace(layers[0]);
      for (size_t i = 0; i < tokens.size(); i++) {
        const std::string token = LowerAscii(tokens[i]);
        if (token == "from" && i + 1 < tokens.size()) {
          double parsed = 0.0;
          if (ParseAngleToken(tokens[i + 1], &parsed)) start_angle = parsed;
          i++;
          continue;
        }
        if (token == "at") {
          ParseGradientPosition(tokens, i, style);
          break;
        }
      }
    }
    ParseGradientColorStops(layers, first_color, true, &colors, &stops,
                            &stop_percent);
    if (colors.size() < 2) return;
    style->gradient_angle = start_angle;
  } else {
    return;
  }

  style->has_gradient = true;
  style->gradient_kind = kind;
  style->gradient_colors = std::move(colors);
  style->gradient_stops = std::move(stops);
  style->gradient_stop_percent = std::move(stop_percent);
}

// Parses CSS `border-radius` (1-4 corner values; elliptical `/` is reduced to
// the horizontal radius).
void ParseBorderRadius(AppStyle* style, const std::string& raw) {
  std::string value = raw;
  const size_t slash = value.find('/');
  if (slash != std::string::npos) value.resize(slash);
  const std::vector<std::string> parts = SplitWhitespace(value);
  if (parts.empty()) return;
  float radii[4] = {0, 0, 0, 0};
  bool percent[4] = {false, false, false, false};
  for (size_t i = 0; i < parts.size() && i < 4; i++) {
    float px = 0.0f;
    float pct = -1.0f;
    if (!ParseLength(parts[i], &px, &pct)) return;
    if (pct >= 0.0f) {
      radii[i] = pct;
      percent[i] = true;
    } else {
      radii[i] = px;
    }
  }
  float tl = radii[0];
  float tr = parts.size() > 1 ? radii[1] : tl;
  float br = parts.size() > 2 ? radii[2] : tl;
  float bl = parts.size() > 3 ? radii[3] : tr;
  if (parts.size() == 1) {
    tr = bl = br = tl;
  }
  const float values[4] = {tl, tr, br, bl};
  const bool flags[4] = {
      percent[0], parts.size() > 1 ? percent[1] : percent[0],
      parts.size() > 2 ? percent[2] : percent[0],
      parts.size() > 3 ? percent[3] : (parts.size() > 1 ? percent[1] : percent[0])};
  for (int i = 0; i < 4; i++) {
    style->corner_radius[i] = values[i];
    style->corner_radius_percent[i] = flags[i];
  }
}

void ParseCornerRadius(AppStyle* style, int corner, const std::string& raw) {
  std::string value = raw;
  const size_t slash = value.find('/');
  if (slash != std::string::npos) value.resize(slash);
  float px = 0.0f;
  float pct = -1.0f;
  if (!ParseLength(TrimAscii(value), &px, &pct)) return;
  if (pct >= 0.0f) {
    style->corner_radius[corner] = pct;
    style->corner_radius_percent[corner] = true;
  } else {
    style->corner_radius[corner] = px;
    style->corner_radius_percent[corner] = false;
  }
}

float EffectiveCornerRadius(const AppStyle& style, int corner) {
  return style.corner_radius[corner] >= 0.0f ? style.corner_radius[corner]
                                             : style.radius;
}

bool CornerRadiusPercent(const AppStyle& style, int corner) {
  return style.corner_radius[corner] >= 0.0f &&
         style.corner_radius_percent[corner];
}

bool HasAnyRadius(const AppStyle& style) {
  if (style.radius > 0.0f) return true;
  for (int i = 0; i < 4; i++) {
    if (EffectiveCornerRadius(style, i) > 0.0f) return true;
  }
  return false;
}

// Border side order is top, right, bottom, left.
float EffectiveBorderWidth(const AppStyle& style, int side) {
  return style.side_border_width[side] >= 0.0f ? style.side_border_width[side]
                                               : style.border_width;
}

uint32_t EffectiveBorderColor(const AppStyle& style, int side) {
  return style.side_border_color_set[side] ? style.side_border_color[side]
                                           : style.border_color;
}

bool HasBorder(const AppStyle& style) {
  for (int i = 0; i < 4; i++) {
    if (EffectiveBorderWidth(style, i) > 0.0f) return true;
  }
  return false;
}

bool BorderSidesChanged(const AppStyle& before, const AppStyle& after) {
  for (int i = 0; i < 4; i++) {
    if (std::fabs(EffectiveBorderWidth(before, i) -
                  EffectiveBorderWidth(after, i)) > 0.01f) {
      return true;
    }
    if (EffectiveBorderColor(before, i) != EffectiveBorderColor(after, i)) {
      return true;
    }
  }
  return false;
}

bool RadiiChanged(const AppStyle& a, const AppStyle& b) {
  auto same = [](float x, float y) {
    if (std::isnan(x) || std::isnan(y)) return std::isnan(x) && std::isnan(y);
    return std::fabs(x - y) < 0.01f;
  };
  if (!same(a.radius, b.radius)) return true;
  for (int i = 0; i < 4; i++) {
    if (!same(a.corner_radius[i], b.corner_radius[i])) return true;
    if (a.corner_radius_percent[i] != b.corner_radius_percent[i]) return true;
  }
  return false;
}

// Resolves transform-origin against the measured border box.
void ResolveTransformOrigin(const AppStyle& style, float width, float height,
                            double* out_x, double* out_y) {
  if (!std::isnan(style.transform_origin_x_percent) &&
      style.transform_origin_x_percent >= 0.0f) {
    *out_x = style.transform_origin_x_percent * width;
  } else if (!std::isnan(style.transform_origin_x)) {
    *out_x = style.transform_origin_x;
  } else {
    *out_x = width * 0.5;
  }
  if (!std::isnan(style.transform_origin_y_percent) &&
      style.transform_origin_y_percent >= 0.0f) {
    *out_y = style.transform_origin_y_percent * height;
  } else if (!std::isnan(style.transform_origin_y)) {
    *out_y = style.transform_origin_y;
  } else {
    *out_y = height * 0.5;
  }
}

// Applies the CSS transform-origin z offset to the parsed matrix:
// T(0,0,z) * M * T(0,0,-z).  The YGNode widget applies the 2D origin
// translation itself, so the two compose into the full 3D pivot.
// Percentage translations resolve against the measured border box.
void BuildTransformMatrix(const AppStyle& style, double width, double height,
                          double* out) {
  float matrix[16];
  std::memcpy(matrix, style.transform, sizeof(matrix));
  if (!std::isnan(style.translate_percent_x)) {
    matrix[12] += static_cast<float>(style.translate_percent_x * width);
  }
  if (!std::isnan(style.translate_percent_y)) {
    matrix[13] += static_cast<float>(style.translate_percent_y * height);
  }
  if (std::fabs(style.transform_origin_z) > 0.0001f) {
    float pre[16];
    float post[16];
    float temporary[16];
    MatrixIdentity(pre);
    MatrixIdentity(post);
    pre[14] = style.transform_origin_z;
    post[14] = -style.transform_origin_z;
    MatrixMultiply(pre, matrix, temporary);
    MatrixMultiply(temporary, post, matrix);
  }
  for (int i = 0; i < 16; i++) out[i] = static_cast<double>(matrix[i]);
}

// Whitespace tokenizer that keeps `rgba(0, 0, 0, 0.5)` together.
std::vector<std::string> TokenizeCssValue(const std::string& value) {
  std::vector<std::string> out;
  std::string current;
  int depth = 0;
  for (char c : value) {
    if (c == '(') depth++;
    if (c == ')' && depth > 0) depth--;
    if (std::isspace(static_cast<unsigned char>(c)) && depth == 0) {
      if (!current.empty()) {
        out.push_back(current);
        current.clear();
      }
    } else {
      current.push_back(c);
    }
  }
  if (!current.empty()) out.push_back(current);
  return out;
}

// `box-shadow: x y [blur] [spread] color [inset]` (first layer only).
void ParseBoxShadow(const std::string& raw, AppStyle* style) {
  const std::string value = TrimAscii(raw);
  if (value == "none" || value.empty()) {
    style->has_shadow = false;
    return;
  }
  const std::vector<std::string> layers = SplitTopLevelComma(value);
  if (layers.empty()) return;
  const std::vector<std::string> tokens = TokenizeCssValue(layers[0]);
  float numbers[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  int number_count = 0;
  bool inset = false;
  bool have_color = false;
  uint32_t color = style->color;
  for (const std::string& token : tokens) {
    const std::string lowered = LowerAscii(token);
    if (lowered == "inset") {
      inset = true;
      continue;
    }
    uint32_t parsed_color = 0u;
    if (ParseCssColor(token, &parsed_color)) {
      color = parsed_color;
      have_color = true;
      continue;
    }
    double number = 0.0;
    if (number_count < 4 && ParsePxToken(token, &number)) {
      numbers[number_count++] = static_cast<float>(number);
    }
  }
  if (number_count < 2) return;
  style->has_shadow = true;
  style->shadow_offset_x = numbers[0];
  style->shadow_offset_y = numbers[1];
  style->shadow_blur = numbers[2];
  style->shadow_spread = numbers[3];
  style->shadow_color = have_color ? color : style->color;
  style->shadow_inset = inset;
}

void ApplyFlexBasis(AppStyle* style, const std::string& raw) {
  const std::string value = LowerAscii(TrimAscii(raw));
  if (value.empty() || value == "auto" || value == "content") {
    style->flex_basis = std::nanf("");
    style->flex_basis_percent = std::nanf("");
    return;
  }
  float px = -1.0f;
  float percent = -1.0f;
  if (!ParseLength(value, &px, &percent)) return;
  if (percent >= 0.0f) {
    style->flex_basis_percent = percent;
    style->flex_basis = std::nanf("");
  } else if (px >= 0.0f) {
    style->flex_basis = px;
    style->flex_basis_percent = std::nanf("");
  }
}

// CSS `flex` shorthand: `<grow> [<shrink>] [<basis>]`, `none`, `auto` or
// `initial`.  A single number implies `1` shrink and `0%` basis.
void ApplyFlexShorthand(AppStyle* style, const std::string& raw) {
  const std::vector<std::string> parts = SplitWhitespace(raw);
  if (parts.empty()) return;
  const std::string first = LowerAscii(parts[0]);
  if (parts.size() == 1 && (first == "none" || first == "auto" ||
                            first == "initial")) {
    if (first == "none") {
      style->flex = 0.0f;
      style->flex_shrink = 0.0f;
    } else if (first == "auto") {
      style->flex = 1.0f;
      style->flex_shrink = 1.0f;
    } else {
      style->flex = 0.0f;
      style->flex_shrink = 1.0f;
    }
    style->has_flex = true;
    style->has_flex_shrink = true;
    ApplyFlexBasis(style, "auto");
    return;
  }
  double grow = 0.0;
  if (ParseNumber(first, &grow)) {
    style->flex = static_cast<float>(grow);
    style->has_flex = true;
    style->flex_shrink = 1.0f;
    style->has_flex_shrink = true;
    ApplyFlexBasis(style, "0px");
    if (parts.size() >= 2) {
      double shrink = 0.0;
      if (ParseNumber(parts[1], &shrink)) {
        style->flex_shrink = static_cast<float>(shrink);
        if (parts.size() >= 3) ApplyFlexBasis(style, parts[2]);
      } else {
        ApplyFlexBasis(style, parts[1]);
      }
    }
    return;
  }
  // A lone width token: `1 1 <basis>`.
  ApplyFlexBasis(style, parts[0]);
  style->flex = 1.0f;
  style->has_flex = true;
  style->flex_shrink = 1.0f;
  style->has_flex_shrink = true;
}

bool IsHorizontalPositionKeyword(const std::string& token) {
  return token == "left" || token == "right";
}

bool IsVerticalPositionKeyword(const std::string& token) {
  return token == "top" || token == "bottom";
}

bool ParsePositionOffset(const std::string& raw, CssPositionAxis* out) {
  float px = -1.0f;
  float percent = -1.0f;
  if (!ParseLength(raw, &px, &percent)) return false;
  if (percent >= 0.0f) {
    out->value = percent * 100.0f;
    out->percent = true;
  } else if (px >= 0.0f) {
    out->value = px;
    out->percent = false;
  } else {
    return false;
  }
  out->from_end = false;
  return true;
}

bool ParsePositionAxis(const std::string& raw, bool horizontal,
                       CssPositionAxis* out) {
  const std::string token = LowerAscii(TrimAscii(raw));
  if (token == "center") {
    out->value = 50.0f;
    out->percent = true;
    out->from_end = false;
    return true;
  }
  if (horizontal) {
    if (token == "left") {
      out->value = 0.0f;
      out->percent = true;
      out->from_end = false;
      return true;
    }
    if (token == "right") {
      out->value = 100.0f;
      out->percent = true;
      out->from_end = false;
      return true;
    }
  } else {
    if (token == "top") {
      out->value = 0.0f;
      out->percent = true;
      out->from_end = false;
      return true;
    }
    if (token == "bottom") {
      out->value = 100.0f;
      out->percent = true;
      out->from_end = false;
      return true;
    }
  }
  return ParsePositionOffset(token, out);
}

// Parses the 1/2/4-value CSS <position> syntax.  Supports keyword, percentage
// and px forms plus the `right 10px` / `bottom 20%` edge offsets.
bool ParsePositionPair(const std::string& raw,
                       CssPositionAxis* x,
                       CssPositionAxis* y) {
  const std::vector<std::string> parts = SplitWhitespace(raw);
  if (parts.empty()) return false;
  if (parts.size() == 1) {
    const std::string token = LowerAscii(parts[0]);
    if (IsVerticalPositionKeyword(token)) {
      if (!ParsePositionAxis(token, false, y)) return false;
      *x = CssPositionAxis{50.0f, true, false};
      return true;
    }
    if (!ParsePositionAxis(token, true, x)) return false;
    *y = CssPositionAxis{50.0f, true, false};
    return true;
  }
  if (parts.size() == 2) {
    const std::string first = LowerAscii(parts[0]);
    if (IsVerticalPositionKeyword(first)) {
      if (!ParsePositionAxis(first, false, y)) return false;
      if (!ParsePositionAxis(parts[1], true, x)) return false;
      return true;
    }
    if (!ParsePositionAxis(parts[0], true, x)) return false;
    if (!ParsePositionAxis(parts[1], false, y)) return false;
    return true;
  }
  if (parts.size() >= 4) {
    // [left|right <offset>] [top|bottom <offset>] in either order.
    CssPositionAxis xa;
    CssPositionAxis ya;
    bool have_x = false;
    bool have_y = false;
    const std::string first = LowerAscii(parts[0]);
    const std::string third = LowerAscii(parts[2]);
    if (IsHorizontalPositionKeyword(first) && ParsePositionOffset(parts[1], &xa)) {
      xa.from_end = first == "right";
      have_x = true;
      if (IsVerticalPositionKeyword(third) &&
          ParsePositionOffset(parts[3], &ya)) {
        ya.from_end = third == "bottom";
        have_y = true;
      }
    } else if (IsVerticalPositionKeyword(first) &&
               ParsePositionOffset(parts[1], &ya)) {
      ya.from_end = first == "bottom";
      have_y = true;
      if (IsHorizontalPositionKeyword(third) &&
          ParsePositionOffset(parts[3], &xa)) {
        xa.from_end = third == "right";
        have_x = true;
      }
    }
    if (have_x && have_y) {
      *x = xa;
      *y = ya;
      return true;
    }
  }
  return false;
}

// Extracts `<position>` from the `background` shorthand.  Tokens after `/`
// are background-size, and colour/image/repeat/fit keywords are ignored.
void ParseBackgroundShorthandPosition(AppStyle* style, const std::string& raw) {
  std::vector<std::string> position;
  bool after_slash = false;
  for (const std::string& raw_token : TokenizeCssValue(raw)) {
    const std::string token = LowerAscii(raw_token);
    if (token.empty() || token == ",") {
      if (token == ",") break;  // first layer only
      continue;
    }
    if (token == "/") {
      after_slash = true;
      continue;
    }
    const size_t slash = token.find('/');
    if (slash != std::string::npos) {
      const std::string before = TrimAscii(token.substr(0, slash));
      if (!after_slash && !before.empty()) position.push_back(before);
      after_slash = true;
      continue;
    }
    if (after_slash) continue;
    if (token.find('(') != std::string::npos) continue;
    if (token == "repeat" || token == "repeat-x" || token == "repeat-y" ||
        token == "no-repeat" || token == "space" || token == "round" ||
        token == "cover" || token == "contain" || token == "auto" ||
        token == "none" || token == "scroll" || token == "fixed" ||
        token == "local" || token == "border-box" ||
        token == "padding-box" || token == "content-box") {
      continue;
    }
    uint32_t color = 0u;
    if (ParseCssColor(token, &color)) continue;
    position.push_back(token);
  }
  if (position.empty() || position.size() > 4) return;
  std::string joined;
  for (const std::string& token : position) {
    if (!joined.empty()) joined.push_back(' ');
    joined += token;
  }
  ParsePositionPair(joined, &style->background_position_x,
                    &style->background_position_y);
}

// Extracts `<size>` after the top-level `/` of the `background` shorthand,
// e.g. `background: url(x) center / 50px 50% no-repeat`.
void ParseBackgroundShorthandSize(AppStyle* style, const std::string& raw) {
  std::vector<std::string> size;
  bool after_slash = false;
  for (const std::string& raw_token : TokenizeCssValue(raw)) {
    std::string token = raw_token;
    const size_t slash = token.find('/');
    if (slash != std::string::npos) {
      after_slash = true;
      token = TrimAscii(token.substr(slash + 1));
      if (token.empty()) continue;
    } else if (!after_slash) {
      continue;
    }
    if (token == ",") break;
    const std::string lower = LowerAscii(token);
    float px = 0.0f;
    float percent = -1.0f;
    const bool is_length = ParseLength(token, &px, &percent);
    if (lower != "auto" && lower != "cover" && lower != "contain" &&
        !is_length) {
      break;  // repeat/fit/box keywords end the size list
    }
    size.push_back(token);
    if (lower == "cover" || lower == "contain" || size.size() >= 2) break;
  }
  if (size.empty()) return;
  std::string joined;
  for (const std::string& token : size) {
    if (!joined.empty()) joined.push_back(' ');
    joined += token;
  }
  ParseBackgroundSize(style, joined);
}

// Parses a CSS `font-family` list.  Quoted names are unquoted; the order is
// preserved because it becomes the front of the text fallback chain.
std::vector<std::string> ParseFontFamilies(const std::string& raw) {
  std::vector<std::string> out;
  for (const std::string& layer : SplitTopLevelComma(raw)) {
    std::string family = TrimAscii(layer);
    if (family.size() >= 2 &&
        (family.front() == '"' || family.front() == '\'') &&
        family.back() == family.front()) {
      family = family.substr(1, family.size() - 2);
    }
    if (!family.empty()) out.push_back(family);
  }
  return out;
}

void ApplyDeclaration(AppStyle* style,
                      const std::string& raw_key,
                      const std::string& raw_value) {
  const std::string key = LowerAscii(TrimAscii(raw_key));
  const std::string value = TrimAscii(raw_value);
  if (key.empty() || value.empty()) return;

  if (key == "display") {
    const std::string display = LowerAscii(value);
    style->display_none = display == "none";
    style->inline_block = display == "inline-block" ||
                          display == "inline-flex" || display == "inline-grid";
    style->inline_level = display == "inline";
    style->has_display = true;
  } else if (key == "pointer-events") {
    style->pointer_events_none = LowerAscii(value) == "none";
  } else if (key == "overflow" || key == "overflow-x" ||
             key == "overflow-y") {
    style->clip = value == "hidden" || value == "clip" || value == "scroll" ||
                  value == "auto";
  } else if (key == "position") {
    style->position = value == "fixed"      ? 2
                      : value == "absolute" ? 1
                      : value == "sticky"   ? 3
                      : value == "relative" ? 4
                                            : 0;
  } else if (key == "top" || key == "right" || key == "bottom" ||
             key == "left") {
    int index = key == "top" ? 0 : key == "right" ? 1 : key == "bottom" ? 2 : 3;
    float px = -1.0f;
    float percent = -1.0f;
    if (!ParseLength(value, &px, &percent)) return;
    if (percent >= 0.0f) {
      style->inset[index] = std::nan("");
      style->inset_percent[index] = percent;
    } else if (value == "auto") {
      style->inset[index] = std::nan("");
      style->inset_percent[index] = std::nan("");
    } else {
      style->inset[index] = px;
      style->inset_percent[index] = std::nan("");
    }
  } else if (key == "z-index") {
    double number = 0.0;
    if (ParseNumber(value, &number)) {
      style->z_index = static_cast<int>(number);
    }
  } else if (key == "transform") {
    if (value != "none") ParseTransform(value, style);
  } else if (key == "transform-origin") {
    ParseTransformOrigin(style, value);
  } else if (key == "transition") {
    ParseTransitionShorthand(style, value);
  } else if (key == "transition-property") {
    uint32_t mask = 0;
    for (const std::string& part : SplitTopLevelComma(value)) {
      mask |= TransitionPropertyBit(part);
    }
    style->transition_mask = mask;
  } else if (key == "transition-duration") {
    double duration = 0.0;
    const std::vector<std::string> parts = SplitTopLevelComma(value);
    if (!parts.empty() && ParseTimeMs(parts[0], &duration)) {
      style->transition_duration = duration;
    }
  } else if (key == "transition-delay") {
    double delay = 0.0;
    const std::vector<std::string> parts = SplitTopLevelComma(value);
    if (!parts.empty() && ParseTimeMs(parts[0], &delay)) {
      style->transition_delay = delay;
    }
  } else if (key == "transition-timing-function") {
    style->transition_easing = TransitionEasing(value);
  } else if (key == "box-shadow") {
    ParseBoxShadow(value, style);
  } else if (key == "background-image" || key == "background") {
    if (value.find("linear-gradient") != std::string::npos ||
        value.find("radial-gradient") != std::string::npos ||
        value.find("conic-gradient") != std::string::npos) {
      ParseCssGradient(style, value);
    }
    const std::string url = ParseCssUrl(value);
    if (!url.empty()) style->background_image = url;
    if (value.find("repeat-x") != std::string::npos) {
      style->background_repeat = 2;
    } else if (value.find("repeat-y") != std::string::npos) {
      style->background_repeat = 3;
    } else if (value.find("no-repeat") != std::string::npos) {
      style->background_repeat = 0;
    } else if (value.find("repeat") != std::string::npos) {
      style->background_repeat = 1;
    }
    if (value.find("cover") != std::string::npos) style->background_fit = 2;
    else if (value.find("contain") != std::string::npos) style->background_fit = 1;
    if (key == "background") {
      // The shorthand may also carry a colour token.
      for (const std::string& token : TokenizeCssValue(value)) {
        uint32_t color = 0u;
        if (ParseCssColor(token, &color)) {
          style->background = color;
          style->has_background = true;
          break;
        }
      }
      ParseBackgroundShorthandPosition(style, value);
      ParseBackgroundShorthandSize(style, value);
    }
  } else if (key == "background-repeat") {
    style->background_repeat = ParseBackgroundRepeat(value);
  } else if (key == "background-size") {
    ParseBackgroundSize(style, value);
  } else if (key == "background-position") {
    ParsePositionPair(value, &style->background_position_x,
                      &style->background_position_y);
  } else if (key == "background-position-x") {
    ParsePositionAxis(value, true, &style->background_position_x);
  } else if (key == "background-position-y") {
    ParsePositionAxis(value, false, &style->background_position_y);
  } else if (key == "flex-direction") {
    style->row = StartsWith(value, "row");
  } else if (key == "flex-wrap") {
    style->wrap = value == "wrap" || value == "wrap-reverse";
  } else if (key == "align-content") {
    if (value == "flex-start" || value == "start") {
      style->align_content = 1;
    } else if (value == "center") {
      style->align_content = 2;
    } else if (value == "flex-end" || value == "end") {
      style->align_content = 3;
    } else if (value == "space-between" || value == "space-around" ||
               value == "stretch") {
      style->align_content = 0;
    }
  } else if (key == "order") {
    double number = 0.0;
    if (ParseNumber(value, &number)) {
      style->order = static_cast<int>(number);
    }
  } else if (key == "justify-content") {
    style->justify = ParseJustify(value);
  } else if (key == "align-items") {
    style->align = ParseAlign(value);
  } else if (key == "align-self") {
    style->align_self = value == "auto" ? -1 : ParseAlign(value);
  } else if (key == "white-space") {
    const std::string white_space = LowerAscii(value);
    style->white_space = white_space == "nowrap"  ? 1
                         : white_space == "pre"   ? 2
                         : white_space == "pre-wrap" ? 3
                         : white_space == "pre-line" ? 4
                                                     : 0;
  } else if (key == "text-overflow") {
    const std::string text_overflow = LowerAscii(value);
    style->text_overflow = text_overflow == "ellipsis"   ? 2
                            : text_overflow == "fade"    ? 1
                            : text_overflow == "clip"    ? 0
                            : text_overflow == "visible" ? 3
                                                         : 3;
  } else if (key == "aspect-ratio") {
    // CSS syntax: `auto`, `<ratio>`, or `auto <ratio>`, where a ratio is
    // `number` or `number / number`.
    std::string ratio = LowerAscii(value);
    if (StartsWith(ratio, "auto")) ratio = TrimAscii(ratio.substr(4));
    double numerator = 0.0;
    double denominator = 1.0;
    const size_t slash = ratio.find('/');
    if (slash == std::string::npos) {
      if (ParseNumber(ratio, &numerator) && numerator > 0.0) {
        style->aspect_ratio = static_cast<float>(numerator);
      }
    } else {
      const std::string left = TrimAscii(ratio.substr(0, slash));
      const std::string right = TrimAscii(ratio.substr(slash + 1));
      if (ParseNumber(left, &numerator) && ParseNumber(right, &denominator) &&
          numerator > 0.0 && denominator > 0.0) {
        style->aspect_ratio = static_cast<float>(numerator / denominator);
      }
    }
  } else if (key == "box-sizing") {
    style->box_sizing = LowerAscii(value) == "content-box" ? 1 : 0;
  } else if (key == "width" || key == "min-width" || key == "max-width" ||
             key == "height" || key == "min-height" || key == "max-height") {
    float px = -1.0f;
    float percent = -1.0f;
    if (!ParseLength(value, &px, &percent)) return;
    if (key == "width") {
      style->width = px;
      style->width_percent = percent;
    } else if (key == "height") {
      style->height = px;
      style->height_percent = percent;
    } else if (key == "min-width") {
      style->min_width = px > 0 ? px : 0.0f;
    } else if (key == "max-width") {
      style->max_width = px > 0 ? px : kInf;
    } else if (key == "min-height") {
      style->min_height = px > 0 ? px : 0.0f;
    } else if (key == "max-height") {
      style->max_height = px > 0 ? px : kInf;
    }
  } else if (key == "padding" || key == "margin") {
    const std::vector<std::string> parts = SplitWhitespace(value);
    if (parts.empty()) return;
    float px_values[4] = {0, 0, 0, 0};
    float pct_values[4] = {std::nanf(""), std::nanf(""), std::nanf(""),
                           std::nanf("")};
    for (size_t i = 0; i < parts.size() && i < 4; i++) {
      float px = 0.0f;
      float percent = -1.0f;
      if (!ParseLength(parts[i], &px, &percent)) return;
      if (percent >= 0.0f) {
        pct_values[i] = percent;
        px_values[i] = 0.0f;
      } else {
        px_values[i] = px > 0.0f ? px : 0.0f;
      }
    }
    const size_t n = parts.size();
    const int top = 0;
    const int right = n > 1 ? 1 : 0;
    const int bottom = n > 2 ? 2 : 0;
    const int left = n > 3 ? 3 : (n > 1 ? 1 : 0);
    float* target_px = key == "margin" ? style->margin : style->padding;
    float* target_pct =
        key == "margin" ? style->margin_percent : style->padding_percent;
    const int order[4] = {top, right, bottom, left};
    for (int i = 0; i < 4; i++) {
      target_px[i] = px_values[order[i]];
      target_pct[i] = pct_values[order[i]];
    }
  } else if (key == "padding-top" || key == "padding-right" ||
             key == "padding-bottom" || key == "padding-left" ||
             key == "margin-top" || key == "margin-right" ||
             key == "margin-bottom" || key == "margin-left") {
    const int side = EndsWith(key, "-top")      ? 0
                     : EndsWith(key, "-right")  ? 1
                     : EndsWith(key, "-bottom") ? 2
                                                 : 3;
    float px = 0.0f;
    float percent = -1.0f;
    if (!ParseLength(value, &px, &percent)) return;
    float* target_px =
        StartsWith(key, "margin") ? style->margin : style->padding;
    float* target_pct = StartsWith(key, "margin") ? style->margin_percent
                                                  : style->padding_percent;
    if (percent >= 0.0f) {
      target_pct[side] = percent;
      target_px[side] = 0.0f;
    } else {
      target_px[side] = px > 0.0f ? px : 0.0f;
      target_pct[side] = std::nanf("");
    }
  } else if (key == "gap" || key == "column-gap" || key == "row-gap") {
    if (key == "gap") {
      const std::vector<std::string> parts = SplitWhitespace(value);
      float row = 0.0f;
      float column = 0.0f;
      float percent = -1.0f;
      if (!parts.empty() && ParseLength(parts[0], &row, &percent)) {
        column = row;
        if (parts.size() > 1) {
          float second_percent = -1.0f;
          ParseLength(parts[1], &column, &second_percent);
        }
        style->gap = row;
        style->row_gap = row;
        style->column_gap = column;
      }
    } else {
      float px = 0.0f;
      float percent = -1.0f;
      if (ParseLength(value, &px, &percent) && px >= 0) {
        if (key == "row-gap") {
          style->row_gap = px;
          style->gap = px;
        } else {
          style->column_gap = px;
          style->gap = px;
        }
      }
    }
  } else if (key == "flex") {
    ApplyFlexShorthand(style, value);
  } else if (key == "flex-grow") {
    double number = 0.0;
    if (ParseNumber(value, &number) && number >= 0.0) {
      style->flex = static_cast<float>(number);
      style->has_flex = true;
    }
  } else if (key == "flex-shrink") {
    double number = 0.0;
    if (ParseNumber(value, &number) && number >= 0.0) {
      style->flex_shrink = static_cast<float>(number);
      style->has_flex_shrink = true;
    }
  } else if (key == "flex-basis") {
    ApplyFlexBasis(style, value);
  } else if (key == "background" || key == "background-color") {
    uint32_t color = 0u;
    if (ParseCssColor(value, &color)) {
      style->background = color;
      style->has_background = true;
    }
  } else if (key == "color") {
    uint32_t color = 0u;
    if (ParseCssColor(value, &color)) style->color = color;
  } else if (key == "font-size") {
    float px = 0.0f;
    float percent = -1.0f;
    if (ParseLength(value, &px, &percent) && px > 0) style->font_size = px;
  } else if (key == "font-weight") {
    if (value == "bold") {
      style->font_weight = 6;
    } else if (value == "normal") {
      style->font_weight = 3;
    } else {
      double number = 0.0;
      if (ParseNumber(value, &number)) {
        int index = static_cast<int>(number / 100.0) - 1;
        if (index < 0) index = 0;
        if (index > 8) index = 8;
        style->font_weight = index;
      }
    }
  } else if (key == "font-style") {
    style->font_style = value == "italic" ? 1 : 0;
  } else if (key == "font-family") {
    style->font_families = ParseFontFamilies(value);
  } else if (key == "line-height") {
    if (value == "normal") {
      style->line_height = -1.0;
      style->line_height_factor = 1.2;
    } else if (EndsWith(value, "px")) {
      float px = 0.0f;
      float percent = -1.0f;
      if (ParseLength(value, &px, &percent) && px > 0) style->line_height = px;
    } else {
      double number = 0.0;
      if (ParseNumber(value, &number) && number > 0) {
        style->line_height_factor = number;
        style->line_height = -1.0;
      }
    }
  } else if (key == "letter-spacing") {
    float px = 0.0f;
    float percent = -1.0f;
    if (ParseLength(value, &px, &percent)) style->letter_spacing = px;
  } else if (key == "text-align") {
    if (value == "center") style->text_align = 2;
    else if (value == "right" || value == "end") style->text_align = 1;
    else if (value == "justify") style->text_align = 3;
    else style->text_align = 0;
  } else if (key == "border" || key == "border-top" ||
             key == "border-right" || key == "border-bottom" ||
             key == "border-left" || key == "border-top-width" ||
             key == "border-right-width" || key == "border-bottom-width" ||
             key == "border-left-width" || key == "border-top-color" ||
             key == "border-right-color" || key == "border-bottom-color" ||
             key == "border-left-color") {
    const bool uniform = key == "border";
    int side = -1;
    if (!uniform) {
      if (key.find("top") != std::string::npos) side = 0;
      else if (key.find("right") != std::string::npos) side = 1;
      else if (key.find("bottom") != std::string::npos) side = 2;
      else side = 3;
    }
    const bool width_only = EndsWith(key, "-width");
    const bool color_only = EndsWith(key, "-color");
    if (color_only) {
      uint32_t color = 0u;
      if (ParseCssColor(value, &color)) {
        if (uniform) {
          style->border_color = color;
        } else {
          style->side_border_color[side] = color;
          style->side_border_color_set[side] = true;
        }
      }
    } else if (width_only) {
      float px = 0.0f;
      float percent = -1.0f;
      if (ParseLength(value, &px, &percent) && px >= 0.0f) {
        if (uniform) {
          style->border_width = px;
        } else {
          style->side_border_width[side] = px;
        }
      }
    } else {
      const std::vector<std::string> parts = SplitWhitespace(value);
      for (const std::string& part : parts) {
        uint32_t color = 0u;
        if (ParseCssColor(part, &color)) {
          if (uniform) {
            style->border_color = color;
          } else {
            style->side_border_color[side] = color;
            style->side_border_color_set[side] = true;
          }
          continue;
        }
        float px = 0.0f;
        float percent = -1.0f;
        if (ParseLength(part, &px, &percent) && px >= 0.0f) {
          if (uniform) {
            style->border_width = px;
          } else {
            style->side_border_width[side] = px;
          }
        }
      }
    }
  } else if (key == "border-width") {
    const std::vector<std::string> parts = SplitWhitespace(value);
    if (!parts.empty()) {
      float values[4] = {0, 0, 0, 0};
      for (size_t i = 0; i < parts.size() && i < 4; i++) {
        float px = 0.0f;
        float percent = -1.0f;
        if (ParseLength(parts[i], &px, &percent) && px >= 0.0f) {
          values[i] = px;
        }
      }
      const size_t n = parts.size();
      float top = values[0];
      float right = n > 1 ? values[1] : top;
      float bottom = n > 2 ? values[2] : top;
      float left = n > 3 ? values[3] : right;
      style->border_width = top;
      style->side_border_width[0] = top;
      style->side_border_width[1] = right;
      style->side_border_width[2] = bottom;
      style->side_border_width[3] = left;
    }
  } else if (key == "border-color") {
    const std::vector<std::string> parts = SplitWhitespace(value);
    if (!parts.empty()) {
      uint32_t values[4] = {0, 0, 0, 0};
      for (size_t i = 0; i < parts.size() && i < 4; i++) {
        uint32_t color = 0u;
        if (ParseCssColor(parts[i], &color)) values[i] = color;
      }
      const size_t n = parts.size();
      uint32_t top = values[0];
      uint32_t right = n > 1 ? values[1] : top;
      uint32_t bottom = n > 2 ? values[2] : top;
      uint32_t left = n > 3 ? values[3] : right;
      style->border_color = top;
      for (int i = 0; i < 4; i++) {
        style->side_border_color[i] = i == 0 ? top : i == 1 ? right : i == 2 ? bottom : left;
        style->side_border_color_set[i] = true;
      }
    }
  } else if (key == "border-radius") {
    ParseBorderRadius(style, value);
  } else if (key == "border-top-left-radius") {
    ParseCornerRadius(style, 0, value);
  } else if (key == "border-top-right-radius") {
    ParseCornerRadius(style, 1, value);
  } else if (key == "border-bottom-right-radius") {
    ParseCornerRadius(style, 2, value);
  } else if (key == "border-bottom-left-radius") {
    ParseCornerRadius(style, 3, value);
  } else if (key == "opacity") {
    double number = 1.0;
    if (ParseNumber(value, &number)) style->opacity = number;
  } else if (key == "object-fit") {
    if (value == "contain") style->object_fit = 1;
    else if (value == "cover") style->object_fit = 2;
    else if (value == "none") style->object_fit = 3;
    else if (value == "scale-down") style->object_fit = 4;
    else style->object_fit = 0;
  } else if (key == "object-position") {
    if (ParsePositionPair(value, &style->object_position_x,
                          &style->object_position_y)) {
      style->has_object_position = true;
    }
  } else if (key == "object-position-x") {
    if (ParsePositionAxis(value, true, &style->object_position_x)) {
      style->has_object_position = true;
    }
  } else if (key == "object-position-y") {
    if (ParsePositionAxis(value, false, &style->object_position_y)) {
      style->has_object_position = true;
    }
  } else if (key == "-webkit-line-clamp") {
    double number = 0.0;
    if (ParseNumber(value, &number) && number > 0) {
      style->max_lines = static_cast<uint32_t>(number);
    }
  }
}

void ApplyDeclarations(AppStyle* style,
                       const std::vector<std::pair<std::string, std::string>>& decls) {
  for (const auto& decl : decls) ApplyDeclaration(style, decl.first, decl.second);
}

std::vector<std::pair<std::string, std::string>> ParseDeclarations(
    const std::string& body) {
  std::vector<std::pair<std::string, std::string>> out;
  std::string current;
  int paren_depth = 0;
  char quote = '\0';
  auto emit = [&out](const std::string& raw) {
    const size_t colon = raw.find(':');
    if (colon == std::string::npos) return;
    out.emplace_back(TrimAscii(raw.substr(0, colon)),
                     TrimAscii(raw.substr(colon + 1)));
  };
  // Split on top-level semicolons only: data URIs contain `;` inside url(...)
  // (e.g. `url("data:image/svg+xml;charset=utf8,...")`).
  for (char c : body) {
    if (quote != '\0') {
      current.push_back(c);
      if (c == quote) quote = '\0';
    } else if (c == '"' || c == '\'') {
      quote = c;
      current.push_back(c);
    } else if (c == '(') {
      paren_depth++;
      current.push_back(c);
    } else if (c == ')') {
      if (paren_depth > 0) paren_depth--;
      current.push_back(c);
    } else if (c == ';' && paren_depth == 0) {
      emit(current);
      current.clear();
    } else {
      current.push_back(c);
    }
  }
  emit(current);
  return out;
}

// -- CSS ---------------------------------------------------------------------

// A single compound selector (`.anchor-item.selected`) without combinators.

}  // namespace appservice

