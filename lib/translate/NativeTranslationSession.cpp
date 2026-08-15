//===- NativeTranslationSession.cpp - Native translated execution -------===//

#include "neverd/translate/NativeTranslationSession.h"

#include "NativeTranslationSessionInternal.h"

#include "neverd/translate/GuestMemoryRuntime.h"
#include "neverd/translate/ResolvedHostTarget.h"
#include "neverd/translate/RuntimeABI.h"
#include "neverd/translate/RuntimeGuestState.h"
#include "neverd/translate/RuntimeHelpers.h"
#include "neverd/translate/RuntimeSymbolRegistry.h"
#include "neverd/translate/TranslationJITLinker.h"
#include "neverd/translate/TranslationObjectRequest.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/TargetParser/Triple.h"

#include <atomic>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace neverd::translate {

char NativeTranslationSessionError::ID;

NativeTranslationSessionError::NativeTranslationSessionError(
    NativeTranslationSessionErrorCode Code, std::string Detail)
    : Code(Code), Detail(std::move(Detail)) {}

void NativeTranslationSessionError::log(llvm::raw_ostream &OS) const {
  OS << "native translation session: ";
  switch (Code) {
  case NativeTranslationSessionErrorCode::InvalidRequest:
    OS << "invalid request";
    break;
  case NativeTranslationSessionErrorCode::UnsupportedProcessTarget:
    OS << "unsupported process target";
    break;
  case NativeTranslationSessionErrorCode::RuntimeCreationFailed:
    OS << "runtime creation failed";
    break;
  case NativeTranslationSessionErrorCode::TranslationFailed:
    OS << "translation failed";
    break;
  case NativeTranslationSessionErrorCode::RuntimeRegistryFailed:
    OS << "runtime registry creation failed";
    break;
  case NativeTranslationSessionErrorCode::LinkFailed:
    OS << "sealed linking failed";
    break;
  case NativeTranslationSessionErrorCode::StaleCode:
    OS << "translated code is stale";
    break;
  case NativeTranslationSessionErrorCode::RuntimeStateRejected:
    OS << "runtime state rejected";
    break;
  case NativeTranslationSessionErrorCode::RuntimeFrameRejected:
    OS << "runtime frame rejected";
    break;
  case NativeTranslationSessionErrorCode::InvocationFailed:
    OS << "native invocation failed";
    break;
  case NativeTranslationSessionErrorCode::RuntimeExitRejected:
    OS << "runtime exit rejected";
    break;
  case NativeTranslationSessionErrorCode::StateCommitFailed:
    OS << "state commit failed";
    break;
  case NativeTranslationSessionErrorCode::UnloadFailed:
    OS << "native allocation unload failed";
    break;
  case NativeTranslationSessionErrorCode::AlreadyRunning:
    OS << "session is already running";
    break;
  case NativeTranslationSessionErrorCode::RestoreWhileRunning:
    OS << "state restore attempted while running";
    break;
  case NativeTranslationSessionErrorCode::IdentityExhausted:
    OS << "runtime identity space exhausted";
    break;
  case NativeTranslationSessionErrorCode::RunCounterOverflow:
    OS << "run work counter overflow";
    break;
  }
  if (!Detail.empty())
    OS << " (" << Detail << ')';
}

std::error_code NativeTranslationSessionError::convertToErrorCode() const {
  return llvm::inconvertibleErrorCode();
}

namespace detail {

namespace {

void clearNativeTranslationRunControlV1(
    NativeTranslationRunControlV1 &Control) {
  Control.ActiveRuntime.reset();
  Control.CancellationPending = false;
  Control.Running = false;
}

} // namespace

void abandonNativeTranslationRunV1(NativeTranslationRunControlV1 &Control) {
  std::lock_guard Lock(Control.Mutex);
  clearNativeTranslationRunControlV1(Control);
}

llvm::Error finalizeNativeTranslationRunV1(
    NativeTranslationRunControlV1 &Control,
    llvm::function_ref<llvm::Error(bool CancellationWins)> Commit) {
  std::lock_guard Lock(Control.Mutex);
  const bool CancellationWins =
      Control.CancellationPending ||
      (Control.ActiveRuntime && Control.ActiveRuntime->cancellationRequested());
  if (CancellationWins && Control.ActiveRuntime)
    Control.ActiveRuntime->requestCancellation();

  llvm::Error Error = Commit(CancellationWins);
  clearNativeTranslationRunControlV1(Control);
  return Error;
}

void requestNativeTranslationCancellationV1(
    NativeTranslationRunControlV1 &Control) {
  std::shared_ptr<GuestMemoryRuntime> Active;
  {
    std::lock_guard Lock(Control.Mutex);
    if (!Control.Running)
      return;
    Control.CancellationPending = true;
    Active = Control.ActiveRuntime;
  }
  if (Active)
    Active->requestCancellation();
}

NativeTranslationObjectFailureV1
classifyNativeTranslationObjectFailureV1(llvm::Error Error) {
  NativeTranslationObjectFailureV1 Result;
  unsigned TypedErrorCount = 0;

  llvm::Error Unhandled = llvm::handleErrors(
      std::move(Error), [&](const TranslationObjectRequestError &Failure) {
        ++TypedErrorCount;
        if (!Result.Detail.empty())
          Result.Detail += "; ";
        std::string Message;
        {
          llvm::raw_string_ostream Stream(Message);
          Failure.log(Stream);
        }
        Result.Detail += Message;

        if (TypedErrorCount != 1)
          return;
        Result.SoleCode = Failure.code();
        Result.BuilderCode = Failure.builderCode();
        Result.BuilderGuestPC = Failure.builderGuestPC();
        Result.BuilderMemoryFaultDetails = Failure.builderMemoryFaultDetails();
        Result.LoweringCode = Failure.loweringCode();
        Result.LoweringGuestPC = Failure.loweringGuestPC();
        Result.CompilerBudgetObserved = Failure.compilerBudgetObserved();
        Result.CompilerBudgetLimit = Failure.compilerBudgetLimit();
        Result.GuestInstructionCount = Failure.guestInstructionCount();
      });

  const bool HasUnhandledError = static_cast<bool>(Unhandled);
  if (HasUnhandledError) {
    if (!Result.Detail.empty())
      Result.Detail += "; ";
    Result.Detail += llvm::toString(std::move(Unhandled));
  }
  if (TypedErrorCount != 1 || HasUnhandledError) {
    Result.SoleCode.reset();
    Result.BuilderCode.reset();
    Result.BuilderGuestPC.reset();
    Result.BuilderMemoryFaultDetails.reset();
    Result.LoweringCode.reset();
    Result.LoweringGuestPC.reset();
    Result.CompilerBudgetObserved.reset();
    Result.CompilerBudgetLimit.reset();
    Result.GuestInstructionCount.reset();
  }
  return Result;
}

std::optional<NativeTranslationUnsupportedInstructionV1>
classifyNativeTranslationUnsupportedInstructionV1(
    const NativeTranslationObjectFailureV1 &Failure) {
  if (Failure.Detail.empty())
    return std::nullopt;

  if (Failure.SoleCode ==
          TranslationObjectRequestErrorCode::BlockConstructionFailed &&
      Failure.BuilderCode && Failure.BuilderGuestPC &&
      !Failure.BuilderMemoryFaultDetails && !Failure.LoweringCode &&
      !Failure.LoweringGuestPC &&
      (*Failure.BuilderCode ==
           X86TranslationBlockBuilderErrorCode::UndecodableInstruction ||
       *Failure.BuilderCode ==
           X86TranslationBlockBuilderErrorCode::UnliftedInstruction))
    return NativeTranslationUnsupportedInstructionV1{
        *Failure.BuilderGuestPC, static_cast<uint64_t>(*Failure.BuilderCode),
        /*Subcode=*/0};

  if (Failure.SoleCode ==
          TranslationObjectRequestErrorCode::BlockLoweringFailed &&
      !Failure.BuilderCode && !Failure.BuilderGuestPC &&
      !Failure.BuilderMemoryFaultDetails &&
      Failure.LoweringCode ==
          TranslationBlockLoweringErrorCode::UnsupportedBlockShape &&
      Failure.LoweringGuestPC)
    return NativeTranslationUnsupportedInstructionV1{
        *Failure.LoweringGuestPC,
        static_cast<uint64_t>(
            TranslationObjectRequestErrorCode::BlockLoweringFailed),
        static_cast<uint64_t>(*Failure.LoweringCode)};

  return std::nullopt;
}

} // namespace detail

