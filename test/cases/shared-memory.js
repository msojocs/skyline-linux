'use strict';

const { capture, uniqueKey, descriptorShape, stabilize } = require('../lib/util');

function requireShared(ctx) {
  if (!ctx.shared) throw new Error('sharedMemory module is not available');
  return ctx.shared;
}

function viewOf(value) {
  if (value instanceof ArrayBuffer) return new Uint8Array(value);
  if (ArrayBuffer.isView(value)) return new Uint8Array(value.buffer, value.byteOffset, value.byteLength);
  return null;
}

function filePathFor(key) {
  return `/dev/shm/skyline_${key}.dat`;
}

function cleanup(shared, key) {
  try { shared.removeMemory(key); } catch { /* already removed or invalid */ }
}


function lengthValidationCases() {
  // Large allocations are intentionally excluded: the official Windows module
  // maps the requested size and a 1 TiB request hangs the Wine host.
  const values = [0, -1, 0.5, NaN, Infinity, '16', null, 1];
  return values.map((value, index) => ({
    id: `shm.length.${index}`,
    group: 'shared-memory',
    isolate: true,
    label: String(value),
    run(ctx) {
      const shared = requireShared(ctx);
      const key = uniqueKey('shm_len');
      return capture(() => {
        const handle = shared.setMemory(key, value);
        const shape = descriptorShape(handle);
        try { shared.removeMemory(key); } catch {}
        return shape;
      });
    },
  }));
}

const cases = [
  {
    id: 'shm.exports',
    group: 'shared-memory',
    run(ctx) {
      const shared = requireShared(ctx);
      return {
        keys: Object.keys(shared).sort(),
        types: Object.fromEntries(Object.keys(shared).sort().map(name => [name, typeof shared[name]])),
      };
    },
  },
  {
    id: 'shm.setMemoryShape',
    group: 'shared-memory',
    run(ctx) {
      const shared = requireShared(ctx);
      const key = uniqueKey('shm_shape');
      try {
        const value = shared.setMemory(key, 64);
        const getValue = shared.getMemory(key);
        return {
          set: descriptorShape(value),
          get: descriptorShape(getValue),
        };
      } finally {
        cleanup(shared, key);
      }
    },
  },
  {
    id: 'shm.zeroInitialized',
    group: 'shared-memory',
    run(ctx) {
      const shared = requireShared(ctx);
      const key = uniqueKey('shm_zero');
      try {
        shared.setMemory(key, 256);
        const value = shared.getMemory(key);
        const view = viewOf(value);
        const data = view ? Array.from(view.subarray(0, 64)) : null;
        return {
          byteLength: view && view.byteLength,
          first64: data,
          allZero: data ? data.every(byte => byte === 0) : null,
        };
      } finally {
        cleanup(shared, key);
      }
    },
  },
  {
    id: 'shm.keyValidation',
    group: 'shared-memory',
    run(ctx) {
      const shared = requireShared(ctx);
      const keys = ['', '../escape', 'a b', '中文', 'ok.key_1-2', 'x'.repeat(201), 'x'.repeat(200)];
      const result = {};
      for (const key of keys) {
        result[JSON.stringify(key)] = capture(() => {
          const value = shared.setMemory(key, 16);
          const shape = descriptorShape(value);
          try { shared.removeMemory(key); } catch {}
          return shape;
        });
      }
      result.number = capture(() => shared.setMemory(42, 16));
      result.null = capture(() => shared.setMemory(null, 16));
      return result;
    },
  },
  ...lengthValidationCases(),
  {
    id: 'shm.reuseKey',
    group: 'shared-memory',
    run(ctx) {
      const shared = requireShared(ctx);
      const key = uniqueKey('shm_reuse');
      try {
        const first = descriptorShape(shared.setMemory(key, 32));
        const second = descriptorShape(shared.setMemory(key, 64));
        const getValue = shared.getMemory(key);
        return {
          first,
          second,
          get: descriptorShape(getValue),
          getByteLength: viewOf(getValue) && viewOf(getValue).byteLength,
        };
      } finally {
        cleanup(shared, key);
      }
    },
  },
  {
    id: 'shm.missingKey',
    group: 'shared-memory',
    run(ctx) {
      const shared = requireShared(ctx);
      const key = uniqueKey('shm_missing');
      return {
        get: capture(() => stabilize(shared.getMemory(key))),
        remove: capture(() => stabilize(shared.removeMemory(key))),
      };
    },
  },
  {
    id: 'shm.removeExisting',
    group: 'shared-memory',
    run(ctx) {
      const shared = requireShared(ctx);
      const key = uniqueKey('shm_remove');
      shared.setMemory(key, 16);
      const first = capture(() => stabilize(shared.removeMemory(key)));
      const second = capture(() => stabilize(shared.removeMemory(key)));
      return { first, second };
    },
  },
  {
    id: 'shm.copyApiShape',
    group: 'shared-memory',
    run(ctx) {
      const shared = requireShared(ctx);
      const names = ['copyMemoryToBuffer', 'deleteCopyCache', 'getMemoryByAddress', 'setMemoryByAddress', 'deleteSetCache', 'deleteGetCache'];
      return Object.fromEntries(names.map(name => [name, typeof shared[name]]));
    },
  },
];

module.exports = cases;
