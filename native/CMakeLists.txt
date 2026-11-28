cmake_minimum_required(VERSION 3.16)
project(skyline_node_native LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

find_program(NODE_EXECUTABLE node REQUIRED)
execute_process(
  COMMAND "${NODE_EXECUTABLE}" -p
    "require('path').join(require('path').dirname(process.execPath),'..','include','node')"
  OUTPUT_VARIABLE NODE_INCLUDE_DIR
  OUTPUT_STRIP_TRAILING_WHITESPACE
)
if(NOT EXISTS "${NODE_INCLUDE_DIR}/node_api.h")
  message(FATAL_ERROR "node_api.h not found under ${NODE_INCLUDE_DIR}; set NODE_INCLUDE_DIR")
endif()
message(STATUS "Node headers: ${NODE_INCLUDE_DIR}")

# Bootstrap source: the ordered parts under src/bootstrap (parts.txt) are
# concatenated into one script.  It runs via napi_run_script and all state is
# closure-local, so the parts cannot be evaluated as separate scripts.
set(SKYLINE_BOOTSTRAP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/bootstrap")
file(STRINGS "${SKYLINE_BOOTSTRAP_DIR}/parts.txt" SKYLINE_BOOTSTRAP_PARTS)
set(SKYLINE_BOOTSTRAP_JS "")
set(_skyline_bootstrap_deps "${SKYLINE_BOOTSTRAP_DIR}/parts.txt")
foreach(_part IN LISTS SKYLINE_BOOTSTRAP_PARTS)
  string(STRIP "${_part}" _part)
  if(_part STREQUAL "")
    continue()
  endif()
  set(_part_path "${SKYLINE_BOOTSTRAP_DIR}/${_part}")
  file(READ "${_part_path}" _part_text)
  string(APPEND SKYLINE_BOOTSTRAP_JS "${_part_text}")
  list(APPEND _skyline_bootstrap_deps "${_part_path}")
endforeach()
string(FIND "${SKYLINE_BOOTSTRAP_JS}" ")SKYLINEJS\"" _skyline_terminator)
if(NOT _skyline_terminator EQUAL -1)
  message(FATAL_ERROR "bootstrap contains the )SKYLINEJS\" raw-string terminator")
endif()
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/bootstrap_js.h"
  "// Generated from src/bootstrap/*.js (parts.txt order)\n"
  "static const char kSkylineBootstrapJs[] = R\"SKYLINEJS(\n${SKYLINE_BOOTSTRAP_JS}\n)SKYLINEJS\";\n")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  ${_skyline_bootstrap_deps})

# ---------------------------------------------------------------------------
# Linux Flutter Engine (phase 2).  When the pinned engine has been built, the
# N-API module links it and hosts windows in-process; otherwise it falls back
# to StubWindowBackend so contract tests still build on a source-only machine.
# ---------------------------------------------------------------------------
set(SKYLINE_ENGINE_ROOT
  "${CMAKE_CURRENT_SOURCE_DIR}/engine/out/linux-x64-debug"
  CACHE PATH "Staged libflutter_engine.so + flutter_embedder.h")
set(SKYLINE_ENGINE_LIB "${SKYLINE_ENGINE_ROOT}/libflutter_engine.so")
set(SKYLINE_ENGINE_HEADER "${SKYLINE_ENGINE_ROOT}/flutter_embedder.h")
find_package(Threads REQUIRED)

add_library(skyline_node MODULE
  src/main.cc
  src/napi_binding.cc
  src/window_backend_stub.cc
  common/config.cc
  common/log.cc
)
target_include_directories(skyline_node PRIVATE
  "${NODE_INCLUDE_DIR}"
  "${CMAKE_CURRENT_BINARY_DIR}"
  "${CMAKE_CURRENT_SOURCE_DIR}/include"
)
if(EXISTS "${SKYLINE_ENGINE_LIB}" AND EXISTS "${SKYLINE_ENGINE_HEADER}")
  target_sources(skyline_node PRIVATE
    host/flutter_window_backend.cc
    host/embedder/embedder.cc
  )
  target_include_directories(skyline_node PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/host"
    "${SKYLINE_ENGINE_ROOT}"
  )
  target_compile_definitions(skyline_node PRIVATE
    SKYLINE_HAVE_ENGINE=1
    "SKYLINE_DEFAULT_ICU=\"${SKYLINE_ENGINE_ROOT}/icudtl.dat\""
  )
  target_link_libraries(skyline_node PRIVATE
    "${SKYLINE_ENGINE_LIB}"
    Threads::Threads
    ${CMAKE_DL_LIBS}
  )
  target_link_options(skyline_node PRIVATE
    "-Wl,-rpath,${SKYLINE_ENGINE_ROOT}"
  )
  message(STATUS "skyline_node: Linux Flutter Engine backend at ${SKYLINE_ENGINE_ROOT}")
