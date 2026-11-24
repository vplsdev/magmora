// The pitch transposer (dsp/src/transpose): pitch, delay, the 0-semitone delay path, switching between shifts,
// hostile input, block sizes, allocation and CPU.
#include <catch.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <new>
#include <thread>
#include <vector>

#include "transpose/Transposer.h"

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define MAGMORA_SANITIZED 1
#endif
#endif
#if defined(__SANITIZE_ADDRESS__)
#define MAGMORA_SANITIZED 1
#endif

// ---------------------------------------------------------------------------------------------- allocation counter
namespace {
std::atomic<bool> gCounting{false};
std::atomic<long> gAllocations{0};
void* allocate(std::size_t n) {
  if (gCounting.load(std::memory_order_relaxed)) gAllocations.fetch_add(1, std::memory_order_relaxed);
  if (void* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
}  // namespace

void* operator new(std::size_t n) { return allocate(n); }
void* operator new[](std::size_t n) { return allocate(n); }
void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
  try {
    return allocate(n);
  } catch (...) {
    return nullptr;
  }
}
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept {
  try {
    return allocate(n);
  } catch (...) {
    return nullptr;
  }
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

using namespace magmora;
using Mode = Transposer::Mode;

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr Mode kModes[] = {Mode::Tight, Mode::Standard};
const char* name(Mode m) { return m == Mode::Tight ? "Tight" : "Standard"; }

struct Note {
  const char* name;
  double hz;
};
const Note kNotes[] = {{"B0", 30.8677}, {"E1", 41.2034}, {"E2", 82.4069}, {"E3", 164.8138},
                       {"E4", 329.6276}, {"E5", 659.2551}, {"E6", 1318.5102}};

std::vector<float> sine(double hz, double rate, double seconds, double level, double phase = 0.3) {
  std::vector<float> x(static_cast<size_t>(seconds * rate));
  for (size_t i = 0; i < x.size(); ++i) x[i] = static_cast<float>(level * std::sin(2 * kPi * hz * static_cast<double>(i) / rate + phase));
  return x;
}

// Plucked strings, chugs and a chord: partials with per-partial decay and a short noisy attack.
std::vector<float> riff(double rate, double seconds) {
  std::vector<float> x(static_cast<size_t>(seconds * rate), 0.0f);
  uint32_t seed = 11;
  const double notes[] = {82.41, 123.47, 82.41, 110.0, 82.41, 146.83, 82.41, 164.81};
  const double step = 0.125;
  for (int e = 0; e * step < seconds - 0.2; ++e) {
    const double f0 = notes[e % 8], tau = e % 8 == 7 ? 0.6 : 0.08;
    const size_t at = static_cast<size_t>(e * step * rate);
    const size_t len = std::min(x.size() - at, static_cast<size_t>(5 * tau * rate));
    for (int h = 1; h <= 24 && h * f0 < 8000.0; ++h) {
      const double amp = std::sin(kPi * h * 0.13) / h, decay = (1 + 0.15 * h) / tau;
      for (size_t i = 0; i < len; ++i) {
        const double t = static_cast<double>(i) / rate;
        x[at + i] += static_cast<float>(0.12 * amp * std::exp(-decay * t) * std::sin(2 * kPi * f0 * h * t + 0.7 * h));
      }
    }
    for (size_t i = 0; i < std::min(len, static_cast<size_t>(0.002 * rate)); ++i) {
      seed = seed * 1664525u + 1013904223u;
      x[at + i] += static_cast<float>(0.05 * ((seed >> 8) / double(1 << 24) - 0.5));
    }
  }
  return x;
}

std::vector<float> run(Transposer& t, const std::vector<float>& x, int block = 64) {
  std::vector<float> y(x.size());
  for (size_t at = 0; at < x.size(); at += static_cast<size_t>(block))
    t.process(x.data() + at, y.data() + at, static_cast<int>(std::min(x.size() - at, static_cast<size_t>(block))));
  return y;
}

void setUp(Transposer& t, double rate, Mode mode, int semitones, int maxBlock = 256) {
  t.setSemitones(semitones);
  t.prepare(rate, maxBlock);
  t.setMode(mode);
}

// Frequency of the tone near `hz` in y[from, to): the slope of its phase, block by block (0.25 s Hann blocks).
double frequency(const std::vector<float>& y, size_t from, size_t to, double hz, double rate) {
  const size_t block = static_cast<size_t>(0.25 * rate), hop = block / 2;
  std::vector<double> t, phase;
  for (size_t a = from; a + block <= to; a += hop) {
    std::complex<double> sum = 0.0;
    for (size_t i = 0; i < block; ++i) {
      const double w = 0.5 - 0.5 * std::cos(2 * kPi * (i + 0.5) / block);
      sum += w * y[a + i] * std::polar(1.0, -2 * kPi * hz * static_cast<double>(a + i) / rate);
    }
    double p = std::arg(sum);
    if (!phase.empty()) p += 2 * kPi * std::round((phase.back() - p) / (2 * kPi));
    t.push_back(static_cast<double>(a) + 0.5 * block);
    phase.push_back(p);
  }
  double mt = 0.0, mp = 0.0;
  for (size_t i = 0; i < t.size(); ++i) {
    mt += t[i] / t.size();
    mp += phase[i] / t.size();
  }
  double num = 0.0, den = 0.0;
  for (size_t i = 0; i < t.size(); ++i) {
    num += (t[i] - mt) * (phase[i] - mp);
    den += (t[i] - mt) * (t[i] - mt);
  }
  return hz + num / den * rate / (2 * kPi);
}

double rms(const std::vector<float>& y, size_t from, size_t to) {
  double s = 0.0;
  for (size_t i = from; i < to; ++i) s += static_cast<double>(y[i]) * y[i];
  return std::sqrt(s / static_cast<double>(to - from));
}

double cents(double a, double b) { return 1200.0 * std::log2(a / b); }
double db(double x) { return 20.0 * std::log10(x + 1e-30); }

bool clean(const std::vector<float>& y) {
  for (float v : y)
    if (!std::isfinite(v) || std::fpclassify(v) == FP_SUBNORMAL) return false;
  return true;
}

// The largest component above `fromHz` in any 10 ms Blackman frame (half-overlapped), with the frame's mean and slope
// taken out: the measure of the existing block-switch test (test_automation.cpp, G-E2), in amplitude units.
double worstAbove(const std::vector<float>& y, size_t from, double fromHz, double rate) {
  const int frame = static_cast<int>(0.01 * rate);
  std::vector<double> w(static_cast<size_t>(frame)), f(static_cast<size_t>(frame));
  double wsum = 0.0;
  for (int n = 0; n < frame; ++n) {
    w[static_cast<size_t>(n)] = 0.42 - 0.5 * std::cos(2 * kPi * n / frame) + 0.08 * std::cos(4 * kPi * n / frame);
    wsum += w[static_cast<size_t>(n)];
  }
  std::vector<std::complex<double>> twiddle(static_cast<size_t>(frame));
  for (int n = 0; n < frame; ++n) twiddle[static_cast<size_t>(n)] = std::polar(1.0, -2 * kPi * n / frame);
  const int first = std::max(1, static_cast<int>(fromHz * frame / rate));
  double worst = 0.0;
  for (size_t at = from; at + static_cast<size_t>(frame) <= y.size(); at += static_cast<size_t>(frame / 2)) {
    double mean = 0.0, slope = 0.0, norm = 0.0;
    for (int n = 0; n < frame; ++n) mean += y[at + static_cast<size_t>(n)] / frame;
    for (int n = 0; n < frame; ++n) {
      const double t = n - 0.5 * (frame - 1);
      slope += t * y[at + static_cast<size_t>(n)];
      norm += t * t;
    }
    slope /= norm;
    for (int n = 0; n < frame; ++n)
      f[static_cast<size_t>(n)] = (y[at + static_cast<size_t>(n)] - mean - slope * (n - 0.5 * (frame - 1))) * w[static_cast<size_t>(n)];
    for (int k = first; k < frame / 2; ++k) {
      std::complex<double> sum = 0.0;
      for (int n = 0, index = 0; n < frame; ++n, index = (index + k) % frame) sum += f[static_cast<size_t>(n)] * twiddle[static_cast<size_t>(index)];
      worst = std::max(worst, 2.0 * std::abs(sum) / wsum);
    }
  }
  return worst;
}

// First sample where the 1 ms RMS envelope of y reaches half of its level `settle` seconds later.
double halfRise(const std::vector<float>& y, size_t from, double rate) {
  const size_t win = static_cast<size_t>(0.001 * rate);
  const auto env = [&](size_t at) { return rms(y, at, at + win); };
  const double target = 0.5 * env(from + static_cast<size_t>(0.06 * rate));
  for (size_t i = from; i + win < y.size(); ++i)
    if (env(i) >= target) return static_cast<double>(i + win / 2);
  return -1.0;
}

// Amplitude of the component at `hz` in y[from, to) (Hann window).
double amplitudeAt(const std::vector<float>& y, size_t from, size_t to, double hz, double rate) {
  std::complex<double> sum = 0.0;
  double wsum = 0.0;
  const size_t n = to - from;
  for (size_t i = 0; i < n; ++i) {
    const double w = 0.5 - 0.5 * std::cos(2 * kPi * (i + 0.5) / n);
    sum += w * y[from + i] * std::polar(1.0, -2 * kPi * hz * static_cast<double>(from + i) / rate);
    wsum += w;
  }
  return 2.0 * std::abs(sum) / wsum;
}

// What a held shift leaves above 4 kHz on a 220 Hz tone (the switch criterion's measure, dB re the tone), with no
// change of shift: the floor that a switch between two shifts is compared with.
double heldAbove4k(Mode mode, int semitones, double rate = 48000.0) {
  Transposer t;
  setUp(t, rate, mode, semitones);
  const std::vector<float> x = sine(220.0, rate, 1.2, 0.25);
  const std::vector<float> y = run(t, x, 64);
  const double tone = std::sqrt(2.0) * rms(y, static_cast<size_t>(0.3 * rate), x.size());
  return db(worstAbove(y, static_cast<size_t>(0.2 * rate), 4000.0, rate) / tone);
}

}  // namespace

TEST_CASE("transpose: the delay is 5.3 ms (Tight) or 10.7 ms (Standard) at every rate and shift") {
  for (double rate : {44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0})
    for (Mode mode : kModes) {
      Transposer t;
      setUp(t, rate, mode, 0);
      const int latency = t.latencySamples();
      const double ms = 1000.0 * latency / rate;
      INFO(name(mode) << " at " << rate << ": " << latency << " samples, " << ms << " ms, N " << t.analysisSamples()
                      << ", hop " << t.hopSamples());
      CHECK(std::fabs(ms - (mode == Mode::Tight ? 5.333 : 10.667)) < 0.1);
      CHECK(t.analysisSamples() >= 2048 * std::max(1.0, std::round(rate / 48000.0)) - 1);
      // Tight zero-pads its FFT to twice the analysis length; Standard does not
      CHECK(t.fftSize() == (mode == Mode::Tight ? 2 : 1) * t.analysisSamples());
      for (int s : {-12, -5, -1, 3, 12}) {
        t.setSemitones(s);
        std::vector<float> x(4096, 0.0f);
        run(t, x);
        CHECK(t.latencySamples() == latency);
      }
    }
  Transposer t;
  setUp(t, 48000.0, Mode::Tight, -2);
  CHECK(t.latencySamples() == 255);
  CHECK(t.hopSamples() == 85);
  CHECK(t.analysisSamples() == 2048);
  CHECK(t.fftSize() == 4096);
  t.setMode(Mode::Standard);
  CHECK(t.latencySamples() == 512);
  CHECK(t.hopSamples() == 128);
  CHECK(t.fftSize() == 2048);
}

TEST_CASE("transpose: at 0 semitones the output is the input delayed, bit for bit, at any block size") {
  for (double rate : {44100.0, 48000.0, 96000.0})
    for (Mode mode : kModes) {
      Transposer t;
      setUp(t, rate, mode, 0, 64);
      CHECK(t.isBypassable());
      std::vector<float> x(static_cast<size_t>(rate * 0.5));
      uint32_t seed = 3;
      for (float& v : x) {
        seed = seed * 1664525u + 1013904223u;
        v = static_cast<float>(((seed >> 8) / double(1 << 24) - 0.5) * 1.8);
      }
      std::vector<float> y(x.size());
      size_t at = 0;
      for (int block : {0, 1, 7, 64, 1000, 3, 513, 0, 2048})
        for (int k = 0; k < 3 && at < x.size(); ++k) {
          const int n = static_cast<int>(std::min(x.size() - at, static_cast<size_t>(block)));
          t.process(x.data() + at, y.data() + at, n);
          at += static_cast<size_t>(n);
        }
      t.process(x.data() + at, y.data() + at, static_cast<int>(x.size() - at));
      const size_t L = static_cast<size_t>(t.latencySamples());
      bool exact = true;
      for (size_t i = 0; i < x.size(); ++i) exact = exact && (i < L ? y[i] == 0.0f : y[i] == x[i - L]);
      INFO(name(mode) << " at " << rate);
      CHECK(exact);
      CHECK(t.isBypassable());
    }
}

TEST_CASE("transpose: sines from B0 to E6 land within a cent, at the input's level") {
  double worst = 0.0, worstDb = 0.0;
  for (double rate : {44100.0, 48000.0, 96000.0})
    for (Mode mode : kModes)
      for (const Note& note : kNotes)
        for (int s : {-12, -5, -2, 2, 12}) {
          Transposer t;
          setUp(t, rate, mode, s);
          const std::vector<float> x = sine(note.hz, rate, 2.5, 0.5);
          const std::vector<float> y = run(t, x, 128);
          const size_t from = static_cast<size_t>(1.0 * rate), to = x.size();
          const double want = note.hz * std::pow(2.0, s / 12.0);
          const double error = cents(frequency(y, from, to, want, rate), want);
          const double level = db(rms(y, from, to) / rms(x, from, to));
          worst = std::max(worst, std::fabs(error));
          worstDb = std::max(worstDb, std::fabs(level));
          INFO(name(mode) << " " << note.name << " " << s << " st at " << rate << ": " << error << " cents, " << level << " dB");
          CHECK(std::fabs(error) <= 1.0);
          CHECK(std::fabs(level) <= 1.0);
        }
  std::printf("transpose: sines B0-E6, -12..+12 st, 44.1/48/96 kHz: worst %.3f cents, worst level %.2f dB\n", worst, worstDb);
}

TEST_CASE("transpose: a shifted onset comes out latencySamples() later") {
  double worst = 0.0;
  for (double rate : {44100.0, 48000.0, 96000.0})
    for (Mode mode : kModes)
      for (int s : {-12, -5, -2, 2, 12}) {
        // silence, then 24 inharmonic partials (300 Hz - 4 kHz) that start within 0.5 ms: an envelope that does not
        // depend on where in a period it is read
        std::vector<float> x(static_cast<size_t>(0.5 * rate), 0.0f);
        const size_t onset = static_cast<size_t>(0.25 * rate);
        double hz[24], phase[24];
        uint32_t seed = 21;
        for (int h = 0; h < 24; ++h) {
          seed = seed * 1664525u + 1013904223u;
          hz[h] = 300.0 * std::pow(4000.0 / 300.0, (seed >> 8) / double(1 << 24));
          seed = seed * 1664525u + 1013904223u;
          phase[h] = 2 * kPi * ((seed >> 8) / double(1 << 24));
        }
        for (size_t i = onset; i < x.size(); ++i) {
          const double t = static_cast<double>(i - onset) / rate, ramp = std::min(1.0, t / 0.0005);
          double v = 0.0;
          for (int h = 0; h < 24; ++h) v += std::sin(2 * kPi * hz[h] * t + phase[h]) * 300.0 / hz[h];
          x[i] = static_cast<float>(0.05 * ramp * v);
        }
        Transposer t;
        setUp(t, rate, mode, s);
        const std::vector<float> y = run(t, x);
        const double in = halfRise(x, onset - static_cast<size_t>(0.01 * rate), rate);
        const double out = halfRise(y, onset - static_cast<size_t>(0.01 * rate), rate);
        const double lag = out - in, ms = 1000.0 * (lag - t.latencySamples()) / rate;
        INFO(name(mode) << " " << s << " st at " << rate << ": onset " << lag << " samples later, latency "
                        << t.latencySamples() << " (" << ms << " ms off)");
        // The synthesis window spreads an attack over its own length, and where in it the half-level point falls
        // depends on the phases the partials arrive with: it may move by a fifth of the latency in Tight and by 30 %
        // in Standard. (Standard was within a fifth too while onsets reset the partials' accumulated phases; the
        // reset was removed, as it made the attack sound worse through a high-gain amp: Standard -12 st now lands
        // 29 % late at 44.1 kHz, every other case within 20 %.)
        CHECK(std::fabs(lag - t.latencySamples()) <= (mode == Mode::Tight ? 0.2 : 0.3) * t.latencySamples());
        worst = std::max(worst, std::fabs(lag - t.latencySamples()) / t.latencySamples());
      }
  std::printf("transpose: onsets land within %.1f %% of the latency of where it says\n", 100.0 * worst);
}

TEST_CASE("transpose: changing the shift under a note crossfades without an edge (the block-switch limit)") {
  // test_automation.cpp's switch criterion (G-E2): nothing above 4 kHz within 80 dB of the tone, in any 10 ms frame
  const double rate = 48000.0;
  const int changes[][2] = {{0, -5}, {-5, 7}, {7, 0}, {-12, 12}, {-2, -3}, {3, 0}};
  for (Mode mode : kModes)
    for (const auto& c : changes) {
      Transposer t;
      setUp(t, rate, mode, c[0]);
      const std::vector<float> x = sine(220.0, rate, 1.2, 0.25);
      std::vector<float> y(x.size());
      const size_t at = static_cast<size_t>(0.6 * rate);
      for (size_t n = 0; n < x.size(); n += 64) {
        if (n == at - at % 64) t.setSemitones(c[1]);
        t.process(x.data() + n, y.data() + n, static_cast<int>(std::min<size_t>(64, x.size() - n)));
      }
      const double before = rms(y, static_cast<size_t>(0.3 * rate), static_cast<size_t>(0.55 * rate));
      const double after = rms(y, static_cast<size_t>(0.9 * rate), x.size());
      const double tone = std::sqrt(2.0) * std::min(before, after);
      const double worst = db(worstAbove(y, static_cast<size_t>(0.2 * rate), 4000.0, rate) / tone);
      // The edge criterion: -80 dB, or no more than 1 dB above what the two shifts leave when held (a pure tone
      // shifted up in Tight leaves -73 to -80 dB above 4 kHz without any switch; see the held-shift case below).
      const double held = std::max(heldAbove4k(mode, c[0]), heldAbove4k(mode, c[1]));
      std::printf("transpose: %-8s %+3d -> %+3d st: above 4 kHz %6.1f dB re the tone (held %6.1f, limit %6.1f)\n", name(mode),
                  c[0], c[1], worst, held, std::max(-80.0, held + 1.0));
      INFO(name(mode) << " " << c[0] << " -> " << c[1] << ", held " << held);
      CHECK(worst < std::max(-80.0, held + 1.0));
      CHECK(std::fabs(db(after / before)) < 1.0);
      // and the new shift is the one heard
      const double want = 220.0 * std::pow(2.0, c[1] / 12.0);
      CHECK(std::fabs(cents(frequency(y, static_cast<size_t>(0.75 * rate), x.size(), want, rate), want)) < 1.0);
    }
}

TEST_CASE("transpose: a shift automated every few milliseconds stays smooth and ends on the last value") {
  const double rate = 48000.0;
  for (Mode mode : kModes) {
    Transposer t;
    setUp(t, rate, mode, 0);
    const std::vector<float> x = sine(220.0, rate, 2.0, 0.25);
    std::vector<float> y(x.size());
    uint32_t seed = 9;
    int last = 0;
    for (size_t n = 0; n < x.size(); n += 64) {
      if (n > static_cast<size_t>(0.3 * rate) && n < static_cast<size_t>(1.0 * rate) && n % 192 == 0) {
        seed = seed * 1664525u + 1013904223u;
        last = static_cast<int>(seed >> 16) % 25 - 12;
        t.setSemitones(last);
      }
      t.process(x.data() + n, y.data() + n, static_cast<int>(std::min<size_t>(64, x.size() - n)));
    }
    REQUIRE(clean(y));
    const double tone = std::sqrt(2.0) * rms(y, static_cast<size_t>(0.1 * rate), static_cast<size_t>(0.3 * rate));
    const double worst = db(worstAbove(y, static_cast<size_t>(0.2 * rate), 4000.0, rate) / tone);
    // as for a single switch: -80 dB, or within 1 dB of the worst held shift
    double held = -200.0;
    for (int s = -12; s <= 12; ++s) held = std::max(held, heldAbove4k(mode, s));
    std::printf("transpose: %-8s random shifts every 4 ms: above 4 kHz %6.1f dB re the tone (worst held shift %6.1f)\n",
                name(mode), worst, held);
    CHECK(worst < std::max(-80.0, held + 1.0));
    float big = 0.0f;
    for (float v : y) big = std::max(big, std::fabs(v));
    CHECK(big < 0.25f * 1.5f);
    const double want = 220.0 * std::pow(2.0, last / 12.0);
    CHECK(std::fabs(cents(frequency(y, static_cast<size_t>(1.4 * rate), x.size(), want, rate), want)) < 1.0);
  }
}

TEST_CASE("transpose: back at 0 semitones the delay path is exact again, and the bypass hint follows") {
  const double rate = 48000.0;
  for (Mode mode : kModes) {
    Transposer t;
    setUp(t, rate, mode, 0);
    const std::vector<float> x = riff(rate, 1.5);
    std::vector<float> y(x.size());
    bool bypassedWhileShifted = false;
    for (size_t n = 0; n < x.size(); n += 64) {
      if (n == 9600) t.setSemitones(-5);
      if (n == 38400) t.setSemitones(0);
      t.process(x.data() + n, y.data() + n, static_cast<int>(std::min<size_t>(64, x.size() - n)));
      if (n > 12000 && n < 38400) bypassedWhileShifted = bypassedWhileShifted || t.isBypassable();
    }
    CHECK_FALSE(bypassedWhileShifted);
    CHECK(t.isBypassable());
    const size_t L = static_cast<size_t>(t.latencySamples());
    bool exact = true;
    for (size_t i = 38400 + 4800; i < x.size(); ++i) exact = exact && y[i] == x[i - L];
    CHECK(exact);
  }
}

TEST_CASE("transpose: a held shift leaves a 220 Hz tone clean above 4 kHz (the switch tests' floor)") {
  // Peaks count down to 80 dB below the frame's largest bin. On a pure tone shifted up in Tight that also moves some of
  // the short window's own sidelobes, which leaves -73 to -80 dB above 4 kHz (they were under the former -60 dB
  // floor). Everything else stays under the switch criterion's -80 dB.
  for (Mode mode : kModes)
    for (int s : {-12, -7, -5, -2, 2, 5, 7, 12}) {
      const double held = heldAbove4k(mode, s);
      const double limit = mode == Mode::Tight && s > 2 ? -72.0 : -80.0;
      std::printf("transpose: %-8s %+3d st held: above 4 kHz %6.1f dB re the tone (limit %.0f)\n", name(mode), s, held, limit);
      INFO(name(mode) << " " << s << " st");
      CHECK(held < limit);
    }
}

TEST_CASE("transpose: a partial 70 dB below the strongest moves to its own shifted frequency") {
  // A 220 Hz tone with a partial at 5 kHz, 70 dB below it: the partial is a peak of its own (the floor is 80 dB below
  // the frame's largest bin), so it moves by its own ratio, to 5 kHz * r, and not with the 220 Hz region (to
  // 5 kHz + 220 Hz * (r - 1)), as it did when the floor was 60 dB. Also the pitch accuracy of Tight's zero-padded FFT
  // high up, at every rate.
  const double quiet = 0.5 * std::pow(10.0, -70.0 / 20.0);
  double worstCents = 0.0, worstDb = 0.0;
  for (double rate : {44100.0, 48000.0, 96000.0})
    for (Mode mode : kModes)
      for (int s : {-12, -5, -2, 3}) {
        std::vector<float> x(static_cast<size_t>(2.0 * rate));
        for (size_t i = 0; i < x.size(); ++i) {
          const double t = static_cast<double>(i) / rate;
          x[i] = static_cast<float>(0.5 * std::sin(2 * kPi * 220.0 * t + 0.3) + quiet * std::sin(2 * kPi * 5000.0 * t + 1.1));
        }
        Transposer t;
        setUp(t, rate, mode, s);
        const std::vector<float> y = run(t, x, 128);
        const double r = std::pow(2.0, s / 12.0), want = 5000.0 * r, wrong = 5000.0 + 220.0 * (r - 1.0);
        const size_t from = static_cast<size_t>(0.5 * rate), to = y.size();
        const double error = cents(frequency(y, from, to, want, rate), want);
        const double level = db(amplitudeAt(y, from, to, want, rate) / quiet);
        const double left = db(amplitudeAt(y, from, to, wrong, rate) / quiet);
        worstCents = std::max(worstCents, std::fabs(error));
        worstDb = std::max(worstDb, std::fabs(level));
        INFO(name(mode) << " " << s << " st at " << rate << ": " << error << " cents, " << level << " dB, at the unshifted "
                        << "place " << left << " dB");
        CHECK(std::fabs(error) <= 1.0);
        CHECK(std::fabs(level) <= 1.5);
        CHECK(left < -20.0);
      }
  std::printf("transpose: a -70 dB partial at 5 kHz, -12..+3 st, 44.1/48/96 kHz: worst %.3f cents, worst level %.2f dB\n",
              worstCents, worstDb);
}

TEST_CASE("transpose: no absolute floor, the result does not depend on the input's level") {
  // Scaling the input by a power of two scales every spectrum exactly, and nothing in the transposer compares with an
  // absolute level (above the input's 1e-20 limit), so the output scales exactly too: a riff at -120 dB comes out as
  // the same riff, 120 dB down, bit for bit.
  const double rate = 48000.0;
  const std::vector<float> x = riff(rate, 1.0);
  const float scale = std::ldexp(1.0f, -20);
  std::vector<float> quiet(x);
  for (float& v : quiet) v *= scale;
  for (Mode mode : kModes) {
    Transposer a, b;
    setUp(a, rate, mode, -5);
    setUp(b, rate, mode, -5);
    const std::vector<float> loud = run(a, x), soft = run(b, quiet);
    bool exact = true;
    for (size_t i = 0; i < x.size(); ++i) exact = exact && soft[i] == loud[i] * scale;
    INFO(name(mode));
    CHECK(exact);
    CHECK(rms(soft, 24000, soft.size()) > 0.0);
  }
}

TEST_CASE("transpose: silence, DC, full-scale noise, NaN and Inf give finite output with no subnormals") {
  const double rate = 48000.0;
  for (Mode mode : kModes)
    for (int s : {0, -12, -5, 7, 12}) {
      INFO(name(mode) << " " << s << " st");
      Transposer t;
      setUp(t, rate, mode, s);
      // silence: exactly zero
      std::vector<float> y = run(t, std::vector<float>(24000, 0.0f));
      CHECK(std::all_of(y.begin(), y.end(), [](float v) { return v == 0.0f; }));
      // subnormal input is silence
      y = run(t, std::vector<float>(24000, 1e-40f));
      CHECK(std::all_of(y.begin(), y.end(), [](float v) { return v == 0.0f; }));
      // DC
      y = run(t, std::vector<float>(48000, 0.5f));
      CHECK(clean(y));
      // full-scale noise
      std::vector<float> noise(96000);
      uint32_t seed = 17;
      for (float& v : noise) {
        seed = seed * 1664525u + 1013904223u;
        v = static_cast<float>(2.0 * ((seed >> 8) / double(1 << 24) - 0.5));
      }
      y = run(t, noise);
      CHECK(clean(y));
      float big = 0.0f;
      for (float v : y) big = std::max(big, std::fabs(v));
      CHECK(big < 8.0f);
      // NaN, Inf and huge values inside a note
      std::vector<float> x = sine(110.0, rate, 1.0, 0.3);
      for (size_t i = 9000; i < 9100; ++i) x[i] = std::numeric_limits<float>::quiet_NaN();
      x[12000] = std::numeric_limits<float>::infinity();
      x[12001] = -std::numeric_limits<float>::infinity();
      x[15000] = 1e30f;
      y = run(t, x);
      CHECK(clean(y));
      for (float v : y) big = std::max(big, std::fabs(v));
      CHECK(big < 1e4f);
      // and it recovers: after the input has been clean for long enough, the note is back at its level
      const std::vector<float> after = run(t, sine(110.0, rate, 1.0, 0.3));
      CHECK(clean(after));
      CHECK(std::fabs(db(rms(after, 24000, 48000) / (0.3 / std::sqrt(2.0)))) < 1.0);
      // after the input stops, the output decays to exactly zero
      y = run(t, std::vector<float>(24000, 0.0f));
      CHECK(clean(y));
      CHECK(std::all_of(y.begin() + 12000, y.end(), [](float v) { return v == 0.0f; }));
    }
}

TEST_CASE("transpose: the output does not depend on the block size (0, 1, odd, larger than prepared)") {
  const double rate = 48000.0;
  const std::vector<float> x = riff(rate, 1.0);
  for (Mode mode : kModes) {
    Transposer ref;
    setUp(ref, rate, mode, -5, 64);
    const std::vector<float> want = run(ref, x, 64);
    for (int block : {1, 7, 85, 333, 4096}) {
      Transposer t;
      setUp(t, rate, mode, -5, 64);
      std::vector<float> y = run(t, x, block);
      INFO(name(mode) << " block " << block);
      CHECK(y == want);
    }
    // irregular blocks, with empty calls and in-place processing
    Transposer t;
    setUp(t, rate, mode, -5, 64);
    std::vector<float> y(x);
    size_t at = 0;
    uint32_t seed = 5;
    while (at < y.size()) {
      seed = seed * 1664525u + 1013904223u;
      const int n = static_cast<int>(std::min<size_t>((seed >> 20) % 700, y.size() - at));
      t.process(y.data() + at, y.data() + at, n);
      at += static_cast<size_t>(n);
    }
    CHECK(y == want);
  }
}

TEST_CASE("transpose: process, setSemitones, setMode and reset allocate nothing; shifts may be set from another thread") {
  Transposer t;
  setUp(t, 48000.0, Mode::Tight, -2, 256);
  const std::vector<float> x = riff(48000.0, 0.5);
  std::vector<float> y(x.size());
  gAllocations = 0;
  gCounting = true;
  for (int pass = 0; pass < 4; ++pass) {
    for (size_t n = 0; n < x.size(); n += 256) {
      if (n % 4096 == 0) t.setSemitones(static_cast<int>(n / 4096) % 25 - 12);
      t.process(x.data() + n, y.data() + n, static_cast<int>(std::min<size_t>(256, x.size() - n)));
    }
    t.setMode(pass % 2 ? Mode::Tight : Mode::Standard);
    t.reset();
  }
  gCounting = false;
  CHECK(gAllocations.load() == 0);

  // a control thread writes while the audio thread runs
  std::atomic<bool> stop{false};
  std::thread control([&] {
    int s = 0;
    while (!stop.load()) {
      t.setSemitones(s = (s + 7) % 25 - 12);
      std::this_thread::yield();
    }
    t.setSemitones(4);
  });
  for (int pass = 0; pass < 8; ++pass) run(t, x, 128);
  stop = true;
  control.join();
  const std::vector<float> tone = sine(220.0, 48000.0, 1.5, 0.3);
  const std::vector<float> out = run(t, tone, 128);
  CHECK(clean(out));
  const double want = 220.0 * std::pow(2.0, 4 / 12.0);
  CHECK(std::fabs(cents(frequency(out, 36000, out.size(), want, 48000.0), want)) < 1.0);
}

TEST_CASE("transpose: reset() starts at the target shift with no fade; setMode() keeps the target") {
  for (Mode mode : kModes) {
    Transposer t;
    setUp(t, 48000.0, mode, -7);
    run(t, riff(48000.0, 0.3));
    t.reset();
    const std::vector<float> x = sine(196.0, 48000.0, 1.5, 0.3);
    const std::vector<float> y = run(t, x);
    const double want = 196.0 * std::pow(2.0, -7 / 12.0);
    CHECK(std::fabs(cents(frequency(y, 12000, y.size(), want, 48000.0), want)) < 1.0);
    t.setMode(mode == Mode::Tight ? Mode::Standard : Mode::Tight);
    const std::vector<float> z = run(t, x);
    CHECK(std::fabs(cents(frequency(z, 12000, z.size(), want, 48000.0), want)) < 1.0);
  }
}

TEST_CASE("transpose: CPU at 48 kHz on a riff (one thread, 64-sample blocks)") {
  const double rate = 48000.0, seconds = 8.0;
  const std::vector<float> x = riff(rate, seconds);
  std::vector<float> y(x.size());
  for (Mode mode : kModes) {
    double best = 1e9;
    for (int trial = 0; trial < 3; ++trial) {
      Transposer t;
      setUp(t, rate, mode, -5, 64);
      const auto t0 = std::chrono::steady_clock::now();
      for (size_t n = 0; n < x.size(); n += 64) t.process(x.data() + n, y.data() + n, 64);
      best = std::min(best, std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
    }
    const double percent = 100.0 * best / seconds;
    std::printf("transpose: %-8s CPU %.3f %% of one core at 48 kHz (-5 st)\n", name(mode), percent);
#if defined(__APPLE__) && defined(__aarch64__) && defined(NDEBUG) && !defined(MAGMORA_SANITIZED)
    // the target on the development machine (Apple M4 Pro); other machines, and CI runners, print only.
    // Tight's FFT is twice the analysis length (zero padding), which about doubles its cost
    if (std::getenv("CI") == nullptr) CHECK(percent <= (mode == Mode::Tight ? 2.0 : 0.6));
#endif
  }
}
