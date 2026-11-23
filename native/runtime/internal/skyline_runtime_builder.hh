// List/grid builder incremental materialization and sync.

#pragma once
#include "skyline_runtime_appservice.hh"

namespace appservice {
bool SyncAppServiceBuilders(RuntimeHolder* holder);
void OnBuilderBuildChild(intptr_t address, int32_t index);
void OnBuilderDisposeChild(intptr_t address, int32_t index);
}  // namespace appservice
