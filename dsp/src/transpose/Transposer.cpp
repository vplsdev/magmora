#include "transpose/Transposer.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "engine/DenormalGuard.h"

namespace magmora {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kTwoPi = 2.0 * kPi;
constexpr float kMaxInput = 1000.0f;   // +60 dBFS: far beyond any instrument, small enough for float spectra
constexpr float kTinyInput = 1e-20f;   // -400 dBFS: taken as silence
constexpr double kTightMs = 256.0 / 48.0;     // target delay 5.33 ms (255 samples at 48 kHz: three hops of 85)
constexpr double kStandardMs = 512.0 / 48.0;  // target delay 10.67 ms (512 samples at 48 kHz: four hops of 128)
constexpr int kBaseAnalysis = 2048;           // at 44.1 and 48 kHz
constexpr int kTightScale = 1, kTightOverlap = 3, kTightPad = 2;           // N 2048, hop 85 (L 255), FFT 4096 at 48 kHz
constexpr int kStandardScale = 1, kStandardOverlap = 4, kStandardPad = 1;  // N 2048, hop 128 (L 512), FFT 2048

inline float clean(float x) noexcept {
  const float m = std::fabs(x);
  if (m <= kMaxInput) return m < kTinyInput ? 0.0f : x;  // false for NaN
  if (x > 0.0f) return kMaxInput;
  if (x < 0.0f) return -kMaxInput;
  return 0.0f;  // NaN
}

// atan2 to about 1e-5 rad (Abramowitz and Stegun 4.4.49 on [0, 1], then the octant); finite for finite arguments,
// 0 for (0, 0).
inline float fastAtan2(float y, float x) noexcept {
  const float ax = std::fabs(x), ay = std::fabs(y);
  const float hi = std::max(ax, ay), lo = std::min(ax, ay);
  if (!(hi > 0.0f)) return 0.0f;
  const float z = lo / hi, z2 = z * z;
  float a = z * (0.9998660f + z2 * (-0.3302995f + z2 * (0.1801410f + z2 * (-0.0851330f + z2 * 0.0208351f))));
  if (ay > ax) a = 1.57079632679f - a;
  if (x < 0.0f) a = 3.14159265359f - a;
  return y < 0.0f ? -a : a;
}

// sin and cos of x in [-pi, pi] to about 3e-7: a quarter turn by the nearest multiple of pi/2, then Taylor to x^8.
inline void fastSinCos(float x, float& s, float& c) noexcept {
  const float q = std::nearbyint(x * 0.636619772f);
  const float r = (x - q * 1.57079637f) + q * 4.37113883e-8f;  // pi/2 in two parts
  const float r2 = r * r;
  const float sr = r * (1.0f + r2 * (-1.66666667e-1f + r2 * (8.33333333e-3f + r2 * -1.98412698e-4f)));
  const float cr = 1.0f + r2 * (-0.5f + r2 * (4.16666667e-2f + r2 * (-1.38888889e-3f + r2 * 2.48015873e-5f)));
  switch (static_cast<int>(q) & 3) {
    case 0: s = sr; c = cr; break;
    case 1: s = cr; c = -sr; break;
    case 2: s = -sr; c = -cr; break;
    default: s = -cr; c = sr; break;
  }
}

inline double wrapPhase(double x) noexcept { return x - kTwoPi * std::nearbyint(x / kTwoPi); }

int nextPow2(int n) {
  int p = 1;
  while (p < n) p <<= 1;
  return p;
}

}  // namespace

Transposer::Transposer() = default;
Transposer::~Transposer() = default;

void Transposer::setSemitones(int semitones) noexcept {
  target_.store(std::clamp(semitones, kMinSemitones, kMaxSemitones), std::memory_order_relaxed);
}

Transposer::Config Transposer::configFor(Mode mode) const {
  Config c;
  const double octaves = std::log2(rate_ / 48000.0);
  const int shiftOct = static_cast<int>(std::lround(octaves));
  const int scale = options_.analysisScale > 0 ? options_.analysisScale : (mode == Mode::Tight ? kTightScale : kStandardScale);
  int n = kBaseAnalysis * scale;
  n = shiftOct >= 0 ? n << shiftOct : n >> -shiftOct;
  c.N = std::max(256, n);
  c.M = c.N * (options_.pad > 0 ? options_.pad : (mode == Mode::Tight ? kTightPad : kStandardPad));
  c.K = c.M / 2;
  const int o = options_.overlap > 0 ? std::max(2, options_.overlap) : (mode == Mode::Tight ? kTightOverlap : kStandardOverlap);
  const double ms = mode == Mode::Tight ? kTightMs : kStandardMs;
  int h = static_cast<int>(std::lround(ms * 1e-3 * rate_ / o));
  h = std::clamp(h, 8, c.N / (2 * o));
  c.H = h;
  c.L = h * o;
  return c;
}

