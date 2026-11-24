#pragma once
// Native callback table for `package:skyline` (hash = stringHash(symbol)).
// The constants are inline so the split translation units share one value.
#include "skyline_runtime_abi.hh"

using NewDecoratedBoxFn = Dart_Handle (*)(
    intptr_t ptr,
    uint32_t backgroundColor,
    double borderLeftWidth,
    double borderTopWidth,
    double borderRightWidth,
    double borderBottomWidth,
    uint32_t borderLeftColor,
    uint32_t borderTopColor,
    uint32_t borderRightColor,
    uint32_t borderBottomColor,
    uint8_t borderLeftStyle,
    uint8_t borderTopStyle,
    uint8_t borderRightStyle,
    uint8_t borderBottomStyle,
    double borderTLRadiusX,
    double borderTLRadiusY,
    double borderTRRadiusX,
    double borderTRRadiusY,
    double borderBLRadiusX,
    double borderBLRadiusY,
    double borderBRRadiusX,
    double borderBRRadiusY,
    uint8_t borderTLRadiusXPercent,
    uint8_t borderTLRadiusYPercent,
    uint8_t borderTRRadiusXPercent,
    uint8_t borderTRRadiusYPercent,
    uint8_t borderBLRadiusXPercent,
    uint8_t borderBLRadiusYPercent,
    uint8_t borderBRRadiusXPercent,
    uint8_t borderBRRadiusYPercent,
    Dart_Handle imageUrl,
    uint8_t backgroundSideX,
    uint8_t backgroundSideY,
    double alignmentX,
    double alignmentY,
    double backgroundOffsetX,
    double backgroundOffsetY,
    uint8_t repeat,
    uint8_t fit,
    double fitWidth,
    double fitHeight,
    uint8_t fitWidthPercent,
    uint8_t fitHeightPercent,
    Dart_Handle gradient,
    uint8_t backgroundImageLayerMode,
    Dart_Handle boxShadows,
    uint8_t hasMaskImage,
    void* onPaint,
    void* onCreateRenderObject,
    void* onDisposed,
    void* onPaintMask,
    Dart_Handle child);

using NewYGNodeFn = Dart_Handle (*)(
    intptr_t yogaNodeAddress,
    Dart_Handle yogaLayout,
    uint8_t isRoot,
    uint8_t isRepaintBoundary,
    uint8_t enableLayoutParagraph,
    uint8_t constraintsType,
    uint32_t concreteWidgetType,
    Dart_Handle transformSharedData,
    double originX,
    double originY,
    double alignmentX,
    double alignmentY,
    uint8_t visible,
    uint8_t catchEvent,
    uint8_t pointerEvents,
    Dart_Handle intersections,
    Dart_Handle yogaConnector,
    Dart_Handle child);

using NewYGConnectorFn = Dart_Handle (*)(uint8_t enableStackingPaint,
                                          Dart_Handle children);

using NewTextSpanFn = Dart_Handle (*)(
    intptr_t textSpanAddress,
    Dart_Handle text,
    uint32_t color,
    uint32_t backgroundColor,
    uint8_t textDecoration,
    uint8_t textDecorationStyle,
    uint32_t decorationColor,
    double decorationThickness,
    void* fontFeatures,
    uint32_t fontFeaturesLength,
    double fontSize,
    uint8_t fontWeight,
    uint8_t fontStyle,
    Dart_Handle fontFamilyFallback,
    double height,
    double letterSpacing,
    double wordSpacing,
    uint8_t wordBreak,
    Dart_Handle shadows,
    Dart_Handle children);

using NewTextFn = Dart_Handle (*)(
    intptr_t ptr,
    void* markDirty,
    void* createParagraph,
    uint8_t textAlign,
    uint8_t textDirection,
    uint8_t overflow,
    uint8_t softWrap,
    uint32_t maxLines,
    uint8_t userSelect,
    uint8_t selectOnGesture,
    uint8_t enableNativeParagraph,
    Dart_Handle children);

// Native views: `package:skyline/widgets/texture.dart`.  `NewTexture` creates
// the Dart leaf widget; `TextureDispose` lets it release channel state.
using NewTextureFn = Dart_Handle (*)(int32_t id,
                                     Dart_Handle type,
                                     uint8_t useTexture);
using TextureDisposeFn = void (*)(Dart_Handle texture);

// Skyline sticky slivers: `package:skyline/widgets/sticky.dart`.
using NewStickyHeaderFn = Dart_Handle (*)(
    intptr_t nativeWidgetPtr,
    Dart_Handle child,
    double paddingTop,
    double paddingRight,
    double paddingBottom,
    double paddingLeft,
    float offsetTop,
    uint8_t allowOverlapping,
    void* onStickOnTopChange);
using NewStickySectionFn = Dart_Handle (*)(Dart_Handle children,
                                           uint8_t pushPinnedChildren,
                                           double paddingTop,
                                           double paddingRight,
                                           double paddingBottom,
                                           double paddingLeft);

using NewOpacityFn = Dart_Handle (*)(double opacity, Dart_Handle child);

using NewLinearGradientFn = Dart_Handle (*)(
    double radianAngle,
    Dart_Handle colors,
    void* stops,
    double fitWidth,
    double fitHeight,
    uint8_t fitWidthPercent,
    uint8_t fitHeightPercent,
    uint8_t backgroundSideX,
    uint8_t backgroundSideY,
    double alignmentX,
    double alignmentY,
    double backgroundOffsetX,
    double backgroundOffsetY,
    uint8_t repeat);

