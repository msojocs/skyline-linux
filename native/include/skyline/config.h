#pragma once

// Environment configuration of the native components (runtime, skyline.node,
// skyline_host, sharedMemory.node).
//
// Every variable is registered in tools/env-vars.json (documented in
// docs/env.md); `npm run env:doc` generates the table below from it.  A
// process reads all registered variables once, on first use, so it keeps the
// environment it was launched with.

#include <string>

namespace skyline::config {

enum Var : int {
#define SKYLINE_CONFIG_VAR(id, name, type, fallback) id,
#include "skyline/config_vars.inc"
#undef SKYLINE_CONFIG_VAR
};

// "SKYLINE_...".
const char* Name(Var var);
// The variable is present in the environment (possibly empty).
bool IsSet(Var var);
// The value, or the registered default when the variable is unset.
const std::string& String(Var var);
// A flag is on unless it is unset, empty, "0", "false", "off" or "no".
bool Flag(Var var);
// The value as an integer; the registered default when unset or malformed.
long Int(Var var);

}  // namespace skyline::config
