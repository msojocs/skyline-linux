#include "skyline/config.h"

#include <cstdlib>
#include <cstring>

namespace skyline::config {
namespace {

enum class Type { kFlag, kString, kPath, kInt, kEnum };

struct Entry {
  const char* name;
  Type type;
  const char* fallback;
};

constexpr Entry kEntries[] = {
#define SKYLINE_CONFIG_VAR(id, name, type, fallback) {name, Type::type, fallback},
#include "skyline/config_vars.inc"
#undef SKYLINE_CONFIG_VAR
};
constexpr size_t kCount = sizeof(kEntries) / sizeof(kEntries[0]);

struct Value {
  bool set = false;
  // The environment value, or the registered default when unset.
  std::string text;
};

// The snapshot taken by the first lookup.  Never freed: lookups may happen
// during static destruction on engine threads.
const Value* Values() {
  static const Value* values = [] {
    auto* result = new Value[kCount];
    for (size_t i = 0; i < kCount; ++i) {
      if (const char* text = std::getenv(kEntries[i].name)) {
        result[i].set = true;
        result[i].text = text;
      } else {
        result[i].text = kEntries[i].fallback;
      }
    }
    return result;
  }();
  return values;
}

const Value& Lookup(Var var) { return Values()[static_cast<size_t>(var)]; }

}  // namespace

const char* Name(Var var) { return kEntries[static_cast<size_t>(var)].name; }

bool IsSet(Var var) { return Lookup(var).set; }

const std::string& String(Var var) { return Lookup(var).text; }

bool Flag(Var var) {
  const Value& value = Lookup(var);
  if (!value.set) return false;
  for (const char* off : {"", "0", "false", "off", "no"}) {
    if (value.text == off) return false;
  }
  return true;
}

long Int(Var var) {
  const char* text = Lookup(var).text.c_str();
  char* end = nullptr;
  const long parsed = std::strtol(text, &end, 10);
  if (end != text && *end == '\0') return parsed;
  return std::strtol(kEntries[static_cast<size_t>(var)].fallback, nullptr, 10);
}

}  // namespace skyline::config