using NewRadialGradientFn = Dart_Handle (*)(
    double positionX,
    double positionY,
    uint8_t positionXPercent,
    uint8_t positionYPercent,
    double radius,
    uint8_t size,
    Dart_Handle colors,
    void* stops);

using NewConicGradientFn = Dart_Handle (*)(double startAngle,
                                          double centerX,
                                          double centerY,
                                          uint8_t centerXPercent,
                                          uint8_t centerYPercent,
                                          Dart_Handle colors,
                                          Dart_Handle stops);

struct GradientStopNative {
  double value;
  uint8_t isPercent;
};

using NewBoxShadowFn = Dart_Handle (*)(uint32_t shadowColor,
                                       double shadowOffsetX,
                                       double shadowOffsetY,
                                       double blurRadius,
                                       double spreadRadius,
                                       uint8_t shadowInset);

using DecoratedBoxUpdateColorFn = void (*)(Dart_Handle decoratedBox,
                                           uint32_t backgroundColor);
using TextSpanUpdateTextFn = void (*)(Dart_Handle text, Dart_Handle data);
using TextSpanUpdateTextColorFn = void (*)(Dart_Handle text, uint32_t color);
using TextSpanUpdateFontSizeFn = void (*)(Dart_Handle text, double size);
using TextSpanUpdateFontWeightFn = void (*)(Dart_Handle text, uint8_t weight);
using TextSpanUpdateFontStyleFn = void (*)(Dart_Handle text, uint8_t style);
using TextSpanUpdateLineHeightFn = void (*)(Dart_Handle text, double height);
using TextSpanUpdateLetterSpacingFn = void (*)(Dart_Handle text, double spacing);
using TextApplyUpdateFn = void (*)(Dart_Handle text, uint8_t subtreeChanged);
using TextUpdateTextAlignFn = void (*)(Dart_Handle text, uint8_t align);
using TextUpdateMaxLinesFn = void (*)(Dart_Handle text, uint32_t maxLines);
using LoadFontFaceFn = void (*)(Dart_Handle family, Dart_Handle urls,
                                int32_t timeout_ms, int32_t callback_id);
using NotifyResourceLoadFn = void (*)(uint8_t callback_id, Dart_Handle pixels);

using NewScrollViewFn = Dart_Handle (*)(
    intptr_t ptr, uint32_t view_tag, uint8_t type, uint8_t scroll_x,
    uint8_t scroll_y, double scroll_top, double scroll_left,
    uint8_t scroll_with_animation, double lower_threshold,
    double upper_threshold, uint8_t refresher_enabled,
    double refresher_threshold, uint32_t refresher_background,
    uint8_t refresher_default_style, uint8_t refresher_triggered,
    uint8_t pinned_when_open_two_level, uint8_t enable_scroll_when_two_level,
    uint8_t enable_ballistic_refresh, uint8_t two_level_enabled,
    uint8_t two_level_triggered, double two_level_threshold,
    double close_two_level_threshold, uint8_t scroll_enabled, uint8_t bounces,
    uint8_t show_scrollbar, uint8_t fast_deceleration, uint8_t reverse,
    double cache_extent, void* recognizers, uint32_t recognizer_num,
    Dart_Handle refresher_header, uint8_t disable_clip,
    double minimum_drag_distance, double padding_top, double padding_right,
    double padding_bottom, double padding_left, uint8_t associative_container,
    uint8_t paint_order, uint8_t scroll_anchoring,
    uint8_t inner_correction_mode, void* compute_content_size,
    void* on_scroll_start, void* on_scroll, void* on_scroll_end,
    void* on_scroll_to_lower, void* on_scroll_to_upper,
    void* on_refresher_pulling, void* on_refresher_will_refresh,
    void* on_refresher_refresh, void* on_refresher_restore,
    void* on_refresher_abort, void* on_refresher_status_change,
    void* on_mark_needs_layout, Dart_Handle children);

// `NewListView`/`NewGridView` build the virtualized slivers used by
// `<list-view>`/`<grid-view>`.  The per-item mount callbacks are optional
// (`Pointer` may be null); the replay supplies every item up front.
using ListViewItemCallbackFn = void (*)(intptr_t ptr, int32_t index);
using NewListViewFn = Dart_Handle (*)(intptr_t ptr, Dart_Handle children,
                                      double padding_top, double padding_right,
                                      double padding_bottom,
                                      double padding_left,
                                      uint32_t background_color,
                                      void* on_mount_child,
                                      void* on_unmount_child);
using NewGridViewFn = Dart_Handle (*)(intptr_t ptr, Dart_Handle children,
                                      uint8_t type, int32_t cross_axis_count,
                                      float main_axis_spacing,
                                      float cross_axis_spacing,
                                      float max_cross_axis_extent,
                                      double padding_top, double padding_right,
                                      double padding_bottom,
                                      double padding_left,
                                      uint32_t background_color);

// `NewDirectRenderWidget` installs a leaf widget whose paint callback receives
// the Dart `Canvas`; images decode + draw natively (see DirectRenderImagePaint).
using NewDirectRenderWidgetFn = Dart_Handle (*)(intptr_t ptr, void* on_paint,
                                                void* on_dispose,
                                                void* on_create_render_object,
                                                void* on_layout);