void Transposer::prepare(double sampleRate, int /*maxBlock*/) {
  rate_ = std::isfinite(sampleRate) ? std::clamp(sampleRate, 8000.0, 768000.0) : 48000.0;
  const Config a = configFor(Mode::Tight), b = configFor(Mode::Standard);
  const int N = std::max(a.N, b.N), M = std::max(a.M, b.M), K = M / 2;
  const int L = std::max(a.L, b.L), H = std::max(a.H, b.H);

  ring_.assign(static_cast<size_t>(nextPow2(2 * N)), 0.0f);
  ringMask_ = ring_.size() - 1;
  // one FFT per mode (their sizes may differ; switching modes must not allocate)
  ffts_[0].prepare(a.M / 2);
  ffts_[1].prepare(b.M / 2);
  twiddleRe_.assign(static_cast<size_t>(K), 0.0f);
  twiddleIm_.assign(static_cast<size_t>(K), 0.0f);
  analysisWindow_.assign(static_cast<size_t>(N), 0.0f);
  const int skirt = std::max(skirtFor(a), skirtFor(b));
  tableRe_.assign(static_cast<size_t>(2 * skirt * kTableStep + 2), 0.0f);
  tableIm_.assign(static_cast<size_t>(2 * skirt * kTableStep + 2), 0.0f);
  synthesisWindow_.assign(static_cast<size_t>(L), 0.0f);
  frame_.assign(static_cast<size_t>(M), 0.0f);
  for (auto* v : {&zr_, &zi_, &re_, &im_, &prevRe_, &prevIm_, &mag_, &prevMag_, &yr_, &yi_, &omega_, &cre_, &cim_, &fitKappa_,
                  &kappaMemo_, &nextKappaMemo_, &lowRe_, &lowIm_, &lowKappa_})
    v->assign(static_cast<size_t>(K), 0.0f);
  peaks_.assign(static_cast<size_t>(K), 0);
  peakFlag_.assign(static_cast<size_t>(K), 0);
  regionEnd_.assign(static_cast<size_t>(K), 0);
  estPeak_.assign(static_cast<size_t>(K), 0);
  estBin_.assign(static_cast<size_t>(K), 0);
  nextEstBin_.assign(static_cast<size_t>(K), 0);
  for (Voice& v : voices_) {
    v.theta.assign(static_cast<size_t>(K), 0.0f);
    v.nextTheta.assign(static_cast<size_t>(K), 0.0f);
    v.acc.assign(static_cast<size_t>(L), 0.0f);
    v.ready.assign(static_cast<size_t>(H), 0.0f);
  }
  fadeLength_ = std::max(1, static_cast<int>(std::lround(kFadeMs * 1e-3 * rate_)));
  fadeIn_.assign(static_cast<size_t>(fadeLength_), 0.0f);
  fadeOut_.assign(static_cast<size_t>(fadeLength_), 0.0f);
  for (int j = 0; j < fadeLength_; ++j) {
    // equal power (sin^2 + cos^2 = 1) along a raised-cosine path: no corner at either end, so the fade itself adds
    // less above the tone than a plain quarter-sine (the chain's switch test, 2026-11-24)
    const double x = 0.5 * kPi * (0.5 - 0.5 * std::cos(kPi * (j + 1) / fadeLength_));
    fadeIn_[static_cast<size_t>(j)] = static_cast<float>(std::sin(x));
    fadeOut_[static_cast<size_t>(j)] = j + 1 == fadeLength_ ? 0.0f : static_cast<float>(std::cos(x));
  }
  configure();
  reset();
}

void Transposer::setMode(Mode mode) {
  mode_ = mode;
  if (ring_.empty()) return;
  configure();
  reset();
}

