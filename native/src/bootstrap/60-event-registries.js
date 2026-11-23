  // Scroll event callbacks registered by the builtin scroll-view component.
  // The keys are backend view tags; native scroll events carry the same tag.
  const scrollEventCallbacks = new Map();
  const scrollStartEventCallbacks = new Map();
  const scrollEndEventCallbacks = new Map();
  // Swiper's `onScrollEvent` carries a single translate-pixel value (and its
  // worklet name is `onTransition`), unlike scroll-view's six-value callback.
  const swiperScrollEventCallbacks = new Map();
  // Last reported offset per scroll view (selector-query `getScrollOffset`).
  const scrollOffsets = new Map();
  // Picker/input events for CDP inspection and regression tests.
  const pickerEvents = [];
  const inputEvents = [];
  const intersectionEvents = [];
  const selectionEvents = [];
  const refresherEvents = [];
  const navigateBackIntercepts = [];
  // D10: accessibility / automation events reported by the native runtime.
  const accessibilityEvents = [];
  const snapshotEvents = [];
  const userGestureEvents = [];
  // `<share-element>` hero frame callbacks (native HandleHeroOnFrame).
  const heroFrameEvents = [];
  // Backend elements for share-element components; used to recover WXML props
  // that glass-easel observers saw before the backend element existed.
  const heroElements = new Map();
  const heroAttributeSignature = new WeakMap();
  // Refresher callbacks registered by the builtin scroll-view component.
  const refresherPullingCallbacks = new Map();
  const refresherWillRefreshCallbacks = new Map();
  const refresherRefreshCallbacks = new Map();
  const refresherRestoreCallbacks = new Map();
  const refresherAbortCallbacks = new Map();
  const refresherStatusCallbacks = new Map();
  let nextIntersectionId = 0;
  // All mirrored backend elements by view tag (native events carry the tag).
  const bridgeElements = new Map();

