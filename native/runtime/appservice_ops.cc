// Appservice operation stream replay and DevTools automation.

#include "internal/skyline_runtime_module.hh"
#include "internal/skyline_runtime_ops.hh"

namespace appservice {

uint32_t MemberUint(const rapidjson::Value& object, const char* key,
                    uint32_t fallback) {
  const auto member = object.FindMember(key);
  if (member == object.MemberEnd() || !member->value.IsUint()) return fallback;
  return member->value.GetUint();
}

// App-bar/tab-bar render contexts use ids beyond uint32 (e.g. 2^32 + pageId),
// which rapidjson stores as Uint64 rather than Uint.
uint64_t MemberUint64(const rapidjson::Value& object, const char* key,
                      uint64_t fallback) {
  const auto member = object.FindMember(key);
  if (member == object.MemberEnd()) return fallback;
  const rapidjson::Value& value = member->value;
  if (value.IsUint64()) return value.GetUint64();
  if (value.IsInt64()) return static_cast<uint64_t>(value.GetInt64());
  if (value.IsUint()) return value.GetUint();
  return fallback;
}

const char* MemberText(const rapidjson::Value& object, const char* key,
                       const char* fallback) {
  const auto member = object.FindMember(key);
  if (member == object.MemberEnd() || !member->value.IsString()) return fallback;
  return member->value.GetString();
}

void ApplyNodeAttribute(AppNode* node, const char* name,
                        const rapidjson::Value& value) {
  if (value.IsString()) {
    ApplyAttribute(node, name, value.GetString());
  } else if (value.IsNumber()) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%g", value.GetDouble());
    ApplyAttribute(node, name, buffer);
  } else if (value.IsBool()) {
    ApplyAttribute(node, name, value.GetBool() ? "1" : "0");
  }
  // Programmatic widget updates.  The native handles exist only after the
  // widget has been materialized, so initial creation is unaffected.
  if (node == nullptr || name == nullptr || *name == '\0') return;
  double number = 0.0;
  bool flag = false;
  if (value.IsNumber()) {
    number = value.GetDouble();
    flag = number != 0.0;
  } else if (value.IsBool()) {
    flag = value.GetBool();
    number = flag ? 1.0 : 0.0;
  } else if (value.IsString()) {
    const char* raw = value.GetString();
    number = std::atof(raw);
    flag = number != 0.0 || std::strcmp(raw, "true") == 0;
  } else {
    return;
  }
  if (IsSwiperNode(node) && std::strcmp(name, "current") == 0) {
    AnimateSwiperTo(node->id, static_cast<uint32_t>(std::max(0.0, number)),
                    AttrNumber(node, "duration", 500.0) > 0.0 &&
                        AttrBool(node, "scrollWithAnimation", true));
  } else if (IsPickerViewColumnNode(node) &&
             (std::strcmp(name, "value") == 0 ||
              std::strcmp(name, "current") == 0)) {
    SetPickerColumnCurrent(node->id,
                           static_cast<uint32_t>(std::max(0.0, number)));
  } else if (node->name == "input" && std::strcmp(name, "focus") == 0) {
    SetInputFocus(node->id, flag);
  } else if (node->scroll_view_p != nullptr &&
             std::strcmp(name, "refresherTriggered") == 0) {
    SetRefresherTriggered(node->id, flag, false);
  } else if (node->scroll_view_p != nullptr &&
             (std::strcmp(name, "refresherTwoLevelTriggered") == 0 ||
              std::strcmp(name, "twoLevelTriggered") == 0)) {
    SetRefresherTriggered(node->id, flag, true);
  } else if (node->snapshot_p != nullptr && g_page != nullptr &&
             g_page->holder != nullptr &&
             std::strcmp(name, "allowSnapshotting") == 0) {
    auto* update = reinterpret_cast<SnapshotUpdateAllowSnapshottingFn>(
        FindCallback(g_page->holder->ffi_table,
                     kSnapshotUpdateAllowSnapshottingHash));
    if (update != nullptr) {
      update(Dart_HandleFromPersistent(node->snapshot_p), flag ? 1 : 0);
    }
  } else if (IsShareElementNode(node) && node->hero_p != nullptr &&
             g_page != nullptr && g_page->holder != nullptr) {
    // WXML property changes (`key`, `transition-on-gesture`, `rect-tween-type`,
    // `shuttle-on-push/pop`) update the existing SkylineHero in place instead
    // of rebuilding the page.
    Dart_Handle hero = Dart_HandleFromPersistent(node->hero_p);
    if (!Dart_IsError(hero) && !Dart_IsNull(hero)) {
      const bool is_tag = std::strcmp(name, "tag") == 0 ||
                          std::strcmp(name, "key") == 0;
      const bool is_gesture = std::strcmp(name, "transitionOnGesture") == 0 ||
                              std::strcmp(name, "transition-on-gesture") == 0;
      const char* string_hash = nullptr;
      if (std::strcmp(name, "rectTweenType") == 0 ||
          std::strcmp(name, "rect-tween-type") == 0) {
        string_hash = "rect";
      } else if (std::strcmp(name, "shuttleOnPush") == 0 ||
                 std::strcmp(name, "shuttle-on-push") == 0) {
        string_hash = "push";
      } else if (std::strcmp(name, "shuttleOnPop") == 0 ||
                 std::strcmp(name, "shuttle-on-pop") == 0) {
        string_hash = "pop";
      }
      if (is_tag) {
        auto* update = reinterpret_cast<UpdateHeroTagFn>(
            FindCallback(g_page->holder->ffi_table, kUpdateHeroTagHash));
        if (update != nullptr && value.IsString()) {
          Dart_Handle handle = Dart_NewStringFromCString(value.GetString());
          if (!Dart_IsError(handle)) update(hero, handle);
        }
      } else if (is_gesture) {
        auto* update = reinterpret_cast<UpdateHeroGestureFn>(
            FindCallback(g_page->holder->ffi_table, kUpdateHeroGestureHash));
        if (update != nullptr) update(hero, flag ? 1 : 0);
      } else if (string_hash != nullptr && value.IsString()) {
        uint32_t hash = string_hash[0] == 'r'
                            ? kUpdateHeroRectTweenTypeHash
                            : (string_hash[0] == 'p'
                                   ? kUpdateHeroShuttleOnPushHash
                                   : kUpdateHeroShuttleOnPopHash);
        auto* update = reinterpret_cast<UpdateHeroStringFn>(
            FindCallback(g_page->holder->ffi_table, hash));
        if (update != nullptr) {
          Dart_Handle handle = Dart_NewStringFromCString(value.GetString());
          if (!Dart_IsError(handle)) update(hero, handle);
        }
      }
    }
  }
  if (IsShareElementNode(node) && node->hero_p == nullptr &&
      (std::strcmp(name, "tag") == 0 || std::strcmp(name, "key") == 0 ||
       std::strcmp(name, "onFrame") == 0 || std::strcmp(name, "onframe") == 0 ||
       std::strcmp(name, "transitionOnGesture") == 0 ||
       std::strcmp(name, "transition-on-gesture") == 0 ||
       std::strcmp(name, "rectTweenType") == 0 ||
       std::strcmp(name, "rect-tween-type") == 0 ||
       std::strcmp(name, "shuttleOnPush") == 0 ||
       std::strcmp(name, "shuttle-on-push") == 0 ||
       std::strcmp(name, "shuttleOnPop") == 0 ||
       std::strcmp(name, "shuttle-on-pop") == 0) &&
      g_page != nullptr) {
    // The builtin component forwards its props after the element was built
    // (the glass-easel observers fire before `$$` exists).  Rebuild once so
    // the Hero is materialized with the resolved tag/gesture/shuttle values.
    g_page->force_rebuild = true;
  }
  if (IsSemanticsAttribute(name)) UpdateNodeSemantics(node);
}

