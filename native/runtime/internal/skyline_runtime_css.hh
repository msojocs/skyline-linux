// CSS value parsing and declaration application.

#pragma once
#include "skyline_runtime_appservice.hh"

namespace appservice {
bool BorderSidesChanged(const AppStyle& before, const AppStyle& after);
bool CornerRadiusPercent(const AppStyle& style, int corner);
bool HasAnyRadius(const AppStyle& style);
bool HasBorder(const AppStyle& style);
bool ParseCssColor(const std::string& raw, uint32_t* out);
bool ParseNumber(const std::string& value, double* out);
bool ParsePositionPair(const std::string& raw, CssPositionAxis* x, CssPositionAxis* y);
bool ParsePxToken(const std::string& raw, double* out);
bool RadiiChanged(const AppStyle& a, const AppStyle& b);
float EffectiveBorderWidth(const AppStyle& style, int side);
float EffectiveCornerRadius(const AppStyle& style, int corner);
int ParseBackgroundRepeat(const std::string& raw);
std::vector<std::pair<std::string, std::string>> ParseDeclarations( const std::string& body);
uint32_t EffectiveBorderColor(const AppStyle& style, int side);
void ApplyDeclarations(AppStyle* style, const std::vector<std::pair<std::string, std::string>>& decls);
void BuildTransformMatrix(const AppStyle& style, double width, double height, double* out);
void ResolveTransformOrigin(const AppStyle& style, float width, float height, double* out_x, double* out_y);
}  // namespace appservice
