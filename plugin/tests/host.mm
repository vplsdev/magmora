// Test host for the built bundles. usage: magmora-host <Magmora.vst3|Magmora.component> <command> [arguments]
//
//   audio            layouts, rates, block sizes, latency, bypass, reconfiguration while running
//   state            round trips, versioning, damaged input
//   automation       host automation, parameter text, relay switching and a zipper measurement
//   ui               every interaction of the panel and the gestures it sends to the host; user presets (in a
//                    folder of the harness's own, MAGMORA_PRESETS_DIR), tab gestures, meters, hint line, tuner
//   shots DIR        screenshots at several scales, positions and states
//   soak SECONDS [SEED]  randomised automation at 64-sample blocks for that much audio
//   cost             time per second of audio at 48 kHz
//   threads          state loads on a worker thread with the editor open and audio running (run it under TSan too)
//   presets          the user preset checks of `ui` alone
//   smoke-presets    one save and delete in the real ~/Library/Audio/Presets/Magmora (refuses if it exists; cleans up)
//
// No audio device is opened and no window is put on screen.
#include "host_window.h"

#include <Accelerate/Accelerate.h>
#include <magmora/Engine.h>
#include <magmora/Preset.h>
#include <pthread/qos.h>
#import <objc/message.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <random>
#include <thread>

using namespace host;

namespace {

int gPassed = 0, gFailed = 0;
std::string gBundle;

void Check(bool ok, const std::string& what, const std::string& measured = "") {
  ok ? ++gPassed : ++gFailed;
  printf("%s %s%s%s\n", ok ? "  ok  " : " FAIL ", what.c_str(), measured.empty() ? "" : ": ", measured.c_str());
  fflush(stdout);
}

template <class... A>
std::string Say(const char* format, A... a) {
  char text[512];
  snprintf(text, sizeof text, format, a...);
  return text;
}

double ThreadCpu() { return clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID) * 1e-9; }

double ThreadCpu();

double Now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

double Db(double x) { return 20. * std::log10(std::max(x, 1e-12)); }

Audio Sine(int n, double rate, double hz, double level) {
  Audio a(n);
  for (int i = 0; i < n; ++i) a[i] = static_cast<float>(level * std::sin(2. * M_PI * hz * i / rate));
  return a;
}

// A plucked-string-like test signal: decaying harmonics, struck four times a second.
Audio Pluck(int n, double rate, double level = 0.2) {
  Audio a(n);
  const double notes[] = {82.41, 110., 98., 146.83};
  const int step = static_cast<int>(rate / 4);
  for (int i = 0; i < n; ++i) {
    const double t = (i % step) / rate, f = notes[(i / step) % 4];
    double x = 0.;
    for (int h = 1; h <= 8; ++h) x += std::sin(2. * M_PI * f * h * t) * std::exp(-t * (3. + 2. * h)) / h;
    a[i] = static_cast<float>(level * x * std::min(1., t * 800.));
  }
  return a;
}

double Peak(const Audio& a, size_t from = 0, size_t to = SIZE_MAX) {
  double p = 0.;
  for (size_t i = from; i < std::min(to, a.size()); ++i) p = std::max<double>(p, std::fabs(a[i]));
  return p;
}

double Difference(const Audio& a, const Audio& b, size_t from = 0) {
  double p = 0.;
  for (size_t i = from; i < a.size(); ++i) p = std::max<double>(p, std::fabs(a[i] - b[i]));
  return p;
}

bool Finite(const Audio& a) {
  for (float x : a)
    if (!std::isfinite(x)) return false;
  return true;
}

// Rebuilding the engine happens on the main thread's timer, with audio running or not.
void Settle(Plug& p, double seconds = 0.2) {
  const Audio quiet(p.format.block);
  const double until = Now() + seconds;
  do {
    p.Run({quiet, quiet});
    Pump(0.004);
  } while (Now() < until);
}

double At(const magmora::ParamInfo& p, double plain) { return (plain - p.min) / (p.max - p.min); }

// The section tabs in the bar, in points (plugin/ui/MagmoraUI.cpp AttachSections): each is a power toggle
// (a 16-point strip, its symbol 7 points in) followed by the section's name.
constexpr float kTabPedal = 293.f, kTabAmp = 356.f, kTabEq = 411.f, kTabCab = 464.f;      // the names
constexpr float kPowerPedal = 265.f, kPowerAmp = 331.f, kPowerEq = 391.f, kPowerCab = 441.f;  // the symbols

Bytes Craft(int32_t version, bool appended, const char* set = nullptr, const char* hash = nullptr);

extern "C" long magmora_test_allocations(void);
std::pair<std::string, std::string> UserOf(const Bytes& state);

// ---- fixtures: a synthetic cabinet pack and response files, written by the harness (nothing private is used) ------

std::string gFixtures;

void WriteWav(const std::string& path, const std::vector<float>& x, uint32_t rate = 48000) {
  FILE* f = fopen(path.c_str(), "wb");
  if (!f) Die("cannot write " + path);
  const uint32_t data = static_cast<uint32_t>(x.size() * 4), riff = 36 + data, fmt = 16, rateBytes = rate * 4;
  const uint16_t format = 3, channels = 1, align = 4, bits = 32;
  fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f); fwrite(&fmt, 4, 1, f);
  fwrite(&format, 2, 1, f); fwrite(&channels, 2, 1, f); fwrite(&rate, 4, 1, f); fwrite(&rateBytes, 4, 1, f);
  fwrite(&align, 2, 1, f); fwrite(&bits, 2, 1, f); fwrite("data", 1, 4, f); fwrite(&data, 4, 1, f);
  fwrite(x.data(), 4, x.size(), f);
  fclose(f);
}

// A decaying, coloured response whose colour depends on the microphone and the position, so moves are audible.
std::vector<float> CabResponse(int mic, double inches) {
  std::vector<float> h(9600);
  uint32_t seed = 1234u + static_cast<uint32_t>(mic * 977 + inches * 64.);
  double lp = 0.;
  const double a = 0.15 + 0.25 * inches / 2. + 0.2 * mic;
  for (size_t n = 0; n < h.size(); ++n) {
    seed = seed * 1664525u + 1013904223u;
    const double noise = (seed >> 8) / double(1 << 24) - 0.5;
    lp += a * (noise - lp);
    h[n] = static_cast<float>((n == 0 ? 1.0 : 0.0) + 0.6 * lp * std::exp(-double(n) / 700.));
  }
  return h;
}

void MakeFixtures(const char* argv0) {
  std::string dir = argv0;
  dir = dir.substr(0, dir.find_last_of('/')) + "/fixtures";
  gFixtures = dir;
  const std::string pack = dir + "/pack";
  [NSFileManager.defaultManager createDirectoryAtPath:@(pack.c_str()) withIntermediateDirectories:YES attributes:nil error:nil];
  const char* mics[] = {"DY57", "DY421"};
  for (int m = 0; m < 2; ++m)
    for (double x : {0.0, 0.5, 1.0, 1.5, 2.0}) {
      char name[128];
      snprintf(name, sizeof name, "/SPK LL CAB 4x12 %s %.2fin 0.0in PRE1.wav", mics[m], x);
      WriteWav(pack + name, CabResponse(m, x));
    }
  std::vector<float> room(14400);
  uint32_t seed = 99;
  for (size_t n = 0; n < room.size(); ++n) {
    seed = seed * 1664525u + 1013904223u;
    room[n] = static_cast<float>((n == 0 ? 1.0 : 0.0) + 0.3 * ((seed >> 8) / double(1 << 24) - 0.5) * std::exp(-double(n) / 2000.));
  }
  WriteWav(dir + "/room.wav", room);
  std::vector<float> hall(2 * 48000);  // two seconds: longer than the head's own tail
  for (size_t n = 0; n < hall.size(); ++n) {
    seed = seed * 1664525u + 1013904223u;
    hall[n] = static_cast<float>((n == 0 ? 1.0 : 0.0) + 0.2 * ((seed >> 8) / double(1 << 24) - 0.5) * std::exp(-double(n) / 30000.));
  }
  WriteWav(dir + "/hall.wav", hall);
  WriteWav(dir + "/dirac.wav", std::vector<float>{1.f});
}

// ---- the chain state: sections can be replaced, as a session saved elsewhere would have them ---------------------

template <class T>
T At(const Bytes& b, size_t at) {
  T v{};
  memcpy(&v, &b[at], sizeof v);
  return v;
}

Bytes ReplaceSection(const Bytes& state, const char* tag, const Bytes& payload, int32_t version = 1) {
  if (state.size() < 12 || memcmp(&state[0], "SCCH", 4) != 0) Die("not a chain state");
  Bytes out(state.begin(), state.begin() + 12);
  int32_t count = At<int32_t>(state, 8);
  size_t at = 12;
  bool replaced = false;
  for (int32_t n = 0; n < count; ++n) {
    const size_t start = at;
    const int32_t hash = At<int32_t>(state, at + 8);
    const int32_t size = At<int32_t>(state, at + 12 + hash);
    at += 16 + hash + size;
    if (memcmp(&state[start], tag, 4) == 0) {
      out.insert(out.end(), tag, tag + 4);
      out.insert(out.end(), reinterpret_cast<const char*>(&version), reinterpret_cast<const char*>(&version) + 4);
      const int32_t zero = 0, bytes = static_cast<int32_t>(payload.size());
      out.insert(out.end(), reinterpret_cast<const char*>(&zero), reinterpret_cast<const char*>(&zero) + 4);
      out.insert(out.end(), reinterpret_cast<const char*>(&bytes), reinterpret_cast<const char*>(&bytes) + 4);
      out.insert(out.end(), payload.begin(), payload.end());
      replaced = true;
    } else {
      out.insert(out.end(), state.begin() + start, state.begin() + at);
    }
  }
  if (!replaced) {
    ++count;
    memcpy(&out[8], &count, 4);
    out.insert(out.end(), tag, tag + 4);
    out.insert(out.end(), reinterpret_cast<const char*>(&version), reinterpret_cast<const char*>(&version) + 4);
    const int32_t zero = 0, bytes = static_cast<int32_t>(payload.size());
    out.insert(out.end(), reinterpret_cast<const char*>(&zero), reinterpret_cast<const char*>(&zero) + 4);
    out.insert(out.end(), reinterpret_cast<const char*>(&bytes), reinterpret_cast<const char*>(&bytes) + 4);
    out.insert(out.end(), payload.begin(), payload.end());
  }
  out.insert(out.end(), state.begin() + at, state.end());  // VST3's bypass word
  return out;
}

void PutStr(Bytes& b, const std::string& s) {
  const int32_t n = static_cast<int32_t>(s.size());
  b.insert(b.end(), reinterpret_cast<const char*>(&n), reinterpret_cast<const char*>(&n) + 4);
  b.insert(b.end(), s.begin(), s.end());
}

// "cabp" as XP6 to XP10 wrote it (version 1): folder, mapping, per microphone its name, speaker and variant.
Bytes PackPayloadV1(const std::string& folder) {
  Bytes b;
  PutStr(b, folder);
  PutStr(b, "");
  for (int m = 0; m < 2; ++m) {
    PutStr(b, "");
    PutStr(b, "");
    PutStr(b, "");
  }
  return b;
}

// Version 2: then per slot its response file, normalisation and raw flag.
Bytes PackPayload(const std::string& folder, const std::string& file1 = "", const std::string& file2 = "") {
  Bytes b = PackPayloadV1(folder);
  for (const std::string* file : {&file1, &file2}) {
    PutStr(b, *file);
    const int32_t normalize = 1, raw = 0;
    b.insert(b.end(), reinterpret_cast<const char*>(&normalize), reinterpret_cast<const char*>(&normalize) + 4);
    b.insert(b.end(), reinterpret_cast<const char*>(&raw), reinterpret_cast<const char*>(&raw) + 4);
  }
  return b;
}

Bytes WithPack(const Bytes& state, const std::string& folder, const std::string& file1 = "", const std::string& file2 = "") {
  return ReplaceSection(state, "cabp", PackPayload(folder, file1, file2), 2);
}

// The old series response file ("uir "), as sessions saved before the cabinet slots took files have it.
Bytes IrPayload(const std::string& path, bool on = true) {
  Bytes b;
  const int32_t count = 2, normalize = 1;
  const double onValue = on ? 1. : 0., level = 0.;
  b.insert(b.end(), reinterpret_cast<const char*>(&count), reinterpret_cast<const char*>(&count) + 4);
  PutStr(b, "user_ir_on");
  b.insert(b.end(), reinterpret_cast<const char*>(&onValue), reinterpret_cast<const char*>(&onValue) + 8);
  PutStr(b, "user_ir_level");
  b.insert(b.end(), reinterpret_cast<const char*>(&level), reinterpret_cast<const char*>(&level) + 8);
  PutStr(b, path);
  b.insert(b.end(), reinterpret_cast<const char*>(&normalize), reinterpret_cast<const char*>(&normalize) + 4);
  return b;
}

bool Contains(const Bytes& b, const std::string& text) {
  return std::search(b.begin(), b.end(), text.begin(), text.end()) != b.end();
}

// The whole chain on: pedal, EQ (one band raised), the fixture pack; slot 1 a pack microphone, slot 2 a response file.
void ChainOn(Plug& p, bool withFiles = true) {
  if (withFiles) {
    Bytes s;
    p.Save(s);
    p.Load(WithPack(s, gFixtures + "/pack", "", gFixtures + "/room.wav"));
    Pump(0.1);
  }
  p.SetPlain(magmora::kPedalOn, 1.);
  p.SetPlain(magmora::kEqOn, 1.);
  p.SetPlain(magmora::kEqBand2Gain, 6.);
  p.SetPlain(magmora::kCabOn, 1.);
  p.SetPlain(magmora::kMic1On, 1.);
  p.SetPlain(magmora::kMic2On, 1.);
  if (withFiles) p.SetPlain(magmora::kMic2Source, 1.);
}

// ---- audio ------------------------------------------------------------------------------------------------------

