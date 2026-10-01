// Copyright 2026 XeniOS contributors. BSD-3-Clause.
#include <iomanip>
#include <random>
#include <sstream>
#include "test_host.h"
#include "xenia/cpu/backend/static/operations.h"
#include "xenia/cpu/backend/static/sha256.h"
using namespace xe::cpu::aot;
using namespace xe::cpu::aot::test;
Status EntryFn(Runtime& e) {
  switch (e.pc) {
    case 0x1000:
      if (!e.Tick(0x1000, 0x3860002a)) {
        return e.status;
      }
      e.cpu.r[3] = 42;
      [[fallthrough]];
    case 0x1004:
      if (!e.Tick(0x1004, 0x4e800020)) {
        return e.status;
      }
      e.pc = uint32_t(*e.cpu.lr);
      return Status::kContinue;
    default:
      return e.Fail(Status::kMissingEntry);
  }
}
std::string Hex(std::array<uint8_t, 32> h) {
  std::ostringstream o;
  for (auto b : h) {
    o << std::hex << std::setw(2) << std::setfill('0') << unsigned(b);
  }
  return o.str();
}
int main() {
  try {
    Sha256 empty;
    Require(
        Hex(empty.Finish()) ==
            "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
        "SHA empty");
    Sha256 abc;
    const uint8_t a[]{'a'}, bc[]{'b', 'c'};
    abc.Update(a);
    abc.Update(bc);
    Require(
        Hex(abc.Finish()) ==
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
        "SHA abc");
    Sha256 million;
    uint8_t byte = 'a';
    for (int i = 0; i < 1000000; ++i) {
      million.Update({&byte, 1});
    }
    Require(
        Hex(million.Finish()) ==
            "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0",
        "SHA million");
    TestHost host;
    const uint8_t data[]{0x38, 0x60, 0, 0x2a, 0x4e, 0x80, 0, 0x20};
    host.Map(0x1000, data);
    Sha256 h;
    h.Update(data);
    CodeRange ranges[]{{0x1000, 0x1008, h.Finish()}};
    Chunk chunks[]{{0x1000, 0x1008, EntryFn}};
    Module module{kAbiVersion, "fixture", ranges, chunks};
    Registry reg;
    std::string error;
    Require(reg.Bind(module, host, error), "bind");
    Require(!reg.Bind(module, host, error) && reg.size() == 1,
            "duplicate atomicity");
    State state;
    Runtime rt(state.view(), host, reg);
    Require(rt.Run(0x1000, 0xBCBCBCBC, 1) == Status::kYield &&
                state.r[3] == 42 && rt.pc == 0x1004,
            "budget");
    Require(rt.Resume(1) == Status::kReturned, "resume");
    Require(rt.counters.instructions == 2 && rt.counters.code_checks == 2,
            "instruction evidence");
    host.bytes[0x1000] ^= 1;
    Require(rt.Run(0x1000, 0xBCBCBCBC, 5) == Status::kCodeMismatch,
            "SMC guard");
    host.bytes[0x1000] ^= 1;
    Require(rt.Run(0x2000, 0xBCBCBCBC, 5) == Status::kMissingEntry,
            "strict missing entry");
    host.stop = true;
    Require(rt.Run(0x1000, 0xBCBCBCBC, 5) == Status::kStopped, "stop");
    host.stop = false;
    Module wrong = module;
    wrong.abi = 999;
    Registry reject;
    Require(!reject.Bind(wrong, host, error) && reject.size() == 0, "ABI");
    ranges[0].sha256[0] ^= 1;
    Require(!reject.Bind(module, host, error) && reject.size() == 0,
            "identity");
    ranges[0].sha256[0] ^= 1;
    chunks[0].end -= 4;
    Require(!reject.Bind(module, host, error) && reject.size() == 0, "tiling");
    chunks[0].end += 4;
    Module overlap = module;
    overlap.name = "other";
    Require(!reg.Bind(overlap, host, error), "overlap");
    host.external = [](Runtime& e) {
      if (e.pc != 0x2000) {
        return Status::kMissingEntry;
      }
      e.cpu.r[3] += 1;
      e.pc = uint32_t(*e.cpu.lr);
      return Status::kContinue;
    };
    Require(rt.Run(0x2000, 0xBCBCBCBC, 1) == Status::kReturned &&
                state.r[3] == 43 && rt.counters.host_calls == 1,
            "compiled host call");
    reg.Unbind("fixture");
    Require(rt.Run(0x1000, 0xBCBCBCBC, 1) == Status::kMissingEntry, "unload");
    uint64_t value;
    Require(!rt.Load(0xffffffff, 8, value) && rt.status == Status::kMemoryFault,
            "address overflow");
    Require(
        !rt.Reserve(0x1000, 4, value) && rt.status == Status::kUnsupportedState,
        "no private reservation fallback");
    std::mt19937_64 rng(2026);
    auto c = state.view();
    for (int i = 0; i < 100000; ++i) {
      uint64_t x = rng(), y = rng();
#if defined(__SIZEOF_INT128__)
      Require(MultiplyHigh(x, y, false) ==
                  uint64_t((static_cast<unsigned __int128>(x) * y) >> 64),
              "unsigned high multiply");
      Require(MultiplyHigh(x, y, true) ==
                  uint64_t((static_cast<__int128>(std::bit_cast<int64_t>(x)) *
                            std::bit_cast<int64_t>(y)) >>
                           64),
              "signed high multiply");
#endif
      uint64_t z = Add(c, x, y, 0, true, true);
      Require(z == x + y &&
                  state.ca == ((uint64_t(uint32_t(x)) + uint32_t(y)) >> 32),
              "carry");
    }
    Require(Mask(0, 31, 32) == 0xffffffffull &&
                Mask(60, 3) == 0xf00000000000000full,
            "rotate mask");
    for (unsigned s = 0; s < 128; ++s) {
      uint8_t ca;
      auto v = ArithmeticShift(0x80000001, s, 32, &ca);
      Require(
          v == (s >= 32 ? ~uint64_t(0)
                        : SignExtend(uint32_t(int32_t(0x80000001) >> s), 32)),
          "sraw");
    }
    std::cout << "runtime checks passed; 100000 randomized multiply/carry "
                 "vectors; no JIT entry exists\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