// List/grid builders.  `on_build_child`/`on_dispose_child` are native
// function pointers invoked by the Dart sliver (`(ptr, index)`).
using BuilderChildCallbackFn = void (*)(intptr_t ptr, int32_t index);
using NewListBuilderFn = Dart_Handle (*)(intptr_t ptr, Dart_Handle children,
                                         uint8_t type, int32_t child_count,
                                         float child_height,
                                         double padding_top,
                                         double padding_right,
                                         double padding_bottom,
                                         double padding_left,
                                         uint32_t background_color,
                                         void* on_build_child,
                                         void* on_dispose_child);
using NewGridBuilderFn = Dart_Handle (*)(intptr_t ptr, Dart_Handle children,
                                         uint8_t type, int32_t child_count,
                                         int32_t cross_axis_count,
                                         float main_axis_spacing,
                                         float cross_axis_spacing,
                                         float max_cross_axis_extent,
                                         double padding_top,
                                         double padding_right,
                                         double padding_bottom,
                                         double padding_left,
                                         uint32_t background_color,
                                         void* on_build_child,
                                         void* on_dispose_child);
using NewListBuilderItemFn = Dart_Handle (*)(Dart_Handle children,
                                              int32_t index);
using BuilderMutateChildFn = void (*)(Dart_Handle parent, Dart_Handle child);
using BuilderInsertBeforeFn = void (*)(Dart_Handle parent, Dart_Handle child,
                                       Dart_Handle pivot);
using BuilderUpdateChildrenFn = void (*)(Dart_Handle parent,
                                         Dart_Handle children);
using BuilderUpdateIntFn = void (*)(Dart_Handle builder, int32_t value);
using BuilderUpdateFloatFn = void (*)(Dart_Handle builder, double value);
using BuilderUpdatePaddingFn = void (*)(Dart_Handle builder, double top,
                                        double right, double bottom,
                                        double left);
using BuilderUpdateColorFn = void (*)(Dart_Handle builder, uint32_t color);
using NavigateBackFn = void (*)(int32_t delta, uint8_t animated,
                                int32_t callback_id);
using NavigatorPopFn = void (*)(int32_t page_id);
using UserGestureCallbackFn = void (*)();
using UpdateAnimationControllerFn = void (*)(int32_t page_id, double value);
using SwitchTabFn = void (*)(int32_t page_id, int32_t callback_id);
using UpdateKeyboardStatusFn = void (*)(int32_t input_key, double height);
using InputValueFn = void (*)(Dart_Handle input, Dart_Handle value);
using InputIntFn = void (*)(Dart_Handle input, int32_t value);
using InputBoolFn = void (*)(Dart_Handle input, uint8_t value);
using InputFocusFn = void (*)(Dart_Handle input, uint8_t focus);
using NewInputFn = Dart_Handle (*)(
    intptr_t ptr, void* markDirty, int32_t viewId, uint32_t viewTag,
    uint8_t multiLine, Dart_Handle value, Dart_Handle type, uint8_t password,
    Dart_Handle placeholder, double placeholderFontSize,
    uint8_t placeholderFontWeight, uint32_t placeholderFontColor,
    uint8_t disable, int32_t maxLength, int32_t cursorSpacing,
    uint32_t cursorColor, uint8_t focus, uint8_t autoHeight,
    Dart_Handle confirmType, uint8_t confirmHold, int32_t cursor,
    int32_t selectionStart, int32_t selectionEnd, uint8_t showConfirmBar,
    uint8_t adjustPosition, uint8_t holdKeyboard, uint8_t textAlign,
    uint32_t color, uint32_t backgroundColor, uint8_t textDecoration,
    uint8_t textDecorationStyle, uint32_t decorationColor,
    double decorationThickness, void* fontFeatures, uint32_t fontFeaturesLength,
    double fontSize, double lineHeight, uint8_t fontWeight, uint8_t fontStyle,
    Dart_Handle fontFamilyFallback, double letterSpacing, double wordSpacing,
    Dart_Handle shadows, void* nativeOnInput, void* nativeOnFocus,
    void* nativeOnBlur, void* nativeOnLineChange, void* nativeOnConfirm,
    void* nativeOnKeyboardHeightChange, void* nativeOnSelectionChange,
    void* nativeOnCompositionStart, void* nativeOnCompositionEnd,
    void* nativeOnCompositionUpdate, void* nativeOnSetValueCompleted,
    void* nativeOnPageBottomPaddingChange);
using YGNodeSetupTransformFn = void (*)(Dart_Handle yoga_node,
                                        Dart_Handle matrix);
using YGNodeTransformOriginFn = void (*)(Dart_Handle yoga_node, double origin_x,
                                         double origin_y, double align_x,
                                         double align_y);
using YGNodeUpdatePointerEventsFn = void (*)(Dart_Handle yoga_node,
                                             uint8_t pointer_events);