void TestAudio() {
  const int layouts[][2] = {{1, 1}, {1, 2}, {2, 2}};
  const double rates[] = {44100., 48000., 88200., 96000., 176400., 192000.};
  const int blocks[] = {1, 7, 64, 512, 4096};
  for (const auto& layout : layouts) {
    for (double rate : rates) {
      auto p = Make(gBundle);
      int bad = 0, dead = 0, refused = 0;
      double worstMono = 0.;
      for (int block : blocks) {
        Format f;
        f.rate = rate;
        f.block = block;
        f.in = layout[0];
        f.out = layout[1];
        if (!p->Setup(f)) {
          ++refused;
          continue;
        }
        const int n = std::max(8192, block * 2) / block * block;
        const Audio x = Pluck(n, rate);
        const auto out = p->Run({x, x});
        for (const Audio& c : out) {
          bad += !Finite(c);
          dead += Peak(c) < 1e-5;
        }
        if (f.out == 2) worstMono = std::max(worstMono, Difference(out[0], out[1]));
      }
      Check(!bad && !dead && !refused && worstMono == 0.,
            Say("%d in %d out at %g Hz, blocks 1..4096", layout[0], layout[1], rate),
            Say("%d refused, %d silent, %d non-finite, L-R %.1e", refused, dead, bad, worstMono));
    }
  }
  {
    auto p = Make(gBundle);
    Format f;
    f.in = 2;
    f.out = 1;
    Check(!p->Setup(f), "2 in 1 out is refused");
  }
  {
    // The output must not depend on how the host cuts the stream (the stand-alone runs at blocks of 1).
    const Audio x = Pluck(19200, 48000.);
    Audio reference;
    double worst = 0.;
    for (int block : {4096, 64, 7, 1}) {
      auto p = Make(gBundle);
      Format f;
      f.block = block;
      p->Setup(f);
      const auto out = p->Run({x});
      if (reference.empty())
        reference = out[0];
      else
        worst = std::max(worst, Difference(reference, out[0]));
    }
    Check(Db(worst) < -120., "block size invariance", Say("%.1f dBFS", Db(worst)));
  }
  {
    // In place, and a block shorter than the prepared one. Offline, as in the state test: the cabinet's first response
    // is in place before the first block whatever the build's speed.
    const Audio x = Pluck(9600, 48000.);
    auto a = Make(gBundle), b = Make(gBundle);
    Format f;
    f.block = 256;
    f.offline = true;
    a->Setup(f);
    b->Setup(f);
    const auto out = a->Run({x}), same = b->Run({x}, 100, true);
    Check(Db(Difference(out[0], same[0])) < -120. && Db(Difference(out[1], same[1])) < -120., "in-place, short blocks, 1 in 2 out",
          Say("%.1f / %.1f dBFS", Db(Difference(out[0], same[0])), Db(Difference(out[1], same[1]))));
  }
  {
    // The tail: the head's ring-out, and the cabinet's response, which a response file can make seconds long.
    auto p = Make(gBundle);
    Format f;
    f.block = 256;
    p->Setup(f);
    Settle(*p, 0.1);
    const int before = p->Tail();
    Bytes s;
    p->Save(s);
    p->Load(WithPack(s, "", gFixtures + "/hall.wav"));
    p->SetPlain(magmora::kCabOn, 1.);
    p->SetPlain(magmora::kMic1On, 1.);
    p->SetPlain(magmora::kMic1Source, 1.);
    p->SetPlain(magmora::kMic2On, 0.);
    Settle(*p, 0.3);
    const int withFile = p->Tail();
    p->SetPlain(magmora::kCabOn, 0.);
    Settle(*p, 0.3);
    const int cabinetOff = p->Tail();
    Check(before >= 0.25 * f.rate && withFile >= 2.25 * f.rate - 1 && cabinetOff >= 0.25 * f.rate && cabinetOff < withFile,
          "the tail covers the head and the cabinet's response", Say("%d, with a 2 s file %d, cabinet off %d samples", before, withFile, cabinetOff));
  }
  {
    // Left feed, sum, dual mono.
    auto p = Make(gBundle);
    Format f;
    f.in = 2;
    f.block = 256;
    p->Setup(f);
    const Audio x = Pluck(24000, 48000.), none(24000);
    auto out = p->Run({none, x});
    Check(Db(Peak(out[0], 4800)) < -40., "mono left ignores the right channel", Say("%.1f dBFS", Db(Peak(out[0], 4800))));
    p->Set(magmora::kChannelMode, 0.5);
    Settle(*p);
    out = p->Run({none, x});
    Check(Peak(out[0]) > 1e-4 && Difference(out[0], out[1]) == 0., "mono sum hears it on both outputs");
    p->Set(magmora::kChannelMode, 1.);
    Settle(*p, 0.4);
    out = p->Run({none, x});
    Check(Peak(out[1]) > 1e-4 && Peak(out[0], 4800) < 0.5 * Peak(out[1], 4800), "dual mono keeps the sides apart",
          Say("L %.1f, R %.1f dBFS", Db(Peak(out[0], 4800)), Db(Peak(out[1], 4800))));
  }
  for (double rate : rates) {
    for (int quality = 0; quality < 3; ++quality) {
      for (int phase = 0; phase < 2; ++phase) {
        auto p = Make(gBundle);
        p->Set(magmora::kQuality, quality / 2.);
        p->Set(magmora::kPhaseMode, phase);
        // VST3: before processing starts the host's values arrive through the state, as here.
        Bytes state;
        p->Save(state);
        p->Load(state);
        Format f;
        f.rate = rate;
        f.block = 64;
        p->Setup(f);
        const int latency = p->Latency();
        p->Set(kBypassId, 1.);
        p->Run({Audio(6400)});
        Audio impulse(1600);
        impulse[100] = 0.5f;
        const auto out = p->Run({impulse});
        int at = -1, others = 0;
        for (int i = 0; i < 1600; ++i) {
          if (out[1][i] == 0.5f && at < 0)
            at = i;
          else if (out[1][i] != 0.f)
            ++others;
        }
        const bool limit = phase ? latency == 0 : (rate != 48000. || quality != 0 || latency <= 64);
        Check(at - 100 == latency && !others && limit, Say("bypass latency, %g Hz quality %d phase %d", rate, quality, phase),
              Say("reported %d, measured %d", latency, at - 100));
      }
    }
  }
  {
    // The blocks around the head add no latency (Chain::latencySamples): the report is the head's with every block
    // on, and a single-sample response file as a slot's source, centred, changes nothing.
    Format f;
    f.block = 128;
    auto off = Make(gBundle), on = Make(gBundle), dirac = Make(gBundle);
    for (auto* p : {off.get(), on.get(), dirac.get()}) p->Setup(f);
    off->SetPlain(magmora::kCabOn, 0.);  // the head alone (a new instance has the built-in cabinet on)
    ChainOn(*on);
    Bytes s;
    dirac->Save(s);
    dirac->Load(WithPack(s, "", gFixtures + "/dirac.wav"));
    // Slot 1 alone, the one-sample file, centred at +3.01 dB: the constant-power centre's -3.01 dB brought to unity.
    dirac->SetPlain(magmora::kCabOn, 1.);
    dirac->SetPlain(magmora::kMic1On, 1.);
    dirac->SetPlain(magmora::kMic1Source, 1.);
    dirac->SetPlain(magmora::kMic1Level, 20. * std::log10(std::sqrt(2.)));
    for (auto* p : {off.get(), on.get(), dirac.get()}) {
      Settle(*p, 0.3);
      p->Reset();
      Settle(*p, 0.3);
    }
    Check(on->Latency() == off->Latency() && dirac->Latency() == off->Latency(),
          "every block on: the reported latency is the head's", Say("%d with the chain, %d without", on->Latency(), off->Latency()));
    const Audio x = Pluck(24000, 48000.);
    const double d = Db(Difference(off->Run({x})[0], dirac->Run({x})[0]));
    Check(d < -100., "a one-sample response file as slot 1's source: output unchanged, no delay", Say("%.1f dBFS", d));
  }
  {
    // Every section switched through its parameter. The head off with every other section off passes the input,
    // delayed by the latency the plug-in reports, which no section changes; each section then changes the sound.
    Format f;
    f.block = 128;
    const Audio x = Pluck(24000, 48000.);
    // For the unity checks a band-limited signal (110 Hz, 1 kHz, 4 kHz): the plucks restart with steps, whose content
    // above the resamplers' pass band (about 20 kHz) is all that a delayed copy cannot match.
    Audio tones(24000);
    for (int i = 0; i < 24000; ++i)
      tones[i] = static_cast<float>(0.2 * std::sin(2. * M_PI * 110. * i / 48000.) + 0.1 * std::sin(2. * M_PI * 1000. * i / 48000.) +
                                    0.05 * std::sin(2. * M_PI * 4000. * i / 48000.));
    struct Take {
      std::vector<Audio> out;
      int latency;
    };
    const auto take = [&](std::initializer_list<std::pair<int, double>> settings, const Audio& in) {
      auto p = Make(gBundle);
      p->Setup(f);
      for (const auto& [id, v] : settings) p->SetPlain(id, v);
      Settle(*p, 0.3);
      p->Reset();
      Settle(*p, 0.3);
      return Take{p->Run({in}), p->Latency()};
    };
    const Take unity = take({{magmora::kAmpOn, 0.}, {magmora::kCabOn, 0.}}, tones);
    const Take fresh = take({}, x), bare = take({{magmora::kAmpOn, 0.}, {magmora::kCabOn, 0.}}, x);
    const Take head = take({{magmora::kCabOn, 0.}}, x), cab = take({{magmora::kAmpOn, 0.}}, x);
    const Take pedal = take({{magmora::kAmpOn, 0.}, {magmora::kCabOn, 0.}, {magmora::kPedalOn, 1.}}, x);
    const Take eq = take({{magmora::kAmpOn, 0.}, {magmora::kCabOn, 0.}, {magmora::kEqOn, 1.}, {magmora::kEqBand2Gain, 6.}}, x);
    const int latency = unity.latency;
    double error = 0.;
    for (size_t i = static_cast<size_t>(latency) + 2048; i < tones.size(); ++i)
      error = std::max(error, std::fabs(double(unity.out[0][i]) - tones[i - latency]));
    Check(Db(error) - Db(Peak(tones)) < -60. && Difference(unity.out[0], unity.out[1]) == 0.,
          "amp_on off, every section off: the input, delayed by the reported latency, on both sides",
          Say("latency %d, error %.1f dB re the peak", latency, Db(error) - Db(Peak(tones))));
    bool same = true;
    for (const Take* t : {&fresh, &bare, &head, &cab, &pedal, &eq}) same &= t->latency == latency;
    Check(same, "the reported latency does not depend on any section being on", Say("%d samples", latency));
    const auto differs = [&](const Take& a, const Take& b) {
      return Db(Difference(a.out[0], b.out[0])) - Db(std::max(Peak(a.out[0]), Peak(b.out[0])));
    };
    const double dHead = differs(head, bare), dCab = differs(cab, bare), dPedal = differs(pedal, bare),
                 dEq = differs(eq, bare), dFresh = differs(fresh, cab);
    Check(dHead > -20. && dCab > -20. && dPedal > -20. && dEq > -20. && dFresh > -20.,
          "each section heard alone changes the sound (head, cabinet, pedal, EQ; head on against off)",
          Say("%.1f / %.1f / %.1f / %.1f / %.1f dB re the louder", dHead, dCab, dPedal, dEq, dFresh));

    // switched while running: the head goes, the input arrives as before, latency kept, nothing non-finite
    auto p = Make(gBundle);
    p->Setup(f);
    p->SetPlain(magmora::kCabOn, 0.);
    Settle(*p, 0.3);
    p->Run({tones});
    p->Set(magmora::kAmpOn, 0., true);
    const auto after = p->Run({tones});
    double late = 0.;
    for (size_t i = 4800; i < tones.size(); ++i) late = std::max(late, std::fabs(double(after[0][i]) - tones[i - latency]));
    p->Set(magmora::kAmpOn, 1., true);
    p->Run({tones});
    const auto back = p->Run({x});
    const double backDb = Db(Peak(back[0], 12000)) - Db(Peak(head.out[0], 12000));
    Check(Finite(after[0]) && Finite(back[0]) && Db(late) - Db(Peak(tones)) < -60. && p->Latency() == latency &&
              std::fabs(backDb) < 1.,
          "amp_on automated off and on while playing: bypassed at unity after the crossfade, the head back after",
          Say("error %.1f dB re the peak; the head back at %+.2f dB re an instance never switched", Db(late) - Db(Peak(tones)), backDb));
  }
  {
    // Ultra plays at High in real time and only renders at Ultra offline.
    const auto measure = [&](int quality, bool offline, int& latency) {
      auto p = Make(gBundle);
      p->Set(magmora::kQuality, quality / 2.);
      Bytes s;
      p->Save(s);
      p->Load(s);
      Format f;
      f.block = 512;
      f.offline = offline;
      p->Setup(f);
      latency = p->Latency();
      const Audio x = Pluck(48000, 48000.);
      const double start = ThreadCpu();
      p->Run({x});
      return ThreadCpu() - start;
    };
    int high = 0, live = 0, offline = 0;
    const double tHigh = measure(1, false, high), tLive = measure(2, false, live), tOffline = measure(2, true, offline);
    Check(live == high && tLive < 1.3 * tHigh, "Ultra in real time runs at High",
          Say("latency %d vs %d, CPU %.2f vs %.2f s per second", live, high, tLive, tHigh));
    Check(tOffline > 1.5 * tHigh, "Ultra renders at Ultra offline",
          Say("latency %d, CPU %.2f s per second (High %.2f)", offline, tOffline, tHigh));
  }
  {
    // Resets from the audio thread and state loads from a worker thread, while audio runs: nothing may block it.
    auto p = Make(gBundle);
    Format f;
    f.block = 128;
    p->Setup(f);
    std::atomic<bool> stop{false}, bad{false};
    std::atomic<double> slowest{0.};
    std::thread audio([&] {
      Audio x = Sine(128, 48000., 375., 0.05), l(128), r(128);
      const float* in[2] = {x.data(), x.data()};
      float* out[2] = {l.data(), r.data()};
      for (int n = 0; !stop.load(); ++n) {
        const double start = Now();
        if (n % 40 == 39) p->Reset();  // AudioUnitReset / setProcessing(false, true) on the audio thread
        p->Render(in, out, 128);
        slowest.store(std::max(slowest.load(), Now() - start));
        if (!Finite(l)) bad.store(true);
        std::this_thread::sleep_for(std::chrono::microseconds(700));
      }
    });
    std::thread loader([&] {
      for (int round = 0; round < 16; ++round) {
        Bytes b = Craft(2, false, round % 2 ? "as-drawn" : "prototype");
        int32_t quality = 0;
        (void)quality;
        p->Load(b);
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
      }
    });
    loader.join();
    Pump(0.3);
    stop.store(true);
    audio.join();
    Check(!bad.load() && slowest.load() < 0.02, "audio-thread resets and worker-thread state loads never block the audio thread",
          Say("slowest block with its reset %.1f ms", 1000. * slowest.load()));
  }
  {
    auto p = Make(gBundle);
    Format f;
    f.in = 2;
    f.block = 64;
    p->Setup(f);
    const int latency = p->Latency(), n = 48000;
    const Audio l = Sine(n, 48000., 330., 0.1), r = Sine(n, 48000., 441., 0.07);
    const auto out = p->Run({l, r}, 0, false, [&](int at) {
      if (at == 64 * 100) p->Set(kBypassId, 1.);
      if (at == 64 * 400) p->Set(kBypassId, 0.);
    });
    double error = 0.;
    for (int i = 64 * 120; i < 64 * 400; ++i)
      error = std::max({error, double(std::fabs(out[0][i] - l[i - latency])), double(std::fabs(out[1][i] - r[i - latency]))});
    Check(error < 1e-6, "bypass passes both channels, latency-aligned", Say("error %.1e", error));
    const auto step = [&](int from, int to) {
      double worst = 0.;
      for (int i = from; i < to; ++i) worst = std::max<double>(worst, std::fabs(out[0][i + 1] - out[0][i]));
      return worst;
    };
    const double steady = std::max(step(64 * 60, 64 * 99), step(64 * 700, 64 * 749));
    const double edge = std::max(step(64 * 99, 64 * 130), step(64 * 399, 64 * 430));
    const double dry = 0.1 * 2. * M_PI * 330. / 48000.;
    Check(edge < 1.5 * std::max(steady, dry) + 1e-3, "bypass switches without a click", Say("step %.4f, signal's own %.4f", edge, steady));
  }
  {
    auto p = Make(gBundle);
    Format f;
    f.block = 128;
    p->Setup(f);
    Pump(0.1);
    const int before = p->Latency(), notices = p->latencyNotices;
    // Only a rendering thread and a main thread, as in a host.
    std::atomic<bool> stop{false}, bad{false};
    std::thread audio([&] {
      Audio x = Sine(128, 48000., 375., 0.05), l(128), r(128);
      const float* in[2] = {x.data(), x.data()};
      float* out[2] = {l.data(), r.data()};
      while (!stop.load()) {
        p->Render(in, out, 128);
        if (!Finite(l) || Peak(l) > 4.01) bad.store(true);
        std::this_thread::sleep_for(std::chrono::microseconds(700));
      }
    });
    int seen[6] = {};
    for (int round = 0; round < 24; ++round) {
      // The VST3 queue belongs to the audio thread in this host, so only the state carries the change.
      auto donor = Make(gBundle);
      donor->Setup(f);
      donor->Set(magmora::kQuality, (round % 2) * 0.5);
      donor->Set(magmora::kPhaseMode, (round / 2) % 2);
      donor->Set(magmora::kLoad, (round / 4) % 2);
      Settle(*donor, 0.08);
      Bytes b;
      donor->Save(b);
      p->Load(b);
      Pump(0.03);
      if (round < 6) seen[round] = p->Latency();
    }
    stop.store(true);
    audio.join();
    Pump(0.1);
    Check(!bad.load(), "24 reconfigurations under a running audio thread: finite and bounded");
    const bool moved = seen[0] != seen[1] || seen[0] != seen[2];
    Check(!moved || p->latencyNotices > notices, "a latency change is announced",
          Say("%d at first, then %d %d %d, %d notices", before, seen[0], seen[1], seen[2], p->latencyNotices - notices));
  }
}

// ---- state ------------------------------------------------------------------------------------------------------

template <class T>
void Put(Bytes& b, T v) {
  const char* c = reinterpret_cast<const char*>(&v);
  b.insert(b.end(), c, c + sizeof v);
}

void PutText(Bytes& b, const char* s) {
  Put<int32_t>(b, static_cast<int32_t>(strlen(s)));
  b.insert(b.end(), s, s + strlen(s));
}

Bytes Craft(int32_t version, bool appended, const char* set, const char* hash) {
  Bytes body;
  Put<int32_t>(body, 0);
  Put<int32_t>(body, 3);
  PutText(body, "pre_lead");
  Put<double>(body, 7.25);
  PutText(body, "from_a_later_version");
  Put<double>(body, 42.);
  PutText(body, "low");
  Put<double>(body, 1e9);  // out of range
  PutText(body, "Cold Ash");
  Put<float>(body, 1.f);
  if (version >= 2) {
    PutText(body, set ? set : "prototype");
    const char* known = magmora::Engine::valueSetModelHash(magmora::Engine::findValueSet(set ? set : "prototype"));
    PutText(body, hash ? hash : known ? known : "");
  }
  if (appended) Put<double>(body, 3.);  // a field this version does not know
  Bytes all;
  Put<uint32_t>(all, 0x41524353);
  Put<int32_t>(all, version);
  Put<int32_t>(all, static_cast<int32_t>(body.size()));
  all.insert(all.end(), body.begin(), body.end());
  Put<int32_t>(all, 0);
  return all;
}