void Transposer::configure() {
  config_ = configFor(mode_);
  fft_ = &ffts_[mode_ == Mode::Tight ? 0 : 1];
  for (int k = 0; k < config_.K; ++k) {
    twiddleRe_[static_cast<size_t>(k)] = static_cast<float>(std::cos(kTwoPi * k / config_.M));
    twiddleIm_[static_cast<size_t>(k)] = static_cast<float>(-std::sin(kTwoPi * k / config_.M));
  }
  const int N = config_.N, L = config_.L, H = config_.H, half = L / 2, rise = N - half;
  // product window p(j) = Hann over the newest L samples; it overlap-adds to overlap / 2 at hop H
  const auto product = [L](int j) { return std::pow(std::sin(kPi * (j + 0.5) / L), 2.0); };
  for (int n = 0; n < N; ++n) {
    double a;
    if (n < rise) {
      const double u = (n + 0.5) / rise;
      switch (options_.riseShape) {
        case 1: a = 0.42 - 0.5 * std::cos(kPi * u) + 0.08 * std::cos(2.0 * kPi * u); break;
        case 2: a = std::sin(0.5 * kPi * u); break;
        default: a = std::pow(std::sin(0.5 * kPi * u), 2.0); break;
      }
    } else {
      a = std::pow(product(n - (N - L)), options_.fallExponent);
    }
    analysisWindow_[static_cast<size_t>(n)] = static_cast<float>(a);
  }
  double ola = 0.0;
  for (int j = 0; j < L; j += H) ola += product(j);
  for (int j = 0; j < L; ++j)
    synthesisWindow_[static_cast<size_t>(j)] =
        static_cast<float>(product(j) / analysisWindow_[static_cast<size_t>(N - L + j)] / ola);
  const double binHz = rate_ / config_.M;
  bassBin_ = static_cast<int>(options_.bassRadiusHz / binHz);
  peakRatio_ = static_cast<float>(std::pow(10.0, options_.peakFloorDb / 10.0));
  maxPeaks_ = std::max(16, options_.maxPeaks * config_.M / 2048);
  // the window's transform, for the image of low partials: A(nu) = sum a[n] e^{-2 pi i nu n / M}
  skirtBins_ = options_.imageCancel ? skirtFor(config_) : 0;
  const int points = 2 * skirtBins_ * kTableStep + 1;
  for (int i = 0; i < points; ++i) {
    const double nu = static_cast<double>(i) / kTableStep - skirtBins_;
    double sr = 0.0, si = 0.0;
    const double step = -kTwoPi * nu / config_.M;
    const double cs = std::cos(step), sn = std::sin(step);
    double pr = 1.0, pi = 0.0;  // e^{i step n}, by rotation (renormalised)
    for (int n = 0; n < N; ++n) {
      const double w = analysisWindow_[static_cast<size_t>(n)];
      sr += w * pr;
      si += w * pi;
      const double t = pr * cs - pi * sn;
      pi = pr * sn + pi * cs;
      pr = t;
      if ((n & 255) == 255) {
        const double g = 1.0 / std::sqrt(pr * pr + pi * pi);
        pr *= g;
        pi *= g;
      }
    }
    tableRe_[static_cast<size_t>(i)] = static_cast<float>(sr);
    tableIm_[static_cast<size_t>(i)] = static_cast<float>(si);
  }
  if (points > 0) {
    tableRe_[static_cast<size_t>(points)] = tableRe_[static_cast<size_t>(points - 1)];
    tableIm_[static_cast<size_t>(points)] = tableIm_[static_cast<size_t>(points - 1)];
  }
}

int Transposer::skirtFor(const Config& c) const {
  return options_.imageCancel ? std::max(16, 6 * c.M / c.L) : 0;
}

void Transposer::startVoice(int slot, int semitones) {
  Voice& v = voices_[slot];
  v.semitones = semitones;
  v.ratio = std::pow(2.0, semitones / 12.0);
  v.frames = 0;
  std::fill(v.theta.begin(), v.theta.end(), 0.0f);
  std::fill(v.acc.begin(), v.acc.end(), 0.0f);
  std::fill(v.ready.begin(), v.ready.end(), 0.0f);
}

void Transposer::reset() {
  if (ring_.empty()) return;
  std::fill(ring_.begin(), ring_.end(), 0.0f);
  written_ = 0;
  hopPos_ = 0;
  std::fill(prevRe_.begin(), prevRe_.end(), 0.0f);
  std::fill(prevIm_.begin(), prevIm_.end(), 0.0f);
  std::fill(prevMag_.begin(), prevMag_.end(), 0.0f);
  std::fill(estBin_.begin(), estBin_.end(), 0);
  std::fill(kappaMemo_.begin(), kappaMemo_.end(), 0.0f);
  primed_ = true;  // the history is silence, and so is the previous spectrum
  fading_ = false;
  fadePos_ = 0;
  pending_ = -1;
  for (int s = 0; s < kVoices; ++s) startVoice(s, 0);
  const int t = target_.load(std::memory_order_relaxed);
  if (t == 0) {
    cur_ = kDry;
  } else {
    startVoice(0, t);
    voices_[0].frames = config_.L / config_.H;
    cur_ = 0;
  }
}

