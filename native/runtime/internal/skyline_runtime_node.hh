// Node tree operations, attributes, text metrics and node predicates.

#pragma once
#include "skyline_runtime_appservice.hh"

namespace appservice {
AppNode* FindNode(uint32_t id);
AppNode* NewNode(uint32_t id, const std::string& tag, const std::string& name);
bool AttachDetachedPortalContent(AppNode* root);
bool AttrBool(const AppNode* node, const char* key, bool fallback);
bool FlattenFragments(AppNode* node);
bool GridIsMasonry(const AppNode* node);
bool HasExplicitHeight(const AppStyle& style);
bool HasExplicitWidth(const AppStyle& style);
bool InsideSelectionContext(const AppNode* node);
bool InsideSliverContext(const AppNode* node);
bool IsBuilderName(const std::string& name);
bool IsBuilderSubtree(const AppNode* node);
bool IsGridBuilderName(const std::string& name);
bool IsGridViewName(const std::string& name);
bool IsImageName(const std::string& name);
bool IsInlineFlowContainer(AppNode* node);
bool IsListBuilderName(const std::string& name);
bool IsListViewName(const std::string& name);
bool IsPickerViewColumnNode(const AppNode* node);
bool IsScrollViewComponentWrapper(const AppNode* node);
bool IsScrollViewName(const std::string& name);
bool IsSelectionNode(const AppNode* node);
bool IsShareElementNode(const AppNode* node);
bool IsSwiperNode(const AppNode* node);
bool ResolveInset(const AppStyle& style, int index, float content_extent, float* out);
double AttrNumber(const AppNode* node, const char* key, double fallback);
double GridCrossGap(const AppNode* node);
double GridMainGap(const AppNode* node);
double TextIntrinsicWidth(const std::string& text, const AppStyle& style);
double TextLineHeight(const AppStyle& style);
float MainGap(const AppStyle& style);
int CountTextLines(const std::string& text, double max_width, const AppStyle& style);
int EffectiveAlign(const AppStyle& parent, const AppStyle& child);
std::string AttrText(const AppNode* node, const char* key, const char* fallback);
std::string NodeNormalizedText(const AppNode* node);
std::vector<AppNode*> OrderedFlowChildren(AppNode* node);
std::vector<std::string> SplitClasses(const std::string& names);
uint32_t NextCodepoint(const std::string& text, size_t* index);
void AppendChild(AppNode* parent, AppNode* child);
void ApplyAttribute(AppNode* node, const std::string& name, const std::string& value);
void CollectListItems(AppNode* parent, std::vector<AppNode*>* out);
void CollectSubtreeNodes(AppNode* node, std::unordered_set<AppNode*>* out);
void ComputeInlineWhitespace(AppNode* node);
void DeletePersistent(Dart_PersistentHandle* handle);
void DetachFromParent(AppNode* node);
void InsertBefore(AppNode* parent, AppNode* child, AppNode* before);
void PrepareLayoutBuffers(AppNode* node);
void RemoveChild(AppNode* parent, AppNode* child);
void ResetNodeHandles(AppNode* node, bool include_layout);
void ResetNodeHandlesSkipping(AppNode* node, bool include_layout, const std::unordered_set<AppNode*>& covered);
void SetClasses(AppNode* node, const std::vector<std::string>& classes);
void Splice(AppNode* parent, int position, int count, const std::vector<AppNode*>& items);
}  // namespace appservice