namespace {

std::atomic<uint64_t> NextSessionID{1};

llvm::Error failure(NativeTranslationSessionErrorCode Code,
                    llvm::StringRef Detail = {}) {
  return llvm::make_error<NativeTranslationSessionError>(Code, Detail.str());
}

llvm::Error failure(NativeTranslationSessionErrorCode Code, llvm::Error Cause) {
  return failure(Code, llvm::toString(std::move(Cause)));
}

llvm::Expected<uint64_t> allocateSessionID() {
  uint64_t Current = NextSessionID.load(std::memory_order_relaxed);
  for (;;) {
    if (Current == 0 || Current == std::numeric_limits<uint64_t>::max())
      return failure(NativeTranslationSessionErrorCode::IdentityExhausted);
    if (NextSessionID.compare_exchange_weak(Current, Current + 1,
                                            std::memory_order_relaxed,
                                            std::memory_order_relaxed))
      return Current;
  }
}

llvm::Expected<MemoryAccessKind>
translationAccess(RuntimeMemoryAccessKindV1 Access) {
  switch (Access) {
  case RuntimeMemoryAccessKindV1::Read:
    return MemoryAccessKind::Read;
  case RuntimeMemoryAccessKindV1::Write:
    return MemoryAccessKind::Write;
  case RuntimeMemoryAccessKindV1::Execute:
    return MemoryAccessKind::Execute;
  }
  return failure(NativeTranslationSessionErrorCode::RuntimeExitRejected,
                 "runtime memory exit has an unknown access kind");
}

llvm::Expected<TranslationExit>
translateRuntimeExit(const RuntimeControlBlockV1 &Control, uint64_t CurrentPC) {
  if (llvm::Error Error = validateRuntimeControlBlockV1(Control))
    return failure(NativeTranslationSessionErrorCode::RuntimeExitRejected,
                   std::move(Error));

  TranslationExit Exit;
  Exit.PC = CurrentPC;
  Exit.NextPC = CurrentPC;
  switch (Control.Exit.Kind) {
  case RuntimeABIExitKindV1::None:
    return failure(NativeTranslationSessionErrorCode::RuntimeExitRejected,
                   "empty runtime exit reached the host dispatcher");

  case RuntimeABIExitKindV1::Cancelled:
    Exit.Reason = TranslationStopReason::Cancelled;
    return Exit;

  case RuntimeABIExitKindV1::BudgetExhausted:
    Exit.Reason = TranslationStopReason::BudgetExhausted;
    Exit.Budget =
        BudgetExit{static_cast<TranslationBudgetKind>(Control.Exit.Size),
                   Control.Exit.Detail0, Control.Exit.Detail1};
    return Exit;

  case RuntimeABIExitKindV1::SelfModification:
    Exit.Reason = TranslationStopReason::SelfModification;
    Exit.SelfModification =
        SelfModificationExit{Control.Exit.Address, Control.Exit.Size,
                             Control.Exit.Detail0, Control.Exit.Detail1};
    return Exit;

  case RuntimeABIExitKindV1::MemoryFault: {
    switch (Control.Exit.Fault) {
    case RuntimeMemoryFaultKindV1::InvalidRuntimeFrame:
      return failure(NativeTranslationSessionErrorCode::RuntimeFrameRejected,
                     "runtime helper rejected its host-owned call frame");
    case RuntimeMemoryFaultKindV1::None:
    case RuntimeMemoryFaultKindV1::InvalidAccessWidth:
    case RuntimeMemoryFaultKindV1::InvalidAlignment:
    case RuntimeMemoryFaultKindV1::PolicyViolation:
      return failure(NativeTranslationSessionErrorCode::RuntimeExitRejected,
                     "runtime helper reported an integrity-policy fault");
    case RuntimeMemoryFaultKindV1::Misaligned:
    case RuntimeMemoryFaultKindV1::AddressOverflow:
    case RuntimeMemoryFaultKindV1::Unmapped:
    case RuntimeMemoryFaultKindV1::CrossRegion:
    case RuntimeMemoryFaultKindV1::PermissionDenied:
    case RuntimeMemoryFaultKindV1::ExecutableWriteRejected:
    case RuntimeMemoryFaultKindV1::ExecutableGenerationOverflow:
    case RuntimeMemoryFaultKindV1::ExecutableGenerationMismatch:
      break;
    }
    llvm::Expected<RuntimeMemoryFaultDetailsV1> Details =
        unpackRuntimeMemoryFaultDetailsV1(
            Control.Exit.Fault, {Control.Exit.Detail0, Control.Exit.Detail1});
    if (!Details)
      return failure(NativeTranslationSessionErrorCode::RuntimeExitRejected,
                     Details.takeError());
    llvm::Expected<MemoryAccessKind> Access =
        translationAccess(Details->Access);
    if (!Access)
      return Access.takeError();
    if (Control.Exit.Size == 0 ||
        Control.Exit.Size > std::numeric_limits<uint32_t>::max() / 8)
      return failure(NativeTranslationSessionErrorCode::RuntimeExitRejected,
                     "runtime memory exit width cannot be represented");
    if (Details->RequiredAlignment != 0 &&
        (Details->RequiredAlignment & (Details->RequiredAlignment - 1)) != 0)
      return failure(NativeTranslationSessionErrorCode::RuntimeExitRejected,
                     "runtime memory exit alignment cannot be represented");

    Exit.Reason = TranslationStopReason::MemoryFault;
    Exit.MemoryFault =
        MemoryFaultExit{Control.Exit.Address, *Access,
                        static_cast<uint32_t>(Control.Exit.Size * 8),
                        Details->RequiredAlignment};
    return Exit;
  }
  }
  return failure(NativeTranslationSessionErrorCode::RuntimeExitRejected,
                 "runtime exit has an unknown kind");
}

llvm::Expected<uint64_t>
validateGenerationBindings(GuestMemoryRuntime &Memory,
                           const TranslationBlockDescriptorV1 &Descriptor) {
  if (Descriptor.GenerationBindings.empty())
    return failure(NativeTranslationSessionErrorCode::StaleCode,
                   "translated block has no executable generation binding");
  for (const GuestExecutableRangeBinding &Binding :
       Descriptor.GenerationBindings) {
    const std::optional<uint64_t> Current =
        Memory.generationForAddress(Binding.Address);
    if (!Current || *Current != Binding.Generation)
      return failure(NativeTranslationSessionErrorCode::StaleCode,
                     llvm::Twine("generation mismatch at guest address ")
                         .concat(llvm::Twine::utohexstr(Binding.Address))
                         .str());
    if (Memory.codeInvalidationPolicy() ==
        CodeInvalidationPolicy::ValidateBeforeDispatch) {
      const GuestMemoryAccessResult Validation =
          Memory.validateExecutableGeneration(Binding.Address,
                                              Binding.Generation);
      if (Validation.Status != GuestMemoryAccessStatus::Completed)
        return failure(NativeTranslationSessionErrorCode::StaleCode,
                       "dispatch-time executable generation was rejected");
    }
  }

  const GuestExecutableRangeBinding &Entry =
      Descriptor.GenerationBindings.front();
  if (Entry.Address != Descriptor.Header.EntryPC)
    return failure(NativeTranslationSessionErrorCode::StaleCode,
                   "entry generation binding does not start at the block PC");
  if (Memory.codeInvalidationPolicy() ==
      CodeInvalidationPolicy::ValidateBeforeDispatch) {
    const GuestMemoryAccessResult Validation =
        Memory.validateExecutableGeneration(Entry.Address, Entry.Generation);
    if (Validation.Status != GuestMemoryAccessStatus::Completed)
      return failure(NativeTranslationSessionErrorCode::StaleCode,
                     "entry executable generation was rejected");
    return Entry.Generation;
  }
  return 0;
}

llvm::Expected<BlockExitV1>
translateBlockExit(uint32_t Status,
                   const TranslationBlockDescriptorV1 &Descriptor,
                   const RuntimeGuestStateX86_64V1 &RuntimeState) {
  if (Descriptor.InstructionBoundaries.empty())
    return failure(NativeTranslationSessionErrorCode::RuntimeExitRejected,
                   "translated block manifest has no instruction boundary");

  BlockExitV1 Exit;
  Exit.PC = Descriptor.InstructionBoundaries.back().Address;
  switch (static_cast<BlockExitKindV1>(Status)) {
  case BlockExitKindV1::DirectBranch: {
    if (!hasTranslationBlockDescriptorFlag(
            Descriptor.Header.Flags,
            TranslationBlockDescriptorFlagV1::HasStaticTarget))
      return failure(NativeTranslationSessionErrorCode::RuntimeExitRejected,
                     "translated direct branch has no manifest target");
    if (Descriptor.Header.Terminator ==
        TranslationBlockTerminatorKindV1::DirectBranch) {
      if (RuntimeState.RIP != Descriptor.Header.StaticTargetPC)
        return failure(
            NativeTranslationSessionErrorCode::RuntimeExitRejected,
            "translated direct branch did not commit its static target");
    } else if (Descriptor.Header.Terminator ==
               TranslationBlockTerminatorKindV1::ConditionalBranch) {
      if (RuntimeState.RIP != Descriptor.Header.StaticTargetPC &&
          RuntimeState.RIP != Descriptor.Header.FallthroughPC)
        return failure(
            NativeTranslationSessionErrorCode::RuntimeExitRejected,
            "translated branch did not commit a conditional successor");
    } else {
      return failure(NativeTranslationSessionErrorCode::RuntimeExitRejected,
                     "translated direct branch disagrees with its manifest");
    }
    Exit.Kind = BlockExitKindV1::DirectBranch;
    Exit.TargetPC = RuntimeState.RIP;
    break;
  }

  case BlockExitKindV1::Return:
    if (Descriptor.Header.Terminator !=
        TranslationBlockTerminatorKindV1::Return)
      return failure(NativeTranslationSessionErrorCode::RuntimeExitRejected,
                     "translated return disagrees with its manifest");
    Exit.Kind = BlockExitKindV1::Return;
    Exit.TargetPC = RuntimeState.RIP;
    break;

  default:
    return failure(NativeTranslationSessionErrorCode::RuntimeExitRejected,
                   "translated block returned a reason outside its manifest");
  }
  if (llvm::Error Error = validateBlockExitV1(Exit))
    return failure(NativeTranslationSessionErrorCode::RuntimeExitRejected,
                   std::move(Error));
  return Exit;
}

bool isGuestInstructionFetchStop(
    const detail::NativeTranslationObjectFailureV1 &Failure) {
  if (Failure.SoleCode !=
          TranslationObjectRequestErrorCode::BlockConstructionFailed ||
      !Failure.BuilderCode || !Failure.BuilderGuestPC ||
      !Failure.BuilderMemoryFaultDetails)
    return false;

  switch (*Failure.BuilderCode) {
  case X86TranslationBlockBuilderErrorCode::InstructionFetchFailed:
  case X86TranslationBlockBuilderErrorCode::TruncatedInstruction:
  case X86TranslationBlockBuilderErrorCode::GuestAddressOverflow:
    break;
  default:
    return false;
  }

  const GuestMemoryFault &Fault = *Failure.BuilderMemoryFaultDetails;
  return Fault.Kind != RuntimeMemoryFaultKindV1::None &&
         Fault.Exit.Access == MemoryAccessKind::Execute &&
         Fault.AccessSize != 0 && Fault.Exit.AccessWidthBits != 0 &&
         (Fault.Exit.AccessWidthBits % 8) == 0 &&
         Fault.AccessSize == Fault.Exit.AccessWidthBits / 8;
}

TranslationResult makeResult(const TranslationOptions &Options,
                             uint64_t StartPC, uint64_t Instructions,
                             uint64_t Blocks, uint64_t GeneratedCodeBytes,
                             TranslationExit Exit) {
  TranslationResult Result;
  Result.Guest = Options.Guest;
  Result.Host = GuestArchitecture::AArch64;
  Result.StartPC = StartPC;
  Result.GuestInstructions = Instructions;
  Result.BlocksTranslated = Blocks;
  Result.GeneratedCodeBytes = GeneratedCodeBytes;
  Result.Exit = std::move(Exit);
  return Result;
}

struct NativeTranslationRunProgressV1 {
  uint64_t GuestInstructions = 0;
  uint64_t BlocksTranslated = 0;
  uint64_t GeneratedCodeBytes = 0;
};

llvm::Expected<uint64_t> addRunCounter(uint64_t Current, uint64_t Delta,
                                       llvm::StringRef Name) {
  if (Delta > std::numeric_limits<uint64_t>::max() - Current)
    return failure(NativeTranslationSessionErrorCode::RunCounterOverflow,
                   llvm::Twine(Name).concat(" counter overflow").str());
  return Current + Delta;
}

llvm::Expected<NativeTranslationRunProgressV1>
advanceRunProgress(const NativeTranslationRunProgressV1 &Progress,
                   uint64_t Instructions, uint64_t Blocks,
                   uint64_t GeneratedCodeBytes) {
  llvm::Expected<uint64_t> InstructionTotal = addRunCounter(
      Progress.GuestInstructions, Instructions, "guest instruction");
  if (!InstructionTotal)
    return InstructionTotal.takeError();
  llvm::Expected<uint64_t> BlockTotal =
      addRunCounter(Progress.BlocksTranslated, Blocks, "translated block");
  if (!BlockTotal)
    return BlockTotal.takeError();
  llvm::Expected<uint64_t> GeneratedTotal = addRunCounter(
      Progress.GeneratedCodeBytes, GeneratedCodeBytes, "generated code byte");
  if (!GeneratedTotal)
    return GeneratedTotal.takeError();
  return NativeTranslationRunProgressV1{*InstructionTotal, *BlockTotal,
                                        *GeneratedTotal};
}

llvm::Expected<TranslationResult>
cancelledResult(const TranslationOptions &Options, uint64_t StartPC,
                uint64_t CurrentPC, GuestMemoryRuntime &Memory,
                uint64_t GuestInstructions, uint64_t BlocksTranslated,
                uint64_t GeneratedCodeBytes) {
  const RuntimeControlBlockV1 Control =
      Memory.snapshotControlBlock(CurrentPC, 0);
  llvm::Expected<TranslationExit> Exit =
      translateRuntimeExit(Control, CurrentPC);
  if (!Exit)
    return Exit.takeError();
  TranslationResult Result =
      makeResult(Options, StartPC, GuestInstructions, BlocksTranslated,
                 GeneratedCodeBytes, std::move(*Exit));
  if (llvm::Error Error = validateTranslationResult(Result, Options))
    return failure(NativeTranslationSessionErrorCode::RuntimeExitRejected,
                   std::move(Error));
  return Result;
}

} // namespace

