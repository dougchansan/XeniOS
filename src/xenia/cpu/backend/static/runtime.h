// Copyright 2026 XeniOS contributors. Released under the BSD-3-Clause license.
// Portable ABI shared by generated code, the XeniOS adapter, and native tests.
#ifndef XENIA_CPU_BACKEND_STATIC_RUNTIME_H_
#define XENIA_CPU_BACKEND_STATIC_RUNTIME_H_

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <mutex>
#include <shared_mutex>
#include <span>
#include <string>
#include <vector>

namespace xe::cpu::aot {
inline constexpr uint32_t kAbiVersion = 1;
inline constexpr uint32_t kInstructionAlignment = 4;

enum class Status : uint32_t {
  kContinue,
  kReturned,
  kYield,
  kStopped,
  kMissingEntry,
  kCodeMismatch,
  kUnsupportedInstruction,
  kMemoryFault,
  kMissingImport,
  kInvalidModule,
  kArithmeticFault,
  kUnsupportedState,
};
const char* StatusName(Status status);

// All pointers alias emulator-owned state. There is no duplicate guest memory
// or register file to marshal when an HLE callback re-enters guest code.
struct CpuView {
  uint64_t* r = nullptr;
  double* f = nullptr;
  unsigned char* v =
      nullptr;  // 128 registers, 16 bytes each, Xenia lane layout.
  uint64_t* lr = nullptr;
  uint64_t* ctr = nullptr;
  uint64_t* msr = nullptr;
  uint32_t* fpscr = nullptr;
  uint32_t* vrsave = nullptr;
  uint8_t* xer_ca = nullptr;
  uint8_t* xer_ov = nullptr;
  uint8_t* xer_so = nullptr;
  void* owner = nullptr;
  uint32_t (*read_cr)(void*) = nullptr;
  void (*write_cr)(void*, uint32_t) = nullptr;
  bool valid() const;
  uint32_t cr() const { return read_cr(owner); }
  void cr(uint32_t value) const { write_cr(owner, value); }
  void cr_field(uint32_t field, uint32_t value) const {
    const uint32_t shift = (7u - field) * 4u;
    cr((cr() & ~(15u << shift)) | ((value & 15u) << shift));
  }
  bool cr_bit(uint32_t bit) const { return ((cr() >> (31u - bit)) & 1u) != 0; }
  void record(uint64_t value) const {
    cr_field(0, (std::bit_cast<int32_t>(uint32_t(value)) < 0 ? 8u
                 : uint32_t(value)                           ? 4u
                                                             : 2u) |
                    (*xer_so != 0));
  }
};

class Runtime;
using Entry = Status (*)(Runtime&);
struct Chunk {
  uint32_t begin;
  uint64_t end;  // exclusive; permits a range ending at 2^32.
  Entry entry;
};
struct CodeRange {
  uint32_t begin;
  uint64_t end;
  std::array<uint8_t, 32> sha256;
};
struct Module {
  uint32_t abi;
  const char* name;
  std::span<const CodeRange> ranges;
  std::span<const Chunk> chunks;
};

// Values crossing this boundary are host integers in guest numeric order.
// Adapters perform endian conversion, MMIO, access checking and GPU tracking.
class Host {
 public:
  virtual ~Host() = default;
  virtual bool Read(uint32_t address, uint32_t size, uint64_t& value) = 0;
  virtual bool Write(uint32_t address, uint32_t size, uint64_t value) = 0;
  virtual bool Fetch(uint32_t address, uint32_t& word) = 0;
  virtual bool StopRequested() { return false; }
  virtual Status CallExternal(Runtime&) { return Status::kMissingEntry; }
  virtual uint64_t Clock() = 0;
  // Reservation semantics must be supplied by the owning emulator; never use a
  // private monitor alongside a JIT/kernel monitor. Unsupported fails closed.
  virtual bool ReserveLoad(uint32_t, uint32_t, uint64_t&) { return false; }
  virtual bool ConditionalStore(uint32_t, uint32_t, uint64_t, bool&) {
    return false;
  }
};

class Registry {
 public:
  // Atomic registration: malformed/overlapping/unverified modules add nothing.
  bool Bind(const Module& module, Host& host, std::string& error);
  void Unbind(const std::string& name);
  Entry Lookup(uint32_t pc) const;
  size_t size() const;

