// The tuner's pitch detection, offline: clean tones and a plucked-string-like signal from A0 to E6 at 44.1, 48 and
// 96 kHz, through the same decimating feed the plug-in's audio thread uses.
#include <algorithm>
#include <magmora/Tuner.h>

#include <catch.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

using namespace magmora;

namespace {

constexpr double kPi = 3.14159265358979323846;

struct Note {
  const char* name;
  double hz;
};
// Equal temperament from A4 = 440 Hz: 7-string low A0 and B0, the six E's of a guitar's range.
const Note kNotes[] = {{"A0", 27.5}, {"B0", 30.8677}, {"E1", 41.2034}, {"E2", 82.4069},
                       {"E3", 164.8138}, {"E4", 329.6276}, {"E5", 659.2551}, {"E6", 1318.5102}};

double cents(double hz, double ref) { return 1200.0 * std::log2(hz / ref); }

// A plucked string as a guitar DI records it: a short noisy attack, slightly inharmonic partials that die away faster
// the higher they are, a weak fundamental on the low strings, and the pickup's gentle high cut.
std::vector<double> pluck(double f0, double rate, double seconds, uint32_t seed) {
  const size_t n = static_cast<size_t>(seconds * rate);
  std::vector<double> x(n, 0.0);
  const double inharmonicity = 8e-5;
  for (int h = 1; h <= 16; ++h) {
    const double f = h * f0 * std::sqrt(1.0 + inharmonicity * h * h);
    if (f > 0.45 * rate) break;
    const double amp = (f0 < 100.0 && h == 1 ? 0.35 : 1.0) / h * (h % 7 == 0 ? 0.2 : 1.0);
    const double decay = 1.2 + 0.9 * h;
    const double phase = 0.37 * h * h;
    for (size_t i = 0; i < n; ++i) {
      const double t = static_cast<double>(i) / rate;
      x[i] += amp * std::exp(-decay * t) * std::sin(2 * kPi * f * t + phase);
    }
  }
  double lp = 0.0;
  const double a = 1.0 - std::exp(-2 * kPi * 3500.0 / rate);
  for (size_t i = 0; i < n; ++i) {
    seed = seed * 1664525u + 1013904223u;
    const double noise = ((seed >> 8) / double(1 << 24) - 0.5) * std::exp(-static_cast<double>(i) / (0.004 * rate));
    lp += a * (x[i] + 0.5 * noise - lp);
    x[i] = 0.25 * lp;
  }
  return x;
}

std::vector<double> sine(double f0, double rate, double seconds, double level) {
  std::vector<double> x(static_cast<size_t>(seconds * rate));
  for (size_t i = 0; i < x.size(); ++i) x[i] = level * std::sin(2 * kPi * f0 * static_cast<double>(i) / rate + 0.3);
  return x;
}

// Feeds `x` in host blocks and returns the detector's reading from the newest window after `atSeconds`.
Pitch measure(const std::vector<double>& x, double rate, double atSeconds) {
  TunerFeed feed;
  feed.prepare(rate);
  const size_t stop = std::min(x.size(), static_cast<size_t>(atSeconds * rate));
  for (size_t at = 0; at < stop; at += 256) feed.push(x.data() + at, static_cast<int>(std::min<size_t>(256, stop - at)));
  PitchDetector detector;
  const int n = detector.windowSamples(feed.rate());
  std::vector<float> window(static_cast<size_t>(n));
  if (!feed.latest(window.data(), n)) return {};
  return detector.detect(window.data(), n, feed.rate());
}

}  // namespace

TEST_CASE("tuner: clean tones from A0 to E6 read within a cent at 44.1, 48 and 96 kHz") {
  double worst = 0.0;
  for (double rate : {44100.0, 48000.0, 96000.0})
    for (const Note& note : kNotes)
      for (double detune : {-23.0, 0.0, 7.0}) {
        const double hz = note.hz * std::pow(2.0, detune / 1200.0);
        const Pitch p = measure(sine(hz, rate, 0.6, 0.3), rate, 0.6);
        INFO(note.name << " " << detune << " cents at " << rate << " Hz: read " << p.hz);
        REQUIRE(p.found);
        const double error = cents(p.hz, hz);
        worst = std::max(worst, std::fabs(error));
        CHECK(std::fabs(error) <= 1.0);
        const NoteReading r = nearestNote(p.hz, 440.0);
        CHECK(std::string(noteName(r.midi)) + std::to_string(noteOctave(r.midi)) == note.name);
        CHECK(std::fabs(r.cents - detune) <= 1.0);
      }
  std::printf("tuner: clean tones, worst error %.3f cents\n", worst);
}

