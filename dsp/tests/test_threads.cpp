// The chain across threads: an audio thread processing while the main thread loads responses, changes the cabinet
// and saves the state; and main-thread calls that must not wait for the cabinet's worker while it builds. Run it
// under ThreadSanitizer too.
#include <magmora/Chain.h>

#include <catch.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <random>
#include <thread>
#include <vector>

using namespace magmora;
using Clock = std::chrono::steady_clock;

namespace {

void loadNoise(Chain& c, int slot, unsigned seed, double seconds) {
  const int n = static_cast<int>(48000 * seconds);
  std::vector<float> h(static_cast<size_t>(n));
  std::mt19937 r(seed);
  std::normal_distribution<float> d;
  for (int i = 0; i < n; ++i) h[static_cast<size_t>(i)] = d(r) * std::exp(-3.f * static_cast<float>(i) / static_cast<float>(n));
  const float* ch[1] = {h.data()};
  REQUIRE(c.loadCabinetIrSamples(slot, ch, 1, n, 48000.0));
}

double millisecondsSince(Clock::time_point t0) {
  return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

}  // namespace

TEST_CASE("threads: the main thread's calls do not wait for the cabinet's worker while it builds a response") {
  Chain c;
  const int block = 512;
  REQUIRE(c.prepare(48000.0, block, 2));
  c.setParameter(kAmpOn, 0.0);
  c.setParameter(kCabOn, 1.0);
  c.setParameter(kMic1On, 1.0);
  c.setParameter(kMic1Source, 1.0);
  loadNoise(c, 0, 1, 10.0);
  REQUIRE(c.waitForCabinet(60.0));
  std::vector<float> l(block, 0.1f), r(block, 0.1f), ol(block), orr(block);
  const float* in[2] = {l.data(), r.data()};
  float* out[2] = {ol.data(), orr.data()};
  for (int b = 0; b < 48000 * 11 / block; ++b) c.process(in, out, block);  // 11 s of history to prime with
  const long builds = c.cabinetStatus().builds;
  loadNoise(c, 0, 2, 10.0);
  // while it builds (the build primes the new response with the slot's whole recent input), every call returns
  double worst = 0.0;
  const auto start = Clock::now();
  while (c.cabinetStatus().builds == builds && millisecondsSince(start) < 60000.0) {
    auto t0 = Clock::now();
    std::vector<uint8_t> state;
    c.getState(state);
    (void)c.cabinetMicSpeaker(0);
    (void)c.cabinetIrLoaded(0);
    worst = std::max(worst, millisecondsSince(t0));
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  REQUIRE(c.waitForCabinet(60.0));
  const double buildMs = c.cabinetStatus().lastBuildMs;
  INFO("build " << buildMs << " ms, longest wait " << worst << " ms");
  REQUIRE(buildMs > 20.0);  // long enough for the measurement to mean something
  CHECK(worst < 0.25 * buildMs);
}

TEST_CASE("threads: an audio thread runs while the main thread loads, swaps and clears responses and saves the state") {
  const int block = 64;
  Chain c;
  REQUIRE(c.prepare(48000.0, block, 2));
  c.setParameter(kAmpOn, 0.0);  // cheap under the sanitizer
  c.setParameter(kCabOn, 1.0);
  c.setParameter(kMic1On, 1.0);
  c.setParameter(kMic1Source, 1.0);
  c.setParameter(kMic2On, 1.0);
  c.setParameter(kMic2Source, 1.0);
  c.reset();
  std::atomic<bool> quit{false};
  std::atomic<long> bad{0}, blocks{0};
  std::thread audio([&] {
    std::vector<float> l(block), r(block), ol(block), orr(block);
    const float* in[2] = {l.data(), r.data()};
    float* out[2] = {ol.data(), orr.data()};
    std::mt19937 rng(1);
    std::uniform_real_distribution<float> u(-0.5f, 0.5f);
    while (!quit.load()) {
      for (int i = 0; i < block; ++i) l[static_cast<size_t>(i)] = r[static_cast<size_t>(i)] = u(rng);
      c.process(in, out, block);
      for (float v : ol) bad += std::isfinite(v) ? 0 : 1;
      ++blocks;
      std::this_thread::sleep_for(std::chrono::microseconds(300));
    }
  });
  std::vector<float> h(9600);
  for (int k = 0; k < 12; ++k) {
    for (size_t i = 0; i < h.size(); ++i)
      h[i] = std::exp(-0.001f * static_cast<float>(i)) * ((i * static_cast<size_t>(k + 3)) % 7 == 0 ? 1.f : -0.5f);
    const float* ch[1] = {h.data()};
    REQUIRE(c.loadCabinetIrSamples(k % 2, ch, 1, static_cast<int>(h.size()), 44100.0));
    c.setParameter(kMic1Level, -3.0 + k * 0.1);
    c.setCabinetIrNormalize(k % 2, k % 3 != 0);
    if (k == 7) c.clearCabinetIr(1);
    (void)c.cabinetStatus();
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    std::vector<uint8_t> state;
    c.getState(state);
    c.idle();
  }
  quit.store(true);
  audio.join();
  CHECK(bad.load() == 0);
  CHECK(blocks.load() > 0);
}
