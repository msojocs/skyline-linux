// Flexbox/grid measurement, layout and sliver metric handling.

#pragma once
#include "skyline_runtime_appservice.hh"

namespace appservice {
SizeF MeasureNode(AppNode* node, float avail_w, bool stretch_w, float avail_h, bool stretch_h, float containing_w = -1.0f);
float ContentBoxWidth(const AppNode* node);
float GridSlotWidth(const AppNode* node, int columns);
int GridColumnCount(const AppNode* node, float cross_extent);
void ClearSliverLayoutMarks();
void LayoutNode(AppNode* node, float x, float y, float w, float h, std::vector<NodeMetric>* metrics, float abs_x = 0.0f, float abs_y = 0.0f, float cb_abs_x = -1.0f, float cb_abs_y = -1.0f, float cb_w = -1.0f, float cb_h = -1.0f);
void ResetNodeLayoutToOrigin(AppNode* node);
void ApplySliverMargins(AppNode* node);
void RestoreSliverLayoutMetrics();
void WriteLayoutMetrics(const std::vector<NodeMetric>& metrics);
}  // namespace appservice
