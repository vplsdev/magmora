// The chain shell (gates G-X1 and G-X2 for the head with every block off: bit for bit, the same latency).
#include <algorithm>
#include <magmora/Chain.h>

#include <catch.hpp>

#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

#include "chain/Json.h"

using namespace magmora;

namespace {

constexpr double kPi = 3.14159265358979323846;

std::vector<float> playing(double rate, int samples, double level = 0.4) {
  std::vector<float> x(static_cast<size_t>(samples));
  for (int n = 0; n < samples; ++n) {
    const double t = n / rate;
    x[static_cast<size_t>(n)] = static_cast<float>(
        level * std::exp(-std::fmod(t, 0.1) * 25.0) * (std::sin(2 * kPi * 82.4 * t) + 0.4 * std::sin(2 * kPi * 247.0 * t)));
  }
  return x;
}

// Stereo float renders of the same input, the engine's alone and the chain's; the right input is a different
// signal so that dual-mono routing is exercised too.
template <class Run>
std::vector<float> render(Run&& process, const std::vector<float>& x, int channels, int block) {
  std::vector<float> l(x), r(x.size()), out;
  for (size_t n = 0; n < r.size(); ++n) r[n] = 0.5f * x[(n * 7) % x.size()];
  for (size_t at = 0; at < x.size(); at += static_cast<size_t>(block)) {
    const int frames = static_cast<int>(std::min<size_t>(static_cast<size_t>(block), x.size() - at));
    float* io[2] = {l.data() + at, r.data() + at};
    float right[4096];
    float* outs[2] = {l.data() + at, channels == 2 ? r.data() + at : right};
    process(io, outs, frames);
    if (channels == 1) std::copy_n(right, frames, r.data() + at);
  }
  out = l;
  out.insert(out.end(), r.begin(), r.end());
  return out;
}

bool sameBits(const std::vector<float>& a, const std::vector<float>& b) {
  return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0;
}

std::vector<float> engineRender(Engine& e, const std::vector<float>& x, int channels, int block) {
  return render(
      [&](float* const* in, float* const* out, int n) {
        e.process(in, out, n);
        if (channels == 1) std::copy_n(out[0], n, out[1]);
      },
      x, channels, block);
}

std::vector<float> chainRender(Chain& c, const std::vector<float>& x, int channels, int block) {
  return render([&](float* const* in, float* const* out, int n) { c.process(in, out, n); }, x, channels, block);
}

std::string readFile(const std::filesystem::path& p) {
  std::ifstream f(p, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

// The head state chunk as plug-in 0.1.0 wrote it (version 1: no value set), built here byte by byte from the
// documented layout, independently of the chain's own reader.
struct Bytes {
  std::vector<uint8_t> b;
  template <class T>
  void put(T v) {
    const auto* p = reinterpret_cast<const uint8_t*>(&v);
    b.insert(b.end(), p, p + sizeof(T));
  }
  void str(const std::string& s) {
    put(static_cast<int32_t>(s.size()));
    b.insert(b.end(), s.begin(), s.end());
  }
};

std::vector<uint8_t> headChunk(int version, int flags, const std::vector<std::pair<std::string, double>>& values,
                               const char* set = nullptr) {
  Bytes body;
  body.put(static_cast<int32_t>(flags));
  body.put(static_cast<int32_t>(values.size()));
  for (const auto& [key, v] : values) {
    body.str(key);
    body.put(v);
  }
  body.str("Talus");
  body.put(1.25f);
  if (version >= 2) {
    body.str(set ? set : "prototype");
    body.str(Engine::valueSetModelHash(Engine::findValueSet(set ? set : "prototype")));
  }
  Bytes out;
  out.put(static_cast<uint32_t>(0x41524353));
  out.put(static_cast<int32_t>(version));
  out.put(static_cast<int32_t>(body.b.size()));
  out.b.insert(out.b.end(), body.b.begin(), body.b.end());
  return out.b;
}

const std::vector<std::pair<std::string, double>> kSession = {
    {"input_trim", 3.5}, {"input_jack", 1}, {"channel", 0}, {"bright", 1}, {"crunch", 1}, {"pre_rhythm", 7.25},
    {"low", 6.5}, {"mid", 3.0}, {"high", 7.0}, {"post_rhythm", 5.5}, {"resonance", 8.0}, {"presence", 2.5},
    {"output_level", -6.0}, {"gate_threshold", -70.0}, {"phase_mode", 1}, {"unknown_future_key", 3.0}};

void applyToEngine(Engine& e, const std::vector<std::pair<std::string, double>>& values) {
  for (const auto& [key, v] : values)
    for (const ParamInfo& p : kParams)
      if (key == p.key) e.setParam(p.id, v);
}

}  // namespace

TEST_CASE("chain: the parameter table continues the head's and stays consistent") {
  REQUIRE(Chain::parameterCount() == kNumChainParams);
  REQUIRE(kNumParams == 22);
  std::set<std::string> keys;
  for (int id = 0; id < Chain::parameterCount(); ++id) {
    const ParamInfo* p = Chain::parameterInfo(id);
    REQUIRE(p != nullptr);
    CHECK(static_cast<int>(p->id) == id);
    CHECK(keys.insert(p->key).second);
    CHECK(p->min < p->max);
    CHECK(p->def >= p->min);
    CHECK(p->def <= p->max);
    CHECK(Chain::findParameter(p->key) == id);
  }
  CHECK(Chain::parameterInfo(-1) == nullptr);
  CHECK(Chain::parameterInfo(kNumChainParams) == nullptr);
  Chain c;
  for (ChainBlock b : {ChainBlock::Pedal, ChainBlock::Eq, ChainBlock::Cabinet, ChainBlock::UserIr, ChainBlock::FxSlot})
    CHECK_FALSE(c.blockOn(b));  // a new chain, like an old session, has every block off
  c.setParameter(kEqHighPassSlope, 1.4);
  CHECK(c.getParameter(kEqHighPassSlope) == 1.0);
  c.setParameter(kMic1Pan, -7.0);
  CHECK(c.getParameter(kMic1Pan) == -1.0);
  c.setParameter(kPresence, 3.0);
  CHECK(c.engine().getParam(kPresence) == 3.0);
}

TEST_CASE("chain: with every block off the output is the engine's, bit for bit, at the engine's latency") {
  struct Setup {
    double rate;
    Quality quality;
    int phase, channels, mode, block;
  };
  const Setup setups[] = {
      {48000.0, Quality::Standard, 0, 2, 0, 64},   {48000.0, Quality::Standard, 0, 1, 0, 64},
      {48000.0, Quality::Standard, 1, 2, 2, 100},  {44100.0, Quality::High, 0, 2, 1, 512},
      {96000.0, Quality::Standard, 0, 2, 2, 128},  {192000.0, Quality::High, 0, 2, 0, 64},
      {48000.0, Quality::Ultra, 0, 1, 0, 37},
  };
  for (const Setup& s : setups) {
    INFO("rate " << s.rate << " quality " << static_cast<int>(s.quality) << " channels " << s.channels);
    Engine e;
    Chain c;
    for (auto [id, v] : {std::pair<ParamId, double>{kQuality, static_cast<double>(s.quality)}, {kPhaseMode, s.phase},
                         {kChannelMode, s.mode}}) {
      e.setParam(id, v);
      c.setParameter(id, v);
    }
    REQUIRE(e.prepare(s.rate, s.block, s.channels));
    REQUIRE(c.prepare(s.rate, s.block, s.channels));
    CHECK(c.latencySamples() == e.latencySamples());
    const std::vector<float> x = playing(s.rate, static_cast<int>(s.rate * (s.quality == Quality::Ultra ? 0.1 : 0.3)));
    CHECK(sameBits(engineRender(e, x, s.channels, s.block), chainRender(c, x, s.channels, s.block)));
  }
}

TEST_CASE("chain: latency equals the engine's at every rate, quality and phase mode") {
  for (double rate : {44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0})
    for (int q = 0; q < 3; ++q)
      for (int phase = 0; phase < 2; ++phase) {
        Engine e;
        Chain c;
        e.setParam(kQuality, q);
        e.setParam(kPhaseMode, phase);
        c.setParameter(kQuality, q);
        c.setParameter(kPhaseMode, phase);
        REQUIRE(e.prepare(rate, 64, 2));
        REQUIRE(c.prepare(rate, 64, 2));
        CHECK(c.latencySamples() == e.latencySamples());
      }
}

TEST_CASE("chain: every preset loads and renders as the engine with the preset's parameters") {
  const std::filesystem::path dir = std::filesystem::path(MAGMORA_SOURCE_DIR) / "presets";
  int count = 0;
  for (const auto& entry : std::filesystem::directory_iterator(dir)) {
    if (entry.path().extension() != ".json") continue;
    ++count;
    INFO(entry.path().filename().string());
    const std::string json = readFile(entry.path());
    const auto root = chain::parseJson(json.data(), json.size());
    REQUIRE(root);
    Engine e;
    if (const chain::JsonValue* params = root->member("params"))
      for (const auto& [key, v] : params->members)
        for (const ParamInfo& p : kParams)
          if (key == p.key && !Chain::isSessionParameter(p.id)) e.setParam(p.id, v->number);
    Chain c;
    REQUIRE(c.applyPreset(json.data(), json.size()));
    REQUIRE(e.prepare(48000.0, 64, 2));
    REQUIRE(c.prepare(48000.0, 64, 2));
    for (int id = 0; id < kNumParams; ++id) CHECK(c.getParameter(id) == e.getParam(static_cast<ParamId>(id)));
    // Bedrock and the full-chain presets switch the cabinet on (the plug-in's built-in pack plays);
    // no other preset touches it. A preset never carries a pack: here no pack is loaded, so the cabinet passes the
    // head through, and a preset whose only block is the cabinet still renders as the engine, bit for bit.
    const chain::JsonValue* cab = root->member("cab");
    const chain::JsonValue* cabOn = cab ? cab->member("cab_on") : nullptr;
    CHECK(c.blockOn(ChainBlock::Cabinet) == (cabOn && cabOn->number == 1.0));
    // the full-chain presets (pedal or EQ on) render through those blocks; the others are the engine, bit for bit
    bool blocks = false;
    for (ChainBlock b : {ChainBlock::Pedal, ChainBlock::Eq, ChainBlock::UserIr, ChainBlock::FxSlot})
      blocks = blocks || c.blockOn(b);
    for (const char* block : {"pedal", "eq", "cab", "fx"})
      if (const chain::JsonValue* o = root->member(block))
        for (const auto& [key, v] : o->members)
          if (const int id = Chain::findParameter(key.c_str()); id >= 0) CHECK(c.getParameter(id) == v->number);
    const std::vector<float> x = playing(48000.0, 14400);
    if (!blocks) CHECK(sameBits(engineRender(e, x, 2, 64), chainRender(c, x, 2, 64)));
  }
  CHECK(count >= 16);
}

TEST_CASE("chain: a version 0.1.0 session loads and renders as the engine did") {
  // the committed fixture (dsp/tests/data/head-state-0.1.0.bin, written by gen_state_fixture.py from the 0.1.0
  // layout) and the same layout built here; both must give the engine's sound with every new block off
  const std::string fixture = readFile(std::filesystem::path(MAGMORA_SOURCE_DIR) / "dsp/tests/data/head-state-0.1.0.bin");
  REQUIRE(fixture.size() > 16);
  const std::vector<uint8_t> built = headChunk(1, 0, kSession);
  CHECK(std::vector<uint8_t>(fixture.begin(), fixture.end()) == built);
  for (const std::vector<uint8_t>& state : {built, std::vector<uint8_t>(fixture.begin(), fixture.end())}) {
    Engine e;
    applyToEngine(e, kSession);
    Chain c;
    c.setParameter(kCabOn, 1.0);  // whatever the chain held before, an old session turns the new blocks off
    c.setParameter(kPresence, 9.0);
    REQUIRE(c.setState(state.data(), state.size()));
    CHECK_FALSE(c.blockOn(ChainBlock::Cabinet));
    CHECK(std::string(c.presetName()) == "Talus");
    CHECK(c.uiScale() == 1.25f);
    CHECK(c.engine().valueSet() == Engine::findValueSet("prototype"));
    REQUIRE(e.prepare(48000.0, 64, 2));
    REQUIRE(c.prepare(48000.0, 64, 2));
    for (int id = 0; id < kNumParams; ++id) CHECK(c.getParameter(id) == e.getParam(static_cast<ParamId>(id)));
    CHECK(c.latencySamples() == e.latencySamples());
    const std::vector<float> x = playing(48000.0, 14400);
    CHECK(sameBits(engineRender(e, x, 2, 64), chainRender(c, x, 2, 64)));
  }
}

TEST_CASE("chain: a version 2 head state keeps its value set") {
  const std::vector<uint8_t> state = headChunk(2, 0, kSession, "as-drawn");
  Chain c;
  REQUIRE(c.setState(state.data(), state.size()));
  CHECK(c.engine().valueSet() == Engine::findValueSet("as-drawn"));
}

TEST_CASE("chain: state round trip, unknown sections kept, missing sections off, damage refused") {
  Chain a;
  a.setParameter(kPreLead, 8.5);
  a.setParameter(kQuality, 1.0);
  a.setParameter(kPedalOn, 1.0);
  a.setParameter(kPedalDrive, 7.0);
  a.setParameter(kCabOn, 1.0);
  a.setParameter(kMic2Distance, 120.0);
  a.setParameter(kEqBand3Gain, -4.5);
  a.setParameter(kUserIrOn, 1.0);
  a.setUserIrReference("my cab.wav");
  a.setSessionInfo("Rime", 1.5f);
  REQUIRE(a.engine().setValueSet(Engine::findValueSet("as-drawn")));
  std::vector<uint8_t> state;
  a.getState(state);

  Chain b;
  REQUIRE(b.setState(state.data(), state.size()));
  for (int id = 0; id < kNumChainParams; ++id) CHECK(b.getParameter(id) == a.getParameter(id));
  CHECK(std::string(b.userIrReference()) == "my cab.wav");
  CHECK(std::string(b.presetName()) == "Rime");
  CHECK(b.engine().valueSet() == Engine::findValueSet("as-drawn"));
  std::vector<uint8_t> again;
  b.getState(again);
  CHECK(again == state);

  // the head section is the head chunk exactly as the plug-in writes it: a bare copy of it loads on its own
  {
    // section layout: tag, version, hash, size, payload; the head is first
    size_t at = 12;
    int32_t hashLength = 0, size = 0;
    std::memcpy(&hashLength, state.data() + at + 8, 4);
    std::memcpy(&size, state.data() + at + 12 + hashLength, 4);
    const uint8_t* head = state.data() + at + 16 + hashLength;
    uint32_t magic = 0;
    std::memcpy(&magic, head, 4);
    CHECK(magic == 0x41524353);
    Chain h;
    REQUIRE(h.setState(head, static_cast<size_t>(size)));
    for (int id = 0; id < kNumParams; ++id) CHECK(h.getParameter(id) == a.getParameter(id));
    CHECK_FALSE(h.blockOn(ChainBlock::Pedal));
  }

  // an unknown section from a later version is kept and written back
  std::vector<uint8_t> later = state;
  int32_t sections = 0;
  std::memcpy(&sections, later.data() + 8, 4);
  ++sections;
  std::memcpy(later.data() + 8, &sections, 4);
  Bytes extra;
  extra.b = {'d', 'l', 'y', ' '};
  extra.put(int32_t{3});
  extra.str("abc");
  extra.put(int32_t{5});
  for (uint8_t v : {1, 2, 3, 4, 5}) extra.b.push_back(v);
  later.insert(later.end(), extra.b.begin(), extra.b.end());
  Chain d;
  REQUIRE(d.setState(later.data(), later.size()));
  std::vector<uint8_t> written;
  d.getState(written);
  CHECK(written == later);

  // a state without the cabinet section loads the cabinet off
  {
    Chain e;
    std::vector<uint8_t> s;
    e.getState(s);
    Chain f;
    f.setParameter(kCabOn, 1.0);
    REQUIRE(f.setState(s.data(), s.size()));
    CHECK_FALSE(f.blockOn(ChainBlock::Cabinet));
  }

  // every truncation and a sample of corruptions are refused and change nothing
  Chain g;
  g.setParameter(kMid, 2.0);
  g.setParameter(kEqOn, 1.0);
  for (size_t n = 0; n < state.size(); ++n) CHECK_FALSE(g.setState(state.data(), n));
  for (size_t at : {size_t{0}, size_t{8}, size_t{12}, size_t{20}}) {  // magic, section count, head tag, hash length
    std::vector<uint8_t> bad = state;
    bad[at] ^= 0xFF;
    CHECK_FALSE(g.setState(bad.data(), bad.size()));
  }
  CHECK(g.getParameter(kMid) == 2.0);
  CHECK(g.blockOn(ChainBlock::Eq));
}

TEST_CASE("chain: presets set the blocks they name and leave the others") {
  Chain c;
  c.setParameter(kCabOn, 1.0);
  c.setParameter(kMic1Distance, 80.0);
  c.setParameter(kInputTrim, 4.0);
  const std::string amp = R"({"name": "x", "params": {"pre_lead": 9, "input_trim": -10}})";
  REQUIRE(c.applyPreset(amp.data(), amp.size()));
  CHECK(c.getParameter(kPreLead) == 9.0);
  CHECK(c.getParameter(kMid) == kParams[kMid].def);
  CHECK(c.getParameter(kInputTrim) == 4.0);  // a session parameter: never from a preset
  CHECK(c.blockOn(ChainBlock::Cabinet));     // no "cab" object: the cabinet is left as it is
  CHECK(c.getParameter(kMic1Distance) == 80.0);
  const std::string cab = R"({"cab": {"cab_on": true, "mic1_type": 1}, "pedal": {"pedal_drive": 8}})";
  REQUIRE(c.applyPreset(cab.data(), cab.size()));
  CHECK(c.getParameter(kMic1Type) == 1.0);
  CHECK(c.getParameter(kMic1Distance) == kChainParams[kMic1Distance - kNumParams].def);
  CHECK(c.getParameter(kPedalDrive) == 8.0);
  CHECK_FALSE(c.blockOn(ChainBlock::Pedal));
  CHECK(c.getParameter(kPreLead) == 9.0);
  for (const char* bad : {"", "{", R"({"params": 3})", R"({"cab": [1]})", "{} x"}) {
    CHECK_FALSE(c.applyPreset(bad, std::strlen(bad)));
  }
  CHECK(c.getParameter(kPreLead) == 9.0);
}

TEST_CASE("chain: checkState accepts exactly what setState accepts, and changes nothing") {
  Chain a;
  a.setParameter(kEqOn, 1.0);
  a.setParameter(kPreLead, 7.25);
  std::vector<uint8_t> good;
  a.getState(good);
  Chain b;
  std::vector<uint8_t> before, after;
  b.getState(before);
  CHECK(Chain::checkState(good.data(), good.size()));
  for (size_t cut = 0; cut < good.size(); cut += 7) {  // truncations and a flipped byte at each place
    std::vector<uint8_t> damaged(good.begin(), good.begin() + static_cast<long>(cut));
    std::vector<uint8_t> flipped = good;
    flipped[cut] ^= 0xA5;
    for (const std::vector<uint8_t>* d : {&damaged, &flipped}) {
      const bool checked = Chain::checkState(d->data(), d->size());
      Chain c;
      CHECK(checked == c.setState(d->data(), d->size()));
    }
  }
  CHECK_FALSE(Chain::checkState(nullptr, 0));
  b.getState(after);
  CHECK(before == after);
}

TEST_CASE("chain: the cabinet's response length is reported for the tail") {
  Chain c;
  REQUIRE(c.prepare(48000.0, 64, 2));
  CHECK(c.cabinetResponseSeconds() == 0.0);
  std::vector<float> h(96000, 0.f);  // 2 s
  h[0] = 1.f;
  h[95999] = 0.01f;
  const float* ch[1] = {h.data()};
  c.setParameter(kCabOn, 1.0);
  c.setParameter(kMic1On, 1.0);
  c.setParameter(kMic1Source, 1.0);
  REQUIRE(c.loadCabinetIrSamples(0, ch, 1, static_cast<int>(h.size()), 48000.0));
  REQUIRE(c.waitForCabinet(10.0));
  CHECK(c.cabinetResponseSeconds() == Approx(2.0).margin(0.01));
  c.clearCabinetIr(0);
  REQUIRE(c.waitForCabinet(10.0));
  CHECK(c.cabinetResponseSeconds() == 0.0);
}