std::string CamelToKebab(const std::string& key) {
  std::string out;
  out.reserve(key.size() + 4);
  for (char c : key) {
    if (c >= 'A' && c <= 'Z') {
      out.push_back('-');
      out.push_back(static_cast<char>(c - 'A' + 'a'));
    } else {
      out.push_back(c);
    }
  }
  return out;
}

bool IsColorDeclaration(const std::string& key) {
  return key == "background-color" || key == "color" ||
         key == "border-color" || EndsWith(key, "-color");
}

bool IsLengthDeclaration(const std::string& key) {
  return key == "width" || key == "height" || key == "left" || key == "top" ||
         key == "right" || key == "bottom" || key == "font-size" ||
         key == "border-width" || key == "border-radius";
}

// Converts a React-Native-style animated style map into CSS declarations.
std::string AnimatedStyleDeclarations(const rapidjson::Value& style) {
  std::string out;
  if (!style.IsObject()) return out;
  for (auto it = style.MemberBegin(); it != style.MemberEnd(); ++it) {
    const std::string key = CamelToKebab(it->name.GetString());
    const rapidjson::Value& value = it->value;
    out += key;
    out.push_back(':');
    if (value.IsString()) {
      out += value.GetString();
    } else if (value.IsBool()) {
      out += value.GetBool() ? "1" : "0";
    } else if (value.IsNumber()) {
      char buffer[64];
      if (IsColorDeclaration(key)) {
        std::snprintf(buffer, sizeof(buffer), "#%08X",
                      static_cast<unsigned>(value.GetUint()));
      } else if (IsLengthDeclaration(key) && value.GetDouble() != 0.0) {
        std::snprintf(buffer, sizeof(buffer), "%gpx", value.GetDouble());
      } else {
        std::snprintf(buffer, sizeof(buffer), "%g", value.GetDouble());
      }
      out += buffer;
    } else if (value.IsArray() && key == "transform") {
      bool first = true;
      for (const rapidjson::Value& entry : value.GetArray()) {
        if (!entry.IsObject()) continue;
        for (auto fn = entry.MemberBegin(); fn != entry.MemberEnd(); ++fn) {
          const std::string name = fn->name.GetString();
          const rapidjson::Value& arg = fn->value;
          if (!first) out.push_back(' ');
          first = false;
          out += name;
          out.push_back('(');
          if (arg.IsString()) {
            out += arg.GetString();
          } else if (arg.IsNumber()) {
            char buffer[64];
            const bool angle = StartsWith(name, "rotate") ||
                               StartsWith(name, "skew");
            const bool scale = StartsWith(name, "scale");
            if (angle) {
              std::snprintf(buffer, sizeof(buffer), "%gdeg", arg.GetDouble());
            } else if (scale) {
              std::snprintf(buffer, sizeof(buffer), "%g", arg.GetDouble());
            } else {
              std::snprintf(buffer, sizeof(buffer), "%gpx", arg.GetDouble());
            }
            out += buffer;
          }
          out.push_back(')');
        }
      }
    } else {
      out += "0";
    }
    out.push_back(';');
  }
  return out;
}

void CollectAnimatedStyles(const rapidjson::Value& op, AppNode* node) {
  if (g_page == nullptr || node == nullptr) return;
  const auto timeline = op.FindMember("timeline");
  if (timeline != op.MemberEnd() && timeline->value.IsArray()) {
    for (const rapidjson::Value& frame : timeline->value.GetArray()) {
      if (!frame.IsObject()) continue;
      const auto style = frame.FindMember("style");
      if (style == frame.MemberEnd()) continue;
      PendingAnimatedStyle entry;
      entry.node_id = node->id;
      entry.at_ms = static_cast<int>(MemberNumber(frame, "atMs", 0.0));
      entry.declarations = AnimatedStyleDeclarations(style->value);
      if (!entry.declarations.empty()) {
        g_page->animated_styles.push_back(entry);
      }
    }
  }
  const auto updates = op.FindMember("updates");
  const auto style = op.FindMember("style");
  const rapidjson::Value* value = nullptr;
  if (updates != op.MemberEnd()) value = &updates->value;
  else if (style != op.MemberEnd()) value = &style->value;
  if (value != nullptr) {
    PendingAnimatedStyle entry;
    entry.node_id = node->id;
    entry.at_ms = static_cast<int>(MemberNumber(op, "atMs", 0.0));
    entry.declarations = AnimatedStyleDeclarations(*value);
    if (!entry.declarations.empty()) {
      g_page->animated_styles.push_back(entry);
    }
  }
  std::fprintf(stderr,
               "[skyline-runtime] addAnimatedStyle: #%u (%zu timeline frame(s))\n",
               node->id, g_page->animated_styles.size());
}

// -- synthetic input state ---------------------------------------------------

int Utf8CodepointCount(const std::string& text) {
  int count = 0;
  size_t index = 0;
  while (index < text.size()) {
    NextCodepoint(text, &index);
    count++;
  }
  return count;
}

size_t Utf8ByteOffset(const std::string& text, int codepoint_index) {
  if (codepoint_index <= 0) return 0;
  size_t index = 0;
  int count = 0;
  while (index < text.size() && count < codepoint_index) {
    NextCodepoint(text, &index);
    count++;
  }
  return index;
}

void Utf8Erase(std::string* text, int start, int end) {
  if (text == nullptr || start >= end) return;
  const size_t begin = Utf8ByteOffset(*text, start);
  const size_t finish = Utf8ByteOffset(*text, end);
  if (begin < finish) text->erase(begin, finish - begin);
}

void Utf8Insert(std::string* text, int position, const std::string& value) {
  if (text == nullptr || value.empty()) return;
  const size_t offset = Utf8ByteOffset(*text, position);
  text->insert(offset, value);
}

void ClampInputState(AppNode* node) {
  if (node == nullptr) return;
  const int length = Utf8CodepointCount(node->text);
  auto clamp = [length](int value) {
    if (value < 0) return 0;
    if (value > length) return length;
    return value;
  };
  node->input_cursor = clamp(node->input_cursor);
  node->input_selection_start = clamp(node->input_selection_start);
  node->input_selection_end = clamp(node->input_selection_end);
}

void CollapseInputSelection(AppNode* node) {
  node->input_selection_start = node->input_cursor;
  node->input_selection_end = node->input_cursor;
}

// Replaces the current selection (or inserts at the cursor) and collapses the
// selection after the inserted text.
void InsertInputText(AppNode* node, const std::string& value) {
  if (node == nullptr) return;
  ClampInputState(node);
  int start = node->input_selection_start;
  int end = node->input_selection_end;
  if (start > end) std::swap(start, end);
  if (start == end) {
    start = end = node->input_cursor;
  }
  Utf8Erase(&node->text, start, end);
  Utf8Insert(&node->text, start, value);
  node->input_cursor = start + Utf8CodepointCount(value);
  node->composing = false;
  node->composition_base_text.clear();
  CollapseInputSelection(node);
}

// IME composition preview: the uncommitted text replaces the marked range of
// the original value until compositionEnd commits it.
void ReplaceComposition(AppNode* node, const std::string& value) {
  if (node == nullptr) return;
  std::string text = node->composition_base_text;
  Utf8Erase(&text, node->composition_start, node->composition_end);
  Utf8Insert(&text, node->composition_start, value);
  node->text = text;
  node->composition_text = value;
  node->input_cursor =
      node->composition_start + Utf8CodepointCount(value);
  node->input_selection_start = node->composition_start;
  node->input_selection_end = node->input_cursor;
}

