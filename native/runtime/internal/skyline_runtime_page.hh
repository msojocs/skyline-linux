// Page route materialization and live operation batches.

#pragma once
#include "skyline_runtime_appservice.hh"

namespace appservice {
void ApplyLiveOpsBatch(RuntimeHolder* holder, const char* descriptor);
void ApplyOpsPage(RuntimeHolder* holder, const char* descriptor);
void HandleKeyInput(RuntimeHolder* holder, int32_t kind, uint32_t logical, const std::string& text);
void ScrollNodeTo(uint32_t node_id, double top);
void ScrollIntoView(uint32_t scroll_node_id, uint32_t target_node_id,
                    bool within_extent, bool animated, double offset,
                    uint8_t alignment);
}  // namespace appservice
