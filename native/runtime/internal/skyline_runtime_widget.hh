// Flutter widget materialization (BuildWidget and wrappers).

#pragma once
#include "skyline_runtime_appservice.hh"

namespace appservice {
Dart_Handle BuildBoxShadowList(const AppStyle& style);
Dart_Handle BuildGradientHandle(const AppStyle& style);
Dart_Handle BuildWidget(AppNode* node, NewYGNodeFn newYGNode, NewYGConnectorFn newYGConnector, NewTextFn newText, NewTextSpanFn newTextSpan, NewDecoratedBoxFn newDecoratedBox, NewOpacityFn newOpacity, NewScrollViewFn newScrollView, NewListViewFn newListView, NewGridViewFn newGridView, NewDirectRenderWidgetFn newDirectRenderWidget, NewListBuilderFn newListBuilder, NewGridBuilderFn newGridBuilder, bool is_root);
bool IsSemanticsAttribute(const char* name);
void UpdateNodeSemantics(AppNode* node);
}  // namespace appservice