// Mirrors the state to a real SkylineInput when one was materialized.
void NotifyInputState(AppNode* node) {
  if (node == nullptr || node->input_widget_p == nullptr || g_page == nullptr ||
      g_page->holder == nullptr) {
    if (node != nullptr) {
      std::fprintf(stderr,
                   "[skyline-runtime] InputHoldKeyboard: id=%u hold=%d "
                   "(no native input widget)\n",
                   node->id, node->input_hold_keyboard ? 1 : 0);
    }
    return;
  }
  RuntimeHolder* holder = g_page->holder;
  Dart_Handle widget = Dart_HandleFromPersistent(node->input_widget_p);
  auto* value = reinterpret_cast<InputValueFn>(
      FindCallback(holder->ffi_table, kInputValueHash));
  auto* cursor = reinterpret_cast<InputIntFn>(
      FindCallback(holder->ffi_table, kInputCursorHash));
  auto* selection_start = reinterpret_cast<InputIntFn>(
      FindCallback(holder->ffi_table, kInputSelectionStartHash));
  auto* selection_end = reinterpret_cast<InputIntFn>(
      FindCallback(holder->ffi_table, kInputSelectionEndHash));
  auto* hold = reinterpret_cast<InputBoolFn>(
      FindCallback(holder->ffi_table, kInputHoldKeyboardHash));
  if (value != nullptr) {
    Dart_Handle text = Dart_NewStringFromCString(node->text.c_str());
    if (!Dart_IsError(text)) value(widget, text);
  }
  if (cursor != nullptr) cursor(widget, node->input_cursor);
  if (selection_start != nullptr) {
    selection_start(widget, node->input_selection_start);
  }
  if (selection_end != nullptr) selection_end(widget, node->input_selection_end);
  if (hold != nullptr) {
    hold(widget, node->input_hold_keyboard ? 1 : 0);
    std::fprintf(stderr,
                 "[skyline-runtime] InputHoldKeyboard: id=%u hold=%d\n",
                 node->id, node->input_hold_keyboard ? 1 : 0);
  }
}


// ---------------------------------------------------------------------------
// D10: DevTools automation (tree dumps, GC, snapshots, runner messages)
// ---------------------------------------------------------------------------

FfiTable* ActiveFfiTable() {
  if (g_page != nullptr && g_page->holder != nullptr &&
      g_page->holder->ffi_table != nullptr) {
    return g_page->holder->ffi_table;
  }
  return g_fallback_holder.ffi_table;
}

std::string EncodeBase64(const uint8_t* data, size_t size) {
  static const char kAlphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve(((size + 2) / 3) * 4);
  for (size_t i = 0; i < size; i += 3) {
    const uint32_t b0 = data[i];
    const uint32_t b1 = i + 1 < size ? data[i + 1] : 0;
    const uint32_t b2 = i + 2 < size ? data[i + 2] : 0;
    const uint32_t triple = (b0 << 16) | (b1 << 8) | b2;
    out.push_back(kAlphabet[(triple >> 18) & 0x3F]);
    out.push_back(kAlphabet[(triple >> 12) & 0x3F]);
    out.push_back(i + 1 < size ? kAlphabet[(triple >> 6) & 0x3F] : '=');
    out.push_back(i + 2 < size ? kAlphabet[triple & 0x3F] : '=');
  }
  return out;
}

bool ReadDartBytes(Dart_Handle data, std::vector<uint8_t>* out) {
  if (data == nullptr || Dart_IsError(data) || Dart_IsNull(data)) return false;
  Dart_TypedData_Type type = Dart_TypedData_kInvalid;
  void* bytes = nullptr;
  intptr_t length = 0;
  if (Dart_IsError(Dart_TypedDataAcquireData(data, &type, &bytes, &length)) ||
      bytes == nullptr || length <= 0) {
    return false;
  }
  out->assign(static_cast<uint8_t*>(bytes),
              static_cast<uint8_t*>(bytes) + length);
  Dart_TypedDataReleaseData(data);
  return true;
}

bool WriteSnapshotFile(const char* prefix, uint32_t request_id,
                       const std::string& extension,
                       const std::vector<uint8_t>& bytes) {
  const std::string& dir = config::String(config::kSnapshotDir);
  if (dir.empty()) return false;
  char path[512];
  std::snprintf(path, sizeof(path), "%s/%s-%u.%s", dir.c_str(), prefix,
                request_id, extension.c_str());
  std::ofstream file(path, std::ios::binary);
  if (!file) return false;
  file.write(reinterpret_cast<const char*>(bytes.data()),
             static_cast<std::streamsize>(bytes.size()));
  return file.good();
}

void EmitSnapshotJson(const char* type, uint32_t request_id, int32_t width,
                      int32_t height, const char* format,
                      const std::vector<uint8_t>& bytes) {
  const std::string encoded = EncodeBase64(bytes.data(), bytes.size());
  std::string json = "{\"type\":\"";
  json += type;
  json += "\",\"requestId\":";
  json += std::to_string(request_id);
  if (width > 0 && height > 0) {
    json += ",\"width\":";
    json += std::to_string(width);
    json += ",\"height\":";
    json += std::to_string(height);
  }
  json += ",\"format\":\"";
  json += format;
  json += "\",\"byteLength\":";
  json += std::to_string(bytes.size());
  json += ",\"data\":\"";
  json += encoded;
  json += "\"}";
  EmitJsonEvent(json);
}

// Dart calls this back from `takeAppSnapshot` with (ptr, width, height, data).
void OnAppSnapshotResult(intptr_t ptr, int32_t width, int32_t height,
                         Dart_Handle data) {
  std::vector<uint8_t> bytes;
  const bool ok = ReadDartBytes(data, &bytes);
  const char* format = "rgba";
  if (!bytes.empty() && bytes.size() >= 8 && bytes[0] == 0x89 &&
      bytes[1] == 'P' && bytes[2] == 'N' && bytes[3] == 'G') {
    format = "png";
  }
  std::fprintf(stderr,
               "[skyline-runtime] app snapshot: request=%ld %dx%d format=%s "
               "bytes=%zu ok=%d\n",
               static_cast<long>(ptr), width, height, format, bytes.size(),
               ok ? 1 : 0);
  if (!ok) return;
  WriteSnapshotFile("snapshot-app", static_cast<uint32_t>(ptr),
                    std::strcmp(format, "png") == 0 ? "png" : "rgba", bytes);
  EmitSnapshotJson("appSnapshot", static_cast<uint32_t>(ptr), width, height,
                   format, bytes);
}

// Dart calls this back from `YGNodeTakeSnapshot` with (ptr, data).
void OnNodeSnapshotResult(intptr_t ptr, Dart_Handle data) {
  std::vector<uint8_t> bytes;
  const bool ok = ReadDartBytes(data, &bytes);
  const char* format = "rgba";
  if (!bytes.empty() && bytes.size() >= 8 && bytes[0] == 0x89 &&
      bytes[1] == 'P' && bytes[2] == 'N' && bytes[3] == 'G') {
    format = "png";
  }
  std::fprintf(stderr,
               "[skyline-runtime] node snapshot: request=%ld format=%s "
               "bytes=%zu ok=%d\n",
               static_cast<long>(ptr), format, bytes.size(), ok ? 1 : 0);
  if (!ok) return;
  WriteSnapshotFile("snapshot-node", static_cast<uint32_t>(ptr),
                    std::strcmp(format, "png") == 0 ? "png" : "rgba", bytes);
  EmitSnapshotJson("nodeSnapshot", static_cast<uint32_t>(ptr), 0, 0, format,
                   bytes);
}

void DumpTree(bool elements) {
  FfiTable* table = ActiveFfiTable();
  DumpTreeFn dump = nullptr;
  if (elements) {
    dump = reinterpret_cast<DumpTreeFn>(
        FindCallback(table, kDumpElementTreeHash));
  } else {
    dump = reinterpret_cast<DumpTreeFn>(
        FindCallback(table, kDumpRenderTreeHash));
  }
  std::fprintf(stderr, "[skyline-runtime] %s requested (callback=%s)\n",
               elements ? "DumpElementTree" : "DumpRenderTree",
               dump != nullptr ? "yes" : "no");
  if (dump != nullptr) dump();
}