llvm::Expected<BlockExitV1> detail::translateNativeBlockExitV1(
    uint32_t Status, const TranslationBlockDescriptorV1 &Descriptor,
    const RuntimeGuestStateX86_64V1 &RuntimeState) {
  return translateBlockExit(Status, Descriptor, RuntimeState);
}

llvm::Expected<TranslationResult>
detail::makeNativeTranslationCancelledResultV1(
    const TranslationOptions &Options, uint64_t StartPC, uint64_t CurrentPC,
    GuestMemoryRuntime &Memory, uint64_t GuestInstructions,
    uint64_t BlocksTranslated, uint64_t GeneratedCodeBytes) {
  return cancelledResult(Options, StartPC, CurrentPC, Memory, GuestInstructions,
                         BlocksTranslated, GeneratedCodeBytes);
}

struct NativeTranslationSessionV1::Impl {
  TranslationOptions Options;
  GuestState LogicalState;
  TranslationSemanticPolicyV1 Semantic;
  uint64_t SessionID = 0;
  uint64_t NextBlockID = 1;
  uint64_t CacheGeneration = 1;
  uint64_t CodeEpoch = 1;

  detail::NativeTranslationRunControlV1 RunControl;
};

NativeTranslationSessionV1::NativeTranslationSessionV1(
    std::unique_ptr<Impl> State)
    : State(std::move(State)) {}

