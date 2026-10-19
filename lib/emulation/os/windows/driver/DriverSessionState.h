//===- DriverSessionState.h - Driver resource ownership ----------*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Driver resource ownership.
///

#ifndef NEVERD_EMULATION_DRIVERSESSIONSTATE_H
#define NEVERD_EMULATION_DRIVERSESSIONSTATE_H
#include "../exception/X64SEH.h"
#include "../kernel/KernelExportRegistry.h"
#include "../kernel/KernelModel.h"
#include "../kernel/KernelReferenceImage.h"
#include "DriverImage.h"
#include "GuardControlFlow.h"
#include "WindowsX64ExecutionPolicy.h"

#include "neverd/emulation/CPU.h"
#include "neverd/emulation/DriverSession.h"
#include "neverd/emulation/ExecutionBudget.h"
#include "neverd/emulation/IntegerABI.h"
#include "neverd/modules/ModuleImage.h"

#include <array>
#include <deque>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace neverd::emulation {
struct DriverResourceInputs {
  std::shared_ptr<const modules::ModuleImage> Image, Kernel;
  std::string ImageID, KernelID;
  modules::LoadPolicy ImageLoad = modules::LoadPolicy::Eager;
  modules::LoadPolicy KernelLoad = modules::LoadPolicy::Eager;
  bool LinkModuleImports = false;
};
/// Canonical driver resources and resident frame data. References captured by
/// model/context providers point at this stable, nonmovable owner, not an
/// initialization stack. create() initializes only; it executes no guest code.
/// DriverExecutionEnvironment retains this owner and its suspended
/// orchestration task; ExecutionRun alone runs the session. This is neither a
/// second executor nor the public SDK driver factory.
class DriverSessionState final {
public:
  using ResourceInputs = DriverResourceInputs;
  static llvm::Expected<std::unique_ptr<DriverSessionState>>
  create(const std::filesystem::path &, const DriverOptions &,
         ExecutionTimePolicy = ExecutionTimePolicy::AbsoluteDeadline,
         std::optional<uint64_t> TimeoutMicroseconds = {},
         ResourceInputs Inputs = {});
  ~DriverSessionState();
  DriverSessionState(const DriverSessionState &) = delete;
  DriverSessionState &operator=(const DriverSessionState &) = delete;
  DriverSessionState(DriverSessionState &&) = delete;
  DriverSessionState &operator=(DriverSessionState &&) = delete;

  struct EnvironmentRead {
    WindowsX64ExecutionPolicy::Action Request;
    uint64_t NextPC;
  };
  struct ExceptionExecution {
    X64SEH::Dispatch Dispatch;
    X64SEH::Exception Raised;
    X64SEH::Action Next;
    std::unique_ptr<BackendContext> Original;
    bool CanContinue = false;
  };
  struct TimeSlice {
    uint64_t Remaining;
    uint64_t ReadyOrder = 0;
  };
  struct Execution {
    uint64_t ID = 0;
    uint64_t Base = 0, Size = 0, InitialSP = 0, PC = 0;
    uint8_t EntryIRQL = 0;
    uint64_t ThreadKey = 0;
    std::shared_ptr<TimeSlice> Slice;
    std::optional<KernelModel::ExecutionContext> PausedModel;
    bool Boundary = false;
    std::string Phase;
    std::unique_ptr<BackendContext> Context;
    std::optional<KernelModel::Wait> Wait;
    std::optional<uint64_t> ResumeValue;
    std::optional<uint8_t> ResumeIRQL;
    size_t WaitEvent = 0;
    bool PrivateStack = false;
    GuestCallToken ReturnToken;
    std::optional<GuestCallToken> PendingEntry;
    uint64_t SynchronizationObject = 0;
    bool SynchronizationEntered = false, CallbackEntered = false;
    bool PowerManagedCallback = false;
    uint64_t RequestIRP = 0;
    std::optional<KernelGuestCall> ChildCall;
    bool ThreadTerminated = false, DeferredThreadTermination = false;
    std::unique_ptr<ExceptionExecution> Exception;
    std::optional<X64SEH::ActionKind> ExceptionCallback;
    std::unique_ptr<Execution> Parent;
  };
  struct ClockCall {
    KernelGuestCall Call;
    std::optional<KernelModel::Wait> Wait;
    uint64_t ReadyOrder;
  };

  DriverOptions Options;
  /// Pinned observations, unaffected by later source/debug catalogue changes.
  ResourceInputs Inputs;
  std::vector<DriverImport> ModuleImports;
  bool AllowImportPublication = false, ModuleImportsLinked = true;
  DriverResult Result;
  KernelExportRegistry Exports;
  std::unique_ptr<DriverImage> Image;
  std::optional<KernelReferenceImage> Reference;
  IntegerABI ABI =
      llvm::cantFail(IntegerABI::get(IntegerCallingConvention::Win64));
  std::unique_ptr<GuardControlFlow> Guard;
  std::unique_ptr<KernelModel> Kernel;
  std::unique_ptr<X64SEH> Exceptions;
  std::set<std::string> ExceptionModuleIDs;
  std::set<std::string> GuardModuleIDs;
  WindowsX64ExecutionPolicy Policy;
  std::optional<DriverCPUIDModel> CPUID;
  std::shared_ptr<ExecutionBudget> Budget;
  bool ForegroundReadyPolicy = false;
  uint64_t ExpectedReturnSP = 0, ActiveStackBase = 0, ActiveStackSize = 0;
  bool Stopped = false;
  std::optional<uint64_t> InvocationReturn;
  const KernelExportRegistry::Export *Pending = nullptr;
  uint64_t PendingGuard = 0;
  std::optional<EnvironmentRead> PendingEnvironmentRead;
  bool ProcessorViewMapped = false, ProcessorReadAdmitted = false;
  uint64_t RunInstructions = 0, RunAllowance = UINT64_MAX;
  bool AdmissionBoundary = false;
  std::vector<std::unique_ptr<Execution>> Waiting;
  std::deque<ClockCall> ClockCalls;
  uint64_t AccountedInstructions = 0;
  std::array<bool, profile::MaxConcurrentCallbacks> StackMapped{}, StackInUse{};
  size_t RegistryEventCursor = 0, ProcessEventCursor = 0;

  // Stable view: initialization owns UnattachedCPU, then the execution session
  // takes the transport. That session must die before this model owner.
  ExecutionBackend *CPU = nullptr;
  // Model destructors release host storage only and must not access the CPU.
  std::unique_ptr<ExecutionBackend> UnattachedCPU;

private:
  DriverSessionState() = default;
};
} // namespace neverd::emulation
#endif