 private:
  struct Binding {
    std::string name;
    std::vector<Chunk> chunks;
  };
  mutable std::shared_mutex mutex_;
  std::vector<Binding> bindings_;
};

struct Counters {
  uint64_t instructions = 0;
  uint64_t dispatches = 0;
  uint64_t host_calls = 0;
  uint64_t code_checks = 0;
  uint64_t yields = 0;
  // No fallback engine exists in this ABI.
};

class Runtime {
 public:
  Runtime(CpuView view, Host& host, const Registry& registry)
      : cpu(view), host(host), registry(registry) {}
  Status Run(uint32_t entry, uint32_t return_address, uint64_t budget);
  Status Resume(uint64_t budget);
  bool Tick(uint32_t address, uint32_t expected_word);
  bool Load(uint32_t address, uint32_t size, uint64_t& value);
  bool Store(uint32_t address, uint32_t size, uint64_t value);
  bool Reserve(uint32_t address, uint32_t size, uint64_t& value);
  bool StoreConditional(uint32_t address, uint32_t size, uint64_t value);
  Status Fail(Status why, uint32_t detail = 0);
  CpuView cpu;
  Host& host;
  const Registry& registry;
  uint32_t pc = 0;
  uint32_t return_pc = 0;
  uint32_t detail = 0;
  Status status = Status::kContinue;
  uint64_t remaining = 0;
  Counters counters;
};

// Defined-bit arithmetic helpers avoid C++ signed overflow and over-wide
// shifts.
inline uint64_t SignExtend(uint64_t value, unsigned bits) {
  const uint64_t sign = uint64_t{1} << (bits - 1);
  const uint64_t mask = bits == 64 ? ~uint64_t{0} : (uint64_t{1} << bits) - 1;
  return ((value & mask) ^ sign) - sign;
}
inline uint64_t Mask(unsigned begin, unsigned end, unsigned width = 64) {
  const uint64_t all = width == 64 ? ~uint64_t{0} : 0xFFFFFFFFull;
  const uint64_t left = all >> begin;
  const uint64_t right = end == width - 1 ? 0 : all >> (end + 1);
  return begin <= end ? (left ^ right) : ((~(left ^ right)) & all);
}
inline uint64_t Add(CpuView& c, uint64_t a, uint64_t b, uint64_t carry,
                    bool set_carry, bool set_overflow) {
  const uint64_t partial = a + b, result = partial + carry;
  if (set_carry) {
    *c.xer_ca = (uint64_t(uint32_t(a)) + uint32_t(b) + carry) >> 32;
  }
  if (set_overflow) {
    *c.xer_ov =
        ((~(uint32_t(a) ^ uint32_t(b)) & (uint32_t(a) ^ uint32_t(result))) >>
         31) != 0;
    *c.xer_so |= *c.xer_ov;
  }
  return result;
}
inline uint64_t ArithmeticShift(uint64_t value, unsigned shift, unsigned width,
                                uint8_t* carry) {
  const uint64_t mask = width == 64 ? ~uint64_t{0} : 0xFFFFFFFFull;
  value &= mask;
  const bool negative = (value >> (width - 1)) != 0;
  const uint64_t shifted_out = shift >= width ? value
                               : shift ? value & (mask >> (width - shift))
                                       : 0;
  if (carry) {
    *carry = negative && shifted_out != 0;
  }
  if (shift >= width) {
    return negative ? ~uint64_t{0} : 0;
  }
  uint64_t result = value >> shift;
  if (negative && shift) {
    result |= mask ^ (mask >> shift);
  }
  return width == 32 ? SignExtend(result, 32) : result;
}
inline uint64_t MultiplyHigh(uint64_t a, uint64_t b, bool is_signed) {
  const uint64_t a0 = uint32_t(a), a1 = a >> 32, b0 = uint32_t(b), b1 = b >> 32;
  const uint64_t low = a0 * b0;
  const uint64_t middle = a1 * b0 + (low >> 32);
  const uint64_t carry = (middle & 0xFFFFFFFFull) + a0 * b1;
  uint64_t high = a1 * b1 + (middle >> 32) + (carry >> 32);
  if (is_signed) {
    if (a >> 63) {
      high -= b;
    }
    if (b >> 63) {
      high -= a;
    }
  }
  return high;
}
// Xenia's vector byte representation is reversed per 32-bit lane, not as a
// single reversed 128-bit scalar. All vector byte operations use these helpers.
inline uint8_t VectorByte(const CpuView& c, unsigned reg, unsigned guest_byte) {
  return c.v[reg * 16 + (guest_byte ^ 3u)];
}
inline void VectorByte(CpuView& c, unsigned reg, unsigned guest_byte,
                       uint8_t v) {
  c.v[reg * 16 + (guest_byte ^ 3u)] = v;
}
inline uint32_t VectorWord(const CpuView& c, unsigned reg, unsigned lane) {
  uint32_t result;
  std::memcpy(&result, c.v + reg * 16 + lane * 4, 4);
  return result;
}
inline void VectorWord(CpuView& c, unsigned reg, unsigned lane,
                       uint32_t value) {
  std::memcpy(c.v + reg * 16 + lane * 4, &value, 4);
}
}  // namespace xe::cpu::aot
#endif