void TestState() {
  std::mt19937 random(7);
  std::uniform_real_distribution<double> unit(0., 1.);
  int same = 0, values = 0, audio = 0;
  const int trials = 50;
  for (int t = 0; t < trials; ++t) {
    auto a = Make(gBundle), b = Make(gBundle);
    Format f;
    f.block = 256;
    a->Setup(f);
    for (int id = 0; id < kAllParams; ++id) {
      if (id == magmora::kCabFloor) continue;  // hidden, never written by a host
      const magmora::ParamInfo& p = PI(id);
      double v = unit(random);
      if (p.taper == magmora::Taper::Stepped) v = std::round(v * (p.max - p.min)) / (p.max - p.min);
      if (id == magmora::kQuality && t >= 10) v = 0.;  // ten of them at the costlier rates are enough
      a->Set(id, v);
    }
    a->Run({Audio(256)});  // VST3: the processor takes its values in process
    Pump(0.03);
    Bytes first, second;
    a->Save(first);
    if (!b->Load(first)) continue;
    b->Save(second);
    same += first == second;
    bool equal = true;
    for (int id = 0; id < kAllParams; ++id) equal &= std::fabs(a->Get(id) - b->Get(id)) < 1e-6;
    values += equal;
    if (t % 5 == 0) {
      // Rendered offline: the plug-in then waits for each instance's cabinet response, however slow its worker is
      // (a sanitizer build), so that the two start alike. In real time a response not built in a second fades in.
      auto c = Make(gBundle);
      c->Load(first);
      Format offline = f;
      offline.offline = true;
      b->Setup(offline);
      c->Setup(offline);
      const Audio x = Pluck(4096, 48000.);
      audio += Difference(b->Run({x})[0], c->Run({x})[0]) == 0.;
    }
  }
  Check(same == trials, "save, load, save is bit exact", Say("%d of %d", same, trials));
  Check(values == trials, "every parameter comes back", Say("%d of %d", values, trials));
  Check(audio == trials / 5, "two instances of one state null", Say("%d of %d", audio, trials / 5));
  {
    // Hosts that hand the state over in their own way.
    auto a = Make(gBundle), b = Make(gBundle);
    a->SetPlain(magmora::kPreLead, 7.25);
    a->SetPlain(magmora::kEqOn, 1.);
    Bytes s, again;
    a->Save(s);
    if (gBundle.find(".vst3") != std::string::npos) {
      const bool loaded = b->LoadQuirky(s);
      b->Save(again);
      Check(loaded && again == s, "VST3: a state read through a stream that reports a non-OK status with its data (WaveLab)");
    } else {
      // A state the plug-in rejects, in a dictionary that names a factory preset: nothing may change.
      const std::string preset = "Basalt Wall";  // the second factory preset (generated/Presets.inc)
      Bytes before, after, damaged(s.begin(), s.begin() + static_cast<long>(s.size() / 2));
      b->Save(before);
      const bool refused = !b->LoadQuirky(damaged, preset.c_str()) && !b->LoadQuirky(Bytes{'S', 'C', 'P', 'I'}, preset.c_str());
      b->Save(after);
      Check(refused && before == after, "AU: a rejected state that names a factory preset leaves the instance untouched");
      const bool good = b->LoadQuirky(s, preset.c_str());
      b->Save(after);
      Check(good && after == s, "AU: an accepted state that names a factory preset is the state, not the preset");
    }
  }
  {
    auto a = Make(gBundle), b = Make(gBundle);
    Format f;
    a->Setup(f);
    a->Set(kBypassId, 1.);
    a->Run({Audio(64)});
    Bytes s;
    a->Save(s);
    b->Load(s);
    if (gBundle.find(".vst3") != std::string::npos) Check(b->Get(kBypassId) > 0.5, "VST3 bypass is part of the state");
  }
  {
    // Sessions from before the chain: the 0.1.0 head state (a committed fixture of the DSP package) and a head v2.
    FILE* f = fopen((std::string(MAGMORA_SOURCE_DIR) + "/dsp/tests/data/head-state-0.1.0.bin").c_str(), "rb");
    Bytes old;
    if (f) {
      int c;
      while ((c = fgetc(f)) != EOF) old.push_back(static_cast<char>(c));
      fclose(f);
    }
    auto p = Make(gBundle);
    p->SetPlain(magmora::kPedalOn, 1.);
    const bool ok = !old.empty() && p->Load(old);
    Bytes again;
    p->Save(again);
    Check(ok && p->Plain(magmora::kPedalOn) == 0. && p->Plain(magmora::kEqOn) == 0. && p->Plain(magmora::kCabOn) == 0. &&
              memcmp(again.data(), "SCCH", 4) == 0,
          "a 0.1.0 session loads with every block off and saves as a chain state", Say("%zu bytes", old.size()));
    Bytes v2 = Craft(2, false, "as-drawn");
    auto q = Make(gBundle);
    q->SetPlain(magmora::kEqOn, 1.);
    Check(q->Load(v2) && q->Plain(magmora::kEqOn) == 0. && q->Plain(magmora::kCabOn) == 0.,
          "a head-only version 2 session loads with the blocks off");
  }
  {
    // amp_on is saved; a session from before amp_on (the chain state written by the code of that time, committed by the DSP
    // package as dsp/tests/data/chain-state-a31.bin) loads with the head on and its blocks as saved.
    Format f;
    auto a = Make(gBundle), b = Make(gBundle);
    a->Setup(f);
    a->SetPlain(magmora::kAmpOn, 0.);
    a->Run({Audio(256)});
    Pump(0.03);
    Bytes s;
    a->Save(s);
    const bool saved = b->Load(s) && b->Plain(magmora::kAmpOn) == 0. && Contains(s, "amp ");
    FILE* file = fopen((std::string(MAGMORA_SOURCE_DIR) + "/dsp/tests/data/chain-state-a31.bin").c_str(), "rb");
    Bytes old;
    if (file) {
      int c;
      while ((c = fgetc(file)) != EOF) old.push_back(static_cast<char>(c));
      fclose(file);
    }
    auto q = Make(gBundle);
    q->SetPlain(magmora::kAmpOn, 0.);
    const bool loaded = !old.empty() && !Contains(old, "amp ") && q->Load(old);
    const bool head = q->Plain(magmora::kAmpOn) == 1. && q->Plain(magmora::kPedalOn) == 1. && q->Plain(magmora::kEqOn) == 1. &&
                      std::fabs(q->Plain(magmora::kEqBand2Gain) - 4.5) < 1e-3 && q->Plain(magmora::kCabOn) == 1.;
    auto r = Make(gBundle);
    r->SetPlain(magmora::kAmpOn, 0.);
    const bool v010 = r->Load(Craft(2, false)) && r->Plain(magmora::kAmpOn) == 1.;
    Check(saved && loaded && head && v010,
          "amp_on survives save and load; an older chain session and a head-only session load with the head on",
          Say("%d %d %d %d", saved, loaded, head, v010));
  }
  {
    // Factory presets as iPlug2 restores them (a preset chunk: "SCPI" and the index). A full-chain preset sets its
    // blocks (the cabinet on: the built-in one plays); a head preset leaves the blocks alone; the first
    // preset (Bedrock) sets the blocks as a new instance has them: pedal and EQ off, the cabinet on.
    const auto preset = [](int32_t index) {
      Bytes b(8);
      memcpy(&b[0], "SCPI", 4);
      memcpy(&b[4], &index, 4);
      return b;
    };
    auto p = Make(gBundle);
    p->SetPlain(magmora::kCabOn, 0.);
    const bool a = p->Load(preset(16));  // Cinder Push
    const bool full = p->Plain(magmora::kPedalOn) == 1. && p->Plain(magmora::kEqOn) == 1. && p->Plain(magmora::kCabOn) == 1. &&
                      std::fabs(p->Plain(magmora::kPedalDrive) - 1.5) < 1e-3;
    const bool b = p->Load(preset(1));  // Basalt Wall: the head only
    const bool kept = p->Plain(magmora::kPedalOn) == 1. && p->Plain(magmora::kCabOn) == 1. &&
                      std::fabs(p->Plain(magmora::kPreLead) - 4.2) < 1e-3;
    p->SetPlain(magmora::kAmpOn, 0.);
    const bool c = p->Load(preset(0));
    const bool cleared = p->Plain(magmora::kPedalOn) == 0. && p->Plain(magmora::kEqOn) == 0. && p->Plain(magmora::kCabOn) == 1. &&
                         p->Plain(magmora::kAmpOn) == 1.;  // every preset plays the head
    Check(a && b && c && full && kept && cleared,
          "presets: full-chain sets its blocks, head-only keeps them, Bedrock sets them as a new instance has them",
          Say("%d%d%d %d%d%d", a, b, c, full, kept, cleared));
  }
  {
    // An XP7 to XP10 session with the series response file on and the cabinet off ("cabp" version 1, the file in
    // "uir "): it opens as the cabinet with slot 2 playing that file, sounds as a session made that way now,
    // and saves as one.
    const double level = -4.5;
    auto old = Make(gBundle);
    old->SetPlain(magmora::kCabOn, 0.);  // as sessions from before the built-in cabinet had it
    Format f;
    f.block = 256;
    Bytes s;
    old->Save(s);
    s = ReplaceSection(s, "cabp", PackPayloadV1(gFixtures + "/pack"), 1);
    Bytes uir = IrPayload(gFixtures + "/room.wav");
    memcpy(&uir[uir.size() - 4 - 4 - (gFixtures + "/room.wav").size() - 8], &level, 8);  // user_ir_level
    s = ReplaceSection(s, "uir ", uir);
    auto migrated = Make(gBundle);
    migrated->Setup(f);
    const bool loaded = migrated->Load(s);
    const bool params = migrated->Plain(magmora::kCabOn) == 1. && migrated->Plain(magmora::kMic1On) == 0. &&
                        migrated->Plain(magmora::kMic2On) == 1. && migrated->Plain(magmora::kMic2Source) == 1. &&
                        std::fabs(migrated->Plain(magmora::kMic2Level) - (level + 3.0103)) < 1e-3 &&
                        migrated->Plain(magmora::kUserIrOn) == 0.;
    Bytes again;
    migrated->Save(again);
    Check(loaded && params && Contains(again, gFixtures + "/room.wav"),
          "an XP7 session with a series response file opens with it as slot 2's source",
          Say("slot 2 level %+.2f dB (old %+.1f)", migrated->Plain(magmora::kMic2Level), level));
    auto now = Make(gBundle);
    now->Setup(f);
    Bytes fresh;
    now->Save(fresh);
    now->Load(WithPack(fresh, gFixtures + "/pack", "", gFixtures + "/room.wav"));
    now->SetPlain(magmora::kCabOn, 1.);
    now->SetPlain(magmora::kMic1On, 0.);
    now->SetPlain(magmora::kMic2On, 1.);
    now->SetPlain(magmora::kMic2Source, 1.);
    now->SetPlain(magmora::kMic2Level, migrated->Plain(magmora::kMic2Level));
    for (auto* p : {migrated.get(), now.get()}) {
      Settle(*p, 0.3);
      p->Reset();
      Settle(*p, 0.3);
    }
    auto head = Make(gBundle);  // no cabinet: the file must be heard
    head->SetPlain(magmora::kCabOn, 0.);
    head->Setup(f);
    Settle(*head, 0.3);
    head->Reset();
    Settle(*head, 0.3);
    const Audio x = Pluck(24000, 48000.);
    const auto mo = migrated->Run({x});
    const double d = Db(Difference(mo[0], now->Run({x})[0])), heard = Db(Difference(mo[0], head->Run({x})[0]));
    Check(d < -100. && heard > -40., "the migrated session sounds as one made with the file in slot 2",
          Say("%.1f dBFS; against the head alone %.1f dBFS", d, heard));
  }
  {
    // Unknown sections, as a later version writes them, are kept and written back.
    auto p = Make(gBundle);
    Bytes s;
    p->Save(s);
    const Bytes future = {'f', 'r', 'o', 'm', ' ', 'l', 'a', 't', 'e', 'r'};
    s = ReplaceSection(s, "zzzz", future, 7);
    auto q = Make(gBundle);
    Bytes back;
    Check(q->Load(s) && q->Save(back) && Contains(back, "zzzz") && Contains(back, "from later"),
          "an unknown section survives load and save");
  }
  {
    // A new instance: the cabinet on, slot 1 a microphone of the built-in modelled pack. The session saves
    // the built-in pack by its reference, a restored session plays it again, and it is clearly heard.
    Format f;
    f.block = 256;
    auto fresh = Make(gBundle), head = Make(gBundle);
    head->SetPlain(magmora::kCabOn, 0.);
    Bytes s;
    fresh->Save(s);
    auto restored = Make(gBundle);
    const bool loaded = restored->Load(s);
    for (auto* p : {fresh.get(), restored.get(), head.get()}) {
      p->Setup(f);
      Settle(*p, 0.3);
      p->Reset();
      Settle(*p, 0.3);
    }
    const bool defaults = fresh->Plain(magmora::kCabOn) == 1. && fresh->Plain(magmora::kMic1On) == 1. &&
                          fresh->Plain(magmora::kMic1Source) == 0. && fresh->Plain(magmora::kMic2On) == 0.;
    const Audio x = Pluck(24000, 48000.);
    const auto a = fresh->Run({x});
    const double same = Db(Difference(a[0], restored->Run({x})[0])), heard = Db(Difference(a[0], head->Run({x})[0]));
    Check(loaded && defaults && Contains(s, "builtin:trad-4x12-modelled") && same < -120. && heard > -40.,
          "a new instance plays the built-in cabinet, and a session keeps it",
          Say("restored %.1f dBFS, against the head alone %.1f dBFS", same, heard));
  }
  {
    // The pack and the response file are references: restored, loaded, saved again; a missing one is kept.
    auto p = Make(gBundle);
    Format f;
    f.block = 256;
    p->Setup(f);
    ChainOn(*p);
    Settle(*p, 0.3);
    Bytes saved;
    p->Save(saved);
    Check(Contains(saved, gFixtures + "/pack") && Contains(saved, gFixtures + "/room.wav"),
          "pack folder and response file are saved as references");
    const Bytes missing = WithPack(saved, "/no/such/pack", "/no/such/one.wav", "/no/such/file.wav");
    auto q = Make(gBundle);
    Bytes back;
    Check(q->Load(missing) && q->Save(back) && Contains(back, "/no/such/pack") && Contains(back, "/no/such/one.wav") &&
              Contains(back, "/no/such/file.wav"),
          "a missing pack or slot file is reported and its reference kept");
    // The chain's sound is the same after a round trip through the state.
    auto r = Make(gBundle);
    r->Setup(f);
    r->Load(saved);
    Settle(*r, 0.3);
    p->Reset();
    r->Reset();
    Settle(*p, 0.3);
    Settle(*r, 0.3);
    auto twin = Make(gBundle);  // set the same way, without a state in between
    twin->Setup(f);
    ChainOn(*twin);
    Settle(*twin, 0.3);
    twin->Reset();
    Settle(*twin, 0.3);
    const Audio x = Pluck(24000, 48000.);
    const auto a = p->Run({x}), b = r->Run({x}), c = twin->Run({x});
    const double d = Db(Difference(a[0], b[0])), base = Db(Difference(a[0], c[0]));
    printf("      twin %.1f, restored %.1f dBFS\n", base, d);
    Check(d < -120., "a session with every block on renders the same after save and load", Say("%.1f dBFS", d));
  }
  for (int32_t version : {1, 2, 3}) {
    auto p = Make(gBundle);
    p->Set(magmora::kMid, 0.9);
    const bool ok = p->Load(Craft(version, version > 2));
    Check(ok && std::fabs(p->Plain(magmora::kPreLead) - 7.25) < 1e-4 && std::fabs(p->Plain(magmora::kMid) - 5.) < 1e-4 &&
              std::fabs(p->Plain(magmora::kLow) - 10.) < 1e-4,
          Say("version %d: unknown keys skipped, missing ones default, values clamped", version));
  }
  {
    // The value set: saved by name with its model hash; version 1 states play "prototype".
    const auto contains = [](const Bytes& b, const char* text) {
      return std::search(b.begin(), b.end(), text, text + strlen(text)) != b.end();
    };
    const auto saved = [&](const Bytes& state) {
      auto p = Make(gBundle);
      Bytes again;
      return p->Load(state) && p->Save(again) ? again : Bytes();
    };
    const Bytes v1 = saved(Craft(1, false)), drawn = saved(Craft(2, false, "as-drawn"));
    const Bytes stale = saved(Craft(2, false, "as-drawn", "0123456789abcdef")), unknown = saved(Craft(2, false, "no-such-set"));
    Check(contains(v1, "prototype") && !contains(v1, "as-drawn"), "a version 1 state plays the prototype values");
    Check(contains(drawn, "as-drawn"), "the value set is restored by name");
    Check(contains(stale, "as-drawn"), "a different model hash keeps the chosen set");
    Check(contains(unknown, "prototype"), "an unknown set falls back to the default");
    Format f;
    f.block = 256;
    auto a = Make(gBundle), b = Make(gBundle);
    a->Load(Craft(2, false, "prototype"));
    b->Load(Craft(2, false, "as-drawn"));
    a->Setup(f);
    b->Setup(f);
    const Audio x = Pluck(24000, 48000.);
    const double apart = Db(Difference(a->Run({x})[0], b->Run({x})[0]));
    Check(apart > -60., "the two value sets sound different", Say("difference peak %.1f dBFS", apart));
  }
  {
    auto p = Make(gBundle);
    p->SetPlain(magmora::kMid, 8.5);
    Format f;
    p->Setup(f);
    p->Run({Audio(64)});
    const Bytes good = Craft(1, false);
    int taken = 0, changed = 0;
    for (size_t n = 0; n + 4 < good.size(); ++n) {  // every truncation inside the chunk
      taken += p->Load(Bytes(good.begin(), good.begin() + n));
      changed += std::fabs(p->Plain(magmora::kMid) - 8.5) > 1e-4;
    }
    Check(!taken && !changed, "truncated state is refused and changes nothing", Say("%d accepted, %d changed", taken, changed));
    int survived = 0;
    for (int i = 0; i < 2000; ++i) {
      Bytes noisy = good;
      for (int k = 0; k < 1 + i % 8; ++k) noisy[random() % noisy.size()] = static_cast<char>(random());
      p->Load(noisy);
      survived += std::isfinite(p->Get(magmora::kMid));
    }
    Check(survived == 2000, "2000 corrupted states survived");
  }
  {
    auto a = Make(gBundle);
    Window* w = new Window(*a, 1.);
    w->Click(1180., 19.);
    w->Click(1140., 52. + 25. * 3 + 12.);  // 125 %
    Pump(0.1);
    Bytes s;
    a->Save(s);
    delete w;
    auto b = Make(gBundle);
    b->Load(s);
    Window other(*b, 1.);
    Check(std::fabs(other.Scale() - 1.25) < 0.002, "the window size is part of the state", Say("%.3f", other.Scale()));
  }

  // The user preset in use ("upre": its name and file): kept through save and load, dropped when malformed, cleared by
  // a factory preset; the file need not exist (the name then shows as edited).
  {
    auto a = Make(gBundle);
    a->Setup(Format{});
    Bytes s;
    a->Save(s);
    Check(!Contains(s, "upre"), "a session without a user preset has no user preset section");
    Bytes named;
    PutStr(named, "Wall Of Sound");
    PutStr(named, "Live/Wall Of Sound.json");
    const Bytes with = ReplaceSection(s, "upre", named);
    auto b = Make(gBundle);
    b->Setup(Format{});
    Check(b->Load(with), "a state with a user preset loads");
    Bytes again;
    b->Save(again);
    Check(UserOf(again) == std::make_pair(std::string("Wall Of Sound"), std::string("Live/Wall Of Sound.json")),
          "the user preset is saved back, its file missing or not", UserOf(again).first);
    size_t count = 0;
    const char tag[] = "upre";
    for (auto it = again.begin(); (it = std::search(it, again.end(), tag, tag + 4)) != again.end(); ++it) ++count;
    Check(count == 1, "and only once", Say("%zu", count));
    Bytes bad;
    const int32_t huge = 1 << 30;
    bad.insert(bad.end(), reinterpret_cast<const char*>(&huge), reinterpret_cast<const char*>(&huge) + 4);
    auto c = Make(gBundle);
    c->Setup(Format{});
    Check(c->Load(ReplaceSection(s, "upre", bad)), "a malformed user preset section does not refuse the session");
    c->Save(again);
    Check(UserOf(again).first.empty(), "and is dropped");
    Bytes program;
    const uint32_t magic = 0x49504353;  // "SCPI": a factory preset by index
    const int32_t index = 2;
    program.insert(program.end(), reinterpret_cast<const char*>(&magic), reinterpret_cast<const char*>(&magic) + 4);
    program.insert(program.end(), reinterpret_cast<const char*>(&index), reinterpret_cast<const char*>(&index) + 4);
    b->Load(program);
    b->Save(again);
    Check(UserOf(again).first.empty(), "a factory preset from the host replaces the user preset");
  }
}

// ---- automation -------------------------------------------------------------------------------------------------

// Strongest component above `hz` in any 10 ms frame, relative to a full-scale sine.
double AboveDb(const Audio& a, int from, int to, double rate, double hz) {
  const int n = 4096, frame = static_cast<int>(rate / 100.);
  static FFTSetupD setup = vDSP_create_fftsetupD(12, kFFTRadix2);
  std::vector<double> re(n), im(n), window(frame);
  double gain = 0.;
  for (int i = 0; i < frame; ++i) {
    const double t = 2. * M_PI * i / (frame - 1);
    window[i] = 0.35875 - 0.48829 * std::cos(t) + 0.14128 * std::cos(2 * t) - 0.01168 * std::cos(3 * t);
    gain += window[i];
  }
  double worst = 0.;
  for (int at = from; at + frame <= to; at += frame / 4) {
    std::fill(re.begin(), re.end(), 0.);
    std::fill(im.begin(), im.end(), 0.);
    for (int i = 0; i < frame; ++i) re[i] = a[at + i] * window[i];
    DSPDoubleSplitComplex z{re.data(), im.data()};
    vDSP_fft_zipD(setup, &z, 1, 12, FFT_FORWARD);
    for (int k = static_cast<int>(hz / rate * n); k < n / 2; ++k) worst = std::max(worst, 2. * std::hypot(re[k], im[k]) / gain);
  }
  return Db(worst);
}

