// magmora-render: exactly what the plug-in runs.
#include <magmora/Chain.h>

#include <algorithm>
#include <vector>

#include "EngineSupport.h"
#include "WavFile.h"

namespace {

const char* kUsage =
    "magmora-render in.wav out.wav [--set key=value ...] [--rate R] [--block N] [--quality standard|high|ultra]\n"
    "              [--in-volts V] [--values NAME] [--chain]\n"
    "  The output is the speaker voltage / 100 at output_level 0, as float32, latency included (reported on stderr).\n"
    "  --rate R     treat the input samples as being at R Hz (no conversion)\n"
    "  --in-volts   volts at the jack for a full-scale input sample (default 2)\n"
    "  --values     value set: prototype (default) or as-drawn\n"
    "  --chain      through magmora::Chain with every block around the head off (gate G-X1)\n"
    "  --chain-set  key=value ... chain parameters (implies --chain)\n";

}  // namespace

int main(int argc, char** argv) {
  using namespace magmora;
  if (argc == 2 && std::strcmp(argv[1], "--version") == 0) {
    tools::printVersion("magmora-render");
    return 0;
  }
  if (argc < 3) {
    std::fputs(kUsage, argc == 2 && std::strcmp(argv[1], "--help") == 0 ? stdout : stderr);
    return argc == 2 && std::strcmp(argv[1], "--help") == 0 ? 0 : 2;
  }
  tools::Args args(argc, argv);
  const std::string inPath = args.take("in.wav"), outPath = args.take("out.wav");
  tools::Settings settings;
  double rate = 0.0;
  long block = 512;
  Calibration calibration;
  int values = magmora::Engine::defaultValueSet();
  bool throughChain = false;
  tools::Settings chainSettings;  // --chain: through magmora::Chain, every block around the head at its default (off)
  while (!args.empty()) {
    const std::string a = args.take("option");
    if (a == "--set") args.settings(settings);
    else if (a == "--rate") rate = args.number("--rate");
    else if (a == "--block") block = static_cast<long>(args.number("--block"));
    else if (a == "--quality") settings.emplace_back("quality", args.take("--quality"));
    else if (a == "--in-volts") calibration.inputVoltsPerFS = args.number("--in-volts");
    else if (a == "--values") values = tools::valueSetIndex(args.take("--values"));
    else if (a == "--chain") throughChain = true;
    else if (a == "--chain-set") {
      args.settings(chainSettings);
      throughChain = true;
    }
    else tools::fail("unknown option " + a);
  }
  if (block < 1 || block > 1 << 20) tools::fail("--block out of range");
  if (!(calibration.inputVoltsPerFS > 0.0)) tools::fail("--in-volts must be positive");

  tools::Audio audio = tools::readWav(inPath);
  if (audio.channels.size() > 2) tools::fail("one or two channels");
  if (rate > 0.0) audio.rate = rate;

  Chain chain;
  Engine& engine = chain.engine();
  for (const auto& [key, value] : settings) tools::setParameter(engine, key, value);
  engine.setCalibration(calibration);
  engine.setValueSet(values);
  for (const auto& [key, value] : chainSettings) {
    const int id = Chain::findParameter(key.c_str());
    if (id < kNumParams) tools::fail("not a chain parameter: " + key);
    chain.setParameter(id, std::stod(value));
  }
  const int channels = static_cast<int>(audio.channels.size());
  const bool prepared = throughChain ? chain.prepare(audio.rate, static_cast<int>(block), channels)
                                     : engine.prepare(audio.rate, static_cast<int>(block), channels);
  if (!prepared) tools::fail("unsupported sample rate");

  const size_t frames = audio.frames();
  std::vector<float> spare(static_cast<size_t>(block));
  for (size_t at = 0; at < frames; at += static_cast<size_t>(block)) {
    float* io[2] = {audio.channels[0].data() + at, channels > 1 ? audio.channels[1].data() + at : nullptr};
    const int n = static_cast<int>(std::min(frames - at, static_cast<size_t>(block)));
    if (throughChain) {
      float* out[2] = {io[0], channels > 1 ? io[1] : spare.data()};  // a mono file keeps the left output
      chain.process(io, out, n);
    } else {
      engine.process(io, io, n);
    }
  }
  tools::writeWav(outPath, audio);
  std::fprintf(stderr, "latency %d samples, solver fallbacks %u, values %s\n", engine.latencySamples(),
               static_cast<unsigned>(engine.meters().solverFallbacks.load()), tools::valueSetDescription(values).c_str());
  return 0;
}
