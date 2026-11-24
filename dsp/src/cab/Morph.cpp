#include "cab/Morph.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "cab/Spectral.h"

namespace magmora::cab::morph {
namespace {

using spectral::Complex;
constexpr double kPi = 3.14159265358979323846;
constexpr double kSpeedOfSound = 343.0;
constexpr int kPerOctave = 96;                    // LOGF_PER_OCT
constexpr int kMaxShift = kPerOctave / 6;         // a feature moves at most 1/6 octave
constexpr int kTrend = kPerOctave / 2;            // the detail is the curve minus its half-octave trend
constexpr double kStiffness = 0.02;
constexpr double kAxisStart = 60.0;

size_t nextPow2(double x) {  // int(2 ** np.ceil(np.log2(x)))
  return static_cast<size_t>(std::llround(std::pow(2.0, std::ceil(std::log2(x)))));
}

// numpy.interp, including its handling of the ends and of an exact hit.
double interp(double x, const double* xp, const double* fp, size_t n, double left, double right) {
  if (std::isnan(x)) return x;
  if (x < xp[0]) return left;
  if (x > xp[n - 1]) return right;
  // j with xp[j] <= x < xp[j + 1], or n - 1 when x == xp[n - 1]
  size_t j = static_cast<size_t>(std::upper_bound(xp, xp + n, x) - xp) - 1;
  if (j == n - 1) return fp[j];
  if (xp[j] == x) return fp[j];
  const double slope = (fp[j + 1] - fp[j]) / (xp[j + 1] - xp[j]);
  double r = slope * (x - xp[j]) + fp[j];
  if (std::isnan(r)) {
    r = slope * (x - xp[j + 1]) + fp[j + 1];
    if (std::isnan(r) && fp[j] == fp[j + 1]) r = fp[j];
  }
  return r;
}

std::vector<double> smooth(const std::vector<double>& x, int n) {  // virtualmic._smooth
  const int m = 2 * n + 1;
  std::vector<double> k(static_cast<size_t>(m));
  double sum = 0.0;
  for (int i = 0; i < m; ++i) {
    k[static_cast<size_t>(i)] = 0.5 - 0.5 * std::cos(2.0 * kPi * i / (m - 1));  // numpy.hanning
    sum += k[static_cast<size_t>(i)];
  }
  for (double& v : k) v /= sum;
  const long len = static_cast<long>(x.size());
  auto at = [&](long i) { return x[static_cast<size_t>(std::clamp(i, 0L, len - 1))]; };  // edge padding
  std::vector<double> y(x.size());
  for (long i = 0; i < len; ++i) {
    double acc = 0.0;
    for (int j = 0; j < m; ++j) acc += at(i - n + j) * k[static_cast<size_t>(m - 1 - j)];
    y[static_cast<size_t>(i)] = acc;
  }
  return y;
}

std::vector<double> rfftFreq(size_t nfft, double fs) {  // numpy.fft.rfftfreq(nfft, 1 / fs)
  const double val = 1.0 / (static_cast<double>(nfft) * (1.0 / fs));
  std::vector<double> f(nfft / 2 + 1);
  for (size_t k = 0; k < f.size(); ++k) f[k] = static_cast<double>(k) * val;
  return f;
}

std::vector<double> logAxis(double fs) {  // virtualmic._log_axis
  const int count = static_cast<int>(std::log2(0.49 * fs / kAxisStart) * kPerOctave) + 1;
  std::vector<double> fl(static_cast<size_t>(std::max(count, 1)));
  for (size_t k = 0; k < fl.size(); ++k) fl[k] = kAxisStart * std::pow(2.0, static_cast<double>(k) / kPerOctave);
  return fl;
}

std::vector<double> dbOnLog(const std::vector<double>& mag, const std::vector<double>& f, const std::vector<double>& fl) {
  std::vector<double> c(mag.size() + 1, 0.0);
  for (size_t k = 0; k < mag.size(); ++k) c[k + 1] = c[k] + mag[k] * mag[k];
  const double up = std::pow(2.0, 0.5 / kPerOctave), down = std::pow(2.0, -0.5 / kPerOctave);
  std::vector<double> out(fl.size());
  for (size_t k = 0; k < fl.size(); ++k) {
    const double e = fl[k] * up, b = fl[k] * down;
    const size_t lo = static_cast<size_t>(std::lower_bound(f.begin(), f.end(), b) - f.begin());
    const size_t hi = std::max(static_cast<size_t>(std::lower_bound(f.begin(), f.end(), e) - f.begin()), lo + 1);
    out[k] = 10.0 * std::log10((c[std::min(hi, mag.size())] - c[lo]) / static_cast<double>(hi - lo) + 1e-30);
  }
  return out;
}

std::vector<double> magnitude(const std::vector<double>& x, size_t nfft) {
  const std::vector<Complex> X = spectral::rfft(x, nfft);
  std::vector<double> m(X.size());
  for (size_t k = 0; k < X.size(); ++k) m[k] = std::abs(X[k]);
  return m;
}

}  // namespace

std::vector<double> minimumPhase(const std::vector<double>& mag, size_t n) {
  const size_t bins = mag.size(), nfft = 2 * (bins - 1);
  const double peak = *std::max_element(mag.begin(), mag.end());
  std::vector<Complex> L(bins);
  for (size_t k = 0; k < bins; ++k) L[k] = Complex(std::log(std::max(mag[k], 1e-9 * peak)), 0.0);
  std::vector<double> c = spectral::irfft(L, nfft);  // the real cepstrum (the log magnitude is even)
  for (size_t k = 1; k < nfft / 2; ++k) c[k] *= 2.0;  // fold onto the causal half
  for (size_t k = nfft / 2 + 1; k < nfft; ++k) c[k] = 0.0;
  std::vector<Complex> S = spectral::rfft(c, nfft);
  for (Complex& s : S) s = std::exp(s);
  std::vector<double> h = spectral::irfft(S, nfft);
  h.resize(std::min(n, h.size()));
  return h;
}

double onset(const std::vector<double>& h, double fs) {
  if (h.empty()) return 0.0;
  double peak = 0.0;
  for (double v : h) peak = std::max(peak, std::fabs(v));
  size_t i = 0;
  while (i < h.size() && !(std::fabs(h[i]) > 0.1 * peak)) ++i;
  if (i == h.size()) i = 0;  // numpy.argmax of all-false
  const size_t j = std::min(h.size(), i + static_cast<size_t>(std::max(static_cast<int>(0.0005 * fs), 2)));
  double num = 0.0, den = 0.0;
  for (size_t k = i; k < j; ++k) {
    const double e2 = h[k] * h[k];
    num += static_cast<double>(k) * e2;
    den += e2;
  }
  return num / (den + 1e-30);
}

std::vector<double> shift(const std::vector<double>& h, double d) {
  const size_t n = 2 * h.size();
  std::vector<Complex> H = spectral::rfft(h, n);
  const double val = 1.0 / (static_cast<double>(n) * 1.0);
  for (size_t k = 0; k < H.size(); ++k) {
    const double phase = -2.0 * kPi * (static_cast<double>(k) * val) * d;
    const Complex w(std::cos(phase), std::sin(phase)), x = H[k];
    H[k] = Complex(x.real() * w.real() - x.imag() * w.imag(), x.real() * w.imag() + x.imag() * w.real());
  }
  std::vector<double> y = spectral::irfft(H, n);
  y.resize(h.size());
  return y;
}

std::vector<int> warpPath(const std::vector<double>& a, const std::vector<double>& b) {
  const std::vector<double> sa = smooth(a, kTrend), sb = smooth(b, kTrend);
  const int n = static_cast<int>(a.size()), W = kMaxShift, width = 2 * W + 1;
  std::vector<double> da(a.size()), db(b.size());
  for (size_t k = 0; k < a.size(); ++k) {
    da[k] = a[k] - sa[k];
    db[k] = b[k] - sb[k];
  }
  constexpr double INF = 1e18;
  std::vector<double> D(static_cast<size_t>(n * width), INF);
  std::vector<signed char> back(static_cast<size_t>(n * width), 0);
  auto local = [&](int i, double* c) {
    for (int k = 0; k < width; ++k) {
      const int j = i + k - W;
      c[k] = (j >= 0 && j < n) ? (da[static_cast<size_t>(i)] - db[static_cast<size_t>(j)]) *
                                         (da[static_cast<size_t>(i)] - db[static_cast<size_t>(j)]) +
                                     kStiffness * (k - W) * (k - W)
                               : INF;
    }
  };
  local(0, D.data());
  std::vector<double> c(static_cast<size_t>(width));
  for (int i = 1; i < n; ++i) {
    local(i, c.data());
    const double* prev = D.data() + static_cast<size_t>((i - 1) * width);
    double* row = D.data() + static_cast<size_t>(i * width);
    signed char* br = back.data() + static_cast<size_t>(i * width);
    for (int k = 0; k < width; ++k) {
      const double from[3] = {k + 1 < width ? prev[k + 1] : INF, prev[k], k > 0 ? prev[k - 1] : INF};
      int m = 0;
      for (int q = 1; q < 3; ++q)
        if (from[q] < from[m]) m = q;  // numpy.argmin: the first of equal minima
      row[k] = c[static_cast<size_t>(k)] + from[m];
      br[k] = static_cast<signed char>(m);
    }
  }
  const double* last = D.data() + static_cast<size_t>((n - 1) * width);
  int k = static_cast<int>(std::min_element(last, last + width) - last);
  std::vector<int> j(static_cast<size_t>(n));
  for (int i = n - 1; i >= 0; --i) {
    j[static_cast<size_t>(i)] = std::clamp(i + k - W, 0, n - 1);
    k = std::clamp(k + (1 - back[static_cast<size_t>(i * width + k)]), 0, 2 * W);
  }
  return j;
}

std::vector<double> interpolate(const std::vector<double>& a, const std::vector<double>& b, double t, double fs,
                                Method method, bool minimumPhasePack) {
  if (method == Method::Snap || t <= 0.0 || t >= 1.0) return t < 0.5 ? a : b;
  const size_t n = std::max(a.size(), b.size());
  const double ta = onset(a, fs), tb = onset(b, fs);
  const size_t nfft = nextPow2(2.0 * static_cast<double>(n));
  const std::vector<double> A = magnitude(a, nfft), B = magnitude(b, nfft);
  const size_t bins = A.size();
  std::vector<double> plain(bins);
  for (size_t k = 0; k < bins; ++k) plain[k] = std::exp((1 - t) * std::log(A[k] + 1e-12) + t * std::log(B[k] + 1e-12));
  const std::vector<double> f = rfftFreq(nfft, fs), fl = logAxis(fs);
  const std::vector<double> la = dbOnLog(A, f, fl), lb = dbOnLog(B, f, fl);
  const std::vector<int> j = warpPath(la, lb);
  const size_t L = fl.size();
  std::vector<double> u(L), v(L), mid(L), lt(L);
  for (size_t i = 0; i < L; ++i) {
    const size_t ji = static_cast<size_t>(j[i]);
    u[i] = (1 - t) * static_cast<double>(i) + t * static_cast<double>(ji);  // strictly increasing: no sort needed
    v[i] = (1 - t) * la[i] + t * lb[ji];
    mid[i] = (1 - t) * la[i] + t * lb[i];
  }
  for (size_t i = 0; i < L; ++i) lt[i] = interp(static_cast<double>(i), u.data(), v.data(), L, v[0], v[L - 1]);
  const double nan = std::numeric_limits<double>::quiet_NaN();
  std::vector<double> mag(bins);
  for (size_t k = 0; k < bins; ++k) {
    const double fine = 20.0 * std::log10(plain[k] + 1e-12) - interp(f[k], fl.data(), mid.data(), L, nan, nan);
    const double db = interp(f[k], fl.data(), lt.data(), L, nan, nan) + fine;
    mag[k] = std::isnan(db) ? plain[k] : std::pow(10.0, db / 20.0);
  }
  std::vector<double> h = minimumPhase(mag, n);
  if (!minimumPhasePack) h = shift(h, (1 - t) * ta + t * tb - onset(h, fs));
  return h;
}

void relativeDelay(const std::vector<double>& a, const std::vector<double>& b, double fs, double& delay, int& polarity) {
  constexpr int up = 8;
  constexpr double maxMs = 10.0;
  const size_t n = nextPow2(static_cast<double>(a.size() + b.size()));
  const std::vector<Complex> A = spectral::rfft(a, n), B = spectral::rfft(b, n);
  std::vector<Complex> Xu(n * up / 2 + 1, Complex(0.0, 0.0));
  for (size_t k = 0; k < A.size(); ++k)
    Xu[k] = Complex(A[k].real() * B[k].real() + A[k].imag() * B[k].imag(), A[k].imag() * B[k].real() - A[k].real() * B[k].imag());
  const std::vector<double> c = spectral::irfft(Xu, n * up);
  const long m = static_cast<long>(maxMs * 1e-3 * fs * up);
  const long len = static_cast<long>(c.size());
  long best = 0;
  double bestAbs = -1.0, bestValue = 0.0;
  for (long k = 0; k <= 2 * m; ++k) {  // w = c[-m:] then c[:m + 1]
    const long lag = k - m;
    const double w = c[static_cast<size_t>(lag < 0 ? len + lag : lag)];
    if (std::fabs(w) > bestAbs) {
      bestAbs = std::fabs(w);
      best = k;
      bestValue = w;
    }
  }
  delay = static_cast<double>(best - m) / up;
  polarity = bestValue > 0.0 ? 1 : (bestValue < 0.0 ? -1 : 0);
}

void panGains(double pan, double& left, double& right) {
  const double th = (pan + 1.0) * kPi / 4.0;
  left = std::cos(th);
  right = std::sin(th);
}

TwoMics twoMics(const std::vector<double>& h1, const std::vector<double>& h2, double fs, const MicMix& m1,
                const MicMix& m2, bool align, double offsetMm, PackPhase phase) {
  TwoMics out;
  const bool second = !h2.empty();
  const size_t n = std::max(h1.size(), h2.size());
  std::vector<double> a(h1), b(h2);
  a.resize(n, 0.0);
  if (second) {
    b.resize(n, 0.0);
    if (phase == PackPhase::Raw) relativeDelay(b, a, fs, out.measuredDelay, out.detectedPolarity);
    const double move =
        ((align && phase == PackPhase::Raw) ? -out.measuredDelay : 0.0) + offsetMm * 1e-3 / kSpeedOfSound * fs;
    if (std::fabs(move) > 1e-9) {
      if (move > 0.0) {
        std::vector<double> padded(b);
        padded.resize(n + 64, 0.0);
        b = shift(padded, move);
        b.resize(n);
      } else {
        b = shift(b, move);
      }
    }
    out.moved = move;
  }
  const double g1 = std::pow(10.0, m1.levelDb / 20.0) * (m1.invert ? -1.0 : 1.0);
  const double g2 = std::pow(10.0, m2.levelDb / 20.0) * (m2.invert ? -1.0 : 1.0);
  double l1, r1, l2, r2;
  panGains(m1.pan, l1, r1);
  panGains(m2.pan, l2, r2);
  out.left.resize(n);
  out.right.resize(n);
  for (size_t k = 0; k < n; ++k) {
    out.left[k] = g1 * l1 * a[k] + (second ? g2 * l2 * b[k] : 0.0);
    out.right[k] = g1 * r1 * a[k] + (second ? g2 * r2 * b[k] : 0.0);
  }
  return out;
}

}  // namespace magmora::cab::morph
