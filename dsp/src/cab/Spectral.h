// Double-precision discrete Fourier transforms for the virtual microphone's response building (XP6, worker thread;
// they allocate). Semantics follow numpy.fft: rfft zero-pads or truncates to n and returns n / 2 + 1 bins; irfft
// takes n / 2 + 1 bins, ignores the imaginary parts of the first and (n even) last, and scales by 1 / n. Any size:
// powers of two by an iterative radix-2 transform, other sizes by Bluestein's chirp transform on top of it.
#pragma once
#include <complex>
#include <cstddef>
#include <vector>

namespace magmora::cab::spectral {

using Complex = std::complex<double>;

// In place, length a.size() (any). inverse: conjugate transform scaled by 1 / N (numpy.fft.ifft).
void dft(std::vector<Complex>& a, bool inverse);
std::vector<Complex> rfft(const double* x, size_t length, size_t n);
inline std::vector<Complex> rfft(const std::vector<double>& x, size_t n) { return rfft(x.data(), x.size(), n); }
std::vector<double> irfft(const std::vector<Complex>& X, size_t n);

}  // namespace magmora::cab::spectral
