// Copyright 2026 XeniOS contributors. Released under the BSD-3-Clause license.
#include "xenia/cpu/backend/static/static_backend.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <thread>
#include "xenia/base/clock.h"
#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/base/threading.h"
#include "xenia/cpu/backend/assembler.h"
#include "xenia/cpu/backend/static/sha256.h"
#include "xenia/cpu/function.h"
#include "xenia/cpu/module.h"
#include "xenia/cpu/processor.h"
#include "xenia/cpu/thread_state.h"
#include "xenia/cpu/xex_module.h"
#include "xenia/kernel/guest_scheduler.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/xthread.h"
#include "xenia/memory.h"

DEFINE_path(static_export_path, "",
            "Capture post-load code images here without executing guest code. "
            "Use cpu=static.",
            "CPU");
DEFINE_uint64(static_instruction_limit, 0,
              "Stop static execution after this many instructions per entry (0 "
              "= unlimited).",
              "CPU");

namespace xe::cpu::backend::statik {
namespace {
aot::CpuView View(ppc::PPCContext* p) {
  return {
      p->r,
      p->f,
      reinterpret_cast<unsigned char*>(p->v),
      &p->lr,
      &p->ctr,
      &p->msr,
      &p->fpscr.value,
      &p->vrsave,
      &p->xer_ca,
      &p->xer_ov,
      &p->xer_so,
      p,
      [](void* v) { return uint32_t(static_cast<ppc::PPCContext*>(v)->cr()); },
      [](void* v, uint32_t cr) {
        static_cast<ppc::PPCContext*>(v)->set_cr(cr);
      }};
}
bool SpanAllowed(Memory* memory, uint32_t address, uint32_t size, bool write) {
  if (!size || size > 8 || uint64_t(address) + size > 0x100000000ull) {
    return false;
  }
  uint64_t cursor = address, end = cursor + size;
  while (cursor < end) {
    auto heap = memory->LookupHeap(uint32_t(cursor));
    HeapAllocationInfo info{};
    if (!heap || !heap->QueryRegionInfo(uint32_t(cursor), &info) ||
        !(info.state & kMemoryAllocationCommit) ||
        (write ? !IsWritableProtect(info.protect)
               : !(info.protect & kMemoryProtectRead))) {
      return false;
    }
    const uint64_t next = uint64_t(info.base_address) + info.region_size;
    if (next <= cursor) {
      return false;
    }
    cursor = std::min(next, end);
  }
  return true;
}
class StaticFunction final : public GuestFunction {
 public:
  StaticFunction(Module* module, uint32_t address)
      : GuestFunction(module, address) {
    set_end_address(address + 4);
  }
  uint8_t* machine_code() const override { return nullptr; }
  size_t machine_code_length() const override { return 0; }