// ------------------------------------------------------------------------------------------------ real FFT
// x (M samples, of which the first N are used; the rest is zero) -> re_/im_ bins 0..K-1 (bin 0 is DC).
void Transposer::forwardReal(const float* x) {
  const int K = config_.K, N = config_.N;
  float* zr = zr_.data();
  float* zi = zi_.data();
  const int half = N / 2;
  for (int i = 0; i < half; ++i) {
    zr[i] = x[2 * i];
    zi[i] = x[2 * i + 1];
  }
  std::fill(zr + half, zr + K, 0.0f);
  std::fill(zi + half, zi + K, 0.0f);
  fft_->forward(zr, zi);
  float* re = re_.data();
  float* im = im_.data();
  const float* wr = twiddleRe_.data();
  const float* wi = twiddleIm_.data();
  re[0] = 0.0f;  // DC is not moved
  im[0] = 0.0f;
  for (int k = 1; k < K; ++k) {
    const int j = K - k;
    const float ar = zr[k], ai = zi[k], br = zr[j], bi = -zi[j];  // Z[k], conj Z[K-k]
    const float er = 0.5f * (ar + br), ei = 0.5f * (ai + bi);
    const float orr = 0.5f * (ai - bi), oi = -0.5f * (ar - br);  // (Z - conj)/(2i)
    re[k] = er + wr[k] * orr - wi[k] * oi;
    im[k] = ei + wr[k] * oi + wi[k] * orr;
  }
}

// yr_/yi_ bins 0..K-1 (bin 0 real, Nyquist zero) -> x (M samples).
void Transposer::inverseReal(float* x) {
  const int K = config_.K;
  float* zr = zr_.data();
  float* zi = zi_.data();
  const float* yr = yr_.data();
  const float* yi = yi_.data();
  const float* wr = twiddleRe_.data();
  const float* wi = twiddleIm_.data();
  zr[0] = 0.5f * yr[0];
  zi[0] = 0.5f * yr[0];
  for (int k = 1; k < K; ++k) {
    const int j = K - k;
    const float ar = yr[k], ai = yi[k], br = yr[j], bi = -yi[j];  // Y[k], conj Y[K-k]
    const float er = 0.5f * (ar + br), ei = 0.5f * (ai + bi);
    const float dr = 0.5f * (ar - br), di = 0.5f * (ai - bi);
    const float orr = dr * wr[k] + di * wi[k], oi = di * wr[k] - dr * wi[k];  // d * conj(W^k)
    zr[k] = er - oi;  // E + i O
    zi[k] = ei + orr;
  }
  fft_->inverse(zr, zi);
  for (int i = 0; i < K; ++i) {
    x[2 * i] = zr[i];
    x[2 * i + 1] = zi[i];
  }
}

// ------------------------------------------------------------------------------------------------ one hop
// A(nu0 + j) for j = 0, 1, ..., linearly interpolated (the fraction is the same for every j)
struct Transposer::TableRun {
  TableRun(const Transposer& t, double nu0) : re_(t.tableRe_.data()), im_(t.tableIm_.data()) {
    const double pos = (nu0 + t.skirtBins_) * kTableStep;
    const double lo = std::floor(pos);
    i_ = static_cast<int>(lo);
    top_ = 2 * t.skirtBins_ * kTableStep;
    f_ = static_cast<float>(pos - lo);
  }
  int index() const noexcept { return std::clamp(i_, 0, top_); }
  float re() const noexcept { const int i = index(); return re_[i] + f_ * (re_[i + 1] - re_[i]); }
  float im() const noexcept { const int i = index(); return im_[i] + f_ * (im_[i + 1] - im_[i]); }
  void next() noexcept { i_ += kTableStep; }
  const float* re_;
  const float* im_;
  int i_ = 0, top_ = 0;
  float f_ = 0.0f;
};

