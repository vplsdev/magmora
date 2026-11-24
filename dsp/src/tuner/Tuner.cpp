#include <magmora/Tuner.h>

#include <algorithm>
#include <cmath>

namespace magmora {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr uint64_t kMask = TunerFeed::kCapacity - 1;
static_assert((TunerFeed::kCapacity & (TunerFeed::kCapacity - 1)) == 0, "a power of two");

}  // namespace

TunerFeed::TunerFeed() : ring_(new std::atomic<float>[kCapacity]) {
  for (int i = 0; i < kCapacity; ++i) ring_[static_cast<size_t>(i)].store(0.f, std::memory_order_relaxed);
}

TunerFeed::~TunerFeed() = default;

void TunerFeed::prepare(double hostRate) {
  if (!(hostRate > 0.0)) return;
  factor_ = std::max(1, static_cast<int>(std::lround(hostRate / kTargetRate)));
  phase_ = 0;
  const double out = hostRate / factor_;
  // A 4th-order Butterworth low-pass at 0.4 of the decimated rate (two RBJ sections), against aliasing.
  const double fc = std::min(0.4 * out, 0.45 * hostRate);
  const double qs[2] = {0.54119610014619698, 1.30656296487637652};
  for (int s = 0; s < 2; ++s) {
    Biquad& b = lowpass_[s];
    b = Biquad{};
    if (factor_ == 1) continue;  // nothing to remove
    const double w = 2.0 * kPi * fc / hostRate, alpha = std::sin(w) / (2.0 * qs[s]), c = std::cos(w);
    const double a0 = 1.0 + alpha;
    b.b0 = (1.0 - c) / 2.0 / a0;
    b.b1 = (1.0 - c) / a0;
    b.b2 = b.b0;
    b.a1 = -2.0 * c / a0;
    b.a2 = (1.0 - alpha) / a0;
  }
  written_.store(0, std::memory_order_relaxed);
  claimed_.store(0, std::memory_order_relaxed);
  rate_.store(out, std::memory_order_release);
}

void TunerFeed::push(const double* x, int n, double gain) noexcept {
  if (!x || n <= 0 || rate_.load(std::memory_order_relaxed) <= 0.0) return;
  // Seqlock-style: announce the slots about to be overwritten before writing them, publish after.
  uint64_t w = written_.load(std::memory_order_relaxed);
  claimed_.store(w + static_cast<uint64_t>(n / factor_ + 1), std::memory_order_relaxed);
  std::atomic_thread_fence(std::memory_order_release);
  for (int i = 0; i < n; ++i) {
    double v = std::isfinite(x[i]) ? x[i] * gain : 0.0;
    v = lowpass_[1].run(lowpass_[0].run(v));
    if (++phase_ < factor_) continue;
    phase_ = 0;
    ring_[w & kMask].store(static_cast<float>(v), std::memory_order_relaxed);
    ++w;
  }
  // Denormals cannot build up in the filter's state: it decays to exact zeros through the multiplies only slowly.
  for (Biquad& b : lowpass_) {
    if (std::fabs(b.z1) < 1e-30) b.z1 = 0.0;
    if (std::fabs(b.z2) < 1e-30) b.z2 = 0.0;
  }
  written_.store(w, std::memory_order_release);
}

bool TunerFeed::latest(float* out, int n) const noexcept {
  if (!out || n <= 0 || n > kCapacity) return false;
  const uint64_t end = written_.load(std::memory_order_acquire);
  if (end < static_cast<uint64_t>(n)) return false;
  const uint64_t start = end - static_cast<uint64_t>(n);
  for (int i = 0; i < n; ++i) out[i] = ring_[(start + static_cast<uint64_t>(i)) & kMask].load(std::memory_order_relaxed);
  std::atomic_thread_fence(std::memory_order_acquire);
  // Every slot the writer could have touched meanwhile lies within kCapacity of where the copy started, or it wrapped.
  return claimed_.load(std::memory_order_relaxed) - start <= static_cast<uint64_t>(TunerFeed::kCapacity);
}

PitchDetector::PitchDetector(double minHz, double maxHz) : minHz_(minHz), maxHz_(maxHz) {}

int PitchDetector::windowSamples(double rate) const {
  const int lag = static_cast<int>(std::ceil(rate / minHz_)) + 2;
  return lag + std::max(lag, static_cast<int>(std::lround(0.12 * rate)));
}