NativeTranslationSessionV1::~NativeTranslationSessionV1() = default;

llvm::Expected<std::unique_ptr<NativeTranslationSessionV1>>
NativeTranslationSessionV1::create(TranslationOptions Options, GuestState State,
                                   TranslationSemanticPolicyV1 Semantic) {
  if (Options.Mode != TranslationMode::JIT ||
      Options.Target.Kind != HostTargetKind::Native ||
      Options.Guest != GuestArchitecture::X86_64 ||
      Options.UnsupportedInstructions != UnsupportedInstructionPolicy::Fail ||
      Options.Optimization !=
          TranslationOptimizationPolicy::ProvenSemanticAndLLVM ||
      Options.BlockCache != BlockCachePolicy::Disabled ||
      Options.DeterministicReplay != DeterministicReplayPolicy::Disabled)
    return failure(
        NativeTranslationSessionErrorCode::InvalidRequest,
        "v1 requires native JIT, x86-64, fail-closed scalar lowering, the "
        "composed optimizer, disabled cache, and disabled replay");
  if (llvm::Error Error = validateTranslationRequest(Options, State))
    return failure(NativeTranslationSessionErrorCode::InvalidRequest,
                   std::move(Error));

  llvm::Expected<ResolvedHostTarget> Host = resolveHostTarget(Options);
  if (!Host)
    return failure(NativeTranslationSessionErrorCode::UnsupportedProcessTarget,
                   Host.takeError());
  const llvm::Triple Triple(Host->triple());
  if (Host->architecture() != GuestArchitecture::AArch64 ||
      Triple.getArchName() != "aarch64" ||
      (!Triple.isOSBinFormatELF() && !Triple.isOSBinFormatMachO()))
    return failure(NativeTranslationSessionErrorCode::UnsupportedProcessTarget,
                   Host->triple());
  llvm::Expected<RuntimeGuestStateX86_64V1> RuntimeState =
      createRuntimeGuestStateX86_64V1(State);
  if (!RuntimeState)
    return failure(NativeTranslationSessionErrorCode::RuntimeStateRejected,
                   RuntimeState.takeError());

  llvm::Expected<uint64_t> SessionID = allocateSessionID();
  if (!SessionID)
    return SessionID.takeError();
  auto Pimpl = std::make_unique<Impl>();
  Pimpl->Options = std::move(Options);
  Pimpl->LogicalState = std::move(State);
  Pimpl->Semantic = std::move(Semantic);
  Pimpl->SessionID = *SessionID;
  return std::unique_ptr<NativeTranslationSessionV1>(
      new NativeTranslationSessionV1(std::move(Pimpl)));
}