else()
  message(STATUS
    "skyline_node: engine not built (${SKYLINE_ENGINE_ROOT}); using StubWindowBackend. "
    "Run native/engine/fetch-engine.sh && native/engine/build-engine.sh.")
endif()
set_target_properties(skyline_node PROPERTIES
  PREFIX ""
  SUFFIX ".node"
  OUTPUT_NAME "skyline"
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
  # Build tree only: there are no install rules.  native/engine/
  # stage-devtools-addon.sh rewrites RUNPATH to $ORIGIN when staging.
  BUILD_RPATH "${SKYLINE_ENGINE_ROOT}"
)
# Node-API symbols are provided by the host process at load time.
if(NOT APPLE)
  target_link_options(skyline_node PRIVATE "-Wl,--allow-shlib-undefined")
endif()

# ---------------------------------------------------------------------------
# sharedMemory addon (native/shared-memory).
#
# Vendored from https://github.com/msojocs/skyline-shared-memory so the
# repository no longer needs a sibling checkout, vcpkg, or spdlog.  The addon
# owns /dev/shm/skyline_<key>.dat and exchanges frames with skyline.node; it
# lands next to skyline.node in the build directory.
# ---------------------------------------------------------------------------
if(WIN32)
  message(STATUS
    "shared_memory: the in-repo addon is built on Linux only; Wine oracle runs "
    "load the official sharedMemory.module from the Windows resources")
else()
  set(SKYLINE_SHARED_MEMORY_DIR "${CMAKE_CURRENT_SOURCE_DIR}/shared-memory")
  add_library(shared_memory MODULE
    "${SKYLINE_SHARED_MEMORY_DIR}/src/main.cc"
    "${SKYLINE_SHARED_MEMORY_DIR}/src/logger.cc"
    "${SKYLINE_SHARED_MEMORY_DIR}/src/memory/set.cc"
    "${SKYLINE_SHARED_MEMORY_DIR}/src/memory/get.cc"
    "${SKYLINE_SHARED_MEMORY_DIR}/src/memory/remove.cc"
    "${SKYLINE_SHARED_MEMORY_DIR}/src/memory/manager.cc"
    "${SKYLINE_SHARED_MEMORY_DIR}/src/memory/compat.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/common/config.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/common/log.cc"
  )
  target_include_directories(shared_memory PRIVATE
    "${NODE_INCLUDE_DIR}"
    "${SKYLINE_SHARED_MEMORY_DIR}/third_party/node-addon-api"
    "${SKYLINE_SHARED_MEMORY_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
  )
  target_link_libraries(shared_memory PRIVATE Threads::Threads)
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(shared_memory PRIVATE rt)
  endif()
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    # Keep the object layout stable across clean builds, as upstream did.
    target_compile_options(shared_memory PRIVATE
      "-frandom-seed=skyline_shared_memory")
  endif()
  if(NOT APPLE)
    target_link_options(shared_memory PRIVATE "-Wl,--allow-shlib-undefined")
  endif()
  set_target_properties(shared_memory PROPERTIES
    PREFIX ""
    SUFFIX ".node"
    OUTPUT_NAME "sharedMemory"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
  )
  message(STATUS
    "shared_memory: addon target enabled -> ${CMAKE_BINARY_DIR}/sharedMemory.node")
endif()

