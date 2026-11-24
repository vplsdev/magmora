#include "common/MatVec.h"

#include <array>

namespace magmora {
namespace {

constexpr int kMaxFixedPairs = 20;

template <int Pairs>
void fixedRows(const double* M, int stride, int cols, const double* x, const double* c, double* out) {
  mulAdd<Pairs>(M, stride, cols, x, c, out);
}

// More rows than there are registers (only with many probes attached).
void anyRows(const double* M, int stride, int cols, const double* x, const double* c, double* out) {
  for (int r = 0; r < stride; ++r) {
    double sum = c[r];
    for (int k = 0; k < cols; ++k) sum += M[k * stride + r] * x[k];
    out[r] = sum;
  }
}

template <std::size_t... P>
constexpr std::array<MulAddFn, sizeof...(P)> table(std::index_sequence<P...>) {
  return {{&fixedRows<static_cast<int>(P)>...}};
}

}  // namespace

MulAddFn selectMulAdd(int rows) {
  static constexpr auto kTable = table(std::make_index_sequence<kMaxFixedPairs + 1>());
  return rows / 2 <= kMaxFixedPairs ? kTable[static_cast<size_t>(rows / 2)] : &anyRows;
}

}  // namespace magmora
