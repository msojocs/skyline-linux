// Page route materialization and live operation batches.

#pragma once
#include "skyline_runtime_appservice.hh"

namespace appservice {
void ApplyLiveOpsBatch(RuntimeHolder* holder, const char* descriptor);
void ApplyOpsPage(RuntimeHolder* holder, const char* descriptor);
void HandleKeyInput(RuntimeHolder* holder, int32_t kind, uint32_t logical, const std::string& text);
void ScrollNodeTo(uint32_t node_id, double top);
}  // namespace appservice