const TranslationOptions &NativeTranslationSessionV1::options() const {
  return State->Options;
}

const GuestState &NativeTranslationSessionV1::state() const {
  return State->LogicalState;
}

llvm::Error NativeTranslationSessionV1::restoreState(GuestState NewState) {
  if (llvm::Error Error = validateTranslationRequest(State->Options, NewState))
    return failure(NativeTranslationSessionErrorCode::StateCommitFailed,
                   std::move(Error));
  llvm::Expected<RuntimeGuestStateX86_64V1> RuntimeState =
      createRuntimeGuestStateX86_64V1(NewState);
  if (!RuntimeState)
    return failure(NativeTranslationSessionErrorCode::StateCommitFailed,
                   RuntimeState.takeError());

  std::lock_guard Lock(State->RunControl.Mutex);
  if (State->RunControl.Running)
    return failure(NativeTranslationSessionErrorCode::RestoreWhileRunning);
  if (State->CacheGeneration == std::numeric_limits<uint64_t>::max() ||
      State->CodeEpoch == std::numeric_limits<uint64_t>::max())
    return failure(NativeTranslationSessionErrorCode::IdentityExhausted);
  ++State->CacheGeneration;
  ++State->CodeEpoch;
  std::swap(State->LogicalState, NewState);
  return llvm::Error::success();
}

