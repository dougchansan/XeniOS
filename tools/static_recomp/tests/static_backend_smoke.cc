// Copyright 2026 XeniOS contributors. BSD-3-Clause.
// This links the real XeniOS Memory/Processor/ThreadState/StaticBackend, not
// mocks.
#include <cstring>
#include <iostream>
#include <stdexcept>
#include "fixture.h"
#include "xenia/base/memory.h"
#include "xenia/cpu/backend/static/static_backend.h"
#include "xenia/cpu/export_resolver.h"
#include "xenia/cpu/function.h"
#include "xenia/cpu/module.h"
#include "xenia/cpu/processor.h"
#include "xenia/cpu/thread_state.h"
#include "xenia/memory.h"
#include "xenia/base/cvar.h"

// The smoke target deliberately links the runtime libraries without the full
// frontend executable. emulator.cc references frontend-owned CVars, so provide
// the same definitions here rather than pulling UI translation units into this
// headless strict-runtime test.
DEFINE_string(gpu, "null", "Graphics system.", "GPU");
DEFINE_string(apu, "nop", "Audio system.", "APU");
DEFINE_bool(mount_scratch, false, "Mount scratch device.", "VFS");
DEFINE_bool(mount_cache, false, "Mount cache device.", "VFS");
DEFINE_bool(mount_memory_unit, false, "Mount memory unit.", "VFS");
namespace xe::cpu::backend::statik {
std::span<const aot::Module* const> LinkedModules() {
  static const aot::Module* const modules[]{&SmokeModule()};
  return modules;
}
}  // namespace xe::cpu::backend::statik
using xe::cpu::backend::statik::StaticBackend;
void Require(bool value, const char* message) {
  if (!value) {
    throw std::runtime_error(message);
  }
}
class SmokeGuest final : public xe::cpu::Module {
 public:
  SmokeGuest(xe::cpu::Processor* p) : Module(p) {}
  const std::string& name() const override { return name_; }
  bool is_executable() const override { return true; }
  bool ContainsAddress(uint32_t a) override {
    return a >= 0x82010000 && a < 0x82010000 + sizeof(smoke_code);
  }

 protected:
  std::unique_ptr<xe::cpu::Function> CreateFunction(uint32_t a) override {
    return processor_->backend()->CreateGuestFunction(this, a);
  }

 private:
  std::string name_ = "synthetic-smoke";
};
int main() {
  try {
    xe::Memory memory;
    Require(memory.Initialize(), "Memory::Initialize");
    xe::cpu::ExportResolver exports;
    xe::cpu::Processor processor(&memory, &exports);
    auto backend = std::make_unique<StaticBackend>();
    auto* b = backend.get();
    Require(processor.Setup(std::move(backend)), "Processor::Setup");
    auto* heap = memory.LookupHeap(0x82010000);
    Require(
        heap && heap->AllocFixed(
                    0x82010000, 0x10000, 0x10000,
                    xe::kMemoryAllocationReserve | xe::kMemoryAllocationCommit,
                    xe::kMemoryProtectRead | xe::kMemoryProtectWrite),
        "code allocation");
    std::memcpy(memory.TranslateVirtual(0x82010000), smoke_code,
                sizeof(smoke_code));
    const uint32_t data = memory.SystemHeapAlloc(16);
    Require(data != 0, "data allocation");
    auto guest = std::make_unique<SmokeGuest>(&processor);
    auto* mod = guest.get();
    Require(processor.AddModule(std::move(guest)), "module insertion");
    xe::cpu::Function* function = nullptr;
    mod->DeclareFunction(0x82010040, &function);
    static_cast<xe::cpu::GuestFunction*>(function)->SetupExtern(
        [](xe::cpu::ppc::PPCContext* c, xe::kernel::KernelState*) {
          Require(c->r[3] == 13, "HLE argument");
          c->r[3] += 10;
        },
        nullptr);
    function->set_status(xe::cpu::Symbol::Status::kDeclared);
    Require(b->PrepareModule(mod), "static module hash binding");
    const uint32_t callback = b->CreateGuestTrampoline(
        [](xe::cpu::ppc::PPCContext* c, void*, void*) { c->r[3] *= 2; },
        nullptr, nullptr);
    Require(callback != 0, "data-backed callback");
    {
      xe::cpu::ThreadState thread(&processor, 1);
      thread.context()->r[5] = data;
      thread.context()->r[6] = callback;
      Require(processor.Execute(&thread, 0x82010000), "real static execution");
      Require(thread.context()->r[3] == 46, "native callback result");
      auto* bytes = memory.TranslateVirtual(data);
      Require(bytes[3] == 13 && bytes[7] == 46,
              "shared big-endian guest stores");
      Require(processor.backend()->code_cache() == nullptr,
              "no executable code cache");
      Require(!xe::memory::AllocFixed(
                  nullptr, 4096, xe::memory::AllocationType::kReserveCommit,
                  xe::memory::PageAccess::kExecuteReadWrite),
              "executable allocation must fail");
      b->FreeGuestTrampoline(callback);
      Require(!b->IsCallback(callback), "freed callback is invalid");
      memory.TranslateVirtual(0x82010000)[0] ^= 1;
      Require(!processor.Execute(&thread, 0x82010000) && b->stopped(),
              "live instruction mismatch stops backend");
    }
    processor.RemoveModule(mod->name());
    memory.SystemHeapFree(data);
    std::cout << "Real XeniOS strict static smoke passed: CPU, shared memory, "
                 "registered HLE, native callback, code guard, executable "
                 "allocation denial. No game/device validation.\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