Pitch PitchDetector::detect(const float* in, int n, double rate) {
  Pitch result;
  if (!in || n < 16 || !(rate > 0.0)) return result;
  const int maxLag = std::min(static_cast<int>(std::ceil(rate / minHz_)) + 2, n / 2);
  const int minLag = std::max(2, static_cast<int>(std::floor(rate / maxHz_)));
  const int width = n - maxLag;  // the integration window
  if (maxLag <= minLag + 2 || width < 8) return result;
  x_.assign(in, in + n);
  double mean = 0.0;
  for (double v : x_) mean += v;
  mean /= n;
  double energy = 0.0;
  for (double& v : x_) {
    v -= mean;
    energy += v * v;
  }
  result.rms = std::sqrt(energy / n);
  if (!(result.rms >= kSilence)) return result;

  // YIN: difference function and its cumulative mean normalisation.
  d_.assign(static_cast<size_t>(maxLag) + 1, 0.0);
  cmnd_.assign(static_cast<size_t>(maxLag) + 1, 1.0);
  const double* x = x_.data();
  double running = 0.0;
  for (int tau = 1; tau <= maxLag; ++tau) {
    double sum = 0.0;
    for (int j = 0; j < width; ++j) {
      const double e = x[j] - x[j + tau];
      sum += e * e;
    }
    d_[static_cast<size_t>(tau)] = sum;
    running += sum;
    cmnd_[static_cast<size_t>(tau)] = running > 0.0 ? sum * tau / running : 1.0;
  }
  int tau = -1;
  for (int t = minLag; t < maxLag; ++t) {
    if (cmnd_[static_cast<size_t>(t)] < 0.15) {
      while (t + 1 < maxLag && cmnd_[static_cast<size_t>(t) + 1] < cmnd_[static_cast<size_t>(t)]) ++t;
      tau = t;
      break;
    }
  }
  if (tau < 0) {  // nothing clearly periodic: the best dip, if it is good enough
    int best = minLag;
    for (int t = minLag; t < maxLag; ++t)
      if (cmnd_[static_cast<size_t>(t)] < cmnd_[static_cast<size_t>(best)]) best = t;
    if (cmnd_[static_cast<size_t>(best)] > 0.35) return result;
    tau = best;
  }
  if (tau <= minLag) tau = minLag + 1;
  result.clarity = std::min(std::max(1.0 - cmnd_[static_cast<size_t>(tau)], 0.0), 1.0);
  const auto parabola = [](double a, double b, double c) {
    const double den = a - 2.0 * b + c;
    return std::fabs(den) > 1e-18 ? std::min(std::max(0.5 * (a - c) / den, -1.0), 1.0) : 0.0;
  };
  double period = tau + parabola(cmnd_[static_cast<size_t>(tau) - 1], cmnd_[static_cast<size_t>(tau)],
                                 cmnd_[static_cast<size_t>(tau) + 1]);

  // Refinement: the normalised square difference around multiples of the period (2, 4, 8, ... up to half the
  // window), each step narrowing the last: an interpolation error at a lag of m periods divides by m.
  const int reach = n / 2;
  for (int m = 2; m * period <= reach - 2; m *= 2) {
    const int centre = static_cast<int>(std::lround(m * period));
    const int w = n - (centre + 2);
    if (w <= 8 || centre - 2 <= 0) break;
    double nsdf[5];
    for (int k = -2; k <= 2; ++k) {
      const int lag = centre + k;
      double r = 0.0, e = 0.0;
      for (int j = 0; j < w; ++j) {
        r += x[j] * x[j + lag];
        e += x[j] * x[j] + x[j + lag] * x[j + lag];
      }
      nsdf[k + 2] = e > 0.0 ? 2.0 * r / e : 0.0;
    }
    int peak = 1;
    for (int k = 2; k <= 3; ++k)
      if (nsdf[k] > nsdf[peak]) peak = k;
    if (nsdf[peak] < nsdf[peak - 1] || nsdf[peak] < nsdf[peak + 1]) break;  // no peak where the period says
    const double refined = (centre + peak - 2 + parabola(nsdf[peak - 1], nsdf[peak], nsdf[peak + 1])) / m;
    if (std::fabs(refined - period) > 1.5 / m) break;
    period = refined;
  }
  if (!(period > 0.0)) return result;
  result.hz = rate / period;
  result.found = result.hz >= minHz_ * 0.95 && result.hz <= maxHz_ * 1.05;
  return result;
}

NoteReading nearestNote(double hz, double a4Hz) {
  NoteReading r;
  if (!(hz > 0.0) || !(a4Hz > 0.0)) return r;
  const double semis = 12.0 * std::log2(hz / a4Hz);
  const double nearest = std::round(semis);
  r.midi = 69 + static_cast<int>(nearest);
  r.cents = 100.0 * (semis - nearest);
  return r;
}

const char* noteName(int midi) {
  static const char* const kNames[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
  return kNames[((midi % 12) + 12) % 12];
}

int noteOctave(int midi) { return (midi >= 0 ? midi / 12 : (midi - 11) / 12) - 1; }

}  // namespace magmora
