// List/grid builder incremental materialization and sync.

#include "internal/skyline_runtime_module.hh"
#include "internal/skyline_runtime_builder.hh"

namespace appservice {

AppNode* FindBuilderItemNode(AppNode* builder, int index) {
  int sequential = 0;
  for (AppNode* child : builder->children) {
    if (child->released || child->style.display_none) continue;
    if (child->name != "listbuilderitem" && child->name != "gridbuilderitem" &&
        child->name != "list-item" && child->name != "grid-item" &&
        child->name != "builderitem") {
      continue;
    }
    int item_index = sequential;
    const auto attr = child->attrs.find("index");
    if (attr != child->attrs.end()) {
      item_index = std::atoi(attr->second.c_str());
    }
    if (item_index == index) return child;
    sequential++;
  }
  return nullptr;
}

uint64_t HashBytes(uint64_t seed, const void* data, size_t size) {
  const auto* bytes = static_cast<const uint8_t*>(data);
  for (size_t i = 0; i < size; i++) {
    seed ^= bytes[i];
    seed *= 1099511628211ull;
  }
  return seed;
}

uint64_t HashString(uint64_t seed, const std::string& value) {
  seed = HashBytes(seed, value.data(), value.size());
  const uint8_t separator = 0xFF;
  return HashBytes(seed, &separator, 1);
}

// Content signature used to notice builder-item changes without rebuilding the
// whole page.  Attributes are sorted so unordered_map iteration order does not
// leak into the result.
uint64_t NodeContentSignature(const AppNode* node) {
  if (node == nullptr) return 0;
  uint64_t hash = 1469598103934665603ull;
  hash = HashString(hash, node->tag);
  hash = HashString(hash, node->name);
  hash = HashString(hash, node->text);
  hash = HashString(hash, node->inline_style);
  for (const std::string& klass : node->classes) hash = HashString(hash, klass);
  std::vector<std::string> keys;
  keys.reserve(node->attrs.size());
  for (const auto& entry : node->attrs) keys.push_back(entry.first);
  std::sort(keys.begin(), keys.end());
  for (const std::string& key : keys) {
    hash = HashString(hash, key);
    hash = HashString(hash, node->attrs.at(key));
  }
  for (const AppNode* child : node->children) {
    if (child == nullptr || child->released) continue;
    hash = HashBytes(hash, &child->id, sizeof(child->id));
    const uint64_t child_hash = NodeContentSignature(child);
    hash = HashBytes(hash, &child_hash, sizeof(child_hash));
  }
  return hash;
}

void MaterializeBuilderItem(RuntimeHolder* holder, intptr_t address,
                            int index) {
  auto found = g_builders_by_address.find(address);
  if (found == g_builders_by_address.end() || found->second == nullptr) return;
  AppNode* builder = found->second;
  if (builder->released || builder->builder_p == nullptr) return;
  if (builder->builder_items.count(index) != 0) return;
  builder->builder_pending.erase(index);
  AppNode* item = FindBuilderItemNode(builder, index);
  if (item == nullptr) return;

  auto* newYGNode = reinterpret_cast<NewYGNodeFn>(
      FindCallback(holder->ffi_table, kNewYGNodeHash));
  auto* newYGConnector = reinterpret_cast<NewYGConnectorFn>(
      FindCallback(holder->ffi_table, kNewYGConnectorHash));
  auto* newText =
      reinterpret_cast<NewTextFn>(FindCallback(holder->ffi_table, kNewTextHash));
  auto* newTextSpan = reinterpret_cast<NewTextSpanFn>(
      FindCallback(holder->ffi_table, kNewTextSpanHash));
  auto* newDecoratedBox = reinterpret_cast<NewDecoratedBoxFn>(
      FindCallback(holder->ffi_table, kNewDecoratedBoxHash));
  auto* newOpacity = reinterpret_cast<NewOpacityFn>(
      FindCallback(holder->ffi_table, kNewOpacityHash));
  auto* newScrollView = reinterpret_cast<NewScrollViewFn>(
      FindCallback(holder->ffi_table, kNewScrollViewHash));
  auto* newListView = reinterpret_cast<NewListViewFn>(
      FindCallback(holder->ffi_table, kNewListViewHash));
  auto* newGridView = reinterpret_cast<NewGridViewFn>(
      FindCallback(holder->ffi_table, kNewGridViewHash));
  auto* newDirectRenderWidget = reinterpret_cast<NewDirectRenderWidgetFn>(
      FindCallback(holder->ffi_table, kNewDirectRenderWidgetHash));
  auto* newListBuilder = reinterpret_cast<NewListBuilderFn>(
      FindCallback(holder->ffi_table, kNewListBuilderHash));
  auto* newGridBuilder = reinterpret_cast<NewGridBuilderFn>(
      FindCallback(holder->ffi_table, kNewGridBuilderHash));
  auto* newItem =
      builder->builder_kind == 1
          ? reinterpret_cast<NewListBuilderItemFn>(
                FindCallback(holder->ffi_table, kNewGridBuilderItemHash))
          : reinterpret_cast<NewListBuilderItemFn>(
                FindCallback(holder->ffi_table, kNewListBuilderItemHash));
  auto* append = reinterpret_cast<BuilderMutateChildFn>(
      FindCallback(holder->ffi_table, builder->builder_kind == 1
                                          ? kGridBuilderAppendChildHash
                                          : kListBuilderAppendChildHash));
  if (newYGNode == nullptr || newYGConnector == nullptr || newText == nullptr ||
      newTextSpan == nullptr || newDecoratedBox == nullptr ||
      newOpacity == nullptr || newItem == nullptr || append == nullptr) {
    return;
  }

  float width = builder->measured_w > 0.0f
                    ? builder->measured_w
                    : static_cast<float>(g_viewport_width);
  if (width <= 0.0f) width = 320.0f;
  if (builder->builder_kind == 1) {
    const int columns = std::max(1, builder->builder_cross_axis_count);
    width = (width - builder->builder_cross_spacing * (columns - 1)) /
            static_cast<float>(columns);
    if (width < 0.0f) width = 0.0f;
  }
  PrepareLayoutBuffers(item);
  MeasureNode(item, width, true, kInf, false);
  if (builder->builder_child_height > 0.0f) {
    item->measured_h = builder->builder_child_height;
  }
  if (item->measured_w <= 0.0f) item->measured_w = width;
  std::vector<NodeMetric> metrics;
  LayoutNode(item, 0.0f, 0.0f, item->measured_w, item->measured_h, &metrics,
             item->abs_x, item->abs_y);
  item->sliver_layout = true;
  WriteLayoutMetrics(metrics);

  Dart_Handle widget =
      BuildWidget(item, newYGNode, newYGConnector, newText, newTextSpan,
                  newDecoratedBox, newOpacity, newScrollView, newListView,
                  newGridView, newDirectRenderWidget, newListBuilder,
                  newGridBuilder, true);
  if (widget == nullptr || Dart_IsError(widget)) return;
  Dart_Handle children = Dart_NewList(1);
  Dart_ListSetAt(children, 0, widget);
  Dart_Handle item_widget = newItem(children, index);
  if (item_widget == nullptr || Dart_IsError(item_widget)) {
    ReportDartError("appservice builder item", item_widget);
    return;
  }
  Dart_Handle builder_widget = Dart_HandleFromPersistent(builder->builder_p);
  Dart_Handle pivot = nullptr;
  int pivot_index = -1;
  for (const auto& entry : builder->builder_items) {
    if (entry.first <= index) continue;
    if (pivot_index < 0 || entry.first < pivot_index) {
      pivot_index = entry.first;
      pivot = Dart_HandleFromPersistent(entry.second);
    }
  }
  auto* insert_before = reinterpret_cast<BuilderInsertBeforeFn>(FindCallback(
      holder->ffi_table, builder->builder_kind == 1
                             ? kGridBuilderInsertBeforeHash
                             : kListBuilderInsertBeforeHash));
  if (pivot != nullptr && insert_before != nullptr) {
    insert_before(builder_widget, item_widget, pivot);
    SKY_LOG(kDebug, kLogRuntime, "%s: #%u index=%d before=%d",
            builder->builder_kind == 1 ? "GridBuilderInsertBefore"
                                       : "ListBuilderInsertBefore",
            builder->id, index, pivot_index);
  } else {
    append(builder_widget, item_widget);
    SKY_LOG(kDebug, kLogRuntime, "%s: #%u index=%d",
            builder->builder_kind == 1 ? "GridBuilderAppendChild"
                                       : "ListBuilderAppendChild",
            builder->id, index);
  }
  builder->builder_items[index] = Dart_NewPersistentHandle(item_widget);
  builder->builder_item_nodes[index] = item;
  builder->builder_item_signatures[index] = NodeContentSignature(item);
  SKY_LOG(kDebug, kLogRuntime, "builder mount: #%u index=%d (%gx%g)",
          builder->id, index, static_cast<double>(item->measured_w),
          static_cast<double>(item->measured_h));
}

void OnBuilderBuildChild(intptr_t address, int32_t index) {
  if (g_page == nullptr || g_page->holder == nullptr) return;
  AppNode* builder = nullptr;
  auto found = g_builders_by_address.find(address);
  if (found != g_builders_by_address.end()) builder = found->second;
  if (builder == nullptr || builder->builder_items.count(index) != 0 ||
      builder->builder_pending.count(index) != 0) {
    return;
  }
  builder->builder_pending.insert(index);
  RuntimeHolder* holder = g_page->holder;
  auto runner = UiTaskRunner();
  if (!runner) {
    builder->builder_pending.erase(index);
    return;
  }
  runner->PostTask([holder, address, index]() {
    DemoPageApiScope scope;
    MaterializeBuilderItem(holder, address, index);
  });
}

void OnBuilderDisposeChild(intptr_t address, int32_t index) {
  if (g_page == nullptr || g_page->holder == nullptr) return;
  RuntimeHolder* holder = g_page->holder;
  auto runner = UiTaskRunner();
  if (!runner) return;
  runner->PostTask([holder, address, index]() {
    DemoPageApiScope scope;
    auto found = g_builders_by_address.find(address);
    if (found == g_builders_by_address.end() || found->second == nullptr) return;
    AppNode* builder = found->second;
    auto item = builder->builder_items.find(index);
    if (item == builder->builder_items.end()) return;
    auto* remove = reinterpret_cast<BuilderMutateChildFn>(FindCallback(
        holder->ffi_table, builder->builder_kind == 1
                               ? kGridBuilderRemoveChildHash
                               : kListBuilderRemoveChildHash));
    if (remove != nullptr && builder->builder_p != nullptr) {
      remove(Dart_HandleFromPersistent(builder->builder_p),
             Dart_HandleFromPersistent(item->second));
    }
    Dart_DeletePersistentHandle(item->second);
    builder->builder_items.erase(item);
    builder->builder_item_nodes.erase(index);
    builder->builder_item_signatures.erase(index);
    SKY_LOG(kDebug, kLogRuntime, "builder dispose: #%u index=%d", builder->id,
            index);
  });
}

// Rebuilds one materialized builder item's content and returns a fresh
// SkylineListBuilderItem/GridBuilderItem widget.  The caller swaps it into the
// builder through ListBuilderUpdateChildren; the item-level callback is also
// invoked for API parity.
Dart_Handle RebuildBuilderItemContent(RuntimeHolder* holder, AppNode* builder,
                                      int index, AppNode* item,
                                      Dart_Handle item_widget) {
  if (holder == nullptr || builder == nullptr || item == nullptr ||
      item_widget == nullptr) {
    return nullptr;
  }
  ResetNodeHandles(item, true);
  PrepareLayoutBuffers(item);
  float width = builder->measured_w > 0.0f
                    ? builder->measured_w
                    : static_cast<float>(g_viewport_width);
  if (width <= 0.0f) width = 320.0f;
  if (builder->builder_kind == 1) {
    const int columns = std::max(1, builder->builder_cross_axis_count);
    width = (width - builder->builder_cross_spacing * (columns - 1)) /
            static_cast<float>(columns);
    if (width < 0.0f) width = 0.0f;
  }
  MeasureNode(item, width, true, kInf, false);
  if (builder->builder_child_height > 0.0f) {
    item->measured_h = builder->builder_child_height;
  }
  if (item->measured_w <= 0.0f) item->measured_w = width;
  std::vector<NodeMetric> metrics;
  LayoutNode(item, 0.0f, 0.0f, item->measured_w, item->measured_h, &metrics,
             item->abs_x, item->abs_y);
  item->sliver_layout = true;
  WriteLayoutMetrics(metrics);

  auto* newYGNode = reinterpret_cast<NewYGNodeFn>(
      FindCallback(holder->ffi_table, kNewYGNodeHash));
  auto* newYGConnector = reinterpret_cast<NewYGConnectorFn>(
      FindCallback(holder->ffi_table, kNewYGConnectorHash));
  auto* newText =
      reinterpret_cast<NewTextFn>(FindCallback(holder->ffi_table, kNewTextHash));
  auto* newTextSpan = reinterpret_cast<NewTextSpanFn>(
      FindCallback(holder->ffi_table, kNewTextSpanHash));
  auto* newDecoratedBox = reinterpret_cast<NewDecoratedBoxFn>(
      FindCallback(holder->ffi_table, kNewDecoratedBoxHash));
  auto* newOpacity = reinterpret_cast<NewOpacityFn>(
      FindCallback(holder->ffi_table, kNewOpacityHash));
  auto* newScrollView = reinterpret_cast<NewScrollViewFn>(
      FindCallback(holder->ffi_table, kNewScrollViewHash));
  auto* newListView = reinterpret_cast<NewListViewFn>(
      FindCallback(holder->ffi_table, kNewListViewHash));
  auto* newGridView = reinterpret_cast<NewGridViewFn>(
      FindCallback(holder->ffi_table, kNewGridViewHash));
  auto* newDirectRenderWidget = reinterpret_cast<NewDirectRenderWidgetFn>(
      FindCallback(holder->ffi_table, kNewDirectRenderWidgetHash));
  auto* newListBuilder = reinterpret_cast<NewListBuilderFn>(
      FindCallback(holder->ffi_table, kNewListBuilderHash));
  auto* newGridBuilder = reinterpret_cast<NewGridBuilderFn>(
      FindCallback(holder->ffi_table, kNewGridBuilderHash));
  auto* update_children = reinterpret_cast<BuilderUpdateChildrenFn>(FindCallback(
      holder->ffi_table, builder->builder_kind == 1
                             ? kGridBuilderItemUpdateChildrenHash
                             : kListBuilderItemUpdateChildrenHash));
  auto* new_item = reinterpret_cast<NewListBuilderItemFn>(FindCallback(
      holder->ffi_table, builder->builder_kind == 1
                             ? kNewGridBuilderItemHash
                             : kNewListBuilderItemHash));
  auto* mark_needs_layout = reinterpret_cast<MarkYGNodeNeedsLayoutFn>(
      FindCallback(holder->ffi_table, kMarkYGNodeNeedsLayoutHash));
  if (newYGNode == nullptr || new_item == nullptr) return nullptr;

  Dart_Handle widget =
      BuildWidget(item, newYGNode, newYGConnector, newText, newTextSpan,
                  newDecoratedBox, newOpacity, newScrollView, newListView,
                  newGridView, newDirectRenderWidget, newListBuilder,
                  newGridBuilder, true);
  if (widget == nullptr || Dart_IsError(widget)) return nullptr;
  Dart_Handle children = Dart_NewList(1);
  Dart_ListSetAt(children, 0, widget);
  Dart_Handle updated = new_item(children, index);
  if (updated == nullptr || Dart_IsError(updated)) return nullptr;
  // Keep the item-level callback in the path even though Flutter rebuilds the
  // item through the builder's UpdateChildren below (the reconstructed widget
  // does not wire proxySetState for item-local updates).
  if (update_children != nullptr) update_children(updated, children);
  if (mark_needs_layout != nullptr) mark_needs_layout(updated);
  builder->builder_item_signatures[index] = NodeContentSignature(item);
  SKY_LOG(kDebug, kLogRuntime, "%s: #%u index=%d",
          builder->builder_kind == 1 ? "GridBuilderItemUpdateChildren"
                                     : "ListBuilderItemUpdateChildren",
          builder->id, index);
  return updated;
}

// Applies post-attach builder metadata/item changes without rebuilding the
// whole attached subtree.  Returns true when at least one callback was issued.
bool SyncAppServiceBuilders(RuntimeHolder* holder) {
  if (g_page == nullptr || holder == nullptr) return false;
  AppNode* root = FindNode(g_page->root_id);
  if (root != nullptr) ResolveStyles(root, nullptr);
  if (root != nullptr) ComputeInlineWhitespace(root);
  bool changed = false;
  std::vector<AppNode*> builders;
  builders.reserve(g_builders_by_address.size());
  for (const auto& entry : g_builders_by_address) {
    if (entry.second != nullptr) builders.push_back(entry.second);
  }
  for (AppNode* builder : builders) {
    if (builder == nullptr || builder->released || builder->builder_p == nullptr) {
      continue;
    }
    Dart_Handle builder_widget =
        Dart_HandleFromPersistent(builder->builder_p);
    const bool grid = builder->builder_kind == 1;

    const int child_count = static_cast<int>(
        AttrNumber(builder, "childCount",
                   AttrNumber(builder, "child-count",
                              static_cast<double>(
                                  builder->builder_synced_child_count))));
    if (child_count != builder->builder_synced_child_count) {
      auto* update = reinterpret_cast<BuilderUpdateIntFn>(FindCallback(
          holder->ffi_table, grid ? kGridBuilderUpdateChildCountHash
                                  : kListBuilderUpdateChildCountHash));
      if (update != nullptr) {
        update(builder_widget, child_count);
        builder->builder_child_count = child_count;
        builder->builder_synced_child_count = child_count;
        changed = true;
        SKY_LOG(kDebug, kLogRuntime, "%s: #%u childCount=%d",
                grid ? "GridBuilderUpdateChildCount"
                     : "ListBuilderUpdateChildCount",
                builder->id, child_count);
      }
    }

    if (grid) {
      const int cross = std::max(
          1, static_cast<int>(AttrNumber(builder, "crossAxisCount", 2.0)));
      if (cross != builder->builder_synced_cross_axis_count) {
        auto* update = reinterpret_cast<BuilderUpdateIntFn>(FindCallback(
            holder->ffi_table, kGridBuilderUpdateCrossAxisCountHash));
        if (update != nullptr) {
          update(builder_widget, cross);
          builder->builder_cross_axis_count = cross;
          builder->builder_synced_cross_axis_count = cross;
          changed = true;
          SKY_LOG(kDebug, kLogRuntime,
                  "GridBuilderUpdateCrossAxisCount: #%u count=%d", builder->id,
                  cross);
        }
      }
      const float main_spacing =
          static_cast<float>(GridMainGap(builder));
      if (std::fabs(main_spacing - builder->builder_synced_main_spacing) > 0.01f) {
        auto* update = reinterpret_cast<BuilderUpdateFloatFn>(FindCallback(
            holder->ffi_table, kGridBuilderUpdateMainAxisSpacingHash));
        if (update != nullptr) {
          update(builder_widget, main_spacing);
          builder->builder_main_spacing = main_spacing;
          builder->builder_synced_main_spacing = main_spacing;
          changed = true;
        }
      }
      const float cross_spacing =
          static_cast<float>(GridCrossGap(builder));
      if (std::fabs(cross_spacing - builder->builder_synced_cross_spacing) >
          0.01f) {
        auto* update = reinterpret_cast<BuilderUpdateFloatFn>(FindCallback(
            holder->ffi_table, kGridBuilderUpdateCrossAxisSpacingHash));
        if (update != nullptr) {
          update(builder_widget, cross_spacing);
          builder->builder_cross_spacing = cross_spacing;
          builder->builder_synced_cross_spacing = cross_spacing;
          changed = true;
        }
      }
      const float max_extent = static_cast<float>(
          AttrNumber(builder, "maxCrossAxisExtent", 0.0));
      if (std::fabs(max_extent - builder->builder_synced_max_cross_extent) >
          0.01f) {
        auto* update = reinterpret_cast<BuilderUpdateFloatFn>(FindCallback(
            holder->ffi_table, kGridBuilderUpdateMaxCrossAxisExtentHash));
        if (update != nullptr) {
          update(builder_widget, max_extent);
          builder->builder_max_cross_extent = max_extent;
          builder->builder_synced_max_cross_extent = max_extent;
          changed = true;
        }
      }
    } else {
      const float child_height =
          static_cast<float>(AttrNumber(builder, "childHeight", 0.0));
      if (std::fabs(child_height - builder->builder_synced_child_height) >
          0.01f) {
        auto* update = reinterpret_cast<BuilderUpdateFloatFn>(FindCallback(
            holder->ffi_table, kListBuilderUpdateChildHeightHash));
        if (update != nullptr) {
          update(builder_widget, child_height);
          builder->builder_child_height = child_height;
          builder->builder_synced_child_height = child_height;
          changed = true;
          SKY_LOG(kDebug, kLogRuntime,
                  "ListBuilderUpdateChildHeight: #%u height=%g", builder->id,
                  child_height);
        }
      }
    }

    const uint32_t background =
        builder->style.has_background ? builder->style.background : 0u;
    if (background != builder->builder_synced_background) {
      auto* update = reinterpret_cast<BuilderUpdateColorFn>(FindCallback(
          holder->ffi_table, grid ? kGridBuilderUpdateBackgroundColorHash
                                  : kListBuilderUpdateBackgroundColorHash));
      if (update != nullptr) {
        update(builder_widget, background);
        builder->builder_background = background;
        builder->builder_synced_background = background;
        changed = true;
      }
    }

    // Item content/index changes.  Metadata updates above keep Flutter's
    // sliver window in sync; content updates use the item-level callbacks.
    std::vector<int> indexes;
    indexes.reserve(builder->builder_items.size());
    for (const auto& entry : builder->builder_items) {
      indexes.push_back(entry.first);
    }
    std::sort(indexes.begin(), indexes.end());
    int content_updates = 0;
    std::vector<int> removed;
    for (int index : indexes) {
      auto item_handle = builder->builder_items.find(index);
      if (item_handle == builder->builder_items.end()) continue;
      AppNode* item = nullptr;
      auto node_entry = builder->builder_item_nodes.find(index);
      if (node_entry != builder->builder_item_nodes.end()) item = node_entry->second;
      if (item == nullptr || item->released) {
        item = FindBuilderItemNode(builder, index);
        if (item == nullptr) {
          removed.push_back(index);
          continue;
        }
        builder->builder_item_nodes[index] = item;
      }
      const int declared_index = static_cast<int>(
          AttrNumber(item, "index", static_cast<double>(index)));
      if (declared_index != index) {
        auto* update_index = reinterpret_cast<BuilderUpdateIntFn>(FindCallback(
            holder->ffi_table, grid ? kGridBuilderItemUpdateIndexHash
                                    : kListBuilderItemUpdateIndexHash));
        if (update_index != nullptr) {
          update_index(Dart_HandleFromPersistent(item_handle->second),
                       declared_index);
          SKY_LOG(kDebug, kLogRuntime, "%s: #%u index %d -> %d",
                  grid ? "GridBuilderItemUpdateIndex"
                       : "ListBuilderItemUpdateIndex",
                  builder->id, index, declared_index);
        }
      }
      const uint64_t signature = NodeContentSignature(item);
      auto signature_entry = builder->builder_item_signatures.find(index);
      if (signature_entry == builder->builder_item_signatures.end() ||
          signature_entry->second != signature) {
        Dart_Handle updated = RebuildBuilderItemContent(
            holder, builder, index, item,
            Dart_HandleFromPersistent(item_handle->second));
        if (updated != nullptr) {
          Dart_DeletePersistentHandle(item_handle->second);
          builder->builder_items[index] = Dart_NewPersistentHandle(updated);
          content_updates++;
          changed = true;
        }
      }
    }
    for (int index : removed) {
      auto item = builder->builder_items.find(index);
      if (item == builder->builder_items.end()) continue;
      auto* remove = reinterpret_cast<BuilderMutateChildFn>(FindCallback(
          holder->ffi_table, grid ? kGridBuilderRemoveChildHash
                                  : kListBuilderRemoveChildHash));
      if (remove != nullptr) {
        remove(builder_widget, Dart_HandleFromPersistent(item->second));
      }
      Dart_DeletePersistentHandle(item->second);
      builder->builder_items.erase(item);
      builder->builder_item_nodes.erase(index);
      builder->builder_item_signatures.erase(index);
      changed = true;
      SKY_LOG(kDebug, kLogRuntime, "%s: #%u remove index=%d",
              grid ? "GridBuilderRemoveChild" : "ListBuilderRemoveChild",
              builder->id, index);
    }
    if (content_updates > 0) {
      // Hand Flutter one consistent item list; this is the callback that makes
      // the rebuilt item widgets visible to the sliver.
      auto* update_children = reinterpret_cast<BuilderUpdateChildrenFn>(
          FindCallback(holder->ffi_table, grid
                                                ? kGridBuilderUpdateChildrenHash
                                                : kListBuilderUpdateChildrenHash));
      if (update_children != nullptr) {
        std::vector<Dart_Handle> handles;
        handles.reserve(builder->builder_items.size());
        for (int index : indexes) {
          auto item = builder->builder_items.find(index);
          if (item == builder->builder_items.end()) continue;
          handles.push_back(Dart_HandleFromPersistent(item->second));
        }
        Dart_Handle list =
            Dart_NewList(static_cast<intptr_t>(handles.size()));
        for (size_t i = 0; i < handles.size(); i++) {
          Dart_ListSetAt(list, static_cast<intptr_t>(i), handles[i]);
        }
        update_children(builder_widget, list);
        SKY_LOG(
            kDebug, kLogRuntime, "%s: #%u (%d item(s), %zu updated)",
            grid ? "GridBuilderUpdateChildren" : "ListBuilderUpdateChildren",
            builder->id, static_cast<int>(handles.size()),
            static_cast<size_t>(content_updates));
      }
    }
  }
  return changed;
}



}  // namespace appservice