void Transposer::analyse() {
  const int N = config_.N, K = config_.K, H = config_.H, M = config_.M;
  float* f = frame_.data();
  const float* win = analysisWindow_.data();
  const uint64_t start = written_ - static_cast<uint64_t>(N);
  const size_t first = static_cast<size_t>(start) & ringMask_;
  const size_t run = std::min(static_cast<size_t>(N), ring_.size() - first);
  for (size_t n = 0; n < run; ++n) f[n] = ring_[first + n] * win[n];
  for (size_t n = run; n < static_cast<size_t>(N); ++n) f[n] = ring_[n - run] * win[n];
  forwardReal(f);

  float* mag = mag_.data();
  const float* re = re_.data();
  const float* im = im_.data();
  for (int k = 0; k < K; ++k) mag[k] = re[k] * re[k] + im[k] * im[k];
  mag[0] = 0.0f;
  // the largest bin, eight lanes at a time (K is a multiple of 8)
  float lanesMax[8] = {};
  for (int k = 0; k < K; k += 8)
    for (int j = 0; j < 8; ++j) lanesMax[j] = std::max(lanesMax[j], mag[k + j]);
  float peak = 0.0f;
  for (int j = 0; j < 8; ++j) peak = std::max(peak, lanesMax[j]);

  // peaks: the largest bin within +-radius (one bin below bassBin_), no more than peakFloorDb below the largest. No
  // absolute floor: a silent frame has no bin above 0 and so no peak, and anything quieter is moved as it would be
  // louder (the input is already 0 below 1e-20, and the audio thread flushes subnormals).
  const float thr = peak * peakRatio_;
  // branch-free: a flag per bin, then the flagged bins in order (noise makes a branch per bin unpredictable)
  int count = 0;
  const int wide = options_.peakRadius >= 2 ? std::max(2, bassBin_) : K;  // radius 2 from here up
  uint8_t* flag = peakFlag_.data();
  flag[0] = 0;
  flag[K - 1] = 0;
  for (int k = 1; k < K - 1; ++k) {
    const float m = mag[k];
    flag[k] = static_cast<uint8_t>((m > thr) & (m > mag[k - 1]) & (m >= mag[k + 1]));
  }
  for (int k = std::max(wide, 2); k < K - 2; ++k) {
    const float m = mag[k];
    flag[k] = static_cast<uint8_t>(flag[k] & (m > mag[k - 2]) & (m >= mag[k + 2]));
  }
  if (wide < K - 1) flag[K - 2] = static_cast<uint8_t>(flag[K - 2] & (mag[K - 2] > mag[K - 4]));
  int* peaks = peaks_.data();
  for (int k = 1; k < K - 1; ++k) {
    peaks[count] = k;
    count += flag[k];
  }
  // bounded work: a guitar shows well under a hundred peaks; noise shows K/4. Above the cap, the highest regions
  // merge into the last one.
  count = std::min(count, maxPeaks_);

  // Frequency of each peak: its phase advance over the hop. The advance is measured on the same bin as long as the
  // peak stays within a bin of it: a partial between two bins makes the peak alternate, and the image's small phase
  // errors then no longer cancel from one hop to the next (a bias of several cents on low notes). A peak whose
  // frequency is more than 1.5 bins from it is not a partial but the leakage of one further away (its phase turns
  // at that partial's rate): it is dropped, so that its bins move with that partial's region, unless the bin's energy
  // jumped. Where the previous hop had no energy on the bin, the advance means nothing and the bin's centre is taken.
  if (!primed_) {
    std::fill(estBin_.begin(), estBin_.end(), 0);
    std::fill(kappaMemo_.begin(), kappaMemo_.end(), 0.0f);
  }
  const float* pr = prevRe_.data();
  const float* pi = prevIm_.data();
  const float* pm = prevMag_.data();
  const double binsPerRad = M / kTwoPi, slack = options_.leakageBins * M / N;
  int kept = 0;
  for (int p = 0; p < count; ++p) {
    const int kp = peaks[p];
    const int last = estBin_[static_cast<size_t>(kp)];
    const int e = last > 0 && std::abs(last - kp) <= 1 ? last : kp;
    double w = kTwoPi * kp / M;
    const bool jumped = !(pm[kp] * 4.0f >= mag[kp]);
    if (pm[e] > mag[e] * 1e-6f && mag[e] > 0.0f) {
      const double expected = kTwoPi * e * H / M;
      const float cr = re[e] * pr[e] + im[e] * pi[e], ci = im[e] * pr[e] - re[e] * pi[e];
      const double measured = (expected + wrapPhase(fastAtan2(ci, cr) - expected)) / H;
      if (!(slack > 0.0) || std::fabs(measured * binsPerRad - kp) <= slack) w = measured;
      else if (!jumped) continue;
    }
    peaks[kept] = kp;
    omega_[static_cast<size_t>(kept)] = static_cast<float>(w);
    estPeak_[static_cast<size_t>(kept)] = e;
    ++kept;
  }
  count = kept;
  // regions: from the lowest bin between two peaks
  int a = 1;
  for (int p = 0; p < count; ++p) {
    int b = K;
    if (p + 1 < count) {
      const int kp = peaks[p], kn = peaks[p + 1];
      b = kp + 1;
      for (int k = kp + 2; k < kn; ++k)
        if (mag[k] < mag[b]) b = k;
    }
    regionEnd_[static_cast<size_t>(p)] = b;
    std::fill(nextEstBin_.begin() + a, nextEstBin_.begin() + b, estPeak_[static_cast<size_t>(p)]);
    a = b;
  }
  if (count > 0) std::swap(estBin_, nextEstBin_);
  peakCount_ = count;

  std::copy(re_.begin(), re_.begin() + K, cre_.begin());
  std::copy(im_.begin(), im_.begin() + K, cim_.begin());
  const int V = skirtBins_;
  if (V == 0) return;
  // Low partials: the image also disturbs the phase advance (+-0.3 bin at 40 Hz in Tight). Fit the sinusoid model on
  // this hop's and the previous hop's peak bin with a guess of the frequency; the phase advance between the two
  // complex amplitudes is the next guess. The fixed point is the partial's frequency; it is found by a secant step
  // from the last hop's value (three evaluations, a few thousandths of a bin).
  const auto advance = [&](int kp, double kappa, double& next) {
    double u, v, pu, pv;
    if (!fitPartial(re_.data(), im_.data(), kp, kappa, u, v) || !fitPartial(prevRe_.data(), prevIm_.data(), kp, kappa, pu, pv))
      return false;
    const double dr = u * pu + v * pv, di = v * pu - u * pv;  // c conj(c_prev)
    const double expected = kTwoPi * kappa * H / M;
    next = (expected + wrapPhase(std::atan2(di, dr) - expected)) / H * M / kTwoPi;
    return next > 0.0;
  };
  a = 1;
  int low = 0;
  for (int p = 0; p < count; ++p) {
    lowKappa_[static_cast<size_t>(p)] = -1.0f;
    const int kp = peaks_[static_cast<size_t>(p)], b = regionEnd_[static_cast<size_t>(p)];
    double kappa = omega_[static_cast<size_t>(p)] * M / kTwoPi;
    if (std::fabs(kappa - kp) >= 1.0) kappa = kp;
    const double last = kappaMemo_[static_cast<size_t>(kp)];
    const bool lowPartial = kp < 0.5 * V && kappa >= 0.75;
    if (lowPartial && last >= 0.75 && std::fabs(last - kp) < 1.0) kappa = last;  // start from the last hop's answer
    fitKappa_[static_cast<size_t>(p)] = static_cast<float>(kappa);
    if (primed_ && lowPartial && low < options_.maxLowPartials && prevMag_[static_cast<size_t>(kp)] > mag[kp] * 1e-6f) {
      ++low;
      double g0, g1, g2;
      if (advance(kp, kappa, g0)) {
        double k1 = g0;
        if (advance(kp, k1, g1)) {
          const double h0 = g0 - kappa, h1 = g1 - k1, slope = (h1 - h0) / (k1 - kappa);
          double k2 = g1;
          if (std::fabs(k1 - kappa) > 1e-9 && std::fabs(slope) > 1e-3) k2 = k1 - h1 / slope;
          if (std::fabs(k2 - kp) < 1.5 && advance(kp, k2, g2)) k1 = g2;
          else k1 = g1;
        }
        if (std::fabs(k1 - kp) < 1.5) {
          kappa = k1;
          omega_[static_cast<size_t>(p)] = static_cast<float>(kappa * kTwoPi / M);
        }
      }
      fitKappa_[static_cast<size_t>(p)] = static_cast<float>(kappa);
    }
    std::fill(nextKappaMemo_.begin() + a, nextKappaMemo_.begin() + b, static_cast<float>(kappa));
    a = b;
  }
  if (count > 0) std::swap(kappaMemo_, nextKappaMemo_);
  cancelImages(count);
}

