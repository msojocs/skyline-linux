// Page route materialization and live operation batches.

#include "internal/skyline_runtime_module.hh"
#include "internal/skyline_runtime_page.hh"

namespace appservice {

struct PageBuilders {
  NewYGNodeFn yg_node = nullptr;
  NewYGConnectorFn yg_connector = nullptr;
  NewTextFn text = nullptr;
  NewTextSpanFn text_span = nullptr;
  NewDecoratedBoxFn decorated = nullptr;
  NewOpacityFn opacity = nullptr;
  NewScrollViewFn scroll = nullptr;
  NewListViewFn list = nullptr;
  NewGridViewFn grid = nullptr;
  NewDirectRenderWidgetFn direct = nullptr;
  NewListBuilderFn list_builder = nullptr;
  NewGridBuilderFn grid_builder = nullptr;
  CreatePageRouteFn create_route = nullptr;
};

PageBuilders LoadPageBuilders(RuntimeHolder* holder) {
  PageBuilders builders;
  builders.yg_node = reinterpret_cast<NewYGNodeFn>(
      FindCallback(holder->ffi_table, kNewYGNodeHash));
  builders.yg_connector = reinterpret_cast<NewYGConnectorFn>(
      FindCallback(holder->ffi_table, kNewYGConnectorHash));
  builders.text =
      reinterpret_cast<NewTextFn>(FindCallback(holder->ffi_table, kNewTextHash));
  builders.text_span = reinterpret_cast<NewTextSpanFn>(
      FindCallback(holder->ffi_table, kNewTextSpanHash));
  builders.decorated = reinterpret_cast<NewDecoratedBoxFn>(
      FindCallback(holder->ffi_table, kNewDecoratedBoxHash));
  builders.opacity = reinterpret_cast<NewOpacityFn>(
      FindCallback(holder->ffi_table, kNewOpacityHash));
  builders.scroll = reinterpret_cast<NewScrollViewFn>(
      FindCallback(holder->ffi_table, kNewScrollViewHash));
  builders.list = reinterpret_cast<NewListViewFn>(
      FindCallback(holder->ffi_table, kNewListViewHash));
  builders.grid = reinterpret_cast<NewGridViewFn>(
      FindCallback(holder->ffi_table, kNewGridViewHash));
  builders.direct = reinterpret_cast<NewDirectRenderWidgetFn>(
      FindCallback(holder->ffi_table, kNewDirectRenderWidgetHash));
  builders.list_builder = reinterpret_cast<NewListBuilderFn>(
      FindCallback(holder->ffi_table, kNewListBuilderHash));
  builders.grid_builder = reinterpret_cast<NewGridBuilderFn>(
      FindCallback(holder->ffi_table, kNewGridBuilderHash));
  builders.create_route = reinterpret_cast<CreatePageRouteFn>(
      FindCallback(holder->ffi_table, kCreatePageRouteHash));
  return builders;
}

// The page element is the `wx-x name=page` node under the attach host.  Its
// resolved CSS background is the route's content background (WeChat applies
// the window default there).  `page { background-color: transparent }` has to
// reach `SkylineScaffold`, otherwise a non-opaque custom route paints an
// opaque white sheet behind its page and the covered route stays hidden while
// the page translates during a back gesture.
AppNode* FindPageElement(AppNode* node) {
  if (node == nullptr) return nullptr;
  if (node->name == "page" || node->tag == "page") return node;
  for (AppNode* child : node->children) {
    if (child == nullptr) continue;
    if (AppNode* found = FindPageElement(child)) return found;
  }
  return nullptr;
}

// Builds and pushes one page route for an `attach` operation.  Called while
// replaying the captured operation stream, so the node table already contains
// the page's final tree.
bool MaterializeAttachedPage(RuntimeHolder* holder,
                             const PageBuilders& builders, uint32_t root_id,
                             uint8_t open_type, int32_t page_id,
                             int32_t callback_id) {
  if (g_page == nullptr || builders.create_route == nullptr) return false;
  AppNode* root = FindNode(root_id);
  if (root == nullptr) {
    std::fprintf(stderr,
                 "[skyline-runtime] appservice ops: no attach root (root=%u)\n",
                 root_id);
    return false;
  }
  if (builders.yg_node == nullptr || builders.yg_connector == nullptr ||
      builders.text == nullptr || builders.text_span == nullptr ||
      builders.decorated == nullptr || builders.opacity == nullptr) {
    std::fprintf(stderr,
                 "[skyline-runtime] appservice ops: callbacks not registered\n");
    return false;
  }

  // Custom routes (`wx.router.addRouteBuilder` + `routeType`) attach a `host`
  // wrapper containing the page root.  `SkylineCupertinoPageTransition` casts
  // the host to a YGNode, replaces its connector children with the route
  // `child` (built from `root`), and registers the JS route builder's animation
  // worklets on the host view tag.  So the host widget is the animation carrier
  // and `root` stays the page tree that incremental updates rebuild.
  OpPage::CustomRouteConfig& custom = g_page->custom_route;
  AppNode* host = custom.pending ? FindNode(g_page->attach_host_id) : nullptr;
  const bool is_custom = custom.pending && custom.is_custom && host != nullptr;

  // The host contains the root, so laying out the host also lays out the page.
  AppNode* layout_root = is_custom ? host : root;
  ResolveStyles(layout_root, nullptr);
  ComputeInlineWhitespace(layout_root);
  ClearSliverLayoutMarks();
  const float viewport_w =
      g_viewport_width > 0.0 ? static_cast<float>(g_viewport_width) : 320.0f;
  const float viewport_h =
      g_viewport_height > 0.0 ? static_cast<float>(g_viewport_height) : 240.0f;
  PrepareLayoutBuffers(layout_root);
  MeasureNode(layout_root, viewport_w, true, viewport_h, true);
  layout_root->measured_w = viewport_w;
  layout_root->measured_h = viewport_h;
  std::vector<NodeMetric> metrics;
  LayoutNode(layout_root, 0.0f, 0.0f, viewport_w, viewport_h, &metrics);
  WriteLayoutMetrics(metrics);

  Dart_Handle host_widget = Dart_Null();
  if (is_custom) {
    // Built first: the host connector must exist for the transition; the
    // route replaces its children with the (second) root build below.
    ResetDeferredFixedNodes();
    g_fixed_root_node = root;
    g_collect_fixed_nodes = true;
    host_widget =
        BuildWidget(host, builders.yg_node, builders.yg_connector, builders.text,
                    builders.text_span, builders.decorated, builders.opacity,
                    builders.scroll, builders.list, builders.grid,
                    builders.direct, builders.list_builder,
                    builders.grid_builder, true);
    g_collect_fixed_nodes = false;
    ResetDeferredFixedNodes();
    if (host_widget == nullptr || Dart_IsError(host_widget)) {
      std::fprintf(stderr, "[skyline-runtime] appservice ops: host build failed\n");
      return false;
    }
  }

  ResetDeferredFixedNodes();
  g_fixed_root_node = root;
  g_collect_fixed_nodes = true;
  Dart_Handle root_widget =
      BuildWidget(root, builders.yg_node, builders.yg_connector,
                  builders.text, builders.text_span, builders.decorated,
                  builders.opacity, builders.scroll, builders.list,
                  builders.grid, builders.direct, builders.list_builder,
                  builders.grid_builder, true);
  g_collect_fixed_nodes = false;
  ResetDeferredFixedNodes();
  if (root_widget == nullptr || Dart_IsError(root_widget)) {
    std::fprintf(stderr, "[skyline-runtime] appservice ops: build failed\n");
    return false;
  }

  static char page_url[] = "pages/appservice";
  static SkylinePageConfig config;
  uint32_t page_background = 0xFFFFFFFFu;
  if (AppNode* page_element = FindPageElement(layout_root)) {
    if (page_element->style.has_background) {
      page_background = page_element->style.background;
    }
  }
  config.backgroundColorContent = page_background;
  config.pageUrl = page_url;
  const uint32_t previous_root_id = g_page->attach_previous_root_id;
  const int32_t previous_page_id = g_page->current_page_id;
  Dart_Handle empty = Dart_NewStringFromCString("");
  Dart_Handle animation_hash = empty;
  Dart_Handle secondary_animation_hash = empty;
  Dart_Handle barrier_label = empty;
  uint32_t barrier_color = 0;
  uint8_t has_barrier_color = 0;
  if (is_custom) {
    if (!custom.animation_hash.empty()) {
      animation_hash = Dart_NewStringFromCString(custom.animation_hash.c_str());
    }
    if (!custom.secondary_animation_hash.empty()) {
      secondary_animation_hash =
          Dart_NewStringFromCString(custom.secondary_animation_hash.c_str());
    }
    if (!custom.barrier_label.empty()) {
      barrier_label = Dart_NewStringFromCString(custom.barrier_label.c_str());
    }
    barrier_color = custom.barrier_color;
    has_barrier_color = custom.has_barrier_color ? 1 : 0;
  }
  g_page->root_id = root->id;
  g_page->current_page_id = page_id;
  g_page->page_count++;
  std::fprintf(stderr,
               "[skyline-runtime] appservice page %d -> CreatePageRoute "
               "(openType=%u, %gx%g, custom=%d, bg=%08x)\n",
               g_page->page_count, static_cast<unsigned>(open_type),
               static_cast<double>(viewport_w), static_cast<double>(viewport_h),
               is_custom ? 1 : 0, page_background);
  builders.create_route(
      callback_id, open_type,
      is_custom ? 1 : 0,
      open_type == 1 /* kSwitchTab */ ? 1 : 0, page_id,
      is_custom ? (custom.opaque ? 1 : 0) : 1,
      is_custom ? (custom.maintain_state ? 1 : 0) : 1,
      is_custom ? static_cast<uint32_t>(custom.transition_ms) : 0u,
      is_custom ? static_cast<uint32_t>(custom.reverse_transition_ms) : 0u,
      barrier_color, has_barrier_color,
      is_custom && custom.barrier_dismissible ? 1 : 0, barrier_label,
      is_custom ? (custom.can_transition_to ? 1 : 0) : 1,
      is_custom ? (custom.can_transition_from ? 1 : 0) : 1,
      0 /* forceSecondaryAnimation */,
      is_custom ? 1 : 0 /* popGestureEnabled */,
      0 /* allowEnterRouteSnapshotting */,
      0 /* allowExitRouteSnapshotting */,
      0 /* fullscreenDrag */,
      0 /* dismissDirection (horizontal) */,
      animation_hash, secondary_animation_hash, host_widget, root_widget,
      &config);

  // The config only applies to the page being attached.
  custom = OpPage::CustomRouteConfig();

  // Keep the covered page's widget handles so `navigateBack` can rebuild the
  // route that becomes visible again.  Only the newest page's handles are
  // cleared for the next materialize call.
  if (g_page->root_node != nullptr || g_page->root_connector != nullptr) {
    g_page->page_stack.push_back(OpPage::PageEntry{
        previous_root_id, g_page->root_node, g_page->root_connector,
        previous_page_id});
    g_page->root_node = nullptr;
    g_page->root_connector = nullptr;
  }
  g_page->root_node = Dart_NewPersistentHandle(root_widget);
  if (root->connector != nullptr) {
    g_page->root_connector = Dart_NewPersistentHandle(root->connector);
  }
  PublishNodeRects();
  SnapshotNode(root);
  if (std::getenv("SKYLINE_APP_SERVICE_DEBUG") != nullptr) {
    DebugDumpNode(layout_root, 0);
    for (const NodeMetric& metric : metrics) {
      std::fprintf(stderr,
                   "[ops-debug] metrics #%u -> top=%g left=%g w=%g h=%g\n",
                   metric.node->id, static_cast<double>(metric.y),
                   static_cast<double>(metric.x), static_cast<double>(metric.w),
                   static_cast<double>(metric.h));
    }
  }
  return true;
}

void ApplyOpsPage(RuntimeHolder* holder, const char* descriptor) {
  const PageBuilders builders = LoadPageBuilders(holder);
  if (builders.yg_node == nullptr || builders.yg_connector == nullptr ||
      builders.create_route == nullptr || builders.text == nullptr ||
      builders.text_span == nullptr || builders.decorated == nullptr ||
      builders.opacity == nullptr) {
    std::fprintf(stderr,
                 "[skyline-runtime] appservice ops: callbacks not registered\n");
    return;
  }
  auto* navigateBack = reinterpret_cast<NavigateBackFn>(
      FindCallback(holder->ffi_table, kNavigateBackHash));
  auto* switchTab = reinterpret_cast<SwitchTabFn>(
      FindCallback(holder->ffi_table, kSwitchTabHash));

  std::string json;
  if (!ReadDescriptor(descriptor, &json)) {
    std::fprintf(stderr, "[skyline-runtime] appservice ops: cannot read %s\n",
                 descriptor);
    return;
  }
  g_ops_document = std::make_unique<rapidjson::Document>();
  rapidjson::Document& doc = *g_ops_document;
  doc.Parse(json.c_str());
  if (doc.HasParseError() || !doc.IsObject()) {
    std::fprintf(stderr,
                 "[skyline-runtime] appservice ops: parse error at %zu\n",
                 static_cast<size_t>(doc.GetErrorOffset()));
    return;
  }
  const auto ops_member = doc.FindMember("ops");
  if (ops_member == doc.MemberEnd() || !ops_member->value.IsArray()) {
    std::fprintf(stderr, "[skyline-runtime] appservice ops: missing ops array\n");
    return;
  }

  g_page = new OpPage();
  ClearTransitions();
  g_page->holder = holder;
  g_ui_task_runner = UiTaskRunner();
  g_next_rule_order = 0;
  g_next_address = 0x5000;
  g_builders_by_address.clear();
  g_image_nodes.clear();
  g_scroll_view_ids.clear();
  g_swiper_ids.clear();
  g_picker_column_ids.clear();
  g_selection_ids.clear();
  g_node_address_ids.clear();
  g_intersection_unique_nodes.clear();
  g_intersection_node_observers.clear();
  bool attach_pending = false;
  for (const rapidjson::Value& op : ops_member->value.GetArray()) {
    ApplyOperation(op);
    const char* kind = MemberText(op, "op");
    if (std::strcmp(kind, "attach") == 0) {
      // `attachCustomRoute` follows `attach` in the SDK's op stream; defer the
      // build until the whole batch is applied so the config is visible.
      attach_pending = true;
    } else if (std::strcmp(kind, "notifyNavigateBack") == 0) {
      if (g_page->page_count > 0 && navigateBack != nullptr) {
        navigateBack(g_page->pending_back_delta,
                     g_page->pending_back_animated ? 1 : 0, 0);
        std::fprintf(stderr,
                     "[skyline-runtime] NavigateBack: delta=%d animated=%d\n",
                     g_page->pending_back_delta,
                     g_page->pending_back_animated ? 1 : 0);
      }
      g_page->pending_open_type = 0;
    } else if (std::strcmp(kind, "notifySwitchTab") == 0) {
      if (g_page->page_count > 0 && switchTab != nullptr) {
        switchTab(g_page->pending_page_id, 0);
        std::fprintf(stderr, "[skyline-runtime] SwitchTab: page=%d\n",
                     g_page->pending_page_id);
        g_page->pending_open_type = 0;
      }
    }
  }
  if (attach_pending) {
    // The whole batch is applied now, including the trailing custom route
    // config; hoist detached portals and flatten late fragments first.
    if (AppNode* root = FindNode(g_page->root_id)) {
      AttachDetachedPortalContent(root);
      FlattenFragments(root);
    }
    const uint8_t open_type = g_page->pending_open_type != 0
                                  ? g_page->pending_open_type
                                  : (g_page->page_count == 0 ? 0 : 4);
    const int32_t page_id =
        g_page->pending_page_id != 0 ? g_page->pending_page_id
                                     : (g_page->page_count == 0 ? 0 : 1);
    MaterializeAttachedPage(holder, builders, g_page->root_id, open_type,
                            page_id, g_page->pending_callback_id);
    g_page->pending_open_type = 0;
    g_page->pending_page_id = 0;
    g_page->pending_callback_id = 0;
  }
  if (g_page->page_count == 0 && g_page->root_id != 0) {
    if (AppNode* root = FindNode(g_page->root_id)) {
      AttachDetachedPortalContent(root);
      FlattenFragments(root);
    }
    MaterializeAttachedPage(holder, builders, g_page->root_id, 0, 0, 0);
  }
  if (g_page->page_count == 0) {
    std::fprintf(stderr,
                 "[skyline-runtime] appservice ops: no attach root (root=%u)\n",
                 g_page->root_id);
    return;
  }
  SchedulePendingAnimatedStyles(holder);
  if (std::getenv("SKYLINE_APP_SERVICE_DEBUG") != nullptr) {
    DebugDumpNode(FindNode(g_page->root_id), 0);
  }
  std::fprintf(stderr,
               "[skyline-runtime] appservice ops: %d ops, %u nodes -> "
               "CreatePageRoute (%d page(s))\n",
               g_page->applied_ops,
               static_cast<unsigned>(g_page->nodes.size()), g_page->page_count);

  // @font-face requests go through the real Dart font pipeline:
  // LoadFontFace -> ResourceLoader.loadResourceAsync -> LoadResourceAsync ->
  // NotifyResourceLoad -> Flutter FontLoader -> NotifyFontFaceLoad.
  if (!g_page->fonts.empty()) {
    auto* load_font_face = reinterpret_cast<LoadFontFaceFn>(
        FindCallback(holder->ffi_table, kLoadFontFaceHash));
    static int32_t font_callback_id = 0;
    for (const FontFaceRequest& font : g_page->fonts) {
      if (load_font_face == nullptr) break;
      Dart_Handle family = Dart_NewStringFromCString(font.family.c_str());
      Dart_Handle urls = Dart_NewList(1);
      Dart_Handle url = Dart_NewStringFromCString(font.url.c_str());
      if (Dart_IsError(family) || Dart_IsError(urls) || Dart_IsError(url)) {
        continue;
      }
      Dart_ListSetAt(urls, 0, url);
      std::fprintf(stderr, "[skyline-runtime] appservice font face: %s -> %s\n",
                   font.family.c_str(), font.url.c_str());
      const int32_t callback_id = ++font_callback_id;
      g_font_callback_families[static_cast<uint32_t>(callback_id)] =
          font.family;
      g_font_status[font.family] = -1;
      load_font_face(family, urls, 1000, callback_id);
    }
  }

  // `updates` batches replay the appservice flush loop: apply a group of
  // operations on the UI task runner, then update the attached tree.
  const auto updates_member = doc.FindMember("updates");
  if (updates_member != doc.MemberEnd() && updates_member->value.IsArray()) {
    auto task_runner = UiTaskRunner();
    for (const rapidjson::Value& batch : updates_member->value.GetArray()) {
      if (!batch.IsObject()) continue;
      const auto ops_member = batch.FindMember("ops");
      if (ops_member == batch.MemberEnd() || !ops_member->value.IsArray()) {
        continue;
      }
      const int delay_ms =
          static_cast<int>(MemberNumber(batch, "afterMs", 300.0f));
      const rapidjson::Value* ops = &ops_member->value;
      if (!task_runner) continue;
      task_runner->PostDelayedTask(
          [holder, ops, builders, navigateBack, switchTab]() {
            if (g_page == nullptr ||
                (Dart_CurrentIsolate() == nullptr && g_isolate == nullptr)) {
              return;
            }
            DemoPageApiScope scope;
            bool structural = false;
            bool needs_flush = false;
            bool created_route = false;
            bool builder_structural_only = true;
            bool general_dirty = false;
            bool attach_pending = false;
            for (const rapidjson::Value& op : ops->GetArray()) {
              ApplyOperation(op);
              const auto kind = op.FindMember("op");
              if (kind == op.MemberEnd() || !kind->value.IsString()) continue;
              const char* name = kind->value.GetString();
              if (std::strcmp(name, "attach") == 0) {
                // The SDK emits `attachCustomRoute` *after* `attach` in the
                // same batch.  Materialising inside the loop would build a
                // plain route and leave the custom config pending for the next
                // navigation (stale host + broken second page).  Defer until
                // the whole batch has been applied.
                attach_pending = true;
              } else if (std::strcmp(name, "notifyNavigateBack") == 0) {
                if (navigateBack != nullptr && g_page->page_count > 0) {
                  navigateBack(g_page->pending_back_delta,
                               g_page->pending_back_animated ? 1 : 0, 0);
                  std::fprintf(
                      stderr,
                      "[skyline-runtime] NavigateBack: delta=%d animated=%d\n",
                      g_page->pending_back_delta,
                      g_page->pending_back_animated ? 1 : 0);
                }
                g_page->pending_open_type = 0;
              } else if (std::strcmp(name, "notifySwitchTab") == 0) {
                if (switchTab != nullptr && g_page->page_count > 0) {
                  switchTab(g_page->pending_page_id, 0);
                  std::fprintf(stderr, "[skyline-runtime] SwitchTab: page=%d\n",
                               g_page->pending_page_id);
                  g_page->pending_open_type = 0;
                }
              } else if (std::strcmp(name, "dispatchTouch") != 0 &&
                         std::strcmp(name, "startRender") != 0 &&
                         !IsAutomationOpKind(name)) {
                if (IsStructuralOpKind(name)) {
                  structural = true;
                  if (std::strcmp(name, "createElement") != 0) {
                    AppNode* target = FindNode(MemberUint(op, "id", 0));
                    if (target == nullptr || !IsBuilderSubtree(target)) {
                      builder_structural_only = false;
                    }
                  }
                } else {
                  AppNode* target = FindNode(MemberUint(op, "id", 0));
                  if (target != nullptr && IsBuilderSubtree(target)) {
                    // Builder sync handles this; do not mark the batch as a
                    // general style/text change.
                  } else {
                    general_dirty = true;
                  }
                }
                needs_flush = true;
              }
            }
            if (attach_pending) {
              const uint8_t open_type = g_page->pending_open_type != 0
                                            ? g_page->pending_open_type
                                            : 4;
              const int32_t page_id =
                  g_page->pending_page_id != 0 ? g_page->pending_page_id : 1;
              if (MaterializeAttachedPage(holder, builders, g_page->root_id,
                                          open_type, page_id,
                                          g_page->pending_callback_id)) {
                created_route = true;
              }
              g_page->pending_open_type = 0;
              g_page->pending_page_id = 0;
              g_page->pending_callback_id = 0;
            }
            const bool synced = SyncAppServiceBuilders(holder);
            if (g_page->force_rebuild) {
              structural = true;
              g_page->force_rebuild = false;
            }
            if (structural && builder_structural_only && synced) {
              structural = false;
            }
            if (needs_flush && !created_route) {
              if (!structural && !general_dirty && synced) {
                // The builder callbacks already applied the entire batch.
              } else {
                if (structural) {
                  ClearTransitions();
                } else {
                  StartTransitions(g_page->root_id);
                }
                SchedulePendingAnimatedStyles(holder);
                FlushAppServicePage(holder, structural);
              }
            }
          },
          fml::TimeDelta::FromMilliseconds(delay_ms));
      std::fprintf(stderr,
                   "[skyline-runtime] appservice update in %d ms (%u ops)\n",
                   delay_ms, static_cast<unsigned>(ops->GetArray().Size()));
    }
  }
}

// Applies one live bridge batch to a persistent appservice page.
//
// The SkylineGlobal.PageContext bridge flushes deltas: the first batches may
// only create elements, a later batch attaches the tree, and subsequent
// batches update it.  Rebuilding from each batch (as ApplyOpsPage does for a
// recorded fixture) loses the nodes created by earlier batches and leaves the
// page slot occupied, so keep `g_page` alive across calls and materialise the
// route only when an `attach` op arrives.
void ApplyLiveOpsBatch(RuntimeHolder* holder, const char* descriptor) {
  const PageBuilders builders = LoadPageBuilders(holder);
  if (builders.yg_node == nullptr || builders.yg_connector == nullptr ||
      builders.create_route == nullptr || builders.text == nullptr ||
      builders.text_span == nullptr || builders.decorated == nullptr ||
      builders.opacity == nullptr) {
    std::fprintf(stderr,
                 "[skyline-runtime] live ops: callbacks not registered\n");
    return;
  }
  std::string json;
  if (!ReadDescriptor(descriptor, &json)) {
    std::fprintf(stderr, "[skyline-runtime] live ops: cannot read %s\n",
                 descriptor);
    return;
  }
  g_ops_document = std::make_unique<rapidjson::Document>();
  rapidjson::Document& doc = *g_ops_document;
  doc.Parse(json.c_str());
  if (doc.HasParseError() || !doc.IsObject()) {
    std::fprintf(stderr, "[skyline-runtime] live ops: parse error at %zu\n",
                 static_cast<size_t>(doc.GetErrorOffset()));
    return;
  }
  const auto ops_member = doc.FindMember("ops");
  if (ops_member == doc.MemberEnd() || !ops_member->value.IsArray()) {
    std::fprintf(stderr, "[skyline-runtime] live ops: missing ops array\n");
    return;
  }

  const bool first_batch = g_page == nullptr;
  if (first_batch) {
    g_page = new OpPage();
    ClearTransitions();
    g_page->holder = holder;
    g_ui_task_runner = UiTaskRunner();
    g_next_rule_order = 0;
    g_next_address = 0x5000;
    g_builders_by_address.clear();
    g_image_nodes.clear();
    g_scroll_view_ids.clear();
    g_swiper_ids.clear();
    g_picker_column_ids.clear();
    g_selection_ids.clear();
    g_node_address_ids.clear();
    g_intersection_unique_nodes.clear();
    g_intersection_node_observers.clear();
  }

  bool structural = false;
  bool needs_flush = false;
  bool created_route = false;
  bool general_dirty = false;
  bool attach_pending = false;
  for (const rapidjson::Value& op : ops_member->value.GetArray()) {
    ApplyOperation(op);
    const char* kind = MemberText(op, "op");
    if (kind == nullptr) continue;
    if (std::strcmp(kind, "attach") == 0) {
      // The SDK emits `attachCustomRoute` *after* `attach` in the same flush
      // batch.  Materialising here would build a plain route and leave the
      // custom config pending, so the next navigation would reuse this page's
      // stale host and render nothing.  Defer until the batch is applied.
      attach_pending = true;
    } else if (std::strcmp(kind, "notifyNavigateBack") == 0) {
      auto* navigateBack = reinterpret_cast<NavigateBackFn>(
          FindCallback(holder->ffi_table, kNavigateBackHash));
      if (g_page->page_count > 0 && navigateBack != nullptr) {
        navigateBack(g_page->pending_back_delta,
                     g_page->pending_back_animated ? 1 : 0, 0);
      }
      g_page->pending_open_type = 0;
    } else if (std::strcmp(kind, "notifySwitchTab") == 0) {
      auto* switchTab = reinterpret_cast<SwitchTabFn>(
          FindCallback(holder->ffi_table, kSwitchTabHash));
      if (g_page->page_count > 0 && switchTab != nullptr) {
        switchTab(g_page->pending_page_id, 0);
      }
      g_page->pending_open_type = 0;
    } else if (std::strcmp(kind, "startRender") != 0 &&
               !IsAutomationOpKind(kind)) {
      needs_flush = true;
      if (IsStructuralOpKind(kind)) {
        structural = true;
      } else {
        general_dirty = true;
      }
    }
  }

  if (attach_pending) {
    // The whole batch is applied now, including a trailing
    // `attachCustomRoute`, so a custom route sees its config and host.
    // Detached root-portal content is hoisted to the page root and late-filled
    // fragments are flattened before building.
    if (AppNode* root = FindNode(g_page->root_id)) {
      AttachDetachedPortalContent(root);
      FlattenFragments(root);
    }
    const uint8_t open_type = g_page->pending_open_type != 0
                                  ? g_page->pending_open_type
                                  : (g_page->page_count == 0 ? 0 : 4);
    const int32_t page_id =
        g_page->pending_page_id != 0 ? g_page->pending_page_id
                                     : (g_page->page_count == 0 ? 0 : 1);
    if (MaterializeAttachedPage(holder, builders, g_page->root_id, open_type,
                                page_id, g_page->pending_callback_id)) {
      created_route = true;
    }
    g_page->pending_open_type = 0;
    g_page->pending_page_id = 0;
    g_page->pending_callback_id = 0;
  }

  if (g_page->force_rebuild) {
    structural = true;
    g_page->force_rebuild = false;
  }
  // Fragments spliced before their children were appended become real children
  // only now; flattening changes the tree, so force a structural rebuild.
  if (g_page->root_id != 0) {
    if (AppNode* root = FindNode(g_page->root_id)) {
      const bool portal_changed = AttachDetachedPortalContent(root);
      if (FlattenFragments(root) || portal_changed) {
        needs_flush = true;
        structural = true;
      }
    }
  }
  // A live batch often attaches early (the page shell) and then appends the
  // list/content in the remaining thousands of ops.  MaterializeAttachedPage
  // only sees the tree at the attach op, so flush the post-attach changes as
  // well instead of skipping the update when a route was created.
  if (needs_flush && g_page->page_count > 0) {
    if (structural) {
      ClearTransitions();
    } else {
      StartTransitions(g_page->root_id);
    }
    SchedulePendingAnimatedStyles(holder);
    FlushAppServicePage(holder, structural);
  }
  std::fprintf(stderr,
               "[skyline-runtime] live ops: %u ops (first=%d structural=%d "
               "flush=%d route=%d pages=%d)\n",
               static_cast<unsigned>(ops_member->value.GetArray().Size()),
               first_batch ? 1 : 0, structural ? 1 : 0, needs_flush ? 1 : 0,
               created_route ? 1 : 0, g_page->page_count);
}

// Synthetic keyboard input: append/delete text on the focused node and push
// the change through the normal fine-grained update path.
// Scrolls a materialized scroll view to `top` (UI thread).  Used by the
// DevTools `scroll-into-view` bridge once the appservice component property
// changes.
void ScrollNodeTo(uint32_t node_id, double top) {
  if (g_page == nullptr || g_page->holder == nullptr) return;
  AppNode* node = FindNode(node_id);
  if (node == nullptr || node->scroll_view_p == nullptr) {
    if (std::getenv("SKYLINE_APP_SERVICE_DEBUG") != nullptr) {
      std::fprintf(stderr,
                   "[skyline-runtime] scrollNodeTo #%u: %s\n", node_id,
                   node == nullptr ? "no node" : "no scroll widget");
    }
    return;
  }
  auto* update = reinterpret_cast<ScrollViewUpdateScrollTopFn>(FindCallback(
      g_page->holder->ffi_table, kScrollViewUpdateScrollTopHash));
  if (update == nullptr) return;
  // Persist the requested offset: a following rebuild recreates the scroll
  // widget from `scrollTop`, and without this the page snapped back to the
  // top whenever the appservice batched a class/style update after the jump.
  char buffer[64];
  std::snprintf(buffer, sizeof(buffer), "%g", top);
  node->attrs["scrollTop"] = buffer;
  if (std::getenv("SKYLINE_APP_SERVICE_DEBUG") != nullptr) {
    std::fprintf(stderr, "[skyline-runtime] scrollNodeTo #%u -> %g\n", node_id,
                 top);
  }
  update(Dart_HandleFromPersistent(node->scroll_view_p), top);
  // The native scroll callback may not fire for a programmatic jump on every
  // host; republish the viewport rects here as well.
  PublishNodeRects();
}

void HandleKeyInput(RuntimeHolder* holder, int32_t kind, uint32_t logical,
                    const std::string& text) {  if (g_page == nullptr || kind == 1 /* key up */) return;
  AppNode* input = FindNode(g_page->focused_input_id);
  if (input == nullptr) {
    std::fprintf(stderr,
                 "[skyline-runtime] key event ignored: no focused input "
                 "(kind=%d logical=%u)\n",
                 static_cast<int>(kind), logical);
    return;
  }
  ClampInputState(input);
  int start = input->input_selection_start;
  int end = input->input_selection_end;
  if (start > end) std::swap(start, end);
  if (start == end) start = end = input->input_cursor;
  const bool has_selection = start != end;
  const int length = Utf8CodepointCount(input->text);
  auto erase_selection = [&]() {
    if (has_selection) {
      Utf8Erase(&input->text, start, end);
      input->input_cursor = start;
    }
  };

  bool changed = false;
  bool handled = true;
  switch (logical) {
    case 0x00000008u:  // backspace
      if (has_selection) {
        erase_selection();
        changed = true;
      } else if (input->input_cursor > 0) {
        Utf8Erase(&input->text, input->input_cursor - 1,
                  input->input_cursor);
        input->input_cursor--;
        changed = true;
      }
      break;
    case 46u:  // delete
      if (has_selection) {
        erase_selection();
        changed = true;
      } else if (input->input_cursor < length) {
        Utf8Erase(&input->text, input->input_cursor,
                  input->input_cursor + 1);
        changed = true;
      }
      break;
    case 37u:  // left
      input->input_cursor = has_selection ? start : std::max(0, start - 1);
      changed = true;
      break;
    case 38u:  // up: move to the start of the value
      input->input_cursor = 0;
      changed = true;
      break;
    case 39u:  // right
      input->input_cursor = has_selection ? end : std::min(length, end + 1);
      changed = true;
      break;
    case 40u:  // down: move to the end of the value
      input->input_cursor = length;
      changed = true;
      break;
    case 36u:  // home
      input->input_cursor = 0;
      changed = true;
      break;
    case 35u:  // end
      input->input_cursor = length;
      changed = true;
      break;
    case 13u:  // enter
      if (!input->input_hold_keyboard) {
        if (input->input_widget_p != nullptr && g_page->holder != nullptr) {
          auto* focus = reinterpret_cast<InputFocusFn>(FindCallback(
              g_page->holder->ffi_table, kInputFocusHash));
          if (focus != nullptr) {
            focus(Dart_HandleFromPersistent(input->input_widget_p), 0);
          }
        }
        std::fprintf(stderr,
                     "[skyline-runtime] key input: id=%u confirm, "
                     "InputHoldKeyboard hold=0\n",
                     input->id);
        g_page->focused_input_id = 0;
        return;
      }
      changed = true;
      break;
    case 27u:  // escape
      g_page->focused_input_id = 0;
      return;
    default:
      if (!text.empty()) {
        erase_selection();
        const int position = input->input_cursor;
        Utf8Insert(&input->text, position, text);
        input->input_cursor = position + Utf8CodepointCount(text);
        input->composing = false;
        input->composition_base_text.clear();
        changed = true;
      } else {
        handled = false;
      }
      break;
  }
  if (!handled) return;
  CollapseInputSelection(input);
  ClampInputState(input);
  if (!changed) return;
  NotifyInputState(input);
  std::fprintf(stderr,
               "[skyline-runtime] key input: id=%u value=\"%s\" cursor=%d "
               "selection=[%d,%d]\n",
               input->id, input->text.c_str(), input->input_cursor,
               input->input_selection_start, input->input_selection_end);
  FlushAppServicePage(holder, false);
}

}  // namespace appservice
