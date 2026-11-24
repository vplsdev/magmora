// The user response-file slot (XP10): WAV reading, resampling, zero-latency convolution, the slot in the chain.
#include <magmora/Chain.h>

#include <catch.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <random>
#include <vector>

#include "cab/Convolver.h"
#include "cab/UserIr.h"
#include "cab/Wav.h"

using namespace magmora;
using namespace magmora::cab;

namespace {

constexpr double kPi = 3.14159265358979323846;

struct Bytes {
  std::vector<uint8_t> b;
  void u32(uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>(v >> (8 * i)));
  }
  void u16(uint16_t v) {
    b.push_back(static_cast<uint8_t>(v));
    b.push_back(static_cast<uint8_t>(v >> 8));
  }
  void tag(const char* t) { b.insert(b.end(), t, t + 4); }
};

// A WAV in memory: format code, bits, channels, rate, and samples in [-1, 1) as doubles (interleaved).
std::vector<uint8_t> wav(int format, int bits, int channels, uint32_t rate, const std::vector<double>& x, bool extensible = false) {
  Bytes data;
  for (double v : x) {
    if (format == 3 && bits == 32) {
      float f = static_cast<float>(v);
      uint32_t w;
      std::memcpy(&w, &f, 4);
      data.u32(w);
    } else if (format == 3) {
      uint64_t w;
      std::memcpy(&w, &v, 8);
      data.u32(static_cast<uint32_t>(w));
      data.u32(static_cast<uint32_t>(w >> 32));
    } else if (bits == 8) {
      data.b.push_back(static_cast<uint8_t>(std::lround(v * 128.0) + 128));
    } else {
      const int64_t scale = int64_t{1} << (bits - 1);
      const int64_t s = std::max<int64_t>(-scale, std::min<int64_t>(scale - 1, std::llround(v * scale)));
      for (int i = 0; i < bits / 8; ++i) data.b.push_back(static_cast<uint8_t>(static_cast<uint64_t>(s) >> (8 * i)));
    }
  }
  Bytes f;
  f.tag("RIFF");
  f.u32(0);  // not checked
  f.tag("WAVE");
  f.tag("LIST");  // an unrelated chunk of odd size first, padded
  f.u32(3);
  f.b.insert(f.b.end(), {'a', 'b', 'c', 0});
  f.tag("fmt ");
  f.u32(extensible ? 40 : 16);
  f.u16(extensible ? 0xFFFE : static_cast<uint16_t>(format));
  f.u16(static_cast<uint16_t>(channels));
  f.u32(rate);
  f.u32(rate * static_cast<uint32_t>(channels * bits / 8));
  f.u16(static_cast<uint16_t>(channels * bits / 8));
  f.u16(static_cast<uint16_t>(bits));
  if (extensible) {
    f.u16(22);
    f.u16(static_cast<uint16_t>(bits));
    f.u32(3);
    f.u16(static_cast<uint16_t>(format));
    for (int i = 0; i < 14; ++i) f.b.push_back(0);
  }
  f.tag("data");
  f.u32(static_cast<uint32_t>(data.b.size()));
  f.b.insert(f.b.end(), data.b.begin(), data.b.end());
  return f.b;
}

std::vector<double> reference(const std::vector<float>& x, const std::vector<float>& h) {
  std::vector<double> y(x.size(), 0.0);
  for (size_t n = 0; n < x.size(); ++n)
    for (size_t k = 0; k < h.size() && k <= n; ++k) y[n] += static_cast<double>(h[k]) * x[n - k];
  return y;
}

std::vector<float> noise(size_t n, unsigned seed, double decay = 0.0) {
  std::mt19937 rng(seed);
  std::normal_distribution<double> g(0.0, 1.0);
  std::vector<float> v(n);
  for (size_t i = 0; i < n; ++i) v[i] = static_cast<float>(g(rng) * std::exp(-decay * static_cast<double>(i)));
  return v;
}

}  // namespace

