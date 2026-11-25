// The cabinet's virtual microphone (XP6): position names, morph and snap, two microphones, the pack in the
// chain. Synthetic responses only (no pack is part of the repository).
#include <magmora/Chain.h>

#include <catch.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <random>
#include <thread>
#include <vector>

#include "cab/Morph.h"
#include "cab/Positions.h"
#include "cab/Spectral.h"

using namespace magmora;
using namespace magmora::cab;
namespace fs = std::filesystem;

namespace {

// MSVC treats a double-to-float container construction as a narrowing warning (an error under /WX)
std::vector<float> toFloat(const std::vector<double>& v) {
  std::vector<float> f(v.size());
  for (size_t i = 0; i < v.size(); ++i) f[i] = static_cast<float>(v[i]);
  return f;
}

constexpr double kFs = 48000.0;

// A minimum-phase response: a gentle low-pass with one notch at fc (eval/cab/test_virtualmic.py's notch_ir).
std::vector<double> notchIr(double fc, double depthDb = 18.0, double q = 8.0, size_t n = 2048) {
  const size_t nfft = 4 * n;
  std::vector<double> mag(nfft / 2 + 1);
  for (size_t k = 0; k < mag.size(); ++k) {
    const double f = static_cast<double>(k) * kFs / static_cast<double>(nfft);
    const double fm = std::max(f, 1.0);
    const double shape = q * (f / fc - fc / fm);
    mag[k] = 1.0 / std::sqrt(1.0 + std::pow(f / 6000.0, 4)) * std::pow(10.0, -depthDb / 20.0 / (1.0 + shape * shape));
  }
  return morph::minimumPhase(mag, n);
}

// The deepest point of h relative to the low-pass, between lo and hi (Hz): (frequency, dB).
std::pair<double, double> notchAt(const std::vector<double>& h, double lo, double hi) {
  const auto H = spectral::rfft(h, 8192);
  std::pair<double, double> best{0.0, 1e9};
  for (size_t k = 0; k < H.size(); ++k) {
    const double f = static_cast<double>(k) * kFs / 8192.0;
    if (f <= lo || f >= hi) continue;
    const double d = 20 * std::log10(std::abs(H[k]) + 1e-12) - 20 * std::log10(1.0 / std::sqrt(1.0 + std::pow(f / 6000.0, 4)));
    if (d < best.second) best = {f, d};
  }
  return best;
}

void writeWav(const fs::path& path, const std::vector<double>& x, uint32_t rate = 48000) {
  std::ofstream o(path, std::ios::binary);
  auto u32 = [&](uint32_t v) { o.write(reinterpret_cast<const char*>(&v), 4); };
  auto u16 = [&](uint16_t v) { o.write(reinterpret_cast<const char*>(&v), 2); };
  o.write("RIFF", 4);
  u32(static_cast<uint32_t>(36 + 4 * x.size()));
  o.write("WAVEfmt ", 8);
  u32(16);
  u16(3);
  u16(1);
  u32(rate);
  u32(rate * 4);
  u16(4);
  u16(32);
  o.write("data", 4);
  u32(static_cast<uint32_t>(4 * x.size()));
  for (double v : x) {
    const float f = static_cast<float>(v);
    o.write(reinterpret_cast<const char*>(&f), 4);
  }
}

struct TempDir {
  fs::path path;
  TempDir() {
    std::random_device rd;
    path = fs::temp_directory_path() / ("magmora-cab-test-" + std::to_string(rd()));
    fs::create_directories(path);
  }
  ~TempDir() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
};

double maxDiff(const std::vector<double>& a, const std::vector<double>& b) {
  double d = 0.0;
  for (size_t k = 0; k < std::max(a.size(), b.size()); ++k)
    d = std::max(d, std::fabs((k < a.size() ? a[k] : 0.0) - (k < b.size() ? b[k] : 0.0)));
  return d;
}

}  // namespace

TEST_CASE("cab: positions are read from names, mapping schemes and overrides as the prototype reads them") {
  Position p = parseName("SPK LR CAB 4x12 DY57 0.75in 0.5in OA30 PRE1.wav");
  CHECK(p.speaker == "LR");
  CHECK(p.mic == "dy57");
  CHECK(p.angleDeg == 30.0);
  CHECK(p.xMm == Approx(19.05));
  CHECK(p.distMm == Approx(12.7));
  CHECK(p.source == "tokens");
  p = parseName("Maker OS R30 57 Cap Edge 1in.wav");
  CHECK((p.hasZone && p.zone == 1.0 && p.hasDist && p.distMm == Approx(25.4) && !p.hasX));
  p = parseName("412_R30_dy421_cone_45deg_25mm.wav");
  CHECK((p.mic == "dy421" && p.angleDeg == 45.0 && p.distMm == 25.0));
  CHECK(parseName("Brand 4x12 RB121 pos 3.wav").step == 3);
  CHECK(parseName("PACK-57-C.wav").step == 2);
  CHECK(parseName("XYZ 412 Q25 b5-CE 1.wav").mic == "b5");  // generic tokens, as virtualmic.mic_token
  CHECK(parseName("ab57 then cd421 cap.wav").mic == "cd421");
  CHECK(parseName("XYZ 412 AB20 N22-5.wav").mic.empty());
  CHECK(parseName("OA20 cone 1in.wav").mic.empty());
  CHECK(parseName("pos5 cap.wav").mic.empty());
  CHECK(parseName("cone57 abcd57.wav").mic.empty());
  CHECK(parseName("DY7B 2,5cm from cap 30mm.wav").xMm == Approx(25.0));
  const char* mappingText = R"({"phase": "raw", "schemes": [
      {"regex": "^SPK (?P<speaker>LL|LR|UL|UR) .* (?P<mic>[A-Za-z]{0,3}57) (?P<x>[0-9.]+)in (?P<dist>[0-9.]+)in(?: OA(?P<angle>[0-9]+))? (?P<variant>\\w+)$",
       "units": {"x": "in", "dist": "in"}},
      {"regex": "-(?P<mic>[0-9A-Za-z]+)-(?P<step>[A-E])$", "order": ["A", "B", "C", "D", "E"]}],
      "files": {"my favourite.wav": {"mic": "57", "x_mm": 19.0, "dist_mm": 0.0}}})";
  Mapping m;
  std::string error;
  REQUIRE(parseMapping(mappingText, m, &error));
  CHECK(m.phase == "raw");
  p = parseName("SPK UL CAB 4x12 DY57 1.25in 0.0in PRE2.wav", &m);
  CHECK((p.source == "scheme" && p.speaker == "UL" && p.variant == "PRE2" && p.xMm == Approx(31.75)));
  CHECK(parseName("PACK-OS-421-D.wav", &m).step == 3);
  p = parseName("my favourite.wav", &m);
  CHECK((p.source == "mapping" && p.mic == "57" && p.xMm == 19.0));
  CHECK_FALSE(parseMapping(R"({"phase": "linear"})", m, &error));
  CHECK_FALSE(parseMapping(R"({"schemes": [{"regex": "(?<!a)b"}]})", m, &error));  // no look-behind
}