using DispatchTouchEventFn = void (*)(uint8_t type, double x, double y);
// The kernel declares ScrollViewUpdateScrollTop as (Handle, Float): the
// argument is a 32-bit float, so passing a double corrupts the value.
using ScrollViewUpdateScrollTopFn = void (*)(Dart_Handle scroll_view, float top);
// `ScrollViewUpdateScrollIntoView` scrolls the owning scroll view to a target
// YGNode, honoring the scroll axis and alignment policy
// (0 explicit, 1 end, 2 center, 3 start, 4 nearest).
using ScrollViewUpdateScrollIntoViewFn = void (*)(
    Dart_Handle scroll_view, Dart_Handle yoga_node, uint8_t within_extent,
    uint8_t animated, float offset, uint8_t alignment);
// `ScrollViewUpdateOnScroll*` install the native callbacks that the Dart
// scroll view invokes when the user scrolls ("Void Function(Handle, Pointer)").
using ScrollViewUpdateOnScrollFn = void (*)(Dart_Handle scroll_view,
                                            void* callback);
using ScrollViewUpdateOnScrollStartFn = void (*)(Dart_Handle scroll_view,
                                                 void* callback);
using ScrollViewUpdateOnScrollEndFn = void (*)(Dart_Handle scroll_view,
                                               void* callback);

// `swiper` is a native Dart widget.  Its callbacks use their own signatures
// from package:skyline/widgets/swiper.dart (not the scroll-view ones):
//   onPageChanged: (ptr, page, reason)
//   onScroll:      (ptr, translatePixels, ScrollState, current)
using NewSwiperFn = Dart_Handle (*)(
    intptr_t ptr, uint32_t layout_type, uint32_t transformer_type,
    uint32_t indicator_type, uint8_t show_indicator, float indicator_offset_x,
    float indicator_offset_y, double indicator_alignment_x,
    double indicator_alignment_y, float indicator_margin,
    float indicator_spacing, float indicator_radius, float indicator_width,
    float indicator_height, uint32_t indicator_dot_color,
    uint32_t indicator_dot_active_color, uint8_t scroll_direction,
    uint8_t axis_direction, uint8_t circular, uint8_t autoplay,
    uint32_t interval, uint32_t duration, Dart_Handle easing_function,
    uint32_t initial_page, float cache_extent, float display_multiple_items,
    uint8_t snap_to_edge, float viewport_margin_left,
    float viewport_margin_right, void* recognizers, uint32_t recognizer_num,
    void* on_page_changed, void* on_scroll, Dart_Handle children);
using SwiperUpdateOnChangeFn = void (*)(Dart_Handle swiper, void* callback);
using SwiperUpdateOnScrollFn = void (*)(Dart_Handle swiper, void* callback);
using SwiperAnimateToPageFn = void (*)(Dart_Handle swiper, uint32_t page);
using SwiperJumpToPageFn = void (*)(Dart_Handle swiper, uint32_t page);

// `picker-view-column` is also a native Dart widget; there is no
// `NewPickerView` (the parent picker-view is a plain box).
using NewPickerViewColumnFn = Dart_Handle (*)(
    intptr_t ptr, uint32_t current, float indicator_height,
    double border_left_width, double border_top_width,
    double border_right_width, double border_bottom_width,
    uint32_t border_left_color, uint32_t border_top_color,
    uint32_t border_right_color, uint32_t border_bottom_color,
    uint8_t border_left_style, uint8_t border_top_style,
    uint8_t border_right_style, uint8_t border_bottom_style,
    uint32_t background_color, uint8_t immediate_change_changed,
    uint8_t magnifier, double magnification, double over_under_center_opacity,
    void* on_change, void* on_start, void* on_end, Dart_Handle children);
using PickerViewColumnUpdateOnChangeFn = void (*)(Dart_Handle column,
                                                  void* callback);
using PickerViewColumnUpdateOnStartFn = void (*)(Dart_Handle column,
                                                 void* callback);
using PickerViewColumnUpdateOnEndFn = void (*)(Dart_Handle column,
                                               void* callback);
using PickerViewColumnUpdateCurrentFn = void (*)(Dart_Handle column,
                                                 uint32_t current);

// Programmatic focus/blur (input.dart focusManually/blurManually).
using InputFocusManuallyFn = void (*)(Dart_Handle input);
using InputBlurManuallyFn = void (*)(Dart_Handle input);

// IntersectionObserver: native registers the observer on the Dart YGNode;
// Dart's layer machinery calls the exported YogaTriggerIntersection* symbols.
using YGNodeAddIntersectionObserverFn = void (*)(
    Dart_Handle yoga_node, uint32_t unique_id, Dart_Handle root,
    double root_margin_top, double root_margin_right,
    double root_margin_bottom, double root_margin_left,
    Dart_Handle thresholds);
using YGNodeRemoveIntersectionObserverFn = void (*)(Dart_Handle yoga_node,
                                                    uint32_t unique_id);

// Selection (SkylineSelection widget).
using NewSelectionFn = Dart_Handle (*)(
    intptr_t ptr, uint8_t disable_context_menu, void* on_selection_change,
    uint32_t selection_handle_color, uint32_t selection_color,
    Dart_Handle child);
using SelectionUpdateOnSelectionChangeFn = void (*)(Dart_Handle selection,
                                                    intptr_t ptr,
                                                    void* callback);
using SelectionSetRangeFn = void (*)(Dart_Handle selection,
                                     intptr_t start_span,
                                     int32_t start_offset,
                                     intptr_t end_span, int32_t end_offset);