// The real partial Re(c e^{i 2 pi kappa n / M}) that explains bin k of the spectrum (re, im) together with its image:
// 2 X(k) = c A(k - kappa) + c* A(k + kappa) = u P + v Q, P = A1 + A2, Q = i (A1 - A2), c = u + i v.
bool Transposer::fitPartial(const float* re, const float* im, int k, double kappa, double& u, double& v) const noexcept {
  float a1r, a1i, a2r, a2i;
  tableAt(k - kappa, a1r, a1i);
  tableAt(k + kappa, a2r, a2i);
  const double pr = a1r + a2r, pi = a1i + a2i, qr = -(a1i - a2i), qi = a1r - a2r;
  const double xr = 2.0 * re[k], xi = 2.0 * im[k];
  const double det = pr * qi - qr * pi;
  if (!(std::fabs(det) > 1e-30)) return false;
  u = (xr * qi - qr * xi) / det;
  v = (pr * xi - pi * xr) / det;
  return true;
}

// The low partials' images. A real partial x = Re(c e^{i w n}) reads X(k) = (c A(k - kappa) + c* A(k + kappa)) / 2
// through the window: the second term is its negative-frequency image, which spills into the lowest bins and would be
// moved the wrong way (a mirror tone at w - dw, -14 dB at 40 Hz and -21 dB at 80 Hz in Tight). Fit c on the peak bin,
// remove the image from cre_/cim_, and keep c so that the part of the partial's own skirt below 0 Hz moves with it.
void Transposer::cancelImages(int count) {
  const int K = config_.K, V = skirtBins_;
  float* cr = cre_.data();
  float* ci = cim_.data();
  int low = 0;
  for (int p = 0; p < count && low < options_.maxLowPartials; ++p) {
    lowKappa_[static_cast<size_t>(p)] = -1.0f;
    const double kappa = fitKappa_[static_cast<size_t>(p)];
    const int kp = peaks_[static_cast<size_t>(p)];
    // a partial's own peak only (the window's ripples make peaks of their own, which are this partial's leakage), not
    // too close to 0 Hz (where c and its image cannot be told apart), and a fit that explains the bin it was made on
    if (!(kappa >= 0.75) || kappa >= 0.5 * V || std::fabs(kp - kappa) >= 1.0) continue;
    double u, v;
    if (!fitPartial(cr, ci, kp, kappa, u, v)) continue;
    float a1r, a1i;
    tableAt(kp - kappa, a1r, a1i);
    const double own = 0.25 * (u * u + v * v) * (a1r * a1r + a1i * a1i), seen = cr[kp] * cr[kp] + ci[kp] * ci[kp];
    if (!(own < 4.0 * seen && own > 0.25 * seen)) continue;
    // subtract conj(c) A(k + kappa) / 2 where the table reaches
    const int hi = std::min(K, static_cast<int>(V - kappa));
    TableRun t(*this, 1 + kappa);
    const float hu = static_cast<float>(0.5 * u), hv = static_cast<float>(0.5 * v);
    for (int k = 1; k < hi; ++k, t.next()) {
      const float tr = t.re(), ti = t.im();
      cr[k] -= hu * tr + hv * ti;
      ci[k] -= hu * ti - hv * tr;
    }
    lowRe_[static_cast<size_t>(p)] = static_cast<float>(u);
    lowIm_[static_cast<size_t>(p)] = static_cast<float>(v);
    lowKappa_[static_cast<size_t>(p)] = static_cast<float>(kappa);
    ++low;
  }
}