TEST_CASE("userir: the WAV reader reads every supported format and refuses the rest") {
  const std::vector<double> x = {0.0, 0.5, -0.5, 0.25, -1.0, 0.75};  // three stereo frames
  struct Case {
    int format, bits;
    bool extensible;
    double tolerance;
  };
  for (const Case c : {Case{1, 8, false, 1.0 / 128}, Case{1, 16, false, 1.0 / 32768}, Case{1, 24, false, 1.0 / 8388608},
                       Case{1, 32, false, 1e-9}, Case{3, 32, false, 1e-7}, Case{3, 64, false, 1e-7}, Case{1, 24, true, 1.0 / 8388608},
                       Case{3, 32, true, 1e-7}}) {
    INFO("format " << c.format << " bits " << c.bits << " extensible " << c.extensible);
    const std::vector<uint8_t> file = wav(c.format, c.bits, 2, 44100, x, c.extensible);
    Audio a;
    std::string error;
    REQUIRE(readWav(file.data(), file.size(), a, &error));
    CHECK(a.rate == 44100.0);
    REQUIRE(a.channels.size() == 2);
    REQUIRE(a.frames() == 3);
    for (size_t n = 0; n < 3; ++n)
      for (size_t ch = 0; ch < 2; ++ch) CHECK(std::fabs(a.channels[ch][n] - x[2 * n + ch]) <= c.tolerance);
  }
  Audio a;
  std::string error;
  const std::vector<uint8_t> good = wav(1, 16, 1, 48000, {0.1, 0.2});
  CHECK(readWav(good.data(), good.size(), a, &error));
  CHECK(a.channels.size() == 1);
  for (size_t cut = 0; cut + 2 < good.size(); cut += 3) {  // truncations: refused, or (in the data) fewer samples
    Audio b;
    const bool ok = readWav(good.data(), cut, b, &error);
    if (ok) CHECK(b.frames() <= 2);
  }
  std::vector<uint8_t> bad = wav(1, 12, 1, 48000, {0.1});
  CHECK_FALSE(readWav(bad.data(), bad.size(), a, &error));
  bad = wav(2, 16, 1, 48000, {0.1});  // ADPCM
  CHECK_FALSE(readWav(bad.data(), bad.size(), a, &error));
  const char junk[] = "RIFF....WAVEjunk";
  CHECK_FALSE(readWav(reinterpret_cast<const uint8_t*>(junk), sizeof junk, a, &error));
}

TEST_CASE("userir: the convolver is the convolution, with zero latency and for any partition into blocks") {
  for (size_t length : {size_t{1}, size_t{100}, size_t{2048}, size_t{2049}, size_t{9000}, size_t{30000}}) {
    INFO("response length " << length);
    const std::vector<float> h = noise(length, 3, 3.0 / static_cast<double>(length));
    const std::vector<float> x = noise(40000, 4);
    const std::vector<double> y = reference(x, h);
    double peak = 0.0;
    for (double v : y) peak = std::max(peak, std::fabs(v));
    std::vector<float> first;
    for (int block : {1, 64, 333, 4096}) {
      Convolver c;
      c.build(h.data(), h.size(), 2048);
      std::vector<float> out(x.size());
      for (size_t at = 0; at < x.size(); at += static_cast<size_t>(block)) {
        const int n = static_cast<int>(std::min(x.size() - at, static_cast<size_t>(block)));
        c.process(x.data() + at, out.data() + at, n);
      }
      double worst = 0.0;
      for (size_t i = 0; i < x.size(); ++i) worst = std::max(worst, std::fabs(out[i] - y[i]));
      CHECK(worst <= 2e-6 * peak);
      if (first.empty())
        first = out;
      else
        CHECK(std::memcmp(first.data(), out.data(), out.size() * sizeof(float)) == 0);
    }
  }
  // an impulse comes out as the response from the first sample on
  const std::vector<float> h = noise(20000, 7, 1e-4);
  Convolver c;
  c.build(h.data(), h.size(), 2048);
  std::vector<float> x(25000, 0.0f), out(x.size());
  x[0] = 1.0f;
  c.process(x.data(), out.data(), static_cast<int>(x.size()));
  double worst = 0.0;
  for (size_t i = 0; i < h.size(); ++i) worst = std::max(worst, static_cast<double>(std::fabs(out[i] - h[i])));
  CHECK(worst < 1e-5);
  CHECK(std::fabs(out[0] - h[0]) < 1e-7);
}

TEST_CASE("userir: resampling keeps a band-limited signal") {
  for (auto [from, to] : {std::pair{44100.0, 48000.0}, std::pair{48000.0, 96000.0}, std::pair{96000.0, 44100.0}}) {
    const double hz = 5000.0;
    std::vector<float> x(static_cast<size_t>(from * 0.1));
    for (size_t n = 0; n < x.size(); ++n) x[n] = static_cast<float>(std::sin(2 * kPi * hz * n / from));
    const std::vector<float> y = resample(x, from, to);
    CHECK(std::abs(static_cast<long>(y.size()) - static_cast<long>(x.size() * to / from)) <= 1);
    double worst = 0.0;
    for (size_t m = y.size() / 4; m < 3 * y.size() / 4; ++m)  // away from the ends
      worst = std::max(worst, std::fabs(y[m] - std::sin(2 * kPi * hz * m / to)));
    INFO(from << " -> " << to);
    CHECK(worst < 1e-4);
  }
}