// `<share-element>` is the Skyline builtin that participates in Flutter's Hero
// flights.  Native creates a `SkylineHero` for every share-element that has a
// `key` and updates it in place when the WXML properties change.  The Dart
// frame callback is invoked while the flight animates and returns the rect the
// shuttle should occupy; the runtime forwards the frame data to the JS
// `onFrame-<viewTag>` worklet (registered by the builtin component).
using NewHeroFn = Dart_Handle (*)(intptr_t ptr, void* mark_dirty,
                                  Dart_Handle tag, uint8_t gesture,
                                  Dart_Handle on_frame,
                                  Dart_Handle rect_tween_type,
                                  Dart_Handle shuttle_on_push,
                                  Dart_Handle shuttle_on_pop, Dart_Handle child);
using UpdateHeroTagFn = void (*)(Dart_Handle hero, Dart_Handle tag);
using UpdateHeroGestureFn = void (*)(Dart_Handle hero, uint8_t gesture);
using UpdateHeroStringFn = void (*)(Dart_Handle hero, Dart_Handle value);

// Refresher programmatic open/close.
using ScrollViewRequestRefresherFn = void (*)(Dart_Handle scroll_view,
                                              uint8_t open, int32_t duration,
                                              Dart_Handle easing);
using SetNavigateBackInterceptionFn = void (*)(int32_t page_id,
                                               uint8_t interception);

// D10: accessibility.  Negative/absent semantics wrappers are Dart widgets the
// runtime builds through NewSemantics and updates in place through Semantics*.
using NewSemanticsFn = Dart_Handle (*)(Dart_Handle child,
                                       Dart_Handle semantics_label,
                                       Dart_Handle semantics_role,
                                       uint8_t semantics_hidden,
                                       uint8_t semantics_disable);
using SemanticsTextFn = void (*)(Dart_Handle semantics, Dart_Handle value);
using SemanticsBoolFn = void (*)(Dart_Handle semantics, uint8_t value);

// D10: snapshot / automation callbacks registered by package:skyline.
using NewSnapshotFn = Dart_Handle (*)(Dart_Handle child,
                                      uint8_t allow_snapshotting);
using SnapshotUpdateAllowSnapshottingFn = void (*)(Dart_Handle snapshot,
                                                   uint8_t allow);
using SnapshotTakeSnapshotFn = uint8_t (*)(Dart_Handle snapshot, uint8_t format,
                                           intptr_t callback_ptr);
// `AppSnapshotCallbackNative`: (ptr, width, height, Uint8List data).
using AppSnapshotCallback = void (*)(intptr_t ptr, int32_t width, int32_t height,
                                     Dart_Handle data);
using TakeAppSnapshotFn = void (*)(intptr_t ptr, double scale,
                                   uint8_t compressed,
                                   AppSnapshotCallback callback);
// `OnSnapshotCallbackNative`: (ptr, Uint8List data).
using NodeSnapshotCallback = void (*)(intptr_t ptr, Dart_Handle data);
using YGNodeTakeSnapshotFn = void (*)(Dart_Handle yoga_node, uint8_t format,
                                      intptr_t ptr,
                                      NodeSnapshotCallback callback);
using YGNodeSetIsRepaintBoundaryFn = void (*)(Dart_Handle yoga_node,
                                              uint8_t is_boundary);
using DumpTreeFn = void (*)();
using CollectGarbageFn = void (*)();
using SendMessageToRunnerFn = void (*)(Dart_Handle message);
using UserGestureFn = void (*)();
using AppCommandFn = void (*)(uint8_t open_type);
using VoidCommandFn = void (*)();
using AccessibilityBoldTextEnabledFn = uint8_t (*)();

// Accessibility state is shared between the Dart runtime layer and the host
// embedder.  The host installs `SkylineSetAccessibilitySink`; `kind` 0 asks the
// embedder to apply accessibility *features*, kind 1 enables the semantics
// tree (the two are separate engine APIs).
using SkylineAccessibilitySink = void (*)(int32_t kind, int32_t value);
inline std::atomic<SkylineAccessibilitySink> g_accessibility_sink{nullptr};
inline std::atomic<int32_t> g_accessibility_enabled{0};
inline std::atomic<int32_t> g_accessibility_bold_text{0};
// DevTools shell HTTP bridge: the addon proxies `setHttpRequestCallback`
// (window.fetch in the pageframe renderer) so network requests show up in the
// DevTools Network panel instead of an in-engine dart:io socket.
using SkylineHttpRequestSink = void (*)(uint32_t request_id, const char* url);
using SkylineHasHttpRequestCallbackFn = int32_t (*)();
inline std::atomic<SkylineHttpRequestSink> g_shell_http_request{nullptr};
inline std::atomic<SkylineHasHttpRequestCallbackFn> g_shell_http_available{nullptr};

inline bool ShellHttpAvailable() {
  auto* has = g_shell_http_available.load();
  return has != nullptr && has() != 0;
}

inline void RequestShellHttp(uint32_t request_id, const std::string& url) {
  auto sink = g_shell_http_request.load();
  if (sink != nullptr) sink(request_id, url.c_str());
}
// SnapshotTakeSnapshot is synchronous: SceneToImage runs inside the call, so
// this carries the DevTools request id into the snapshot callback.
inline std::atomic<int32_t> g_pending_node_snapshot_request{0};
using ConnectorUpdateChildrenFn = void (*)(Dart_Handle connector,
                                           uint8_t stackingPaint,
                                           Dart_Handle children);