void TestAutomation() {
  {
    auto p = Make(gBundle);
    Format f;
    p->Setup(f);
    int flags = 0;
    for (int id = 0; id < kAllParams; ++id) flags += p->Automatable(id) == PI(id).automatable;
    Check(flags == kAllParams, "automatable flags match Params.h and ChainParams.h", Say("%d of %d", flags, kAllParams));
    if (auto* v3 = dynamic_cast<V3*>(p.get())) {
      int hidden = 0, shouldBe = 0;
      for (int id = 0; id < kAllParams; ++id) {
        shouldBe += magmora::Chain::parameterHidden(id);
        hidden += magmora::Chain::parameterHidden(id) && v3->Hidden(id) && !v3->Automatable(id);
      }
      Check(hidden == shouldBe && shouldBe == 3, "cab_floor, user_ir_on and user_ir_level are hidden from the host",
            Say("%d of %d", hidden, shouldBe));
    }
    const std::string texts[] = {p->Text(magmora::kLow, 0.534), p->Text(magmora::kGateThreshold, 0.), p->Text(magmora::kOutputLevel, 0.75),
                                 p->Text(magmora::kChannel, 1.), p->Text(magmora::kQuality, 1.), p->Text(magmora::kStandby, 0.)};
    Check(texts[0].rfind("5.3", 0) == 0 && texts[1].find("off") != std::string::npos && texts[2].rfind("0.0", 0) == 0 &&
              texts[3] == "Lead" && texts[4] == "Ultra" && texts[5] == "Standby",
          "host-visible text", texts[0] + " | " + texts[1] + " | " + texts[2] + " | " + texts[3] + " | " + texts[4] + " | " + texts[5]);
  }
  for (const bool onlyInProcess : {false, true}) {
    auto moved = Make(gBundle), still = Make(gBundle);
    Format f;
    f.block = 64;
    moved->Setup(f);
    still->Setup(f);
    const Audio x = Pluck(24000, 48000.);
    const auto reference = still->Run({x});
    const auto out = moved->Run({x}, 0, false, [&](int at) { moved->Set(magmora::kOutputLevel, at < 6400 ? 0.75 : 0.25, onlyInProcess); });
    const double ratio = Db(Peak(out[0], 12000)) - Db(Peak(reference[0], 12000));
    Check(std::fabs(ratio + 24.) < 0.5 && Db(Difference(Audio(out[0].begin(), out[0].begin() + 6400),
                                                        Audio(reference[0].begin(), reference[0].begin() + 6400))) < -120.,
          onlyInProcess ? "automation through process alone reaches the engine" : "host automation reaches the engine",
          Say("%.2f dB for a step of -24 dB", ratio));
  }
  {
    // Every parameter of the blocks, automated through the processing call alone with its block on: the output stays
    // finite, the value is the one saved, and the sound changes unless the parameter has no audible effect by design.
    const std::map<int, const char*> silent = {
        {magmora::kEqPosition, "no effect while every block after the head is linear"},
        {magmora::kMicAlign, "nothing to align: the fixture pack is declared aligned"},
        {magmora::kMic1Angle, "the fixture pack has one angle, so the angle snaps to it"},
        {magmora::kMic2Angle, "the fixture pack has one angle, so the angle snaps to it"},
        {magmora::kMic1Distance, "the fixture pack has one grille distance"},
        {magmora::kMic2Distance, "the fixture pack has one grille distance"},
        {magmora::kFxOn, "the FX slot is reserved and empty in this release"}};
    int moved = 0, inert = 0, bad = 0, total = 0;
    std::string notMoved, inertNames;
    for (int id = magmora::kNumParams; id < kAllParams; ++id) {
      if (!PI(id).automatable) continue;
      ++total;
      auto still = Make(gBundle), moving = Make(gBundle);
      Format f;
      f.block = 128;
      for (auto* p : {still.get(), moving.get()}) {
        p->Setup(f);
        ChainOn(*p);
        p->SetPlain(magmora::kMic2Source, 0.);  // both slots on pack microphones, so that every control is heard
        p->SetPlain(magmora::kEqHighPassSlope, 1.);
        p->SetPlain(magmora::kEqLowPassSlope, 1.);
        p->SetPlain(magmora::kEqBand1Gain, 4.);
        p->SetPlain(magmora::kEqBand3Gain, -4.);
        p->SetPlain(magmora::kEqBand4Gain, 4.);
        // Transpose from a non-zero value: going in or out of the path (crossing 0) is structural, applied on the
        // idle timer as a change of quality is (TestTransposeAudio); between non-zero values it is automation.
        if (id == magmora::kTranspose) p->SetPlain(magmora::kTranspose, 2.);
        Settle(*p, 0.25);
      }
      const double from = moving->Get(id), to = from < 0.5 ? 0.85 : 0.15;
      const Audio x = Pluck(48000, 48000.);
      const auto a = still->Run({x});
      const auto b = moving->Run({x}, 0, false, [&](int at) {
        const double t = std::min(1., at / 9600.);
        moving->Set(id, from + (to - from) * t, true);
      });
      Pump(0.1);
      const double change = Db(Difference(a[0], b[0], 24000));
      const bool finite = Finite(b[0]) && Finite(b[1]) && Peak(b[0]) < 4.01;
      Bytes saved;
      moving->Save(saved);
      auto again = Make(gBundle);
      again->Load(saved);
      // The processor's value, which only the state can show (a VST3 controller hears of process-only changes from
      // the host, not from the plug-in).
      double wanted = moving->ToPlain(id, to);
      if (PI(id).taper == magmora::Taper::Stepped) wanted = std::round(wanted);
      const bool kept = std::fabs(again->Plain(id) - wanted) < 1e-3 * (PI(id).max - PI(id).min);
      bad += !finite || !kept;
      if (change > -90.) {
        ++moved;
      } else if (silent.count(id)) {
        ++inert;
        inertNames += std::string(" ") + PI(id).key;
      } else {
        notMoved += std::string(PI(id).key) + " ";
      }
    }
    Check(!bad && moved + inert == total, "every new parameter automated through process: finite, saved, audible",
          Say("%d parameters: %d change the sound, %d have no effect by design (%s), %d broken%s%s", total, moved, inert,
              inertNames.c_str() + 1, bad, notMoved.empty() ? "" : "; silent: ", notMoved.c_str()));
  }
  {
    const double rate = 48000.;
    for (const magmora::ParamInfo& info : magmora::kParams) {
      if (!info.smoothed) continue;
      auto p = Make(gBundle);
      Format f;
      f.block = 64;
      p->SetPlain(magmora::kChannel, 0.);
      p->SetPlain(magmora::kPreRhythm, 2.);
      p->Setup(f);
      p->Run({Audio(4800)});
      const Audio x = Sine(48000, rate, 220., 0.005);
      const int sweep = static_cast<int>(0.05 * rate);
      const double top = info.id == magmora::kGateThreshold ? 0.25 : 1.;
      const double home = info.id == magmora::kPreRhythm ? 0.2 : p->Get(info.id);
      const auto out = p->Run({x}, 0, false, [&](int at) {
        if (at < 19200) return;
        const double t = std::min(1., double(at - 19200) / sweep);
        p->Set(info.id, home + (top - home) * t, true);
      });
      const double tone = Db(Peak(out[0], 9600, 19200));
      const double moving = AboveDb(out[0], 19200, 19200 + sweep + 4800, rate, 12000.) - tone;
      const double steady = std::max(AboveDb(out[0], 9600, 19200, rate, 12000.), AboveDb(out[0], 38400, 48000, rate, 12000.)) - tone;
      Check(moving < std::max(-80., steady + 3.), Say("%s swept in 50 ms: no zipper above 12 kHz", info.key),
            Say("%.1f dB re tone (%.1f held)", moving, steady));
    }
    for (int id : {int(magmora::kInputJack), int(magmora::kChannel), int(magmora::kBright), int(magmora::kCrunch), int(magmora::kStandby), int(magmora::kPower),
                   int(magmora::kAmpOn)}) {  // the head's own on/off too
      auto p = Make(gBundle);
      Format f;
      f.block = 64;
      p->SetPlain(magmora::kChannel, 0.);
      p->Setup(f);
      p->Run({Audio(24000)});
      const double was = p->Get(id);
      const auto out = p->Run({Audio(24000)}, 0, false, [&](int at) {
        if (at == 64 * 100) p->Set(id, 1. - was, true);
      });
      const double click = Db(Peak(out[0], 6400 - 480, 6400 + 960));
      Check(click < -60., Say("%s switches without a click", PI(id).key), Say("%.1f dBFS", click));
    }
  }
}


// ---- the transposer (added 2026-11-24) ----------------------------------------------------------------------------

// The transposer's delay per rate and mode (dsp/src/transpose/Transposer.h).
int TransposeDelay(double rate, int mode) {
  if (rate == 44100.) return mode ? 472 : 234;
  if (rate == 96000.) return mode ? 1024 : 513;
  return mode ? 512 : 255;
}

// Plays x through p block by block, letting the main thread run between blocks (as a host's idle timer does), with
// `before(at)` before each block.
Audio PlayPumping(Plug& p, const Audio& x, const std::function<void(int)>& before) {
  Audio out(x.size()), right(x.size());
  const int size = p.format.block;
  for (int at = 0; at + size <= static_cast<int>(x.size()); at += size) {
    if (before) before(at);
    const float* in[2] = {x.data() + at, x.data() + at};
    float* o[2] = {out.data() + at, right.data() + at};
    p.Render(in, o, size);
    Pump(0.001);
  }
  return out;
}

// The head and the cabinet off: the input at unity, delayed, so the transposer's switching is read where it happens.
void HeadOff(Plug& p) {
  p.SetPlain(magmora::kAmpOn, 0.);
  p.SetPlain(magmora::kCabOn, 0.);
}

void TestTransposeAudio() {
  {
    auto p = Make(gBundle);
    Format f;
    p->Setup(f);
    Check(p->Automatable(magmora::kTranspose) && !p->Automatable(magmora::kTransposeMode),
          "transpose is automatable, its mode is not (a structural setting)");
    const std::string text = p->Text(magmora::kTranspose, p->ToNormalised(magmora::kTranspose, -3.));
    Check(text.find("-3") != std::string::npos, "transpose shows whole semitones", text);
    Check(p->Plain(magmora::kTranspose) == 0. && p->Plain(magmora::kTransposeMode) == 0., "a new instance: 0 st, Tight");
  }
  // The latency: the head's, plus the mode's delay while the transposer is in; never changed between non-zero values.
  for (double rate : {44100., 48000., 96000.}) {
    auto p = Make(gBundle);
    Format f;
    f.rate = rate;
    f.block = 128;
    p->Setup(f);
    Settle(*p, 0.1);
    const int head = p->Latency(), notices = p->latencyNotices;
    int seen[6];
    const double steps[][2] = {{3., 0.}, {-7., 0.}, {-7., 1.}, {12., 1.}, {0., 1.}, {0., 0.}};
    for (int k = 0; k < 6; ++k) {
      p->SetPlain(magmora::kTranspose, steps[k][0]);
      p->SetPlain(magmora::kTransposeMode, steps[k][1]);
      Settle(*p, 0.15);
      seen[k] = p->Latency();
    }
    const int tight = TransposeDelay(rate, 0), standard = TransposeDelay(rate, 1);
    Check(seen[0] == head + tight && seen[1] == head + tight && seen[2] == head + standard && seen[3] == head + standard &&
              seen[4] == head && seen[5] == head && p->latencyNotices - notices == 3,  // in, mode, out
          Say("transpose latency at %g Hz: head + Tight / Standard, the head's alone at 0 st, announced", rate),
          Say("head %d: %d %d %d %d %d %d, %d notices", head, seen[0], seen[1], seen[2], seen[3], seen[4], seen[5],
              p->latencyNotices - notices));
  }
  // The bypass stays aligned with the reported latency while the transposer is in.
  {
    auto p = Make(gBundle);
    Format f;
    f.block = 64;
    p->Setup(f);
    p->SetPlain(magmora::kTranspose, 5.);
    p->SetPlain(magmora::kTransposeMode, 1.);
    Settle(*p, 0.15);
    const int latency = p->Latency();
    p->Set(kBypassId, 1.);
    p->Run({Audio(6400)});
    Audio impulse(3200);
    impulse[100] = 0.5f;
    const auto out = p->Run({impulse});
    int at = -1, others = 0;
    for (int i = 0; i < 3200; ++i) {
      if (out[1][i] == 0.5f && at < 0) at = i;
      else if (out[1][i] != 0.f) ++others;
    }
    Check(at - 100 == latency && !others && latency == 49 + 512, "bypass with the transposer in (Standard): latency-aligned",
          Say("reported %d, measured %d", latency, at - 100));
  }
  // Crossing 0 and back, and a change of mode, while a tone plays: no edge above 4 kHz (G-E2: 80 dB under the tone).
  {
    auto p = Make(gBundle);
    Format f;
    f.block = 64;
    p->Setup(f);
    HeadOff(*p);
    Settle(*p, 0.2);
    const int n = 48000 * 2;
    const Audio x = Sine(n, 48000., 220., 0.1);
    const int head = p->Latency();
    int during[3] = {};
    const Audio out = PlayPumping(*p, x, [&](int at) {
      if (at == 64 * 150) p->SetPlain(magmora::kTranspose, 4.);             // in
      if (at == 64 * 600) p->SetPlain(magmora::kTransposeMode, 1.);         // mode
      if (at == 64 * 1050) p->SetPlain(magmora::kTranspose, 0.);            // out
      if (at == 64 * 500) during[0] = p->Latency();
      if (at == 64 * 950) during[1] = p->Latency();
      if (at == 64 * 1450) during[2] = p->Latency();
    });
    const double tone = Db(0.1);
    const double in = AboveDb(out, 64 * 100, 64 * 550, 48000., 4000.) - tone;
    const double mode = AboveDb(out, 64 * 550, 64 * 1000, 48000., 4000.) - tone;
    const double gone = AboveDb(out, 64 * 1000, 64 * 1490, 48000., 4000.) - tone;
    Check(std::max({in, mode, gone}) < -80., "transpose in, mode changed, out again while playing: no edge above 4 kHz",
          Say("in %.1f, mode %.1f, out %.1f dB re the tone (gate -80)", in, mode, gone));
    Check(during[0] == head + 255 && during[1] == head + 512 && during[2] == head, "and the latency followed",
          Say("%d, %d, %d (head %d)", during[0], during[1], during[2], head));
  }
}

// What a shift held still leaves above 4 kHz on the same tone (dB re the tone). A pure tone shifted up in Tight leaves
// -73 to -80 dB there by itself (dsp/tests/test_transpose.cpp, "a held shift"): automation is an edge only if it
// exceeds what the shifts it visits leave when held.
double HeldAboveDb(int mode, double semitones) {
  auto p = Make(gBundle);
  Format f;
  f.block = 64;
  p->Setup(f);
  HeadOff(*p);
  p->SetPlain(magmora::kTranspose, semitones);
  p->SetPlain(magmora::kTransposeMode, mode);
  Settle(*p, 0.2);
  const Audio out = PlayPumping(*p, Sine(48000, 48000., 220., 0.1), nullptr);
  return AboveDb(out, 64 * 100, 48000 - 64, 48000., 4000.) - Db(0.1);
}

void TestTransposeAutomation() {
  // Automated between non-zero values in the processing call alone, as hosts send it: no edge (G-E2's -80 dB, or
  // within 1 dB of the worst shift held), no latency change.
  for (int mode = 0; mode < 2; ++mode) {
    auto p = Make(gBundle);
    Format f;
    f.block = 64;
    p->Setup(f);
    HeadOff(*p);
    p->SetPlain(magmora::kTranspose, 2.);
    p->SetPlain(magmora::kTransposeMode, mode);
    Settle(*p, 0.2);
    const int latency = p->Latency(), notices = p->latencyNotices;
    const Audio x = Sine(48000 * 2, 48000., 220., 0.1);
    const double values[] = {5., -3., 7., -12., 12., 1., -1., 3.};
    bool steady = true;
    const Audio out = PlayPumping(*p, x, [&](int at) {
      if (at >= 64 * 150 && at % (64 * 150) == 0 && at / (64 * 150) - 1 < 8)
        p->SetPlain(magmora::kTranspose, values[at / (64 * 150) - 1], true);
      steady = steady && p->Latency() == latency;
    });
    const double worst = AboveDb(out, 64 * 100, 48000 * 2 - 64, 48000., 4000.) - Db(0.1);
    double held = HeldAboveDb(mode, 2.);
    for (double v : values) held = std::max(held, HeldAboveDb(mode, v));
    const double limit = std::max(-80., held + 1.);
    Bytes saved;  // the processor's value: process-only automation reaches the controller only through the state
    p->Save(saved);
    auto again = Make(gBundle);
    again->Load(saved);
    Check(worst < limit && steady && p->latencyNotices == notices && std::fabs(again->Plain(magmora::kTranspose) - 3.) < 1e-6,
          Say("transpose automated through process (%s): no edge, the latency never changes", mode ? "Standard" : "Tight"),
          Say("%.1f dB re the tone above 4 kHz (worst held shift %.1f, limit %.1f), latency %d, %d notices", worst, held, limit,
              latency, p->latencyNotices - notices));
  }
}

void TestTransposeState() {
  // Saved with the session and restored, latency included; an older session loads at 0 st, Tight; a factory preset
  // leaves it alone (a session setting, as the input trim).
  auto a = Make(gBundle), b = Make(gBundle);
  Format f;
  f.block = 128;
  a->Setup(f);
  b->Setup(f);
  a->SetPlain(magmora::kTranspose, -4.);
  a->SetPlain(magmora::kTransposeMode, 1.);
  Settle(*a, 0.15);
  Bytes s;
  a->Save(s);
  b->Load(s);
  Settle(*b, 0.15);
  Check(std::fabs(b->Plain(magmora::kTranspose) + 4.) < 1e-4 && std::fabs(b->Plain(magmora::kTransposeMode) - 1.) < 1e-4 && b->Latency() == a->Latency() &&
            a->Latency() == 49 + 512,
        "transpose and its mode round-trip, and the restored session reports their latency",
        Say("%.0f st, mode %.0f, latency %d", b->Plain(magmora::kTranspose), b->Plain(magmora::kTransposeMode), b->Latency()));
  b->Load(Craft(1, false, nullptr, nullptr));  // a session of 0.1.0 (the head alone)
  Settle(*b, 0.15);
  const bool oldOk = b->Plain(magmora::kTranspose) == 0. && b->Plain(magmora::kTransposeMode) == 0.;
  Check(oldOk && b->Latency() == 49, "a session from before the transposer loads at 0 st, Tight, with the head's latency",
        Say("%.0f st, latency %d", b->Plain(magmora::kTranspose), b->Latency()));
}

// ---- the panel --------------------------------------------------------------------------------------------------

struct Spot {
  double x = 0., y = 0.;
};
std::map<std::string, Spot> gSpots;

void ReadLayout() {
  NSString* dir = [@(gBundle.c_str()) stringByAppendingPathComponent:@"Contents/Resources/layout.json"];
  NSDictionary* d = [NSJSONSerialization JSONObjectWithData:[NSData dataWithContentsOfFile:dir] options:0 error:nil];
  if (!d) Die("no layout.json in the bundle");
  for (NSDictionary* c in d[@"controls"]) gSpots[[c[@"id"] UTF8String]] = {[c[@"x"] doubleValue], [c[@"y"] doubleValue]};
}

// Balanced, never nested, one parameter per gesture.
bool Gestures(const std::vector<Event>& events, int id, int expected) {
  int open = 0, count = 0;
  for (const Event& e : events) {
    if (e.id != id) return false;
    if (e.what == 'b' && open++) return false;
    if (e.what == 'v' && !open) return false;
    if (e.what == 'e') {
      if (!open) return false;
      --open;
      ++count;
    }
  }
  return !open && count == expected;
}

std::vector<Event> Only(const std::vector<Event>& events, int id) {
  std::vector<Event> mine;
  for (const Event& e : events)
    if (e.id == id) mine.push_back(e);
  return mine;
}

void TestPages();
void TestBarAndPages();
void TestIrSteps();
void TestTuner();
void TestUserPresets();

