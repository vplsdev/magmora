#include "common/Number.h"

#include <clocale>
#include <cstdlib>

#if defined(_WIN32)
#include <locale.h>
#elif defined(__APPLE__)
#include <xlocale.h>
#else
#include <locale.h>
#endif

namespace magmora {

#if defined(_WIN32)
double parseDouble(const char* text, char** end) {
  static const _locale_t c = _create_locale(LC_NUMERIC, "C");  // made once, kept for the life of the process
  return _strtod_l(text, end, c);
}
#else
double parseDouble(const char* text, char** end) {
  static const locale_t c = newlocale(LC_NUMERIC_MASK, "C", static_cast<locale_t>(nullptr));  // made once, kept
  return strtod_l(text, end, c);
}
#endif

}  // namespace magmora

#include <cmath>
#include <cstdint>
#include <string>

namespace magmora {

std::string formatDouble(double value) {
  if (!std::isfinite(value) || std::fabs(value) >= 9.0e9) return std::isfinite(value) ? std::to_string(std::llround(value)) : "0";
  constexpr uint64_t kScale = 1000000000ull;  // nine decimals
  const bool negative = value < 0.0;
  const uint64_t fixed = static_cast<uint64_t>(std::llround(std::fabs(value) * static_cast<double>(kScale)));
  if (fixed == 0) return "0";
  // Integers only from here: std::to_string of an integer never uses the locale's separators.
  std::string text = (negative ? "-" : "") + std::to_string(fixed / kScale);
  uint64_t fraction = fixed % kScale;
  if (fraction) {
    std::string digits = std::to_string(fraction);
    digits.insert(0, 9 - digits.size(), '0');
    while (!digits.empty() && digits.back() == '0') digits.pop_back();
    text += "." + digits;
  }
  return text;
}

}  // namespace magmora
