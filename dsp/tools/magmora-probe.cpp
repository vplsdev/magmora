// magmora-probe: the circuit clusters at an internal rate, with no resampling.
#include <magmora/Engine.h>

#include <chrono>

#include "ToolSupport.h"
#include "graph/AmpGraph.h"

namespace {

const char* kUsage =
    "magmora-probe --in x.f64 --fs RATE [--set key=value ...] --probes p1b,nX,p3a,spk,b,sc --out PREFIX\n"
    "  writes PREFIX.<probe>.f32 (little endian, one value per input sample; the input is in volts at the jack)\n"
    "  --f64                 write float64 (PREFIX.<probe>.f64)\n"
    "  --closed-form         closed-form device laws instead of tables\n"
    "  --tol V               Newton tolerance (product: 1e-4; prototype and golden data: 1e-6)\n"
    "  --prototype-newton    no early accept (the prototype's Newton)\n"
    "  --fixed-fallbacks     the prototype's former fixed fallbacks instead of the adaptive continuation\n"
    "  --delayed-coupling    delayed load currents at the cuts instead of companion coupling\n"
    "  --dc                  print the DC operating point of every node as JSON and exit\n"
    "  --values NAME         value set: prototype (default) or as-drawn\n"
    "  --partition split|merged\n"
    "  --cluster NAME        one cluster alone between ideal sources; the input drives its signal input (absolute V)\n"
    "  --labels a,b          with --cluster: outputs to write, by label (v_out, spk, ...)\n"
    "  --stats               solver statistics per cluster on stderr\n";

}  // namespace