void TestUi() {
  ReadLayout();
  auto p = Make(gBundle);
  Format f;
  p->Setup(f);
  Window w(*p, 1.);
  const auto tick = [&](double seconds = 0.08) { Settle(*p, seconds); };
  const auto fresh = [&] {
    tick();
    p->events.clear();
  };
  const Spot low = gSpots["low"], mid = gSpots["mid"];
  int id = magmora::kLow;

  fresh();
  w.DragUp(low.x, low.y, 50.);
  tick();
  Check(std::fabs(p->Plain(id) - 7.5) < 0.02, "drag: 200 pt is the full travel", Say("%.3f after +50 pt from 5", p->Plain(id)));
  Check(Gestures(p->events, id, 1), "drag is one gesture", Say("%d events", int(p->events.size())));

  fresh();
  w.DragUp(low.x, low.y, -100., kShift);
  tick();
  Check(std::fabs(p->Plain(id) - 7.0) < 0.02, "Shift-drag is ten times finer", Say("%.3f", p->Plain(id)));

  fresh();
  w.DragUp(low.x, low.y, 900.);
  tick();
  Check(p->Plain(id) > 9.999, "drag stops at 10");

  fresh();
  w.DoubleClick(low.x, low.y);
  tick();
  Check(std::fabs(p->Plain(id) - 5.) < 1e-4, "double click returns to the default");
  Check(Gestures(p->events, id, 2), "double click: balanced gestures");

  p->SetPlain(id, 1.2);
  fresh();
  w.Click(low.x, low.y, kOption);
  tick();
  Check(std::fabs(p->Plain(id) - 5.) < 1e-4 && Gestures(p->events, id, 1), "Alt-click returns to the default, one gesture");

  fresh();
  for (int i = 0; i < 5; ++i) {
    w.Wheel(low.x, low.y, 1.);
    Pump(0.03);
  }
  tick(0.8);
  Check(std::fabs(p->Plain(id) - 5.5) < 0.01, "wheel moves in small steps", Say("%.3f", p->Plain(id)));
  Check(Gestures(p->events, id, 1), "a turn of the wheel is one gesture");

  const struct {
    const char* text;
    double value;
  } typed[] = {{"7.3", 7.3}, {"2,5", 2.5}, {"", 2.5}, {"abc", 2.5}, {"99", 10.}, {"-4", 0.}, {" 6.25 ", 6.25}};
  for (const auto& t : typed) {
    fresh();
    w.RightClick(low.x, low.y);
    const bool field = w.Type(t.text);
    tick();
    Check(field && std::fabs(p->Plain(id) - t.value) < 1e-4, Say("typed \"%s\"", t.text), Say("%.3f", p->Plain(id)));
  }
  Check(w.Field() == nil, "the text field is gone afterwards");

  // The overlap of neighbouring cells belongs to neither knob's neighbour.
  id = magmora::kMid;
  fresh();
  w.DragUp(mid.x - 34., mid.y, 20.);
  tick();
  Check(p->Plain(id) > 5.5, "mid's mouse target reaches its edge");

  p->SetPlain(magmora::kMid, 2.);
  tick();
  fresh();
  w.DragUp(mid.x, mid.y, 20., kNone, 10, false);
  Pump(0.05);
  p->Set(magmora::kMid, 0.95);  // automation against the hand
  Pump(0.05);
  w.HoldDrag(mid.x, mid.y, 20., kNone, 5);
  w.Up(mid.x, mid.y);
  tick();
  Check(p->Plain(id) > 3.3 && p->Plain(id) < 4.3, "the hand wins over automation while it holds", Say("%.2f", p->Plain(id)));

  const struct {
    const char* key;
    int id;
  } switches[] = {{"bright", magmora::kBright}, {"crunch", magmora::kCrunch}, {"channel", magmora::kChannel},
                  {"input_jack", magmora::kInputJack}, {"standby", magmora::kStandby}, {"power", magmora::kPower}};
  for (const auto& s : switches) {
    const double was = p->Get(s.id);
    fresh();
    w.Click(gSpots[s.key].x, gSpots[s.key].y);
    tick();
    const double once = p->Get(s.id);
    Check(std::fabs(once - (1. - was)) < 1e-6 && Gestures(p->events, s.id, 1), Say("%s switches, one gesture", s.key));
    w.DoubleClick(gSpots[s.key].x, gSpots[s.key].y);
    tick();
    Check(std::fabs(p->Get(s.id) - once) < 1e-6, Say("%s: two quick presses are two presses", s.key));
    w.Click(gSpots[s.key].x, gSpots[s.key].y);
    tick();
  }

  const struct {
    double x;
    int id;
    const char* text;
    double value;
  } fields[] = {{540., magmora::kInputTrim, "-3.5", -3.5}, {790., magmora::kGateThreshold, "-60 dB", -60.},
                {790., magmora::kGateThreshold, "off", -96.}, {910., magmora::kOutputLevel, "+2", 2.},
                {666., magmora::kTranspose, "-5", -5.}, {666., magmora::kTranspose, "+3 st", 3.},
                {666., magmora::kTranspose, "\xE2\x88\x92" "7", -7.}};
  for (const auto& b : fields) {
    fresh();
    w.RightClick(b.x, 19.);
    w.Type(b.text);
    tick();
    Check(std::fabs(p->Plain(b.id) - b.value) < 1e-3 && Gestures(p->events, b.id, 1), Say("bar field typed \"%s\"", b.text),
          Say("%.2f", p->Plain(b.id)));
  }
  fresh();
  w.DragUp(540., 19., 30.);
  tick();
  Check(p->Plain(magmora::kInputTrim) > 0.8 && Gestures(p->events, magmora::kInputTrim, 1), "bar field drags");

  // Transpose (added 2026-11-24): drags, a wheel notch a semitone, double-click and Alt-click to 0; its mode caption
  // switches Tight and Standard with one gesture of its own parameter, and moves nothing else.
  fresh();
  p->SetPlain(magmora::kTranspose, 0.);
  fresh();
  w.DragUp(666., 19., 40.);
  tick();
  Check(p->Plain(magmora::kTranspose) >= 2. && Gestures(p->events, magmora::kTranspose, 1), "the transpose field drags",
        Say("%.0f st", p->Plain(magmora::kTranspose)));
  fresh();
  p->SetPlain(magmora::kTranspose, 0.);
  fresh();
  w.Wheel(666., 19., 1.);
  tick(0.5);
  w.Wheel(666., 19., 1.);
  tick(0.5);
  Check(std::fabs(p->Plain(magmora::kTranspose) - 2.) < 1e-4, "a wheel notch is a semitone", Say("%.0f st", p->Plain(magmora::kTranspose)));
  fresh();
  w.DoubleClick(666., 19.);
  tick();
  // as on the knobs: the first press opens and closes a (still) drag, the double click is the second gesture
  Check(std::fabs(p->Plain(magmora::kTranspose)) < 1e-4 && Gestures(p->events, magmora::kTranspose, 2), "double click returns transpose to 0",
        Say("%.0f st, %zu events", p->Plain(magmora::kTranspose), p->events.size()));
  p->SetPlain(magmora::kTranspose, 5.);
  fresh();
  w.Click(666., 19., kOption);
  tick();
  Check(std::fabs(p->Plain(magmora::kTranspose)) < 1e-4 && Gestures(p->events, magmora::kTranspose, 1), "Alt-click returns transpose to 0");
  fresh();
  const double modeWas = p->Plain(magmora::kTransposeMode);
  w.Click(630., 36.);
  tick();
  Check(std::fabs(p->Plain(magmora::kTransposeMode) - (1. - modeWas)) < 1e-4 && Gestures(p->events, magmora::kTransposeMode, 1),
        "a click on the mode caption switches Tight and Standard, one gesture");
  fresh();
  w.Click(630., 36.);
  tick();
  Check(std::fabs(p->Plain(magmora::kTransposeMode) - modeWas) < 1e-4 && std::fabs(p->Plain(magmora::kTranspose)) < 1e-4, "and back, the semitones untouched");
  p->SetPlain(magmora::kTranspose, -2.);
  p->SetPlain(magmora::kInputTrim, 1.);
  fresh();

  fresh();
  w.Click(50., 19.);  // next preset
  tick();
  bool oneEach = !p->events.empty();
  for (int i = 0; i < kAllParams; ++i) {
    std::vector<Event> mine;
    for (const Event& e : p->events)
      if (e.id == i) mine.push_back(e);
    if (!mine.empty()) oneEach &= Gestures(mine, i, 1);
  }
  Check(oneEach && std::fabs(p->Plain(magmora::kPreLead) - 4.2) < 1e-4, "next preset applies, one gesture per parameter");
  Check(std::fabs(p->Plain(magmora::kInputTrim)) > 0.5, "a preset leaves the input trim alone");
  Check(std::fabs(p->Plain(magmora::kTranspose) + 2.) < 1e-4, "a preset leaves transpose alone", Say("%.0f st", p->Plain(magmora::kTranspose)));
  w.Click(26., 19.);  // and back
  tick();
  Check(std::fabs(p->Plain(magmora::kPreLead) - 5.) < 1e-4, "previous preset");

  w.Click(100., 19.);
  w.Click(150., 52. + 25. * 3 + 12.);  // heading, Bedrock, heading, Basalt Wall
  tick();
  Check(std::fabs(p->Plain(magmora::kPreLead) - 4.2) < 1e-4, "preset menu picks");

  fresh();
  w.Click(1049., 19.);
  w.Click(1000., 52. + 25. * 2 + 12.);  // High
  tick(0.4);
  Check(p->Plain(magmora::kQuality) == 1., "engine menu picks a quality", Say("%g", p->Plain(magmora::kQuality)));
  w.Click(1049., 19.);
  w.Click(1000., 52. + 25. * 1 + 12.);
  tick(0.4);

  {
    w.Click(1049., 19.);
    w.Click(1000., 52. + 25. * 12 + 12.);  // Circuit: As drawn
    tick(0.4);
    Bytes state;
    p->Save(state);
    const char* name = "as-drawn";
    Check(std::search(state.begin(), state.end(), name, name + strlen(name)) != state.end(), "engine menu picks the circuit");
    w.Click(1049., 19.);
    w.Click(1000., 52. + 25. * 11 + 12.);  // and back to the default
    tick(0.4);
  }

  w.Click(1254., 19.);  // about
  fresh();
  w.DragUp(low.x, low.y, 40.);  // closes it and must not reach the knob
  tick();
  Check(p->events.empty(), "the about page covers the panel");

  const double scales[] = {0.75, 1., 1.25, 1.5};
  for (int i = 3; i >= 0; --i) {
    w.Click(1180., 19.);
    w.Click(1140., 52. + 25. * (i + 1) + 12.);
    Check(std::fabs(w.Scale() - scales[i]) < 0.002, Say("window size %d %%", int(scales[i] * 100)), Say("%.3f", w.Scale()));
    p->SetPlain(magmora::kMid, 5.);
    fresh();
    w.DragUp(mid.x, mid.y, 50.);
    tick();
    Check(std::fabs(p->Plain(magmora::kMid) - 7.5) < 0.05, Say("knob travel is the same at %d %%", int(scales[i] * 100)),
          Say("%.3f", p->Plain(magmora::kMid)));
  }

  if (auto* v3 = dynamic_cast<V3*>(p.get())) {
    const int sizes[][2] = {{1600, 775}, {1000, 900}, {3000, 1500}, {200, 100}};
    for (const auto& s : sizes) {
      ViewRect r(0, 0, s[0], s[1]);
      v3->View()->checkSizeConstraint(&r);
      v3->View()->onSize(&r);
      Pump(0.1);
      w.Fit();
      const double k = r.getWidth() / 1280.;
      Check(k >= 0.7499 && k <= 1.5001 && std::fabs(r.getHeight() - 620. * k) <= 1. && std::fabs(w.Scale() - k) < 0.003,
            Say("host resize %dx%d", s[0], s[1]), Say("%dx%d, drawn at %.3f", r.getWidth(), r.getHeight(), w.Scale()));
    }
  }

  fresh();
  w.Down(low.x, low.y);
  w.HoldDrag(low.x, low.y, 10., kNone, 3);
  p->CloseView();
  Pump(0.1);
  Check(Gestures(p->events, magmora::kLow, 1), "closing the window mid-drag ends the gesture");

  Class formatter = nil;
  unsigned count = 0;
  Class* all = objc_copyClassList(&count);
  for (unsigned i = 0; i < count; ++i)
    if (std::string(class_getName(all[i])).rfind("IGraphicsFormatter_", 0) == 0) formatter = all[i];
  free(all);
  if (formatter) {
    using Init = ::id (*)(::id, SEL, ::id, NSUInteger);
    NSFormatter* f2 = reinterpret_cast<Init>(objc_msgSend)([formatter alloc], NSSelectorFromString(@"initWithAllowed:limit:"),
                                                          [NSCharacterSet characterSetWithCharactersInString:@"0123456789.-+"], 5);
    const auto edit = [&](NSString* from, NSString* to) {
      NSString* s = to;
      NSRange range = NSMakeRange(to.length, 0);
      const BOOL ok = [f2 isPartialStringValid:&s proposedSelectedRange:&range originalString:from
                           originalSelectedRange:NSMakeRange(from.length, 0) errorDescription:nil];
      return std::make_pair(static_cast<bool>(ok), std::string(s.UTF8String));
    };
    ::id object = nil;
    Check(edit(@"1", @"12").first && edit(@"1", @"").first && edit(@"12", @"12x") == std::make_pair(false, std::string("12")) &&
              edit(@"12345", @"123456") == std::make_pair(false, std::string("12345")) &&
              [f2 getObjectValue:&object forString:@"-3.5" errorDescription:nil] && [object isEqual:@"-3.5"],
          "replacement formatter filters characters and length");
  } else {
    Check(false, "replacement formatter is in the binary");
  }

  TestPages();
  TestBarAndPages();
  TestIrSteps();
  TestTuner();
  TestUserPresets();
}

// The block pages: switched from the bar, their controls reach their parameters, the amp below is out of reach.
void TestPages() {
  ReadLayout();
  auto p = Make(gBundle);
  Format f;
  p->Setup(f);
  Window w(*p, 1.);
  const auto tick = [&](double seconds = 0.08) { Settle(*p, seconds); };
  const auto fresh = [&] {
    tick();
    p->events.clear();
  };
  const Spot mid = gSpots["mid"];
  struct PageKnob {
    const char* page;
    float word, x, y;
    int id;
  };
  // Positions from plugin/resources/img/pages.json. The bar reads Pedal, Amp, EQ, Cab (signal order).
  const PageKnob pageKnobs[] = {{"pedal drive", kTabPedal, 568.f, 194.f, magmora::kPedalDrive},
                                {"EQ band 2 gain", kTabEq, 597.f, 384.f, magmora::kEqBand2Gain},
                                {"EQ screen, band 3 node dragged", kTabEq, 758.f, 231.f, magmora::kEqBand3Gain},
                                {"cab mic 1 level", kTabCab, 376.f, 566.f, magmora::kMic1Level},
                                {"cab cone 1 marker, dragged", kTabCab, 378.f, 309.f, magmora::kMic1Distance},
                                {"cab mic 2 pan", kTabCab, 1168.f, 566.f, magmora::kMic2Pan}};
  for (const PageKnob& k : pageKnobs) {
    w.Click(k.word, 19.);
    fresh();
    const double before = p->Plain(k.id);
    w.DragUp(k.x, k.y, 40.);
    tick();
    Check(p->Plain(k.id) != before && Gestures(p->events, k.id, 1), Say("%s page: knob drags, one gesture", k.page),
          Say("%.3f -> %.3f", before, p->Plain(k.id)));
  }
  struct PageSwitch {
    const char* what;
    float word, x, y;
    int id;
  };
  const PageSwitch pageSwitches[] = {{"pedal footswitch", kTabPedal, 640.f, 480.f, magmora::kPedalOn},
                                     {"EQ in", kTabEq, 136.f, 384.f, magmora::kEqOn},
                                     {"EQ order (pre or post the cabinet)", kTabEq, 136.f, 488.f, magmora::kEqPosition},
                                     {"microphone 2 on", kTabCab, 748.f, 566.f, magmora::kMic2On},
                                     {"cab align", kTabCab, 643.f, 566.f, magmora::kMicAlign}};
  for (const PageSwitch& k : pageSwitches) {
    w.Click(k.word, 19.);
    fresh();
    const double before = p->Plain(k.id);
    w.Click(k.x, k.y);
    tick();
    Check(p->Plain(k.id) == 1. - before && Gestures(p->events, k.id, 1), Say("%s switches, one gesture", k.what));
  }
  w.Click(kTabCab, 19.);
  fresh();
  w.DragUp(mid.x, mid.y, 60.);  // under the cabinet page
  tick();
  Check(p->events.empty(), "a page covers the amp: no amp knob moves beneath it");
  // The EQ's slope is a rotary switch: a click steps it on.
  w.Click(kTabEq, 19.);
  fresh();
  const double slope = p->Plain(magmora::kEqHighPassSlope);
  w.Click(262., 488.);
  tick();
  Check(p->Plain(magmora::kEqHighPassSlope) != slope && Gestures(p->events, magmora::kEqHighPassSlope, 1),
        "EQ page: the high-pass slope steps on a click, one gesture");
  w.Click(kTabAmp, 19.);  // back to the amp
  fresh();
  w.DragUp(mid.x, mid.y, 20.);
  tick();
  Check(Gestures(p->events, magmora::kMid, 1), "back on the amp page, its knobs respond again");

  // A power toggle on each tab. A click switches that section's parameter (one gesture, nothing else) and leaves
  // the page alone; its light follows the parameter whoever sets it (host, footswitch, button). The name beside it
  // only shows the page: the two mouse targets meet without overlapping.
  {
    const struct {
      const char* name;
      float power, word;
      int id;
    } tabs[] = {{"Pedal", kPowerPedal, kTabPedal, magmora::kPedalOn}, {"Amp", kPowerAmp, kTabAmp, magmora::kAmpOn},
                {"EQ", kPowerEq, kTabEq, magmora::kEqOn}, {"Cab", kPowerCab, kTabCab, magmora::kCabOn}};
    // How lit a toggle is: the most amber pixel (red over blue) around its symbol, at 1x and 100 %.
    const auto lit = [&](float x) {
      w.Move(640., 600.);
      std::vector<uint8_t> px;
      int width = 0, height = 0;
      if (!w.Capture(px, width, height) || width < 1280) return -1;
      int most = 0;
      for (int y = 11; y <= 27; ++y)
        for (int i = static_cast<int>(x) - 7; i <= static_cast<int>(x) + 7; ++i) {
          const uint8_t* c = &px[(static_cast<size_t>(y) * width + i) * 4];  // BGRA
          most = std::max(most, int(c[2]) - int(c[0]));
        }
      return most;
    };
    w.Click(kTabAmp, 19.);
    for (const auto& t : tabs) {
      fresh();
      const double before = p->Plain(t.id);
      w.Click(t.power, 19.);
      tick();
      bool only = true;
      for (const Event& e : p->events) only &= e.id == t.id;
      Check(p->Plain(t.id) == 1. - before && Gestures(p->events, t.id, 1) && only,
            Say("%s power toggle in the bar switches it, one gesture", t.name), Say("%.0f -> %.0f", before, p->Plain(t.id)));
      fresh();
      w.DragUp(mid.x, mid.y, 10.);  // still the amp page
      tick();
      Check(Gestures(Only(p->events, magmora::kMid), magmora::kMid, 1), Say("%s power toggle leaves the page as it was", t.name));
      p->SetPlain(t.id, 1.);
      tick(0.15);
      const int on = lit(t.power);
      p->SetPlain(t.id, 0.);
      tick(0.15);
      const int off = lit(t.power);
      Check(on > 100 && off < 40, Say("%s power toggle lights with its parameter", t.name), Say("on %d, off %d", on, off));
    }
    // the targets' common edge: the toggle's last point switches, the name's first shows the page
    const float edge = kTabPedal - 19.f;  // the Pedal name's strip starts here (293 - 38 / 2 = 274)
    fresh();
    w.Click(edge - 1.f, 19.);
    tick();
    Check(Gestures(p->events, magmora::kPedalOn, 1), "the toggle's mouse target reaches its edge", Say("at %.0f", edge - 1.f));
    fresh();
    w.Click(edge + 1.f, 19.);
    tick();
    const bool none = p->events.empty();
    w.DragUp(mid.x, mid.y, 10.);  // the pedal page now covers the amp
    tick();
    Check(none && p->events.empty(), "the name's mouse target starts there: it shows the page and switches nothing");
    // the pedal's footswitch and the EQ's button move the toggles too
    for (const auto& t : {tabs[0], tabs[2]}) {
      w.Click(t.word, 19.);
      p->SetPlain(t.id, 0.);
      tick(0.15);
      const float x = t.id == magmora::kPedalOn ? 640.f : 136.f, y = t.id == magmora::kPedalOn ? 480.f : 384.f;
      w.Click(x, y);
      tick(0.15);
      const int on = lit(t.power);
      Check(p->Plain(t.id) == 1. && on > 100, Say("%s switched on its page: its toggle in the bar lights", t.name),
            Say("%d", on));
    }
    w.Click(kTabAmp, 19.);
    tick();
  }

  // A slot's source: the menu in its window picks a pack microphone or the slot's file. A file slot's
  // position knob is dimmed and does nothing; its level and pan work.
  auto q = Make(gBundle);
  q->Setup(f);
  ChainOn(*q);  // slot 2 holds the response file
  Settle(*q, 0.2);
  Window wq(*q, 1.);
  const auto freshQ = [&] {
    Settle(*q, 0.08);
    q->events.clear();
  };
  wq.Click(kTabCab, 19.);
  freshQ();
  const double position = q->Plain(magmora::kMic2Position);
  wq.DragUp(922., 566., 40.);
  Settle(*q, 0.08);
  Check(q->Plain(magmora::kMic2Position) == position && q->events.empty(), "a file slot's position knob is inert");
  freshQ();
  const double pan = q->Plain(magmora::kMic2Pan);
  wq.DragUp(1168., 566., 40.);
  Settle(*q, 0.08);
  Check(q->Plain(magmora::kMic2Pan) != pan && Gestures(q->events, magmora::kMic2Pan, 1), "a file slot's pan works");
  freshQ();
  wq.Click(832., 566.);  // slot 2's source menu: microphones, then the file, Load IR file..., Normalise, Clear
  Settle(*q, 0.1);
  wq.Click(849., 420.);  // the pack's first microphone
  Settle(*q, 0.1);
  Check(q->Plain(magmora::kMic2Source) == 0. && q->Plain(magmora::kMic2Type) == 0. &&
            Gestures(Only(q->events, magmora::kMic2Source), magmora::kMic2Source, 1),
        "the source menu puts a pack microphone in a file slot, one gesture",
        Say("source %.0f, microphone %.0f", q->Plain(magmora::kMic2Source), q->Plain(magmora::kMic2Type)));
  freshQ();
  wq.DragUp(922., 566., 40.);
  Settle(*q, 0.08);
  Check(q->Plain(magmora::kMic2Position) != position, "on a microphone, the slot's position knob works again");
  freshQ();
  wq.Click(832., 566.);
  Settle(*q, 0.1);
  wq.Click(849., 495.);  // the slot's file, still loaded
  Settle(*q, 0.1);
  Check(q->Plain(magmora::kMic2Source) == 1. && Gestures(Only(q->events, magmora::kMic2Source), magmora::kMic2Source, 1),
        "the source menu puts the slot's loaded file back, one gesture");
}

// ---- user presets, tab gestures, meters, tuner, hints (v0.1 UX) ---------------------------------------------------

std::string gPresets;  // the presets folder the plug-in uses here (MAGMORA_PRESETS_DIR), emptied for each run

