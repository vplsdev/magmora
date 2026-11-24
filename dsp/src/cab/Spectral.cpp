#include "cab/Spectral.h"

#include <cmath>
#include <map>
#include <memory>

namespace magmora::cab::spectral {
namespace {

constexpr double kPi = 3.14159265358979323846;

// Plain complex products: std::complex's operator* follows C99 Annex G (infinities and NaN), several times slower.
inline Complex mul(const Complex& a, const Complex& b) {
  return Complex(a.real() * b.real() - a.imag() * b.imag(), a.real() * b.imag() + a.imag() * b.real());
}
inline Complex mulConj(const Complex& a, const Complex& b) {  // a * conj(b)
  return Complex(a.real() * b.real() + a.imag() * b.imag(), a.imag() * b.real() - a.real() * b.imag());
}

bool powerOfTwo(size_t n) { return n >= 1 && (n & (n - 1)) == 0; }

// Radix-2 tables for one size, cached per thread (the worker builds responses of a handful of sizes).
struct Radix2 {
  size_t n = 0;
  std::vector<size_t> reverse;
  std::vector<Complex> twiddle;  // exp(-2 pi i k / n), k < n / 2

  explicit Radix2(size_t size) : n(size), reverse(size), twiddle(size / 2) {
    int bits = 0;
    while ((size_t{1} << bits) < n) ++bits;
    for (size_t k = 0; k < n; ++k) {
      size_t r = 0;
      for (int b = 0; b < bits; ++b)
        if (k & (size_t{1} << b)) r |= size_t{1} << (bits - 1 - b);
      reverse[k] = r;
    }
    for (size_t k = 0; k < n / 2; ++k) {
      const double a = -2.0 * kPi * static_cast<double>(k) / static_cast<double>(n);
      twiddle[k] = Complex(std::cos(a), std::sin(a));
    }
  }

  void run(Complex* a, bool inverse) const {
    for (size_t k = 0; k < n; ++k)
      if (k < reverse[k]) std::swap(a[k], a[reverse[k]]);
    for (size_t len = 2; len <= n; len <<= 1) {
      const size_t half = len / 2, stride = n / len;
      for (size_t start = 0; start < n; start += len)
        for (size_t k = 0; k < half; ++k) {
          const Complex& w = twiddle[k * stride];
          const Complex x = a[start + k + half];
          const Complex v = inverse ? mulConj(x, w) : mul(x, w);
          const Complex u = a[start + k];
          a[start + k] = u + v;
          a[start + k + half] = u - v;
        }
    }
  }
};

const Radix2& radix2(size_t n) {
  thread_local std::map<size_t, std::unique_ptr<Radix2>> cache;
  auto& p = cache[n];
  if (!p) p = std::make_unique<Radix2>(n);
  return *p;
}

// Bluestein: an n-point DFT as a convolution of chirps, by power-of-two transforms of m >= 2n - 1 points.
struct Chirp {
  size_t n = 0, m = 0;
  std::vector<Complex> w;       // exp(-i pi k^2 / n), k < n
  std::vector<Complex> kernel;  // transform of the conjugate chirp, wrapped

  explicit Chirp(size_t size) : n(size) {
    m = 1;
    while (m < 2 * n - 1) m <<= 1;
    w.resize(n);
    for (size_t k = 0; k < n; ++k) {
      const size_t k2 = (k * k) % (2 * n);  // exact: the angle modulo 2 pi
      const double a = -kPi * static_cast<double>(k2) / static_cast<double>(n);
      w[k] = Complex(std::cos(a), std::sin(a));
    }
    kernel.assign(m, Complex(0.0, 0.0));
    kernel[0] = std::conj(w[0]);
    for (size_t k = 1; k < n; ++k) kernel[k] = kernel[m - k] = std::conj(w[k]);
    radix2(m).run(kernel.data(), false);
  }
};

const Chirp& chirp(size_t n) {
  thread_local std::map<size_t, std::unique_ptr<Chirp>> cache;
  auto& p = cache[n];
  if (!p) p = std::make_unique<Chirp>(n);
  return *p;
}

// exp(-2 pi i k / n), k <= n / 2, for the real-input split of an even n (cached per thread)
const std::vector<Complex>& splitTwiddles(size_t n) {
  thread_local std::map<size_t, std::vector<Complex>> cache;
  auto& t = cache[n];
  if (t.empty()) {
    t.resize(n / 2 + 1);
    for (size_t k = 0; k <= n / 2; ++k) {
      const double a = -2.0 * kPi * static_cast<double>(k) / static_cast<double>(n);
      t[k] = Complex(std::cos(a), std::sin(a));
    }
  }
  return t;
}

// Mixed radix (factors 4, 2, 3, 5): recursive decimation in time, for the response lengths files come in (24000,
// 48000, 22050, 88200 ... samples) without Bluestein's three larger transforms.
struct Mixed {
  size_t n = 0;
  std::vector<int> factors;
  std::vector<Complex> w;  // exp(-2 pi i j / n), j < n

