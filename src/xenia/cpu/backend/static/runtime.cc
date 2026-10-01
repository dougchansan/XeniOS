// Copyright 2026 XeniOS contributors. Released under the BSD-3-Clause license.
#include "xenia/cpu/backend/static/runtime.h"
#include <algorithm>
#include <limits>
#include "xenia/cpu/backend/static/sha256.h"

namespace xe::cpu::aot {
const char* StatusName(Status s) {
  switch (s) {
#define S(name)         \
  case Status::k##name: \
    return #name
    S(Continue);
    S(Returned);
    S(Yield);
    S(Stopped);
    S(MissingEntry);
    S(CodeMismatch);
    S(UnsupportedInstruction);
    S(MemoryFault);
    S(MissingImport);
    S(InvalidModule);
    S(ArithmeticFault);
    S(UnsupportedState);
#undef S
  }
  return "InvalidStatus";
}
bool CpuView::valid() const {
  return r && f && v && lr && ctr && msr && fpscr && vrsave && xer_ca &&
         xer_ov && xer_so && owner && read_cr && write_cr;
}
bool Registry::Bind(const Module& m, Host& host, std::string& error) {
  auto fail = [&](const char* message) {
    error = message;
    return false;
  };
  if (m.abi != kAbiVersion || !m.name || !*m.name || m.ranges.empty() ||
      m.chunks.empty()) {
    return fail("invalid module identity or ABI");
  }
  uint64_t previous_end = 0;
  for (const auto& range : m.ranges) {
    if (range.begin % 4 || range.end % 4 || range.end <= range.begin ||
        range.end > (uint64_t{1} << 32) || range.begin < previous_end) {
      return fail("unaligned, overlapping or invalid code range");
    }
    previous_end = range.end;
  }
  size_t chunk_index = 0;
  for (const auto& range : m.ranges) {
    uint64_t cursor = range.begin;
    while (chunk_index < m.chunks.size() &&
           m.chunks[chunk_index].begin < range.end) {
      const auto& chunk = m.chunks[chunk_index++];
      if (!chunk.entry || chunk.begin != cursor || chunk.end <= chunk.begin ||
          chunk.end % 4 || chunk.end > range.end) {
        return fail("chunks must exactly tile all executable ranges");
      }
      cursor = chunk.end;
    }
    if (cursor != range.end) {
      return fail("incomplete executable coverage");
    }
  }
  if (chunk_index != m.chunks.size()) {
    return fail("chunk outside executable ranges");
  }
  // Validate the complete loaded code image, not just a title ID or filename.
  for (const auto& range : m.ranges) {
    Sha256 hash;
    for (uint64_t p = range.begin; p < range.end; p += 4) {
      uint32_t word;
      if (!host.Fetch(uint32_t(p), word)) {
        return fail("code is not readable");
      }
      const uint8_t bytes[]{uint8_t(word >> 24), uint8_t(word >> 16),
                            uint8_t(word >> 8), uint8_t(word)};
      hash.Update(bytes);
    }
    if (hash.Finish() != range.sha256) {
      return fail("loaded executable SHA-256 mismatch");
    }
  }
  std::unique_lock lock(mutex_);
  for (const auto& binding : bindings_) {
    if (binding.name == m.name) {
      return fail("module already bound");
    }
    size_t i = 0, j = 0;
    while (i < binding.chunks.size() && j < m.chunks.size()) {
      const auto& old = binding.chunks[i];
      const auto& added = m.chunks[j];
      if (added.begin < old.end && old.begin < added.end) {
        return fail("module overlaps an active translation");
      }
      if (old.end <= added.begin) {
        ++i;
      } else {
        ++j;
      }
    }
  }
  bindings_.push_back({m.name, {m.chunks.begin(), m.chunks.end()}});
  error.clear();
  return true;
}
void Registry::Unbind(const std::string& name) {
  std::unique_lock lock(mutex_);
  std::erase_if(bindings_, [&](const Binding& b) { return b.name == name; });
}
Entry Registry::Lookup(uint32_t pc) const {
  if (pc % 4) {
    return nullptr;
  }
  std::shared_lock lock(mutex_);
  for (const auto& binding : bindings_) {
    auto it = std::upper_bound(
        binding.chunks.begin(), binding.chunks.end(), pc,
        [](uint32_t address, const Chunk& c) { return address < c.begin; });
    if (it != binding.chunks.begin()) {
      --it;
      if (pc < it->end) {
        return it->entry;
      }
    }
  }
  return nullptr;
}
size_t Registry::size() const {
  std::shared_lock lock(mutex_);
  return bindings_.size();
}
Status Runtime::Fail(Status why, uint32_t information) {
  status = why;
  detail = information;
  return why;
}
bool Runtime::Tick(uint32_t address, uint32_t expected) {
  pc = address;
  if (host.StopRequested()) {
    Fail(Status::kStopped);
    return false;
  }
  if (!remaining) {
    ++counters.yields;
    Fail(Status::kYield);
    return false;
  }
  uint32_t actual;
  if (!host.Fetch(address, actual)) {
    Fail(Status::kMemoryFault, address);
    return false;
  }
  ++counters.code_checks;
  if (actual != expected) {
    Fail(Status::kCodeMismatch, actual);
    return false;
  }
  --remaining;
  ++counters.instructions;
  return true;
}
bool Runtime::Load(uint32_t address, uint32_t size, uint64_t& value) {
  if (!host.Read(address, size, value)) {
    Fail(Status::kMemoryFault, address);
    return false;
  }
  return true;
}
bool Runtime::Store(uint32_t address, uint32_t size, uint64_t value) {
  if (!host.Write(address, size, value)) {
    Fail(Status::kMemoryFault, address);
    return false;
  }
  return true;
}
bool Runtime::Reserve(uint32_t address, uint32_t size, uint64_t& value) {
  if ((size != 4 && size != 8) || (address & (size - 1)) ||
      !host.ReserveLoad(address, size, value)) {
    Fail(Status::kUnsupportedState, address);
    return false;
  }
  return true;
}
bool Runtime::StoreConditional(uint32_t address, uint32_t size,
                               uint64_t value) {
  bool stored = false;
  if ((size != 4 && size != 8) || (address & (size - 1)) ||
      !host.ConditionalStore(address, size, value, stored)) {
    Fail(Status::kUnsupportedState, address);
    return false;
  }
  cpu.cr_field(0, (stored ? 2u : 0u) | (*cpu.xer_so != 0));
  return true;
}
Status Runtime::Run(uint32_t entry, uint32_t return_address, uint64_t budget) {
  if (!cpu.valid() || entry % 4 || return_address % 4) {
    return Fail(Status::kInvalidModule, entry);
  }
  pc = entry;
  return_pc = return_address;
  detail = 0;
  status = Status::kContinue;
  return Resume(budget);
}
Status Runtime::Resume(uint64_t budget) {
  if (status != Status::kContinue && status != Status::kYield) {
    return status;
  }
  remaining = budget;
  status = Status::kContinue;
  while (status == Status::kContinue) {
    if (host.StopRequested()) {
      return Fail(Status::kStopped);
    }
    if (pc == return_pc) {
      return status = Status::kReturned;
    }
    if (pc % 4) {
      return Fail(Status::kMissingEntry, pc);
    }
    if (!remaining) {
      ++counters.yields;
      return status = Status::kYield;
    }
    // HLE thunks and data-backed callback handles are resolved before guest
    // chunks. The host must only accept explicitly registered native services.
    const Status external = host.CallExternal(*this);
    if (external != Status::kMissingEntry) {
      if (external != Status::kContinue) {
        return status = external;
      }
      --remaining;
      ++counters.host_calls;
      continue;
    }
    const auto entry = registry.Lookup(pc);
    if (!entry) {
      return Fail(Status::kMissingEntry, pc);
    }
    const auto old_pc = pc;
    const auto old_remaining = remaining;
    ++counters.dispatches;
    status = entry(*this);
    if (status == Status::kContinue && old_pc == pc &&
        old_remaining == remaining) {
      return Fail(Status::kUnsupportedState,
                  pc);  // Broken native entry cannot spin forever.
    }
  }
  return status;
}
}  // namespace xe::cpu::aot