// A(nu), linearly interpolated; nu within [-V, V]
void Transposer::tableAt(double nu, float& re, float& im) const noexcept {
  const double pos = (nu + skirtBins_) * kTableStep;
  const double lo = std::floor(pos);
  const int i = std::clamp(static_cast<int>(lo), 0, 2 * skirtBins_ * kTableStep);
  const float t = static_cast<float>(pos - lo);
  re = tableRe_[static_cast<size_t>(i)] + t * (tableRe_[static_cast<size_t>(i + 1)] - tableRe_[static_cast<size_t>(i)]);
  im = tableIm_[static_cast<size_t>(i)] + t * (tableIm_[static_cast<size_t>(i + 1)] - tableIm_[static_cast<size_t>(i)]);
}

void Transposer::synthesise(Voice& v) {
  const int N = config_.N, K = config_.K, H = config_.H, M = config_.M, L = config_.L;
  float* yr = yr_.data();
  float* yi = yi_.data();
  std::fill(yr, yr + K, 0.0f);
  std::fill(yi, yi + K, 0.0f);
  const float* re = cre_.data();
  const float* im = cim_.data();
  float* nextTheta = v.nextTheta.data();
  const double centre = options_.centreCorrection ? N - 0.5 * L : 0.0;
  const double r1 = v.ratio - 1.0;
  int a = 1;
  for (int p = 0; p < peakCount_; ++p) {
    const int kp = peaks_[static_cast<size_t>(p)], b = regionEnd_[static_cast<size_t>(p)];
    const double dw = r1 * omega_[static_cast<size_t>(p)];
    const int shift = static_cast<int>(std::lround(dw * M / kTwoPi));
    const double theta = wrapPhase(v.theta[static_cast<size_t>(kp)] + dw * H);
    std::fill(nextTheta + a, nextTheta + b, static_cast<float>(theta));
    const float phi = static_cast<float>(wrapPhase(theta - kTwoPi * shift * centre / M));
    float s, c;
    fastSinCos(phi, s, c);
    // bins that land on 1..K-1
    const int lo = std::max(a, 1 - shift), hi = std::min(b, K - shift);
    float* dr = yr + shift;
    float* di = yi + shift;
    for (int k = lo; k < hi; ++k) {
      dr[k] += re[k] * c - im[k] * s;
      di[k] += re[k] * s + im[k] * c;
    }
    // below 0 Hz: the image folds back
    for (int k = a; k < std::min(b, 1 - shift); ++k) {
      const float xr = re[k] * c - im[k] * s, xi = re[k] * s + im[k] * c;
      const int m = -(k + shift);
      if (m == 0) {
        yr[0] += 2.0f * xr;
      } else if (m < K) {
        yr[m] += xr;
        yi[m] -= xi;
      }
    }
    // a low partial's skirt below 0 Hz, c A(k - kappa) / 2 for k <= 0, moved with it
    const float kappa = lowKappa_[static_cast<size_t>(p)];
    if (kappa >= 0.0f) {
      const float u = 0.5f * lowRe_[static_cast<size_t>(p)], w = 0.5f * lowIm_[static_cast<size_t>(p)];
      const int from = -static_cast<int>(skirtBins_ - kappa);
      TableRun t(*this, from - static_cast<double>(kappa));
      for (int k = from; k <= 0; ++k, t.next()) {
        const float tr = t.re(), ti = t.im();
        const float sr = u * tr - w * ti, si = u * ti + w * tr;
        const float xr = sr * c - si * s, xi = sr * s + si * c;
        const int d = k + shift;
        if (d > 0 && d < K) {
          yr[d] += xr;
          yi[d] += xi;
        } else if (d == 0) {
          yr[0] += 2.0f * xr;
        } else if (-d < K) {
          yr[-d] += xr;
          yi[-d] -= xi;
        }
      }
    }
    a = b;
  }
  if (peakCount_ > 0) std::swap(v.theta, v.nextTheta);
  yi[0] = 0.0f;
  inverseReal(frame_.data());
  const float* f = frame_.data() + (N - L);
  const float* w = synthesisWindow_.data();
  float* acc = v.acc.data();
  for (int j = 0; j < L; ++j) acc[j] += f[j] * w[j];
  std::copy(acc, acc + H, v.ready.begin());
  std::memmove(acc, acc + H, sizeof(float) * static_cast<size_t>(L - H));
  std::fill(acc + (L - H), acc + L, 0.0f);
  ++v.frames;
}

