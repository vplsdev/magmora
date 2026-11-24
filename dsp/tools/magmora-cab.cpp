// The cabinet's virtual microphone (XP6), for comparison with proto/cab/virtualmic.py and for timing.
//   magmora-cab --names list.txt [--mapping m.json]          one JSON line per name: what parse_name reads
//   magmora-cab --interp a.wav b.wav --t T [--method morph|snap] [--phase minimum|aligned|raw] --out y.f64
//   magmora-cab --two a.wav b.wav [--set level1=.. pan1=.. invert1=.. level2=.. pan2=.. invert2=.. align=.. offset=..]
//              [--phase ...] --out prefix                    prefix.L.f64, prefix.R.f64 and the measured delay
//   magmora-cab --pack DIR [--mapping m.json] [--rate R] [--set cab key=value ...] [--source1 SPEAKER/VARIANT]
//              [--source2 ...] --out prefix                 the response the worker builds (before the pack gain)
//   magmora-cab --pack DIR --index                            index time, memory, microphones and tracks
//   magmora-cab --pack DIR --bench [--rate R]                 build time per response over position moves
//   magmora-cab --resample file.wav --rate R --out y.f64      the file's first channel as the cabinet resamples it
// Files are float64 little-endian. Main thread only: no audio runs here.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#include <magmora/Chain.h>

#include "EngineSupport.h"
#include "ToolSupport.h"
#include "cab/Morph.h"
#include "cab/Positions.h"
#include "cab/VirtualMic.h"
#include "cab/UserIr.h"
#include "cab/Wav.h"

using namespace magmora;

namespace {

std::vector<double> readMono(const std::string& path, double& rate) {
  cab::Audio a;
  std::string error;
  if (!cab::readWavFile(path.c_str(), a, &error)) tools::fail(path + ": " + error);
  rate = a.rate;
  return std::vector<double>(a.channels[0].begin(), a.channels[0].end());
}

cab::morph::PackPhase phaseOf(const std::string& s) {
  if (s == "minimum") return cab::morph::PackPhase::Minimum;
  if (s == "raw") return cab::morph::PackPhase::Raw;
  return cab::morph::PackPhase::Aligned;
}

std::string jsonText(const std::string& s) {
  std::string out = "\"";
  for (char c : s) {
    if (c == '"' || c == '\\') out += '\\';
    out += c;
  }
  return out + "\"";
}

void printMic(const CabinetMicStatus& m) {
  std::printf("{\"on\": %s, \"found\": %s, \"mic\": %s, \"speaker\": %s, \"variant\": %s, \"kind\": %d, \"angle\": %g, "
              "\"x_mm\": %.6f, \"dist_mm\": %.6f, \"path_index\": %.6f, \"morph_x\": %s, \"morph_dist\": %s, \"files\": [",
              m.on ? "true" : "false", m.found ? "true" : "false", jsonText(m.mic).c_str(), jsonText(m.speaker).c_str(),
              jsonText(m.variant).c_str(), m.kind, m.angleDeg, m.xMm, m.distMm, m.pathIndex, m.morphX ? "true" : "false",
              m.morphDist ? "true" : "false");
  for (size_t k = 0; k < m.files.size(); ++k) std::printf("%s%s", k ? ", " : "", jsonText(m.files[k]).c_str());
  std::printf("]}");
}

}  // namespace

