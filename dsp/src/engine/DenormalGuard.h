// FTZ/DAZ for the duration of a scope; the caller's mode is restored.
#pragma once
#include <cstdint>

#if defined(__x86_64__) || defined(_M_X64)
#include <xmmintrin.h>
#elif defined(_MSC_VER) && defined(_M_ARM64)
#include <intrin.h>
#endif

namespace magmora::engine {

class DenormalGuard {
 public:
  DenormalGuard() : saved_(read()) { write(saved_ | kFlags); }
  ~DenormalGuard() { write(saved_); }
  DenormalGuard(const DenormalGuard&) = delete;
  DenormalGuard& operator=(const DenormalGuard&) = delete;

 private:
#if defined(__aarch64__)
  static constexpr uint64_t kFlags = uint64_t{1} << 24;  // FPCR.FZ
  static uint64_t read() {
    uint64_t value;
    __asm__ __volatile__("mrs %0, fpcr" : "=r"(value));
    return value;
  }
  static void write(uint64_t value) { __asm__ __volatile__("msr fpcr, %0" : : "r"(value)); }
#elif defined(_MSC_VER) && defined(_M_ARM64)
  static constexpr uint64_t kFlags = uint64_t{1} << 24;
  static uint64_t read() { return static_cast<uint64_t>(_ReadStatusReg(ARM64_FPCR)); }
  static void write(uint64_t value) { _WriteStatusReg(ARM64_FPCR, static_cast<__int64>(value)); }
#elif defined(__x86_64__) || defined(_M_X64)
  static constexpr uint64_t kFlags = 0x8040;  // FTZ and DAZ
  static uint64_t read() { return _mm_getcsr(); }
  static void write(uint64_t value) { _mm_setcsr(static_cast<unsigned>(value)); }
#else
  static constexpr uint64_t kFlags = 0;
  static uint64_t read() { return 0; }
  static void write(uint64_t) {}
#endif

  const uint64_t saved_;
};

}  // namespace magmora::engine
