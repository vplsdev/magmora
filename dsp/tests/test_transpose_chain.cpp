// The pitch transposer in the chain (added 2026-11-24): latency, the 0-semitone path (no transposer, bit for bit),
// click-free changes of path and mode by the switch criterion of test_automation.cpp (G-E2), automation between
// shifts, routing, state, presets and old sessions.
#include <magmora/Chain.h>
#include <magmora/Preset.h>

#include <catch.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

using namespace magmora;

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr int kBlock = 64;

double db(double x) { return 20.0 * std::log10(x + 1e-30); }

// The transposer's delay per rate and mode (dsp/src/transpose/Transposer.h: L).
int expectedDelay(double rate, TransposeMode mode) {
  const bool tight = mode == TransposeMode::Tight;
  if (rate == 44100.0) return tight ? 234 : 472;
  if (rate == 48000.0) return tight ? 255 : 512;
  if (rate == 96000.0) return tight ? 513 : 1024;
  return -1;
}

std::vector<double> sine(double rate, double hz, double level, int samples) {
  std::vector<double> x(static_cast<size_t>(samples));
  for (int n = 0; n < samples; ++n) x[static_cast<size_t>(n)] = level * std::sin(2 * kPi * hz * n / rate + 0.3);
  return x;
}

std::vector<float> playing(double rate, int samples, double level = 0.4) {
  std::vector<float> x(static_cast<size_t>(samples));
  for (int n = 0; n < samples; ++n) {
    const double t = n / rate;
    x[static_cast<size_t>(n)] = static_cast<float>(
        level * std::exp(-std::fmod(t, 0.1) * 25.0) * (std::sin(2 * kPi * 82.4 * t) + 0.4 * std::sin(2 * kPi * 247.0 * t)));
  }
  return x;
}

// Mono in, the left output; `at(n)` is called before the block that starts at sample n.
template <class At>
std::vector<double> play(Chain& c, const std::vector<double>& x, At&& at, int block = kBlock) {
  std::vector<double> y(x.size()), right(static_cast<size_t>(block));
  for (size_t n = 0; n < x.size(); n += static_cast<size_t>(block)) {
    at(n);
    const int frames = static_cast<int>(std::min(x.size() - n, static_cast<size_t>(block)));
    const double* in[1] = {x.data() + n};
    double* out[2] = {y.data() + n, right.data()};
    c.process(in, out, frames);
  }
  return y;
}

void setTranspose(Chain& c, int semitones, TransposeMode mode, bool apply = true) {
  c.setParameter(kTranspose, semitones);
  c.setParameter(kTransposeMode, static_cast<double>(mode));
  if (apply) c.applyTransposeStructure();
}

// test_automation.cpp's switch criterion (G-E2): the largest component at or above `fromHz` in any 10 ms frame
// (Blackman window, the frame's mean and slope removed), from sample `from` on.
double worstAbove(const std::vector<double>& y, size_t from, double fromHz, double rate) {
  const int frame = static_cast<int>(0.010 * rate);
  std::vector<double> window(static_cast<size_t>(frame)), f(static_cast<size_t>(frame));
  double windowSum = 0.0;
  for (int n = 0; n < frame; ++n) {
    window[static_cast<size_t>(n)] = 0.42 - 0.5 * std::cos(2 * kPi * n / frame) + 0.08 * std::cos(4 * kPi * n / frame);
    windowSum += window[static_cast<size_t>(n)];
  }
  const int firstBin = std::max(1, static_cast<int>(fromHz * frame / rate));
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
      f[static_cast<size_t>(n)] = (y[at + static_cast<size_t>(n)] - mean - slope * (n - 0.5 * (frame - 1))) * window[static_cast<size_t>(n)];
    for (int k = firstBin; k < frame / 2; ++k) {
      std::complex<double> sum = 0.0;
      for (int n = 0; n < frame; ++n) sum += f[static_cast<size_t>(n)] * std::polar(1.0, -2 * kPi * k * n / frame);
      worst = std::max(worst, 2.0 * std::abs(sum) / windowSum);
    }
  }
  return worst;
}

