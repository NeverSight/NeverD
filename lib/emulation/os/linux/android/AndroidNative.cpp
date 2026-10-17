//===- AndroidNative.cpp - Bounded Android AArch64 native workloads ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "../../../runtime/RuntimeValues.h"
#include "AndroidInternal.h"
#include "AndroidLog.h"
#include "AndroidObservation.h"
#include "AndroidResolver.h"
#include "AndroidThreads.h"

#include "neverd/emulation/ExecutionSession.h"
#include "neverd/loader/ELF/ELFLoader.h"

#include <set>

namespace neverd::emulation::android_model {
llvm::Expected<ProcessResult> runNative(const std::filesystem::path &Path,
                                        const ProcessOptions &Options) {
  if (!Options.Android || !Options.Arguments.empty() ||
      !Options.Environment.empty())
    return failure(diagnostic::NativeOptions);
  const auto &Native = *Options.Android;
  if (auto E = validateAuxiliaryVector(Native))
    return std::move(E);
  if (auto E = validateCalendar(Native))
    return std::move(E);
  if (Native.ResolverQueries)
    if (auto E = validateResolverQueries(*Native.ResolverQueries))
      return std::move(E);
  if (auto E = validateLogRecords(Native.LogRecords))
    return std::move(E);
  if (!Native.ThreadLimit ||
      Native.ThreadLimit > thread_model::MaximumIdentities ||
      (Native.DrainThreads && Native.ThreadLimit == 1))
    return failure(diagnostic::ThreadOptions);
  if (Native.EntrySymbol.empty() == !Native.EntryAddress.has_value() ||
      Native.EntrySymbol.find('\0') != std::string::npos)
    return failure(diagnostic::EntrySelection);
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
    return failure(diagnostic::ExecutionLimits);
  auto Budget = ExecutionBudget::create(Options.Limits);
  if (!Budget)
    return Budget.takeError();
  std::shared_ptr<ExecutionBudget> Resources(std::move(*Budget));
  uint64_t ReportSize = Native.TraceLimit * 8;
  if (Native.ThreadLimit > 1) {
    const uint64_t ThreadBytes = Native.ThreadLimit * thread_model::ReportBytes;
    if (ThreadBytes > Options.OutputLimit - ReportSize)
      return failure(diagnostic::SnapshotLimits);
    ReportSize += ThreadBytes;
  }
  for (const auto &Read : Native.ReadMemory) {
    if (!Read.Size || Read.Size > Options.OutputLimit - ReportSize ||
        Read.Address > UINT64_MAX - Read.Size)
      return failure(diagnostic::SnapshotLimits);
    ReportSize += Read.Size;
  }
  if (Native.ObserveExecution) {
    auto Bytes = executionObservationBytes(*Native.ObserveExecution,
                                           Options.OutputLimit - ReportSize);
    if (!Bytes)
      return Bytes.takeError();
    ReportSize += *Bytes;
  }
  uint64_t SymbolCount = 0, CatalogBytes = 0;
  if (Native.LogRecords)
    for (const auto &Record : *Native.LogRecords)
      CatalogBytes += 28 + Record.Payload.size();
  if (Native.Libraries.size() > 256)
    return failure(diagnostic::LibraryCount);
  for (const auto &[Library, Symbols] : Native.Libraries) {
    if (Library.empty() || Library.size() > 1024 ||
        Library.find('\0') != std::string::npos)
      return failure(diagnostic::LibraryName);
    CatalogBytes += Library.size();
    std::set<std::string> Unique;
    for (const auto &Symbol : Symbols) {
      if (++SymbolCount > 4096 || Symbol.empty() || Symbol.size() > 1024 ||
          Symbol.find('\0') != std::string::npos ||
          !Unique.insert(Symbol).second)
        return failure(diagnostic::SymbolCatalogue);
      CatalogBytes += Symbol.size();
    }
  }
  if (Native.DefaultScope) {
    if (Native.DefaultScope->size() > Native.Libraries.size())
      return failure(diagnostic::DefaultScopeSize);
    std::set<std::string> Unique;
    for (const auto &Library : *Native.DefaultScope) {
      if (!Native.Libraries.count(Library) || !Unique.insert(Library).second)
        return failure(diagnostic::DefaultScopeMembership);
      CatalogBytes += Library.size();
    }
  }
  if (CatalogBytes > Options.MemoryLimit)
    return failure(diagnostic::CatalogueMemoryLimit);
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
  auto Properties = initializeProperties(**Space, Options, *Resources);
  if (!Properties)
    return Properties.takeError();
  Linked->Properties = std::move(*Properties);
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
      return failure(diagnostic::MemoryRegion);
    unsigned Permissions =
        Read | Write | UserAccessible | (Region.Executable ? Execute : 0u);
    if (auto E = (*Space)->map(Region.Address, Region.Size, Permissions))
      return std::move(E);
    if (auto E = initializeMemoryRegion(**Space, Region, *Resources))
      return std::move(E);
  }
  auto EntryReads = Native.ReadMemory;
  if (Native.ObserveExecution)
    EntryReads.insert(EntryReads.end(),
                      Native.ObserveExecution->ReadMemory.begin(),
                      Native.ObserveExecution->ReadMemory.end());
  for (const auto &Read : EntryReads) {
    if (!Read.RequireMappedAtEntry)
      continue;
    auto Accessible = (*Space)->canAccess(Read.Address, Read.Size,
                                          emulation::Read | UserAccessible);
    if (!Accessible)
      return Accessible.takeError();
    if (!*Accessible)
      return failure(diagnostic::SnapshotEntry);
  }
  auto Calls = IntegerABI::get(IntegerCallingConvention::AAPCS64);
  if (!Calls)
    return Calls.takeError();
  linux_model::MemoryLayout Layout{linux_model::UserLimitARM64, PageSize};
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
  uint64_t TraceThreadID = linux_model::ThreadID;
  auto Observe = [&](uint64_t PC, uint32_t) {
    if (!Native.TraceLimit || Result.TraceTruncated)
      return;
    if (Result.Trace.size() < Native.TraceLimit) {
      if (Native.ThreadLimit > 1 &&
          (Result.TraceThreads.empty() ||
           Result.TraceThreads.back().ID != TraceThreadID)) {
        if (sizeof(NativeTraceThread) > Options.OutputLimit - ReportSize) {
          Result.TraceTruncated = true;
          return;
        }
        ReportSize += sizeof(NativeTraceThread);
        Result.TraceThreads.push_back({Result.Trace.size(), TraceThreadID});
      }
      Result.Trace.push_back(PC);
    } else
      Result.TraceTruncated = true;
  };
  auto Session =
      ExecutionSession::create(std::move(Backend->CPU), Resources, {}, Observe);
  if (!Session)
    return Session.takeError();
  auto &CPU = (*Session)->cpu();
  if (Native.ObserveExecution) {
    Result.ExecutionObservationsEnabled = true;
    std::vector<ExecutionWatch> Watches;
    for (uint64_t PC : Native.ObserveExecution->PCs)
      Watches.push_back({PC, 4});
    if (auto E = (*Session)->watchExecution(std::move(Watches)))
      return std::move(E);
  }
  linux_model::LinuxServices Kernel(CPU, Layout, Linked->InitialBreak, Options,
                                    Result);
  GuestThreads Threads(CPU, *Calls, Options, Result, Kernel.clock(),
                       Kernel.priority());
  if (auto E = Threads.initialize(StackBase))
    return std::move(E);
  if (auto E = Kernel.initializeProc())
    return std::move(E);
  Kernel.setTaskProvider([&] { return Threads.procTasks(); });
  Bionic LibC(CPU, Kernel, Layout, Options, Result, *Resources, *Linked,
              &Threads);
  if (auto E = LibC.initializeDex())
    return std::move(E);
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
    Threads.setReturnSP(Frame->ReturnStackPointer);
    return CPU.writeRegister(CPURegister::AArch64PC, {Result.PC, 0});
  };
  if (auto E = Prepare())
    return std::move(E);
  auto RuntimeFailure = [&](llvm::Error E) {
    Result.Stop = ProcessStopReason::RuntimeFailure;
    Result.Diagnostic = llvm::toString(std::move(E));
  };
  bool NeedSchedule = false;
  uint64_t QuantumRemaining = Options.InstructionQuantum;
  bool ResumeObservation = false;
  auto FinishThread = [&](uint64_t Value, uint32_t ExitStatus = 0) {
    if (!Threads.callbacks().empty()) {
      Result.Stop = ProcessStopReason::UnsupportedService;
      Result.Diagnostic = diagnostic::ThreadCallbackExit;
      return false;
    }
    if (auto E = LibC.jniThreadExit()) {
      RuntimeFailure(std::move(E));
      return false;
    }
    if (auto E = Threads.finish(Value, ExitStatus)) {
      RuntimeFailure(std::move(E));
      return false;
    }
    NeedSchedule = true;
    return true;
  };
  auto StartCallback = [&](const GuestCallback &Callback) -> llvm::Error {
    // The suspended import's live stack remains in place. All admitted
    // callback signatures fit the ordinary scalar register ABI.
    if (Callback.Arguments.size() > Calls->info().Arguments.size())
      return failure(diagnostic::CallbackArguments);
    for (size_t I = 0; I < Callback.Arguments.size(); ++I)
      if (auto E = CPU.writeRegister(Calls->info().Arguments[I],
                                     {Callback.Arguments[I], 0}))
        return E;
    if (auto E = CPU.writeRegister(Calls->info().Link, {ReturnPC, 0}))
      return E;
    if (auto E = CPU.writeRegister(CPURegister::AArch64PC, {Callback.Entry, 0}))
      return E;
    Result.PC = Callback.Entry;
    return llvm::Error::success();
  };
  while (true) {
    if (Threads.enabled() && (NeedSchedule || !QuantumRemaining)) {
      auto Ready = Threads.schedule(LibC);
      if (!Ready) {
        RuntimeFailure(Ready.takeError());
        if (LibC.timedOut())
          Result.Stop = ProcessStopReason::Timeout;
        break;
      }
      if (!*Ready)
        break;
      auto PC = CPU.readRegister(CPURegister::AArch64PC);
      if (!PC) {
        RuntimeFailure(PC.takeError());
        break;
      }
      Result.PC = (*PC)[0];
      NeedSchedule = false;
      QuantumRemaining = Options.InstructionQuantum;
    }
    if (!QuantumRemaining || (!Threads.enabled() && !ResumeObservation))
      QuantumRemaining = Options.InstructionQuantum;
    ResumeObservation = false;
    if (auto E = Kernel.synchronizeDevices()) {
      RuntimeFailure(std::move(E));
      break;
    }
    TraceThreadID = Threads.id();
    auto &Callbacks = Threads.callbacks();
    const uint64_t Before = Resources->instructions();
    auto Exit = (*Session)->run(Result.PC, QuantumRemaining);
    QuantumRemaining -=
        std::min(QuantumRemaining, Resources->instructions() - Before);
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
    if (Exit->Kind == SessionExitKind::ExecutionWatch) {
      const auto &Observation = *Native.ObserveExecution;
      if (Result.ExecutionObservations.size() == Observation.Limit) {
        Result.ExecutionObservationsTruncated = true;
        if (auto E = (*Session)->watchExecution({})) {
          RuntimeFailure(std::move(E));
          break;
        }
      } else {
        auto Captured = captureExecutionObservation(CPU, Observation, Result.PC,
                                                    Threads.id(),
                                                    Resources->instructions());
        if (!Captured) {
          RuntimeFailure(Captured.takeError());
          break;
        }
        Result.ExecutionObservations.push_back(std::move(*Captured));
      }
      // A read-only stop consumes neither a scheduling quantum nor a service
      // event. Resume the watched instruction in this same thread.
      ResumeObservation = true;
      continue;
    }
    if ((Exit->Kind == SessionExitKind::Quantum ||
         (Exit->Kind == SessionExitKind::CPU && Result.LastCPUExit &&
          Result.LastCPUExit->Kind == ExecutionExitKind::ServiceRequest)) &&
        !Kernel.clock().accountInstructions(Resources->instructions(), Result))
      break;
    if (Exit->Kind == SessionExitKind::Quantum) {
      NeedSchedule = true;
      continue;
    }
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
                                             : diagnostic::MissingCPUExit;
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
      if (!Callbacks.empty()) {
        const auto Pending = Callbacks.back();
        auto SP = CPU.readRegister(Calls->info().StackPointer);
        if (!SP) {
          RuntimeFailure(SP.takeError());
          break;
        }
        if ((*SP)[0] != Pending.StackPointer) {
          RuntimeFailure(failure(diagnostic::CallbackStack));
          break;
        }
        auto Value = LibC.finishCallback(Pending.Call);
        if (!Value) {
          RuntimeFailure(Value.takeError());
          if (LibC.timedOut())
            Result.Stop = ProcessStopReason::Timeout;
          break;
        }
        if (!*Value)
          break;
        if (auto *Next = std::get_if<GuestCallback>(&**Value)) {
          if (auto E = StartCallback(*Next)) {
            RuntimeFailure(std::move(E));
            break;
          }
          Callbacks.back().Call = *Next;
          continue;
        }
        const uint64_t Returned = std::get<uint64_t>(**Value);
        if (auto E = CPU.writeRegister(Calls->info().Link, {Pending.Link, 0})) {
          RuntimeFailure(std::move(E));
          break;
        }
        if (auto E =
                linux_model::returnService(CPU, Pending.Request, Returned)) {
          RuntimeFailure(std::move(E));
          break;
        }
        Result.NativeCalls[Pending.Event].Result = Returned;
        Result.PC = Pending.Request.NextPC;
        Callbacks.pop_back();
        continue;
      }
      if (Threads.isEntry() && !MainCall) {
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
      if (Threads.enabled()) {
        auto SP = CPU.readRegister(Calls->info().StackPointer);
        if (!SP) {
          RuntimeFailure(SP.takeError());
          break;
        }
        if ((*SP)[0] != Threads.returnSP()) {
          RuntimeFailure(failure(diagnostic::ThreadStack));
          break;
        }
      }
      const bool Entry = Threads.isEntry();
      if (Entry)
        Result.ReturnValue = (*Value)[0];
      if (!Threads.enabled() || (Entry && !Native.DrainThreads)) {
        if (auto E = Threads.finish((*Value)[0])) {
          RuntimeFailure(std::move(E));
          break;
        }
        Result.Stop = ProcessStopReason::Returned;
        break;
      }
      if (!FinishThread((*Value)[0]))
        break;
      continue;
    }
    auto Import = Linked->Imports.find(Request->PC);
    if (Request->Immediate == ModelTrap && Import != Linked->Imports.end()) {
      NativeCallEvent Event{Request->PC, Import->second};
      if (Options.LinuxFiles && Options.LinuxFiles->IoctlTrace &&
          (Event.Name == "ioctl" || Event.Name == "syscall")) {
        // AArch64 reads the link register, not guest stack memory. Missing
        // observation must not change the native call's result.
        auto Return = Calls->readReturnAddress(CPU, 0);
        if (Return)
          Event.ReturnAddress = *Return;
        else
          llvm::consumeError(Return.takeError());
      }
      if (Threads.enabled())
        Event.ThreadID = Threads.id();
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
      if (!*Value && Threads.kernel() && Threads.kernel()->Exit) {
        if (!FinishThread(0, *Threads.kernel()->Exit))
          break;
        continue;
      }
      if (!*Value)
        break;
      if (std::holds_alternative<GuestThreadWait>(**Value)) {
        Threads.suspend(*Request, Result.NativeCalls.size() - 1);
        NeedSchedule = true;
        continue;
      }
      if (auto *Ended = std::get_if<GuestThreadExit>(&**Value)) {
        if (!FinishThread(Ended->Value))
          break;
        continue;
      }
      if (auto *Callback = std::get_if<GuestCallback>(&**Value)) {
        auto Link = CPU.readRegister(Calls->info().Link);
        if (!Link) {
          RuntimeFailure(Link.takeError());
          break;
        }
        auto SP = CPU.readRegister(Calls->info().StackPointer);
        if (!SP) {
          RuntimeFailure(SP.takeError());
          break;
        }
        if (auto E = Calls->validateStackPointer((*SP)[0])) {
          RuntimeFailure(std::move(E));
          break;
        }
        // Reuse the current guest stack below the suspended import. Preparing
        // a fresh top-level frame would overwrite its caller's live locals.
        if (auto E = StartCallback(*Callback)) {
          RuntimeFailure(std::move(E));
          break;
        }
        Callbacks.push_back({*Callback, *Request, Result.NativeCalls.size() - 1,
                             (*Link)[0], (*SP)[0]});
        continue;
      }
      uint64_t Returned = std::get<uint64_t>(**Value);
      Result.NativeCalls.back().Result = Returned;
      if (auto E = linux_model::returnService(CPU, *Request, Returned)) {
        RuntimeFailure(std::move(E));
        break;
      }
    } else {
      // Only Linux svc #0 is a kernel call. A forged model trap never gains
      // an import model merely by carrying its immediate operand.
      if (Request->Immediate) {
        Result.Stop = ProcessStopReason::UnsupportedService;
        Result.Diagnostic = diagnostic::SVCImmediate;
        break;
      }
      auto Event = linux_model::readService(CPU, *Request);
      if (!Event) {
        RuntimeFailure(Event.takeError());
        break;
      }
      if (Threads.enabled())
        Event->ThreadID = Threads.id();
      Result.Services.push_back(*Event);
      auto Value = Kernel.handle(*Event, Threads.kernel());
      if (!Value) {
        RuntimeFailure(Value.takeError());
        break;
      }
      if (!*Value && Threads.kernel() && Threads.kernel()->Exit) {
        if (!FinishThread(0, *Threads.kernel()->Exit))
          break;
        continue;
      }
      if (!*Value && Threads.waitSleep()) {
        Threads.suspend(*Request, Result.Services.size() - 1,
                        GuestThreads::WaitSource::KernelService);
        NeedSchedule = true;
        continue;
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
  Threads.report();
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
        RuntimeFailure(failure(diagnostic::SnapshotReturn));
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