void FreshPresets() {
  gPresets = gFixtures + "/presets";
  [NSFileManager.defaultManager removeItemAtPath:@(gPresets.c_str()) error:nil];
  setenv("MAGMORA_PRESETS_DIR", gPresets.c_str(), 1);
}

bool FileExists(const std::string& path) { return [NSFileManager.defaultManager fileExistsAtPath:@(path.c_str())]; }

std::string ReadAll(const std::string& path) {
  NSData* d = [NSData dataWithContentsOfFile:@(path.c_str())];
  return d ? std::string(static_cast<const char*>(d.bytes), d.length) : std::string();
}

bool ReadPresetFile(const std::string& path, magmora::UserPreset& p) {
  const std::string text = ReadAll(path);
  return !text.empty() && magmora::readUserPreset(text.data(), text.size(), p);
}

void WritePresetFile(const std::string& path, const std::string& name, double preLead, const std::string& folder = "") {
  magmora::UserPreset p;
  double values[magmora::kNumChainParams];
  for (int i = 0; i < magmora::kNumChainParams; ++i) values[i] = magmora::Chain::parameterInfo(i)->def;
  values[magmora::kPreLead] = preLead;
  magmora::setPresetSnapshot(p, values);
  p.name = name;
  const std::string dir = folder.empty() ? gPresets : gPresets + "/" + folder;
  [NSFileManager.defaultManager createDirectoryAtPath:@(dir.c_str()) withIntermediateDirectories:YES attributes:nil error:nil];
  const std::string text = magmora::writeUserPreset(p);
  [[NSData dataWithBytes:text.data() length:text.size()] writeToFile:@((dir + "/" + path).c_str()) atomically:YES];
}

// The user preset a state names ("upre": name, file relative to the presets folder); empty if none.
std::pair<std::string, std::string> UserOf(const Bytes& state) {
  const char tag[] = "upre";
  auto it = std::search(state.begin(), state.end(), tag, tag + 4);
  if (it == state.end()) return {};
  size_t at = static_cast<size_t>(it - state.begin()) + 4 + 4;  // tag, version
  at += 4 + At<int32_t>(state, at);                              // hash
  at += 4;                                                       // size
  const auto str = [&](std::string& out) {
    const int32_t n = At<int32_t>(state, at);
    out.assign(&state[at + 4], static_cast<size_t>(n));
    at += 4 + static_cast<size_t>(n);
  };
  std::pair<std::string, std::string> r;
  str(r.first);
  str(r.second);
  return r;
}

// The preset menu's right column (plugin/ui/MagmoraUI.cpp OpenPresets): x of a row and y of its slot, for a menu whose
// longest column has `most` rows (Overlay::OpenMenu: rows shrink to fit, then scroll at 22 points).
constexpr double kRightColumn = 74. + 232. + 90.;
double MenuRowH(int most) {
  const double space = 620. - 44. - 16. - 8.;
  double h = std::min(25., space / most);
  return h < 20. ? 22. : h;
}
double MenuRowY(int slot, int most) { return 52. + MenuRowH(most) * slot + MenuRowH(most) / 2.; }
constexpr int kFactoryRows = 25;  // 6 headings and 19 presets

// The brightest luminance ratio against the bar's wall in a box (WCAG contrast of the type's core).
double Contrast(const std::vector<uint8_t>& px, int width, double scale, double x0, double y0, double x1, double y1) {
  const auto lum = [](double c) {
    c /= 255.;
    return c <= 0.03928 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
  };
  const auto L = [&](const uint8_t* c) { return 0.2126 * lum(c[2]) + 0.7152 * lum(c[1]) + 0.0722 * lum(c[0]); };
  const double wall = 0.2126 * lum(0x37) + 0.7152 * lum(0x32) + 0.0722 * lum(0x2c);
  double best = 0.;
  for (int y = static_cast<int>(y0 * scale); y < static_cast<int>(y1 * scale); ++y)
    for (int x = static_cast<int>(x0 * scale); x < static_cast<int>(x1 * scale); ++x)
      best = std::max(best, L(&px[(static_cast<size_t>(y) * width + x) * 4]));
  return (best + 0.05) / (wall + 0.05);
}

// Mean of red minus blue, and the brightest pixel, in a box: how amber, or how lit, something drawn there is.
struct Box {
  double amber = 0., bright = 0.;
};
Box Look(Window& w, double x0, double y0, double x1, double y1, double wait = 0.25) {
  std::vector<uint8_t> px;
  int width = 0, height = 0;
  Box b;
  if (!w.Capture(px, width, height, wait)) return b;
  const double s = width / 1280.;
  for (int y = static_cast<int>(y0 * s); y < static_cast<int>(y1 * s); ++y)
    for (int x = static_cast<int>(x0 * s); x < static_cast<int>(x1 * s); ++x) {
      const uint8_t* c = &px[(static_cast<size_t>(y) * width + x) * 4];
      b.amber = std::max(b.amber, double(c[2]) - double(c[0]));
      b.bright = std::max(b.bright, (double(c[0]) + c[1] + c[2]) / 3.);
    }
  return b;
}

void TestUserPresets() {
  FreshPresets();
  auto p = Make(gBundle);
  Format f;
  p->Setup(f);
  Window w(*p, 1.);
  const auto tick = [&](double seconds = 0.08) { Settle(*p, seconds); };
  const std::string wall = gPresets + "/Wall Of Sound.json";
  const auto menu = [&] {  // closed first: an action may have left it open
    w.Click(700., 610.);
    w.Click(120., 19.);
    tick();
  };

  // Save on a factory preset is Save as: the name field opens, filled with "<name> 2".
  menu();
  w.Click(kRightColumn, MenuRowY(0, kFactoryRows));  // Save
  tick();
  NSTextField* field = w.Field();
  Check(field != nil && std::string(field.stringValue.UTF8String) == "Bedrock 2", "Save on a factory preset asks for a name",
        field ? field.stringValue.UTF8String : "no field");
  Check(!FileExists(gPresets), "the presets folder is made on the first save, not before");
  p->SetPlain(magmora::kLow, 2.5);
  p->SetPlain(magmora::kPedalOn, 1.);
  p->SetPlain(magmora::kInputTrim, 4.);
  tick();
  w.Type("Wall Of Sound");
  tick(0.15);
  magmora::UserPreset saved;
  Check(ReadPresetFile(wall, saved) && saved.name == "Wall Of Sound" && saved.values[magmora::kLow] == 2.5 &&
            saved.values[magmora::kPedalOn] == 1. && saved.hasCabFiles,
        "Save as writes the preset file with every section");
  const std::string text = ReadAll(wall);
  Check(text.find("input_trim") == std::string::npos && text.find("\"quality\"") == std::string::npos &&
            text.find("channel_mode") == std::string::npos && text.find("\"format\": \"magmora-preset\"") != std::string::npos,
        "a preset file has no session settings");
  bool leftovers = false;
  for (NSString* name in [NSFileManager.defaultManager contentsOfDirectoryAtPath:@(gPresets.c_str()) error:nil])
    leftovers |= [name hasSuffix:@".tmp"];
  Check(!leftovers, "the atomic write leaves no temporary file");
  Bytes state;
  p->Save(state);
  Check(UserOf(state) == std::make_pair(std::string("Wall Of Sound"), std::string("Wall Of Sound.json")),
        "the session remembers the user preset and its file", UserOf(state).first + " / " + UserOf(state).second);

  // Save overwrites the loaded user preset without asking, and only once something changed.
  NSDate* before = [[NSFileManager.defaultManager attributesOfItemAtPath:@(wall.c_str()) error:nil] fileModificationDate];
  menu();
  w.Click(kRightColumn, MenuRowY(0, kFactoryRows));  // Save
  tick();
  NSDate* after = [[NSFileManager.defaultManager attributesOfItemAtPath:@(wall.c_str()) error:nil] fileModificationDate];
  Check([before isEqualToDate:after] && w.Field() == nil, "Save is inert while nothing is edited");
  p->SetPlain(magmora::kLow, 3.5);
  tick();
  menu();
  w.Click(kRightColumn, MenuRowY(0, kFactoryRows));  // Save
  tick();
  Check(ReadPresetFile(wall, saved) && saved.values[magmora::kLow] == 3.5, "Save writes the edited user preset in place");

  // Save as onto an existing name: a Replace row with Cancel, in the menu.
  menu();
  w.Click(kRightColumn, MenuRowY(1, kFactoryRows));  // Save as...
  tick();
  p->SetPlain(magmora::kLow, 9.);
  tick();
  w.Type("Wall Of Sound");
  tick(0.15);
  Check(ReadPresetFile(wall, saved) && saved.values[magmora::kLow] == 3.5, "an existing name is not overwritten without asking");
  w.Click(435., MenuRowY(1, kFactoryRows));  // Replace "Wall Of Sound"?, then Replace · Cancel
  tick();
  Check(ReadPresetFile(wall, saved) && saved.values[magmora::kLow] == 3.5, "Cancel keeps the file");
  menu();
  w.Click(kRightColumn, MenuRowY(1, kFactoryRows));  // Save as...
  tick();
  w.Type("Wall Of Sound");
  tick(0.15);
  w.Click(360., MenuRowY(1, kFactoryRows));  // Replace
  tick();
  Check(ReadPresetFile(wall, saved) && saved.values[magmora::kLow] == 9., "Replace overwrites it");
  // A factory name is reserved.
  menu();
  w.Click(kRightColumn, MenuRowY(1, kFactoryRows));
  tick();
  w.Type("bedrock");
  tick(0.15);
  Check(!FileExists(gPresets + "/bedrock.json") && !FileExists(gPresets + "/Bedrock.json"), "factory names are reserved");

  // Rename and delete from the row's glyphs, the loaded preset included. Rows: Save, Save as, User, Wall Of Sound.
  menu();
  const double row = MenuRowY(3, kFactoryRows), right = 74. + 2 * 232.;
  w.Move(kRightColumn, row);
  w.Click(right - 34., row);  // the pencil
  tick();
  Check(w.Field() != nil && std::string(w.Field().stringValue.UTF8String) == "Wall Of Sound", "the pencil opens the name");
  w.Type("Club/Lead: 2");
  tick(0.15);
  const std::string club = gPresets + "/Club-Lead- 2.json";
  Check(!FileExists(wall) && ReadPresetFile(club, saved) && saved.name == "Club/Lead: 2",
        "rename writes the new file and removes the old; the file name is made safe");
  p->Save(state);
  Check(UserOf(state).first == "Club/Lead: 2" && UserOf(state).second == "Club-Lead- 2.json", "renaming the loaded preset follows it");
  menu();
  w.Move(kRightColumn, row);
  w.Click(right - 12., row);  // the bin
  tick();
  Check(FileExists(club), "the bin asks first");
  w.Click(355., MenuRowY(4, kFactoryRows));  // Delete "Club/Lead: 2"?, then Delete · Cancel
  tick();
  Check(!FileExists(club), "Delete removes the file");
  p->Save(state);
  Check(UserOf(state).first == "Club/Lead: 2" && std::fabs(p->Plain(magmora::kLow) - 9.) < 1e-6,
        "deleting the loaded preset keeps the sound and the name (marked edited)");

  // Order of prev/next: factory presets, then the user's (folder itself, then each folder, by name), round and round.
  WritePresetFile("b.json", "b", 3.1);
  WritePresetFile("A.json", "A", 3.2);
  WritePresetFile("C.json", "C", 3.3, "Live");
  [[NSData dataWithBytes:"{\"format\": \"magmora-preset\", \"name\": " length:36]
      writeToFile:@((gPresets + "/broken.json").c_str()) atomically:YES];
  [[NSData dataWithBytes:"\x00\x01garbage" length:9] writeToFile:@((gPresets + "/junk.json").c_str()) atomically:YES];
  menu();
  w.Click(150., 52. + MenuRowH(kFactoryRows) * 24 + 10.);  // the last factory preset (Scooped Wall)
  tick();
  std::vector<std::string> order;
  std::vector<double> leads;
  for (int i = 0; i < 4; ++i) {
    w.Click(50., 19.);
    tick();
    p->Save(state);
    order.push_back(UserOf(state).first);
    leads.push_back(p->Plain(magmora::kPreLead));
  }
  Check(order[0] == "A" && order[1] == "b" && order[2] == "C" && order[3].empty() && std::fabs(leads[0] - 3.2) < 1e-6 &&
            std::fabs(leads[2] - 3.3) < 1e-6 && std::fabs(leads[3] - 5.) < 1e-6,
        "next: factory, then A, b, Live/C, then round to Bedrock; broken files are skipped",
        order[0] + "," + order[1] + "," + order[2] + "," + order[3]);
  w.Click(26., 19.);
  tick();
  p->Save(state);
  Check(UserOf(state).first == "C", "previous goes back round to the last user preset");

  // A loaded user preset renders as the instance that saved it (save, reload in a new instance, play).
  {
    p->SetPlain(magmora::kPreLead, 6.6);
    p->SetPlain(magmora::kEqOn, 1.);
    p->SetPlain(magmora::kEqBand2Gain, 4.);
    p->SetPlain(magmora::kInputTrim, 0.);  // a session setting: the other instance has its own
    tick();
    menu();
    w.Click(kRightColumn, MenuRowY(1, kFactoryRows));  // Save as
    tick();
    w.Type("Render Check");
    tick(0.15);
    Check(FileExists(gPresets + "/Live/Render Check.json"), "Save as from a preset in a folder saves into that folder");
    auto q = Make(gBundle);
    q->Setup(f);
    Window wq(*q, 1.);
    Settle(*q, 0.1);
    wq.Click(120., 19.);
    // rows: Save, Save as, User, A, b, Live, C, Render Check (broken files are not listed)
    wq.Click(kRightColumn, MenuRowY(7, kFactoryRows));
    Settle(*q, 0.3);
    Settle(*p, 0.3);
    Bytes qs;
    q->Save(qs);
    const Audio x = Pluck(48000, 48000.);
    p->Reset();
    q->Reset();
    const auto a = p->Run({x}), b = q->Run({x});
    Check(UserOf(qs).first == "Render Check" && Difference(a[0], b[0]) < 1e-6 && Peak(a[0]) > 1e-3,
          "a user preset loaded in a new instance renders the same", Say("%s, %.2g", UserOf(qs).first.c_str(), Difference(a[0], b[0])));
  }

  // Files: found where saved, next to the preset, or reported missing with the reference kept through a save.
  {
    NSFileManager* fm = NSFileManager.defaultManager;
    [fm copyItemAtPath:@((gFixtures + "/room.wav").c_str()) toPath:@((gPresets + "/moved.wav").c_str()) error:nil];
    magmora::UserPreset u;
    double values[magmora::kNumChainParams];
    for (int i = 0; i < magmora::kNumChainParams; ++i) values[i] = magmora::Chain::parameterInfo(i)->def;
    values[magmora::kCabOn] = 1.;
    values[magmora::kMic2On] = 1.;
    values[magmora::kMic2Source] = 1.;
    magmora::setPresetSnapshot(u, values);
    u.hasCabFiles = true;
    u.cab.pack = {true, "builtin:trad-4x12-modelled", "Trad 4x12 (modelled)", "", -1, true};
    u.cab.ir[1] = {true, "/no/such/folder/moved.wav", "moved.wav", "", -1, true};
    u.name = "Zz Moved";
    std::string t = magmora::writeUserPreset(u);
    [[NSData dataWithBytes:t.data() length:t.size()] writeToFile:@((gPresets + "/Zz Moved.json").c_str()) atomically:YES];
    u.cab.ir[1] = {true, "/no/such/folder/gone.wav", "gone.wav", "", 1234, false};
    u.name = "Zz Missing";
    t = magmora::writeUserPreset(u);
    [[NSData dataWithBytes:t.data() length:t.size()] writeToFile:@((gPresets + "/Zz Missing.json").c_str()) atomically:YES];
    // rows: Save, Save as, User, A, b, Zz Missing, Zz Moved, Live, C, Render Check
    menu();
    w.Click(kRightColumn, MenuRowY(6, kFactoryRows));  // Zz Moved
    tick(0.3);
    p->Save(state);
    Check(Contains(state, gPresets + "/moved.wav"), "a response file moved next to the preset is found there");
    menu();
    w.Click(kRightColumn, MenuRowY(5, kFactoryRows));  // Zz Missing
    tick(0.3);
    p->Save(state);
    Check(Contains(state, "/no/such/folder/gone.wav"), "a missing file keeps its reference in the session");
    p->SetPlain(magmora::kLow, 1.25);
    tick();
    menu();
    w.Click(kRightColumn, MenuRowY(1, kFactoryRows));  // Save (after the "1 file missing" note)
    tick();
    Check(ReadPresetFile(gPresets + "/Zz Missing.json", saved) && saved.cab.ir[1].present &&
              saved.cab.ir[1].path == "/no/such/folder/gone.wav" && saved.values[magmora::kLow] == 1.25,
          "saving keeps the missing file's reference");
  }

  // 200 presets in a folder: the column scrolls, and its last row loads the last preset.
  {
    menu();
    w.Click(150., 52. + MenuRowH(kFactoryRows) * 1 + 10.);  // Bedrock: no missing-file note from here on
    tick();
    for (int i = 0; i < 200; ++i) {
      char name[32];
      snprintf(name, sizeof name, "Bulk %03d", i);
      WritePresetFile(std::string(name) + ".json", name, 0.01 * (i % 100) + (i >= 100 ? 1. : 0.), "Bulk");
    }
    menu();
    for (int i = 0; i < 100; ++i) w.Wheel(kRightColumn, 300., -1.);
    tick();
    const int visible = static_cast<int>((620. - 44. - 16. - 8.) / 22.);
    w.Click(kRightColumn, MenuRowY(visible - 2, 300));  // above the Import row at the bottom: Live/Render Check, the last
    tick(0.2);
    p->Save(state);
    Check(UserOf(state).first == "Render Check", "with 200 presets the menu scrolls to its end", UserOf(state).first);
    menu();
    for (int i = 0; i < 100; ++i) w.Wheel(kRightColumn, 300., -1.);
    w.Click(kRightColumn, MenuRowY(visible - 5, 300));  // Bulk 199 (then the Live heading, C, Render Check, Import)
    tick(0.2);
    p->Save(state);
    Check(UserOf(state).first == "Bulk 199" && std::fabs(p->Plain(magmora::kPreLead) - 1.99) < 1e-6,
          "a scrolled row loads its preset", UserOf(state).first);
    menu();
    w.Click(150., 52. + MenuRowH(300) * 1 + 10.);  // the factory column does not scroll: Bedrock
    tick();
    p->Save(state);
    Check(UserOf(state).first.empty() && std::fabs(p->Plain(magmora::kPreLead) - 5.) < 1e-6, "the factory column still picks");
  }
  FreshPresets();
}

// A manual check of the real location (~/Library/Audio/Presets/Magmora): save one preset there through the menu,
// see it, delete it through the menu, and remove the folder again. Refuses if the folder exists already.
void SmokePresets() {
  unsetenv("MAGMORA_PRESETS_DIR");
  const std::string root = std::string(getenv("HOME")) + "/Library/Audio/Presets/Magmora";
  if (FileExists(root)) {
    Check(false, "the presets folder exists already: not touching it", root);
    return;
  }
  auto p = Make(gBundle);
  Format f;
  p->Setup(f);
  Window w(*p, 1.);
  Settle(*p, 0.1);
  w.Click(120., 19.);
  w.Click(kRightColumn, MenuRowY(0, kFactoryRows));  // Save on Bedrock: Save as
  Settle(*p, 0.1);
  w.Type("Smoke Test");
  Settle(*p, 0.2);
  const std::string file = root + "/Smoke Test.json";
  magmora::UserPreset saved;
  Check(ReadPresetFile(file, saved) && saved.name == "Smoke Test", "saved into the real presets folder", file);
  Bytes state;
  p->Save(state);
  Check(UserOf(state).second == "Smoke Test.json", "the session names it");
  w.Click(700., 610.);
  w.Click(120., 19.);
  w.Click(74. + 2 * 232. - 12., MenuRowY(3, kFactoryRows));  // its bin (Save, Save as, User, Smoke Test)
  w.Click(355., MenuRowY(4, kFactoryRows));                   // Delete
  Settle(*p, 0.1);
  Check(!FileExists(file), "deleted through the menu");
  rmdir(root.c_str());
  Check(!FileExists(root), "the folder is gone again");
}

