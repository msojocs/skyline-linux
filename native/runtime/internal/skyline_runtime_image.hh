// Image decoding, painting, loading and cancellation.

#pragma once
#include "skyline_runtime_appservice.hh"

namespace appservice {
bool LoadBackgroundImageForNode(AppNode* node);
bool LoadImageForNode(AppNode* node);
void CancelPendingImage(AppNode* node);
void DeliverImagePayload(uint32_t request_id, std::vector<uint8_t> bytes);
}  // namespace appservice

namespace appservice {
// Paint/create/dispose hooks passed to NewDirectRenderWidget and the
// DecoratedBox background-image callbacks.  They are referenced by address in
// appservice_widget.cc, so they need external declarations.
void DirectRenderImageDispose(intptr_t /*ptr*/);
void DirectRenderImageCreate(intptr_t /*ptr*/);
void DirectRenderImageLayout(intptr_t /*ptr*/, double /*w*/, double /*h*/);
void DirectRenderImagePaint(intptr_t ptr, Dart_Handle canvas_handle, double width,
                            double height, double offset_x, double offset_y);
void BackgroundImageCreate(intptr_t /*ptr*/);
void BackgroundImageDisposed(intptr_t /*ptr*/);
void BackgroundImagePaint(intptr_t ptr, Dart_Handle canvas_handle, double left,
                          double top, double right, double bottom,
                          Dart_Handle /*path*/);
}  // namespace appservice
