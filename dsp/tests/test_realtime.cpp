// process(), reset() and setParam(): no allocation. Counted at operator new and, on macOS, at malloc itself.
#include <magmora/Chain.h>
#include <magmora/Engine.h>

#include <catch.hpp>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>
#include <new>
#include <vector>

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define MAGMORA_SANITIZED 1
#endif
#endif
#if defined(__SANITIZE_ADDRESS__)
#define MAGMORA_SANITIZED 1
#endif

namespace {

// The thread whose allocations count. Not a thread_local: the first touch of one on a new thread (the cabinet's
// worker) allocates, which the malloc hook below would see while it runs, and recurse.
std::atomic<std::thread::id> gCountingThread{};
struct CountingFlag {
  CountingFlag& operator=(bool on) {
    gCountingThread.store(on ? std::this_thread::get_id() : std::thread::id(), std::memory_order_relaxed);
    return *this;
  }
};
CountingFlag tCounting;
std::atomic<long> gCount{0};

void note() {
  if (gCountingThread.load(std::memory_order_relaxed) == std::this_thread::get_id()) gCount.fetch_add(1, std::memory_order_relaxed);
}

void* allocate(std::size_t n) {
  note();
  if (void* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}

void* allocateAligned(std::size_t n, std::size_t alignment) {
  note();
#if defined(_WIN32)
  if (void* p = _aligned_malloc(n ? n : 1, alignment)) return p;
#else
  void* p = nullptr;
  if (posix_memalign(&p, alignment < sizeof(void*) ? sizeof(void*) : alignment, n ? n : 1) == 0) return p;
#endif
  throw std::bad_alloc();
}

void releaseAligned(void* p) {
#if defined(_WIN32)
  _aligned_free(p);
#else
  std::free(p);
#endif
}

}  // namespace

void* operator new(std::size_t n) { return allocate(n); }
void* operator new[](std::size_t n) { return allocate(n); }
void* operator new(std::size_t n, std::align_val_t a) { return allocateAligned(n, static_cast<std::size_t>(a)); }
void* operator new[](std::size_t n, std::align_val_t a) { return allocateAligned(n, static_cast<std::size_t>(a)); }
void operator delete(void* p) noexcept { note(); std::free(p); }
void operator delete[](void* p) noexcept { note(); std::free(p); }
void operator delete(void* p, std::size_t) noexcept { note(); std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { note(); std::free(p); }
void operator delete(void* p, std::align_val_t) noexcept { note(); releaseAligned(p); }
void operator delete[](void* p, std::align_val_t) noexcept { note(); releaseAligned(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { note(); releaseAligned(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { note(); releaseAligned(p); }

#if defined(__APPLE__) && !defined(MAGMORA_SANITIZED)
// libmalloc calls this for every malloc, realloc and free when it is set.
extern "C" {
typedef void(malloc_logger_t)(uint32_t type, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uint32_t);
extern malloc_logger_t* malloc_logger;
}
namespace {
void countMalloc(uint32_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uint32_t) { note(); }
struct MallocWatch {
  MallocWatch() : previous(malloc_logger) { malloc_logger = countMalloc; }
  ~MallocWatch() { malloc_logger = previous; }
  malloc_logger_t* previous;
};
}  // namespace
#else
namespace {
struct MallocWatch {
  MallocWatch() {}
};
}  // namespace
#endif

using namespace magmora;

TEST_CASE("realtime: the counter sees allocations") {
  const MallocWatch watch;
  gCount = 0;
  tCounting = true;
  {
    std::vector<double> v(100);
    volatile double* sink = v.data();
    (void)sink;
  }
  tCounting = false;
  CHECK(gCount.load() > 0);
}

TEST_CASE("realtime: process, reset and setParam never allocate or free") {
  for (int mode : {0, 2}) {
    Engine e;
    e.setParam(kChannelMode, mode);
    REQUIRE(e.prepare(48000.0, 256, 2));
    float fl[256], fr[256];
    double dl[256], dr[256];
    float* f[2] = {fl, fr};
    double* d[2] = {dl, dr};

    const MallocWatch watch;
    gCount = 0;
    tCounting = true;
    for (int block = 0; block < 600; ++block) {
      for (int n = 0; n < 256; ++n) {
        fl[n] = fr[n] = static_cast<float>(((block * 256 + n) % 109 - 54) * 0.004);
        dl[n] = dr[n] = fl[n];
      }
      // every automatable parameter, knobs and relays alike, and power cycling
      if (block % 10 == 1) {
        for (const ParamInfo& p : kParams)
          if (p.automatable) e.setParam(p.id, block % 20 == 1 ? p.max : p.min);
      } else if (block % 10 == 6) {
        for (const ParamInfo& p : kParams)
          if (p.automatable) e.setParam(p.id, p.def);
      }
      e.process(f, f, block % 2 ? 256 : 64);
      e.process(d, d, 256);
      (void)e.getParam(kPreLead);
      (void)e.latencySamples();
      if (block % 97 == 0) e.reset();
    }
    tCounting = false;
    CHECK(gCount.load() == 0);
  }
}

TEST_CASE("realtime: the chain's process, reset and setParameter never allocate or free") {
  Chain c;
  REQUIRE(c.prepare(48000.0, 256, 2));
  // the EQ and the response slot on, with a response long enough to be partitioned; new responses arrive while it
  // plays (loaded outside the counted region, as a host's main thread would)
  std::vector<float> response(30000);
  for (size_t n = 0; n < response.size(); ++n) response[n] = static_cast<float>(std::exp(-1e-4 * n) * std::sin(0.3 * n));
  const float* ir[1] = {response.data()};
  REQUIRE(c.loadUserIrSamples(ir, 1, static_cast<int>(response.size()), 44100.0));
  c.setParameter(kCabOn, 1.0);  // cabinet slot 2 plays the file
  c.setParameter(kMic2Source, 1.0);
  c.setParameter(kEqOn, 1.0);
  c.setParameter(kPedalOn, 1.0);  // and the pedal (XP2): its knobs, bypass and operating-point resets below
  float fl[256], fr[256];
  float* f[2] = {fl, fr};
  const MallocWatch watch;
  gCount = 0;
  tCounting = true;
  for (int block = 0; block < 300; ++block) {
    for (int n = 0; n < 256; ++n) fl[n] = fr[n] = static_cast<float>(((block * 256 + n) % 109 - 54) * 0.004);
    if (block % 10 == 1)
      for (int id = 0; id < Chain::parameterCount(); ++id) {
        const ParamInfo* p = Chain::parameterInfo(id);
        if (p->automatable) c.setParameter(id, block % 20 == 1 ? p->max : p->min);
      }
    if (block % 50 == 25) {  // the main thread loads, and frees what the audio thread let go of
      tCounting = false;
      REQUIRE(c.loadUserIrSamples(ir, 1, static_cast<int>(response.size()) - block, 48000.0));
      c.idle();
      tCounting = true;
    }
    c.process(f, f, block % 2 ? 256 : 64);
    (void)c.getParameter(kPresence);
    (void)c.blockOn(ChainBlock::Cabinet);
    if (block % 97 == 0) c.reset();
  }
  tCounting = false;
  CHECK(gCount.load() == 0);
}

TEST_CASE("realtime: the transposer in the chain: shifts, paths, modes and resets never allocate or free") {
  // added 2026-11-24: every routing, the path switched in and out and between modes (applyTransposeStructure is
  // wait-free and counted too), shifts automated, resets in the middle
  for (int routing = 0; routing < 3; ++routing) {
    Chain c;
    c.setParameter(kChannelMode, routing);
    c.setParameter(kTranspose, 5.0);
    REQUIRE(c.prepare(48000.0, 256, 2));
    float fl[256], fr[256];
    float* f[2] = {fl, fr};
    double dl[256], dr[256];
    double* d[2] = {dl, dr};
    const MallocWatch watch;
    gCount = 0;
    tCounting = true;
    for (int block = 0; block < 400; ++block) {
      for (int n = 0; n < 256; ++n) {
        fl[n] = fr[n] = static_cast<float>(((block * 256 + n) % 109 - 54) * 0.004);
        dl[n] = dr[n] = fl[n];
      }
      if (block % 7 == 3) c.setParameter(kTranspose, (block * 5) % 25 - 12);
      if (block % 23 == 11) {
        c.setParameter(kTransposeMode, (block / 23) % 2);
        c.applyTransposeStructure();
      }
      if (block % 3 == 0) c.process(d, d, block % 2 ? 256 : 37);
      else c.process(f, f, block % 2 ? 256 : 64);
      if (block % 101 == 50) c.reset();
    }
    tCounting = false;
    INFO("routing " << routing);
    CHECK(gCount.load() == 0);
  }
}

namespace {
void writeFloatWav(const std::string& path, const std::vector<float>& x) {
  std::FILE* f = std::fopen(path.c_str(), "wb");
  REQUIRE(f);
  auto u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
  auto u16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
  std::fwrite("RIFF", 1, 4, f);
  u32(static_cast<uint32_t>(36 + 4 * x.size()));
  std::fwrite("WAVEfmt ", 1, 8, f);
  u32(16);
  u16(3);
  u16(1);
  u32(48000);
  u32(192000);
  u16(4);
  u16(32);
  std::fwrite("data", 1, 4, f);
  u32(static_cast<uint32_t>(4 * x.size()));
  std::fwrite(x.data(), 4, x.size(), f);
  std::fclose(f);
}
}  // namespace

TEST_CASE("realtime: the cabinet's knobs move, slot sources switch, files load, the audio thread never allocates") {
  // a synthetic pack in a temporary folder: two speakers, positions a quarter inch apart and two grille distances
  const std::filesystem::path dir = std::filesystem::temp_directory_path() / "magmora-realtime-cab-pack";
  std::filesystem::create_directories(dir);
  uint32_t seed = 1;
  for (const char* spk : {"LL", "UR"})
    for (double x : {0.0, 0.25, 0.5, 0.75})
      for (double g : {0.0, 0.5}) {
        std::vector<float> h(12000);
        for (size_t n = 0; n < h.size(); ++n) {
          seed = seed * 1664525u + 1013904223u;
          h[n] = static_cast<float>((static_cast<double>(seed >> 8) / 16777216.0 - 0.5) * std::exp(-2e-3 * n));
        }
        char name[96];
        std::snprintf(name, sizeof name, "SPK %s CAB DY57 %.2fin %.1fin PRE1.wav", spk, x, g);
        writeFloatWav((dir / name).string(), h);
      }
  Chain c;
  REQUIRE(c.prepare(48000.0, 64, 2));
  std::string error;
  REQUIRE(c.loadCabinetPack(dir.string().c_str(), &error));
  c.setCabinetMicSource(1, "UR", "");
  c.setParameter(kCabOn, 1.0);
  c.setParameter(kMic2On, 1.0);
  c.setParameter(kMic2Type, 0.0);
  float fl[64], fr[64];
  float* f[2] = {fl, fr};
  const MallocWatch watch;
  gCount = 0;
  tCounting = true;
  for (int block = 0; block < 1500; ++block) {
    for (int n = 0; n < 64; ++n) fl[n] = fr[n] = static_cast<float>(((block * 64 + n) % 97 - 48) * 0.004);
    if (block % 6 == 0) {  // a knob moves: position, distance, level, pan, polarity, offset, align, the block itself
      const double u = 0.5 + 0.5 * std::sin(0.01 * block);
      c.setParameter(kMic1Position, u);
      c.setParameter(kMic2Position, 1.0 - u);
      c.setParameter(kMic1Distance, 20.0 * u);
      c.setParameter(kMic2Level, -6.0 * u);
      c.setParameter(kMic1Pan, 2.0 * u - 1.0);
      c.setParameter(kMic2Polarity, block % 120 < 60 ? 0.0 : 1.0);
      c.setParameter(kMicOffset, 10.0 * u);
      c.setParameter(kMicAlign, block % 300 < 150 ? 1.0 : 0.0);
      if (block % 500 == 498) c.setParameter(kCabOn, 0.0);
      if (block % 500 == 0) c.setParameter(kCabOn, 1.0);
    }
    if (block % 90 == 30) c.setParameter(kMic1Source, (block / 90) % 2);  // a slot switches to its file and back
    if (block % 70 == 10) c.setParameter(kMic2Source, (block / 70) % 3 == 1 ? 1.0 : 0.0);
    if (block % 110 == 55) {  // the main thread loads a file into a slot
      tCounting = false;
      {
        std::vector<float> ir(6000 + static_cast<size_t>(block));
        for (size_t n = 0; n < ir.size(); ++n) ir[n] = static_cast<float>(std::exp(-1e-3 * n) * std::sin(0.2 * n + block));
        const float* irs[2] = {ir.data(), ir.data()};
        REQUIRE(c.loadCabinetIrSamples((block / 110) % 2, irs, 1 + (block / 220) % 2, static_cast<int>(ir.size()), 44100.0));
        if (block % 330 == 55) c.clearCabinetIr(1);
      }
      tCounting = true;
    }
    c.process(f, f, 64);
    (void)c.getParameter(kMic1Position);
    if (block % 397 == 0) c.reset();
    tCounting = false;
    std::this_thread::sleep_for(std::chrono::microseconds(1333));  // real time at 48 kHz: the worker keeps up
    tCounting = true;
  }
  tCounting = false;
  const CabinetStatus st = c.cabinetStatus();
  INFO("responses built " << st.builds << ", last build " << st.lastBuildMs << " ms");
  CHECK(st.builds > 20);
  CHECK(gCount.load() == 0);
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
}
