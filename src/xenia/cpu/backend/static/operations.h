// Copyright 2026 XeniOS contributors. Released under the BSD-3-Clause license.
#ifndef XENIA_CPU_BACKEND_STATIC_OPERATIONS_H_
#define XENIA_CPU_BACKEND_STATIC_OPERATIONS_H_
#include <atomic>
#include <cfenv>
#include <cmath>
#include <limits>
#include "xenia/cpu/backend/static/runtime.h"
#if defined(__x86_64__) || defined(_M_X64)
#include <xmmintrin.h>
#endif
namespace xe::cpu::aot {
inline uint64_t ByteSwap(uint64_t v, unsigned size) {
  uint64_t out = 0;
  for (unsigned i = 0; i < size; ++i) {
    out = (out << 8) | (v & 255);
    v >>= 8;
  }
  return out;
}
// Keep host/HLE FP state separate. Non-IEEE and enabled guest FP exceptions
// require additional emulation and are rejected rather than silently ignored.
class FpScope {
 public:
  explicit FpScope(Runtime& e) : e_(e) {
    if ((*e.cpu.fpscr & 0xFCu) != 0) {
      e.Fail(Status::kUnsupportedState, *e.cpu.fpscr);
      return;
    }
    if (std::feholdexcept(&saved_) != 0) {
      e.Fail(Status::kUnsupportedState);
      return;
    }
    saved_valid_ = true;
    const int modes[]{FE_TONEAREST, FE_TOWARDZERO, FE_UPWARD, FE_DOWNWARD};
    if (std::fesetround(modes[*e.cpu.fpscr & 3]) != 0) {
      e.Fail(Status::kUnsupportedState);
      return;
    }
#if defined(__x86_64__) || defined(_M_X64)
    _mm_setcsr(_mm_getcsr() & ~((1u << 15) | (1u << 6)));
#elif defined(__aarch64__)
    uint64_t fpcr;
    asm volatile("mrs %0, fpcr" : "=r"(fpcr));
    fpcr &= ~((uint64_t{1} << 24) | (uint64_t{1} << 19) | (uint64_t{1} << 25));
    asm volatile("msr fpcr, %0" ::"r"(fpcr));
#endif
    valid_ = true;
  }
  ~FpScope() {
    if (saved_valid_) {
      std::fesetenv(&saved_);
    }
  }
  bool valid() const { return valid_; }
  void Flags(double result, bool classify = true) {
    const int flags = std::fetestexcept(FE_ALL_EXCEPT);
    uint32_t raised = 0;
    if (flags & FE_INEXACT) {
      raised |= 1u << 25;
    }
    if (flags & FE_DIVBYZERO) {
      raised |= 1u << 26;
    }
    if (flags & FE_UNDERFLOW) {
      raised |= 1u << 27;
    }
    if (flags & FE_OVERFLOW) {
      raised |= 1u << 28;
    }
    // Precise invalid-operation subcategories are not implemented: fail before
    // the destination register is committed, instead of claiming full FPSCR.
    if (flags & FE_INVALID) {
      e_.Fail(Status::kUnsupportedState);
      return;
    }
    if (raised & ~*e_.cpu.fpscr) {
      raised |= 1u << 31;
    }
    *e_.cpu.fpscr |= raised;
    if (classify) {
      const uint64_t bits = std::bit_cast<uint64_t>(result);
      uint32_t code = std::isnan(result) ? 0x11u
                      : result == 0 ? (std::signbit(result) ? 0x12u : 0x02u)
                      : std::isinf(result)
                          ? (std::signbit(result) ? 0x09u : 0x05u)
                      : std::signbit(result) ? 0x08u
                                             : 0x04u;
      if (((bits >> 52) & 2047) == 0 && result != 0) {
        code |= 0x10;
      }
      *e_.cpu.fpscr = (*e_.cpu.fpscr & ~0x1F000u) | (code << 12);
    }
  }

 private:
  Runtime& e_;
  std::fenv_t saved_{};
  bool saved_valid_ = false, valid_ = false;
};
enum class FpOp {
  kAdd,
  kSub,
  kMul,
  kDiv,
  kMadd,
  kMsub,
  kNmadd,
  kNmsub,
  kRoundSingle
};
inline bool FloatOp(Runtime& e, FpOp op, unsigned d, unsigned a, unsigned b,
                    unsigned c, bool single, bool record) {
  FpScope scope(e);
  if (!scope.valid()) {
    return false;
  }
  const volatile double av = e.cpu.f[a], bv = e.cpu.f[b], cv = e.cpu.f[c];
  volatile double out = 0;
  switch (op) {
    case FpOp::kAdd:
      out = av + bv;
      break;
    case FpOp::kSub:
      out = av - bv;
      break;
    case FpOp::kMul:
      out = av * cv;
      break;
    case FpOp::kDiv:
      out = av / bv;
      break;
    case FpOp::kMadd:
      out = std::fma(double(av), double(cv), double(bv));
      break;
    case FpOp::kMsub:
      out = std::fma(double(av), double(cv), -double(bv));
      break;
    case FpOp::kNmadd:
      out = -std::fma(double(av), double(cv), double(bv));
      break;
    case FpOp::kNmsub:
      out = -std::fma(double(av), double(cv), -double(bv));
      break;
    case FpOp::kRoundSingle:
      out = bv;
      break;
  }
  // Single-result arithmetic needs Xenon's operand precision and double-round
  // handling. Permit only explicit frsp until that behavior is verified.
  if (single && op != FpOp::kRoundSingle) {
    e.Fail(Status::kUnsupportedState);
    return false;
  }
  if (single) {
    volatile float rounded = static_cast<float>(out);
    out = double(rounded);
  }
  scope.Flags(out);
  if (e.status != Status::kContinue) {
    return false;
  }
  e.cpu.f[d] = out;
  if (record) {
    e.cpu.cr_field(1, (*e.cpu.fpscr >> 28) & 15);
  }
  return true;
}
inline bool FloatToMemory(Runtime& e, double value, uint64_t& out) {
  FpScope scope(e);
  if (!scope.valid()) {
    return false;
  }
  volatile float rounded = static_cast<float>(value);
  out = std::bit_cast<uint32_t>(float(rounded));
  return true;
}
inline bool FloatCompare(Runtime& e, unsigned field, double a, double b,
                         bool ordered) {
  const auto snan = [](double v) {
    const auto bits = std::bit_cast<uint64_t>(v);
    return (bits & 0x7FF0000000000000ull) == 0x7FF0000000000000ull &&
           (bits & 0x000FFFFFFFFFFFFFull) != 0 &&
           !(bits & 0x0008000000000000ull);
  };
  if (snan(a) || snan(b) || (ordered && (std::isnan(a) || std::isnan(b)))) {
    e.Fail(Status::kUnsupportedState);
    return false;
  }
  const uint32_t value = std::isnan(a) || std::isnan(b) ? 1u
                         : a < b                        ? 8u
                         : a > b                        ? 4u
                                                        : 2u;
  e.cpu.cr_field(field, value);
  *e.cpu.fpscr = (*e.cpu.fpscr & ~0xF000u) | (value << 12);
  return true;
}
}  // namespace xe::cpu::aot
#endif
