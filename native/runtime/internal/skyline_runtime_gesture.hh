// Scroll/swiper/picker/refresher/intersection/selection callbacks.

#pragma once
#include "skyline_runtime_appservice.hh"

namespace appservice {
void AnimateSwiperTo(uint32_t node_id, uint32_t page, bool animated);
void InstallPickerColumnCallbacks(AppNode* node);
void InstallScrollCallbacks(AppNode* node);
void InstallSelectionCallbacks(AppNode* node);
void InstallSwiperCallbacks(AppNode* node);
void RegisterIntersectionObserver(uint32_t node_id, uint32_t unique_id, uint32_t root_node_id, double top, double right, double bottom, double left, std::vector<double> thresholds);
void SetInputFocus(uint32_t node_id, bool focus);
void SetPageNavigateBackInterception(int32_t page_id, bool enabled);
void SetPickerColumnCurrent(uint32_t node_id, uint32_t current);
void SetRefresherTriggered(uint32_t node_id, bool open, bool two_level);
void SetSelectionRange(uint32_t selection_id, uint32_t start_id, int32_t start_offset, uint32_t end_id, int32_t end_offset);
void TriggerIntersectionForNode(uint32_t node_id, int32_t unique_id, double b_top, double b_right, double b_bottom, double b_left, double i_top, double i_right, double i_bottom, double i_left, double root_top, double root_right, double root_bottom, double root_left, double ratio, bool intersecting);
void TriggerNavigateBackIntercept(int32_t page_id);
void UnregisterIntersectionObserver(uint32_t node_id, uint32_t unique_id);
}  // namespace appservice

namespace appservice {

// Native gesture recognizer descriptor handed to NewScrollView/NewSwiper.
struct NativeGestureRecognizerNative {
  uint32_t handler_id;
  uint32_t gesture_type;
  const char* tag;
  const char** simult_handler_tags;
  uint32_t simult_handler_tag_num;
};

// The handler id is the owning node's id.  `ShouldGestureResponse` receives it
// back from package:skyline and uses it to match the JS gesture-handler entry
// (whose host node carries the same viewTag) and the node tree.
NativeGestureRecognizerNative MakeScrollGestureRecognizer(uint32_t handler_id,
                                                          bool horizontal);
NativeGestureRecognizerNative MakeSwiperGestureRecognizer(uint32_t handler_id,
                                                          bool vertical);

// Flutter invokes these `extern "C"` trampolines through the recognizer
// pointers passed by appservice_widget.cc.
extern "C" double ScrollNoopContentSize(intptr_t, int32_t, int32_t, double,
                                        double);
extern "C" void ScrollStartEvent(intptr_t ptr, double top, double left,
                                 double width, double height, uint8_t drag);
extern "C" void ScrollEndEvent(intptr_t ptr, double top, double left,
                               double width, double height, uint8_t drag);
extern "C" void ScrollEvent(intptr_t ptr, double top, double left, double width,
                            double height, double dx, double dy, uint8_t drag,
                            double velocity);
extern "C" void ScrollNoopPtr(intptr_t);
extern "C" void SwiperOnChange(intptr_t ptr, uint32_t page, uint8_t reason);
extern "C" void SwiperOnScroll(intptr_t ptr, double translate_pixels,
                               uint8_t state, uint32_t current);
extern "C" void PickerColumnOnChange(intptr_t ptr, int32_t index,
                                     int32_t reason);
extern "C" void PickerColumnOnStart(intptr_t ptr);
extern "C" void PickerColumnOnEnd(intptr_t ptr);
extern "C" void RefresherPulling(intptr_t ptr, double dy);
extern "C" void RefresherWillRefresh(intptr_t ptr, double dy);
extern "C" void RefresherRefresh(intptr_t ptr, double dy);
extern "C" void RefresherRestore(intptr_t ptr, double dy);
extern "C" void RefresherAbort(intptr_t ptr, double dy);
extern "C" void RefresherStatusChange(intptr_t ptr, int32_t mode, double yy);
extern "C" void SelectionOnChangeCallback(intptr_t ptr, double left, double top,
                                          double right, double bottom,
                                          Dart_Handle selected);

}  // namespace appservice
