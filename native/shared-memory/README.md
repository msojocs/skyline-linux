# shared-memory

In-tree build of the `sharedMemory.node` addon used by the Skyline host and by
the WeChat DevTools renderer.  It was migrated here from
[`msojocs/skyline-shared-memory`](https://github.com/msojocs/skyline-shared-memory)
(upstream commit `c6b51c4`; the upstream package declares ISC) so this
repository no longer needs a sibling checkout, vcpkg, or a Node `node_modules`
tree.

## Layout

```
src/main.cc               module entry, exports setMemory/getMemory/...
src/logger.{hh,cc}        opt-in stderr logger (replaces spdlog)
src/memory/manager.*      mmap/shm_open backend, outer SharedMemoryHeader
src/memory/{set,get}.cc   data-area views, Electron handle ABI, copy API
src/memory/{remove,compat}.cc
third_party/node-addon-api  vendored headers 7.1.0 (MIT)
```

The Linux data file is `/dev/shm/skyline_<key>.dat`:

```
outer header (16 bytes): u64 data_size | i32 version(1) | 4 bytes padding
data region:             frame header + RGBA8 pixels (owned by skyline.node)
```

`native/host/frame_writer.cc` writes the same layout.

## Build

The addon is part of the normal native build:

```bash
npm run native:build           # native/build/sharedMemory.node
npm run native:build:release   # native/build-release/sharedMemory.node
cmake -S native -B native/build -DCMAKE_BUILD_TYPE=Release
cmake --build native/build --target shared_memory
```

## Changes from upstream

- **No vcpkg / no spdlog.**  Upstream logged through spdlog (and therefore
  depended on vcpkg).  The in-tree logger is a no-op by default; set
  `SKYLINE_SHARED_MEMORY_LOG=1` to print messages to stderr.  Call arguments
  are not interpolated because upstream mixes fmt (`{}`) and printf (`%s`)
  placeholders.
- **No npm dependencies.**  `node-addon-api` 7.1.0 headers are vendored under
  `third_party/node-addon-api` instead of being resolved from `node_modules`.
- Missing `<stdexcept>` includes that spdlog used to provide are declared
  explicitly.

The exported API, key validation, `copyMemoryToBuffer` semantics, and the
Electron 16-byte handle behaviour are unchanged.  Regression coverage lives in
`native/test/shared-memory-addon.test.js` (`npm run test:shared-memory`) and
`test/linux.test.js` (L3).
