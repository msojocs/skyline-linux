  // The miniprogram SDK builds worklets with `module.createWorklet(fn)()`,
  // expecting the wrapper to return a callable.  A plain autoStub returns a
  // number there, which breaks component creation before the page tree is
  // built.  Returning the input function keeps the JS-side worklet callable.
  // `wx.worklet.shared(initial)` goes through makeMutable and the component
  // reads/writes `.value` (address-book's property observer does this), so the
  // mutable stub has to behave like the JS-thread fallback: a value holder.
  // `wx.worklet.shared(initial)` goes through makeMutable and the component
  // reads/writes `.value`.  Assigning a `timing()`/`spring()` updater starts
  // the animation: the official runtime drives `onFrame` until it reports
  // completion and then invokes the animation callback.  Custom routes depend
  // on that callback (`didPop()` runs when the drag-release animation ends).
  function workletMutable(initial) {
    let current = initial;
    let animation = null;
    const listeners = new Set();
    function notify() {
      for (const listener of listeners) {
        try { listener(); } catch (_error) {}
      }
    }
    function startAnimation(descriptor) {
      const previousAnimation = animation;
      animation = descriptor;
      const startValue = current;
      const startTime = Date.now();
      // Reanimated's animation contract: `onStart(animation, value, now,
      // previousAnimation)` seeds `startValue`/`startTime`, then
      // `onFrame(animation, now)` advances `animation.current`.  Calling
      // either with the wrong arity leaves a `timing()` with NaN progress,
      // which collapses the animation into an instant jump to the target
      // (half-screen's open/close sheet never slid).
      if (typeof descriptor.onStart === 'function') {
        try {
          descriptor.onStart(descriptor, startValue, startTime,
                             previousAnimation || null);
        } catch (_error) {}
      }
      const tick = () => {
        if (animation !== descriptor) return;
        let finished = false;
        try {
          finished = !!descriptor.onFrame(descriptor, Date.now());
        } catch (_error) {
          finished = true;
        }
        if (typeof descriptor.current !== 'undefined') {
          current = descriptor.current;
        }
        notify();
        if (!finished) {
          setTimeout(tick, 16);
          return;
        }
        animation = null;
        if (typeof descriptor.callback === 'function') {
          try {
            descriptor.callback(true);
          } catch (_error) {}
        }
      };
      setTimeout(tick, 16);
    }
    return {
      get value() { return current; },
      set value(next) {
        if (typeof next === 'function' && (next.__worklet || next._closure)) {
          let descriptor = null;
          try { descriptor = next(); } catch (_error) { descriptor = null; }
          if (descriptor && typeof descriptor.onFrame === 'function') {
            startAnimation(descriptor);
            return;
          }
        }
        if (animation && typeof animation.cancel === 'function') {
          try { animation.cancel(); } catch (_error) {}
        }
        animation = null;
        current = next;
        notify();
      },
      addListener(listener) {
        if (typeof listener === 'function') listeners.add(listener);
      },
      removeListener(listener) {
        listeners.delete(listener);
      },
      // `wx.worklet.derived(processor)` on the non-native worklet path calls
      // `mutable.installGetter(processor, closureValues)`.  Recompute the
      // derived value whenever a captured shared value changes (route
      // animations rely on this: `_curvePrimaryAnimation` drives the detail
      // page opacity, a stale 0 hides all detail text).
      installGetter(processor, deps) {
        if (typeof processor !== 'function') return;
        const inputs = [];
        collectWorkletInputs(deps || [], inputs, new Set(), 0);
        let computing = false;
        const recompute = () => {
          if (computing) return;
          computing = true;
          try {
            current = processor.call(processor._closure || undefined);
          } catch (_error) {
          } finally {
            computing = false;
          }
          notify();
        };
        for (const input of inputs) {
          try {
            input.addListener(recompute);
          } catch (_error) {}
        }
        recompute();
      },
    };
  }

  // Worklet event handlers are registered by the Skyline component library as
  // `${eventName}-${viewTag}` (e.g. `onScroll-42`) through
  // SkylineGlobal.ReanimatedModule.  Keep them so native scroll events can be
  // delivered to the worklet on the appservice thread.
  const workletEventHandlers = new Map();

  function workletStub() {
    const target = Object.create(null);
    const passthrough = function (value) {
      return typeof value === 'function' ? value : undefined;
    };
    target.installCoreFunctions = function (setter) { return setter; };
    target.makeShareable = function (value) { return value; };
    target.makeMutable = workletMutable;
    target.makeRemote = function (value) { return value; };
    // applyAnimatedStyle/derived mapper registration: the first argument is a
    // selector string (or a direct element), `worklet` is the generated mapper
    // worklet and `inputs` are the captured shared values that schedule a
    // re-run when they change.
    target.startMapper = function (selector, worklet, inputs, outputs, config) {
      if (typeof worklet !== 'function') return 0;
      // `immediate: false` means "do not apply the updater's current styles at
      // registration"; the first run happens when an input changes.  Running
      // it eagerly paints initial shared values (card opacity/scale start at
      // 0) and hides card text/images that the official renderer leaves alone.
      const immediate = !(config && config.immediate === false);
      const id = nextWorkletMapperId++;
      const entry = {
        id,
        selector,
        worklet,
        inputs: [],
        outputs: [],
        dirty: immediate,
        misses: 0,
        owner: null,
        flush: config && config.flush === 'sync' ? 'sync' : 'async',
        listener: null,
      };
      collectWorkletInputs(inputs || [], entry.inputs, new Set(), 0);
      collectWorkletInputs(outputs || [], entry.outputs, new Set(), 0);
      entry.listener = () => {
        entry.dirty = true;
        if (entry.flush === 'sync') {
          executeWorkletMapper(entry);
        } else {
          scheduleWorkletMapperFrame();
        }
      };
      for (const input of entry.inputs) {
        try {
          input.addListener(entry.listener);
        } catch (_error) {}
      }
      workletMappers.set(id, entry);
      if (immediate) scheduleWorkletMapperFrame();
      return id;
    };
    target.stopMapper = function (windowId, mapperId) {
      stopWorkletMapper(Number(mapperId) || 0);
    };
    target.stopMappers = function (windowId, mapperIds) {
      if (Array.isArray(mapperIds)) {
        for (const mapperId of mapperIds) {
          stopWorkletMapper(Number(mapperId) || 0);
        }
      } else if (typeof mapperIds === 'number') {
        stopWorkletMapper(mapperIds);
      } else {
        for (const mapperId of Array.from(workletMappers.keys())) {
          stopWorkletMapper(mapperId);
        }
      }
    };
    target.enableMapper = function () {};
    target.disableMapper = function () {};
    target.pushFrame = function () {};
    target.requestFrame = function () {};
    target.getCurrentTime = function () { return Date.now(); };
    target.registerEventHandler = function (hash, worklet) {
      if (typeof hash === 'string' && typeof worklet === 'function') {
        workletEventHandlers.set(hash, worklet);
      }
    };
    target.unregisterEventHandler = function (hash) {
      workletEventHandlers.delete(hash);
    };
    return new Proxy(target, {
      get(object, property) {
        if (property in object) return object[property];
        if (typeof property !== 'string') return undefined;
        object[property] = passthrough;
        return passthrough;
      },
    });
  }