void CollectGarbage() {
  auto* collect = reinterpret_cast<CollectGarbageFn>(
      FindCallback(ActiveFfiTable(), kCollectGarbageHash));
  std::fprintf(stderr, "[skyline-runtime] CollectGarbage requested "
                       "(callback=%s)\n",
               collect != nullptr ? "yes" : "no");
  if (collect != nullptr) collect();
}

void SendMessageToRunner(const std::string& message) {
  auto* send = reinterpret_cast<SendMessageToRunnerFn>(
      FindCallback(ActiveFfiTable(), kSendMessageToRunnerHash));
  std::fprintf(stderr,
               "[skyline-runtime] SendMessageToRunner: \"%s\" (callback=%s)\n",
               message.c_str(), send != nullptr ? "yes" : "no");
  if (send != nullptr) {
    Dart_Handle text = Dart_NewStringFromCString(message.c_str());
    if (!Dart_IsError(text)) send(text);
  }
}

void TakeAppSnapshot(uint32_t request_id, double scale, bool compressed) {
  auto* take = reinterpret_cast<TakeAppSnapshotFn>(
      FindCallback(ActiveFfiTable(), kTakeAppSnapshotHash));
  std::fprintf(stderr,
               "[skyline-runtime] TakeAppSnapshot: request=%u scale=%g "
               "compressed=%d (callback=%s)\n",
               request_id, scale, compressed ? 1 : 0,
               take != nullptr ? "yes" : "no");
  if (take == nullptr) return;
  take(static_cast<intptr_t>(request_id), scale, compressed ? 1 : 0,
       &OnAppSnapshotResult);
}

void SnapshotNode(uint32_t node_id, uint8_t format, uint32_t request_id) {
  AppNode* node = FindNode(node_id);
  if (node == nullptr) {
    std::fprintf(stderr,
                 "[skyline-runtime] node snapshot: node #%u not found\n",
                 node_id);
    return;
  }
  // `<snapshot>` elements carry a SkylineSnapshot widget; SnapshotTakeSnapshot
  // paints it offscreen and calls back through SceneToImage.
  if (node->snapshot_p != nullptr) {
    auto* take = reinterpret_cast<SnapshotTakeSnapshotFn>(FindCallback(
        ActiveFfiTable(), kSnapshotTakeSnapshotHash));
    std::fprintf(stderr,
                 "[skyline-runtime] snapshot widget request: node=#%u "
                 "format=%u request=%u (callback=%s)\n",
                 node_id, static_cast<unsigned>(format), request_id,
                 take != nullptr ? "yes" : "no");
    if (take == nullptr) return;
    g_pending_node_snapshot_request.store(static_cast<int32_t>(request_id));
    const uint8_t ok = take(Dart_HandleFromPersistent(node->snapshot_p), format,
                            reinterpret_cast<intptr_t>(&OnNodeSnapshotResult));
    g_pending_node_snapshot_request.store(0);
    std::fprintf(stderr,
                 "[skyline-runtime] snapshot widget: node=#%u accepted=%u\n",
                 node_id, static_cast<unsigned>(ok));
    return;
  }
  if (node->yoga_node_p == nullptr) {
    std::fprintf(stderr,
                 "[skyline-runtime] node snapshot: node #%u not materialized\n",
                 node_id);
    return;
  }
  auto* take = reinterpret_cast<YGNodeTakeSnapshotFn>(
      FindCallback(ActiveFfiTable(), kYGNodeTakeSnapshotHash));
  std::fprintf(stderr,
               "[skyline-runtime] node snapshot request: node=#%u format=%u "
               "request=%u (callback=%s)\n",
               node_id, static_cast<unsigned>(format), request_id,
               take != nullptr ? "yes" : "no");
  if (take == nullptr) return;
  // YGNodeRenderObject.takeSnapshotAsync asserts that the render object is a
  // repaint boundary; DevTools asks for snapshots lazily, so enable it now.
  auto* set_boundary = reinterpret_cast<YGNodeSetIsRepaintBoundaryFn>(
      FindCallback(ActiveFfiTable(), kYGNodeSetIsRepaintBoundaryHash));
  Dart_Handle yoga = Dart_HandleFromPersistent(node->yoga_node_p);
  if (set_boundary != nullptr) set_boundary(yoga, 1);
  take(yoga, format, static_cast<intptr_t>(request_id),
       &OnNodeSnapshotResult);
}

void SetUserGesture(bool in_progress) {
  FfiTable* table = ActiveFfiTable();
  UserGestureFn callback = nullptr;
  if (in_progress) {
    callback = reinterpret_cast<UserGestureFn>(
        FindCallback(table, kStartUserGestureHash));
  } else {
    callback = reinterpret_cast<UserGestureFn>(
        FindCallback(table, kStopUserGestureHash));
  }
  std::fprintf(stderr,
               "[skyline-runtime] user gesture: %s (callback=%s)\n",
               in_progress ? "start" : "stop",
               callback != nullptr ? "yes" : "no");
  if (callback != nullptr) callback();
  char buffer[96];
  std::snprintf(buffer, sizeof(buffer),
                "{\"type\":\"userGesture\",\"inProgress\":%s}",
                in_progress ? "true" : "false");
  EmitJsonEvent(buffer);
}

void RelaunchApp(uint8_t open_type) {
  auto* relaunch = reinterpret_cast<AppCommandFn>(
      FindCallback(ActiveFfiTable(), kRelaunchAppHash));
  std::fprintf(stderr,
               "[skyline-runtime] RelaunchApp: openType=%u (callback=%s)\n",
               static_cast<unsigned>(open_type),
               relaunch != nullptr ? "yes" : "no");
  if (relaunch != nullptr) relaunch(open_type);
}

void ShutdownApplication() {
  auto* shutdown = reinterpret_cast<VoidCommandFn>(
      FindCallback(ActiveFfiTable(), kShutdownApplicationHash));
  std::fprintf(stderr, "[skyline-runtime] ShutdownApplication requested "
                       "(callback=%s)\n",
               shutdown != nullptr ? "yes" : "no");
  if (shutdown != nullptr) shutdown();
}

bool QueryAccessibilityBoldText() {
  auto* bold = reinterpret_cast<AccessibilityBoldTextEnabledFn>(FindCallback(
      ActiveFfiTable(), kAccessibilityBoldTextEnabledHash));
  if (bold == nullptr) return false;
  return bold() != 0;
}

void NotifyAccessibilitySink(int32_t kind, int32_t value) {
  SkylineAccessibilitySink sink = g_accessibility_sink.load();
  if (sink != nullptr) sink(kind, value);
}

bool IsAutomationOpKind(const char* kind) {
  return kind != nullptr &&
         (std::strcmp(kind, "accessibility") == 0 ||
          std::strcmp(kind, "dumpElementTree") == 0 ||
          std::strcmp(kind, "dumpRenderTree") == 0 ||
          std::strcmp(kind, "collectGarbage") == 0 ||
          std::strcmp(kind, "sendMessageToRunner") == 0 ||
          std::strcmp(kind, "takeAppSnapshot") == 0 ||
          std::strcmp(kind, "snapshotNode") == 0 ||
          std::strcmp(kind, "userGesture") == 0 ||
          std::strcmp(kind, "relaunchApp") == 0 ||
          std::strcmp(kind, "shutdownApplication") == 0);
}