 protected:
  bool CallImpl(ThreadState* thread, uint32_t return_address) override {
    return static_cast<StaticBackend*>(thread->processor()->backend())
        ->Execute(thread, address(), return_address);
  }
};
}  // namespace
class StaticHost final : public aot::Host {
 public:
  StaticHost(StaticBackend* backend, ThreadState* thread)
      : backend_(backend),
        thread_(thread),
        memory_(backend->processor()->memory()) {}
  bool Read(uint32_t address, uint32_t size, uint64_t& value) override {
    if (auto mmio = memory_->LookupVirtualMappedRange(address)) {
      if (!thread_ || size != 4 || address % 4 || !mmio->read) {
        return false;
      }
      value = mmio->read(thread_->context(), mmio->callback_context, address);
      return true;
    }
    if (!SpanAllowed(memory_, address, size, false)) {
      return false;
    }
    for (uint32_t i = 1; i < size; ++i) {
      if (memory_->LookupVirtualMappedRange(address + i)) {
        return false;
      }
    }
    xe::global_critical_region region;
    memory_->TriggerPhysicalMemoryCallbacks(region.Acquire(), address, size,
                                            false, false);
    value = 0;
    for (uint32_t i = 0; i < size; ++i) {
      value = (value << 8) | *memory_->TranslateVirtual(address + i);
    }
    return true;
  }
  bool Write(uint32_t address, uint32_t size, uint64_t value) override {
    if (auto mmio = memory_->LookupVirtualMappedRange(address)) {
      if (!thread_ || size != 4 || address % 4 || !mmio->write) {
        return false;
      }
      mmio->write(thread_->context(), mmio->callback_context, address,
                  uint32_t(value));
      return true;
    }
    if (!SpanAllowed(memory_, address, size, true)) {
      return false;
    }
    for (uint32_t i = 1; i < size; ++i) {
      if (memory_->LookupVirtualMappedRange(address + i)) {
        return false;
      }
    }
    xe::global_critical_region region;
    memory_->TriggerPhysicalMemoryCallbacks(region.Acquire(), address, size,
                                            true, false);
    for (uint32_t i = 0; i < size; ++i) {
      *memory_->TranslateVirtual(address + i) =
          uint8_t(value >> ((size - i - 1) * 8));
    }
    return true;
  }
  bool Fetch(uint32_t address, uint32_t& word) override {
    // Never read MMIO to validate code: instruction identity checks must not
    // trigger a device side effect. XEX code lives in ordinary guest memory.
    if (address % 4 || !SpanAllowed(memory_, address, 4, false)) {
      return false;
    }
    word = 0;
    for (uint32_t i = 0; i < 4; ++i) {
      if (memory_->LookupVirtualMappedRange(address + i)) {
        return false;
      }
      word = (word << 8) | *memory_->TranslateVirtual(address + i);
    }
    return true;
  }
  uint64_t Clock() override { return xe::Clock::QueryGuestTickCount(); }
  bool StopRequested() override {
    if (backend_->stopped()) {
      return true;
    }
#if XE_PLATFORM_IOS
    return backend_->processor()->title_stop_requested_ios();
#else
    return false;
#endif
  }
  aot::Status CallExternal(aot::Runtime& rt) override {
    return backend_->CallExternal(rt, thread_);
  }

 private:
  StaticBackend* backend_;
  ThreadState* thread_;
  Memory* memory_;
};
class CallbackModule final : public Module {
 public:
  explicit CallbackModule(StaticBackend* b)
      : Module(b->processor()), backend_(b) {}
  const std::string& name() const override { return name_; }
  bool is_executable() const override { return false; }
  bool ContainsAddress(uint32_t address) override {
    return backend_->IsCallback(address);
  }

 protected:
  std::unique_ptr<Function> CreateFunction(uint32_t address) override {
    return std::make_unique<StaticFunction>(this, address);
  }

