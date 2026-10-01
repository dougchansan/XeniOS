// Copyright 2026 XeniOS contributors. Released under the BSD-3-Clause license.
#ifndef XENIA_CPU_BACKEND_STATIC_STATIC_BACKEND_H_
#define XENIA_CPU_BACKEND_STATIC_STATIC_BACKEND_H_
#include <atomic>
#include <memory>
#include <unordered_map>
#include "xenia/cpu/backend/backend.h"
#include "xenia/cpu/backend/static/runtime.h"
namespace xe::cpu {
class XexModule;
}
namespace xe::cpu::backend::statik {
// Supplied by the generated, statically linked catalog. The default is empty.
std::span<const aot::Module* const> LinkedModules();
class StaticHost;
class CallbackModule;
class StaticBackend final : public Backend {
 public:
  std::string name() const override {
    return "static-aot (strict, experimental)";
  }
  bool Initialize(Processor* processor) override;
  ~StaticBackend() override;
  bool UsesRuntimeCompiler() const override { return false; }
  bool BindFunction(GuestFunction* function) override;
  bool PrepareModule(Module* module) override;
  void ForgetModule(const std::string& name) override;
  bool IsExportOnly() const override;
  void CommitExecutableRange(uint32_t, uint32_t) override {}
  std::unique_ptr<Assembler> CreateAssembler() override;
  std::unique_ptr<GuestFunction> CreateGuestFunction(Module*,
                                                     uint32_t) override;
  uint64_t CalculateNextHostInstruction(ThreadDebugInfo*, uint64_t) override {
    return 0;
  }
  uint32_t CreateGuestTrampoline(GuestTrampolineProc proc, void* arg1,
                                 void* arg2, bool long_term = false) override;
  void FreeGuestTrampoline(uint32_t address) override;
  bool Execute(ThreadState* thread, uint32_t entry, uint32_t return_address);
  bool IsCallback(uint32_t address) const;
  aot::Status CallExternal(aot::Runtime&, ThreadState*);
  bool stopped() const { return stopped_.load(std::memory_order_acquire); }

 private:
  struct Callback {
    GuestTrampolineProc proc;
    void* a;
    void* b;
  };
  aot::Registry registry_;
  mutable std::mutex callback_mutex_;
  std::unordered_map<uint32_t, Callback> callbacks_;
  // Quarantine freed addresses for this backend's lifetime; a stale guest
  // function pointer must never become a different native callback.
  std::vector<uint32_t> allocated_callbacks_;
  std::atomic<bool> stopped_{false};
};
}  // namespace xe::cpu::backend::statik
#endif
