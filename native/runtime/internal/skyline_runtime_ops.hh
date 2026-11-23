// Appservice operation stream replay and DevTools automation.

#pragma once
#include "skyline_runtime_appservice.hh"

namespace appservice {
bool IsAutomationOpKind(const char* kind);
bool QueryAccessibilityBoldText();
const char* MemberText(const rapidjson::Value& object, const char* key, const char* fallback = "");
int Utf8CodepointCount(const std::string& text);
uint32_t MemberUint(const rapidjson::Value& object, const char* key, uint32_t fallback);
void ApplyOperation(const rapidjson::Value& op);
void ClampInputState(AppNode* node);
void CollapseInputSelection(AppNode* node);
void CollectGarbage();
void DumpTree(bool elements);
void NotifyAccessibilitySink(int32_t kind, int32_t value);
void NotifyInputState(AppNode* node);
void RelaunchApp(uint8_t open_type);
void SendMessageToRunner(const std::string& message);
void SetUserGesture(bool in_progress);
void ShutdownApplication();
void SnapshotNode(uint32_t node_id, uint8_t format, uint32_t request_id);
void TakeAppSnapshot(uint32_t request_id, double scale, bool compressed);
void Utf8Erase(std::string* text, int start, int end);
void Utf8Insert(std::string* text, int position, const std::string& value);
}  // namespace appservice