TEST_CASE("cab: morph moves a notch, keeps a fixed one deep, and leaves the end points untouched") {
  const auto a = notchIr(3000), b = notchIr(3600);
  for (auto method : {morph::Method::Snap, morph::Method::Morph}) {
    CHECK(maxDiff(morph::interpolate(a, b, 0.0, kFs, method, true), a) == 0.0);
    CHECK(maxDiff(morph::interpolate(a, b, 1.0, kFs, method, true), b) == 0.0);
  }
  const auto [f, depth] = notchAt(morph::interpolate(a, b, 0.5, kFs, morph::Method::Morph, true), 2500, 4200);
  CHECK(std::fabs(f - 3290) < 80);  // one notch, moved to the middle
  CHECK(depth < -13.0);              // and still deep (a dB crossfade leaves two shallow ones)
  std::vector<double> half(notchIr(3000));
  for (double& v : half) v *= 0.5;
  CHECK(notchAt(morph::interpolate(notchIr(3000), half, 0.5, kFs, morph::Method::Morph, true), 2500, 3600).second < -16.0);
  // identical neighbours: the morph is the file
  const auto same = morph::interpolate(a, a, 0.5, kFs, morph::Method::Morph, true);
  CHECK(maxDiff(same, a) < 1e-3 * *std::max_element(a.begin(), a.end()));
}

TEST_CASE("cab: a raw pack keeps the time of flight; two raw microphones are aligned by measurement") {
  const auto n = notchIr(3000);
  std::vector<double> a(20, 0.0), b(60, 0.0);
  a.insert(a.end(), n.begin(), n.end());
  b.insert(b.end(), n.begin(), n.end());
  const auto h = morph::interpolate(a, b, 0.5, kFs, morph::Method::Morph, false);
  CHECK(morph::onset(h, kFs) == Approx(morph::onset(n, kFs) + 40.0).margin(0.5));
  // the second microphone 41.37 samples late and inverted: measured to an eighth of a sample, polarity shown
  std::vector<double> late = morph::shift(std::vector<double>(n.begin(), n.end()), 41.37);
  for (double& v : late) v = -v;
  double delay = 0.0;
  int polarity = 0;
  morph::relativeDelay(late, n, kFs, delay, polarity);
  CHECK(delay == Approx(41.375));
  CHECK(polarity == -1);
  morph::MicMix m1, m2;
  m2.invert = true;
  const auto mix = morph::twoMics(n, late, kFs, m1, m2, true, 0.0, morph::PackPhase::Raw);
  CHECK(mix.detectedPolarity == -1);
  CHECK(mix.moved == Approx(-41.375));
  double l, r;
  morph::panGains(0.0, l, r);
  std::vector<double> twice(n.size());
  for (size_t k = 0; k < n.size(); ++k) twice[k] = 2.0 * l * n[k];
  CHECK(maxDiff(mix.left, twice) < 0.02);  // the eighth-sample residual of the measurement
  // declared minimum phase or aligned: nothing is measured or moved
  const auto kept = morph::twoMics(n, late, kFs, m1, m2, true, 0.0, morph::PackPhase::Aligned);
  CHECK(kept.moved == 0.0);
  // an offset in millimetres is a path length
  const auto off = morph::twoMics(n, n, kFs, m1, m1, true, 10.0, morph::PackPhase::Minimum);
  CHECK(off.moved == Approx(10e-3 / 343.0 * kFs));
}

namespace {

// A synthetic pack: one microphone, one speaker, notches that move with the position across the cone.
void writePack(const fs::path& dir, const std::vector<double>& inches) {
  for (double x : inches) {
    char name[96];
    std::snprintf(name, sizeof name, "SPK LL CAB 4x12 DY57 %.2fin 0.0in PRE1.wav", x);
    writeWav(dir / name, notchIr(2500.0 + 800.0 * x, 15.0, 6.0, 4800));
  }
}

void play(Chain& c, std::vector<float>& outL, size_t blocks) {
  std::vector<float> in(64), l(64), r(64);
  for (size_t b = 0; b < blocks; ++b) {
    for (size_t n = 0; n < 64; ++n) in[n] = static_cast<float>(0.2 * std::sin(0.05 * (b * 64 + n)));
    const float* i[1] = {in.data()};
    float* o[2] = {l.data(), r.data()};
    c.process(i, o, 64);
    outL.insert(outL.end(), l.begin(), l.end());
  }
}

}  // namespace

