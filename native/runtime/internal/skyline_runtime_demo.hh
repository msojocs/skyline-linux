// Synthetic Skyline demo pages (stripes/text/card/JSON descriptor).

#pragma once
#include "skyline_runtime_appservice.hh"

Dart_Handle CreateTextWidget(NewTextFn newText, NewTextSpanFn newTextSpan, intptr_t address, const char* value, double fontSize, uint32_t color, int fontWeight, int fontStyle, double letterSpacing, double lineHeight, int textAlign, int maxLines, int overflow, bool softWrap, bool userSelect, const std::vector<std::string>& fontFamilies, Dart_Handle* out_span = nullptr);
bool ReadDescriptor(const char* descriptor, std::string* out);
float MemberNumber(const rapidjson::Value& object, const char* key, float fallback);
uint32_t ParseColorString(const char* value, uint32_t fallback);
void CreateDemoPage(RuntimeHolder* holder);