// `attachCustomRoute` is emitted by the SkylineJS runtime right before the
// page's `attach` op.  Arguments (see assets/app.js `attachCustomRoute`):
//   isCustomRoute, opaque, maintainState, transitionDuration,
//   reverseTransitionDuration, barrierColor, barrierDismissible, barrierLabel,
//   canTransitionTo, canTransitionFrom, animationHash, secondaryAnimationHash.
// They are handed to Dart's `CreatePageRoute`, which is what makes the route
// expose `wx.router.getRouteContext` animations and gesture pop.
void ApplyAttachCustomRoute(const rapidjson::Value& op) {
  if (g_page == nullptr) return;
  const auto args = op.FindMember("args");
  if (args == op.MemberEnd() || !args->value.IsArray()) return;
  const rapidjson::Value& values = args->value;
  auto at = [&](rapidjson::SizeType index) -> const rapidjson::Value* {
    return index < values.Size() ? &values[index] : nullptr;
  };
  auto as_bool = [&](rapidjson::SizeType index, bool fallback) {
    const rapidjson::Value* value = at(index);
    if (value == nullptr) return fallback;
    if (value->IsBool()) return value->GetBool();
    if (value->IsNumber()) return value->GetDouble() != 0.0;
    if (value->IsString()) {
      const char* text = value->GetString();
      return std::strcmp(text, "true") == 0 || std::strcmp(text, "1") == 0;
    }
    return fallback;
  };
  auto as_int = [&](rapidjson::SizeType index, int fallback) {
    const rapidjson::Value* value = at(index);
    if (value == nullptr) return fallback;
    if (value->IsNumber()) return static_cast<int>(value->GetDouble());
    if (value->IsString()) return std::atoi(value->GetString());
    return fallback;
  };
  auto as_string = [&](rapidjson::SizeType index) -> std::string {
    const rapidjson::Value* value = at(index);
    if (value != nullptr && value->IsString()) return value->GetString();
    return std::string();
  };

  auto& config = g_page->custom_route;
  config.pending = true;
  config.is_custom = as_bool(0, false);
  config.opaque = as_bool(1, true);
  config.maintain_state = as_bool(2, true);
  config.transition_ms = as_int(3, 300);
  config.reverse_transition_ms = as_int(4, 300);
  const std::string barrier_color = as_string(5);
  config.has_barrier_color = !barrier_color.empty();
  // The route builder returns a CSS color string (`rgba(0, 0, 0, 0.3)` in
  // card_transition), not a hex literal.
  uint32_t parsed_barrier = 0u;
  config.barrier_color =
      config.has_barrier_color && ParseCssColor(barrier_color, &parsed_barrier)
          ? parsed_barrier
          : 0u;
  config.barrier_dismissible = as_bool(6, false);
  config.barrier_label = as_string(7);
  config.can_transition_to = as_bool(8, true);
  config.can_transition_from = as_bool(9, true);
  config.animation_hash = as_string(10);
  config.secondary_animation_hash = as_string(11);
  if (config::Flag(config::kAppServiceDebug)) {
    std::fprintf(stderr,
                 "[skyline-runtime] attachCustomRoute custom=%d opaque=%d "
                 "transition=%d/%d barrier=%08x anim=%s secondary=%s\n",
                 config.is_custom ? 1 : 0, config.opaque ? 1 : 0,
                 config.transition_ms, config.reverse_transition_ms,
                 config.has_barrier_color ? config.barrier_color : 0u,
                 config.animation_hash.c_str(),
                 config.secondary_animation_hash.c_str());
  }
}

