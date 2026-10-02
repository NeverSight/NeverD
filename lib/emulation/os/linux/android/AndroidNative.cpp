//===- AndroidNative.cpp - Bounded Android AArch64 native workloads ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "../../../core/ExecutionDeadline.h"
#include "../../../runtime/RuntimeValues.h"
#include "AndroidInternal.h"

#include "neverd/emulation/ExecutionSession.h"
#include "neverd/loader/ELF/ELFLoader.h"

#include <set>

namespace neverd::emulation::android_model {
llvm::Expected<ProcessResult> runNative(const std::filesystem::path &Path,
                                        const ProcessOptions &Options) {
  if (!Options.Android || !Options.Arguments.empty() ||
      !Options.Environment.empty())
    return failure(
        "requires Android native options and no process argv/environment");
  const auto &Native = *Options.Android;
  if (Native.EntrySymbol.empty() == !Native.EntryAddress.has_value() ||
      Native.EntrySymbol.find('\0') != std::string::npos)
    return failure("select exactly one entry_symbol or entry_address");
  if (!Options.Limits.Instructions || !Options.Limits.Events ||
      !Options.Limits.TimeoutMicroseconds || !Options.MemoryLimit ||
      !Options.StackSize || !Options.OutputLimit ||
      !Options.InstructionQuantum || Options.StackSize % PageSize ||
      Options.StackSize >= Options.MemoryLimit ||
      Options.StackSize >= linux_model::StackTop - ThunkBase ||
      Options.OutputLimit > Options.MemoryLimit ||
      Native.LoadBias < linux_model::MinimumAddress ||
      Native.LoadBias >= TLSAddress || Native.LoadBias % PageSize ||
      Native.TraceLimit > Options.OutputLimit / 8 ||
      Native.Arguments.size() > Options.StackSize / 8)
    return failure("invalid native execution limits or layout");
  auto Deadline = makeExecutionDeadline(Options.Limits.TimeoutMicroseconds);
  if (!Deadline)
    return Deadline.takeError();
  uint64_t ReportSize = Native.TraceLimit * 8;
  for (const auto &Read : Native.ReadMemory) {
    if (!Read.Size || Read.Size > Options.OutputLimit - ReportSize ||
        Read.Address > UINT64_MAX - Read.Size)
      return failure("invalid memory snapshot extent or output budget");
    ReportSize += Read.Size;
  }
  for (const auto &[Name, Value] : Native.Properties)
    if (Name.empty() || Name.find('\0') != std::string::npos ||
        Value.find('\0') != std::string::npos || Value.size() >= 92 ||
        Name.size() > Options.MemoryLimit)
      return failure("invalid Android property name or value");
  uint64_t SymbolCount = 0, CatalogBytes = 0;
  if (Native.Libraries.size() > 256)
    return failure("too many modeled Android libraries");
  for (const auto &[Library, Symbols] : Native.Libraries) {
    if (Library.empty() || Library.size() > 1024 ||
        Library.find('\0') != std::string::npos)
      return failure("invalid modeled Android library name");
    CatalogBytes += Library.size();
    std::set<std::string> Unique;
    for (const auto &Symbol : Symbols) {
      if (++SymbolCount > 4096 || Symbol.empty() || Symbol.size() > 1024 ||
          Symbol.find('\0') != std::string::npos ||
          !Unique.insert(Symbol).second)
        return failure("invalid or excessive modeled Android symbols");
      CatalogBytes += Symbol.size();
    }
  }
  if (CatalogBytes > Options.MemoryLimit)
    return failure("Android symbol catalogue exceeds memory limit");
  ELFLoader Loader;
  auto Image = Loader.load(Path);
  if (!Image)
    return Image.takeError();
  auto Physical = PhysicalMemory::create(Options.MemoryLimit);
  if (!Physical)
    return Physical.takeError();
  auto Space = AddressSpace::create(*Physical, Options.MemoryLimit);
  if (!Space)
    return Space.takeError();
  auto Linked = loadImage(**Space, *Image, Options);
  if (!Linked)
    return Linked.takeError();
  uint64_t StackBase = linux_model::StackTop - Options.StackSize;
  if (auto E = (*Space)->map(StackBase, Options.StackSize,
                             Read | Write | UserAccessible))
    return std::move(E);
  for (const auto &Region : Native.Memory) {
    if (!Region.Size || Region.Size % PageSize || Region.Address % PageSize ||
        Region.Address < linux_model::MinimumAddress ||
        Region.Address >= TLSAddress ||
        Region.Size > TLSAddress - Region.Address ||
        Region.Bytes.size() > Region.Size)
      return failure("invalid explicit native memory region");
    unsigned Permissions =
        Read | Write | UserAccessible | (Region.Executable ? Execute : 0u);
    if (auto E = (*Space)->map(Region.Address, Region.Size, Permissions))
      return std::move(E);
    if (!Region.Bytes.empty())
      if (auto E = (*Space)->write(Region.Address, Region.Bytes))
        return std::move(E);
  }
  for (const auto &Read : Native.ReadMemory) {
    auto Accessible = (*Space)->canAccess(Read.Address, Read.Size,
                                          emulation::Read | UserAccessible);
    if (!Accessible)
      return Accessible.takeError();
    if (!*Accessible)
      return failure("requested snapshot is not readable at entry");
  }
  auto Calls = IntegerABI::get(IntegerCallingConvention::AAPCS64);
  if (!Calls)
    return Calls.takeError();
  linux_model::ProcessLayout Layout{GuestArchitecture::AArch64,
                                    *Calls,
                                    linux_model::UserLimitARM64,
                                    0,
                                    PageSize,
                                    Native.LoadBias,
                                    false};
  auto Backend = createExecutionBackend(Options.Backend,
                                        ExecutionContract::CheckedUserAArch64,
                                        *Space, GuestArchitecture::AArch64);
  if (!Backend)
    return Backend.takeError();
  if (auto E = Backend->CPU->writeRegister(CPURegister::AArch64TPIDR_EL0,
                                           {TLSAddress, 0}))
    return std::move(E);
  ProcessResult Result{ProcessProfile::AndroidNativeAArch64,
                       GuestArchitecture::AArch64, Backend->Kind,
                       Backend->Reason};
  Result.Entry = Result.PC = Linked->Entry;
  Result.InitializersEnabled = Native.Initialize;
  auto Budget = ExecutionBudget::create(Options.Limits);
  if (!Budget)
    return Budget.takeError();
  std::shared_ptr<ExecutionBudget> Resources(std::move(*Budget));
  auto Observe = [&](uint64_t PC, uint32_t) {
    if (!Native.TraceLimit)
      return;
    if (Result.Trace.size() < Native.TraceLimit)
      Result.Trace.push_back(PC);
    else
      Result.TraceTruncated = true;
  };
  auto Session =
      ExecutionSession::create(std::move(Backend->CPU), Resources, {}, Observe);
  if (!Session)
    return Session.takeError();
  auto &CPU = (*Session)->cpu();
  linux_model::LinuxMemory Memory(**Space, Layout, Linked->InitialBreak,
                                  Options);
  Bionic LibC(CPU, Memory, Layout, Options, Result, *Resources, *Linked);
  size_t NextConstructor = 0;
  bool MainCall = false;
  auto Prepare = [&]() -> llvm::Error {
    std::vector<uint64_t> Arguments;
    if (NextConstructor < Linked->Constructors.size()) {
      Result.PC = Linked->Constructors[NextConstructor++];
      // An explicit empty guest process: argc=0, argv/envp each point to NULL.
      Arguments = {0, TLSAddress + 0x100, TLSAddress + 0x108};
    } else {
      MainCall = true;
      Result.PC = Linked->Entry;
      Arguments = Native.Arguments;
    }
    auto Frame = Calls->prepareCall(CPU, StackBase, Options.StackSize, ReturnPC,
                                    Arguments);
    if (!Frame)
      return Frame.takeError();
    return CPU.writeRegister(CPURegister::AArch64PC, {Result.PC, 0});
  };
  if (auto E = Prepare())
    return std::move(E);
  auto RuntimeFailure = [&](llvm::Error E) {
    Result.Stop = ProcessStopReason::RuntimeFailure;
    Result.Diagnostic = llvm::toString(std::move(E));
  };
  while (true) {
    auto Exit = (*Session)->run(Result.PC, Options.InstructionQuantum);
    if (!Exit) {
      RuntimeFailure(Exit.takeError());
      break;
    }
    Result.LastCPUExit = std::move(Exit->CPU);
    auto PC = CPU.readRegister(CPURegister::AArch64PC);
    if (!PC) {
      RuntimeFailure(PC.takeError());
      break;
    }
    Result.PC = (*PC)[0];
    if (Exit->Kind == SessionExitKind::Quantum)
      continue;
    if (Exit->Kind == SessionExitKind::InstructionLimit) {
      Result.Stop = ProcessStopReason::InstructionLimit;
      Result.Diagnostic = runtime::InstructionLimit;
      break;
    }
    if (Exit->Kind == SessionExitKind::Timeout) {
      Result.Stop = ProcessStopReason::Timeout;
      Result.Diagnostic = runtime::Timeout;
      break;
    }
    if (!Result.LastCPUExit ||
        Result.LastCPUExit->Kind != ExecutionExitKind::ServiceRequest) {
      Result.Stop = ProcessStopReason::CPUFailure;
      Result.Diagnostic = Result.LastCPUExit ? Result.LastCPUExit->Diagnostic
                                             : "missing CPU exit";
      break;
    }
    if (!Resources->consumeEvents()) {
      Result.Stop = ProcessStopReason::EventLimit;
      Result.Diagnostic = runtime::EventLimit;
      break;
    }
    auto Request = (*Session)->takeServiceRequest();
    if (!Request) {
      RuntimeFailure(Request.takeError());
      break;
    }
    if (Request->Immediate == ModelTrap && Request->PC == ReturnPC) {
      if (!MainCall) {
        if (auto E = Prepare()) {
          RuntimeFailure(std::move(E));
          break;
        }
        continue;
      }
      auto Value = CPU.readRegister(CPURegister::AArch64X0);
      if (!Value) {
        RuntimeFailure(Value.takeError());
        break;
      }
      Result.ReturnValue = (*Value)[0];
      Result.Stop = ProcessStopReason::Returned;
      break;
    }
    auto Import = Linked->Imports.find(Request->PC);
    if (Request->Immediate == ModelTrap && Import != Linked->Imports.end()) {
      NativeCallEvent Event{Request->PC, Import->second};
      auto Provider = Linked->DynamicProviders.find(Request->PC);
      if (Provider != Linked->DynamicProviders.end())
        Event.Library = Provider->second;
      bool Failed = false;
      for (size_t I = 0; I < Event.Arguments.size(); ++I) {
        auto V = CPU.readRegister(Calls->info().Arguments[I]);
        if (!V) {
          RuntimeFailure(V.takeError());
          Failed = true;
          break;
        }
        Event.Arguments[I] = (*V)[0];
      }
      if (Failed)
        break;
      Result.NativeCalls.push_back(Event);
      auto Value = LibC.invoke(Result.NativeCalls.back());
      if (!Value) {
        RuntimeFailure(Value.takeError());
        if (LibC.timedOut())
          Result.Stop = ProcessStopReason::Timeout;
        break;
      }
      if (!*Value)
        break;
      Result.NativeCalls.back().Result = **Value;
      if (auto E = linux_model::returnService(CPU, *Request, **Value)) {
        RuntimeFailure(std::move(E));
        break;
      }
    } else {
      // Only Linux svc #0 is a kernel call. A forged model trap never gains
      // an import model merely by carrying its immediate operand.
      if (Request->Immediate) {
        Result.Stop = ProcessStopReason::UnsupportedService;
        Result.Diagnostic = "unsupported Android SVC immediate";
        break;
      }
      auto Event = linux_model::readService(CPU, *Request);
      if (!Event) {
        RuntimeFailure(Event.takeError());
        break;
      }
      Result.Services.push_back(*Event);
      auto Value = linux_model::handleService(CPU, Memory, *Event, Layout,
                                              Options, Result);
      if (!Value) {
        RuntimeFailure(Value.takeError());
        break;
      }
      if (!*Value)
        break;
      Result.Services.back().Result = **Value;
      if (auto E = linux_model::returnService(CPU, *Request, **Value)) {
        RuntimeFailure(std::move(E));
        break;
      }
    }
    Result.PC = Request->NextPC;
  }
  Result.Instructions = Resources->instructions();
  Result.Events = Resources->events();
  // A terminal CPU fault can prohibit model memory access. Preserve that
  // original failure; memory snapshots are produced only at resumable stops.
  if (Result.Stop != ProcessStopReason::CPUFailure &&
      Result.Stop != ProcessStopReason::RuntimeFailure) {
    for (const auto &Read : Native.ReadMemory) {
      auto Access = CPU.canAccess(Read.Address, Read.Size,
                                  emulation::Read | UserAccessible);
      if (!Access) {
        RuntimeFailure(Access.takeError());
        break;
      }
      if (!*Access) {
        RuntimeFailure(failure("requested memory became unreadable"));
        break;
      }
      NativeMemorySnapshot Snapshot{Read.Address,
                                    std::vector<uint8_t>(Read.Size)};
      if (auto E = CPU.read(Read.Address, Snapshot.Bytes)) {
        RuntimeFailure(std::move(E));
        break;
      }
      Result.MemorySnapshots.push_back(std::move(Snapshot));
    }
  }
  return Result;
}
} // namespace neverd::emulation::android_model
