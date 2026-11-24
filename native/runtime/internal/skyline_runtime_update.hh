// Style diffing, targeted updates, page rebuilds and transitions.

#pragma once
#include "skyline_runtime_appservice.hh"

namespace appservice {
// Suspends page-wide rebuilds while a swiper drag/animation is active.
void SetSwiperGestureActive(bool active);
bool IsStructuralOpKind(const char* kind);
void ClearTransitions();
void DebugDumpNode(const AppNode* node, int depth);
void FlushAppServicePage(RuntimeHolder* holder, bool structural);
void RebuildAppServicePage(RuntimeHolder* holder);
void SchedulePendingAnimatedStyles(RuntimeHolder* holder);
void SnapshotNode(AppNode* node);
void StartTransitions(uint32_t root_id);
}  // namespace appservice