TEST_CASE("cab: in the chain, a pack is indexed, the response is built off the audio thread and convolved") {
  TempDir dir;
  writePack(dir.path, {0.0, 0.25, 0.5, 1.5});
  Chain c, dry;
  for (Chain* k : {&c, &dry}) {
    k->setParameter(kChannel, 0.0);
    k->setParameter(kPreRhythm, 2.0);
    REQUIRE(k->prepare(48000.0, 64, 1));
  }
  std::string error;
  REQUIRE(c.loadCabinetPack(dir.path.string().c_str(), &error));
  CHECK(c.cabinetMicCount() == 1);
  CHECK(c.cabinetMicName(0) == "dy57");
  CHECK(c.cabinetSpeakers(0) == std::vector<std::string>{"LL"});
  CHECK(std::string(c.cabinetPackReference()) == dir.path.string());
  CHECK(Chain::parameterHidden(kCabFloor));

  c.setParameter(kCabOn, 1.0);
  c.setParameter(kMic1On, 1.0);
  c.setParameter(kMic1Type, 0.0);
  c.setParameter(kMic2On, 0.0);
  c.setParameter(kMic1Distance, 0.0);
  // 0.375 in: between files a quarter inch apart, so the morph
  c.setParameter(kMic1Position, 0.375 / 1.5);
  std::vector<float> wet, ref;
  CabinetStatus st;
  for (int k = 0; k < 400 && !(st.builds > 0 && st.mic[0].on && std::fabs(st.mic[0].xMm - 0.375 * 25.4) < 1e-9); ++k) {
    play(c, wet, 1);
    play(dry, ref, 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    st = c.cabinetStatus();
  }
  REQUIRE(st.builds > 0);
  CHECK(st.packLoaded);
  CHECK(st.mic[0].kind == 0);
  CHECK(st.mic[0].morphX);
  CHECK(st.mic[0].xMm == Approx(0.375 * 25.4));
  CHECK(st.mic[0].files.size() == 2);
  // once the new response has heard its whole length of input (its convolver starts empty at the hand-over; the 20 ms
  // crossfade covers the start), the output is the head's convolved with the morph (pack gain, centre pan)
  play(c, wet, 120);
  play(dry, ref, 120);
  const auto a = notchIr(2500.0 + 800.0 * 0.25, 15.0, 6.0, 4800), b = notchIr(2500.0 + 800.0 * 0.5, 15.0, 6.0, 4800);
  std::vector<double> h = morph::interpolate(std::vector<double>(a.begin(), a.end()), b, 0.5, kFs, morph::Method::Morph, false);
  double energy = 0.0;
  for (double x : {0.0, 0.25, 0.5, 1.5}) {
    double e = 0.0;
    for (double v : notchIr(2500.0 + 800.0 * x, 15.0, 6.0, 4800)) e += static_cast<double>(static_cast<float>(v)) * static_cast<float>(v);
    energy += e / 4.0;
  }
  double l, r;
  morph::panGains(0.0, l, r);
  const double g = l / std::sqrt(energy);
  const size_t from = wet.size() - 30 * 64, len = wet.size();
  double worst = 0.0, peak = 0.0;
  for (size_t n = from; n < len; ++n) {
    double acc = 0.0;
    for (size_t k = 0; k < h.size() && k <= n; ++k) acc += h[k] * ref[n - k];
    worst = std::max(worst, std::fabs(wet[n] - g * acc));
    peak = std::max(peak, std::fabs(g * acc));
  }
  CHECK(worst <= 1e-4 * peak);  // float32 kernel and convolution

  // 1.1 in: the neighbours are an inch apart, so it snaps to the nearer recording
  c.setParameter(kMic1Position, 1.1 / 1.5);
  for (int k = 0; k < 400 && !(st.mic[0].files.size() == 1 && !st.mic[0].morphX); ++k) {
    play(c, wet, 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    st = c.cabinetStatus();
  }
  CHECK_FALSE(st.mic[0].morphX);
  CHECK(st.mic[0].files.size() == 1);
  CHECK(st.mic[0].xMm == Approx(1.5 * 25.4));
}

TEST_CASE("cab: a missing pack is reported, the cabinet passes the signal, and the pack's state survives") {
  Chain c, head;
  for (Chain* k : {&c, &head}) REQUIRE(k->prepare(48000.0, 64, 1));
  std::string error;
  CHECK_FALSE(c.loadCabinetPack("/nonexistent/magmora/pack", &error));
  CHECK_FALSE(error.empty());
  CHECK_FALSE(c.cabinetStatus().error.empty());
  CHECK_FALSE(c.cabinetPackLoaded());
  c.setParameter(kCabOn, 1.0);
  std::vector<float> a, b;
  play(c, a, 50);
  play(head, b, 50);
  CHECK(a == b);  // no pack: the cabinet's crossfade is to the dry signal

  TempDir dir;
  writePack(dir.path, {0.0, 0.5});
  REQUIRE(c.loadCabinetPack(dir.path.string().c_str(), &error));
  c.setCabinetMicSource(0, "LL", "PRE1");
  std::vector<uint8_t> state;
  c.getState(state);
  Chain d;
  REQUIRE(d.prepare(48000.0, 64, 1));
  REQUIRE(d.setState(state.data(), state.size()));
  CHECK(std::string(d.cabinetPackReference()) == dir.path.string());
  CHECK(d.cabinetMicSpeaker(0) == "LL");
  CHECK(d.cabinetMicVariant(0) == "PRE1");
  REQUIRE(d.loadCabinetPack(d.cabinetPackReference(), &error));
  CHECK(d.cabinetMicName(static_cast<int>(d.getParameter(kMic1Type))) == "dy57");
  std::vector<uint8_t> again;
  d.getState(again);
  CHECK(again == state);
}

// -------------------------------------------------------------------------------------------------- slot sources

#include <atomic>

#include "cab/VirtualMic.h"
#include "chain/Bytes.h"

namespace {

std::vector<double> decaying(size_t n, uint32_t seed, double decay) {
  std::vector<double> h(n);
  for (size_t k = 0; k < n; ++k) {
    seed = seed * 1664525u + 1013904223u;
    h[k] = (static_cast<double>(seed >> 8) / 16777216.0 - 0.5) * std::exp(-decay * static_cast<double>(k));
  }
  return h;
}

struct Section {
  char tag[4];
  int32_t version;
  std::string hash;
  std::vector<uint8_t> payload;
};
std::vector<Section> sections(const std::vector<uint8_t>& state) {
  chain::Reader in(state.data(), state.size());
  uint32_t magic = 0;
  int32_t version = 0, count = 0;
  in.get(magic);
  in.get(version);
  in.get(count);
  std::vector<Section> out;
  for (int k = 0; k < count; ++k) {
    Section s;
    int32_t size = 0;
    in.bytes(s.tag, 4);
    in.get(s.version);
    in.string(s.hash);
    in.get(size);
    s.payload.assign(in.here(), in.here() + size);
    in.skip(static_cast<size_t>(size));
    out.push_back(std::move(s));
  }
  return out;
}
std::vector<uint8_t> join(const std::vector<Section>& list) {
  std::vector<uint8_t> out;
  chain::Writer w(out);
  w.put(uint32_t{0x48434353});
  w.put(int32_t{1});
  w.put(static_cast<int32_t>(list.size()));
  for (const Section& s : list) {
    w.bytes(s.tag, 4);
    w.put(s.version);
    w.string(s.hash.c_str());
    w.put(static_cast<int32_t>(s.payload.size()));
    w.bytes(s.payload.data(), s.payload.size());
  }
  return out;
}

}  // namespace

TEST_CASE("cab: slots mix pack microphones and files in every combination") {
  TempDir dir;
  writePack(dir.path, {0.0, 0.25});
  cab::Pack pack;
  std::string error;
  REQUIRE(pack.load(dir.path.string(), "", &error));
  const std::vector<double> f1 = decaying(3000, 3, 2e-3), f2 = decaying(5000, 9, 1e-3), f2r = decaying(5000, 11, 1e-3);
  const std::vector<std::vector<double>> file1{f1}, file2{f2, f2r};
  cab::VirtualMic::SlotFile files[2] = {{&file1, false, false, "a.wav"}, {&file2, false, false, "b.wav"}};
  const std::string none[2];
  cab::VirtualMic::Settings s;
  s.on[0] = s.on[1] = true;
  s.mix[0] = {-3.0, -0.4, false};
  s.mix[1] = {2.0, 0.7, true};
  s.offsetMm = 0.0;
  auto gains = [](const morph::MicMix& m, bool file, double& l, double& r) {
    morph::panGains(m.pan, l, r);
    (void)file;  // one pan law for both kinds of slot
    const double g = std::pow(10.0, m.levelDb / 20.0) * (m.invert ? -1.0 : 1.0);
    l *= g;
    r *= g;
  };
  CabinetStatus st;
  {  // two files, the second stereo: constant-power gains, each side by its own
    s.source[0] = s.source[1] = 1;
    const auto out = cab::VirtualMic::build(&pack, s, none, none, files, kFs, st);
    double l1, r1, l2, r2;
    gains(s.mix[0], true, l1, r1);
    gains(s.mix[1], true, l2, r2);
    std::vector<double> L(5000, 0.0), R(5000, 0.0);
    for (size_t k = 0; k < 5000; ++k) {
      L[k] = (k < f1.size() ? l1 * f1[k] : 0.0) + l2 * f2[k];
      R[k] = (k < f1.size() ? r1 * f1[k] : 0.0) + r2 * f2r[k];
    }
    CHECK(maxDiff(out.left, L) < 1e-12);
    CHECK(maxDiff(out.right, R) < 1e-12);
    CHECK((st.mic[0].source == 1 && st.mic[0].found && st.mic[1].mic == "b.wav"));
  }
  {  // a pack microphone (constant power, the pack's gain) and a file; position does nothing for the file
    s.source[0] = 0;
    s.position[0] = 0.0;
    s.position[1] = 0.7;
    s.distanceMm[1] = 40.0;
    const auto out = cab::VirtualMic::build(&pack, s, none, none, files, kFs, st);
    CabinetMicStatus info;
    std::vector<double> h = pack.response(0, "", "", 0.0, 0.0, 0.0, kFs, info);
    double l1, r1, l2, r2;
    gains(s.mix[0], false, l1, r1);
    gains(s.mix[1], true, l2, r2);
    const size_t n = std::max(h.size(), f2.size());
    std::vector<double> L(n, 0.0);
    for (size_t k = 0; k < n; ++k) L[k] = (k < h.size() ? pack.gain() * l1 * h[k] : 0.0) + (k < f2.size() ? l2 * f2[k] : 0.0);
    CHECK(maxDiff(out.left, L) < 1e-12);
    CHECK(st.mic[0].source == 0);
  }
  {  // normalised: unit energy of the louder channel
    files[0].normalize = true;
    s.on[1] = false;
    s.source[0] = 1;
    s.mix[0] = {0.0, 0.0, false};
    const auto out = cab::VirtualMic::build(&pack, s, none, none, files, kFs, st);
    double e = 0.0;
    for (double v : f1) e += v * v;
    CHECK(out.left[10] == Approx(std::sqrt(0.5) * f1[10] / std::sqrt(e)));
    CHECK(out.right[10] == Approx(std::sqrt(0.5) * f1[10] / std::sqrt(e)));
  }
  {  // a file slot without a file, and everything off: silence
    const cab::VirtualMic::SlotFile empty[2];
    const auto out = cab::VirtualMic::build(nullptr, s, none, none, empty, kFs, st);
    CHECK((out.left.size() == 1 && out.left[0] == 0.0));
    CHECK((st.mic[0].on && !st.mic[0].found));
    s.on[0] = false;
    const auto off = cab::VirtualMic::build(&pack, s, none, none, files, kFs, st);
    CHECK((off.left.size() == 1 && off.left[0] == 0.0));
  }
}

TEST_CASE("cab: a new response starts with its whole tail (primed with the slot's recent input)") {
  Chain c, dry;
  for (Chain* k : {&c, &dry}) {
    k->setParameter(kChannel, 0.0);
    k->setParameter(kPreRhythm, 3.0);
    REQUIRE(k->prepare(48000.0, 64, 1));
  }
  const std::vector<double> a = decaying(24000, 5, 2e-4), b = decaying(24000, 7, 1e-4);  // long tails
  std::vector<float> af(a.size()), bf(b.size());
  for (size_t i = 0; i < a.size(); ++i) af[i] = static_cast<float>(a[i]);
  for (size_t i = 0; i < b.size(); ++i) bf[i] = static_cast<float>(b[i]);
  const float* pa[1] = {af.data()};
  const float* pb[1] = {bf.data()};
  c.setParameter(kCabOn, 1.0);
  c.setParameter(kMic1On, 0.0);
  c.setParameter(kMic2On, 1.0);
  c.setParameter(kMic2Source, 1.0);
  c.setCabinetIrNormalize(1, false);
  REQUIRE(c.loadCabinetIrSamples(1, pa, 1, 24000, 48000.0));
  REQUIRE(c.waitForCabinet());
  std::vector<float> wet, ref;
  play(c, wet, 750);  // one second
  play(dry, ref, 750);
  REQUIRE(c.loadCabinetIrSamples(1, pb, 1, 24000, 48000.0));
  REQUIRE(c.waitForCabinet());
  const size_t swap = wet.size();
  play(c, wet, 300);
  play(dry, ref, 300);
  // from 30 ms after the hand-over (the 20 ms crossfade done), the output is the head's convolved with the whole of b
  double l, r;
  morph::panGains(0.0, l, r);
  double worst = 0.0, peak = 0.0;
  for (size_t n = swap + 1440; n < swap + 9600; ++n) {
    double acc = 0.0;
    for (size_t k = 0; k < b.size() && k <= n; ++k) acc += b[k] * ref[n - k];
    acc *= l;  // constant-power centre
    worst = std::max(worst, std::fabs(wet[n] - acc));
    peak = std::max(peak, std::fabs(acc));
  }
  INFO("worst " << worst << " peak " << peak);
  CHECK(worst <= 1e-4 * peak);
}

TEST_CASE("cab: a state with the old series response file loads as slot 2 = that file") {
  Chain a;
  a.setParameter(kUserIrOn, 1.0);
  a.setParameter(kUserIrLevel, -4.5);
  a.setParameter(kMic1On, 1.0);
  std::vector<uint8_t> state;
  a.getState(state);
  // what a state from before the slots took files looked like: no "cabp" section, the file in "uir "
  std::vector<Section> list = sections(state);
  std::vector<Section> old;
  for (Section& s : list) {
    if (std::memcmp(s.tag, "cabp", 4) == 0) continue;
    if (std::memcmp(s.tag, "uir ", 4) == 0) {
      chain::Reader in(s.payload.data(), s.payload.size());
      int32_t count = 0;
      in.get(count);
      std::vector<uint8_t> p;
      chain::Writer w(p);
      w.put(count);
      for (int k = 0; k < count; ++k) {
        std::string key;
        double v = 0.0;
        in.string(key);
        in.get(v);
        w.string(key.c_str());
        w.put(v);
      }
      w.string("/old/session/cab.wav");
      w.put(int32_t{0});  // normalisation off
      s.payload = p;
    }
    old.push_back(s);
  }
  const std::vector<uint8_t> before = join(old);
  Chain b;
  REQUIRE(b.setState(before.data(), before.size()));
  CHECK(b.getParameter(kCabOn) == 1.0);
  CHECK(b.getParameter(kMic1On) == 0.0);  // the cabinet was off: the file was all that was heard
  CHECK(b.getParameter(kMic2On) == 1.0);
  CHECK(b.getParameter(kMic2Source) == 1.0);
  CHECK(b.getParameter(kMic2Level) == Approx(-4.5 + 3.010299956639812));  // XP10's unity, at the -3 dB centre
  CHECK(b.getParameter(kMic2Pan) == 0.0);
  CHECK(b.getParameter(kUserIrOn) == 0.0);
  CHECK(std::string(b.cabinetIrReference(1)) == "/old/session/cab.wav");
  CHECK_FALSE(b.cabinetIrNormalize(1));
  CHECK(b.blockOn(ChainBlock::UserIr));
  // saved again, it is a new state: loading it changes nothing further
  std::vector<uint8_t> again, third;
  b.getState(again);
  Chain d;
  REQUIRE(d.setState(again.data(), again.size()));
  d.getState(third);
  CHECK(third == again);
  // with the cabinet on before (a microphone in series), slot 1 keeps its microphone and the file is summed
  Chain e;
  e.setParameter(kCabOn, 1.0);
  e.setParameter(kUserIrOn, 1.0);
  std::vector<uint8_t> s2;
  e.getState(s2);
  std::vector<Section> l2 = sections(s2), o2;
  for (Section& s : l2) {
    if (std::memcmp(s.tag, "cabp", 4) == 0) continue;
    if (std::memcmp(s.tag, "uir ", 4) == 0) {
      s.payload.resize(s.payload.size() - 8);  // the empty reference and the flag
      chain::Writer w(s.payload);
      w.string("x.wav");
      w.put(int32_t{1});
    }
    o2.push_back(s);
  }
  const std::vector<uint8_t> before2 = join(o2);
  Chain f;
  REQUIRE(f.setState(before2.data(), before2.size()));
  CHECK(f.getParameter(kMic1On) == 1.0);
  CHECK(f.getParameter(kMic2Source) == 1.0);
}

TEST_CASE("cab: a migrated series-IR session plays at its old loudness, and the level clamp") {
  // XP10's series slot played a mono file at unity after the head: the old output was head * h at the old level
  const std::vector<double> h = decaying(12000, 21, 4e-4);
  std::vector<float> hf(h.size());
  for (size_t i = 0; i < h.size(); ++i) hf[i] = static_cast<float>(h[i]);
  const float* ph[1] = {hf.data()};
  for (double oldLevel : {-4.5, 0.0, 6.0, 10.0}) {
    Chain old;
    old.setParameter(kUserIrOn, 1.0);
    old.setParameter(kUserIrLevel, oldLevel);
    std::vector<uint8_t> state;
    old.getState(state);
    std::vector<Section> list = sections(state), pre;
    for (Section& s : list) {
      if (std::memcmp(s.tag, "cabp", 4) == 0) continue;
      if (std::memcmp(s.tag, "uir ", 4) == 0) {
        s.payload.resize(s.payload.size() - 8);
        chain::Writer w(s.payload);
        w.string("old.wav");
        w.put(int32_t{0});
      }
      pre.push_back(s);
    }
    const std::vector<uint8_t> before = join(pre);
    Chain c, dry;
    REQUIRE(c.setState(before.data(), before.size()));
    for (Chain* k : {&c, &dry}) {  // the same head for both (after the state, which carries the head's parameters)
      k->setParameter(kChannel, 0.0);
      k->setParameter(kPreRhythm, 3.0);
    }
    for (Chain* k : {&c, &dry}) REQUIRE(k->prepare(48000.0, 64, 1));
    REQUIRE(c.loadCabinetIrSamples(1, ph, 1, 12000, 48000.0));  // the host reloads the referenced file
    REQUIRE(c.waitForCabinet());
    std::vector<float> wet, ref;
    play(c, wet, 300);
    play(dry, ref, 300);
    const double g = std::pow(10.0, oldLevel / 20.0);
    double ew = 0.0, eo = 0.0;
    for (size_t n = 4000; n < wet.size(); ++n) {
      double acc = 0.0;
      for (size_t k = 0; k < h.size() && k <= n; ++k) acc += h[k] * ref[n - k];
      ew += static_cast<double>(wet[n]) * wet[n];
      eo += g * acc * g * acc;
    }
    const double db = 10.0 * std::log10(ew / eo);
    INFO("old level " << oldLevel << " dB: new against old " << db << " dB");
    if (oldLevel + 3.010299956639812 <= 12.0) CHECK(std::fabs(db) <= 0.05);
    else CHECK(db == Approx(12.0 - oldLevel - 3.010299956639812).margin(0.05));  // the slot level's +12 dB clamp
  }
}

// ------------------------------------------------------------------------------------ a pack held in memory

TEST_CASE("cab: a pack held in memory plays exactly as the same responses in a folder, and its reference is saved") {
  // Two speakers, three positions across the cone, two off the grille, named the way the built-in pack is (its
  // mapping scheme), written once as files and once handed over in memory, in another order.
  const char* mapping = R"json({"phase": "aligned", "schemes": [{"regex": "^(?P<mic>57-type) (?P<speaker>LL|LR|UL|UR) (?P<x>[0-9.]+)in (?P<dist>[0-9.]+)in$", "units": {"x": "in", "dist": "in"}}]})json";
  TempDir dir;
  std::vector<std::string> names;
  std::vector<std::vector<float>> data;
  for (const char* speaker : {"UR", "LL"})
    for (double x : {0.5, 0.0, 0.25})
      for (double g : {0.5, 0.0}) {
        char name[64];
        std::snprintf(name, sizeof name, "57-type %s %.2fin %.1fin.wav", speaker, x, g);
        const auto h = notchIr(2500.0 + 800.0 * x + 300.0 * g + (speaker[0] == 'U' ? 150.0 : 0.0), 15.0, 6.0, 2048);
        writeWav(dir.path / name, h);
        names.push_back(name);
        data.push_back(toFloat(h));
      }
  { std::ofstream(dir.path / "magmora-positions.json") << mapping; }
  std::vector<Chain::CabinetPackResponse> responses;
  for (size_t k = 0; k < names.size(); ++k)
    responses.push_back({names[k].c_str(), 48000.0, data[k].data(), static_cast<int>(data[k].size())});

  Chain disk, memory;
  std::string error;
  for (Chain* k : {&disk, &memory}) {
    k->setParameter(kChannel, 0.0);
    REQUIRE(k->prepare(48000.0, 64, 1));
  }
  REQUIRE(disk.loadCabinetPack(dir.path.string().c_str(), &error));
  CHECK_FALSE(memory.loadCabinetPackMemory("builtin:test", responses.data(), 0, mapping, &error));  // nothing to load
  CHECK_FALSE(memory.loadCabinetPackMemory("builtin:test", responses.data(), static_cast<int>(responses.size()), "{", &error));
  CHECK_FALSE(memory.cabinetPackLoaded());
  REQUIRE(memory.loadCabinetPackMemory("builtin:test", responses.data(), static_cast<int>(responses.size()), mapping, &error));
  CHECK(std::string(memory.cabinetPackReference()) == "builtin:test");
  CHECK(memory.cabinetStatus().folder == "builtin:test");
  CHECK(memory.cabinetMicCount() == 1);
  CHECK(memory.cabinetMicName(0) == "57-type");
  CHECK(memory.cabinetSpeakers(0) == std::vector<std::string>{"LL", "UR"});
  CHECK(memory.cabinetSpeakers(0) == disk.cabinetSpeakers(0));

  for (Chain* k : {&disk, &memory}) {
    k->setParameter(kCabOn, 1.0);
    k->setParameter(kMic1On, 1.0);
    k->setParameter(kMic1Position, 0.3);   // 0.15 in: between 0 and 0.25 in, morphed
    k->setParameter(kMic1Distance, 5.0);   // between 0 and 0.5 in off the grille, morphed
    k->setCabinetMicSource(0, "UR", "");
    REQUIRE(k->waitForCabinet());
  }
  const CabinetStatus a = disk.cabinetStatus(), b = memory.cabinetStatus();
  CHECK(b.phase == "aligned");
  CHECK(a.phase == b.phase);
  CHECK(b.mic[0].morphX);
  CHECK(b.mic[0].morphDist);
  CHECK(b.mic[0].speaker == "UR");
  CHECK(a.mic[0].xMm == b.mic[0].xMm);
  CHECK(a.mic[0].distMm == b.mic[0].distMm);
  CHECK(a.files == b.files);
  std::vector<float> x, y;
  play(disk, x, 120);
  play(memory, y, 120);
  CHECK(x == y);  // bit for bit

  std::vector<uint8_t> state;
  memory.getState(state);
  Chain restored;
  REQUIRE(restored.prepare(48000.0, 64, 1));
  REQUIRE(restored.setState(state.data(), state.size()));
  CHECK(std::string(restored.cabinetPackReference()) == "builtin:test");
  CHECK(restored.cabinetMicSpeaker(0) == "UR");
}

namespace {

void writeWav64(const fs::path& path, const std::vector<double>& x, uint32_t rate = 48000) {
  std::ofstream o(path, std::ios::binary);
  auto u32 = [&](uint32_t v) { o.write(reinterpret_cast<const char*>(&v), 4); };
  auto u16 = [&](uint16_t v) { o.write(reinterpret_cast<const char*>(&v), 2); };
  o.write("RIFF", 4);
  u32(static_cast<uint32_t>(36 + 8 * x.size()));
  o.write("WAVEfmt ", 8);
  u32(16);
  u16(3);
  u16(1);
  u32(rate);
  u32(rate * 8);
  u16(8);
  u16(64);
  o.write("data", 4);
  u32(static_cast<uint32_t>(8 * x.size()));
  for (double v : x) o.write(reinterpret_cast<const char*>(&v), 8);
}

// Slot 1 alone, playing its response file; the head off so that the cabinet sees the input as it is.
void slotOneOnly(Chain& c) {
  c.setParameter(kAmpOn, 0.0);
  c.setParameter(kCabOn, 1.0);
  c.setParameter(kMic1On, 1.0);
  c.setParameter(kMic1Source, 1.0);
  c.setParameter(kMic2On, 0.0);
}

struct Rendered {
  int nonFinite = 0;
  double peak = 0.0;
};

Rendered renderConstant(Chain& c, int blocks, float level) {
  std::vector<float> l(64, level), r(64, level), ol(64), orr(64);
  const float* in[2] = {l.data(), r.data()};
  float* out[2] = {ol.data(), orr.data()};
  Rendered result;
  for (int b = 0; b < blocks; ++b) {
    c.process(in, out, 64);
    for (const std::vector<float>* v : {&ol, &orr})
      for (float s : *v) {
        if (!std::isfinite(s)) ++result.nonFinite;
        else result.peak = std::max(result.peak, static_cast<double>(std::fabs(s)));
      }
  }
  return result;
}

}  // namespace

TEST_CASE("cab: a 64-bit response file with a sample beyond float range plays finite") {
  TempDir dir;
  std::vector<double> x(480, 0.0);
  x[0] = 1.0;
  x[10] = 0.5;
  x[200] = 1e300;  // finite as a double, infinite as a float
  x[201] = -1e300;
  const fs::path file = dir.path / "huge64.wav";
  writeWav64(file, x);
  Chain c;
  REQUIRE(c.prepare(48000.0, 64, 2));
  slotOneOnly(c);
  std::string error;
  REQUIRE(c.loadCabinetIr(0, file.string().c_str(), &error));
  REQUIRE(c.cabinetIrNormalize(0));
  REQUIRE(c.waitForCabinet(5.0));
  const Rendered r = renderConstant(c, 200, 0.3f);
  CHECK(r.nonFinite == 0);
  CHECK(r.peak > 0.05);  // the rest of the response plays
}

TEST_CASE("cab: responses with samples that are not finite are refused, in a slot and in a pack") {
  const float nan = std::numeric_limits<float>::quiet_NaN(), inf = std::numeric_limits<float>::infinity();
  Chain c;
  REQUIRE(c.prepare(48000.0, 64, 2));
  for (float bad : {nan, inf, -inf}) {
    std::vector<float> h(256, 0.f);
    h[0] = 1.f;
    h[100] = bad;
    const float* ch[1] = {h.data()};
    std::string error;
    CHECK_FALSE(c.loadCabinetIrSamples(0, ch, 1, static_cast<int>(h.size()), 48000.0, &error));
    CHECK_FALSE(error.empty());
    CHECK_FALSE(c.cabinetIrLoaded(0));
  }
  // a pack in memory: a response that is not finite is skipped; the others play
  const char* mapping = R"json({"schemes": [{"regex": "^(?P<mic>57-type) (?P<x>[0-9.]+)in$", "units": {"x": "in"}}]})json";
  std::vector<std::vector<float>> data;
  for (double x : {0.0, 0.5, 1.0}) {
    const auto h = notchIr(2500.0 + 800.0 * x, 15.0, 6.0, 1024);
    data.push_back(toFloat(h));
  }
  data[1][17] = nan;
  const char* names[] = {"57-type 0.0in.wav", "57-type 0.5in.wav", "57-type 1.0in.wav"};
  std::vector<Chain::CabinetPackResponse> responses;
  for (size_t k = 0; k < data.size(); ++k)
    responses.push_back({names[k], 48000.0, data[k].data(), static_cast<int>(data[k].size())});
  std::string error;
  REQUIRE(c.loadCabinetPackMemory("builtin:test", responses.data(), 3, mapping, &error));
  c.setParameter(kAmpOn, 0.0);
  c.setParameter(kCabOn, 1.0);
  c.setParameter(kMic1On, 1.0);
  c.setParameter(kMic1Source, 0.0);
  c.setParameter(kMic1Position, 0.4);  // between the two responses that are left
  REQUIRE(c.waitForCabinet(5.0));
  CHECK(c.cabinetStatus().files == 2);
  CHECK(c.cabinetStatus().skipped == 1);
  const Rendered r = renderConstant(c, 100, 0.3f);
  CHECK(r.nonFinite == 0);
  CHECK(r.peak > 0.0);
  // every response not finite: the pack is refused
  for (auto& d : data) d[3] = inf;
  CHECK_FALSE(c.loadCabinetPackMemory("builtin:bad", responses.data(), 3, mapping, &error));
}

TEST_CASE("chain: nothing after the head can make the output non-finite or louder than the engine's clamp") {
  // a response with huge (finite) taps and normalisation off: the convolution overflows
  Chain c;
  REQUIRE(c.prepare(48000.0, 64, 2));
  slotOneOnly(c);
  c.setCabinetIrNormalize(0, false);
  std::vector<float> h(1000, 0.f);
  for (size_t i = 0; i < h.size(); i += 3) h[i] = 3e37f;
  const float* ch[1] = {h.data()};
  REQUIRE(c.loadCabinetIrSamples(0, ch, 1, static_cast<int>(h.size()), 48000.0));
  REQUIRE(c.waitForCabinet(5.0));
  const Rendered r = renderConstant(c, 200, 0.3f);
  CHECK(r.nonFinite == 0);
  CHECK(r.peak <= 4.0);  // engine::kOutputClamp
  CHECK(r.peak == 4.0);
}

TEST_CASE("cab: a path guessed from the names runs from the bright end (the cap)") {
  // a decaying exponential a^n: a near 1 is dark (low-passed), near 0 bright
  auto onePole = [](double a) {
    std::vector<double> x(2048);
    for (size_t n = 0; n < x.size(); ++n) x[n] = std::pow(a, static_cast<double>(n));
    return x;
  };
  const char letters[] = "ABCDE";
  const double darkToBright[] = {0.95, 0.8, 0.6, 0.4, 0.2};
  auto stepOf = [](const cab::Pack& pack, const std::string& name) {
    for (const cab::PackFile& f : pack.files())
      if (f.name == name) return f.pos.step;
    return -1;
  };
  std::string error;

  SECTION("lettered dark to bright: turned round") {
    TempDir dir;
    for (int k = 0; k < 5; ++k) writeWav(dir.path / ("PK-57-" + std::string(1, letters[k]) + ".wav"), onePole(darkToBright[k]));
    cab::Pack pack;
    REQUIRE(pack.load(dir.path.string(), "", &error));
    CHECK(stepOf(pack, "PK-57-E.wav") == 0);
    CHECK(stepOf(pack, "PK-57-C.wav") == 2);
    CHECK(stepOf(pack, "PK-57-A.wav") == 4);
  }
  SECTION("lettered bright to dark: as named") {
    TempDir dir;
    for (int k = 0; k < 5; ++k) writeWav(dir.path / ("PK-57-" + std::string(1, letters[k]) + ".wav"), onePole(darkToBright[4 - k]));
    cab::Pack pack;
    REQUIRE(pack.load(dir.path.string(), "", &error));
    CHECK(stepOf(pack, "PK-57-A.wav") == 0);
    CHECK(stepOf(pack, "PK-57-E.wav") == 4);
  }
  SECTION("an order the mapping file gives is kept") {
    TempDir dir;
    for (int k = 0; k < 5; ++k) writeWav(dir.path / ("PK-57-" + std::string(1, letters[k]) + ".wav"), onePole(darkToBright[k]));
    std::ofstream(dir.path / cab::kMappingFileName)
        << R"({"schemes": [{"regex": "-(?P<mic>[0-9]+)-(?P<step>[A-E])$", "order": ["A", "B", "C", "D", "E"]}]})";
    cab::Pack pack;
    REQUIRE(pack.load(dir.path.string(), "", &error));
    CHECK(stepOf(pack, "PK-57-A.wav") == 0);
    CHECK(stepOf(pack, "PK-57-E.wav") == 4);
  }
}