// Amplitude of the component at `hz` in y[from, from + n).
double toneAt(const std::vector<double>& y, size_t from, size_t n, double hz, double rate) {
  std::complex<double> sum = 0.0;
  double wsum = 0.0;
  for (size_t i = 0; i < n; ++i) {
    const double w = 0.5 - 0.5 * std::cos(2 * kPi * (i + 0.5) / n);
    sum += w * y[from + i] * std::polar(1.0, -2 * kPi * hz * static_cast<double>(from + i) / rate);
    wsum += w;
  }
  return 2.0 * std::abs(sum) / wsum;
}

// The strongest frequency between lo and hi Hz in y[from, from + n), to a tenth of a cent (golden-section search on
// the windowed DFT magnitude after a coarse scan).
double strongest(const std::vector<double>& y, size_t from, size_t n, double lo, double hi, double rate) {
  double best = lo, bestV = 0.0;
  for (double hz = lo; hz <= hi; hz *= std::pow(2.0, 1.0 / 1200.0)) {
    const double v = toneAt(y, from, n, hz, rate);
    if (v > bestV) {
      bestV = v;
      best = hz;
    }
  }
  double a = best * std::pow(2.0, -1.0 / 1200.0), b = best * std::pow(2.0, 1.0 / 1200.0);
  for (int i = 0; i < 40; ++i) {
    const double m1 = b - (b - a) / 1.618, m2 = a + (b - a) / 1.618;
    if (toneAt(y, from, n, m1, rate) > toneAt(y, from, n, m2, rate)) b = m2;
    else a = m1;
  }
  return 0.5 * (a + b);
}

bool sameBits(const std::vector<float>& a, const std::vector<float>& b) {
  return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0;
}

// Stereo renders (left, then right): the right input is a different signal, so that every routing is exercised.
template <class Process>
std::vector<float> render(Process&& process, const std::vector<float>& x, int channels, int block) {
  std::vector<float> l(x), r(x.size());
  for (size_t n = 0; n < r.size(); ++n) r[n] = 0.5f * x[(n * 7) % x.size()];
  std::vector<float> right(static_cast<size_t>(block));
  for (size_t at = 0; at < x.size(); at += static_cast<size_t>(block)) {
    const int frames = static_cast<int>(std::min<size_t>(static_cast<size_t>(block), x.size() - at));
    float* io[2] = {l.data() + at, r.data() + at};
    float* outs[2] = {l.data() + at, channels == 2 ? r.data() + at : right.data()};
    process(io, outs, frames);
    if (channels == 1) std::copy_n(right.data(), frames, r.data() + at);
  }
  l.insert(l.end(), r.begin(), r.end());
  return l;
}

