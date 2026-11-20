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

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/src/bootstrap.js" SKYLINE_BOOTSTRAP_JS)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/bootstrap_js.h"
  "// Generated from src/bootstrap.js\n"
  "static const char kSkylineBootstrapJs[] = R\"SKYLINEJS(\n${SKYLINE_BOOTSTRAP_JS}\n)SKYLINEJS\";\n")

# ---------------------------------------------------------------------------
# Linux Flutter Engine (phase 2).  When the pinned engine has been built, the
# N-API module links it and hosts windows in-process; otherwise it falls back
# to StubEngine so contract tests still build on a source-only machine.
# ---------------------------------------------------------------------------
set(SKYLINE_ENGINE_ROOT
  "${CMAKE_CURRENT_SOURCE_DIR}/engine/out/linux-x64-debug"
  CACHE PATH "Staged libflutter_engine.so + flutter_embedder.h")
set(SKYLINE_ENGINE_LIB "${SKYLINE_ENGINE_ROOT}/libflutter_engine.so")
set(SKYLINE_ENGINE_HEADER "${SKYLINE_ENGINE_ROOT}/flutter_embedder.h")
find_package(Threads REQUIRED)

add_library(skyline_node MODULE
  src/main.cc
  src/engine_stub.cc
)
target_include_directories(skyline_node PRIVATE
  "${NODE_INCLUDE_DIR}"
  "${CMAKE_CURRENT_BINARY_DIR}"
  "${CMAKE_CURRENT_SOURCE_DIR}/include"
)
if(EXISTS "${SKYLINE_ENGINE_LIB}" AND EXISTS "${SKYLINE_ENGINE_HEADER}")
  target_sources(skyline_node PRIVATE host/host_engine.cc)
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
    "skyline_node: engine not built (${SKYLINE_ENGINE_ROOT}); using StubEngine. "
    "Run native/engine/fetch-engine.sh && native/engine/build-engine.sh.")
endif()
set_target_properties(skyline_node PROPERTIES
  PREFIX ""
  SUFFIX ".node"
  OUTPUT_NAME "skyline"
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
  BUILD_RPATH "${SKYLINE_ENGINE_ROOT}"
  INSTALL_RPATH "$ORIGIN/../engine/out/linux-x64-debug"
)
# Node-API symbols are provided by the host process at load time.
if(NOT APPLE)
  target_link_options(skyline_node PRIVATE "-Wl,--allow-shlib-undefined")
endif()

# ---------------------------------------------------------------------------
# Phase-3 stub of the private skyline.node runtime (Linux shared library).
#
# package:skyline resolves 73 symbols from this library and registers its Dart
# callbacks into an FFI table.  The stub implements the bootstrap surface and
# renders the Flutter UI; layout/paint/gesture behaviour is still a no-op.
# ---------------------------------------------------------------------------
add_library(skyline_runtime SHARED
  runtime/skyline_runtime_stub.cc
)
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
    )
    target_include_directories(skyline_host PRIVATE
      "${CMAKE_CURRENT_SOURCE_DIR}/host"
      "${SKYLINE_ENGINE_ROOT}"
    )
    target_link_libraries(skyline_host PRIVATE
      "${SKYLINE_ENGINE_LIB}"
      Threads::Threads
      ${CMAKE_DL_LIBS}
    )
    set_target_properties(skyline_host PROPERTIES
      BUILD_RPATH "${SKYLINE_ENGINE_ROOT}"
      INSTALL_RPATH "$ORIGIN/../engine/out/linux-x64-debug"
      BUILD_WITH_INSTALL_RPATH FALSE
      SKIP_BUILD_RPATH FALSE
    )
    message(STATUS "skyline_host: engine found at ${SKYLINE_ENGINE_ROOT}")
  else()
    add_executable(skyline_host host/unavailable_main.cc)
    message(STATUS
      "skyline_host: engine not built (${SKYLINE_ENGINE_ROOT}); using exit-77 stub. "
      "Run native/engine/fetch-engine.sh && native/engine/build-engine.sh.")
  endif()
endif()