llvm::Expected<TranslationResult> NativeTranslationSessionV1::run() {
  {
    std::lock_guard Lock(State->RunControl.Mutex);
    if (State->RunControl.Running)
      return failure(NativeTranslationSessionErrorCode::AlreadyRunning);
    State->RunControl.Running = true;
    State->RunControl.CancellationPending = false;
  }
  llvm::scope_exit FinishRun(
      [&] { detail::abandonNativeTranslationRunV1(State->RunControl); });

  const GuestRegisterValue *RIP = findRegisterValue(State->LogicalState, 16);
  if (!RIP)
    return failure(NativeTranslationSessionErrorCode::RuntimeStateRejected,
                   "logical x86-64 state is missing RIP");
  const uint64_t StartPC = RIP->Value.getZExtValue();
  uint64_t CurrentPC = StartPC;

  GuestMemoryRuntimeConfig RuntimeConfig;
  RuntimeConfig.CodeInvalidation = State->Options.CodeInvalidation;
  RuntimeConfig.InstructionBudget = State->Options.InstructionBudget;
  RuntimeConfig.BlockBudget = State->Options.BlockBudget;
  llvm::Expected<std::unique_ptr<GuestMemoryRuntime>> MemoryOrErr =
      GuestMemoryRuntime::create(State->LogicalState, RuntimeConfig);
  if (!MemoryOrErr)
    return failure(NativeTranslationSessionErrorCode::RuntimeCreationFailed,
                   MemoryOrErr.takeError());
  std::shared_ptr<GuestMemoryRuntime> Memory(std::move(*MemoryOrErr));
  {
    std::lock_guard Lock(State->RunControl.Mutex);
    State->RunControl.ActiveRuntime = Memory;
    if (State->RunControl.CancellationPending)
      Memory->requestCancellation();
  }

  llvm::Expected<RuntimeGuestStateX86_64V1> RuntimeStateOrErr =
      createRuntimeGuestStateX86_64V1(State->LogicalState);
  if (!RuntimeStateOrErr)
    return failure(NativeTranslationSessionErrorCode::RuntimeStateRejected,
                   RuntimeStateOrErr.takeError());
  RuntimeGuestStateX86_64V1 RuntimeState = *RuntimeStateOrErr;

  llvm::Expected<RuntimeSymbolRegistryV1> RegistryOrErr =
      RuntimeSymbolRegistryV1::create();
  if (!RegistryOrErr)
    return failure(NativeTranslationSessionErrorCode::RuntimeRegistryFailed,
                   RegistryOrErr.takeError());
  RuntimeSymbolRegistryV1 Registry = std::move(*RegistryOrErr);
  GuestState Candidate = State->LogicalState;
  NativeTranslationRunProgressV1 Progress;

  auto FinishSuccessfulRun =
      [&](llvm::Expected<TranslationResult> ResultOrErr,
          std::optional<GuestState> Candidate =
              std::nullopt) -> llvm::Expected<TranslationResult> {
    if (!ResultOrErr)
      return ResultOrErr.takeError();
    TranslationResult Result = std::move(*ResultOrErr);
    llvm::Error CommitError = detail::finalizeNativeTranslationRunV1(
        State->RunControl, [&](bool CancellationWins) -> llvm::Error {
          if (CancellationWins) {
            llvm::Expected<TranslationResult> Cancelled =
                detail::makeNativeTranslationCancelledResultV1(
                    State->Options, Result.StartPC, CurrentPC, *Memory,
                    Result.GuestInstructions, Result.BlocksTranslated,
                    Result.GeneratedCodeBytes);
            if (!Cancelled)
              return Cancelled.takeError();
            Result = std::move(*Cancelled);
          }
          if (Candidate)
            std::swap(State->LogicalState, *Candidate);
          return llvm::Error::success();
        });
    FinishRun.release();
    if (CommitError)
      return std::move(CommitError);
    return Result;
  };

  auto FinishAtProgress =
      [&](const NativeTranslationRunProgressV1 &FinalProgress,
          TranslationExit Exit) -> llvm::Expected<TranslationResult> {
    TranslationResult Result =
        makeResult(State->Options, StartPC, FinalProgress.GuestInstructions,
                   FinalProgress.BlocksTranslated,
                   FinalProgress.GeneratedCodeBytes, std::move(Exit));
    if (llvm::Error Error = validateTranslationResult(Result, State->Options))
      return failure(NativeTranslationSessionErrorCode::RuntimeExitRejected,
                     std::move(Error));
    return FinishSuccessfulRun(std::move(Result), std::move(Candidate));
  };

  auto FinishRuntimeStop =
      [&](const NativeTranslationRunProgressV1 &FinalProgress)
      -> llvm::Expected<TranslationResult> {
    return FinishSuccessfulRun(detail::makeNativeTranslationCancelledResultV1(
                                   State->Options, StartPC, CurrentPC, *Memory,
                                   FinalProgress.GuestInstructions,
                                   FinalProgress.BlocksTranslated,
                                   FinalProgress.GeneratedCodeBytes),
                               std::move(Candidate));
  };

  for (;;) {
    if (Memory->cancellationRequested())
      return FinishRuntimeStop(Progress);

    const auto StopAtExactBudget =
        [&](TranslationBudgetKind Kind,
            uint64_t Limit) -> llvm::Expected<TranslationResult> {
      TranslationExit Exit;
      Exit.Reason = TranslationStopReason::BudgetExhausted;
      Exit.PC = CurrentPC;
      Exit.NextPC = CurrentPC;
      Exit.Budget = BudgetExit{Kind, Limit, Limit};
      return FinishAtProgress(Progress, std::move(Exit));
    };
    if (State->Options.InstructionBudget != 0 &&
        Progress.GuestInstructions >= State->Options.InstructionBudget) {
      if (Progress.GuestInstructions != State->Options.InstructionBudget)
        return failure(NativeTranslationSessionErrorCode::TranslationFailed,
                       "guest-instruction progress exceeded its budget");
      return StopAtExactBudget(TranslationBudgetKind::GuestInstructions,
                               State->Options.InstructionBudget);
    }
    if (State->Options.BlockBudget != 0 &&
        Progress.BlocksTranslated >= State->Options.BlockBudget) {
      if (Progress.BlocksTranslated != State->Options.BlockBudget)
        return failure(NativeTranslationSessionErrorCode::TranslationFailed,
                       "translated-block progress exceeded its budget");
      return StopAtExactBudget(TranslationBudgetKind::Blocks,
                               State->Options.BlockBudget);
    }
    if (State->Options.GeneratedCodeByteBudget != 0 &&
        Progress.GeneratedCodeBytes >= State->Options.GeneratedCodeByteBudget) {
      if (Progress.GeneratedCodeBytes != State->Options.GeneratedCodeByteBudget)
        return failure(NativeTranslationSessionErrorCode::TranslationFailed,
                       "generated-code progress exceeded its budget");
      return StopAtExactBudget(TranslationBudgetKind::GeneratedCodeBytes,
                               State->Options.GeneratedCodeByteBudget);
    }

    TranslationOptions BlockOptions = State->Options;
    if (BlockOptions.InstructionBudget != 0)
      BlockOptions.InstructionBudget -= Progress.GuestInstructions;
    if (BlockOptions.BlockBudget != 0)
      BlockOptions.BlockBudget -= Progress.BlocksTranslated;
    if (BlockOptions.GeneratedCodeByteBudget != 0)
      BlockOptions.GeneratedCodeByteBudget -= Progress.GeneratedCodeBytes;

    llvm::Expected<TranslationObjectResultV1> ObjectOrErr =
        compileTranslationObjectRequestV1(
            {Candidate, CurrentPC, BlockOptions, State->Semantic});
    if (!ObjectOrErr) {
      detail::NativeTranslationObjectFailureV1 ObjectFailure =
          detail::classifyNativeTranslationObjectFailureV1(
              ObjectOrErr.takeError());
      if (Memory->cancellationRequested())
        return FinishRuntimeStop(Progress);
      if (isGuestInstructionFetchStop(ObjectFailure)) {
        TranslationExit Exit;
        Exit.Reason = TranslationStopReason::MemoryFault;
        Exit.PC = *ObjectFailure.BuilderGuestPC;
        Exit.NextPC = CurrentPC;
        Exit.MemoryFault = ObjectFailure.BuilderMemoryFaultDetails->Exit;
        Exit.Diagnostic = ObjectFailure.Detail;
        return FinishAtProgress(Progress, std::move(Exit));
      }
      if (std::optional<detail::NativeTranslationUnsupportedInstructionV1>
              Unsupported =
                  detail::classifyNativeTranslationUnsupportedInstructionV1(
                      ObjectFailure)) {
        const bool Restartable = Unsupported->GuestPC == CurrentPC;
        TranslationExit Exit;
        Exit.Reason = TranslationStopReason::UnsupportedInstruction;
        Exit.PC = Unsupported->GuestPC;
        Exit.NextPC = CurrentPC;
        Exit.Trap = TrapExit{TranslationTrapKind::UnsupportedInstruction,
                             Unsupported->Code, Unsupported->Subcode,
                             Unsupported->GuestPC, Restartable};
        Exit.FallbackRequested = false;
        Exit.Diagnostic = ObjectFailure.Detail;
        return FinishAtProgress(Progress, std::move(Exit));
      }
      if (ObjectFailure.SoleCode ==
              TranslationObjectRequestErrorCode::InstructionBudgetExceeded &&
          ObjectFailure.BuilderCode ==
              X86TranslationBlockBuilderErrorCode::InstructionBudgetExceeded &&
          ObjectFailure.BuilderGuestPC &&
          State->Options.InstructionBudget != 0 &&
          BlockOptions.InstructionBudget != 0) {
        llvm::Expected<NativeTranslationRunProgressV1> Exhausted =
            advanceRunProgress(Progress, BlockOptions.InstructionBudget,
                               /*Blocks=*/0, /*GeneratedCodeBytes=*/0);
        if (!Exhausted)
          return Exhausted.takeError();
        if (Exhausted->GuestInstructions != State->Options.InstructionBudget)
          return failure(
              NativeTranslationSessionErrorCode::TranslationFailed,
              "block instruction-budget telemetry is not globally exact");
        TranslationExit Exit;
        Exit.Reason = TranslationStopReason::BudgetExhausted;
        Exit.PC = *ObjectFailure.BuilderGuestPC;
        Exit.NextPC = *ObjectFailure.BuilderGuestPC;
        Exit.Budget = BudgetExit{TranslationBudgetKind::GuestInstructions,
                                 State->Options.InstructionBudget,
                                 Exhausted->GuestInstructions};
        return FinishAtProgress(*Exhausted, std::move(Exit));
      }
      if (ObjectFailure.SoleCode ==
              TranslationObjectRequestErrorCode::GeneratedCodeBudgetExceeded &&
          ObjectFailure.CompilerBudgetObserved &&
          ObjectFailure.CompilerBudgetLimit &&
          ObjectFailure.GuestInstructionCount &&
          State->Options.GeneratedCodeByteBudget != 0 &&
          BlockOptions.GeneratedCodeByteBudget != 0 &&
          *ObjectFailure.CompilerBudgetLimit != 0 &&
          *ObjectFailure.CompilerBudgetLimit ==
              BlockOptions.GeneratedCodeByteBudget &&
          *ObjectFailure.CompilerBudgetObserved >
              *ObjectFailure.CompilerBudgetLimit) {
        llvm::Expected<NativeTranslationRunProgressV1> Exhausted =
            advanceRunProgress(Progress, *ObjectFailure.GuestInstructionCount,
                               /*Blocks=*/1,
                               *ObjectFailure.CompilerBudgetObserved);
        if (!Exhausted)
          return Exhausted.takeError();
        if (Exhausted->GeneratedCodeBytes <=
            State->Options.GeneratedCodeByteBudget)
          return failure(
              NativeTranslationSessionErrorCode::TranslationFailed,
              "block generated-code telemetry did not exceed the global "
              "budget");
        TranslationExit Exit;
        Exit.Reason = TranslationStopReason::BudgetExhausted;
        Exit.PC = CurrentPC;
        Exit.NextPC = CurrentPC;
        Exit.Budget = BudgetExit{TranslationBudgetKind::GeneratedCodeBytes,
                                 State->Options.GeneratedCodeByteBudget,
                                 Exhausted->GeneratedCodeBytes};
        return FinishAtProgress(*Exhausted, std::move(Exit));
      }
      return failure(NativeTranslationSessionErrorCode::TranslationFailed,
                     ObjectFailure.Detail);
    }
    TranslationObjectResultV1 Object = std::move(*ObjectOrErr);
    const uint64_t ObjectCodeBytes =
        static_cast<uint64_t>(Object.artifact().bytes().size());
    llvm::Expected<NativeTranslationRunProgressV1> ProspectiveOrErr =
        advanceRunProgress(Progress,
                           Object.descriptor().Header.GuestInstructionCount,
                           /*Blocks=*/1, ObjectCodeBytes);
    if (!ProspectiveOrErr)
      return ProspectiveOrErr.takeError();
    const NativeTranslationRunProgressV1 Prospective = *ProspectiveOrErr;
    if ((State->Options.InstructionBudget != 0 &&
         Prospective.GuestInstructions > State->Options.InstructionBudget) ||
        (State->Options.BlockBudget != 0 &&
         Prospective.BlocksTranslated > State->Options.BlockBudget) ||
        (State->Options.GeneratedCodeByteBudget != 0 &&
         Prospective.GeneratedCodeBytes >
             State->Options.GeneratedCodeByteBudget))
      return failure(NativeTranslationSessionErrorCode::TranslationFailed,
                     "block compiler exceeded its remaining run budget");

    if (Memory->cancellationRequested())
      return FinishRuntimeStop(Prospective);

    if (State->NextBlockID == std::numeric_limits<uint64_t>::max())
      return failure(NativeTranslationSessionErrorCode::IdentityExhausted);
    const RuntimeCodeCredentialV1 Credential{
        State->SessionID, State->NextBlockID++, CurrentPC,
        State->CacheGeneration, State->CodeEpoch};

    llvm::Expected<LinkedTranslationBlockV1> LinkedOrErr =
        linkTranslationObjectV1(Object, Registry, Credential);
    if (!LinkedOrErr)
      return failure(NativeTranslationSessionErrorCode::LinkFailed,
                     LinkedOrErr.takeError());
    LinkedTranslationBlockV1 Linked = std::move(*LinkedOrErr);

    auto Unload = [&]() -> llvm::Error {
      if (llvm::Error Error = Linked.unload())
        return failure(NativeTranslationSessionErrorCode::UnloadFailed,
                       std::move(Error));
      return llvm::Error::success();
    };
    auto FailAfterLink = [&](llvm::Error Primary) -> llvm::Error {
      return llvm::joinErrors(std::move(Primary), Unload());
    };

    if (Memory->cancellationRequested()) {
      llvm::Expected<TranslationResult> Result =
          detail::makeNativeTranslationCancelledResultV1(
              State->Options, StartPC, CurrentPC, *Memory,
              Prospective.GuestInstructions, Prospective.BlocksTranslated,
              Prospective.GeneratedCodeBytes);
      if (!Result)
        return FailAfterLink(Result.takeError());
      if (llvm::Error Error = Unload())
        return std::move(Error);
      return FinishSuccessfulRun(std::move(Result), std::move(Candidate));
    }

    llvm::Expected<uint64_t> ValidatedEntryGeneration =
        validateGenerationBindings(*Memory, Object.descriptor());
    if (!ValidatedEntryGeneration)
      return FailAfterLink(ValidatedEntryGeneration.takeError());

    llvm::Expected<RuntimeCallFrameV1> FrameOrErr = createRuntimeCallFrameV1(
        *Memory, Credential, Credential,
        State->Options.CodeInvalidation ==
                CodeInvalidationPolicy::ValidateBeforeDispatch
            ? CurrentPC
            : 0,
        *ValidatedEntryGeneration);
    if (!FrameOrErr)
      return FailAfterLink(
          failure(NativeTranslationSessionErrorCode::RuntimeFrameRejected,
                  FrameOrErr.takeError()));
    RuntimeCallFrameV1 Frame = *FrameOrErr;

    const RuntimePollResult Before = Memory->poll();
    if (Before.Status != RuntimePollStatus::Continue) {
      llvm::Expected<TranslationResult> Result =
          detail::makeNativeTranslationCancelledResultV1(
              State->Options, StartPC, CurrentPC, *Memory,
              Prospective.GuestInstructions, Prospective.BlocksTranslated,
              Prospective.GeneratedCodeBytes);
      if (!Result)
        return FailAfterLink(Result.takeError());
      if (llvm::Error Error = Unload())
        return std::move(Error);
      return FinishSuccessfulRun(std::move(Result), std::move(Candidate));
    }

    llvm::Expected<uint32_t> StatusOrErr = Linked.invoke(RuntimeState, Frame);
    if (!StatusOrErr)
      return FailAfterLink(
          failure(NativeTranslationSessionErrorCode::InvocationFailed,
                  StatusOrErr.takeError()));
    const uint32_t Status = *StatusOrErr;
    const bool IsBlockExit = Status >= kBlockExitKindBaseV1;
    std::optional<TranslationExit> ValidatedRuntimeExit;
    std::optional<BlockExitV1> ValidatedBlockExit;
    if (IsBlockExit) {
      llvm::Expected<BlockExitV1> BlockExit =
          detail::translateNativeBlockExitV1(Status, Object.descriptor(),
                                             RuntimeState);
      if (!BlockExit)
        return FailAfterLink(BlockExit.takeError());
      ValidatedBlockExit = *BlockExit;
    } else {
      if (Status < static_cast<uint32_t>(RuntimeABIExitKindV1::MemoryFault) ||
          Status > static_cast<uint32_t>(RuntimeABIExitKindV1::Cancelled) ||
          Status != static_cast<uint32_t>(Frame.Control.Exit.Kind))
        return FailAfterLink(
            failure(NativeTranslationSessionErrorCode::RuntimeExitRejected,
                    "runtime status disagrees with the sealed control block"));
      llvm::Expected<TranslationExit> RuntimeExit =
          translateRuntimeExit(Frame.Control, RuntimeState.RIP);
      if (!RuntimeExit)
        return FailAfterLink(RuntimeExit.takeError());
      ValidatedRuntimeExit = std::move(*RuntimeExit);
    }

    const uint64_t CompletedInstructions =
        IsBlockExit ? Object.descriptor().Header.GuestInstructionCount
                    : Object.descriptor().Header.GuestInstructionCount - 1;
    const RuntimePollResult After =
        Memory->poll(CompletedInstructions, IsBlockExit ? 1 : 0);
    CurrentPC = RuntimeState.RIP;

    std::optional<TranslationExit> TerminalExit;
    if (Memory->cancellationRequested()) {
      llvm::Expected<TranslationExit> ExitOrErr = translateRuntimeExit(
          Memory->snapshotControlBlock(CurrentPC, 0), CurrentPC);
      if (!ExitOrErr)
        return FailAfterLink(ExitOrErr.takeError());
      TerminalExit = std::move(*ExitOrErr);
    } else if (!IsBlockExit) {
      TerminalExit = std::move(*ValidatedRuntimeExit);
    } else {
      llvm::Expected<uint64_t> Revalidated =
          validateGenerationBindings(*Memory, Object.descriptor());
      if (!Revalidated)
        return FailAfterLink(Revalidated.takeError());
      if (ValidatedBlockExit->Kind == BlockExitKindV1::Return) {
        TranslationExit Exit;
        Exit.Reason = TranslationStopReason::Returned;
        Exit.PC = ValidatedBlockExit->PC;
        Exit.NextPC = ValidatedBlockExit->TargetPC;
        TerminalExit = std::move(Exit);
      } else if (After.Status == RuntimePollStatus::BudgetExhausted) {
        if (!After.Budget)
          return FailAfterLink(
              failure(NativeTranslationSessionErrorCode::RuntimeExitRejected,
                      "runtime budget stop is missing its typed payload"));
        TranslationExit Exit;
        Exit.Reason = TranslationStopReason::BudgetExhausted;
        Exit.PC = CurrentPC;
        Exit.NextPC = CurrentPC;
        Exit.Budget = *After.Budget;
        TerminalExit = std::move(Exit);
      } else if (State->Options.GeneratedCodeByteBudget != 0 &&
                 Prospective.GeneratedCodeBytes ==
                     State->Options.GeneratedCodeByteBudget) {
        TranslationExit Exit;
        Exit.Reason = TranslationStopReason::BudgetExhausted;
        Exit.PC = CurrentPC;
        Exit.NextPC = CurrentPC;
        Exit.Budget = BudgetExit{TranslationBudgetKind::GeneratedCodeBytes,
                                 State->Options.GeneratedCodeByteBudget,
                                 Prospective.GeneratedCodeBytes};
        TerminalExit = std::move(Exit);
      }
    }

    if (llvm::Error Error =
            applyRuntimeGuestStateX86_64V1(RuntimeState, Candidate))
      return FailAfterLink(
          failure(NativeTranslationSessionErrorCode::StateCommitFailed,
                  std::move(Error)));
    Candidate.Memory = Memory->snapshotMemoryRegions();
    if (llvm::Error Error = validateGuestState(Candidate))
      return FailAfterLink(
          failure(NativeTranslationSessionErrorCode::StateCommitFailed,
                  std::move(Error)));

    if (llvm::Error Error = Unload())
      return std::move(Error);
    Progress = Prospective;
    if (!TerminalExit)
      continue;
    return FinishAtProgress(Progress, std::move(*TerminalExit));
  }
}

void NativeTranslationSessionV1::requestCancellation() {
  detail::requestNativeTranslationCancellationV1(State->RunControl);
}

} // namespace neverd::translate
