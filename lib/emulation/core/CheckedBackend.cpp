//===- CheckedBackend.cpp - Shared checked execution lifecycle -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "CheckedBackend.h"

#include "ExecutionDeadline.h"
#include "ExecutionDiagnostics.h"
#include "ExecutionExitBuilder.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/ScopeExit.h"

#include <array>
#include <iterator>

namespace neverd::emulation {
struct CheckedBackend::InstructionCache {
  cs_detail *DecoderDetail = nullptr;
  struct Entry {
    cs_insn Instruction;
    cs_detail Detail;
  };
  std::array<std::unique_ptr<Entry>, execution_limits::InstructionCacheEntries>
      Entries;
  std::unique_ptr<Entry> &slot(uint64_t PC) {
    return Entries[((PC >> 2) ^ (PC >> 12)) % Entries.size()];
  }
};

CheckedBackend::CheckedBackend(unsigned MaxInstructionBytes,
                               unsigned InstructionAlignment, bool UserMode,
                               bool Direct)
    : UserMode(UserMode), Direct(Direct), InstructionBytes(MaxInstructionBytes),
      InstructionAlignment(InstructionAlignment) {}

bool CheckedBackend::executionWatched(uint64_t Address) const {
  const auto I = llvm::upper_bound(
      ExecutionWatches, Address, [](uint64_t Address, const ExecutionWatch &W) {
        return Address < W.Address;
      });
  return I != ExecutionWatches.begin() &&
         Address - std::prev(I)->Address < std::prev(I)->Size;
}
llvm::Error CheckedBackend::mapMMIO(uint64_t A, uint64_t N,
                                    GuestMMIOCallbacks Callbacks) {
  if (auto E = mutableMemory())
    return E;
  if (!supportsDeviceMappings())
    return diagnostic::error(diagnostic::DeviceMapping);
  if (!canonicalRange(A, N))
    return diagnostic::error(diagnostic::InvalidMapping);
  return addressSpace()->mapMMIO(A, N, std::move(Callbacks));
}
llvm::Error CheckedBackend::unmapMMIO(uint64_t A, uint64_t N) {
  if (auto E = mutableMemory())
    return E;
  return addressSpace()->unmapMMIO(A, N);
}
using diagnostic::error;
llvm::Error CheckedBackend::initializeDecoder(cs_arch Arch, cs_mode Mode) {
  if (cs_open(Arch, Mode, &Decoder) != CS_ERR_OK ||
      cs_option(Decoder, CS_OPT_DETAIL, CS_OPT_ON) != CS_ERR_OK)
    return error(diagnostic::Decode);
  Decoded = cs_malloc(Decoder);
  if (!Decoded)
    return error(diagnostic::Decode);
  CachedInstructions = std::make_unique<InstructionCache>();
  CachedInstructions->DecoderDetail = Decoded->detail;
  return llvm::Error::success();
}
CheckedBackend::~CheckedBackend() {
  if (Decoded) {
    if (CachedInstructions)
      Decoded->detail = CachedInstructions->DecoderDetail;
    cs_free(Decoded, 1);
  }
  if (Decoder)
    cs_close(&Decoder);
}

llvm::Error
CheckedBackend::checkExecutionState(bool AllowResourceContinuation) const {
  if (FirstFault || RecoverableFault)
    return error(diagnostic::Faulted);
  if (PendingService)
    return error(diagnostic::PendingService);
  if (ResourcePending || (ResourceResumePC && !AllowResourceContinuation))
    return error(diagnostic::ResourcePausePending);
  return llvm::Error::success();
}

llvm::Error CheckedBackend::mutableMemory() const {
  if (auto E = checkExecutionState())
    return E;
  if (Running)
    return error(diagnostic::Running);
  return Memory->mutableMemory();
}

llvm::Error
CheckedBackend::bindAddressSpace(std::shared_ptr<AddressSpace> Space) {
  if (auto E = mutableMemory())
    return E;
  return Memory->bind(
      std::move(Space),
      [this](uint64_t A, uint64_t N) { return canonicalRange(A, N); },
      supportsDeviceMappings());
}

llvm::Error CheckedBackend::map(uint64_t A, uint64_t N, unsigned P) {
  if (auto E = mutableMemory())
    return E;
  if (!canonicalRange(A, N))
    return error(diagnostic::InvalidMapping);
  return Memory->map(A, N, P);
}

llvm::Error CheckedBackend::mapAlias(uint64_t A, uint64_t S, uint64_t N,
                                     unsigned P) {
  return replaceAliases({}, {GuestAliasMapping{A, S, N, P}});
}

llvm::Error CheckedBackend::unmapAlias(uint64_t A, uint64_t N) {
  return replaceAliases({GuestAliasRange{A, N}}, {});
}

llvm::Error
CheckedBackend::replaceAliases(llvm::ArrayRef<GuestAliasRange> Remove,
                               llvm::ArrayRef<GuestAliasMapping> Add) {
  if (auto E = mutableMemory())
    return E;
  for (const auto &R : Add)
    if (!canonicalRange(R.Address, R.Size) || !canonicalRange(R.Source, R.Size))
      return error(diagnostic::InvalidMapping);
  return Memory->aliases(Remove, Add);
}

llvm::Error CheckedBackend::protect(uint64_t A, uint64_t N, unsigned P) {
  if (auto E = mutableMemory())
    return E;
  return Memory->protect(A, N, P);
}

llvm::Error CheckedBackend::access(uint64_t A, uint64_t N, unsigned P,
                                   bool Recoverable, bool Guest) {
  if (FirstFault)
    return error(diagnostic::Faulted);
  if (auto Failure = Memory->firstAccessFailure(
          A, N, Guest ? executionPermissions(P) : P)) {
    auto Access = P == Execute ? BackendAccessKind::Execute
                  : P & Write  ? BackendAccessKind::Write
                               : BackendAccessKind::Read;
    BackendFault F{Failure->Kind, programCounter(), Failure->Address,
                   Failure->Size, Access,           std::nullopt};
    if (Guest && Running && !Direct &&
        F.Kind == BackendFaultKind::UnmappedMemory && !ResourcePending &&
        Hooks.ResourcePause) {
      ResourcePause Pause{F, programCounter(), programCounter(),
                          P == Execute
                              ? ResourceRestartKind::FetchBoundary
                              : ResourceRestartKind::ArchitecturalRetry,
                          P != Execute};
      Pause.RequiredPermissions = executionPermissions(P);
      if (Hooks.ResourcePause(Pause) && !FirstFault && !RecoverableFault &&
          !DeviceFailed) {
        ResourcePending = Pause;
        StopRequested = true;
        return llvm::Error::success();
      }
    }
    return raiseFault(F, Recoverable && P != Execute);
  }
  return llvm::Error::success();
}

llvm::Error CheckedBackend::raiseFault(BackendFault Fault, bool Recoverable) {
  if (FirstFault || RecoverableFault)
    return error(diagnostic::Faulted);
  if (Recoverable && Hooks.RecoverableFault && Hooks.RecoverableFault(Fault)) {
    onGuestException();
    RecoverableFault = Fault;
    StopRequested = true;
    return llvm::Error::success();
  }
  onGuestException();
  FirstFault = Fault;
  if (Fault.Interrupt && Hooks.Interrupt)
    Hooks.Interrupt(*Fault.Interrupt);
  if (Fault.Address && Fault.Size && Fault.Access && Hooks.Fault)
    Hooks.Fault(*Fault.Address, *Fault.Size,
                backendAccessKindName(*Fault.Access));
  return error(Fault.Access ? diagnostic::MemoryAccess : diagnostic::Faulted);
}

llvm::Error CheckedBackend::read(uint64_t A, llvm::MutableArrayRef<uint8_t> B) {
  if (ResourcePending || ResourceResumePC)
    return error(diagnostic::ResourcePausePending);
  auto Lock = Memory->lock();
  if (!Lock)
    return Lock.takeError();
  if (auto E = access(A, B.size(), Read))
    return E;
  return Memory->read(A, B);
}

llvm::Error CheckedBackend::write(uint64_t A, llvm::ArrayRef<uint8_t> B) {
  if (auto E = mutableMemory())
    return E;
  if (auto E = access(A, B.size(), Write))
    return E;
  return Memory->write(A, B);
}

llvm::Error CheckedBackend::fetch(uint64_t A,
                                  llvm::MutableArrayRef<uint8_t> B) {
  if (ResourcePending || ResourceResumePC)
    return error(diagnostic::ResourcePausePending);
  auto Lock = Memory->lock();
  if (!Lock)
    return Lock.takeError();
  if (auto E = access(A, B.size(), Execute))
    return E;
  return Memory->read(A, B, Execute);
}

llvm::Expected<bool> CheckedBackend::canAccess(uint64_t A, uint64_t N,
                                               unsigned P) const {
  if (FirstFault || (P & ~GuestPermissionMask))
    return error(diagnostic::Faulted);
  return Memory->addressSpace()->canAccess(A, N, P);
}

llvm::Error CheckedBackend::validateBacking(uint64_t A, uint64_t N) const {
  if (auto E = mutableMemory())
    return E;
  return Memory->addressSpace()->validateBacking(A, N);
}

llvm::Error CheckedBackend::readBacking(uint64_t A,
                                        llvm::MutableArrayRef<uint8_t> B) {
  if (auto E = validateBacking(A, B.size()))
    return E;
  return Memory->read(A, B, 0);
}

llvm::Error CheckedBackend::writeBacking(uint64_t A,
                                         llvm::ArrayRef<uint8_t> B) {
  if (auto E = validateBacking(A, B.size()))
    return E;
  return Memory->write(A, B, 0);
}

llvm::Error CheckedBackend::snapshotBacking(uint64_t A,
                                            llvm::MutableArrayRef<uint8_t> B) {
  if (Running)
    return error(diagnostic::Running);
  return Memory->addressSpace()->snapshotBacking(A, B);
}

llvm::Error CheckedBackend::installHooks(BackendHooks H) {
  if (auto E = mutableMemory())
    return E;
  if (H.ResourcePause && !supportsResourcePause())
    return error(diagnostic::ResourcePauseUnsupported);
  Hooks = std::move(H);
  return llvm::Error::success();
}

llvm::Error CheckedBackend::setMemoryWriteWatches(
    const std::vector<MemoryWriteWatch> &Watches) {
  if (auto E = mutableMemory())
    return E;
  for (const auto &W : Watches)
    if (!W.Size || W.Size - 1 > UINT64_MAX - W.Address)
      return error(diagnostic::WriteWatchRange);
  if (Memory->setWriteWatches(Watches))
    ++WatchEpoch;
  return llvm::Error::success();
}

llvm::Expected<bool> CheckedBackend::decodeInstruction(uint64_t PC) {
  if (PC % InstructionAlignment)
    return false;
  size_t Count = InstructionBytes.size();
  if (Count - 1 > UINT64_MAX - PC)
    Count = UINT64_MAX - PC + 1;
  const unsigned Permissions = executionPermissions(Execute);
  if (auto Failure = Memory->firstAccessFailure(PC, Count, Permissions))
    Count = Failure->Address - PC;
  if (!Count)
    return false;
  // Fetch one accessible prefix. Decoding a short instruction still succeeds
  // at a page boundary, without reading a faulting tail or locking per byte.
  if (auto E = Memory->read(
          PC, llvm::MutableArrayRef(InstructionBytes).take_front(Count),
          Permissions))
    return std::move(E);
  const uint8_t *Input = InstructionBytes.data();
  auto &Cached = CachedInstructions->slot(PC);
  if (Cached && Cached->Instruction.address == PC &&
      Cached->Instruction.size <= Count &&
      std::equal(Cached->Instruction.bytes,
                 Cached->Instruction.bytes + Cached->Instruction.size, Input)) {
    // The current readable/executable prefix was fetched above. Metadata is
    // owned, including printer-adjusted aliases and complete operand detail;
    // neither an old permission nor a borrowed decoder packet is reused.
    *Decoded = Cached->Instruction;
    Decoded->detail = &Cached->Detail;
    return true;
  }
  Decoded->detail = CachedInstructions->DecoderDetail;
  uint64_t DecodePC = PC;
  if (!cs_disasm_iter(Decoder, &Input, &Count, &DecodePC, Decoded))
    return false;
  if (!Cached)
    Cached = std::make_unique<InstructionCache::Entry>();
  Cached->Instruction = *Decoded;
  Cached->Instruction.detail = nullptr;
  Cached->Detail = *Decoded->detail;
  return true;
}

llvm::Expected<uint32_t> CheckedBackend::instructionSize(uint64_t Address) {
  if (auto E = mutableMemory())
    return std::move(E);
  auto Lock = Memory->lock();
  if (!Lock)
    return Lock.takeError();
  auto Decoding = decodeInstruction(Address);
  if (!Decoding)
    return Decoding.takeError();
  if (!*Decoding)
    return error(diagnostic::Decode);
  return Decoded->size;
}

std::optional<BackendFault> CheckedBackend::takeRecoverableFault() {
  return std::exchange(RecoverableFault, std::nullopt);
}
llvm::Error CheckedBackend::acknowledgeResourcePause() {
  if (!ResourcePending)
    return error(diagnostic::ResourcePauseMissing);
  if (Running || FirstFault || RecoverableFault || PendingService ||
      DeviceFailed)
    return error(diagnostic::Faulted);
  const auto &F = ResourcePending->Fault;
  auto Access = addressSpace()->canAccess(*F.Address, *F.Size,
                                          ResourcePending->RequiredPermissions);
  if (!Access)
    return Access.takeError();
  if (!*Access)
    return error(diagnostic::ResourcePauseUnresolved);
  ResourceResumePC = ResourcePending->ResumePC;
  ResourcePending.reset();
  return llvm::Error::success();
}

std::optional<ServiceRequest> CheckedBackend::takeServiceRequest() {
  if (Running)
    return std::nullopt;
  return std::exchange(PendingService, std::nullopt);
}

llvm::Expected<ExecutionExit> CheckedBackend::runUntilExit(uint64_t PC,
                                                           uint64_t Timeout) {
  if (Entered.test_and_set())
    return error(diagnostic::Running);
  auto Leave = llvm::scope_exit([&] { Entered.clear(); });
  if (auto E = checkExecutionState(true))
    return std::move(E);
  if (ResourceResumePC && *ResourceResumePC != PC)
    return error(diagnostic::ResourceResumePC);
  if (!Memory->parallelEnabled())
    if (auto E = Memory->mutableMemory())
      return std::move(E);
  auto Limit = makeExecutionDeadline(Timeout);
  if (!Limit)
    return Limit.takeError();
  StopRequested = false;
  Deadline = *Limit;
  bool Started = false, BackendFailed = false;
  auto E =
      withExecutionOwner([&] { return runImpl(PC, Started, BackendFailed); });
  if (!Started) {
    if (E)
      return std::move(E);
    return error(diagnostic::MissingExecutionStart);
  }
  // A transport failure or service interception can coincide with the deadline.
  // Retain elapsed budget independently of the higher-priority exit reason.
  if ((BackendFailed || PendingService) &&
      std::chrono::steady_clock::now() >= Deadline)
    TimedOut = true;
  if (FirstFault || BackendFailed || DeviceFailed) {
    ResourcePending.reset();
    ResourceResumePC.reset();
  }
  return makeExecutionExit(
      std::move(E), {.Fault = FirstFault,
                     .Recoverable = RecoverableFault,
                     .Service = PendingService,
                     .DeviceFailed = hasDeviceError(),
                     .BackendFailed = BackendFailed,
                     .InstructionRejected =
                         FirstFault && FirstFault->Kind ==
                                           BackendFaultKind::InvalidInstruction,
                     .StopRequested = StopRequested,
                     .DeadlineReached = TimedOut,
                     .Resource = ResourcePending});
}

llvm::Error CheckedBackend::executeDirect() {
  return error(diagnostic::DirectExecutionUnsupported);
}

llvm::Error CheckedBackend::runImpl(uint64_t PC, bool &Started,
                                    bool &BackendFailed) {
  // Free execution does not declare every guest store. Write-watch callbacks
  // locate selected changes, but cannot preserve physical write histories for
  // other bytes, including identical-value stores and interrupted execution.
  if (auto E = Memory->parallelEnabled()
                   ? Memory->beginParallelRun({Deadline, &StopRequested})
                   : Memory->beginRun(Direct ? RAMWriteTracking::Opaque
                                             : RAMWriteTracking::Declared))
    return E;
  auto Release = llvm::scope_exit([&] { Memory->endRun(); });
  {
    auto Startup = Memory->beginInstruction();
    if (!Startup)
      return Startup.takeError();
    const auto Space = Memory->addressSpace();
    const uint64_t Generation = Space->mappingGeneration();
    // The startup lease keeps mappings stable through validation and reuse.
    // External mutations advance the epoch; equal epochs on another address
    // space do not establish admission for this backend's fixed ISA policy.
    if (ValidatedMappingSpace.lock() != Space ||
        ValidatedMappingGeneration != Generation) {
      if (auto E = Memory->validateMappings(
              [this](uint64_t A, uint64_t N) { return canonicalRange(A, N); },
              supportsDeviceMappings()))
        return E;
      ValidatedMappingSpace = Space;
      ValidatedMappingGeneration = Generation;
    }
    setProgramCounter(PC);
  }
  ResourceResumePC.reset();
  Running = true;
  Started = true;
  TimedOut = false;
  auto Reset = llvm::scope_exit([&] { Running = false; });
  auto Interrupted = [&](llvm::Error E) {
    llvm::handleAllErrors(std::move(E),
                          [&](const MachineInterruptedError &Interruption) {
                            if (Interruption.stopRequested())
                              StopRequested = true;
                            TimedOut = Interruption.deadlineReached();
                          });
  };
  try {
    while (!StopRequested) {
      if (std::chrono::steady_clock::now() >= Deadline) {
        TimedOut = true;
        break;
      }
      auto Instruction = Memory->beginInstruction();
      if (!Instruction) {
        auto E = Instruction.takeError();
        if (!E.isA<MachineInterruptedError>())
          return E;
        Interrupted(std::move(E));
        break;
      }
      PC = programCounter();
      if (Direct) {
        if (auto E = executeDirect()) {
          if (!FirstFault && E.isA<MachineInterruptedError>()) {
            llvm::handleAllErrors(
                std::move(E), [&](const MachineInterruptedError &Interrupted) {
                  if (Interrupted.stopRequested())
                    StopRequested = true;
                  TimedOut = Interrupted.deadlineReached();
                });
            break;
          }
          if (!FirstFault) {
            BackendFailed = true;
            FirstFault = BackendFault{BackendFaultKind::UnhandledException,
                                      programCounter()};
          }
          return E;
        }
        if (FirstFault)
          return error(diagnostic::Faulted);
        if (PendingService)
          break;
        continue;
      }
      auto Decoding = decodeInstruction(PC);
      if (!Decoding)
        return Decoding.takeError();
      if (!*Decoding) {
        if (!(PC % InstructionAlignment) &&
            Memory->check(PC, 1, executionPermissions(Execute)))
          return access(PC, 1, Execute, false, true);
        if (!(PC % InstructionAlignment) && Hooks.ResourcePause) {
          uint64_t Count = InstructionBytes.size();
          if (Count - 1 > UINT64_MAX - PC)
            Count = UINT64_MAX - PC + 1;
          if (auto Missing = Memory->firstAccessFailure(
                  PC, Count, executionPermissions(Execute));
              Missing && Missing->Kind == BackendFaultKind::UnmappedMemory)
            return access(Missing->Address, 1, Execute, false, true);
        }
        FirstFault = BackendFault{BackendFaultKind::InvalidInstruction, PC};
        if (Hooks.InvalidInstruction)
          Hooks.InvalidInstruction();
        return llvm::make_error<UnsupportedExecutionError>();
      }
      if (Hooks.Instruction)
        Hooks.Instruction(PC, Decoded->size);
      if (FirstFault)
        return error(diagnostic::Faulted);
      if (StopRequested)
        break;
      if (UserMode) {
        PendingService = decodeServiceRequest(*Decoded);
        if (PendingService) {
          onGuestException();
          break;
        }
      }
      if (auto E = execute(*Decoded)) {
        if (!FirstFault && E.isA<MachineInterruptedError>()) {
          Interrupted(std::move(E));
          break;
        }
        if (!FirstFault) {
          const bool Unsupported = E.isA<UnsupportedExecutionError>();
          BackendFailed = !Unsupported;
          FirstFault =
              BackendFault{Unsupported ? BackendFaultKind::InvalidInstruction
                                       : BackendFaultKind::UnhandledException,
                           PC};
          if (Unsupported && Hooks.InvalidInstruction) {
            try {
              Hooks.InvalidInstruction();
            } catch (...) {
              // Error owns an unchecked payload until returned or consumed.
              // An observer exception must reach the outer fault-preserving
              // handler without aborting while this Error is unwound.
              llvm::consumeError(std::move(E));
              throw;
            }
          }
        }
        return E;
      }
      if (FirstFault)
        return error(diagnostic::Faulted);
      if (Memory->takeWatchedWrite() && Hooks.MemoryWritten)
        Hooks.MemoryWritten();
    }
  } catch (...) {
    // A secondary observer failure must not replace the original guest fault.
    if (!FirstFault) {
      BackendFailed = true;
      FirstFault =
          BackendFault{BackendFaultKind::UnhandledException, programCounter()};
    }
    return error(diagnostic::Callback);
  }
  return llvm::Error::success();
}
} // namespace neverd::emulation