  static bool factor(size_t n, std::vector<int>& out) {
    out.clear();
    for (int p : {4, 2, 3, 5})
      while (n % static_cast<size_t>(p) == 0 && n > 1) {
        out.push_back(p);
        n /= static_cast<size_t>(p);
      }
    return n == 1;
  }
  explicit Mixed(size_t size) : n(size), w(size) {
    factor(n, factors);
    for (size_t j = 0; j < n; ++j) {
      const double a = -2.0 * kPi * static_cast<double>(j) / static_cast<double>(n);
      w[j] = Complex(std::cos(a), std::sin(a));
    }
  }
  void run(const Complex* in, size_t stride, Complex* out, size_t len, size_t level) const {
    if (len == 1) {
      out[0] = in[0];
      return;
    }
    const size_t p = static_cast<size_t>(factors[level]), m = len / p, step = n / len;
    for (size_t r = 0; r < p; ++r) run(in + r * stride, stride * p, out + r * m, m, level + 1);
    Complex y[5];
    for (size_t k = 0; k < m; ++k) {
      y[0] = out[k];
      for (size_t r = 1; r < p; ++r) y[r] = mul(out[r * m + k], w[(r * k * step) % n]);
      for (size_t q = 0; q < p; ++q) {
        Complex acc = y[0];
        for (size_t r = 1; r < p; ++r) acc += mul(y[r], w[((r * q) % p) * (n / p)]);
        out[k + q * m] = acc;
      }
    }
  }
};

const Mixed* mixed(size_t n) {
  thread_local std::map<size_t, std::unique_ptr<Mixed>> cache;
  auto it = cache.find(n);
  if (it == cache.end()) {
    std::vector<int> f;
    it = cache.emplace(n, Mixed::factor(n, f) ? std::make_unique<Mixed>(n) : nullptr).first;
  }
  return it->second.get();
}

void forwardAny(std::vector<Complex>& a) {
  const size_t n = a.size();
  if (n <= 1) return;
  if (powerOfTwo(n)) {
    radix2(n).run(a.data(), false);
    return;
  }
  if (const Mixed* plan = mixed(n)) {
    std::vector<Complex> out(n);
    plan->run(a.data(), 1, out.data(), n, 0);
    a.swap(out);
    return;
  }
  const Chirp& c = chirp(n);
  std::vector<Complex> x(c.m, Complex(0.0, 0.0));
  for (size_t k = 0; k < n; ++k) x[k] = mul(a[k], c.w[k]);
  const Radix2& r = radix2(c.m);
  r.run(x.data(), false);
  for (size_t k = 0; k < c.m; ++k) x[k] = mul(x[k], c.kernel[k]);
  r.run(x.data(), true);
  const double scale = 1.0 / static_cast<double>(c.m);
  for (size_t k = 0; k < n; ++k) a[k] = mul(x[k] * scale, c.w[k]);
}

}  // namespace

void dft(std::vector<Complex>& a, bool inverse) {
  const size_t n = a.size();
  if (n == 0) return;
  if (inverse)
    for (Complex& v : a) v = std::conj(v);
  forwardAny(a);
  if (inverse) {
    const double scale = 1.0 / static_cast<double>(n);
    for (Complex& v : a) v = std::conj(v) * scale;
  }
}

std::vector<Complex> rfft(const double* x, size_t length, size_t n) {
  std::vector<Complex> out(n / 2 + 1);
  if (n >= 4 && n % 2 == 0) {  // the two interleaved real halves as one complex transform of n / 2 points
    const size_t m = n / 2;
    std::vector<Complex> z(m);
    for (size_t k = 0; k < m; ++k) {
      const double re = 2 * k < length ? x[2 * k] : 0.0, im = 2 * k + 1 < length ? x[2 * k + 1] : 0.0;
      z[k] = Complex(re, im);
    }
    forwardAny(z);
    const std::vector<Complex>& tw = splitTwiddles(n);
    for (size_t k = 0; k <= m; ++k) {
      const Complex zk = z[k % m], zm = std::conj(z[(m - k) % m]);
      const Complex d = zk - zm;
      const Complex even = 0.5 * (zk + zm), odd(0.5 * d.imag(), -0.5 * d.real());  // -i/2 (zk - zm)
      out[k] = even + mul(tw[k], odd);
    }
    return out;
  }
  std::vector<Complex> a(n, Complex(0.0, 0.0));
  for (size_t k = 0; k < n && k < length; ++k) a[k] = Complex(x[k], 0.0);
  forwardAny(a);
  for (size_t k = 0; k < out.size(); ++k) out[k] = a[k];
  return out;
}

std::vector<double> irfft(const std::vector<Complex>& Xin, size_t n) {
  std::vector<double> out(n);
  const size_t bins = n / 2 + 1;
  std::vector<Complex> X(bins, Complex(0.0, 0.0));
  for (size_t k = 0; k < bins && k < Xin.size(); ++k) X[k] = Xin[k];
  X[0] = Complex(X[0].real(), 0.0);
  if (n % 2 == 0) X[bins - 1] = Complex(X[bins - 1].real(), 0.0);
  if (n >= 4 && n % 2 == 0) {
    const size_t m = n / 2;
    const std::vector<Complex>& tw = splitTwiddles(n);
    std::vector<Complex> z(m);
    for (size_t k = 0; k < m; ++k) {
      const Complex xk = X[k], xm = std::conj(X[m - k]);
      const Complex even = 0.5 * (xk + xm);
      const Complex odd = mulConj(0.5 * (xk - xm), tw[k]);  // divided by exp(-2 pi i k / n)
      // conj(even + i odd): the inverse transform as a forward one of the conjugate
      z[k] = Complex(even.real() - odd.imag(), -(even.imag() + odd.real()));
    }
    forwardAny(z);
    const double scale = 1.0 / static_cast<double>(m);
    for (size_t k = 0; k < m; ++k) {
      out[2 * k] = z[k].real() * scale;
      out[2 * k + 1] = -z[k].imag() * scale;
    }
    return out;
  }
  std::vector<Complex> a(n);
  for (size_t k = 0; k < n; ++k) a[k] = k < bins ? X[k] : std::conj(X[n - k]);
  dft(a, true);
  for (size_t k = 0; k < n; ++k) out[k] = a[k].real();
  return out;
}

}  // namespace magmora::cab::spectral