TEST_CASE("userir: in the chain, the slot convolves the head's output and adds no latency") {
  for (int channels : {1, 2}) {
    const std::vector<float> h = noise(30000, 11, 2e-4);  // long enough to be partitioned at 48 kHz
    Chain dry, wet;
    for (Chain* c : {&dry, &wet}) {
      c->setParameter(kChannel, 0.0);
      c->setParameter(kPreRhythm, 3.0);
      c->setParameter(kChannelMode, 2.0);  // dual mono with a stereo input
      REQUIRE(c->prepare(48000.0, 128, channels));
    }
    const float* irs[1] = {h.data()};
    REQUIRE(wet.loadUserIrSamples(irs, 1, static_cast<int>(h.size()), 48000.0));  // cabinet slot 2's file
    wet.setUserIrNormalize(false);
    wet.setParameter(kCabOn, 1.0);
    wet.setParameter(kMic1On, 0.0);
    wet.setParameter(kMic2On, 1.0);
    wet.setParameter(kMic2Source, 1.0);
    wet.setParameter(kMic2Level, 3.010299956639812);  // the constant-power centre back to unity: XP10's level
    REQUIRE(wet.waitForCabinet());
    CHECK(wet.latencySamples() == dry.latencySamples());
    const size_t frames = 48000;
    std::vector<float> inL(frames), inR(frames);
    for (size_t n = 0; n < frames; ++n) {
      inL[n] = static_cast<float>(0.3 * std::sin(2 * kPi * 110.0 * n / 48000.0) * std::exp(-std::fmod(n / 48000.0, 0.25) * 8));
      inR[n] = 0.5f * inL[(n * 3) % frames];
    }
    std::vector<float> d[2] = {std::vector<float>(frames), std::vector<float>(frames)};
    std::vector<float> w[2] = {std::vector<float>(frames), std::vector<float>(frames)};
    for (size_t at = 0; at < frames; at += 128) {
      const float* in[2] = {inL.data() + at, inR.data() + at};
      float* od[2] = {d[0].data() + at, d[1].data() + at};
      float* ow[2] = {w[0].data() + at, w[1].data() + at};
      dry.process(in, od, 128);
      wet.process(in, ow, 128);
    }
    // after the 20 ms fade-in the wet output is the dry output convolved with the response
    for (int c = 0; c < 2; ++c) {
      const std::vector<double> ref = reference(d[c], h);
      double worst = 0.0, peak = 0.0;
      for (size_t n = 2000; n < frames; ++n) {
        worst = std::max(worst, std::fabs(w[c][n] - ref[n]));
        peak = std::max(peak, std::fabs(ref[n]));
      }
      INFO("input channels " << channels << ", output " << c);
      CHECK(worst <= 1e-5 * peak);
    }
  }
}

TEST_CASE("userir: a new response crossfades, the old one is freed on the main thread, clear goes back to dry") {
  Chain c, head;
  REQUIRE(c.prepare(48000.0, 64, 1));
  REQUIRE(head.prepare(48000.0, 64, 1));
  const std::vector<float> a = noise(4000, 1, 1e-3), b = noise(60000, 2, 1e-4);
  const float* pa[1] = {a.data()};
  const float* pb[1] = {b.data()};
  REQUIRE(c.loadUserIrSamples(pa, 1, static_cast<int>(a.size()), 44100.0));
  CHECK(c.userIrLoaded());
  c.setParameter(kCabOn, 1.0);
  c.setParameter(kMic1On, 0.0);
  c.setParameter(kMic2On, 1.0);
  c.setParameter(kMic2Source, 1.0);
  std::vector<float> x(64, 0.0f), l(64), r(64);
  const float* in[1] = {x.data()};
  float* out[2] = {l.data(), r.data()};
  std::vector<float> hl(64), hr(64);
  float* headOut[2] = {hl.data(), hr.data()};
  bool finite = true, same = true;
  for (int block = 0; block < 400; ++block) {
    for (int n = 0; n < 64; ++n) x[static_cast<size_t>(n)] = static_cast<float>(0.2 * std::sin(0.05 * (block * 64 + n)));
    if (block == 100) REQUIRE(c.loadUserIrSamples(pb, 1, static_cast<int>(b.size()), 96000.0));
    if (block == 200) c.clearUserIr();
    if (block == 300) c.setParameter(kCabOn, 0.0);
    c.process(in, out, 64);
    head.process(in, headOut, 64);
    c.idle();
    for (float v : l) finite = finite && std::isfinite(v);
    if (block >= 320) same = same && std::memcmp(l.data(), hl.data(), sizeof(float) * 64) == 0;  // off and faded
  }
  CHECK(same);
  CHECK(finite);
  CHECK_FALSE(c.userIrLoaded());
  std::string error;
  CHECK_FALSE(c.loadUserIr("/nonexistent/response.wav", &error));
  CHECK_FALSE(error.empty());
}