 private:
  StaticBackend* backend_;
  const std::string name_ = "static-native-callbacks";
};
bool StaticBackend::Initialize(Processor* processor) {
  if (!Backend::Initialize(processor)) {
    return false;
  }
  stopped_ = false;
  processor->AddModule(std::make_unique<CallbackModule>(this));
  XELOGI(
      "Static AOT backend: {} prelinked modules; runtime CPU code generation "
      "disabled",
      LinkedModules().size());
  return true;
}
StaticBackend::~StaticBackend() {
  // Processor destroys guest modules before the backend; Memory outlives both.
  for (auto address : allocated_callbacks_) {
    processor_->memory()->SystemHeapFree(address);
  }
}
bool StaticBackend::IsExportOnly() const {
  return !cvars::static_export_path.empty();
}
std::unique_ptr<Assembler> StaticBackend::CreateAssembler() {
  XELOGE("Static backend does not create runtime assemblers");
  return nullptr;
}
std::unique_ptr<GuestFunction> StaticBackend::CreateGuestFunction(
    Module* module, uint32_t address) {
  return std::make_unique<StaticFunction>(module, address);
}
bool StaticBackend::BindFunction(GuestFunction* function) {
  return !IsExportOnly() &&
         (function->behavior() == Function::Behavior::kExtern ||
          IsCallback(function->address()) ||
          registry_.Lookup(function->address()));
}
void StaticBackend::ForgetModule(const std::string& name) {
  registry_.Unbind(name);
}
bool StaticBackend::PrepareModule(Module* module) {
  StaticHost host(this, nullptr);
  if (IsExportOnly()) {
    auto* xex = dynamic_cast<XexModule*>(module);
    if (!xex) {
      XELOGE("Static capture currently supports XEX modules only");
      return false;
    }
    auto sections = xex->pe_sections();
    std::sort(
        sections.begin(), sections.end(),
        [](const auto& a, const auto& b) { return a.address < b.address; });
    const auto name = xex->name();
    if (name.empty() ||
        name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTU"
                               "VWXYZ0123456789._-") != std::string::npos ||
        name == "." || name == "..") {
      XELOGE("Invalid module name for static capture");
      return false;
    }
    const auto destination = cvars::static_export_path / name;
    const auto temporary = cvars::static_export_path / (name + ".partial");
    bool owns_temporary = false;
    try {
      std::filesystem::create_directories(cvars::static_export_path);
      if (std::filesystem::exists(destination) ||
          !std::filesystem::create_directory(temporary)) {
        XELOGE("Refusing to overwrite static capture: {}",
               destination.string());
        return false;
      }
      owns_temporary = true;
      std::ofstream ignore(temporary / ".gitignore");
      ignore << "*\n";
      ignore.close();
      std::ostringstream manifest;
      manifest << "{\"format\":\"xenios-aot-image-v1\",\"abi\":1,\"name\":\""
               << name << "\",\"ranges\":[";
      bool first = true;
      size_t index = 0;
      uint64_t total = 0;
      for (const auto& section : sections) {
        if (!(section.flags & kXEPESectionMemoryExecute) || !section.size) {
          continue;
        }
        if (section.address % 4 || section.size % 4 ||
            uint64_t(section.address) + section.size > 0x100000000ull ||
            (total += section.size) > 64u * 1024u * 1024u) {
          throw std::runtime_error("invalid executable range");
        }
        const auto filename = "code" + std::to_string(index++) + ".bin";
        std::ofstream output(temporary / filename, std::ios::binary);
        output.exceptions(std::ios::failbit | std::ios::badbit);
        aot::Sha256 hash;
        for (uint64_t a = section.address;
             a < uint64_t(section.address) + section.size; a += 4) {
          uint32_t word;
          if (!host.Fetch(uint32_t(a), word)) {
            throw std::runtime_error("unreadable code");
          }
          const uint8_t bytes[]{uint8_t(word >> 24), uint8_t(word >> 16),
                                uint8_t(word >> 8), uint8_t(word)};
          hash.Update(bytes);
          output.write(reinterpret_cast<const char*>(bytes), 4);
        }
        output.close();
        std::ostringstream hex;
        for (auto b : hash.Finish()) {
          hex << std::hex << std::setw(2) << std::setfill('0') << unsigned(b);
        }
        if (!first) {
          manifest << ',';
        }
        first = false;
        manifest << "{\"base\":" << section.address
                 << ",\"size\":" << section.size << ",\"file\":\"" << filename
                 << "\",\"sha256\":\"" << hex.str() << "\"}";
      }
      if (first) {
        throw std::runtime_error("no executable sections");
      }
      manifest << "]}\n";
      std::ofstream json(temporary / "image.json");
      json.exceptions(std::ios::failbit | std::ios::badbit);
      json << manifest.str();
      json.close();
      std::filesystem::rename(temporary, destination);
      XELOGI(
          "Static post-load image captured to {}. No guest instructions were "
          "executed.",
          destination.string());
      return true;
    } catch (const std::exception& e) {
      std::error_code ignored;
      if (owns_temporary) {
        std::filesystem::remove_all(temporary, ignored);
      }
      XELOGE("Static capture failed: {}", e.what());
      return false;
    }
  }
  for (const auto* candidate : LinkedModules()) {
    if (candidate && candidate->name && module->name() == candidate->name) {
      std::string error;
      if (registry_.Bind(*candidate, host, error)) {
        return true;
      }
      XELOGE("Static module {} rejected: {}", module->name(), error);
      return false;
    }
  }
  XELOGE(
      "No prelinked static module for {}. Export, compile, link and sign the "
      "matching module; no JIT fallback is permitted.",
      module->name());
  return false;
}
bool StaticBackend::IsCallback(uint32_t address) const {
  std::lock_guard lock(callback_mutex_);
  return callbacks_.contains(address);
}
uint32_t StaticBackend::CreateGuestTrampoline(GuestTrampolineProc proc, void* a,
                                              void* b, bool) {
  if (!proc || IsExportOnly()) {
    return 0;
  }
  const uint32_t address = processor_->memory()->SystemHeapAlloc(4);
  if (!address) {
    return 0;
  }
  std::lock_guard lock(callback_mutex_);
  callbacks_.emplace(address, Callback{proc, a, b});
  allocated_callbacks_.push_back(address);
  return address;
}
void StaticBackend::FreeGuestTrampoline(uint32_t address) {
  {
    std::lock_guard lock(callback_mutex_);
    callbacks_.erase(address);
  }
  processor_->RemoveFunctionByAddress(address);
}
aot::Status StaticBackend::CallExternal(aot::Runtime& rt, ThreadState* thread) {
  if (!thread) {
    return aot::Status::kMissingEntry;
  }
  Callback cb{};
  {
    std::lock_guard lock(callback_mutex_);
    auto it = callbacks_.find(rt.pc);
    if (it != callbacks_.end()) {
      cb = it->second;
    }
  }
  const uint32_t resume = uint32_t(*rt.cpu.lr);
  if (cb.proc) {
    cb.proc(thread->context(), cb.a, cb.b);
    rt.pc = resume;
    return aot::Status::kContinue;
  }
  auto module = processor_->LookupModule(rt.pc);
  auto symbol = module ? module->LookupSymbol(rt.pc) : nullptr;
  if (!symbol || symbol->type() != Symbol::Type::kFunction) {
    return aot::Status::kMissingEntry;
  }
  auto function = static_cast<Function*>(symbol);
  if (function->behavior() == Function::Behavior::kBuiltin) {
    if (!function->Call(thread, resume)) {
      return rt.Fail(aot::Status::kMissingImport, rt.pc);
    }
    rt.pc = resume;
    return aot::Status::kContinue;
  }
  if (function->behavior() != Function::Behavior::kExtern) {
    return aot::Status::kMissingEntry;
  }
  // Only loader-registered and unchanged sc-2 import thunks may enter HLE.
  constexpr uint32_t expected[]{0x44000042, 0x4e800020, 0x60000000, 0x60000000};
  for (unsigned i = 0; i < 4; ++i) {
    uint32_t word;
    if (uint64_t(rt.pc) + i * 4 > 0xfffffffcull ||
        !rt.host.Fetch(rt.pc + i * 4, word)) {
      return rt.Fail(aot::Status::kMemoryFault, rt.pc);
    }
    ++rt.counters.code_checks;
    if (word != expected[i]) {
      return rt.Fail(aot::Status::kCodeMismatch, word);
    }
  }
  auto handler = static_cast<GuestFunction*>(function)->extern_handler();
  if (!handler) {
    return rt.Fail(aot::Status::kMissingImport, rt.pc);
  }
  handler(thread->context(), thread->context()->kernel_state);
  rt.pc = resume;
  return aot::Status::kContinue;
}
bool StaticBackend::Execute(ThreadState* thread, uint32_t entry,
                            uint32_t return_address) {
  if (!thread || IsExportOnly() || stopped()) {
    return false;
  }
  StaticHost host(this, thread);
  aot::Runtime runtime(View(thread->context()), host, registry_);
  constexpr uint64_t quantum = 4096;
  const uint64_t limit = cvars::static_instruction_limit;
  uint64_t budget = limit ? std::min(quantum, limit) : quantum;
  auto status = runtime.Run(entry, return_address, budget);
  while (status == aot::Status::kYield) {
    if (limit &&
        runtime.counters.instructions + runtime.counters.host_calls >= limit) {
      XELOGE("Static execution instruction limit reached at {:08X}",
             runtime.pc);
      return false;
    }
    if (kernel::XThread::GetCurrentFiberThread() &&
        thread->context()->kernel_state) {
      thread->context()->kernel_state->guest_scheduler()->YieldCurrentThread();
    } else {
      std::this_thread::yield();
    }
    budget = limit ? std::min(quantum, limit - runtime.counters.instructions -
                                           runtime.counters.host_calls)
                   : quantum;
    status = runtime.Resume(budget);
  }
  if (status == aot::Status::kReturned) {
    return true;
  }
  stopped_.store(true, std::memory_order_release);
  XELOGE(
      "Static execution stopped: {} PC={:08X} detail={:08X}, instructions={}, "
      "native_dispatches={}, HLE_calls={}. No fallback.",
      aot::StatusName(status), runtime.pc, runtime.detail,
      runtime.counters.instructions, runtime.counters.dispatches,
      runtime.counters.host_calls);
  return false;
}
}  // namespace xe::cpu::backend::statik
