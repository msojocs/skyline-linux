// CSS selectors, stylesheet parsing and style resolution.

#pragma once
#include "skyline_runtime_appservice.hh"

namespace appservice {
bool NodeHasClass(const AppNode* node, const std::string& name);
void ParseStyleSheet(const std::string& css, int style_scope = 0,
                     uint64_t context_id = 0);
void ResolveStyles(AppNode* node, const AppStyle* parent);
}  // namespace appservice