TEST_CASE("tuner: a plucked string reads the right note, stably, through its decay") {
  double worst = 0.0;
  for (double rate : {44100.0, 48000.0, 96000.0})
    for (const Note& note : kNotes) {
      const std::vector<double> x = pluck(note.hz, rate, 2.0, 7u + static_cast<uint32_t>(note.hz));
      for (double at : {0.35, 0.8, 1.5}) {
        const Pitch p = measure(x, rate, at);
        INFO(note.name << " at " << rate << " Hz, " << at << " s: read " << p.hz << " clarity " << p.clarity);
        REQUIRE(p.found);
        const NoteReading r = nearestNote(p.hz, 440.0);
        CHECK(std::string(noteName(r.midi)) + std::to_string(noteOctave(r.midi)) == note.name);
        // The partials' inharmonicity pulls a real string's pitch a little sharp; a tuner reads that, as it should.
        const double error = cents(p.hz, note.hz);
        worst = std::max(worst, std::fabs(error));
        CHECK(std::fabs(error) <= 3.0);
      }
    }
  std::printf("tuner: plucked strings, worst error %.3f cents\n", worst);
}

TEST_CASE("tuner: silence and noise give no note; the reference pitch moves the reading") {
  const double rate = 48000.0;
  CHECK_FALSE(measure(std::vector<double>(48000, 0.0), rate, 1.0).found);
  std::vector<double> noise(48000);
  uint32_t seed = 5;
  for (double& v : noise) {
    seed = seed * 1664525u + 1013904223u;
    v = 0.2 * ((seed >> 8) / double(1 << 24) - 0.5);
  }
  const Pitch p = measure(noise, rate, 1.0);
  CHECK((!p.found || p.clarity < 0.7));
  // 440 Hz against A = 432: A4 sharp by 31.8 cents
  const NoteReading r = nearestNote(440.0, 432.0);
  CHECK(r.midi == 69);
  CHECK(r.cents == Approx(31.77).margin(0.05));
  CHECK(nearestNote(440.0, 450.0).cents == Approx(-38.9).margin(0.05));
  CHECK(std::string(noteName(59)) == "B");
  CHECK(noteOctave(23) == 0);  // B0
}

TEST_CASE("tuner: the feed is a lock-free ring a reader can copy while the audio thread writes") {
  TunerFeed feed;
  feed.prepare(44100.0);
  CHECK(feed.rate() == Approx(11025.0));
  feed.prepare(96000.0);
  CHECK(feed.rate() == Approx(96000.0 / 9));
  float out[64];
  CHECK_FALSE(feed.latest(out, 64));  // nothing yet
  // A writer and a reader at once (run under TSan too): every copy that succeeds is a contiguous run of the ramp.
  feed.prepare(11025.0);  // no decimation: what goes in comes out
  std::atomic<bool> stop{false};
  std::thread writer([&] {
    std::vector<double> block(64);
    double next = 0.0;
    for (int b = 0; b < 20000; ++b) {
      for (double& v : block) v = next++;
      feed.push(block.data(), 64);
    }
    stop = true;
  });
  int good = 0, torn = 0;
  std::vector<float> copy(1024);
  while (!stop) {
    if (!feed.latest(copy.data(), 1024)) continue;
    ++good;
    for (size_t i = 1; i < copy.size(); ++i)
      if (copy[i] - copy[i - 1] != 1.f && copy[i] < 16777216.f) {
        ++torn;
        break;
      }
  }
  writer.join();
  CHECK(good > 0);
  CHECK(torn == 0);
  static_assert(std::atomic<float>::is_always_lock_free && std::atomic<uint64_t>::is_always_lock_free, "lock-free");
}
