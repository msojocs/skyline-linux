
  function bridgeId(value) {
    if (value === undefined || value === null) return 0;
    return typeof value === 'object' ? value.viewTag : value;
  }

  function bridgeScalar(value) {
    if (value === undefined) return undefined;
    const type = typeof value;
    if (type === 'string' || type === 'number' || type === 'boolean') {
      return value;
    }
    try {
      return JSON.parse(JSON.stringify(value));
    } catch (_error) {
      return String(value);
    }
  }

  function bridgePlain(value) {
    if (value === undefined || value === null) return null;
    try {
      return JSON.parse(JSON.stringify(value));
    } catch (_error) {
      return null;
    }
  }

  // glass-easel's spliceBefore(beforeNode, count, ...nodes) passes a ShadowNode
  // where the generic splice() takes an index.  Both forms are normalised to a
  // child index.  A missing/unknown anchor appends at the end.
  function childIndexIn(children, value) {
    if (typeof value === 'number' && Number.isFinite(value)) {
      const index = Math.trunc(value);
      return index < 0 || index > children.length ? children.length : index;
    }
    if (value === undefined || value === null) return children.length;
    const index = children.indexOf(value);
    return index < 0 ? children.length : index;
  }

  // Fragments are recorded by id; the native replay flattens their children.
  // The JS-side mirror of the tree needs the flattened form.
  function splitFragment(item) {
    if (item === undefined || item === null || typeof item !== 'object') return [];
    return item.tag === 'fragment' ? item.children.slice() : [item];
  }

  // Defensive serialisation: a single ShadowNode captured in an op payload used
  // to make JSON.stringify throw (its `ops` array points back at the operation),
  // which aborted the whole render.  Nodes are replaced by their id and other
  // cycles are dropped so one malformed op cannot blank the simulator.
  function serializeOps(ops) {
    const seen = new WeakSet();
    return JSON.stringify({ ops }, (key, value) => {
      if (value === null || typeof value !== 'object') return value;
      if (typeof value.viewTag === 'number' && Array.isArray(value.ops)) {
        return value.viewTag;
      }
      if (seen.has(value)) return null;
      seen.add(value);
      return value;
    });
  }

  // ShadowNode arguments (parent/children links) are cyclic, so logging them
  // with a plain JSON.stringify throws inside the miniprogram SDK.  Collapse
  // nodes to their viewTag and drop leftover cycles.
  function debugArgs(args) {
    const seen = new WeakSet();
    try {
      return JSON.stringify(args, (key, value) => {
        if (value === null || typeof value !== 'object') return value;
        if (typeof value.viewTag === 'number' && Array.isArray(value.ops)) {
          return `#${value.viewTag}`;
        }
        if (seen.has(value)) return '[circular]';
        seen.add(value);
        return value;
      });
    } catch (_error) {
      return '[unserializable]';
    }
  }

  function toText(data) {
    if (data === null || data === undefined) return null;
    if (typeof data === 'string') return data;
    try {
      let bytes = null;
      if (data instanceof ArrayBuffer) {
        bytes = new Uint8Array(data);
      } else if (ArrayBuffer.isView(data)) {
        bytes = new Uint8Array(data.buffer, data.byteOffset, data.byteLength);
      }
      if (!bytes) return null;
      return new TextDecoder('utf-8', { fatal: false }).decode(bytes);
    } catch (_error) {
      return null;
    }
  }

  // Binary counterpart of toText for project resources (images, fonts).  The
  // DevTools loader may return an ArrayBuffer or a typed array from another
  // realm, so detect by shape rather than `instanceof`.
  function toBytes(data) {
    if (data === null || data === undefined) return null;
    if (typeof data === 'string') {
      const bytes = new Uint8Array(data.length);
      for (let i = 0; i < data.length; i += 1) {
        bytes[i] = data.charCodeAt(i) & 0xff;
      }
      return bytes;
    }
    try {
      if (typeof data.byteLength === 'number' &&
          typeof data.slice === 'function') {
        // ArrayBuffer: copy into this realm's typed array.
        return new Uint8Array(data.slice(0));
      }
      if (typeof data.byteLength === 'number' && typeof data.subarray === 'function') {
        return new Uint8Array(data.buffer || data, data.byteOffset || 0,
                              data.byteLength);
      }
      if (typeof data.length === 'number') {
        const bytes = new Uint8Array(data.length);
        for (let i = 0; i < data.length; i += 1) bytes[i] = data[i] & 0xff;
        return bytes;
      }
    } catch (_error) {
      return null;
    }
    return null;
  }

  // Project-package paths have no http(s)/data scheme: `assets/play.png`,
  // `wxfile://assets/...`, `weapp:///...`.  Returns the loader-relative path,
  // or null for remote/inline sources.
  function localResourcePath(url) {
    const text = String(url === null || url === undefined ? '' : url);
    if (!text) return null;
    if (/^data:/i.test(text)) return null;
    if (/^[a-z][a-z0-9+.-]*:\/\//i.test(text)) {
      if (!/^(wxfile|weapp|file):\/\//i.test(text)) return null;
      // `wxfile://assets/x.png` has no host component: strip the scheme only.
      return text.replace(/^[a-z][a-z0-9+.-]*:\/\//i, '')
                 .replace(/^\/+/, '') || null;
    }
    return text.replace(/^\/+/, '');
  }

  function looksLikeCss(text) {
    if (typeof text !== 'string' || text.length === 0) return false;
    // Reject binary blobs: require mostly printable text and CSS punctuation.
    let printable = 0;
    for (let i = 0; i < text.length && i < 512; i += 1) {
      const code = text.charCodeAt(i);
      if (code === 9 || code === 10 || code === 13 || (code >= 32 && code < 127) ||
          code > 159) {
        printable += 1;
      }
    }
    if (printable < Math.min(text.length, 512) * 0.95) return false;
    return text.indexOf('{') >= 0 && (text.indexOf(':') >= 0 || text.indexOf(';') >= 0);
  }

  function describeData(data, text) {
    if (data === null || data === undefined) return 'null';
    const length = typeof data.byteLength === 'number'
      ? data.byteLength
      : (typeof data.length === 'number' ? data.length : -1);
    const head = typeof text === 'string' ? text.slice(0, 120).replace(/\s+/g, ' ') : '';
    return `len=${length} text=${looksLikeCss(text) ? 'css' : 'no'} head="${head}"`;
  }

  // The DevTools appservice exposes the compiled WXSS as base64 text at
  // `/appservice/__skyline__/<path>.wxss` (the base `wxlibfile://` library
  // stylesheets are not served there).  Use the appservice's own XHR so this
  // works in every frame of the appservice webview.
  function loadStyleSheetText(path) {
    if (typeof path !== 'string' || path.startsWith('wxlibfile://')) return null;
    try {
      const pageGlobal = (typeof window !== 'undefined' &&
        (window.__global || window.__global__)) || null;
      const Xhr = pageGlobal && pageGlobal.XMLHttpRequest;
      if (typeof Xhr !== 'function') return null;
      const xhr = new Xhr();
      xhr.open('GET', `/appservice/__skyline__/${path}.wxss`, false);
      xhr.send(null);
      if (xhr.status !== 200 || !xhr.response) return null;
      const atobFn = (pageGlobal && pageGlobal.atob) ||
        (typeof atob === 'function' ? atob : null);
      if (typeof atobFn !== 'function') return null;
      const binary = atobFn(xhr.response);
      const bytes = new Uint8Array(binary.length);
      for (let i = 0; i < binary.length; i += 1) {
        bytes[i] = binary.charCodeAt(i);
      }
      return new TextDecoder('utf-8', { fatal: false }).decode(bytes);
    } catch (_error) {
      return null;
    }
  }

  function autoStub(target) {
    return new Proxy(target, {
      get(object, property) {
        if (property in object) return object[property];
        if (typeof property !== 'string') return undefined;
        return () => ++nextBridgeTag;
      },
    });
  }