int main(int argc, char** argv) {
  using namespace magmora;
  if (argc == 2 && std::strcmp(argv[1], "--version") == 0) {
    std::printf("magmora-probe %s model %s\n", versionString(), modelHash());
    return 0;
  }
  tools::Args args(argc, argv);
  std::string inPath, outPrefix, cluster;
  std::vector<std::string> labels;
  std::vector<std::pair<std::string, std::string>> settings;
  graph::GraphOptions options;
  bool f64 = false, dc = false, stats = false;
  while (!args.empty()) {
    const std::string a = args.take("option");
    if (a == "--in") inPath = args.take("--in");
    else if (a == "--fs") options.sampleRate = args.number("--fs");
    else if (a == "--set") args.settings(settings);
    else if (a == "--probes") options.probes = tools::split(args.take("--probes"));
    else if (a == "--out") outPrefix = args.take("--out");
    else if (a == "--f64") f64 = true;
    else if (a == "--closed-form") options.laws = graph::Laws::Closed;
    else if (a == "--tol") options.solver.tolerance = args.number("--tol");
    else if (a == "--prototype-newton") options.solver.fastAccept = false;
    else if (a == "--fixed-fallbacks") options.solver.fallback = graph::Fallback::Prototype;
    else if (a == "--delayed-coupling") options.companion = false;
    else if (a == "--dc") dc = true;
    else if (a == "--stats") stats = true;
    else if (a == "--cluster") cluster = args.take("--cluster");
    else if (a == "--labels") labels = tools::split(args.take("--labels"));
    else if (a == "--values") {
      options.valueSet = circuit::findValueSet(args.take("--values").c_str());
      if (options.valueSet < 0) tools::fail("no such value set");
    }
    else if (a == "--partition") {
      const std::string p = args.take("--partition");
      if (p != "split" && p != "merged") tools::fail("--partition split|merged");
      options.partition = p == "split" ? circuit::Partition::Split : circuit::Partition::Merged;
    } else if (a == "--help" || a == "-h") {
      std::fputs(kUsage, stdout);
      return 0;
    } else {
      std::fputs(kUsage, stderr);
      tools::fail("unknown option " + a);
    }
  }
  if (!(options.sampleRate > 0.0)) tools::fail("--fs must be positive");

  circuit::Controls controls;
  for (const auto& [key, value] : settings)
    if (!controls.set(key.c_str(), value.c_str())) tools::fail("bad setting " + key + "=" + value);

  graph::AmpGraph amp;
  if (!amp.prepare(options, controls)) tools::fail("the operating point was not found");

  if (dc) {
    std::printf("{");
    for (size_t k = 0; k < amp.dcNodes().size(); ++k)
      std::printf("%s\"%s\": %.17g", k ? ", " : "", amp.dcNodes()[k].c_str(), amp.dcVoltages()[k]);
    std::printf("}\n");
    return 0;
  }
  if (inPath.empty() || outPrefix.empty()) {
    std::fputs(kUsage, stderr);
    tools::fail("--in and --out are required");
  }

  std::vector<std::string> names;
  std::vector<const double*> signals;
  if (!cluster.empty()) {
    const int k = amp.amp().find(cluster);
    if (k < 0) tools::fail("no cluster " + cluster);
    const bool first = amp.amp().clusters[static_cast<size_t>(k)].findInput("cut_in") < 0;
    if (!amp.isolate(cluster, first ? "in" : "cut_in")) tools::fail("cluster " + cluster + " has no signal input");
    for (const std::string& label : labels) {
      const double* p = amp.output(cluster, label);
      if (!p) tools::fail("cluster " + cluster + " has no output " + label);
      names.push_back(label);
      signals.push_back(p);
    }
  } else {
    if (options.probes.empty()) tools::fail("--probes is required");
    for (const std::string& name : options.probes) {
      const double* p = amp.probe(name);
      if (!p) tools::fail("no node " + name);
      names.push_back(name);
      signals.push_back(p);
    }
  }

  const std::vector<double> input = tools::readF64(inPath);
  std::vector<std::vector<double>> recorded(signals.size(), std::vector<double>(input.size()));
  const auto start = std::chrono::steady_clock::now();
  for (size_t t = 0; t < input.size(); ++t) {
    amp.processSample(input[t]);
    for (size_t s = 0; s < signals.size(); ++s) recorded[s][t] = *signals[s];
  }
  const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

  for (size_t s = 0; s < signals.size(); ++s) {
    const std::string base = outPrefix + "." + names[s];
    if (f64) tools::writeRaw(base + ".f64", recorded[s]);
    else {
      std::vector<float> narrow(recorded[s].size());
      for (size_t i = 0; i < narrow.size(); ++i) narrow[i] = static_cast<float>(recorded[s][i]);
      tools::writeRaw(base + ".f32", narrow);
    }
  }
  if (stats && !input.empty()) {
    const double n = static_cast<double>(input.size());
    std::fprintf(stderr, "%.1f ns/sample\n", elapsed / n * 1e9);
    for (int k = 0; k < amp.size(); ++k) {
      const graph::Cluster& c = amp.cluster(k);
      const graph::ClusterStats s = c.stats();
      const auto& m = c.system().matrices;
      std::fprintf(stderr,
                   "cluster %s: ports %d states %d inputs %d outputs %d  it/sample %.2f evals %.2f lu %.2f  "
                   "retries %ld fallbacks %ld non-finite %ld; evaluations per recovery: %.0f in the failed solve, %.0f "
                   "after it, worst %ld\n",
                   amp.amp().names[static_cast<size_t>(k)].c_str(), m.ports, m.states, m.inputs, m.outputs,
                   s.iterations / n, s.evaluations / n, s.factorisations / n, s.retries, s.fallbacks, s.nonFinite,
                   s.retries ? static_cast<double>(s.failedEvaluations) / s.retries : 0.0,
                   s.retries ? static_cast<double>(s.recoveryEvaluations) / s.retries : 0.0, s.worstRecovery);
    }
  }
  if (amp.fallbacks() || amp.nonFinite())
    std::fprintf(stderr, "solver fallbacks %ld, non-finite resets %ld\n", amp.fallbacks(), amp.nonFinite());
  return 0;
}
