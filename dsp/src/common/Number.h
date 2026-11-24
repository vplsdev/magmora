// Text to double in the C locale's format, whatever LC_NUMERIC the process has set. std::strtod follows the
// process locale: under a decimal-comma locale (de_DE, fr_FR) it stops at the '.' of "7.5". A plug-in shares its
// process with the host and with other plug-ins, any of which may call setlocale. Not for the audio thread.
#pragma once

namespace magmora {

// As std::strtod in the "C" locale: *end (if end is given) points past the last character used, or at text.
double parseDouble(const char* text, char** end);

}  // namespace magmora

#include <string>

namespace magmora {

// A double as text with '.' for the decimal point whatever LC_NUMERIC says (std::snprintf follows the locale), to
// 1e-9: the shortest of "7", "7.5", "0.71", "-96". Finite values only (anything else is written as 0).
std::string formatDouble(double value);

}  // namespace magmora
