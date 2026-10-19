//===- DriverSessionState.cpp - Driver initialization --------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Driver initialization.
///

#include "DriverSessionState.h"

#include "../../../core/ExecutionDiagnostics.h"
#include "../../../runtime/RuntimeValues.h"
#include "../process/WindowsProcessContext.h"
#include "DriverScenario.h"

#include "neverd/modules/ModuleSet.h"

#include "llvm/Support/FormatVariadic.h"

namespace neverd::emulation {
namespace {
namespace session_diagnostic {
#define NEVERD_DRIVER_SESSION_DIAGNOSTIC(Name, Text)                           \
  constexpr char Name[] = Text;
#include "DriverSessionDiagnostics.def"
#undef NEVERD_DRIVER_SESSION_DIAGNOSTIC
} // namespace session_diagnostic
using namespace profile;
llvm::Error failure(const std::string &Text) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(), Text);
}
llvm::Expected<std::shared_ptr<const modules::ModuleImage>>
prepareInput(const std::filesystem::path &Path,
             const std::filesystem::path &Debug = {}) {
  std::error_code EC;
  auto Absolute = std::filesystem::absolute(Path, EC);
  if (EC)
    return llvm::createStringError(EC, "cannot resolve explicit driver input");
  modules::ModuleOptions O;
  O.Modules.push_back({"bootstrap-input", std::move(Absolute)});
  if (!Debug.empty()) {
    auto PDB = std::filesystem::absolute(Debug, EC);
    if (EC)
      return llvm::createStringError(EC, "cannot resolve explicit kernel_pdb");
    O.Modules.back().Debug =
        modules::DebugInput{modules::DebugKind::PDB, std::move(PDB)};
  }
  auto Sources = modules::ModuleSet::create(std::move(O));
  if (!Sources)
    return Sources.takeError();
  return (*Sources)->prepare("bootstrap-input");
}
} // namespace
DriverSessionState::~DriverSessionState() = default;
llvm::Expected<std::unique_ptr<DriverSessionState>> DriverSessionState::create(
    const std::filesystem::path &Path, const DriverOptions &Input,
    ExecutionTimePolicy TimePolicy, std::optional<uint64_t> TimeoutMicroseconds,
    ResourceInputs Inputs) {
  if (Input.Contract != ExecutionContract::Legacy &&
      Input.Contract != ExecutionContract::CheckedX64)
    return diagnostic::error(diagnostic::Contract);
  if (!Input.InstructionLimit || !Input.MemoryLimit || !Input.EventLimit ||
      !Input.TimeoutMilliseconds ||
      Input.TimeoutMilliseconds > MaxTimeoutMilliseconds ||
      Input.MemoryLimit > MaxMemoryLimit || Input.EventLimit > MaxEventLimit)
    return failure(llvm::formatv(session_diagnostic::Limits, MaxMemoryLimit,
                                 MaxEventLimit, MaxTimeoutMilliseconds)
                       .str());
  if (auto E = validateDriverScenario(Input))
    return std::move(E);
  if (TimeoutMicroseconds &&
      (!*TimeoutMicroseconds ||
       *TimeoutMicroseconds / runtime::MicrosecondsPerMillisecond +
               (*TimeoutMicroseconds % runtime::MicrosecondsPerMillisecond !=
                0) !=
           Input.TimeoutMilliseconds))
    return failure(
        "exact execution timeout differs from driver timeout envelope");
  auto State = std::unique_ptr<DriverSessionState>(new DriverSessionState);
  State->Options = Input;
  State->Inputs = std::move(Inputs);
  const auto &Options = State->Options;
  State->ForegroundReadyPolicy =
      foregroundReadyCallbacks(Options.CallbackDrainPolicy);
  auto &Exports = State->Exports;
  if (auto E = Exports.initialize(Options))
    return std::move(E);
  if (!State->Inputs.Image) {
    auto Source = prepareInput(Path);
    if (!Source)
      return Source.takeError();
    State->Inputs.Image = std::move(*Source);
  }
  auto LoadedImage =
      loadDriverImage(*State->Inputs.Image, Options.MemoryLimit,
                      Options.LoadAddress, State->Inputs.LinkModuleImports);
  if (!LoadedImage)
    return LoadedImage.takeError();
  State->Image = std::make_unique<DriverImage>(std::move(*LoadedImage));
  auto &Image = State->Image;
  auto &Reference = State->Reference;
  if (State->Inputs.Kernel || !Options.KernelImage.empty()) {
    if (!State->Inputs.Kernel) {
      auto Source = prepareInput(Options.KernelImage, Options.KernelPDB);
      if (!Source)
        return Source.takeError();
      State->Inputs.Kernel = std::move(*Source);
    }
    auto Loaded = KernelReferenceImage::fromModule(*State->Inputs.Kernel,
                                                   Options.MemoryLimit);
    if (!Loaded)
      return Loaded.takeError();
    Reference = std::move(*Loaded);
    if (auto E = Exports.attachReference(*Reference, Options))
      return std::move(E);
  }
  auto &Result = State->Result;
  if (Reference) {
    Result.KernelImageSHA256 = Reference->digest();
    Result.KernelPDBSHA256 = Reference->pdbDigest();
  }
  Result.Configuration = Options;
  Result.ImageBase = Image->Base;
  Result.PreferredImageBase = Image->PreferredBase;
  Result.SecurityCookieAddress = Image->SecurityCookieAddress;
  Result.Entry = Image->Entry;
  Result.PC = Image->Entry;
  if (Image->Imports.size() > (ThunkSize / ThunkStride) - 1)
    return failure(session_diagnostic::ImportLimit);
  auto Backend = createExecutionBackend(Options.Backend, Options.Contract,
                                        Options.MemoryLimit);
  if (!Backend)
    return Backend.takeError();
  Result.SelectedBackend = Backend->Kind;
  Result.BackendSelectionReason = Backend->Reason;
  State->UnattachedCPU = std::move(Backend->CPU);
  State->CPU = State->UnattachedCPU.get();
  auto &CPU = *State->CPU;
  if (Reference)
    if (auto E = Reference->map(CPU))
      return std::move(E);
  State->Guard = std::make_unique<GuardControlFlow>(*Image);
  for (const auto &Region : Image->Regions) {
    if (auto E = CPU.map(Region.Address, Region.Bytes.size(), Read | Write))
      return std::move(E);
    if (auto E = CPU.write(Region.Address, Region.Bytes))
      return std::move(E);
  }
  for (const auto &Import : Image->Imports) {
    if (State->Inputs.LinkModuleImports) {
      auto Known = Exports.findImport(Import);
      if (!Known)
        return Known.takeError();
      // Existing model identity wins. Unspecified named kernel APIs retain
      // their lazy traps; a source file cannot infer missing OS semantics.
      if (!*Known &&
          (Import.Ordinal ||
           KernelExportRegistry::canonicalImportModule(Import.Module) !=
               std::optional<std::string>("ntoskrnl.exe"))) {
        State->ModuleImports.push_back(Import);
        State->ModuleImportsLinked = false;
        if (auto E = CPU.writeInteger(Import.Slot, 0, PointerSize))
          return std::move(E);
        continue;
      }
    }
    auto Address = Exports.bindImport(Import);
    if (!Address)
      return Address.takeError();
    if (auto E = CPU.writeInteger(Import.Slot, *Address, PointerSize))
      return std::move(E);
  }
  if (Image->Guard.Enabled) {
    if (auto E = CPU.writeInteger(Image->Guard.CheckPointerAddress,
                                  GuardCheckThunk, PointerSize))
      return std::move(E);
    if (Image->Guard.DispatchPointerAddress)
      if (auto E = CPU.writeInteger(Image->Guard.DispatchPointerAddress,
                                    GuardDispatchThunk, PointerSize))
        return std::move(E);
  }
  // Reserve helpers before publication, including for a later guarded image
  // joining an initially unguarded primary driver.
  if (auto E = CPU.map(GuardThunkBase, PageSize, Read | Execute))
    return std::move(E);
  for (const auto &Region : Image->Regions)
    if (auto E = CPU.protect(Region.Address, Region.Bytes.size(),
                             Region.Permissions))
      return std::move(E);
  if (auto E = CPU.map(ThunkBase, ThunkSize, Read | Write))
    return std::move(E);
  std::vector<uint8_t> TrapBytes(ThunkSize, ThunkTrapByte);
  if (auto E = CPU.write(ThunkBase, TrapBytes))
    return std::move(E);
  if (auto E = CPU.protect(ThunkBase, ThunkSize, Read | Execute))
    return std::move(E);
  if (Reference) {
    for (const auto &Range : KernelExportRegistry::executionWatches()) {
      if (Range.Address == ThunkBase)
        continue;
      bool Used = false;
      for (const auto &[Address, Entry] : Exports.entries())
        Used |= Entry.Kind == KernelExportRegistry::ExportKind::ModuleExport &&
                Address >= Range.Address &&
                Address - Range.Address < Range.Size;
      if (!Used)
        continue;
      if (auto E = CPU.map(Range.Address, Range.Size, Read | Write))
        return E;
      std::vector<uint8_t> Bytes(Range.Size, ThunkTrapByte);
      if (auto E = CPU.write(Range.Address, Bytes))
        return E;
      if (auto E = CPU.protect(Range.Address, Range.Size, Read | Execute))
        return E;
    }
  }
  if (auto E = CPU.map(StackBase, StackSize, Read | Write))
    return std::move(E);
  State->Kernel = std::make_unique<KernelModel>(CPU, Result, &Exports);
  auto &Kernel = *State->Kernel;
  if (Reference)
    if (auto E = Kernel.setDebuggerReference(*Reference))
      return std::move(E);
  if (auto E = Kernel.initialize(*Image, Options))
    return std::move(E);
  if (auto E = Kernel.initializeLoadedModule(*Image, Options.ServiceName))
    return E;
  if (Reference)
    Kernel.addReferenceModule(Reference->base(), Reference->size());
  if (auto E = Kernel.initializeDebuggerState())
    return std::move(E);
  Kernel.setContextCapture([S = State.get()](uint64_t Address) -> llvm::Error {
    constexpr uint32_t ContextSize = 1232;
    if (auto E = S->Kernel->validateGuestAccess(Address, ContextSize, true))
      return E;
    return windows_process::captureCallerContext(*S->CPU, Address, false);
  });
  State->Exceptions = std::make_unique<X64SEH>(
      Image->Exceptions, Image->PreferredBase, Image->Base, Image->Size,
      [S = State.get()](uint64_t Address) -> llvm::Expected<uint64_t> {
        if (auto E =
                S->Kernel->validateGuestAccess(Address, PointerSize, false))
          return std::move(E);
        return S->CPU->readInteger(Address, PointerSize);
      },
      [S = State.get()](uint64_t Address) {
        return S->CPU->executable(Address);
      },
      [S = State.get()](uint64_t Address,
                        llvm::MutableArrayRef<uint8_t> Bytes) {
        return S->CPU->fetch(Address, Bytes);
      },
      [S = State.get()]() -> llvm::Expected<uint64_t> {
        if (!S->Image->SecurityCookieAddress)
          return failure(session_diagnostic::MissingSecurityCookie);
        if (auto E = S->Kernel->validateGuestAccess(
                S->Image->SecurityCookieAddress, PointerSize, false))
          return std::move(E);
        return S->CPU->readInteger(S->Image->SecurityCookieAddress,
                                   PointerSize);
      });
  auto CPUID = DriverCPUIDModel::fromDriverOptions(Options);
  if (!CPUID)
    return CPUID.takeError();
  State->CPUID = std::move(*CPUID);
  const auto BranchModel = CPU.x64BranchModel();
  if (!BranchModel)
    return diagnostic::error(diagnostic::BranchModel);
  if (auto E = State->Policy.initialize(
          *BranchModel, State->CPUID->enabled(), Options.PageTables,
          WindowsX64ExecutionPolicy::Purpose::Driver,
          Kernel.hasModelSpecificRegisters()))
    return std::move(E);
  auto Budget = ExecutionBudget::createWithTimePolicy(
      {Options.InstructionLimit, Options.EventLimit,
       TimeoutMicroseconds.value_or(Options.TimeoutMilliseconds *
                                    runtime::MicrosecondsPerMillisecond)},
      TimePolicy);
  if (!Budget)
    return Budget.takeError();
  State->Budget = std::move(*Budget);
  return State;
}
} // namespace neverd::emulation