void TestBarAndPages() {
  auto p = Make(gBundle);
  Format f;
  p->Setup(f);
  Window w(*p, 1.);
  const auto tick = [&](double seconds = 0.08) { Settle(*p, seconds); };
  const auto fresh = [&] {
    tick();
    p->events.clear();
  };
  const Spot mid = gSpots["mid"];

  // Tab names: a double click switches the section and shows it, a right click switches it only; one gesture each.
  {
    fresh();
    const double before = p->Plain(magmora::kEqOn);
    w.DoubleClick(kTabEq, 19.);
    tick();
    Check(p->Plain(magmora::kEqOn) == 1. - before && Gestures(Only(p->events, magmora::kEqOn), magmora::kEqOn, 1),
          "double-click on a tab name switches its section, one gesture");
    fresh();
    w.DragUp(597., 384., 10.);  // the EQ's band 2 gain
    tick();
    Check(Gestures(p->events, magmora::kEqBand2Gain, 1), "and shows its page");
    w.Click(kTabAmp, 19.);
    fresh();
    w.RightClick(kTabPedal, 19.);
    const Box flash = Look(w, kPowerPedal - 9., 10., kPowerPedal + 9., 28., 0.06);
    tick(0.5);
    const Box later = Look(w, kPowerPedal - 9., 10., kPowerPedal + 9., 28.);
    Check(p->Plain(magmora::kPedalOn) == 1. && Gestures(Only(p->events, magmora::kPedalOn), magmora::kPedalOn, 1),
          "right-click on a tab name switches it");
    w.RightClick(kTabPedal, 19.);
    const Box offFlash = Look(w, kPowerPedal - 9., 10., kPowerPedal + 9., 28., 0.06);
    tick(0.5);
    const Box off = Look(w, kPowerPedal - 9., 10., kPowerPedal + 9., 28.);
    Check(offFlash.amber > off.amber + 30., "switching a section off from its name flashes its symbol",
          Say("flash %.0f, after %.0f (on: %.0f, %.0f)", offFlash.amber, off.amber, flash.amber, later.amber));
    fresh();
    w.DragUp(mid.x, mid.y, 10.);
    tick();
    Check(Gestures(Only(p->events, magmora::kMid), magmora::kMid, 1), "a right click leaves the page as it was");
    fresh();
    w.Click(kTabPedal, 19.);
    tick();
    Check(p->events.empty(), "a single click on a name switches nothing");
    w.Click(kTabAmp, 19.);
    // the symbol's mouse target is 22 points: 6 more to its left, into the gap
    fresh();
    w.Click(kPowerEq - 7. - 5., 19.);
    tick();
    Check(Gestures(p->events, magmora::kEqOn, 1), "the power symbol's target reaches 6 points further left");
    w.Click(kPowerEq - 7. - 5., 19.);
    tick();
    // an off section's name is readable: at least 4.5:1 against the bar
    p->SetPlain(magmora::kPedalOn, 0.);
    p->SetPlain(magmora::kEqOn, 0.);
    w.Move(640., 600.);
    tick(0.15);
    std::vector<uint8_t> px;
    int width = 0, height = 0;
    w.Capture(px, width, height);
    const double pedal = Contrast(px, width, 1., kTabPedal - 18., 12., kTabPedal + 18., 26.);
    const double eq = Contrast(px, width, 1., kTabEq - 10., 12., kTabEq + 10., 26.);
    Check(pedal >= 4.5 && eq >= 4.5, "switched-off tab names have 4.5:1 contrast", Say("Pedal %.2f, EQ %.2f", pedal, eq));
  }

  // Meters: the Input field's target marks; a clip mark that latches and clears on a click.
  {
    w.Move(640., 600.);
    const double left = 494., width = 96.;
    const auto x = [&](double db) { return std::round(left + width * (db + 60.) / 66.); };
    const Box mark = Look(w, x(-18.), 30., x(-18.) + 1., 34.);
    const Box between = Look(w, x(-12.) + 3., 30., x(-12.) + 4., 34.);
    Check(mark.bright > between.bright + 30., "the Input meter shows its target marks",
          Say("mark %.0f, beside %.0f", mark.bright, between.bright));
    p->SetPlain(magmora::kOutputLevel, 12.);
    p->SetPlain(magmora::kInputTrim, 12.);
    const Audio loud = Sine(48000 / 4, 48000., 220., 0.9);
    for (int i = 0; i < 8; ++i) {
      p->Run({loud});
      Pump(0.02);
    }
    p->SetPlain(magmora::kOutputLevel, 0.);
    p->SetPlain(magmora::kInputTrim, 0.);
    tick(1.5);  // the meters fall; the marks stay
    const Box clipOut = Look(w, 862. + 96. - 2., 31., 862. + 96., 36.);
    const Box clipIn = Look(w, 590. - 2., 31., 590., 36.);
    Check(clipOut.amber > 100. && clipIn.amber > 100., "the clip marks latch after the level went into the red",
          Say("output %.0f, input %.0f", clipOut.amber, clipIn.amber));
    fresh();
    w.Click(957., 36.);
    tick();
    const Box cleared = Look(w, 862. + 96. - 2., 31., 862. + 96., 36.);
    Check(cleared.amber < 60. && p->events.empty(), "a click on the clip mark clears it and moves nothing",
          Say("%.0f", cleared.amber));
    fresh();
    w.DragUp(900., 19., 20.);
    tick();
    Check(Gestures(p->events, magmora::kOutputLevel, 1), "the rest of the field still drags");
    p->SetPlain(magmora::kOutputLevel, 0.);
  }

  // The hint line: after a rest on a control, under the bar; gone a while after the pointer leaves.
  {
    const Spot presence = gSpots["presence"];
    w.Move(640., 600.);
    tick(1.5);
    const Box none = Look(w, 14., 48., 600., 62.);
    w.Move(presence.x, presence.y);
    tick(0.9);
    const Box shown = Look(w, 14., 48., 600., 62.);
    w.Move(640., 300.);  // the grille: no control
    tick(1.6);
    const Box gone = Look(w, 14., 48., 600., 62.);
    Check(shown.bright > none.bright + 60. && gone.bright < shown.bright - 60., "the hint line shows and goes",
          Say("before %.0f, shown %.0f, after %.0f", none.bright, shown.bright, gone.bright));
  }

  // Double click resets a cabinet microphone and an EQ point, as on the knobs.
  {
    w.Click(kTabCab, 19.);
    tick();
    w.DragUp(378., 309., -40.);  // marker 1 down: farther
    tick();
    const double moved = p->Plain(magmora::kMic1Distance);
    // cone 1: centre (424.65, 298), radius 160.48; the marker is distance / 300 of 0.86 radius below the centre
    const double cy = 298. + moved / 300. * 160.48 * 0.86;
    fresh();
    w.DoubleClick(378., cy);
    tick();
    Check(moved > 30. && std::fabs(p->Plain(magmora::kMic1Distance) - 25.) < 0.01 && std::fabs(p->Plain(magmora::kMic1Position) - 0.3) < 1e-4,
          "double-click puts a microphone marker back", Say("%.1f mm -> %.1f mm", moved, p->Plain(magmora::kMic1Distance)));
    w.Click(kTabEq, 19.);
    tick();
    w.DragUp(758., 231., 30.);
    tick();
    const double gain = p->Plain(magmora::kEqBand3Gain);
    fresh();
    w.DoubleClick(758., 231.);  // with the EQ off its points stay on the 0 dB line
    tick();
    Check(std::fabs(gain) > 1. && std::fabs(p->Plain(magmora::kEqBand3Gain)) < 0.01, "double-click puts an EQ point back",
          Say("%.2f dB -> %.2f dB", gain, p->Plain(magmora::kEqBand3Gain)));
    w.Click(kTabAmp, 19.);
    tick();
  }
}

void TestIrSteps() {
  auto q = Make(gBundle);
  Format f;
  q->Setup(f);
  ChainOn(*q);  // slot 2 holds room.wav; its folder also has dirac.wav and hall.wav
  Settle(*q, 0.2);
  Window w(*q, 1.);
  w.Click(kTabCab, 19.);
  Settle(*q, 0.1);
  const double next = 890. - 16. - 7., previous = 890. - 30. - 7.;
  w.Click(next, 566.);
  Settle(*q, 0.3);
  Bytes s;
  q->Save(s);
  Check(Contains(s, gFixtures + "/dirac.wav") && !Contains(s, gFixtures + "/room.wav"), "the next arrow loads the next file, round");
  w.Click(previous, 566.);
  Settle(*q, 0.3);
  q->Save(s);
  Check(Contains(s, gFixtures + "/room.wav"), "the previous arrow goes back");
  w.Click(previous, 566.);
  Settle(*q, 0.3);
  q->Save(s);
  Check(Contains(s, gFixtures + "/hall.wav"), "and on, alphabetically");
  // the menu's row does the same: rows mics (heading, 2), file heading, file, arrows ...
  w.Click(832., 566.);
  Settle(*q, 0.1);
  const double top = 620. - 8. - (16. + 25. * 9.) + 8.;
  w.Click(774. + 175., top + 25. * 5 + 12.);  // "Next file", the second word of the row
  Settle(*q, 0.3);
  q->Save(s);
  Check(Contains(s, gFixtures + "/room.wav"), "the source menu steps too");
}

void TestTuner() {
  auto p = Make(gBundle);
  Format f;
  p->Setup(f);
  Window w(*p, 1.);
  // A long tone played on in slices, so that the tuner hears it without a seam.
  size_t at = 0;
  const auto play = [&](const Audio& x, double seconds) {
    const double until = Now() + seconds;
    Audio out;
    while (Now() < until) {
      if (at + 1536 > x.size()) at = 0;
      out = p->Run({Audio(x.begin() + static_cast<long>(at), x.begin() + static_cast<long>(at) + 1536)})[0];
      at += 1536;
      Pump(0.01);
    }
    return out;
  };
  const Audio a2 = Sine(48000 * 20, 48000., 110. * std::pow(2., 20. / 1200.), 0.2);
  Check(Peak(play(a2, 0.3)) > 0.01, "the amp plays before the tuner opens");
  w.Click(1222., 19.);
  const Audio muted = play(a2, 0.6);
  Check(Peak(muted) < 1e-4, "the tuner mutes the output while open", Say("%.2g", Peak(muted)));
  // the needle: amber, 20 cents sharp of A2 = 88 points right of the centre on the +-50 cent scale (440 points)
  std::vector<uint8_t> px;
  int width = 0, height = 0;
  double needleX = -1., most = 0.;
  w.Capture(px, width, height);
  for (int x = 640 - 230; x <= 640 + 230; ++x) {
    const uint8_t* c = &px[(static_cast<size_t>(300) * width + x) * 4];
    const double amber = double(c[2]) - double(c[0]);
    if (amber > most) {
      most = amber;
      needleX = x;
    }
  }
  Check(most > 80. && std::fabs(needleX - (640. + 88.)) <= 3., "the tuner's needle reads A2 + 20 cents",
        Say("at %.0f (amber %.0f)", needleX, most));
  // The tuner reads the strings, ahead of the transposer: with transpose at +5 st the note shown is the same A2, and a
  // D3 played (what +5 st makes of A2) shows something else, so the comparison can tell.
  {
    const auto noteArea = [&] {
      std::vector<uint8_t> all, area;
      int cw = 0, ch = 0;
      w.Capture(all, cw, ch);
      for (int y = 222; y < 278; ++y)
        for (int x = 596; x < 690; ++x)
          for (int k = 0; k < 3; ++k) area.push_back(all[(static_cast<size_t>(y) * cw + x) * 4 + k]);
      return area;
    };
    const auto differs = [](const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
      int count = 0;
      for (size_t i = 0; i < a.size() && i < b.size(); ++i) count += std::abs(int(a[i]) - int(b[i])) > 40;
      return count;
    };
    const std::vector<uint8_t> plain = noteArea();
    p->SetPlain(magmora::kTranspose, 5.);
    play(a2, 0.6);
    const std::vector<uint8_t> transposed = noteArea();
    const Audio d3 = Sine(48000 * 20, 48000., 110. * std::pow(2., (500. + 20.) / 1200.), 0.2);
    at = 0;
    play(d3, 0.7);
    const std::vector<uint8_t> other = noteArea();
    p->SetPlain(magmora::kTranspose, 0.);
    at = 0;
    play(a2, 0.6);
    Check(differs(plain, transposed) == 0 && differs(plain, other) > 50,
          "with transpose at +5 st the tuner still reads the strings (A2), not the shifted sound (D3)",
          Say("%d values differ at +5 st, %d for a played D3", differs(plain, transposed), differs(plain, other)));
  }
  // A4 = 432 typed; a 432 Hz tone is then in tune: the needle at the centre
  w.RightClick(750., 407.);
  w.Type("432");
  const Audio a4 = Sine(48000 * 20, 48000., 432., 0.2);
  at = 0;
  play(a4, 0.7);
  w.Capture(px, width, height);
  double brightX = -1., brightest = 0.;
  for (int x = 640 - 230; x <= 640 + 230; ++x) {
    const uint8_t* c = &px[(static_cast<size_t>(300) * width + x) * 4];
    const double v = (double(c[0]) + c[1] + c[2]) / 3.;
    if (v > brightest) {
      brightest = v;
      brightX = x;
    }
  }
  Check(std::fabs(brightX - 640.) <= 2., "with A4 = 432 Hz typed, 432 Hz is in tune", Say("needle at %.0f", brightX));
  // mute off from the tuner itself, then closed by a click outside: the output is back
  w.Click(480., 407.);
  Check(Peak(play(a2, 0.4)) > 0.01, "the tuner's mute can be switched off");
  w.Click(480., 407.);
  w.Click(640., 580.);
  Check(Peak(play(a2, 0.4)) > 0.01, "closing the tuner unmutes");
  w.Click(1222., 19.);
  play(a2, 0.2);
  p->CloseView();
  Pump(0.1);
  Check(Peak(play(a2, 0.3)) > 0.01, "closing the editor with the tuner open unmutes");
}

// ---- screenshots ------------------------------------------------------------------------------------------------