void Transposer::runHop() {
  const int overlap = config_.L / config_.H;
  if (fading_ && fadePos_ >= fadeLength_) {
    cur_ = fadeTo_;
    fading_ = false;
  }
  if (!fading_) {
    const int t = target_.load(std::memory_order_relaxed);
    const int curSemis = cur_ == kDry ? 0 : voices_[cur_].semitones;
    if (pending_ >= 0 && voices_[pending_].semitones != t) pending_ = -1;  // retarget (it was not heard yet)
    if (pending_ < 0 && t != curSemis) {
      if (t == 0) {
        fading_ = true;
        fadeFrom_ = cur_;
        fadeTo_ = kDry;
        fadePos_ = 0;
      } else {
        pending_ = cur_ == 0 ? 1 : 0;
        startVoice(pending_, t);
      }
    }
  }
  int active[3];
  int n = 0;
  if (fading_) {
    if (fadeFrom_ != kDry) active[n++] = fadeFrom_;
    if (fadeTo_ != kDry) active[n++] = fadeTo_;
  } else if (cur_ != kDry) {
    active[n++] = cur_;
  }
  if (pending_ >= 0) active[n++] = pending_;
  if (n == 0) {
    primed_ = false;
    return;
  }
  analyse();
  if (primed_)
    for (int i = 0; i < n; ++i) synthesise(voices_[active[i]]);
  std::swap(re_, prevRe_);
  std::swap(im_, prevIm_);
  std::swap(mag_, prevMag_);
  primed_ = true;
  if (pending_ >= 0 && voices_[pending_].frames >= overlap) {
    fading_ = true;
    fadeFrom_ = cur_;
    fadeTo_ = pending_;
    fadePos_ = 0;
    pending_ = -1;
  }
}

void Transposer::process(const float* in, float* out, int frames) noexcept {
  if (frames <= 0) return;
  if (ring_.empty()) {
    std::fill(out, out + frames, 0.0f);
    return;
  }
  engine::DenormalGuard guard;
  const int H = config_.H;
  const uint64_t L = static_cast<uint64_t>(config_.L);
  int done = 0;
  while (done < frames) {
    const int n = std::min(frames - done, H - hopPos_);
    for (int i = 0; i < n; ++i) ring_[static_cast<size_t>(written_ + static_cast<uint64_t>(i)) & ringMask_] = clean(in[done + i]);
    float* o = out + done;
    const auto source = [&](int src, int i) -> float {
      return src == kDry ? readDry(written_ + static_cast<uint64_t>(i) - L) : voices_[src].ready[static_cast<size_t>(hopPos_ + i)];
    };
    if (!fading_) {
      if (cur_ == kDry)
        for (int i = 0; i < n; ++i) o[i] = readDry(written_ + static_cast<uint64_t>(i) - L);
      else
        std::copy_n(voices_[cur_].ready.data() + hopPos_, n, o);
    } else {
      for (int i = 0; i < n; ++i) {
        const float x = source(fadeFrom_, i), y = source(fadeTo_, i);
        if (fadePos_ < fadeLength_) {
          o[i] = x * fadeOut_[static_cast<size_t>(fadePos_)] + y * fadeIn_[static_cast<size_t>(fadePos_)];
          ++fadePos_;
        } else {
          o[i] = y;
        }
      }
    }
    written_ += static_cast<uint64_t>(n);
    hopPos_ += n;
    done += n;
    if (hopPos_ == H) {
      hopPos_ = 0;
      runHop();
    }
  }
}

}  // namespace magmora