std::string readFile(const std::filesystem::path& p) {
  std::ifstream f(p, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

void cleanAmp(Chain& c) {
  c.setParameter(kChannel, 0.0);
  c.setParameter(kPreRhythm, 2.0);
}

}  // namespace

TEST_CASE("transpose in the chain: the parameters are appended, session settings, Tight and 0 st by default") {
  REQUIRE(kTranspose == kAmpOn + 1);
  REQUIRE(kTransposeMode == kAmpOn + 2);
  REQUIRE(kNumChainParams == kAmpOn + 3);
  const ParamInfo& t = *Chain::parameterInfo(kTranspose);
  const ParamInfo& m = *Chain::parameterInfo(kTransposeMode);
  CHECK(std::string(t.key) == "transpose");
  CHECK((t.min == -12.0 && t.max == 12.0 && t.def == 0.0 && t.taper == Taper::Stepped && t.automatable));
  CHECK(std::string(m.key) == "transpose_mode");
  CHECK((m.min == 0.0 && m.max == 1.0 && m.def == 0.0 && m.taper == Taper::Stepped && !m.automatable));
  CHECK(Chain::isSessionParameter(kTranspose));
  CHECK(Chain::isSessionParameter(kTransposeMode));
  CHECK_FALSE(isPresetParameter(kTranspose));
  CHECK_FALSE(isPresetParameter(kTransposeMode));
  Chain c;
  c.setParameter(kTranspose, 3.4);
  CHECK(c.getParameter(kTranspose) == 3.0);
  c.setParameter(kTranspose, -40.0);
  CHECK(c.getParameter(kTranspose) == -12.0);
}

TEST_CASE("transpose in the chain: latency is the head's plus the mode's delay, and exactly the head's at 0 st") {
  for (double rate : {44100.0, 48000.0, 96000.0})
    for (TransposeMode mode : {TransposeMode::Tight, TransposeMode::Standard})
      for (int quality = 0; quality < 2; ++quality) {
        INFO("rate " << rate << " mode " << static_cast<int>(mode) << " quality " << quality);
        Engine e;
        Chain c;
        e.setParam(kQuality, quality);
        c.setParameter(kQuality, quality);
        REQUIRE(e.prepare(rate, kBlock, 2));
        REQUIRE(c.prepare(rate, kBlock, 2));
        const int head = e.latencySamples(), delay = expectedDelay(rate, mode);
        CHECK(c.latencySamples() == head);
        setTranspose(c, 0, mode);  // the mode alone changes nothing at 0 st
        CHECK(c.latencySamples() == head);
        CHECK_FALSE(c.transposeInPath());
        setTranspose(c, 5, mode);
        CHECK(c.transposeInPath());
        CHECK(c.transposeLatencySamples() == delay);
        CHECK(c.latencySamples() == head + delay);
        // about 5.3 or 10.7 ms at any rate
        CHECK(std::fabs(1000.0 * delay / rate - (mode == TransposeMode::Tight ? 5.3 : 10.7)) < 0.1);
        for (int st : {-12, -1, 1, 7, 12}) {  // between non-zero values the latency never changes
          setTranspose(c, st, mode);
          CHECK(c.latencySamples() == head + delay);
        }
        c.applyStructuralParams();  // and a rebuild keeps it
        CHECK(c.latencySamples() == head + delay);
        setTranspose(c, 0, mode);
        CHECK(c.latencySamples() == head);
        CHECK(c.transposeLatencySamples() == 0);
        // set before prepare: prepare applies it
        Chain d;
        d.setParameter(kQuality, quality);
        setTranspose(d, -3, mode, false);
        REQUIRE(d.prepare(rate, kBlock, 2));
        CHECK(d.latencySamples() == head + delay);
      }
}

TEST_CASE("transpose in the chain: at 0 st nothing of the transposer is in the path, bit for bit") {
  struct Setup {
    double rate;
    int channels, mode, block;
  };
  for (const Setup& s : {Setup{48000.0, 2, 0, 64}, Setup{48000.0, 1, 0, 64}, Setup{44100.0, 2, 1, 100}, Setup{96000.0, 2, 2, 128}}) {
    INFO("rate " << s.rate << " channels " << s.channels << " routing " << s.mode);
    Engine e;
    Chain c, back;
    e.setParam(kChannelMode, s.mode);
    for (Chain* k : {&c, &back}) k->setParameter(kChannelMode, s.mode);
    c.setParameter(kTransposeMode, 1.0);  // a mode, at 0 st, is nothing
    REQUIRE(e.prepare(s.rate, s.block, s.channels));
    REQUIRE(c.prepare(s.rate, s.block, s.channels));
    REQUIRE(back.prepare(s.rate, s.block, s.channels));
    CHECK(c.latencySamples() == e.latencySamples());
    // `back` had the transposer in, playing, and took it out again: once the crossfade is over the path is the input
    const std::vector<float> warm = playing(s.rate, static_cast<int>(0.2 * s.rate));
    setTranspose(back, 7, TransposeMode::Standard);
    render([&](float* const* in, float* const* out, int n) { back.process(in, out, n); }, warm, s.channels, s.block);
    setTranspose(back, 0, TransposeMode::Standard);
    render([&](float* const* in, float* const* out, int n) { back.process(in, out, n); }, warm, s.channels, s.block);
    CHECK_FALSE(back.transposeSwitching());
    e.reset();
    back.reset();
    const std::vector<float> x = playing(s.rate, static_cast<int>(0.3 * s.rate));
    const auto engineOut = render([&](float* const* in, float* const* out, int n) {
      e.process(in, out, n);
      if (s.channels == 1) std::copy_n(out[0], n, out[1]);
    }, x, s.channels, s.block);
    CHECK(sameBits(engineOut, render([&](float* const* in, float* const* out, int n) { c.process(in, out, n); }, x, s.channels, s.block)));
    CHECK(sameBits(engineOut, render([&](float* const* in, float* const* out, int n) { back.process(in, out, n); }, x, s.channels, s.block)));
  }
  // and in double precision: the head is fed the input pointers themselves
  Engine e;
  Chain c;
  REQUIRE(e.prepare(48000.0, 64, 1));
  REQUIRE(c.prepare(48000.0, 64, 1));
  const std::vector<double> x = sine(48000.0, 110.0, 0.3, 9600);
  std::vector<double> a(x.size()), b(x.size()), right(64);
  for (size_t n = 0; n < x.size(); n += 64) {
    const double* in[1] = {x.data() + n};
    double* oa[1] = {a.data() + n};
    double* ob[2] = {b.data() + n, right.data()};
    e.process(in, oa, 64);
    c.process(in, ob, 64);
  }
  CHECK(std::memcmp(a.data(), b.data(), a.size() * sizeof(double)) == 0);
}

TEST_CASE("transpose in the chain: the head hears the shifted signal, on every routing") {
  const double rate = 48000.0;
  for (int routing = 0; routing < 3; ++routing)
    for (int st : {-12, -5, 7}) {
      INFO("routing " << routing << " " << st << " st");
      Chain c;
      c.setParameter(kAmpOn, 0.0);  // the head off: the input at unity, so the pitch can be read exactly
      c.setParameter(kChannelMode, routing);
      setTranspose(c, st, TransposeMode::Tight, false);
      REQUIRE(c.prepare(rate, kBlock, 2));
      const int n = static_cast<int>(1.0 * rate);
      std::vector<double> l = sine(rate, 220.0, 0.1, n), r = sine(rate, 220.0, 0.1, n), yl(l.size()), yr(l.size());
      for (size_t i = 0; i < r.size(); ++i) r[i] *= 0.5;  // the right channel at half the level
      for (size_t at = 0; at < l.size(); at += kBlock) {
        const double* in[2] = {l.data() + at, r.data() + at};
        double* out[2] = {yl.data() + at, yr.data() + at};
        c.process(in, out, kBlock);
      }
      const double want = 220.0 * std::pow(2.0, st / 12.0);
      const size_t from = static_cast<size_t>(0.4 * rate), len = static_cast<size_t>(0.5 * rate);
      const double got = strongest(yl, from, len, want * 0.97, want * 1.03, rate);
      CHECK(std::fabs(1200.0 * std::log2(got / want)) < 1.0);
      // the level the routing gives: left, the sum's half, or each side
      const double level = toneAt(yl, from, len, got, rate), expect = routing == 1 ? 0.075 : 0.1;
      CHECK(std::fabs(db(level / expect)) < 1.0);
      if (routing == 2) CHECK(std::fabs(db(toneAt(yr, from, len, got, rate) / 0.05)) < 1.0);
    }
}

// The switch criterion is applied where the switch happens: on the head's input, read with the head off (amp_on 0
// passes the input at unity through the same oversampled loop, a linear path). Through the clean amp the same
// switches are printed too: there the amp's treble lifts the transposer's own residue above 4 kHz against a 220 Hz
// tone (about 5 dB), so there the switch may not exceed the same switch with the head off by more than 10 dB.
namespace {
struct SwitchResult {
  double worst, held, level;  // dB re the tone: the run from before the switch on, and the new shift held; dB
};
template <class At>
SwitchResult switchRun(bool head, int fromSt, TransposeMode fromMode, double seconds, size_t switchAt, At&& at,
                       int toSt, int& latencyBefore, int& latencyAfter, bool& switching) {
  const double rate = 48000.0;
  Chain c;
  cleanAmp(c);
  c.setParameter(kAmpOn, head ? 1.0 : 0.0);
  setTranspose(c, fromSt, fromMode, false);
  REQUIRE(c.prepare(rate, kBlock, 1));
  latencyBefore = c.latencySamples();
  const std::vector<double> x = sine(rate, 220.0, 0.005, static_cast<int>(seconds * rate));
  const std::vector<double> y = play(c, x, [&](size_t n) { at(c, n); });
  latencyAfter = c.latencySamples();
  switching = c.transposeSwitching();
  const double fromHz = 220.0 * std::pow(2.0, fromSt / 12.0), toHz = 220.0 * std::pow(2.0, toSt / 12.0);
  const size_t len = static_cast<size_t>(0.2 * rate);
  const double was = toneAt(y, switchAt - len - 4800, len, fromHz, rate);
  const double now = toneAt(y, y.size() - len, len, toHz, rate);
  const double tone = std::min(was, now);
  const std::vector<double> held(y.end() - static_cast<long>(len), y.end());
  return {db(worstAbove(y, switchAt - len, 4000.0, rate) / tone), db(worstAbove(held, 0, 4000.0, rate) / tone), db(now / was)};
}

// What a shift leaves above 4 kHz with the head off when nothing changes (dB re the tone; -200 at 0 st, where the
// transposer is out of the path). A pure tone shifted up in Tight leaves -73 to -80 dB there by itself
// (test_transpose.cpp, "a held shift"): a switch is an edge only if it exceeds what the two shifts leave when held.
double heldFloor(int st, TransposeMode mode) {
  if (st == 0) return -200.0;
  int before = 0, after = 0;
  bool switching = false;
  const size_t at = static_cast<size_t>(0.6 * 48000.0) / kBlock * kBlock;
  return switchRun(false, st, mode, 1.2, at, [](Chain&, size_t) {}, st, before, after, switching).worst;
}
}  // namespace

TEST_CASE("transpose in the chain: in or out, and a change of mode, are click-free (G-E2's switch criterion)") {
  const double rate = 48000.0;
  struct Case {
    int fromSt;
    TransposeMode fromMode;
    int toSt;
    TransposeMode toMode;
    int applyLate;  // samples between the parameter and the structure (a host's idle timer)
  };
  const Case cases[] = {
      {0, TransposeMode::Tight, 3, TransposeMode::Tight, 0},        {0, TransposeMode::Standard, -5, TransposeMode::Standard, 0},
      {3, TransposeMode::Tight, 0, TransposeMode::Tight, 0},        {-5, TransposeMode::Standard, 0, TransposeMode::Standard, 0},
      {3, TransposeMode::Tight, 0, TransposeMode::Tight, 1600},     {0, TransposeMode::Tight, 4, TransposeMode::Tight, 1600},
      {-5, TransposeMode::Tight, -5, TransposeMode::Standard, 0},   {4, TransposeMode::Standard, 4, TransposeMode::Tight, 0},
      {0, TransposeMode::Tight, -12, TransposeMode::Standard, 0},   {12, TransposeMode::Standard, 0, TransposeMode::Standard, 0},
      {3, TransposeMode::Tight, 7, TransposeMode::Tight, 0},        {-2, TransposeMode::Standard, -9, TransposeMode::Standard, 0},
  };
  for (const Case& k : cases) {
    double headOff = 0.0;
    for (bool head : {false, true}) {
      const size_t at = static_cast<size_t>(0.6 * rate) / kBlock * kBlock;
      int before = 0, after = 0;
      bool switching = true;
      const SwitchResult r = switchRun(head, k.fromSt, k.fromMode, 1.2, at, [&](Chain& c, size_t n) {
        if (n == at) setTranspose(c, k.toSt, k.toMode, false);
        if (n == at + static_cast<size_t>(k.applyLate) / kBlock * kBlock) c.applyTransposeStructure();
      }, k.toSt, before, after, switching);
      std::printf("transpose chain, head %-3s: %+3d %-8s -> %+3d %-8s (structure %4d later): above 4 kHz %6.1f dB re the "
                  "tone, held %6.1f dB, level %+.2f dB\n",
                  head ? "on" : "off", k.fromSt, k.fromMode == TransposeMode::Tight ? "Tight" : "Standard", k.toSt,
                  k.toMode == TransposeMode::Tight ? "Tight" : "Standard", k.applyLate, r.worst, r.held, r.level);
      INFO((head ? "head on: " : "head off: ") << k.fromSt << " -> " << k.toSt << ", mode " << static_cast<int>(k.fromMode)
                                              << " -> " << static_cast<int>(k.toMode));
      if (!head) {
        headOff = r.worst;
        // G-E2's -80 dB, or within 1 dB of what the two shifts leave when held
        const double held = std::max(heldFloor(k.fromSt, k.fromMode), heldFloor(k.toSt, k.toMode));
        INFO("held " << held);
        CHECK(r.worst < std::max(-80.0, held + 1.0));
        CHECK(std::fabs(r.level) < 1.0);  // the head off: unity at every pitch
      } else {
        CHECK(r.worst < std::max(-80.0, headOff + 10.0));
      }
      CHECK_FALSE(switching);
      const bool structural = (k.fromSt != 0) != (k.toSt != 0) || (k.toSt != 0 && k.fromMode != k.toMode);
      if (!structural) CHECK(after == before);
    }
  }
}

TEST_CASE("transpose in the chain: automated between shifts while playing, no edge and no latency change") {
  const double rate = 48000.0;
  const int steps[] = {2, 5, -3, 7, -7, 1, -1, 4, 12, -12, 3};
  for (TransposeMode mode : {TransposeMode::Tight, TransposeMode::Standard}) {
    double headOff = 0.0;
    for (bool head : {false, true}) {
      bool steady = true;
      int before = 0, after = 0, latency = -1;
      bool switching = true;
      const size_t first = static_cast<size_t>(0.6 * rate) / kBlock * kBlock, every = static_cast<size_t>(0.2 * rate);
      const SwitchResult r = switchRun(head, 2, mode, 0.6 + 0.2 * std::size(steps) + 0.3, first, [&](Chain& c, size_t n) {
        if (latency < 0) latency = c.latencySamples();
        if (n >= first && (n - first) / every < std::size(steps)) c.setParameter(kTranspose, steps[(n - first) / every]);
        steady = steady && c.latencySamples() == latency && !c.transposeSwitching();
      }, 3, before, after, switching);
      std::printf("transpose chain, head %-3s: %s automated every 200 ms across -12..+12: above 4 kHz %6.1f dB re the tone, "
                  "held %6.1f dB\n", head ? "on" : "off", mode == TransposeMode::Tight ? "Tight" : "Standard", r.worst, r.held);
      INFO((head ? "head on, " : "head off, ") << static_cast<int>(mode));
      CHECK(steady);
      CHECK(after == before);
      if (!head) {
        headOff = r.worst;
        // G-E2's -80 dB, or within 1 dB of the worst of the shifts when held
        double held = heldFloor(2, mode);
        for (int st : steps) held = std::max(held, heldFloor(st, mode));
        INFO("worst held shift " << held);
        CHECK(r.worst < std::max(-80.0, held + 1.0));
      } else {
        CHECK(r.worst < std::max(-80.0, headOff + 10.0));
      }
    }
  }
}

TEST_CASE("transpose in the chain: state round trip, old sessions at 0 st Tight, presets never touch it") {
  Chain c;
  REQUIRE(c.prepare(48000.0, 64, 2));
  setTranspose(c, -4, TransposeMode::Standard);
  std::vector<uint8_t> state;
  c.getState(state);
  Chain d;
  REQUIRE(d.prepare(48000.0, 64, 2));
  REQUIRE(d.setState(state.data(), state.size()));
  CHECK(d.getParameter(kTranspose) == -4.0);
  CHECK(d.getParameter(kTransposeMode) == 1.0);
  CHECK_FALSE(d.transposeInPath());  // structural: it takes effect where the host applies it
  d.applyStructuralParams();
  CHECK(d.transposeInPath());
  CHECK(d.latencySamples() == c.latencySamples());
  std::vector<uint8_t> again;
  d.getState(again);
  CHECK(again == state);

  // a session from before the transposer: the A31 chain fixture and a bare 0.1.0 head chunk
  for (const char* name : {"chain-state-a31.bin", "head-state-0.1.0.bin"}) {
    const std::string bytes = readFile(std::filesystem::path(MAGMORA_SOURCE_DIR) / "dsp/tests/data" / name);
    REQUIRE(!bytes.empty());
    Chain old;
    setTranspose(old, 9, TransposeMode::Standard, false);  // whatever it was, the old session sets the defaults
    REQUIRE(old.setState(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()));
    INFO(name);
    CHECK(old.getParameter(kTranspose) == 0.0);
    CHECK(old.getParameter(kTransposeMode) == 0.0);
  }

  // presets: every factory preset leaves the transposer as it is
  int presets = 0;
  for (const auto& entry : std::filesystem::directory_iterator(std::filesystem::path(MAGMORA_SOURCE_DIR) / "presets")) {
    if (entry.path().extension() != ".json") continue;
    const std::string json = readFile(entry.path());
    CHECK(json.find("transpose") == std::string::npos);
    REQUIRE(c.applyPreset(json.c_str(), json.size()));
    CHECK(c.getParameter(kTranspose) == -4.0);
    CHECK(c.getParameter(kTransposeMode) == 1.0);
    ++presets;
  }
  CHECK(presets > 0);
  // a user preset neither writes them nor reads them
  UserPreset snapshot;
  double values[kNumChainParams];
  for (int id = 0; id < kNumChainParams; ++id) values[id] = c.getParameter(id);
  setPresetSnapshot(snapshot, values);
  const std::string text = writeUserPreset(snapshot);
  CHECK(text.find("transpose") == std::string::npos);
  const std::string hand = R"({"format": "magmora-preset", "version": 1, "name": "X", "params": {"transpose": 5, "transpose_mode": 1}})";
  UserPreset read;
  REQUIRE(readUserPreset(hand.c_str(), hand.size(), read));
  CHECK_FALSE(read.given[kTranspose]);
  CHECK_FALSE(read.given[kTransposeMode]);
  REQUIRE(c.applyPreset(hand.c_str(), hand.size()));
  CHECK(c.getParameter(kTranspose) == -4.0);
}

TEST_CASE("transpose in the chain: the path may change from another thread while audio runs") {
  Chain c;
  cleanAmp(c);
  REQUIRE(c.prepare(48000.0, 128, 2));
  std::atomic<bool> stop{false};
  std::atomic<long> blocks{0};
  bool finite = true;
  std::thread audio([&] {
    std::vector<float> l(128), r(128);
    long n = 0;
    while (!stop.load()) {
      for (int i = 0; i < 128; ++i) l[static_cast<size_t>(i)] = r[static_cast<size_t>(i)] = static_cast<float>(0.2 * std::sin(0.03 * static_cast<double>(n++)));
      float* io[2] = {l.data(), r.data()};
      c.process(io, io, 128);
      for (float v : l) finite = finite && std::isfinite(v) && std::fabs(v) <= 8.0f;
      blocks.fetch_add(1);
    }
  });
  const int pattern[][2] = {{3, 0}, {0, 0}, {-5, 1}, {7, 0}, {0, 1}, {12, 1}, {-12, 0}, {0, 0}};
  for (int round = 0; round < 40; ++round) {
    const auto& p = pattern[round % std::size(pattern)];
    setTranspose(c, p[0], static_cast<TransposeMode>(p[1]));
    const long until = blocks.load() + 1 + round % 5;
    while (blocks.load() < until) std::this_thread::yield();
  }
  setTranspose(c, 0, TransposeMode::Tight);
  const long until = blocks.load() + 40;
  while (blocks.load() < until) std::this_thread::yield();
  stop.store(true);
  audio.join();
  CHECK(finite);
  CHECK_FALSE(c.transposeSwitching());
  CHECK(c.latencySamples() == c.engine().latencySamples());
}