using MarkYGNodeNeedsLayoutFn = void (*)(Dart_Handle yogaNode);
using OpacityUpdateFn = void (*)(Dart_Handle widget, double opacity);
using DecoratedBoxUpdateBorderFn = void (*)(Dart_Handle decoratedBox,
                                            double leftWidth,
                                            double topWidth,
                                            double rightWidth,
                                            double bottomWidth,
                                            uint32_t leftColor,
                                            uint32_t topColor,
                                            uint32_t rightColor,
                                            uint32_t bottomColor,
                                            uint8_t leftStyle,
                                            uint8_t topStyle,
                                            uint8_t rightStyle,
                                            uint8_t bottomStyle);
using DecoratedBoxUpdateShadowFn = void (*)(Dart_Handle decoratedBox,
                                            Dart_Handle shadows);
using DecoratedBoxUpdateRadiusFn = void (*)(Dart_Handle decoratedBox,
                                            uint8_t side, double x, double y,
                                            uint8_t x_is_percent,
                                            uint8_t y_is_percent);
using DecoratedBoxUpdateGradientFn = void (*)(Dart_Handle decoratedBox,
                                              Dart_Handle gradient);
using DecoratedBoxClearGradientFn = void (*)(Dart_Handle decoratedBox);
using ClipRRectUpdateRadiusFn = void (*)(
    Dart_Handle clip_rrect, double top_left_x, double top_left_y,
    double top_right_x, double top_right_y, double bottom_left_x,
    double bottom_left_y, double bottom_right_x, double bottom_right_y,
    uint8_t top_left_x_pct, uint8_t top_left_y_pct, uint8_t top_right_x_pct,
    uint8_t top_right_y_pct, uint8_t bottom_left_x_pct,
    uint8_t bottom_left_y_pct, uint8_t bottom_right_x_pct,
    uint8_t bottom_right_y_pct);

using NewClipRRectFn = Dart_Handle (*)(
    uint8_t disableClip,
    double topLeftRadiusX,
    double topLeftRadiusY,
    double topRightRadiusX,
    double topRightRadiusY,
    double bottomLeftRadiusX,
    double bottomLeftRadiusY,
    double bottomRightRadiusX,
    double bottomRightRadiusY,
    uint8_t topLeftRadiusXPercent,
    uint8_t topLeftRadiusYPercent,
    uint8_t topRightRadiusXPercent,
    uint8_t topRightRadiusYPercent,
    uint8_t bottomLeftRadiusXPercent,
    uint8_t bottomLeftRadiusYPercent,
    uint8_t bottomRightRadiusXPercent,
    uint8_t bottomRightRadiusYPercent,
    double borderLeftWidth,
    double borderTopWidth,
    double borderRightWidth,
    double borderBottomWidth,
    Dart_Handle child);

using CreatePageRouteFn = void (*)(
    int32_t callbackId,
    uint8_t openType,
    uint8_t isCustomRoute,
    uint8_t isTabPage,
    int32_t pageId,
    uint8_t opaque,
    uint8_t maintainState,
    uint32_t transitionDuration,
    uint32_t reverseTransitionDuration,
    uint32_t barrierColor,
    uint8_t hasBarrierColor,
    uint8_t barrierDismissible,
    Dart_Handle barrierLabel,
    uint8_t canTransitionTo,
    uint8_t canTransitionFrom,
    uint8_t forceSecondaryAnimation,
    uint8_t popGestureEnabled,
    uint8_t allowEnterRouteSnapshotting,
    uint8_t allowExitRouteSnapshotting,
    uint8_t fullscreenDrag,
    uint8_t dismissDirection,
    Dart_Handle animationHash,
    Dart_Handle secondaryAnimationHash,
    Dart_Handle host,
    Dart_Handle root,
    void* config);