# ---------------------------------------------------------------------------
# Phase-3 stub of the private skyline.node runtime (Linux shared library).
#
# package:skyline resolves 73 symbols from this library and registers its Dart
# callbacks into an FFI table.  The stub implements the bootstrap surface and
# renders the Flutter UI; layout/paint/gesture behaviour is still a no-op.
# ---------------------------------------------------------------------------
# Runtime sources: the manifest is shared with the engine patch script and the
# conformance scanner.  Only [common] and [shared] translation units are
# compiled into the standalone libskyline.so; the engine embedder adds the
# [embedded] modules.  [shared] paths are relative to native/.
set(SKYLINE_RUNTIME_DIR "${CMAKE_CURRENT_SOURCE_DIR}/runtime")
file(STRINGS "${SKYLINE_RUNTIME_DIR}/sources.txt" SKYLINE_RUNTIME_MANIFEST)
set(_skyline_runtime_section "")
set(SKYLINE_RUNTIME_SOURCES "")
foreach(_line IN LISTS SKYLINE_RUNTIME_MANIFEST)
  string(STRIP "${_line}" _line)
  if(_line STREQUAL "" OR _line MATCHES "^#")
    continue()
  endif()
  if(_line MATCHES "^\\[([a-z]+)\\]$")
    set(_skyline_runtime_section "${CMAKE_MATCH_1}")
    continue()
  endif()
  if(NOT _line MATCHES "\\.cc$")
    continue()
  endif()
  if(_skyline_runtime_section STREQUAL "common")
    list(APPEND SKYLINE_RUNTIME_SOURCES "${SKYLINE_RUNTIME_DIR}/${_line}")
  elseif(_skyline_runtime_section STREQUAL "shared")
    list(APPEND SKYLINE_RUNTIME_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/${_line}")
  endif()
endforeach()
if(NOT SKYLINE_RUNTIME_SOURCES)
  message(FATAL_ERROR "runtime/sources.txt has no [common] sources")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${SKYLINE_RUNTIME_DIR}/sources.txt")

add_library(skyline_runtime SHARED ${SKYLINE_RUNTIME_SOURCES})
target_include_directories(skyline_runtime PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/include")
set_target_properties(skyline_runtime PROPERTIES
  PREFIX "lib"
  OUTPUT_NAME "skyline"
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

set(SKYLINE_DART_DL_INCLUDE "")
foreach(candidate
    "${CMAKE_CURRENT_SOURCE_DIR}/engine/toolchain/dart-sdk/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/engine/checkout/engine/src/flutter/third_party/dart/runtime/include")
  if(EXISTS "${candidate}/dart_api_dl.h" AND EXISTS "${candidate}/dart_api_dl.c")
    set(SKYLINE_DART_DL_INCLUDE "${candidate}")
    break()
  endif()
endforeach()

if(SKYLINE_DART_DL_INCLUDE)
  target_sources(skyline_runtime PRIVATE "${SKYLINE_DART_DL_INCLUDE}/dart_api_dl.c")
  target_include_directories(skyline_runtime PRIVATE "${SKYLINE_DART_DL_INCLUDE}")
  message(STATUS "skyline_runtime: Dart API DL at ${SKYLINE_DART_DL_INCLUDE}")
else()
  message(STATUS "skyline_runtime: dart_api_dl.h not found; log messages will be opaque")
endif()

# ---------------------------------------------------------------------------
# Headless Flutter engine host (phase 2).
#
# The engine is produced by native/engine/build-engine.sh into
# native/engine/out/linux-x64-debug/.  When it is absent we still define the
# target (as a stub returning exit 77) so CMake's generated build is stable and
# `npm run native:build` keeps working without the multi-GiB checkout.
# ---------------------------------------------------------------------------
option(SKYLINE_BUILD_HOST "Build the headless Flutter host" ON)

if(SKYLINE_BUILD_HOST)
  if(EXISTS "${SKYLINE_ENGINE_LIB}" AND EXISTS "${SKYLINE_ENGINE_HEADER}")
    add_executable(skyline_host
      host/main.cc
      host/frame_writer.cc
      host/embedder/embedder.cc
      common/config.cc
      common/log.cc
    )
    target_include_directories(skyline_host PRIVATE
      "${CMAKE_CURRENT_SOURCE_DIR}/host"
      "${CMAKE_CURRENT_SOURCE_DIR}/include"
      "${SKYLINE_ENGINE_ROOT}"
    )
    target_link_libraries(skyline_host PRIVATE
      "${SKYLINE_ENGINE_LIB}"
      Threads::Threads
      ${CMAKE_DL_LIBS}
    )
    set_target_properties(skyline_host PROPERTIES
      BUILD_RPATH "${SKYLINE_ENGINE_ROOT}"
    )
    message(STATUS "skyline_host: engine found at ${SKYLINE_ENGINE_ROOT}")
  else()
    add_executable(skyline_host host/unavailable_main.cc)
    message(STATUS
      "skyline_host: engine not built (${SKYLINE_ENGINE_ROOT}); using exit-77 stub. "
      "Run native/engine/fetch-engine.sh && native/engine/build-engine.sh.")
  endif()
endif()
