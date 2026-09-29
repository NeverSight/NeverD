//===- ExecutionBackend.h - Private checked CPU execution boundary -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_CORE_EXECUTIONBACKEND_H
#define NEVERD_EMULATION_CORE_EXECUTIONBACKEND_H
#include "../GuestMemory.h"
#include "../X64Registers.h"

#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <utility>

namespace neverd::emulation {
enum class BackendFaultKind {
#define NEVERD_BACKEND_FAULT_KIND(Name, Spelling) Name,
#include "BackendFaults.def"
#undef NEVERD_BACKEND_FAULT_KIND
};
enum class BackendAccessKind {
#define NEVERD_BACKEND_ACCESS_KIND(Name, Spelling) Name,
#include "BackendFaults.def"
#undef NEVERD_BACKEND_ACCESS_KIND
};
const char *backendFaultKindName(BackendFaultKind Kind);
const char *backendAccessKindName(BackendAccessKind Kind);
struct BackendFault {
  BackendFaultKind Kind;
  uint64_t PC = 0;
  /// Memory-event address and access size, not a decoded operand extent.
  /// Unicorn may split a memory access at a page boundary.
  std::optional<uint64_t> Address;
  std::optional<uint64_t> Size;
  std::optional<BackendAccessKind> Access;
  std::optional<uint32_t> Interrupt;
};
struct BackendHooks {
  std::function<void(uint64_t, uint32_t)> Instruction;
  std::function<void(uint64_t, uint32_t)> Read;
  std::function<void(uint64_t, uint32_t, uint64_t)> Write;
  std::function<void(uint64_t, uint32_t, const char *)> Fault;
  /// Admit only a modeled, synchronous guest memory exception. The faulting
  /// instruction is abandoned; the caller must consume the fault and install
  /// a validated guest exception transfer before running again.
  std::function<bool(const BackendFault &)> RecoverableFault;
  std::function<void(uint32_t)> Interrupt;
  std::function<void()> InvalidInstruction;
};
/// An owning CPU snapshot associated with exactly one backend instance.
/// Guest memory and hooks are shared by all contexts and are never rolled back.
/// The snapshot may outlive its backend, but can no longer be used afterward.
class BackendContext final {
public:
  struct Storage {
    virtual ~Storage() = default;
    std::weak_ptr<const void> Owner;
  };
  ~BackendContext() = default;
  BackendContext(BackendContext &&) noexcept = default;
  BackendContext &operator=(BackendContext &&) noexcept = default;
  BackendContext(const BackendContext &) = delete;
  BackendContext &operator=(const BackendContext &) = delete;

private:
  friend class UnicornBackend;
  friend class CheckedX64Backend;
  explicit BackendContext(std::unique_ptr<Storage> State)
      : State(std::move(State)) {}
  std::unique_ptr<Storage> State;
};
/// The checked x64 execution contract used by the existing environment.
/// Hardware adapters may implement a strictly smaller, explicitly selected ISA
/// profile; selecting one never changes the environment's required checks.
class ExecutionBackend : public GuestMemory {
public:
  using XmmValue = std::array<uint64_t, 2>;
  virtual llvm::Error fetch(uint64_t Address,
                            llvm::MutableArrayRef<uint8_t> Bytes) = 0;
  virtual llvm::Expected<uint64_t> reg(X64Register Register) = 0;
  virtual llvm::Error setReg(X64Register Register, uint64_t Value) = 0;
  virtual llvm::Error setGSBase(uint64_t Address) = 0;
  virtual llvm::Expected<XmmValue> xmm(unsigned Register) = 0;
  virtual llvm::Error setXmm(unsigned Register, const XmmValue &Value) = 0;
  virtual llvm::Expected<std::unique_ptr<BackendContext>> saveContext() = 0;
  virtual llvm::Error saveContext(BackendContext &Context) = 0;
  virtual llvm::Error restoreContext(const BackendContext &Context) = 0;
  virtual llvm::Error installHooks(BackendHooks Hooks) = 0;
  virtual llvm::Error run(uint64_t PC, uint64_t TimeoutMicroseconds) = 0;
  virtual bool timedOut() const = 0;
  virtual void stop() = 0;
  virtual bool hasMemoryFault() const = 0;
  virtual bool hasDeviceError() const = 0;
  virtual std::optional<BackendFault> fault() const = 0;
  virtual std::optional<BackendFault> takeRecoverableFault() = 0;
  virtual bool executable(uint64_t Address) const = 0;
};
} // namespace neverd::emulation
#endif