inline constexpr uint32_t kCreatePageRouteHash = 662439267u;
// Direct-render image invalidation callbacks (used by the image and update
// modules).
inline constexpr uint32_t kBackgroundImageMarkNeedPaintHash = 741124659u;
inline constexpr uint32_t kImageMarkNeedPaintHash = 753875657u;
inline constexpr uint32_t kNewDecoratedBoxHash = 548213892u;
inline constexpr uint32_t kNewYGNodeHash = 644570152u;
inline constexpr uint32_t kNewYGConnectorHash = 517238689u;
inline constexpr uint32_t kNewTextHash = 738901768u;
inline constexpr uint32_t kNewTextSpanHash = 553157049u;
inline constexpr uint32_t kNewTextureHash = 669864072u;
inline constexpr uint32_t kTextureDisposeHash = 664334829u;
inline constexpr uint32_t kNewStickyHeaderHash = 528134358u;
inline constexpr uint32_t kNewStickySectionHash = 567872153u;
inline constexpr uint32_t kNewOpacityHash = 357244940u;
inline constexpr uint32_t kNewClipRRectHash = 744380804u;
inline constexpr uint32_t kNewLinearGradientHash = 753559496u;
inline constexpr uint32_t kNewRadialGradientHash = 767633286u;
inline constexpr uint32_t kNewConicGradientHash = 767183460u;
inline constexpr uint32_t kNewBoxShadowHash = 459522077u;
inline constexpr uint32_t kDecoratedBoxUpdateColorHash = 533446964u;
inline constexpr uint32_t kYGConnectorUpdateChildrenHash = 542320778u;
inline constexpr uint32_t kMarkYGNodeNeedsLayoutHash = 739701891u;
inline constexpr uint32_t kOpacityUpdateHash = 362821014u;
inline constexpr uint32_t kDecoratedBoxUpdateBorderHash = 515044687u;
inline constexpr uint32_t kDecoratedBoxUpdateShadowHash = 458311868u;
inline constexpr uint32_t kDecoratedBoxUpdateBorderSideRadiusHash = 464860390u;
inline constexpr uint32_t kDecoratedBoxUpdateGradientHash = 756142529u;
inline constexpr uint32_t kDecoratedBoxClearGradientHash = 754568836u;
inline constexpr uint32_t kClipRRectUpdateRadiusHash = 450816005u;
inline constexpr uint32_t kTextSpanUpdateTextHash = 752549844u;
inline constexpr uint32_t kTextSpanUpdateTextColorHash = 504732227u;
inline constexpr uint32_t kTextSpanUpdateFontSizeHash = 666327014u;
inline constexpr uint32_t kTextSpanUpdateFontWeightHash = 745670781u;
inline constexpr uint32_t kTextSpanUpdateFontStyleHash = 640254097u;
inline constexpr uint32_t kTextSpanUpdateLineHeightHash = 756511120u;
inline constexpr uint32_t kTextSpanUpdateLetterSpacingHash = 527068474u;
inline constexpr uint32_t kTextApplyUpdateHash = 651704001u;
inline constexpr uint32_t kTextUpdateTextAlignHash = 549308516u;
inline constexpr uint32_t kTextUpdateMaxLinesHash = 464602096u;
inline constexpr uint32_t kLoadFontFaceHash = 643604203u;
inline constexpr uint32_t kNotifyResourceLoadHash = 729783546u;
inline constexpr uint32_t kDispatchTouchEventHash = 746946373u;
inline constexpr uint32_t kNewScrollViewHash = 457879666u;
inline constexpr uint32_t kNewListViewHash = 461881501u;
inline constexpr uint32_t kNewGridViewHash = 440905746u;
inline constexpr uint32_t kNewDirectRenderWidgetHash = 755744935u;
inline constexpr uint32_t kRequestHttpResourceHash = 670378368u;
inline constexpr uint32_t kOnForwardHttpGetResponseHash = 663443835u;
inline constexpr uint32_t kGetNodeFromPointHash = 771509372u;
inline constexpr uint32_t kScrollViewUpdateScrollTopHash = 390858736u;
inline constexpr uint32_t kScrollViewUpdateScrollIntoViewHash = 449098045u;
inline constexpr uint32_t kScrollViewUpdateOnScrollHash = 694938287u;
inline constexpr uint32_t kScrollViewUpdateOnScrollEndHash = 721422539u;
inline constexpr uint32_t kScrollViewUpdateOnScrollStartHash = 759191700u;
inline constexpr uint32_t kNewSwiperHash = 523432330u;
inline constexpr uint32_t kSwiperUpdateOnChangeHash = 670492408u;
inline constexpr uint32_t kSwiperUpdateOnScrollHash = 692717296u;
inline constexpr uint32_t kSwiperAnimateToPageHash = 659191896u;
inline constexpr uint32_t kSwiperJumpToPageHash = 659178726u;
inline constexpr uint32_t kNewPickerViewColumnHash = 558687230u;
inline constexpr uint32_t kPickerViewColumnUpdateOnChangeHash = 639706827u;
inline constexpr uint32_t kPickerViewColumnUpdateOnStartHash = 760325763u;
inline constexpr uint32_t kPickerViewColumnUpdateOnEndHash = 711584966u;
inline constexpr uint32_t kPickerViewColumnUpdateCurrentHash = 756271499u;
inline constexpr uint32_t kInputFocusManuallyHash = 353543806u;
inline constexpr uint32_t kInputBlurManuallyHash = 349711200u;
inline constexpr uint32_t kYGNodeAddIntersectionObserverHash = 507061131u;
inline constexpr uint32_t kYGNodeRemoveIntersectionObserverHash = 535482992u;
inline constexpr uint32_t kNewSelectionHash = 552006557u;
inline constexpr uint32_t kSelectionUpdateOnSelectionChangeHash = 648273072u;
inline constexpr uint32_t kSelectionSetRangeHash = 656347875u;
inline constexpr uint32_t kScrollViewRequestRefresherRefreshHash = 491884374u;
inline constexpr uint32_t kScrollViewRequestRefresherTwoLevelHash = 685371207u;
inline constexpr uint32_t kSetNavigateBackInterceptionHash = 562643095u;
inline constexpr uint32_t kNewListBuilderHash = 526654284u;
inline constexpr uint32_t kNewListBuilderItemHash = 629516111u;
inline constexpr uint32_t kListBuilderAppendChildHash = 707989954u;
inline constexpr uint32_t kListBuilderRemoveChildHash = 721255976u;
inline constexpr uint32_t kListBuilderInsertBeforeHash = 641555288u;
inline constexpr uint32_t kListBuilderUpdateChildrenHash = 544349630u;
inline constexpr uint32_t kListBuilderUpdateChildCountHash = 769400916u;
inline constexpr uint32_t kListBuilderUpdateChildHeightHash = 738571139u;
inline constexpr uint32_t kListBuilderUpdatePaddingHash = 527346916u;
inline constexpr uint32_t kListBuilderUpdateBackgroundColorHash = 528923708u;
inline constexpr uint32_t kListBuilderItemUpdateChildrenHash = 549538126u;
inline constexpr uint32_t kListBuilderItemUpdateIndexHash = 556218809u;
inline constexpr uint32_t kNewGridBuilderHash = 506712993u;
inline constexpr uint32_t kNewGridBuilderItemHash = 615943092u;
inline constexpr uint32_t kGridBuilderAppendChildHash = 720875816u;
inline constexpr uint32_t kGridBuilderRemoveChildHash = 719309313u;
inline constexpr uint32_t kGridBuilderInsertBeforeHash = 639062234u;
inline constexpr uint32_t kGridBuilderUpdateChildrenHash = 542877480u;
inline constexpr uint32_t kGridBuilderUpdateChildCountHash = 753287585u;
inline constexpr uint32_t kGridBuilderUpdateCrossAxisCountHash = 750670620u;
inline constexpr uint32_t kGridBuilderUpdateMainAxisSpacingHash = 534430105u;
inline constexpr uint32_t kGridBuilderUpdateCrossAxisSpacingHash = 530243228u;
inline constexpr uint32_t kGridBuilderUpdateMaxCrossAxisExtentHash = 759854312u;
inline constexpr uint32_t kGridBuilderUpdatePaddingHash = 519451945u;
inline constexpr uint32_t kGridBuilderUpdateBackgroundColorHash = 514461315u;
inline constexpr uint32_t kGridBuilderItemUpdateChildrenHash = 551358966u;
inline constexpr uint32_t kGridBuilderItemUpdateIndexHash = 562990716u;
inline constexpr uint32_t kNavigateBackHash = 802972930u;
inline constexpr uint32_t kSwitchTabHash = 958852003u;
inline constexpr uint32_t kUpdateKeyboardStatusHash = 467802059u;
inline constexpr uint32_t kInputValueHash = 660826392u;
inline constexpr uint32_t kInputCursorHash = 532468509u;
inline constexpr uint32_t kInputSelectionStartHash = 760722848u;
inline constexpr uint32_t kInputSelectionEndHash = 712315579u;
inline constexpr uint32_t kInputHoldKeyboardHash = 720865937u;
inline constexpr uint32_t kInputFocusHash = 453329760u;
inline constexpr uint32_t kNewInputHash = 762417174u;
inline constexpr uint32_t kYGNodeSetupTransformHash = 620430677u;
inline constexpr uint32_t kYGNodeTransformOriginHash = 740488219u;
inline constexpr uint32_t kYGNodeUpdatePointerEventsHash = 449412672u;
// D10: accessibility, snapshots and DevTools automation.
inline constexpr uint32_t kNewSemanticsHash = 448407021u;
inline constexpr uint32_t kSemanticsLabelHash = 681175330u;
inline constexpr uint32_t kSemanticsRoleHash = 666094628u;
inline constexpr uint32_t kSemanticsHiddenHash = 560051307u;
inline constexpr uint32_t kSemanticsDisableHash = 646536060u;
inline constexpr uint32_t kAccessibilityBoldTextEnabledHash = 715306527u;
inline constexpr uint32_t kNewSnapshotHash = 744193752u;
inline constexpr uint32_t kSnapshotUpdateAllowSnapshottingHash = 519292321u;
inline constexpr uint32_t kSnapshotTakeSnapshotHash = 750173314u;
inline constexpr uint32_t kTakeAppSnapshotHash = 762914009u;
inline constexpr uint32_t kYGNodeTakeSnapshotHash = 762672341u;
inline constexpr uint32_t kYGNodeSetIsRepaintBoundaryHash = 367311360u;
inline constexpr uint32_t kDumpElementTreeHash = 669860362u;
inline constexpr uint32_t kDumpRenderTreeHash = 665271336u;
inline constexpr uint32_t kCollectGarbageHash = 655232023u;
inline constexpr uint32_t kSendMessageToRunnerHash = 517490473u;
inline constexpr uint32_t kRelaunchAppHash = 390635245u;
inline constexpr uint32_t kShutdownApplicationHash = 539287559u;
// `<share-element>` hero flights (package:skyline/widgets/hero.dart).
inline constexpr uint32_t kNewHeroHash = 454206811u;
inline constexpr uint32_t kUpdateHeroTagHash = 506784048u;
inline constexpr uint32_t kUpdateHeroGestureHash = 665789340u;
inline constexpr uint32_t kUpdateHeroRectTweenTypeHash = 654694293u;
inline constexpr uint32_t kUpdateHeroShuttleOnPushHash = 482852632u;
inline constexpr uint32_t kUpdateHeroShuttleOnPopHash = 397872794u;
inline constexpr uint32_t kStartUserGestureHash = 662129673u;
inline constexpr uint32_t kStopUserGestureHash = 638807572u;
// stringHash('NavigatorPop') / stringHash('UpdateAnimationController'): the
// Dart route callbacks the custom-route worklets drive through
// `workletUIModule` (see native/src/bootstrap.js).
inline constexpr uint32_t kNavigatorPopHash = 399219931u;
inline constexpr uint32_t kUpdateAnimationControllerHash = 519424339u;
