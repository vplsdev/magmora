// The user response-file slot alone (XP10).
//   magmora-ir --bench --rate R --seconds S [--channels 1|2] [--stereo-input]   cost per block, steady and worst
//   magmora-ir --render --ir response.wav --in in.wav --out out.wav [--no-normalize]
//       the input (its rate is the host rate) through the slot: resampled response, zero latency
//   magmora-ir --kernel --ir response.wav --rate R --out kernel.f64   the response as convolved, channel after channel
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <random>
#include <vector>

#include "EngineSupport.h"
#include "ToolSupport.h"
#include "WavFile.h"
#include "cab/UserIr.h"

using namespace magmora;

namespace {

#if defined(__APPLE__)
#include <pthread.h>
#endif

// CPU time of this thread where the platform has it (POSIX); wall-clock time elsewhere (Windows).
double threadSeconds() {
#if defined(CLOCK_THREAD_CPUTIME_ID)
  timespec t;
  clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
  return static_cast<double>(t.tv_sec) + 1e-9 * static_cast<double>(t.tv_nsec);
#else
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
#endif
}

}  // namespace

int main(int argc, char** argv) {
  if (argc == 2 && std::strcmp(argv[1], "--version") == 0) {
    tools::printVersion("magmora-ir");
    return 0;
  }
#if defined(__APPLE__)
  pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
  tools::Args args(argc, argv);
  std::string mode, irPath, inPath, outPath;
  double rate = 48000.0, seconds = 0.5;
  int channels = 1;
  bool stereoInput = false, normalize = true;
  while (!args.empty()) {
    const std::string a = args.take("option");
    if (a == "--bench" || a == "--render" || a == "--kernel") mode = a;
    else if (a == "--rate") rate = args.number("--rate");
    else if (a == "--seconds") seconds = args.number("--seconds");
    else if (a == "--channels") channels = static_cast<int>(args.number("--channels"));
    else if (a == "--stereo-input") stereoInput = true;
    else if (a == "--no-normalize") normalize = false;
    else if (a == "--ir") irPath = args.take("--ir");
    else if (a == "--in") inPath = args.take("--in");
    else if (a == "--out") outPath = args.take("--out");
    else tools::fail("unknown option " + a);
  }
  constexpr int kBlock = 64;
  cab::UserIr slot;
  std::string error;
  if (mode == "--bench") {
    cab::Audio response;
    response.rate = rate;
    std::mt19937 rng(1);
    std::normal_distribution<double> g(0.0, 1.0);
    const size_t length = static_cast<size_t>(seconds * rate);
    for (int c = 0; c < channels; ++c) {
      response.channels.emplace_back(length);
      for (size_t n = 0; n < length; ++n)
        response.channels.back()[n] = static_cast<float>(g(rng) * std::exp(-6.9 * static_cast<double>(n) / length));
    }
    slot.prepare(rate, kBlock, stereoInput);
    if (!slot.load(response, &error)) tools::fail(error);
    slot.setControls(true, 0.0);
    const long blocks = static_cast<long>(10.0 * rate / kBlock);
    std::vector<float> l(kBlock), r(kBlock);
    double total = 0.0, worst = 0.0;
    for (long b = 0; b < blocks; ++b) {
      for (int n = 0; n < kBlock; ++n) l[static_cast<size_t>(n)] = r[static_cast<size_t>(n)] = static_cast<float>(g(rng) * 0.1);
      const double t0 = threadSeconds();
      slot.setControls(true, 0.0);
      slot.process(l.data(), r.data(), kBlock);
      const double spent = threadSeconds() - t0;
      if (b > 100) worst = std::max(worst, spent);
      total += spent;
    }
    slot.collect();
    const double budget = kBlock / rate;
    std::printf("%.2f s response, %d channel(s)%s at %.0f Hz: partition %d, %.2f %% of a core, slowest block %.1f %% of "
                "its time (10 s, 64-sample blocks, thread time)\n",
                seconds, channels, stereoInput ? ", stereo input" : "", rate, slot.partitionSize(),
                100.0 * total / (blocks * budget), 100.0 * worst / budget);
    return 0;
  }
  if (mode == "--kernel") {
    slot.prepare(rate, kBlock, false);
    if (!slot.loadFile(irPath.c_str(), &error)) tools::fail(error);
    FILE* f = std::fopen(outPath.c_str(), "wb");
    if (!f) tools::fail("cannot write " + outPath);
    for (const auto& c : slot.kernelResponse()) {
      std::vector<double> d(c.begin(), c.end());
      std::fwrite(d.data(), sizeof(double), d.size(), f);
    }
    std::fclose(f);
    std::printf("%zu channel(s), %zu samples\n", slot.kernelResponse().size(), slot.kernelResponse().front().size());
    return 0;
  }
  if (mode == "--render") {
    tools::Audio audio = tools::readWav(inPath);
    slot.prepare(audio.rate, kBlock, audio.channels.size() == 2);
    slot.setNormalize(normalize);
    if (!slot.loadFile(irPath.c_str(), &error)) tools::fail(error);
    slot.setControls(true, 0.0);
    std::vector<float> l(kBlock), r(kBlock);
    // the slot fades in over 20 ms from the dry signal: run it in on silence first, as a host would have
    for (int b = 0; b < static_cast<int>(audio.rate * 0.05 / kBlock); ++b) {
      std::fill(l.begin(), l.end(), 0.0f);
      std::fill(r.begin(), r.end(), 0.0f);
      slot.setControls(true, 0.0);
      slot.process(l.data(), r.data(), kBlock);
    }
    slot.reset();
    const size_t frames = audio.frames();
    for (size_t at = 0; at < frames; at += kBlock) {
      const int n = static_cast<int>(std::min<size_t>(kBlock, frames - at));
      for (int i = 0; i < n; ++i) {
        l[static_cast<size_t>(i)] = audio.channels[0][at + static_cast<size_t>(i)];
        r[static_cast<size_t>(i)] = audio.channels.size() > 1 ? audio.channels[1][at + static_cast<size_t>(i)] : l[static_cast<size_t>(i)];
      }
      slot.setControls(true, 0.0);
      slot.process(l.data(), r.data(), n);
      for (int i = 0; i < n; ++i) {
        audio.channels[0][at + static_cast<size_t>(i)] = l[static_cast<size_t>(i)];
        if (audio.channels.size() > 1) audio.channels[1][at + static_cast<size_t>(i)] = r[static_cast<size_t>(i)];
      }
    }
    tools::writeWav(outPath, audio);
    return 0;
  }
  tools::fail("one of --bench, --render, --kernel");
}
