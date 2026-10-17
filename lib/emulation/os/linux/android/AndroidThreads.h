//===- AndroidThreads.h - Cooperative guest-thread ownership ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_ANDROIDTHREADS_H
#define NEVERD_EMULATION_ANDROIDTHREADS_H
#include "AndroidInternal.h"

#include <deque>

namespace neverd::emulation::android_model {
namespace thread_model {
#define NEVERD_ANDROID_THREAD_VALUE(Name, Value)                               \
  inline constexpr uint64_t Name = Value;
#include "AndroidThreads.def"
#undef NEVERD_ANDROID_THREAD_VALUE
} // namespace thread_model
struct PendingCallback {
  GuestCallback Call;
  ServiceRequest Request;
  size_t Event;
  uint64_t Link, StackPointer;
};

/// One shared CPU transport, with complete saved CPU contexts at consumed
/// service/quantum boundaries. Guest RAM is never rolled back on a switch.
/// Bionic process state is shared; TLS, waits and callbacks belong to threads.
class GuestThreads {
public:
  enum class WaitSource { NativeCall, KernelService };
  GuestThreads(ExecutionBackend &CPU, const IntegerABI &Calls,
               const ProcessOptions &Options, ProcessResult &Result,
               linux_model::LinuxClock &Clock,
               linux_model::LinuxPriority &PriorityState)
      : CPU(CPU), Calls(Calls), Options(Options), Result(Result), Clock(Clock),
        PriorityState(PriorityState) {}
  llvm::Error initialize(uint64_t StackBase);
  llvm::Error validateTLS();
  bool enabled() const { return Options.Android->ThreadLimit > 1; }
  uint64_t id() const { return current().Kernel.ID; }
  uint64_t tls() const { return current().Report.TLS; }
  bool isEntry() const { return Current == 0; }
  linux_model::ThreadContext *kernel() {
    return enabled() ? &current().Kernel : nullptr;
  }
  std::vector<PendingCallback> &callbacks() { return current().Callbacks; }
  void setReturnSP(uint64_t SP) { current().ReturnSP = SP; }
  uint64_t returnSP() const { return current().ReturnSP; }
  BionicResult invoke(const NativeCallEvent &Call);
  bool onceActive(uint64_t Control) const;
  BionicResult waitOnce(uint64_t Control);
  void completeOnce(uint64_t Control);
  void waitMutex(uint64_t Address, uint16_t Attributes);
  void wakeMutex(uint64_t Address);
  void waitSemaphore(uint64_t Address, bool Shared);
  void wakeSemaphore(uint64_t Address);
  bool waitSleep();
  void suspend(const ServiceRequest &Request, size_t Event,
               WaitSource Source = WaitSource::NativeCall);
  llvm::Error finish(uint64_t Value, uint32_t ExitStatus = 0);
  /// Round robin, including the current thread if it is the only runnable
  /// owner. False classifies all-finished or a wait cycle in Result.
  llvm::Expected<bool> schedule(Bionic &LibC);
  void report();
  std::vector<linux_model::ProcTask> procTasks() const;

private:
  struct Join {
    size_t Target;
    uint64_t Output;
  };
  struct Once {
    size_t Owner;
    uint64_t Control;
    bool Ready = false;
  };
  struct Mutex {
    uint64_t Address;
    uint16_t Attributes;
    bool Ready = false;
  };
  struct Sleep {
    uint64_t Deadline;
  };
  struct Semaphore {
    uint64_t Address;
    bool Shared, Ready = false;
  };
  struct Wait {
    std::variant<Join, Once, Mutex, Sleep, Semaphore> Operation;
    std::optional<ServiceRequest> Request;
    size_t Event = 0;
    WaitSource Source = WaitSource::NativeCall;
  };
  struct Thread {
    NativeThreadSnapshot Report;
    linux_model::ThreadContext Kernel;
    std::array<uint8_t, thread_attribute_abi::ObjectBytes> Attributes{};
    std::unique_ptr<BackendContext> Context;
    std::vector<PendingCallback> Callbacks;
    std::optional<Wait> Waiting;
    std::optional<size_t> Joiner;
    uint64_t MappingBase = 0, MappingSize = 0, ReturnSP = 0;
  };
  ExecutionBackend &CPU;
  const IntegerABI &Calls;
  const ProcessOptions &Options;
  ProcessResult &Result;
  linux_model::LinuxClock &Clock;
  linux_model::LinuxPriority &PriorityState;
  std::deque<Thread> Threads;
  size_t Current = 0;
  uint32_t LastExitStatus = 0;
  Thread &current() { return Threads[Current]; }
  const Thread &current() const { return Threads[Current]; }
  llvm::Error access(uint64_t Address, uint64_t Size, unsigned Permissions,
                     unsigned Alignment = 8);
  llvm::Error put64(uint64_t Address, uint64_t Value);
  llvm::Error retire(size_t Index);
  std::optional<size_t> onceOwner(uint64_t Control) const;
  bool ready(const Wait &Pending) const;
  std::optional<size_t> nextRunnable() const;
  std::optional<uint64_t> nextWake() const;
  llvm::Error switchTo(size_t Next);
  llvm::Error prepareJoin(const Join &Pending);
  BionicResult resumeOnce(const Once &Pending);
  BionicResult resumeWait(Bionic &LibC);
  llvm::Error completeWait(uint64_t Value);
  static std::optional<BionicValue> value(uint64_t Value) {
    return BionicValue(Value);
  }
  static std::array<uint8_t, thread_attribute_abi::ObjectBytes>
  defaultAttributes();
  static void
  stackAttributes(std::array<uint8_t, thread_attribute_abi::ObjectBytes> &Bytes,
                  const NativeThreadSnapshot &Thread);
  llvm::Expected<size_t> findHandle(uint64_t Handle) const;
  BionicResult self(const NativeCallEvent &Call);
  BionicResult equal(const NativeCallEvent &Call);
  BionicResult exit(const NativeCallEvent &Call);
  BionicResult getTID(const NativeCallEvent &Call);
  BionicResult getAttributes(const NativeCallEvent &Call);
  BionicResult detach(const NativeCallEvent &Call);
  BionicResult join(const NativeCallEvent &Call);
  BionicResult create(const NativeCallEvent &Call);
  BionicResult unsupported(llvm::StringRef Reason);
};
} // namespace neverd::emulation::android_model
#endif