void ApplyOperation(const rapidjson::Value& op) {
  if (g_page == nullptr || !op.IsObject()) return;
  const char* kind = MemberText(op, "op");
  if (kind[0] == '\0') return;
  g_page->applied_ops++;

  if (std::strcmp(kind, "focusInput") == 0) {
    auto op_int = [&](const char* key, int fallback) -> int {
      const auto member = op.FindMember(key);
      if (member == op.MemberEnd()) return fallback;
      if (member->value.IsInt()) return member->value.GetInt();
      if (member->value.IsUint()) {
        return static_cast<int>(member->value.GetUint());
      }
      if (member->value.IsDouble()) {
        return static_cast<int>(member->value.GetDouble());
      }
      if (member->value.IsBool()) return member->value.GetBool() ? 1 : 0;
      return fallback;
    };
    const uint32_t id = MemberUint(op, "id", 0);
    g_page->focused_input_id = id;
    AppNode* node = FindNode(id);
    if (node != nullptr) {
      node->input_hold_keyboard = op_int("holdKeyboard",
                                         node->input_hold_keyboard ? 1 : 0) != 0;
      const bool has_cursor = op.FindMember("cursor") != op.MemberEnd();
      const int cursor =
          op_int("cursor", has_cursor ? 0 : Utf8CodepointCount(node->text));
      const int start = op_int("selectionStart", cursor);
      const int end = op_int("selectionEnd", cursor);
      node->input_cursor = cursor;
      node->input_selection_start = start;
      node->input_selection_end = end;
      ClampInputState(node);
      NotifyInputState(node);
      SetInputFocus(id, true);
    }
    std::fprintf(stderr, "[skyline-runtime] focus input: id=%u\n",
                 g_page->focused_input_id);
  } else if (std::strcmp(kind, "blurInput") == 0) {
    const uint32_t id = MemberUint(op, "id", g_page->focused_input_id);
    if (id != 0) {
      SetInputFocus(id, false);
    } else if (g_page->focused_input_id != 0) {
      SetInputFocus(g_page->focused_input_id, false);
    }
    g_page->focused_input_id = 0;
    std::fprintf(stderr, "[skyline-runtime] blur input: id=%u\n", id);
  } else if (std::strcmp(kind, "setSelection") == 0) {
    AppNode* node = FindNode(MemberUint(op, "id", 0));
    if (node != nullptr) {
      const int cursor = static_cast<int>(MemberNumber(op, "cursor", -1.0));
      const int start = static_cast<int>(
          MemberNumber(op, "start",
                       cursor >= 0 ? cursor : node->input_selection_start));
      const int end = static_cast<int>(
          MemberNumber(op, "end", cursor >= 0 ? cursor : node->input_selection_end));
      node->input_cursor = cursor >= 0 ? cursor : start;
      node->input_selection_start = start;
      node->input_selection_end = end;
      ClampInputState(node);
      NotifyInputState(node);
      std::fprintf(stderr,
                   "[skyline-runtime] setSelection: id=%u selection=[%d,%d] "
                   "cursor=%d\n",
                   node->id, node->input_selection_start,
                   node->input_selection_end, node->input_cursor);
    }
  } else if (std::strcmp(kind, "compositionStart") == 0 ||
             std::strcmp(kind, "compositionUpdate") == 0 ||
             std::strcmp(kind, "compositionEnd") == 0) {
    AppNode* node = FindNode(MemberUint(op, "id", 0));
    if (node != nullptr) {
      const bool start = std::strcmp(kind, "compositionStart") == 0;
      const bool end = std::strcmp(kind, "compositionEnd") == 0;
      if (start) {
        node->composing = true;
        node->composition_base_text = node->text;
        int range_start = node->input_selection_start;
        int range_end = node->input_selection_end;
        if (range_start > range_end) std::swap(range_start, range_end);
        if (range_start == range_end) range_start = range_end = node->input_cursor;
        const auto start_member = op.FindMember("start");
        const auto end_member = op.FindMember("end");
        if (start_member != op.MemberEnd() && start_member->value.IsInt()) {
          range_start = start_member->value.GetInt();
        }
        if (end_member != op.MemberEnd() && end_member->value.IsInt()) {
          range_end = end_member->value.GetInt();
        }
        node->composition_start = range_start;
        node->composition_end = range_end;
      }
      if (start || node->composing) {
        const std::string value = MemberText(op, "text");
        ReplaceComposition(node, value);
        if (end) {
          node->composing = false;
          node->composition_base_text.clear();
          node->input_cursor = node->composition_start +
                               Utf8CodepointCount(node->composition_text);
          CollapseInputSelection(node);
        }
        NotifyInputState(node);
        std::fprintf(stderr,
                     "[skyline-runtime] %s: id=%u text=\"%s\" cursor=%d "
                     "selection=[%d,%d] value=\"%s\"\n",
                     start ? "composition start"
                           : (end ? "composition end" : "composition update"),
                     node->id, value.c_str(), node->input_cursor,
                     node->input_selection_start, node->input_selection_end,
                     node->text.c_str());
      }
    }
  } else if (std::strcmp(kind, "dispatchKey") == 0) {
    AppNode* input = FindNode(g_page->focused_input_id);
    const char* text = MemberText(op, "text");
    if (input != nullptr && text[0] != '\0') {
      InsertInputText(input, text);
      NotifyInputState(input);
      std::fprintf(stderr,
                   "[skyline-runtime] dispatchKey: id=%u text=%s cursor=%d\n",
                   input->id, input->text.c_str(), input->input_cursor);
    }
  } else if (std::strcmp(kind, "keyboardStatus") == 0) {
    g_page->keyboard_height = MemberNumber(op, "height", 0.0);
    const int32_t input_key = static_cast<int32_t>(MemberUint(op, "inputKey", 0));
    void* callback = g_page->holder != nullptr
                         ? FindCallback(g_page->holder->ffi_table,
                                        kUpdateKeyboardStatusHash)
                         : nullptr;
    if (callback != nullptr) {
      reinterpret_cast<UpdateKeyboardStatusFn>(callback)(
          input_key, g_page->keyboard_height);
      std::fprintf(stderr,
                   "[skyline-runtime] UpdateKeyboardStatus: key=%d height=%g\n",
                   input_key, g_page->keyboard_height);
    }
  } else if (std::strcmp(kind, "setScrollTop") == 0) {
    AppNode* node = FindNode(MemberUint(op, "id", 0));
    void* callback = g_page->holder != nullptr
                         ? FindCallback(g_page->holder->ffi_table,
                                        kScrollViewUpdateScrollTopHash)
                         : nullptr;
    if (node != nullptr && callback != nullptr &&
        node->scroll_view_p != nullptr) {
      const double value = MemberNumber(op, "value", 0.0);
      char buffer[64];
      std::snprintf(buffer, sizeof(buffer), "%g", value);
      node->attrs["scrollTop"] = buffer;
      reinterpret_cast<ScrollViewUpdateScrollTopFn>(callback)(
          Dart_HandleFromPersistent(node->scroll_view_p), value);
      std::fprintf(stderr,
                   "[skyline-runtime] appservice setScrollTop: id=%u value=%g\n",
                   node->id, value);
    }
  } else if (std::strcmp(kind, "swiperTo") == 0) {
    bool animated = true;
    const auto animated_member = op.FindMember("animated");
    if (animated_member != op.MemberEnd()) {
      if (animated_member->value.IsBool()) {
        animated = animated_member->value.GetBool();
      } else if (animated_member->value.IsNumber()) {
        animated = animated_member->value.GetDouble() != 0.0;
      }
    }
    AnimateSwiperTo(MemberUint(op, "id", 0),
                    static_cast<uint32_t>(MemberNumber(op, "page", 0.0)),
                    animated);
  } else if (std::strcmp(kind, "pickerTo") == 0) {
    SetPickerColumnCurrent(
        MemberUint(op, "id", 0),
        static_cast<uint32_t>(MemberNumber(op, "value", 0.0)));
  } else if (std::strcmp(kind, "refresherTrigger") == 0) {
    const bool two_level = MemberUint(op, "twoLevel", 0) != 0;
    SetRefresherTriggered(MemberUint(op, "id", 0),
                          MemberUint(op, "open", 1) != 0, two_level);
  } else if (std::strcmp(kind, "addIntersectionObserver") == 0) {
    std::vector<double> thresholds;
    const auto list = op.FindMember("thresholds");
    if (list != op.MemberEnd() && list->value.IsArray()) {
      for (const rapidjson::Value& entry : list->value.GetArray()) {
        if (entry.IsNumber()) thresholds.push_back(entry.GetDouble());
      }
    }
    if (thresholds.empty()) thresholds.push_back(0.0);
    RegisterIntersectionObserver(
        MemberUint(op, "id", 0), MemberUint(op, "uniqueId", 0),
        MemberUint(op, "rootId", 0), MemberNumber(op, "marginTop", 0.0),
        MemberNumber(op, "marginRight", 0.0),
        MemberNumber(op, "marginBottom", 0.0),
        MemberNumber(op, "marginLeft", 0.0), thresholds);
  } else if (std::strcmp(kind, "intersect") == 0) {
    const uint32_t id = MemberUint(op, "id", 0);
    const bool has_unique = op.FindMember("uniqueId") != op.MemberEnd();
    const int32_t unique =
        has_unique ? static_cast<int32_t>(MemberUint(op, "uniqueId", 0)) : -1;
    const double ratio = MemberNumber(op, "ratio", 1.0);
    const bool intersecting = MemberUint(op, "intersecting", 1) != 0;
    TriggerIntersectionForNode(
        id, unique, 0.0, 100.0, 100.0, 0.0, 0.0, 100.0, 100.0, 0.0, 0.0,
        320.0, 240.0, 0.0, ratio, intersecting);
  } else if (std::strcmp(kind, "selectionRange") == 0) {
    SetSelectionRange(
        MemberUint(op, "selectionId", 0), MemberUint(op, "startId", 0),
        static_cast<int32_t>(MemberNumber(op, "startOffset", 0.0)),
        MemberUint(op, "endId", 0),
        static_cast<int32_t>(MemberNumber(op, "endOffset", 0.0)));
  } else if (std::strcmp(kind, "navigateBackIntercept") == 0) {
    SetPageNavigateBackInterception(
        static_cast<int32_t>(MemberNumber(op, "pageId", 1.0)),
        MemberUint(op, "enabled", 1) != 0);
  } else if (std::strcmp(kind, "triggerNavigateBackIntercept") == 0) {
    TriggerNavigateBackIntercept(
        static_cast<int32_t>(MemberNumber(op, "pageId", 1.0)));
  } else if (std::strcmp(kind, "dispatchTouch") == 0) {
    void* callback = g_page->holder != nullptr
                         ? FindCallback(g_page->holder->ffi_table,
                                        kDispatchTouchEventHash)
                         : nullptr;
    if (callback != nullptr) {
      const double x = MemberNumber(op, "x", 0.0);
      const double y = MemberNumber(op, "y", 0.0);
      const uint8_t type =
          static_cast<uint8_t>(MemberUint(op, "type", 0));
      reinterpret_cast<DispatchTouchEventFn>(callback)(type, x, y);
      if (config::Flag(config::kAppServiceDebug)) {
        using GetNodeFromPointFn = intptr_t (*)(double, double, uint8_t);
        auto* hit = reinterpret_cast<GetNodeFromPointFn>(
            FindCallback(g_page->holder->ffi_table, kGetNodeFromPointHash));
        const intptr_t address = hit != nullptr
                                     ? hit(x, y, 1)
                                     : static_cast<intptr_t>(0);
        std::fprintf(stderr,
                     "[skyline-runtime] appservice dispatchTouch: type=%u (%g,%g) "
                     "hit=0x%lx\n",
                     static_cast<unsigned>(type), x, y,
                     static_cast<long>(address));
      } else {
        std::fprintf(stderr,
                     "[skyline-runtime] appservice dispatchTouch: type=%u (%g,%g)\n",
                     static_cast<unsigned>(type), x, y);
      }
    }
  } else if (std::strcmp(kind, "createElement") == 0) {
    NewNode(MemberUint(op, "id", 0), MemberText(op, "tag"),
            MemberText(op, "name"));
  } else if (std::strcmp(kind, "notifyNavigateTo") == 0 ||
             std::strcmp(kind, "notifySwitchTab") == 0 ||
             std::strcmp(kind, "notifyNavigateBack") == 0) {
    // Route requests from the renderer.  The recorder stores the native call
    // arguments as an array (`[windowId, pageId, ...]`).
    const auto args = op.FindMember("args");
    auto arg_number = [&](size_t index, double fallback) -> double {
      if (args == op.MemberEnd() || !args->value.IsArray() ||
          index >= args->value.Size()) {
        return fallback;
      }
      const rapidjson::Value& value = args->value[static_cast<rapidjson::SizeType>(
          index)];
      if (value.IsNumber()) return value.GetDouble();
      return fallback;
    };
    const bool is_back = std::strcmp(kind, "notifyNavigateBack") == 0;
    const bool is_switch = std::strcmp(kind, "notifySwitchTab") == 0;
    g_page->pending_open_type = is_back ? 6 : (is_switch ? 1 : 4);
    g_page->pending_page_id =
        static_cast<int32_t>(arg_number(1, 0.0));
    if (is_back) {
      g_page->pending_back_delta =
          static_cast<int>(arg_number(2, 1.0));
      g_page->pending_back_animated = arg_number(3, 1.0) != 0.0;
      // Restore the covered page's root/connector so the pending rebuild
      // targets the route revealed by the pop instead of the destroyed one.
      int delta = g_page->pending_back_delta;
      if (delta < 1) delta = 1;
      while (delta-- > 0 && !g_page->page_stack.empty()) {
        DeletePersistent(&g_page->root_node);
        DeletePersistent(&g_page->root_connector);
        OpPage::PageEntry entry = g_page->page_stack.back();
        g_page->page_stack.pop_back();
        g_page->root_id = entry.root_id;
        g_page->root_node = entry.root_node;
        g_page->root_connector = entry.root_connector;
        if (g_page->page_count > 0) g_page->page_count--;
      }
      if (g_page->root_id != 0 && g_page->root_connector != nullptr) {
        g_page->force_rebuild = true;
      }
      std::fprintf(stderr,
                   "[skyline-runtime] navigateBack restore root=%u stack=%zu "
                   "root_node=%p connector=%p\n",
                   g_page->root_id, g_page->page_stack.size(),
                   static_cast<void*>(g_page->root_node),
                   static_cast<void*>(g_page->root_connector));
    }
  } else if (std::strcmp(kind, "createContext") == 0) {
    // Every appservice flush starts with this op so the native replay knows
    // which renderer context the batch belongs to.  The SDK also passes the
    // rendererOptions-derived feature flags (defaultContentBox, ...).
    if (g_page != nullptr) {
      g_page->active_context_id = MemberUint64(op, "id", 0);
      const auto flags = op.FindMember("flags");
      if (flags != op.MemberEnd() && flags->value.IsObject()) {
        const auto flag = [&](const char* key) {
          const auto member = flags->value.FindMember(key);
          return member != flags->value.MemberEnd() &&
                 member->value.IsBool() && member->value.GetBool();
        };
        if (flags->value.HasMember("defaultContentBox")) {
          g_page->default_content_box = flag("defaultContentBox");
        }
      }
    }
  } else if (std::strcmp(kind, "attachAppBar") == 0) {
    // Skyline app-bar contexts install their tree as a window overlay instead
    // of a page route (Dart `CreateAppBar`).  Keep the root and remember the
    // context so later app-bar updates rebuild the overlay, not the page.
    if (g_page != nullptr) {
      g_page->appbar_root_id = MemberUint(op, "root", 0);
      g_page->appbar_context_id = g_page->active_context_id;
    }
  } else if (std::strcmp(kind, "setAsTab") == 0 ||
             std::strcmp(kind, "registerEventCallback") == 0 ||
             std::strcmp(kind, "registerAnimationCallback") == 0 ||
             std::strcmp(kind, "startMapper") == 0 ||
             std::strcmp(kind, "stopMapper") == 0) {
    // Native bindings with no page-tree effect in this milestone.
  } else if (std::strcmp(kind, "appendStyleSheet") == 0) {
    const auto member = op.FindMember("css");
    if (member != op.MemberEnd() && member->value.IsString()) {
      // `index` is the component style scope the sheet was compiled for
      // (glass-easel `compiledStyleSheet scope=N id=...`).
      ParseStyleSheet(member->value.GetString(),
                      static_cast<int>(MemberUint(op, "index", 0)),
                      g_page != nullptr ? g_page->active_context_id : 0);
    }
  } else if (std::strcmp(kind, "createElementDone") == 0) {
    // Reserved.
  } else if (std::strcmp(kind, "setText") == 0) {
    AppNode* node = FindNode(MemberUint(op, "id", 0));
    if (node != nullptr) node->text = MemberText(op, "text");
  } else if (std::strcmp(kind, "setAttribute") == 0 ||
             std::strcmp(kind, "setAttributeValue") == 0) {
    AppNode* node = FindNode(MemberUint(op, "id", 0));
    const auto value = op.FindMember("value");
    if (node != nullptr && value != op.MemberEnd()) {
      const char* attr = MemberText(op, "name");
      ApplyNodeAttribute(node, attr, value->value);
      if (IsImageName(node->name) &&
          (std::strcmp(attr, "src") == 0 || std::strcmp(attr, "url") == 0 ||
           std::strcmp(attr, "imageUrl") == 0 ||
           std::strcmp(attr, "image") == 0 ||
           std::strcmp(attr, "path") == 0)) {
        g_page->force_rebuild = true;
      }
    }
  } else if (std::strcmp(kind, "setAttributes") == 0) {
    AppNode* node = FindNode(MemberUint(op, "id", 0));
    const auto attrs = op.FindMember("attrs");
    if (node != nullptr && attrs != op.MemberEnd() &&
        attrs->value.IsObject()) {
      for (auto it = attrs->value.MemberBegin(); it != attrs->value.MemberEnd();
           ++it) {
        const std::string attr = it->name.GetString();
        ApplyNodeAttribute(node, attr.c_str(), it->value);
        if (IsImageName(node->name) &&
            (attr == "src" || attr == "url" || attr == "imageUrl" ||
             attr == "image" || attr == "path")) {
          g_page->force_rebuild = true;
        }
      }
    }
  } else if (std::strcmp(kind, "removeAttribute") == 0) {
    AppNode* node = FindNode(MemberUint(op, "id", 0));
    if (node != nullptr) {
      const std::string attr = MemberText(op, "name");
      node->attrs.erase(attr);
      if (IsSemanticsAttribute(attr.c_str())) UpdateNodeSemantics(node);
    }
  } else if (std::strcmp(kind, "setStyle") == 0) {
    AppNode* node = FindNode(MemberUint(op, "id", 0));
    if (node != nullptr) ApplyAttribute(node, "style", MemberText(op, "value"));
  } else if (std::strcmp(kind, "addClass") == 0) {
    AppNode* node = FindNode(MemberUint(op, "id", 0));
    if (node != nullptr) {
      const std::string klass = MemberText(op, "prefix");
      const int style_scope = static_cast<int>(MemberUint(op, "name", 0));
      bool present = false;
      for (size_t i = 0; i < node->classes.size(); i++) {
        if (node->classes[i] != klass) continue;
        const int existing =
            i < node->class_scopes.size() ? node->class_scopes[i] : 0;
        if (existing == style_scope) {
          present = true;
          break;
        }
      }
      if (!klass.empty() && !present) {
        node->classes.push_back(klass);
        node->class_scopes.push_back(style_scope);
      }
    }
  } else if (std::strcmp(kind, "removeClass") == 0) {
    AppNode* node = FindNode(MemberUint(op, "id", 0));
    if (node != nullptr) {
      const std::string klass = MemberText(op, "prefix");
      const int style_scope = static_cast<int>(MemberUint(op, "name", 0));
      for (size_t i = 0; i < node->classes.size();) {
        const int existing =
            i < node->class_scopes.size() ? node->class_scopes[i] : 0;
        if (node->classes[i] == klass &&
            (style_scope == 0 || existing == style_scope)) {
          node->classes.erase(node->classes.begin() +
                              static_cast<std::ptrdiff_t>(i));
          if (i < node->class_scopes.size()) {
            node->class_scopes.erase(node->class_scopes.begin() +
                                     static_cast<std::ptrdiff_t>(i));
          }
          continue;
        }
        i++;
      }
    }
  } else if (std::strcmp(kind, "setStyleScope") == 0) {
    AppNode* node = FindNode(MemberUint(op, "id", 0));
    if (node != nullptr) {
      node->style_scope = std::atoi(MemberText(op, "scope", "0"));
    }
  } else if (std::strcmp(kind, "setClass") == 0) {
    AppNode* node = FindNode(MemberUint(op, "id", 0));
    if (node != nullptr) {
      SetClasses(node, SplitClasses(MemberText(op, "names")));
    }
  } else if (std::strcmp(kind, "setId") == 0) {
    AppNode* node = FindNode(MemberUint(op, "id", 0));
    if (node != nullptr) node->attrs["id"] = MemberText(op, "value");
  } else if (std::strcmp(kind, "appendChild") == 0) {
    AppNode* parent = FindNode(MemberUint(op, "id", 0));
    AppNode* child = FindNode(MemberUint(op, "child", 0));
    if (parent != nullptr && child != nullptr) {
      if (child->tag == "fragment") {
        std::vector<AppNode*> nested = child->children;
        child->children.clear();
        for (AppNode* item : nested) AppendChild(parent, item);
      } else {
        AppendChild(parent, child);
      }
    }
  } else if (std::strcmp(kind, "insertBefore") == 0) {
    AppNode* parent = FindNode(MemberUint(op, "id", 0));
    AppNode* child = FindNode(MemberUint(op, "child", 0));
    AppNode* before = FindNode(MemberUint(op, "before", 0));
    if (parent != nullptr && child != nullptr) {
      InsertBefore(parent, child, before);
    }
  } else if (std::strcmp(kind, "removeChild") == 0) {
    AppNode* parent = FindNode(MemberUint(op, "id", 0));
    AppNode* child = FindNode(MemberUint(op, "child", 0));
    RemoveChild(parent, child);
  } else if (std::strcmp(kind, "replaceChild") == 0) {
    AppNode* parent = FindNode(MemberUint(op, "id", 0));
    AppNode* child = FindNode(MemberUint(op, "child", 0));
    const auto index_member = op.FindMember("index");
    if (parent != nullptr && child != nullptr) {
      DetachFromParent(child);
      const int index = index_member != op.MemberEnd() &&
                                index_member->value.IsInt()
                            ? index_member->value.GetInt()
                            : static_cast<int>(parent->children.size());
      if (index >= 0 && index < static_cast<int>(parent->children.size())) {
        parent->children[static_cast<size_t>(index)]->parent = nullptr;
        parent->children[static_cast<size_t>(index)] = child;
      } else {
        parent->children.push_back(child);
      }
      child->parent = parent;
    }
  } else if (std::strcmp(kind, "splice") == 0) {
    AppNode* parent = FindNode(MemberUint(op, "id", 0));
    const auto items_member = op.FindMember("items");
    std::vector<AppNode*> items;
    if (items_member != op.MemberEnd() && items_member->value.IsArray()) {
      for (const rapidjson::Value& item : items_member->value.GetArray()) {
        if (item.IsUint()) items.push_back(FindNode(item.GetUint()));
      }
    }
    const auto pos_member = op.FindMember("pos");
    const auto count_member = op.FindMember("count");
    const int pos = pos_member != op.MemberEnd() && pos_member->value.IsInt()
                        ? pos_member->value.GetInt()
                        : -1;
    const int count =
        count_member != op.MemberEnd() && count_member->value.IsInt()
            ? count_member->value.GetInt()
            : 0;
    Splice(parent, pos, count, items);
  } else if (std::strcmp(kind, "release") == 0) {
    AppNode* node = FindNode(MemberUint(op, "id", 0));
    if (node != nullptr) {
      CancelPendingImage(node);
      DetachFromParent(node);
      node->released = true;
    }
  } else if (std::strcmp(kind, "addAnimatedStyle") == 0) {
    CollectAnimatedStyles(op, FindNode(MemberUint(op, "id", 0)));
  } else if (std::strcmp(kind, "attachCustomRoute") == 0) {
    ApplyAttachCustomRoute(op);
  } else if (std::strcmp(kind, "attach") == 0) {
    // Remember the previously attached page before the new root replaces it;
    // MaterializeAttachedPage pushes it onto the page stack so navigateBack can
    // restore the covered page's connector.
    g_page->attach_previous_root_id = g_page->root_id;
    g_page->root_id = MemberUint(op, "root", 0);
    g_page->attach_host_id = MemberUint(op, "host", 0);
  } else if (std::strcmp(kind, "startRender") == 0) {
    g_page->started = true;
  } else if (std::strcmp(kind, "accessibility") == 0) {
    const bool enabled = MemberNumber(op, "enabled", 0.0f) != 0.0f;
    g_accessibility_enabled.store(enabled ? 1 : 0);
    std::fprintf(stderr,
                 "[skyline-runtime] accessibility: enabled=%d (automation)\n",
                 enabled ? 1 : 0);
    // Enable the semantics tree before the accessibility feature flag: the
    // page's root `ExcludeSemantics` wrapper rebuilds when MediaQuery reports
    // accessibleNavigation, and it must observe semanticsEnabled == true then.
    NotifyAccessibilitySink(SKYLINE_ACCESSIBILITY_SEMANTICS, enabled ? 1 : 0);
    NotifyAccessibilitySink(SKYLINE_ACCESSIBILITY_FEATURES, enabled ? 1 : 0);
  } else if (std::strcmp(kind, "dumpElementTree") == 0) {
    DumpTree(true);
  } else if (std::strcmp(kind, "dumpRenderTree") == 0) {
    DumpTree(false);
  } else if (std::strcmp(kind, "collectGarbage") == 0) {
    CollectGarbage();
  } else if (std::strcmp(kind, "sendMessageToRunner") == 0) {
    SendMessageToRunner(MemberText(op, "message", ""));
  } else if (std::strcmp(kind, "takeAppSnapshot") == 0) {
    TakeAppSnapshot(MemberUint(op, "requestId", 0),
                    static_cast<double>(MemberNumber(op, "scale", 1.0f)),
                    MemberNumber(op, "compressed", 1.0f) != 0.0f);
  } else if (std::strcmp(kind, "snapshotNode") == 0) {
    SnapshotNode(MemberUint(op, "id", 0),
                 static_cast<uint8_t>(MemberNumber(op, "format", 4.0f)),
                 MemberUint(op, "requestId", 0));
  } else if (std::strcmp(kind, "userGesture") == 0) {
    SetUserGesture(MemberNumber(op, "active", 0.0f) != 0.0f);
  } else if (std::strcmp(kind, "relaunchApp") == 0) {
    RelaunchApp(static_cast<uint8_t>(MemberNumber(op, "openType", 0.0f)));
  } else if (std::strcmp(kind, "shutdownApplication") == 0) {
    ShutdownApplication();
  } else {
    // setStyleScope / associateComponent / setListenerOption /
    // setHitTestBehavior / forceDetached / ...
  }
}


}  // namespace appservice