TEST_CASE("userir: the reference, the normalisation switch and the slot's state survive a round trip") {
  Chain a;
  a.setUserIrReference("cab.wav");
  a.setUserIrNormalize(false);
  a.setParameter(kCabOn, 1.0);
  a.setParameter(kMic2On, 1.0);
  a.setParameter(kMic2Source, 1.0);
  std::vector<uint8_t> state;
  a.getState(state);
  Chain b;
  REQUIRE(b.setState(state.data(), state.size()));
  CHECK(std::string(b.userIrReference()) == "cab.wav");
  CHECK_FALSE(b.userIrNormalize());
  CHECK(b.blockOn(ChainBlock::UserIr));
}

TEST_CASE("userir: a 64-bit float sample beyond float range is read as silence, never as infinity") {
  std::vector<double> x(16, 0.0);
  x[0] = 1.0;
  x[3] = 1e300;
  x[4] = -1e300;
  x[5] = std::numeric_limits<double>::infinity();
  x[6] = std::numeric_limits<double>::quiet_NaN();
  x[7] = 3.0e38;  // still a float
  const std::vector<uint8_t> file = wav(3, 64, 1, 48000, x);
  Audio a;
  REQUIRE(readWav(file.data(), file.size(), a));
  REQUIRE(a.frames() == x.size());
  for (float v : a.channels[0]) CHECK(std::isfinite(v));
  CHECK(a.channels[0][0] == 1.0f);
  CHECK(a.channels[0][3] == 0.0f);
  CHECK(a.channels[0][4] == 0.0f);
  CHECK(a.channels[0][7] == 3.0e38f);
}

TEST_CASE("userir: a block longer than prepare's maximum is processed as the same signal in shorter blocks") {
  const std::vector<float> h = noise(2000, 5, 1e-3);
  const float* ch[1] = {h.data()};
  Chain once, chunked;
  for (Chain* c : {&once, &chunked}) {
    REQUIRE(c->prepare(48000.0, 64, 2));
    c->setParameter(kPedalOn, 1.0);
    c->setParameter(kEqOn, 1.0);
    c->setParameter(kCabOn, 1.0);
    c->setParameter(kMic1On, 1.0);
    c->setParameter(kMic1Source, 1.0);
    REQUIRE(c->loadCabinetIrSamples(0, ch, 1, static_cast<int>(h.size()), 48000.0));
    REQUIRE(c->waitForCabinet(5.0));
  }
  {  // the first blocks alike: blocks switch on at a block's start (the pedal from its operating point, the response)
    std::vector<float> w(64 * 20, 0.01f), o[2] = {std::vector<float>(w.size()), std::vector<float>(w.size())};
    for (Chain* c : {&once, &chunked})
      for (size_t at = 0; at < w.size(); at += 64) {
        const float* in[2] = {w.data() + at, w.data() + at};
        float* out[2] = {o[0].data() + at, o[1].data() + at};
        c->process(in, out, 64);
      }
  }
  std::vector<float> x = noise(4800, 9);
  for (float& v : x) v *= 0.1f;
  std::vector<float> a[2] = {std::vector<float>(x.size()), std::vector<float>(x.size())};
  std::vector<float> b[2] = {std::vector<float>(x.size()), std::vector<float>(x.size())};
  size_t at = 0;
  for (const int frames : {65, 200, 1000, 1, 64, 3470}) {  // most longer than 64
    const float* in[2] = {x.data() + at, x.data() + at};
    float* outA[2] = {a[0].data() + at, a[1].data() + at};
    once.process(in, outA, frames);
    for (int k = 0; k < frames; k += 64) {
      const int n = std::min(64, frames - k);
      const float* inK[2] = {x.data() + at + k, x.data() + at + k};
      float* outB[2] = {b[0].data() + at + k, b[1].data() + at + k};
      chunked.process(inK, outB, n);
    }
    at += static_cast<size_t>(frames);
  }
  REQUIRE(at == x.size());
  size_t first = 0;
  while (first < x.size() && a[0][first] == b[0][first] && a[1][first] == b[1][first]) ++first;
  INFO("first difference at sample " << first);
  CHECK(first == x.size());
}

