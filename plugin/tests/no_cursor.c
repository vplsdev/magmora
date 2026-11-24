/* Loaded by the test harness only. (1) The plug-in's knobs hide and warp the cursor, which must not happen to the
   cursor of whoever is using the machine while the tests run. (2) Allocations made on a thread that the harness
   marks as the render thread are counted, to prove the audio path allocates nothing. */
#include <ApplicationServices/ApplicationServices.h>
#include <malloc/malloc.h>
#include <stdatomic.h>
#include <execinfo.h>
#include <stdlib.h>
#include <unistd.h>

#define INTERPOSE(replacement, original)                                      \
  __attribute__((used)) static struct {                                       \
    const void* r;                                                            \
    const void* o;                                                            \
  } interpose_##original __attribute__((section("__DATA,__interpose"))) = { \
      (const void*)(unsigned long)&replacement, (const void*)(unsigned long)&original};

static CGError Display(CGDirectDisplayID display) {
  (void)display;
  return kCGErrorSuccess;
}
static CGError Move(CGDirectDisplayID display, CGPoint point) {
  (void)display;
  (void)point;
  return kCGErrorSuccess;
}
static CGError Associate(boolean_t connected) {
  (void)connected;
  return kCGErrorSuccess;
}
static CGError Warp(CGPoint point) {
  (void)point;
  return kCGErrorSuccess;
}

INTERPOSE(Display, CGDisplayHideCursor)
INTERPOSE(Display, CGDisplayShowCursor)
INTERPOSE(Move, CGDisplayMoveCursorToPoint)
INTERPOSE(Associate, CGAssociateMouseAndMouseCursorPosition)
INTERPOSE(Warp, CGWarpMouseCursorPosition)

static _Thread_local int gRendering = 0;
static _Atomic long gAllocations = 0;

__attribute__((visibility("default"))) void magmora_test_rendering(int on) { gRendering = on; }
__attribute__((visibility("default"))) long magmora_test_allocations(void) { return atomic_load(&gAllocations); }

// MAGMORA_ALLOC_TRACE=n prints where the first n allocations on the render thread come from.
static void Trace(void) {
  static int left = -1;
  if (left < 0) {
    const char* e = getenv("MAGMORA_ALLOC_TRACE");
    left = e ? atoi(e) : 0;
  }
  if (left <= 0) return;
  --left;
  const int was = gRendering;
  gRendering = 0;
  void* frames[24];
  const int n = backtrace(frames, 24);
  backtrace_symbols_fd(frames, n, 2);
  write(2, "----\n", 5);
  gRendering = was;
}

static void* Malloc(size_t n) {
  if (gRendering) {
    atomic_fetch_add(&gAllocations, 1);
    Trace();
  }
  return malloc(n);
}
static void* Calloc(size_t c, size_t n) {
  if (gRendering) atomic_fetch_add(&gAllocations, 1);
  return calloc(c, n);
}
static void* Realloc(void* p, size_t n) {
  if (gRendering) atomic_fetch_add(&gAllocations, 1);
  return realloc(p, n);
}
static int PosixMemalign(void** p, size_t a, size_t n) {
  if (gRendering) atomic_fetch_add(&gAllocations, 1);
  return posix_memalign(p, a, n);
}
static void Free(void* p) {
  if (gRendering && p) atomic_fetch_add(&gAllocations, 1);
  free(p);
}

INTERPOSE(Malloc, malloc)
INTERPOSE(Calloc, calloc)
INTERPOSE(Realloc, realloc)
INTERPOSE(PosixMemalign, posix_memalign)
INTERPOSE(Free, free)