void Shots(const std::string& dir) {
  ReadLayout();
  const std::string kind = gBundle.find(".vst3") != std::string::npos ? "vst3" : "au";
  struct Shot {
    const char* name;
    double screen;
    int percent;
    double knobs;  // < 0: as they are
  };
  const Shot shots[] = {{"1x-075", 1., 75, -1.}, {"1x-100", 1., 100, -1.}, {"1x-125", 1., 125, -1.}, {"1x-150", 1., 150, -1.},
                        {"2x-075", 2., 75, -1.}, {"2x-100", 2., 100, -1.}, {"2x-125", 2., 125, -1.}, {"2x-150", 2., 150, -1.},
                        {"2x-100-knobs0", 2., 100, 0.}, {"2x-100-knobs10", 2., 100, 10.},
                        {"1x-075-knobs0", 1., 75, 0.}, {"1x-150-knobs10", 1., 150, 10.}};
  const auto scaled = [](Plug& p, int percent) {
    Bytes b;
    p.Save(b);
    // Walk the body to the editor scale: flags, count, entries, preset name, then the scale.
    const auto i32 = [&](size_t at) {
      int32_t v = 0;
      memcpy(&v, &b[at], 4);
      return v;
    };
    // The head section of the chain state: tag, version, hash, size, then the head chunk.
    size_t head = 12;
    for (int32_t n = 0, sections = i32(8); n < sections; ++n) {
      if (memcmp(&b[head], "head", 4) == 0) break;
      head += 16 + i32(head + 8) + i32(head + 12 + i32(head + 8));
    }
    head += 16 + i32(head + 8);
    size_t at = head + 12 + 4;
    const int32_t count = i32(at);
    at += 4;
    for (int32_t n = 0; n < count; ++n) at += 4 + i32(at) + 8;
    at += 4 + i32(at);
    const float s = percent / 100.f;
    memcpy(&b[at], &s, 4);
    p.Load(b);
  };
  const int knobs[] = {magmora::kPreRhythm, magmora::kPreLead, magmora::kLow, magmora::kMid, magmora::kHigh,
                       magmora::kPostRhythm, magmora::kPostLead, magmora::kResonance, magmora::kPresence};
  Format f;
  f.block = 256;
  const auto save = [&](Window& w, const std::string& name) {
    const std::string path = dir + "/" + kind + "-" + name + ".png";
    Check(w.SavePng(path), path);
  };
  for (const Shot& s : shots) {
    auto p = Make(gBundle);
    p->Setup(f);
    if (s.knobs >= 0.)
      for (int id : knobs) p->SetPlain(id, s.knobs);
    Settle(*p);
    scaled(*p, s.percent);
    Window w(*p, s.screen);
    Pump(1.6);  // valves
    save(w, s.name);
  }
  auto p = Make(gBundle);
  p->Setup(f);
  scaled(*p, 100);
  Window w(*p, 2.);
  const double mixed[] = {6.5, 4.2, 3.8, 1.8, 7.2, 4.6, 3.5, 8.4, 6.1};
  for (int i = 0; i < 9; ++i) p->SetPlain(knobs[i], mixed[i]);
  p->SetPlain(magmora::kGateThreshold, -62.);
  const Audio x = Pluck(256 * 40, 48000., 0.3);
  const auto play = [&](double seconds) {
    const double until = Now() + seconds;
    while (Now() < until) {
      p->Run({x});
      Pump(0.02);
    }
  };
  play(1.5);
  const Spot k = gSpots["mid"];
  w.Down(k.x, k.y);
  w.HoldDrag(k.x, k.y, 24., kNone, 4);
  play(0.4);
  save(w, "2x-100-playing-drag");
  w.Up(k.x, k.y);
  w.Move(600., 520.);

  p->SetPlain(magmora::kChannel, 0.);
  p->SetPlain(magmora::kBright, 1.);
  p->SetPlain(magmora::kCrunch, 1.);
  p->SetPlain(magmora::kInputJack, 1.);
  Settle(*p, 1.2);
  save(w, "2x-100-rhythm-switches");
  p->SetPlain(magmora::kStandby, 0.);
  Settle(*p, 2.);
  save(w, "2x-100-standby");
  p->SetPlain(magmora::kPower, 0.);
  Settle(*p, 5.);
  save(w, "2x-100-power-off");
  p->SetPlain(magmora::kPower, 1.);
  p->SetPlain(magmora::kStandby, 1.);
  Settle(*p, 2.);
  w.Click(120., 19.);
  w.Move(120., 52. + 25. * 4 + 12.);
  save(w, "2x-100-menu-presets");
  w.Click(700., 560.);
  w.Click(1049., 19.);
  save(w, "2x-100-menu-engine");
  w.Click(400., 560.);
  // The transposer in the bar (added 2026-11-24): a shift and its mode, the hint lines, the engine menu's latency.
  p->SetPlain(magmora::kTranspose, 3.);
  p->SetPlain(magmora::kTransposeMode, 1.);
  Settle(*p, 0.5);
  w.Move(600., 520.);
  save(w, "2x-100-transpose-plus3-standard");
  w.Move(666., 19.);
  Settle(*p, 1.0);
  save(w, "2x-100-transpose-hint");
  w.Move(630., 36.);
  Settle(*p, 1.0);
  save(w, "2x-100-transpose-mode-hint");
  w.Move(600., 520.);
  w.Click(1049., 19.);
  save(w, "2x-100-menu-engine-transpose");
  w.Click(400., 560.);
  for (const Shot& t : {Shot{"1x-075", 1., 75, -1.}, Shot{"1x-100", 1., 100, -1.}, Shot{"1x-125", 1., 125, -1.},
                        Shot{"2x-075", 2., 75, -1.}, Shot{"2x-100", 2., 100, -1.}, Shot{"2x-125", 2., 125, -1.}}) {
    auto q = Make(gBundle);
    q->Setup(f);
    q->SetPlain(magmora::kTranspose, -2.);
    Settle(*q);
    scaled(*q, t.percent);
    Window wq(*q, t.screen);
    Pump(1.0);
    save(wq, std::string(t.name) + "-transpose");
  }
  p->SetPlain(magmora::kTranspose, 0.);
  p->SetPlain(magmora::kTransposeMode, 0.);
  Settle(*p, 0.3);
  w.Click(1254., 19.);
  save(w, "2x-100-about");
  w.Click(400., 560.);

  ChainOn(*p);
  p->SetPlain(magmora::kMic2Source, 0.);  // two pack microphones
  p->SetPlain(magmora::kEqHighPassSlope, 1.);
  p->SetPlain(magmora::kEqBand2Gain, -3.);
  p->SetPlain(magmora::kEqBand3Gain, 2.5);
  p->SetPlain(magmora::kMic1Position, 0.3);
  p->SetPlain(magmora::kMic2Level, -4.);
  p->SetPlain(magmora::kMic2Pan, 0.3);
  const std::pair<float, const char*> pages[] = {{kTabPedal, "pedal"}, {kTabEq, "eq"}, {kTabCab, "cab"}};
  for (const auto& page : pages) {
    w.Click(page.first, 19.);
    w.Move(640., 650.);
    Settle(*p, 0.3);
    save(w, std::string("2x-100-page-") + page.second);
  }
  w.Click(kTabCab, 19.);
  w.Click(122., 566.);  // slot 1's source, in its window
  Settle(*p, 0.2);
  save(w, "2x-100-page-cab-menu");
  w.Click(700., 600.);
  // The pack as a session restores it when the folder has gone: the error is on the page.
  Bytes s;
  p->Save(s);
  p->Load(WithPack(s, "/no/such/pack", "", gFixtures + "/room.wav"));
  Settle(*p, 0.3);
  save(w, "2x-100-page-cab-missing");
  // A new instance: the built-in cabinet.
  auto empty = Make(gBundle);
  empty->Setup(f);
  scaled(*empty, 100);
  Window w2(*empty, 1.);
  w2.Click(kTabCab, 19.);
  w2.Move(640., 650.);
  Settle(*empty, 0.3);
  save(w2, "1x-100-page-cab-default");
  w2.Click(593., 566.);  // the pack menu: the built-in cabinet, chosen
  Settle(*empty, 0.2);
  save(w2, "1x-100-page-cab-pack-menu");
  w2.Click(700., 600.);
  // Every block page at 1x as well, with the blocks on.
  {
    auto one = Make(gBundle);
    one->Setup(f);
    ChainOn(*one);
    one->SetPlain(magmora::kMic2Source, 0.);  // both slots on microphones
    one->SetPlain(magmora::kMic2On, 1.);
    one->SetPlain(magmora::kMic1Position, 0.3);
    scaled(*one, 100);
    Window w1(*one, 1.);
    for (const auto& page : pages) {
      w1.Click(page.first, 19.);
      w1.Move(640., 650.);
      Settle(*one, 0.3);
      save(w1, std::string("1x-100-page-") + page.second);
    }
  }
  // Every block page at 75 % and 150 % on a 2x screen, and 150 % on a 1x screen: chosen at 100 %, then the
  // session's window size is loaded with the window open, as a host restoring a session does.
  for (const auto& size : {std::pair<double, int>{2., 75}, {2., 150}, {1., 150}}) {
    for (const auto& page : pages) {
      auto one = Make(gBundle);
      one->Setup(f);
      ChainOn(*one);
      one->SetPlain(magmora::kMic2Source, 0.);
      scaled(*one, 100);
      Window ws(*one, size.first);
      Settle(*one, 0.2);
      ws.Click(page.first, 19.);
      Settle(*one, 0.2);
      scaled(*one, size.second);
      Settle(*one, 0.4);
      save(ws, std::string(size.first > 1.5 ? "2x-" : "1x-") + std::to_string(size.second) + "-page-" + page.second);
    }
  }
  // v0.1 UX: the preset menu with presets of the user's, naming and confirming in it, the tuner, the hint line and
  // the latched clip marks; at 75, 100 and 125 % on 1x and 2x screens.
  {
    FreshPresets();
    WritePresetFile("Wall Of Sound.json", "Wall Of Sound", 6.);
    WritePresetFile("Glass Clean.json", "Glass Clean", 2.);
    WritePresetFile("Club Lead.json", "Club Lead", 8.5, "Live");
    WritePresetFile("Night Rhythm.json", "Night Rhythm", 4.5, "Live");
    const Audio tone = Sine(48000 * 20, 48000., 82.41 * std::pow(2., -12. / 1200.), 0.25);
    for (const auto& size : {std::pair<double, int>{2., 100}, {1., 100}, {1., 75}, {2., 75}, {1., 125}, {2., 125}}) {
      auto u = Make(gBundle);
      u->Setup(f);
      scaled(*u, size.second);
      Window wu(*u, size.first);
      Settle(*u, 0.2);
      const std::string tag = std::string(size.first > 1.5 ? "2x-" : "1x-") + (size.second < 100 ? "0" : "") + std::to_string(size.second);
      wu.Click(120., 19.);
      wu.Move(kRightColumn, MenuRowY(4, kFactoryRows));  // Wall Of Sound, pointed at: its pencil and bin
      save(wu, tag + "-menu-presets-user");
      wu.Click(700., 610.);
      // the tuner, a low E 12 cents flat playing
      wu.Click(1222., 19.);
      size_t at = 0;
      for (double until = Now() + 1.2; Now() < until;) {
        u->Run({Audio(tone.begin() + static_cast<long>(at), tone.begin() + static_cast<long>(at) + 1536)});
        at += 1536;
        Pump(0.01);
      }
      save(wu, tag + "-tuner");
      wu.Click(640., 580.);
      // the hint line on Presence
      const Spot presence = gSpots["presence"];
      wu.Move(presence.x, presence.y);
      Settle(*u, 0.9);
      save(wu, tag + "-hint");
      wu.Move(640., 300.);
      if (size.first > 1.5 && size.second == 100) {
        wu.Click(120., 19.);
        wu.Click(kRightColumn, MenuRowY(1, kFactoryRows));  // Save as...: the name in the menu
        Settle(*u, 0.1);
        save(wu, tag + "-menu-presets-saveas");
        wu.Type("Wall Of Sound");
        Settle(*u, 0.2);
        save(wu, tag + "-menu-presets-replace");
        wu.Click(435., MenuRowY(1, kFactoryRows));  // Cancel
        wu.Click(700., 610.);
        wu.Click(120., 19.);
        wu.Click(74. + 2 * 232. - 12., MenuRowY(3, kFactoryRows));  // the bin of Glass Clean
        Settle(*u, 0.1);
        save(wu, tag + "-menu-presets-delete");
        wu.Click(700., 610.);
        // clip marks: a loud blast, then the meters fall back and the marks stay
        u->SetPlain(magmora::kInputTrim, 12.);
        u->SetPlain(magmora::kOutputLevel, 12.);
        for (int i = 0; i < 8; ++i) {
          u->Run({Sine(48000 / 4, 48000., 220., 0.9)});
          Pump(0.02);
        }
        u->SetPlain(magmora::kInputTrim, 0.);
        u->SetPlain(magmora::kOutputLevel, 0.);
        Settle(*u, 1.5);
        save(wu, tag + "-clip-marks");
        // tab names: EQ and Pedal off, the Pedal just switched on from its name
        wu.RightClick(kTabPedal, 19.);
        Pump(0.05);
        save(wu, tag + "-tab-flash");
      }
    }
    FreshPresets();
  }

  // A response file in the cabinet: it takes slot 2, which dims what it does not have yet.
  {
    auto ir = Make(gBundle);
    ir->Setup(f);
    ChainOn(*ir);
    ir->SetPlain(magmora::kMic2Pan, 0.25);
    scaled(*ir, 100);
    for (double screen : {2., 1.}) {
      Window wi(*ir, screen);
      wi.Click(kTabCab, 19.);
      wi.Move(640., 650.);
      Settle(*ir, 0.3);
      save(wi, screen > 1.5 ? "2x-100-page-cab-ir" : "1x-100-page-cab-ir");
      if (screen > 1.5) {
        wi.Click(832., 566.);  // slot 2's source menu, with its file
        Settle(*ir, 0.2);
        save(wi, "2x-100-page-cab-ir-menu");
      }
    }
  }
}

// ---- soak and cost ----------------------------------------------------------------------------------------------

// The render thread of a real host runs at real-time priority on a performance core. Without this the scheduler
// may put the test on an efficiency core, about three times slower, and the budget figures become its, not ours.
void LikeAnAudioThread() { pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0); }

void Soak(double seconds, unsigned seed) {
  LikeAnAudioThread();
  auto p = Make(gBundle);
  Format f;
  f.block = 64;
  p->Setup(f);
  // The whole chain: pedal, EQ, the fixture pack, slot 1 a microphone, slot 2 a response file. Every 750 blocks a
  // slot's source switches; every 3 000 blocks a session with other files in the slots is loaded, so the
  // files are read and handed to the audio thread while it runs.
  ChainOn(*p);
  Settle(*p, 0.3);
  Bytes base;
  p->Save(base);
  const std::string pack = gFixtures + "/pack", room = gFixtures + "/room.wav", dirac = gFixtures + "/dirac.wav";
  const std::vector<Bytes> sessions = {WithPack(base, pack, room, dirac), WithPack(base, pack, dirac, room),
                                       WithPack(base, pack, "", room), WithPack(base, pack, room, "")};
  long switches = 0, loads = 0;
  const Audio x = Pluck(48000 * 4, f.rate, 0.4);
  Audio l(64), r(64);
  long allocations = 0;
  std::mt19937 random(seed);
  std::uniform_real_distribution<double> unit(0., 1.);
  const long blocks = static_cast<long>(seconds * f.rate / 64);
  long bad = 0, over = 0;
  double worst = 0., busy = 0., peak = 0., worstCpu = 0.;
  long overCpu = 0;
  int lastMoved = -1;
  for (long b = 0; b < blocks; ++b) {
    if (b % 3000 == 1500) {
      p->Load(sessions[static_cast<size_t>(loads++ % 4)]);
    } else if (b % 750 == 0 && b > 0) {
      const int id = (switches++ % 2) ? magmora::kMic2Source : magmora::kMic1Source;
      p->SetPlain(id, p->Plain(id) >= 0.5 ? 0. : 1.);
    }
    if (b % 8 == 0) {
      const int id = static_cast<int>(random() % kAllParams);
      const magmora::ParamInfo& info = PI(id);
      // Power and standby stay mostly on: an amp that is off proves little.
      if (info.automatable) lastMoved = id;
      if (info.automatable) p->Set(id, (id == magmora::kPower || id == magmora::kStandby) ? (random() % 20 != 0) : unit(random), true);
    }
    const float* in[2] = {x.data() + (b * 64) % (x.size() - 64), x.data() + (b * 64) % (x.size() - 64)};
    float* out[2] = {l.data(), r.data()};
    const double start = Now(), cpuStart = ThreadCpu();
    const long before = magmora_test_allocations();
    gCountAllocations = true;
    p->Render(in, out, 64);
    gCountAllocations = false;
    allocations += magmora_test_allocations() - before;
    const double took = Now() - start, cpu = ThreadCpu() - cpuStart;
    worstCpu = std::max(worstCpu, cpu);
    if (cpu > 64. / f.rate && overCpu++ < 12) {
      printf("      over: block %ld, %.0f %%, last moved %s; trim %+.1f dB, pre lead %.1f, post lead %.1f, channel %.0f, "
             "presence %.1f, resonance %.1f, standby %.0f, power %.0f\n",
             b, 100. * cpu * f.rate / 64., lastMoved >= 0 ? PI(lastMoved).key : "-", p->Plain(magmora::kInputTrim),
             p->Plain(magmora::kPreLead), p->Plain(magmora::kPostLead), p->Plain(magmora::kChannel), p->Plain(magmora::kPresence),
             p->Plain(magmora::kResonance), p->Plain(magmora::kStandby), p->Plain(magmora::kPower));
    }
    busy += took;
    worst = std::max(worst, took);
    over += took > 64. / f.rate;
    bad += !Finite(l) || !Finite(r);
    peak = std::max(peak, Peak(l));
  }
  const double budget = 64. / f.rate;
  Check(!bad, Say("%.0f s of random automation of all %d parameters, whole chain on: finite", seconds, kAllParams),
        Say("%ld bad blocks, peak %.2f", bad, peak));
  Check(!allocations, "no allocation or free on the render thread",
        Say("%ld in %ld blocks, %ld source switches, %ld sessions with new files loaded", allocations, blocks, switches, loads));
  // Wall time includes whatever else the machine does; thread CPU time is the plug-in's own work.
  Check(!overCpu, "no block over real time in CPU time",
        Say("%ld of %ld over, worst %.0f %% of the budget", overCpu, blocks, 100. * worstCpu / budget));
  printf("      wall clock: %ld of %ld over, mean %.1f %%, worst %.0f %% of the budget\n", over, blocks,
         100. * busy / (blocks * budget), 100. * worst / budget);
}

void Cost() {
  LikeAnAudioThread();
  // The worst 64-sample block while one knob is swept 0 to 10 in 200 ms, best of five sweeps per knob (so that
  // the figure is the plug-in's work, not the scheduler's), in thread CPU time. Gate G3: at most 90 %.
  for (int id : {int(magmora::kPresence), int(magmora::kResonance), int(magmora::kPostLead), int(magmora::kHigh), int(magmora::kPreLead)}) {
    double best = 1e9;
    for (int trial = 0; trial < 5; ++trial) {
      auto p = Make(gBundle);
      Format f;
      f.block = 64;
      p->Setup(f);
      const Audio x = Pluck(64 * 400, f.rate, 0.3);
      Audio l(64), r(64);
      double worst = 0.;
      for (int b = 0; b < 400; ++b) {
        if (b >= 100 && b <= 250) p->Set(id, (b - 100) / 150., true);
        const float* in[2] = {x.data() + b * 64, x.data() + b * 64};
        float* out[2] = {l.data(), r.data()};
        const double start = ThreadCpu();
        p->Render(in, out, 64);
        if (b >= 100) worst = std::max(worst, ThreadCpu() - start);
      }
      best = std::min(best, worst);
    }
    printf("      worst block while %s turns: %.0f %% of a 64-sample block at 48 kHz\n", magmora::kParams[id].key,
           100. * best * 48000. / 64.);
  }
  for (int block : {1, 16, 64, 512}) {
    auto p = Make(gBundle);
    Format f;
    f.block = block;
    p->Setup(f);
    const Audio x = Pluck(48000 * 3, f.rate);
    const double start = Now();
    p->Run({x});
    printf("      48 kHz, blocks of %4d: %.1f %% of one core\n", block, 100. * (Now() - start) / 3.);
  }
}

}  // namespace

// ---- threads -----------------------------------------------------------------------------------------------------
// States loaded on a thread other than the main thread (hosts do), while the editor is open and audio runs. Run it
// under ThreadSanitizer too (plugin/scripts/build_test_host.sh with a sanitizer build).
void TestThreads() {
  Format f;
  f.block = 128;
  std::vector<Bytes> states;
  for (int k = 0; k < 4; ++k) {
    auto donor = Make(gBundle);
    donor->Setup(f);
    ChainOn(*donor, k % 2 == 0);
    donor->SetPlain(magmora::kPreLead, 2. + k);
    donor->Run({Audio(256)});
    Pump(0.05);
    Bytes b;
    donor->Save(b);
    states.push_back(k == 3 ? WithPack(b, gFixtures + "/pack", gFixtures + "/hall.wav", gFixtures + "/room.wav") : b);
  }
  {
    auto p = Make(gBundle);
    p->Setup(f);
    Pump(0.2);
    Window w(*p, 1.);
    std::atomic<bool> stop{false};
    std::atomic<long> bad{0}, blocks{0};
    std::atomic<int> refused{0};
    std::thread audio([&] {
      LikeAnAudioThread();
      Audio x(128, 0.01f), l(128), r(128);
      const float* in[2] = {x.data(), x.data()};
      float* out[2] = {l.data(), r.data()};
      while (!stop.load()) {
        p->Render(in, out, 128);
        for (float v : l) bad += std::isfinite(v) ? 0 : 1;
        ++blocks;
        std::this_thread::sleep_for(std::chrono::microseconds(700));
      }
    });
    std::atomic<bool> loading{true};
    std::thread loader([&] {
      for (int round = 0; round < 12; ++round) {
        if (!p->Load(states[static_cast<size_t>(round) % states.size()])) ++refused;
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
      }
      loading.store(false);
    });
    long renders = 0;
    while (loading.load()) {  // the editor draws (and polls the cabinet's references) meanwhile
      NSMutableArray* views = [NSMutableArray arrayWithObject:w.View()];
      while (views.count) {
        NSView* v = views.lastObject;
        [views removeLastObject];
        if ([v respondsToSelector:@selector(render)]) {
          [v performSelector:@selector(render)];
          ++renders;
        }
        [views addObjectsFromArray:v.subviews];
      }
      Pump(0.005);
    }
    loader.join();
    Pump(0.2);
    stop.store(true);
    audio.join();
    Bytes last;
    p->Save(last);
    Check(!refused.load() && !bad.load() && blocks.load() > 0 && renders > 0 && Contains(last, "room.wav"),
          "states loaded on a worker thread while the editor draws and audio runs",
          Say("%d refused, %ld non-finite, %ld blocks, %ld editor renders", refused.load(), bad.load(), blocks.load(), renders));
  }
  {
    // A host that loads a state on a worker thread and renders offline at once gets the session's sound, cabinet and
    // response files included: as an instance that loaded the same state on the main thread.
    Format offline = f;
    offline.offline = true;
    auto main = Make(gBundle), worker = Make(gBundle);
    main->Setup(offline);
    worker->Setup(offline);
    const Bytes& s = states[2];  // the fixture pack, and slot 2 playing its response file
    const bool a = main->Load(s);
    bool b = false;
    std::thread([&] { b = worker->Load(s); }).join();
    const Audio x = Pluck(9600, 48000.);
    const auto ym = main->Run({x}), yw = worker->Run({x});
    Check(a && b && Difference(ym[0], yw[0]) == 0. && Difference(ym[1], yw[1]) == 0. && Peak(ym[0]) > 1e-3,
          "a state loaded on a worker thread, then rendered offline at once, sounds as one loaded on the main thread",
          Say("%.1f dBFS", Db(std::max(Difference(ym[0], yw[0]), Difference(ym[1], yw[1])))));
  }
}

int main(int argc, const char** argv) {
  @autoreleasepool {
    if (argc < 3) {
      fprintf(stderr, "usage: %s bundle audio|state|automation|ui|presets|smoke-presets|shots DIR|soak SECONDS|cost|threads\n", argv[0]);
      return 2;
    }
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyProhibited];
    InstallHooks();
    gBundle = argv[1];
    MakeFixtures(argv[0]);
    const std::string command = argv[2];
    double load[1] = {0.};
    getloadavg(load, 1);
    printf("== %s %s (load average %.1f)\n", [[@(argv[1]) lastPathComponent] UTF8String], command.c_str(), load[0]);
    if (command == "audio") {
      TestAudio();
      TestTransposeAudio();
    } else if (command == "state") {
      TestState();
      TestTransposeState();
    } else if (command == "automation") {
      TestAutomation();
      TestTransposeAutomation();
    }
    else if (command == "ui")
      TestUi();
    else if (command == "smoke-presets")
      SmokePresets();
    else if (command == "presets") {
      ReadLayout();
      TestUserPresets();
    }
    else if (command == "shots" && argc > 3)
      Shots(argv[3]);
    else if (command == "soak" && argc > 3)
      Soak(atof(argv[3]), argc > 4 ? static_cast<unsigned>(atoi(argv[4])) : 11u);
    else if (command == "cost")
      Cost();
    else if (command == "threads")
      TestThreads();
    else
      return 2;
    printf("-- %d passed, %d failed\n", gPassed, gFailed);
  }
  return gFailed ? 1 : 0;
}
