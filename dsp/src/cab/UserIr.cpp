#include "cab/UserIr.h"

#include <algorithm>
#include <cfloat>
#include <cmath>

namespace magmora::cab {
namespace {

constexpr double kPi = 3.14159265358979323846;

double besselI0(double x) {
  double sum = 1.0, term = 1.0;
  for (int k = 1; k < 50; ++k) {
    term *= (x / (2.0 * k)) * (x / (2.0 * k));
    sum += term;
    if (term < 1e-17 * sum) break;
  }
  return sum;
}

// A new response catches up on at most kCatchUpPerFrame input samples per sample of the block, and never more than
// kCatchUpPerBlock in one block: the work per block stays proportional to the block (a one-sample block catches up on
// four samples, not 256). Net progress is three samples per block sample, so the longest backlog (kMaxCatchUpSeconds)
// takes at most a twelfth of a second.
constexpr int kCatchUpPerBlock = 256;
constexpr int kCatchUpPerFrame = 4;
constexpr double kMaxCatchUpSeconds = 0.25;  // older than that, it starts with no history

// Measured (magmora-ir --bench, 96 kHz): a 40 ms response as a 3840-tap direct FIR cost 4.2 % of a core, a 2 s one
// partitioned 1.7 %. So the direct form is not worth its cost for long responses here: only responses up to 512 taps
// run direct, everything longer is partitioned.
int directTaps(double) { return 512; }

}  // namespace

bool finiteSamples(const std::vector<float>& x) {
  for (float v : x)
    if (!std::isfinite(v)) return false;
  return true;
}

float narrowSample(double v) { return std::isfinite(v) ? static_cast<float>(std::clamp(v, -double{FLT_MAX}, double{FLT_MAX})) : 0.0f; }

std::vector<float> resample(const std::vector<float>& x, double from, double to) {
  if (from == to || x.empty()) return x;
  constexpr int kZeros = 64;
  constexpr double kBeta = 10.0;
  const double ratio = to / from, cutoff = 0.95 * std::min(1.0, ratio);  // in units of the input Nyquist
  const double half = kZeros / cutoff;                                   // half-width in input samples
  const size_t frames = static_cast<size_t>(std::ceil(static_cast<double>(x.size()) * ratio));
  std::vector<float> y(frames);
  const double norm = besselI0(kBeta);
  auto weight = [&](double d) {
    const double u = d / half;
    const double window = besselI0(kBeta * std::sqrt(std::max(0.0, 1.0 - u * u))) / norm;
    const double arg = kPi * cutoff * d;
    const double sinc = std::fabs(arg) < 1e-12 ? 1.0 : std::sin(arg) / arg;
    return cutoff * sinc * window;
  };
  // Whole-number rates in a small ratio (44.1 to 48 kHz: 147 / 160): the output positions fall on a few fractional
  // phases, so the taps are computed once per phase (the cabinet resamples a whole pack). Same formula.
  const long a = std::lround(from), b = std::lround(to);
  if (static_cast<double>(a) == from && static_cast<double>(b) == to && a > 0 && b > 0) {
    long g = a, h = b;
    while (h) {
      const long r = g % h;
      g = h;
      h = r;
    }
    const long M = a / g, L = b / g;  // input position of output m: m M / L
    if (L <= 4096) {
      const long first = static_cast<long>(std::ceil(-half)) - 1, count = static_cast<long>(std::floor(half)) - first + 2;
      std::vector<double> taps(static_cast<size_t>(L * count));
      for (long phase = 0; phase < L; ++phase) {
        const double f = static_cast<double>(phase) / static_cast<double>(L);
        for (long j = 0; j < count; ++j) {
          const double d = f - static_cast<double>(first + j);
          taps[static_cast<size_t>(phase * count + j)] = std::fabs(d) <= half ? weight(d) : 0.0;
        }
      }
      for (size_t m = 0; m < frames; ++m) {
        const long long pos = static_cast<long long>(m) * M;
        const long q = static_cast<long>(pos / L), phase = static_cast<long>(pos % L);
        const double* w = taps.data() + phase * count;
        double acc = 0.0;
        const long lo = std::max(0L, q + first), hi = std::min(static_cast<long>(x.size()) - 1, q + first + count - 1);
        for (long n = lo; n <= hi; ++n) acc += x[static_cast<size_t>(n)] * w[n - q - first];
        y[m] = narrowSample(acc);
      }
      return y;
    }
  }
  for (size_t m = 0; m < frames; ++m) {
    const double t = static_cast<double>(m) / ratio;  // position in input samples
    const long first = static_cast<long>(std::ceil(t - half)), last = static_cast<long>(std::floor(t + half));
    double acc = 0.0;
    for (long n = std::max(first, 0L); n <= last && n < static_cast<long>(x.size()); ++n)
      acc += x[static_cast<size_t>(n)] * weight(t - static_cast<double>(n));
    y[m] = narrowSample(acc);
  }
  return y;
}

UserIr::~UserIr() {
  delete warming_;
  delete current_;
  delete old_;
  delete pending_.exchange(nullptr);
  delete retired_.exchange(nullptr);
}

void UserIr::prepare(double hostRate, int maxBlockSize, bool stereoInput) {
  rate_ = hostRate;
  maxBlock_ = maxBlockSize;
  stereoInput_ = stereoInput;
  for (auto* v : {in_, a_, b_})
    for (int c = 0; c < 2; ++c) v[c].assign(static_cast<size_t>(maxBlockSize), 0.0f);
  level_.prepare(hostRate);
  step_ = 1.0 / std::max(1.0, kSwapSeconds * hostRate);
  normStep_ = step_;
  {
    size_t cap = 1;
    while (static_cast<double>(cap) < kMaxResponseSeconds * hostRate + 16384.0) cap <<= 1;
    for (auto& r : ring_) r.assign(cap, 0.0f);
    ringMask_ = cap - 1;
    written_.store(0);
    resetAt_.store(0);
    catchIn_.assign(kCatchUpPerBlock, 0.0f);
    catchOut_.assign(kCatchUpPerBlock, 0.0f);
  }
  delete warming_;
  warming_ = nullptr;
  delete current_;
  delete old_;
  delete pending_.exchange(nullptr);
  delete retired_.exchange(nullptr);
  old_ = nullptr;
  current_ = haveSource_ ? build(source_) : nullptr;
  swap_ = 1.0;
  on_ = false;
  wet_ = 0.0;
  gain_ = -1.0;
  normWeight_ = normalize() ? 1.0 : 0.0;
}

UserIr::Kernel* UserIr::build(const Audio& source) {
  auto* k = new Kernel;
  std::vector<std::vector<float>> ir;
  for (const auto& channel : source.channels) ir.push_back(resample(channel, source.rate, rate_));
  double energy = 0.0;
  for (const auto& c : ir) {
    double e = 0.0;
    for (float v : c) e += static_cast<double>(v) * v;
    energy = std::max(energy, e);
  }
  // load() refuses samples that are not finite, so the energy is finite; a tiny one must not overflow the gain
  k->normGain = energy > 0.0 && std::isfinite(energy) ? narrowSample(1.0 / std::sqrt(energy)) : 1.0f;
  k->identity = false;
  const bool stereoIr = ir.size() == 2;
  k->lanes = stereoIr || stereoInput_ ? 2 : 1;
  for (int lane = 0; lane < k->lanes; ++lane) {
    const auto& h = ir[stereoIr ? static_cast<size_t>(lane) : 0];
    k->conv[lane].build(h.data(), h.size(), directTaps(rate_));
  }
  built_ = ir;
  return k;
}

void UserIr::prime(Kernel* k) {
  if (!k || k->identity || ring_[0].empty()) return;
  const uint64_t to = written_.load(std::memory_order_acquire);
  uint64_t taps = 0;
  for (const auto& h : built_) taps = std::max<uint64_t>(taps, h.size());
  const uint64_t room = ringMask_ + 1 - 16384;  // the audio thread writes on while this reads
  uint64_t from = to > taps ? to - taps : 0;
  from = std::max({from, resetAt_.load(std::memory_order_acquire), to > room ? to - room : uint64_t{0}});
  std::vector<float> in(4096), out(4096);
  for (int lane = 0; lane < k->lanes; ++lane) {
    const std::vector<float>& src = ring_[stereoInput_ ? lane : 0];
    for (uint64_t at = from; at < to;) {
      const int n = static_cast<int>(std::min<uint64_t>(4096, to - at));
      for (int i = 0; i < n; ++i) in[static_cast<size_t>(i)] = src[static_cast<size_t>((at + static_cast<uint64_t>(i)) & ringMask_)];
      k->conv[lane].process(in.data(), out.data(), n);
      at += static_cast<uint64_t>(n);
    }
  }
  k->fed = to;
  k->primed = true;
}

void UserIr::install(Kernel* k) {
  prime(k);
  delete pending_.exchange(k, std::memory_order_acq_rel);  // a kernel the audio thread never took
  collect();
}

bool UserIr::load(const Audio& source, std::string* error) {
  if (source.channels.empty() || source.frames() == 0 || !(source.rate > 0.0)) {
    if (error) *error = "empty response";
    return false;
  }
  if (static_cast<double>(source.frames()) / source.rate > kMaxResponseSeconds) {
    if (error) *error = "response longer than 10 s";
    return false;
  }
  for (const auto& c : source.channels)
    if (!finiteSamples(c)) {
      if (error) *error = "the response has samples that are not finite";
      return false;
    }
  source_ = source;
  if (source_.channels.size() > 2) source_.channels.resize(2);
  haveSource_ = true;
  if (maxBlock_ > 0) install(build(source_));
  return true;
}

bool UserIr::loadFile(const char* path, std::string* error) {
  Audio a;
  return readWavFile(path, a, error) && load(a, error);
}

void UserIr::clear() {
  haveSource_ = false;
  source_ = Audio{};
  built_.clear();
  if (maxBlock_ > 0) install(new Kernel);  // an identity kernel: the slot crossfades to the dry signal
}

void UserIr::collect() { delete retired_.exchange(nullptr, std::memory_order_acq_rel); }

double UserIr::responseSeconds() const {
  return haveSource_ ? static_cast<double>(source_.frames()) / source_.rate : 0.0;
}

int UserIr::partitionSize() const { return current_ && !current_->identity ? current_->conv[0].partitionSize() : 0; }

void UserIr::setControls(bool on, double levelDb) {
  if (on && !on_ && wet_ == 0.0) {
    level_.snap(levelDb);
    gain_ = -1.0;
    if (current_)
      for (int lane = 0; lane < current_->lanes; ++lane) current_->conv[lane].reset();
    resetAt_.store(written_.load(std::memory_order_relaxed), std::memory_order_release);  // nothing heard while off
  }
  on_ = on;
  level_.setTarget(levelDb);
}

void UserIr::reset() {
  // A reset starts afresh: a response being crossfaded in, catching up or handed over and not yet taken becomes the
  // current one at once, without a crossfade (from the dry signal or the previous response). So what follows a reset
  // does not depend on how far the building thread had got. The one it replaces goes back through retired_.
  if (!retired_.load(std::memory_order_acquire)) {
    if (old_) {  // a crossfade under way: finish it
      retired_.store(old_, std::memory_order_release);
      old_ = nullptr;
    } else if (Kernel* next = warming_ ? warming_ : pending_.exchange(nullptr, std::memory_order_acq_rel)) {
      if (current_) retired_.store(current_, std::memory_order_release);
      current_ = next;
      warming_ = nullptr;
    }
    swap_ = 1.0;
  }
  for (Kernel* k : {current_, warming_})
    if (k)
      for (int lane = 0; lane < k->lanes; ++lane) k->conv[lane].reset();
  const uint64_t now = written_.load(std::memory_order_relaxed);
  if (warming_) warming_->fed = now;
  resetAt_.store(now, std::memory_order_release);
}

void UserIr::runKernel(Kernel* k, int frames, std::vector<float>* out) {
  const size_t n = static_cast<size_t>(frames);
  if (!k || k->identity) {
    for (int c = 0; c < 2; ++c) std::copy_n(in_[c].begin(), n, out[c].begin());
    return;
  }
  for (int lane = 0; lane < k->lanes; ++lane)
    k->conv[lane].process(in_[stereoInput_ ? lane : 0].data(), out[lane].data(), frames);
  if (k->lanes == 1) std::copy_n(out[0].begin(), n, out[1].begin());
}

template <class Sample>
void UserIr::run(Sample* left, Sample* right, int frames) {
  Sample* io[2] = {left, right};
  const size_t n = static_cast<size_t>(frames);
  for (int c = 0; c < 2; ++c)
    for (size_t i = 0; i < n; ++i) in_[c][i] = static_cast<float>(io[c][i]);
  const uint64_t blockStart = written_.load(std::memory_order_relaxed);
  if (!ring_[0].empty()) {  // the input history new responses are fed
    for (int c = 0; c < 2; ++c)
      for (size_t i = 0; i < n; ++i) ring_[c][static_cast<size_t>((blockStart + i) & ringMask_)] = in_[c][i];
    written_.store(blockStart + n, std::memory_order_release);
  }
  if (!old_ && !warming_ && !retired_.load(std::memory_order_acquire))
    warming_ = pending_.exchange(nullptr, std::memory_order_acq_rel);
  if (warming_) {
    Kernel* k = warming_;
    if (k->primed && !k->identity) {
      uint64_t backlog = blockStart > k->fed ? blockStart - k->fed : 0;
      if (backlog > static_cast<uint64_t>(kMaxCatchUpSeconds * rate_)) {  // stalled too long: start afresh
        for (int lane = 0; lane < k->lanes; ++lane) k->conv[lane].reset();
        k->fed = blockStart;
        backlog = 0;
      }
      const int allowance = std::min(kCatchUpPerBlock, kCatchUpPerFrame * frames);
      const int chunk = static_cast<int>(std::min<uint64_t>(backlog, static_cast<uint64_t>(allowance)));
      for (int lane = 0; lane < k->lanes && chunk > 0; ++lane) {
        const std::vector<float>& src = ring_[stereoInput_ ? lane : 0];
        for (int i = 0; i < chunk; ++i)
          catchIn_[static_cast<size_t>(i)] = src[static_cast<size_t>((k->fed + static_cast<uint64_t>(i)) & ringMask_)];
        k->conv[lane].process(catchIn_.data(), catchOut_.data(), chunk);
      }
      k->fed += static_cast<uint64_t>(chunk);
    }
    if (!k->primed || k->identity || k->fed >= blockStart) {  // caught up: it takes this block's input and fades in
      old_ = current_;
      current_ = k;
      warming_ = nullptr;
      swap_ = 0.0;
      // from no response (old_ null) the crossfade starts from the dry signal: runKernel(null) is the dry path
    }
  }
  const bool swapping = swap_ < 1.0;
  runKernel(current_, frames, a_);
  if (swapping) runKernel(old_, frames, b_);
  const double normTarget = normalize() ? 1.0 : 0.0;
  const double normA = current_ && !current_->identity ? current_->normGain : 1.0;
  const double normB = old_ && !old_->identity ? old_->normGain : 1.0;
  for (size_t i = 0; i < n; ++i) {
    double w = 1.0;
    if (swap_ < 1.0) {
      w = 0.5 - 0.5 * std::cos(kPi * swap_);
      swap_ = std::min(1.0, swap_ + step_);
    }
    if (level_.moving() || gain_ < 0.0) gain_ = std::pow(10.0, level_.next() / 20.0);
    if (normWeight_ != normTarget)  // the normalisation switch is a gain change: ramped like a swap
      normWeight_ = normTarget > normWeight_ ? std::min(normTarget, normWeight_ + normStep_)
                                             : std::max(normTarget, normWeight_ - normStep_);
    const double gA = 1.0 + normWeight_ * (normA - 1.0), gB = 1.0 + normWeight_ * (normB - 1.0);
    const double fade = 0.5 - 0.5 * std::cos(kPi * wet_);
    for (int c = 0; c < 2; ++c) {
      const double dry = static_cast<double>(io[c][i]);
      double wet = gA * a_[c][i];
      if (swapping) wet = gB * b_[c][i] + w * (wet - gB * b_[c][i]);
      io[c][i] = static_cast<Sample>(dry + fade * (gain_ * wet - dry));
    }
    if (on_ ? wet_ < 1.0 : wet_ > 0.0) wet_ = std::clamp(wet_ + (on_ ? step_ : -step_), 0.0, 1.0);
  }
  if (old_ && swap_ >= 1.0) {
    retired_.store(old_, std::memory_order_release);
    old_ = nullptr;
  }
}

// A host may pass more frames than prepare's maximum (the scratch buffers' size): such a block runs in pieces.
void UserIr::process(float* left, float* right, int frames) {
  if (idle() || maxBlock_ <= 0) return;
  for (int at = 0; at < frames; at += maxBlock_) run(left + at, right + at, std::min(maxBlock_, frames - at));
}
void UserIr::process(double* left, double* right, int frames) {
  if (idle() || maxBlock_ <= 0) return;
  for (int at = 0; at < frames; at += maxBlock_) run(left + at, right + at, std::min(maxBlock_, frames - at));
}

}  // namespace magmora::cab
