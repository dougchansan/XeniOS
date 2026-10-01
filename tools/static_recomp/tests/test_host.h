// Copyright 2026 XeniOS contributors. BSD-3-Clause.
#pragma once
#include <iostream>
#include <map>
#include <stdexcept>
#include "xenia/cpu/backend/static/runtime.h"
namespace xe::cpu::aot::test {
struct State {
  uint64_t r[32]{}, lr = 0xBCBCBCBC, ctr = 0, msr = 0;
  double f[32]{};
  unsigned char v[128 * 16]{};
  uint32_t cr = 0, fpscr = 0, vrsave = 0;
  uint8_t ca = 0, ov = 0, so = 0;
  CpuView view() {
    return {r,
            f,
            v,
            &lr,
            &ctr,
            &msr,
            &fpscr,
            &vrsave,
            &ca,
            &ov,
            &so,
            this,
            [](void* p) { return static_cast<State*>(p)->cr; },
            [](void* p, uint32_t v) { static_cast<State*>(p)->cr = v; }};
  }
};
struct TestHost : Host {
  std::map<uint32_t, uint8_t> bytes;
  bool stop = false;
  std::function<Status(Runtime&)> external;
  bool Read(uint32_t a, uint32_t size, uint64_t& v) override {
    if (!size || size > 8 || uint64_t(a) + size > 0x100000000ull) {
      return false;
    }
    v = 0;
    for (uint32_t i = 0; i < size; ++i) {
      auto it = bytes.find(a + i);
      if (it == bytes.end()) {
        return false;
      }
      v = (v << 8) | it->second;
    }
    return true;
  }
  bool Write(uint32_t a, uint32_t size, uint64_t v) override {
    if (!size || size > 8 || uint64_t(a) + size > 0x100000000ull) {
      return false;
    }
    for (uint32_t i = 0; i < size; ++i) {
      if (!bytes.contains(a + i)) {
        return false;
      }
    }
    for (uint32_t i = 0; i < size; ++i) {
      bytes[a + i] = uint8_t(v >> ((size - i - 1) * 8));
    }
    return true;
  }
  bool Fetch(uint32_t a, uint32_t& w) override {
    uint64_t v;
    if (!Read(a, 4, v)) {
      return false;
    }
    w = uint32_t(v);
    return true;
  }
  bool StopRequested() override { return stop; }
  uint64_t Clock() override { return 123456; }
  Status CallExternal(Runtime& e) override {
    return external ? external(e) : Status::kMissingEntry;
  }
  void Map(uint32_t a, std::span<const uint8_t> data) {
    for (auto b : data) {
      bytes[a++] = b;
    }
  }
};
inline void Require(bool ok, const char* msg) {
  if (!ok) {
    throw std::runtime_error(msg);
  }
}
}  // namespace xe::cpu::aot::test