int main(int argc, char** argv) {
  if (argc == 2 && std::strcmp(argv[1], "--version") == 0) {
    tools::printVersion("magmora-cab");
    return 0;
  }
  tools::Args args(argc, argv);
  std::string names, mapping, out, method = "morph", phase = "minimum", pack, a, b;
  std::string source[2];
  double t = 0.5, rate = 0.0;
  bool index = false, bench = false, two = false;
  std::vector<std::pair<std::string, std::string>> settings;
  while (!args.empty()) {
    const std::string o = args.take("option");
    if (o == "--names") names = args.take("--names");
    else if (o == "--mapping") mapping = args.take("--mapping");
    else if (o == "--out") out = args.take("--out");
    else if (o == "--interp" || o == "--two") {
      two = o == "--two";
      a = args.take("first file");
      b = args.take("second file");
      if (!two) method = method.empty() ? "morph" : method;
    } else if (o == "--t") t = args.number("--t");
    else if (o == "--method") method = args.take("--method");
    else if (o == "--phase") phase = args.take("--phase");
    else if (o == "--pack") pack = args.take("--pack");
    else if (o == "--rate") rate = args.number("--rate");
    else if (o == "--set") args.settings(settings);
    else if (o == "--source1") source[0] = args.take("--source1");
    else if (o == "--source2") source[1] = args.take("--source2");
    else if (o == "--index") index = true;
    else if (o == "--resample") a = "resample:" + args.take("--resample");
    else if (o == "--bench") bench = true;
    else tools::fail("unknown option " + o);
  }

  if (!names.empty()) {
    cab::Mapping m;
    if (!mapping.empty()) {
      std::ifstream in(mapping, std::ios::binary);
      const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
      std::string error;
      if (!cab::parseMapping(text, m, &error)) tools::fail(error);
    }
    std::ifstream in(names);
    for (std::string line; std::getline(in, line);) {
      if (line.empty()) continue;
      const cab::Position p = cab::parseName(line, mapping.empty() ? nullptr : &m);
      std::printf("{\"name\": %s, \"source\": %s", jsonText(line).c_str(), jsonText(p.source).c_str());
      if (!p.mic.empty()) std::printf(", \"mic\": %s", jsonText(p.mic).c_str());
      if (!p.speaker.empty()) std::printf(", \"speaker\": %s", jsonText(p.speaker).c_str());
      if (!p.variant.empty()) std::printf(", \"variant\": %s", jsonText(p.variant).c_str());
      if (p.hasX) std::printf(", \"x_mm\": %.12g", p.xMm);
      if (p.hasDist) std::printf(", \"dist_mm\": %.12g", p.distMm);
      if (p.hasAngle) std::printf(", \"angle_deg\": %.12g", p.angleDeg);
      if (p.hasZone) std::printf(", \"zone\": %.12g", p.zone);
      if (p.hasStep) std::printf(", \"step\": %d", p.step);
      std::printf("}\n");
    }
    return 0;
  }

  if (a.rfind("resample:", 0) == 0) {  // a file as the cabinet resamples it to --rate (first channel, float32)
    cab::Audio in;
    std::string error;
    if (!cab::readWavFile(a.substr(9).c_str(), in, &error)) tools::fail(error);
    const std::vector<float> r = cab::resample(in.channels[0], in.rate, rate);
    tools::writeRaw(out, std::vector<double>(r.begin(), r.end()));
    return 0;
  }
  if (!a.empty() && !two) {
    double ra = 0, rb = 0;
    const std::vector<double> x = readMono(a, ra), y = readMono(b, rb);
    if (ra != rb) tools::fail("the two files differ in rate");
    const auto t0 = std::chrono::steady_clock::now();
    const std::vector<double> h = cab::morph::interpolate(
        x, y, t, ra, method == "snap" ? cab::morph::Method::Snap : cab::morph::Method::Morph, phase == "minimum");
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    tools::writeRaw(out, h);
    std::printf("%zu samples, %.2f ms\n", h.size(), ms);
    return 0;
  }

  if (two) {
    double ra = 0, rb = 0;
    const std::vector<double> x = readMono(a, ra), y = readMono(b, rb);
    if (ra != rb) tools::fail("the two files differ in rate");
    cab::morph::MicMix m1, m2;
    bool align = true;
    double offset = 0.0;
    for (const auto& [k, v] : settings) {
      const double d = std::stod(v);
      if (k == "level1") m1.levelDb = d;
      else if (k == "pan1") m1.pan = d;
      else if (k == "invert1") m1.invert = d >= 0.5;
      else if (k == "level2") m2.levelDb = d;
      else if (k == "pan2") m2.pan = d;
      else if (k == "invert2") m2.invert = d >= 0.5;
      else if (k == "align") align = d >= 0.5;
      else if (k == "offset") offset = d;
      else tools::fail("unknown setting " + k);
    }
    const cab::morph::TwoMics r = cab::morph::twoMics(x, y, ra, m1, m2, align, offset, phaseOf(phase));
    tools::writeRaw(out + ".L.f64", r.left);
    tools::writeRaw(out + ".R.f64", r.right);
    std::printf("measured %.4f samples, polarity %d, moved %.6f samples\n", r.measuredDelay, r.detectedPolarity, r.moved);
    return 0;
  }

  if (pack.empty()) tools::fail("nothing to do (see the header of magmora-cab.cpp)");
  cab::Pack p;
  std::string error;
  const auto t0 = std::chrono::steady_clock::now();
  if (!p.load(pack, mapping, &error)) tools::fail(error);
  const double loadMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  if (rate <= 0.0) rate = p.files().front().rate;

  if (index) {
    std::printf("%zu files (%d skipped) indexed and decoded in %.0f ms; %.1f MB of responses; phase %s%s; pack gain %.4g\n",
                p.files().size(), p.skipped(), loadMs, static_cast<double>(p.bytes()) / 1e6,
                p.phase() == cab::morph::PackPhase::Minimum ? "minimum" : (p.phase() == cab::morph::PackPhase::Raw ? "raw" : "aligned"),
                p.phaseDeclared() ? " (declared)" : " (default)", p.gain());
    for (size_t m = 0; m < p.mics().size(); ++m) {
      std::printf("mic %zu %s:", m, cab::Pack::micLabel(p.mics()[m]));
      for (const std::string& s : p.speakers(static_cast<int>(m))) {
        std::printf(" speaker '%s' [", s.c_str());
        for (const std::string& v : p.variants(static_cast<int>(m), s)) {
          std::printf(" '%s' angles", v.c_str());
          for (double ang : p.angles(static_cast<int>(m), s, v)) std::printf(" %g", ang);
          std::printf(";");
        }
        std::printf(" ]");
      }
      std::printf("\n");
    }
    return 0;
  }

  double controls[cab::kCtlCount];
  for (int id = kCabOn; id <= kMicOffset; ++id) controls[id - kCabOn] = Chain::parameterInfo(id)->def;
  controls[cab::kCtlOn] = 1.0;
  for (const auto& [k, v] : settings) {
    const int id = Chain::findParameter(k.c_str());
    if (id < kCabOn || id > kMicOffset) tools::fail("not a cabinet parameter: " + k);
    controls[id - kCabOn] = std::stod(v);
  }
  std::string speakers[2], variants[2];
  const cab::VirtualMic::SlotFile noFiles[2];
  for (int m = 0; m < 2; ++m) {
    const size_t slash = source[m].find('/');
    speakers[m] = source[m].substr(0, slash);
    variants[m] = slash == std::string::npos ? "" : source[m].substr(slash + 1);
  }

  if (bench) {  // build time over position moves of microphone 1 (microphone 2 as set), first build excluded
    std::mt19937 rng(5);
    std::uniform_real_distribution<double> u(0.0, 1.0);
    std::vector<double> ms;
    for (int k = 0; k < 41; ++k) {
      controls[cab::kCtlMic1Position] = u(rng);
      const auto s0 = std::chrono::steady_clock::now();
      CabinetStatus st;
      const auto r = cab::VirtualMic::build(&p, cab::VirtualMic::settingsFrom(controls), speakers, variants, noFiles, rate, st, false);
      if (k > 0) ms.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - s0).count());
      if (r.left.empty()) tools::fail("empty response");
    }
    std::sort(ms.begin(), ms.end());
    std::printf("build (one morphing microphone%s) at %.0f Hz: median %.2f ms, worst %.2f ms over %zu moves; pack index %.0f ms\n",
                controls[cab::kCtlMic2On] >= 0.5 ? " and a second" : "", rate, ms[ms.size() / 2], ms.back(), ms.size(), loadMs);
    return 0;
  }

  CabinetStatus st;
  const auto s0 = std::chrono::steady_clock::now();
  const cab::morph::TwoMics r = cab::VirtualMic::build(&p, cab::VirtualMic::settingsFrom(controls), speakers, variants, noFiles, rate, st, false);
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - s0).count();
  if (!out.empty()) {
    tools::writeRaw(out + ".L.f64", r.left);
    tools::writeRaw(out + ".R.f64", r.right);
  }
  std::printf("{\"build_ms\": %.3f, \"samples\": %zu, \"delay\": %.6f, \"polarity\": %d, \"pack_gain\": %.10g, \"mic\": [",
              ms, r.left.size(), st.measuredDelaySamples, st.detectedPolarity, p.gain());
  printMic(st.mic[0]);
  std::printf(", ");
  printMic(st.mic[1]);
  std::printf("]}\n");
  return 0;
}