TEST_CASE("userir: with one-sample blocks a new response still catches up and takes over") {
  const std::vector<float> h = noise(30000, 13, 2e-4);  // partitioned
  const float* ch[1] = {h.data()};
  Chain c, dry;
  for (Chain* k : {&c, &dry}) {
    REQUIRE(k->prepare(48000.0, 1, 2));
    k->setParameter(kAmpOn, 0.0);  // the input reaches the cabinet as it is (at the head's latency)
  }
  c.setParameter(kCabOn, 1.0);
  c.setParameter(kMic1On, 1.0);
  c.setParameter(kMic1Source, 1.0);
  c.setParameter(kMic1Level, 3.010299956639812);  // the constant-power centre back to unity
  c.setCabinetIrNormalize(0, false);
  const size_t frames = 48000;
  std::vector<float> x = noise(frames, 21);
  for (float& v : x) v *= 0.001f;  // the convolved output well inside the output clamp
  std::vector<float> y(frames), d(frames), right(frames);
  for (size_t n = 0; n < frames; ++n) {
    if (n == 12000) {
      REQUIRE(c.loadCabinetIrSamples(0, ch, 1, static_cast<int>(h.size()), 48000.0));
      REQUIRE(c.waitForCabinet(5.0));
    }
    const float* in[2] = {x.data() + n, x.data() + n};
    float* out[2] = {y.data() + n, right.data() + n};
    c.process(in, out, 1);
    out[0] = d.data() + n;
    dry.process(in, out, 1);
  }
  // the last 2000 samples: the response, with its whole tail (it was fed the input from before it was loaded)
  double worst = 0.0, peak = 0.0;
  for (size_t n = frames - 2000; n < frames; ++n) {
    double ref = 0.0;
    for (size_t k = 0; k < h.size() && k <= n; ++k) ref += static_cast<double>(h[k]) * d[n - k];
    worst = std::max(worst, std::fabs(y[n] - ref));
    peak = std::max(peak, std::fabs(ref));
  }
  INFO("worst " << worst << ", peak " << peak);
  CHECK(peak > 0.0);
  CHECK(worst <= 1e-5 * peak);
}

TEST_CASE("userir: after a reset a response handed over plays at once, however far its build had got") {
  // Both chains have the same response. One is prepared again after its worker built it (as a structural change
  // does): that prepare builds the response at once, as the current one. The other's waits in the hand-over. After
  // waitForCabinet and a reset the two must be the same signal from the first sample.
  const std::vector<float> h = noise(3000, 17, 1e-3);
  const float* ch[1] = {h.data()};
  Chain again, handed;
  for (Chain* c : {&again, &handed}) {
    REQUIRE(c->prepare(48000.0, 64, 2));
    c->setParameter(kCabOn, 1.0);
    c->setParameter(kMic1On, 1.0);
    c->setParameter(kMic1Source, 1.0);
    REQUIRE(c->loadCabinetIrSamples(0, ch, 1, static_cast<int>(h.size()), 48000.0));
    REQUIRE(c->waitForCabinet(10.0));
  }
  again.applyStructuralParams();
  for (Chain* c : {&again, &handed}) {
    REQUIRE(c->waitForCabinet(10.0));
    c->reset();
  }
  std::vector<float> x = noise(4800, 3);
  for (float& v : x) v *= 0.05f;
  std::vector<float> a(x.size()), b(x.size()), r(x.size());
  for (size_t at = 0; at < x.size(); at += 64) {
    const float* in[2] = {x.data() + at, x.data() + at};
    float* oa[2] = {a.data() + at, r.data() + at};
    again.process(in, oa, 64);
    float* ob[2] = {b.data() + at, r.data() + at};
    handed.process(in, ob, 64);
  }
  size_t first = 0;
  while (first < x.size() && a[first] == b[first]) ++first;
  INFO("first difference at sample " << first);
  CHECK(first == x.size());
}
