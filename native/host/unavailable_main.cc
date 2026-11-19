// Stub target built when the pinned Flutter engine has not been fetched/built yet.
// Keeping a stable target name means `npm run native:build` always succeeds and the
// smoke test can distinguish "engine unavailable" (exit 77) from a broken host.

#include <cstdio>

int main() {
  std::fprintf(stderr,
               "skyline_host: libflutter_engine.so/flutter_embedder.h not found.\n"
               "Run:\n"
               "  bash native/engine/fetch-engine.sh\n"
               "  bash native/engine/build-engine.sh\n"
               "  npm run native:build\n");
  return 77;
}
