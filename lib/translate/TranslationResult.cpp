//===- TranslationResult.cpp - Observable translation stop state ---------===//

#include "neverd/translate/TranslationResult.h"

#include "neverd/translate/TranslationOptions.h"

#include "llvm/ADT/Twine.h"
#include "llvm/Support/Errc.h"

#include <limits>

namespace neverd::translate {
namespace {

llvm::Error invalid(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::errc::invalid_argument, "%s",
                                 Message.str().c_str());
}

bool isKnown(TranslationStopReason Reason) {
  switch (Reason) {
  case TranslationStopReason::NotStarted:
  case TranslationStopReason::Returned:
  case TranslationStopReason::Syscall:
  case TranslationStopReason::Exception:
  case TranslationStopReason::Signal:
  case TranslationStopReason::Breakpoint:
  case TranslationStopReason::UnsupportedInstruction:
  case TranslationStopReason::SelfModification:
  case TranslationStopReason::BudgetExhausted:
  case TranslationStopReason::Cancelled:
  case TranslationStopReason::InternalError:
  case TranslationStopReason::Dispatch:
  case TranslationStopReason::ExternalCall:
  case TranslationStopReason::MemoryFault:
  case TranslationStopReason::JITUnavailable:
    return true;
  }
  return false;
}

bool isKnown(MemoryAccessKind Access) {
  return Access == MemoryAccessKind::Read ||
         Access == MemoryAccessKind::Write ||
         Access == MemoryAccessKind::Execute ||
         Access == MemoryAccessKind::AtomicReadModifyWrite;
}

bool isKnown(TranslationTrapKind Kind) {
  switch (Kind) {
  case TranslationTrapKind::Breakpoint:
  case TranslationTrapKind::UnsupportedInstruction:
  case TranslationTrapKind::Exception:
  case TranslationTrapKind::Signal:
    return true;
  }
  return false;
}

bool isKnown(TranslationBudgetKind Kind) {
  return Kind == TranslationBudgetKind::GuestInstructions ||
         Kind == TranslationBudgetKind::Blocks ||
         Kind == TranslationBudgetKind::GeneratedCodeBytes;
}

std::optional<TranslationTrapKind>
requiredTrapKind(TranslationStopReason Reason) {
  switch (Reason) {
  case TranslationStopReason::Breakpoint:
    return TranslationTrapKind::Breakpoint;
  case TranslationStopReason::UnsupportedInstruction:
    return TranslationTrapKind::UnsupportedInstruction;
  case TranslationStopReason::Exception:
    return TranslationTrapKind::Exception;
  case TranslationStopReason::Signal:
    return TranslationTrapKind::Signal;
  default:
    return std::nullopt;
  }
}

} // namespace

const char *translationStopReasonName(TranslationStopReason Reason) {
  switch (Reason) {
  case TranslationStopReason::NotStarted:
    return "not-started";
  case TranslationStopReason::Returned:
    return "returned";
  case TranslationStopReason::Syscall:
    return "syscall";
  case TranslationStopReason::Exception:
    return "exception";
  case TranslationStopReason::Signal:
    return "signal";
  case TranslationStopReason::Breakpoint:
    return "breakpoint";
  case TranslationStopReason::UnsupportedInstruction:
    return "unsupported-instruction";
  case TranslationStopReason::SelfModification:
    return "self-modification";
  case TranslationStopReason::BudgetExhausted:
    return "budget-exhausted";
  case TranslationStopReason::Cancelled:
    return "cancelled";
  case TranslationStopReason::InternalError:
    return "internal-error";
  case TranslationStopReason::Dispatch:
    return "dispatch";
  case TranslationStopReason::ExternalCall:
    return "external-call";
  case TranslationStopReason::MemoryFault:
    return "memory-fault";
  case TranslationStopReason::JITUnavailable:
    return "jit-unavailable";
  }
  return "unknown";
}

bool isResumableTranslationStop(TranslationStopReason Reason) {
  switch (Reason) {
  case TranslationStopReason::Syscall:
  case TranslationStopReason::Exception:
  case TranslationStopReason::Signal:
  case TranslationStopReason::Breakpoint:
  case TranslationStopReason::UnsupportedInstruction:
  case TranslationStopReason::SelfModification:
  case TranslationStopReason::BudgetExhausted:
  case TranslationStopReason::Cancelled:
  case TranslationStopReason::Dispatch:
  case TranslationStopReason::ExternalCall:
  case TranslationStopReason::MemoryFault:
    return true;
  case TranslationStopReason::NotStarted:
  case TranslationStopReason::Returned:
  case TranslationStopReason::InternalError:
  case TranslationStopReason::JITUnavailable:
    return false;
  }
  return false;
}

llvm::Error validateTranslationResult(const TranslationResult &Result) {
  const ArchitectureDescription *Guest =
      getArchitectureDescription(Result.Guest);
  if (!Guest)
    return invalid("translation result has an unknown guest architecture");
  if (!getArchitectureDescription(Result.Host))
    return invalid("translation result has an unknown host architecture");
  if (Result.Exit.Version != kTranslationExitVersion)
    return invalid("translation result has an unsupported exit version");
  if (!isKnown(Result.Exit.Reason))
    return invalid("translation result has an unknown stop reason");
  if (getTranslationPairSupport(Result.Guest, Result.Host) !=
          TranslationPairSupport::ContractDefined &&
      Result.Exit.Reason != TranslationStopReason::JITUnavailable)
    return invalid("translation result has an unsupported matrix cell");

  const uint64_t MaxAddress = Guest->AddressWidth == 64
                                  ? std::numeric_limits<uint64_t>::max()
                                  : (uint64_t{1} << Guest->AddressWidth) - 1;
  if (Result.StartPC > MaxAddress || Result.Exit.PC > MaxAddress ||
      Result.Exit.NextPC > MaxAddress)
    return invalid(
        "translation result address exceeds the guest address space");
  if (Result.Exit.FallbackRequested &&
      Result.Exit.Reason != TranslationStopReason::UnsupportedInstruction)
    return invalid(
        "interpreter fallback requires an unsupported-instruction stop");
  if ((Result.Exit.Reason == TranslationStopReason::InternalError ||
       Result.Exit.Reason == TranslationStopReason::JITUnavailable) &&
      Result.Exit.Diagnostic.empty())
    return invalid("internal or JIT availability error requires a diagnostic");

  const unsigned PayloadCount =
      Result.Exit.MemoryFault.has_value() +
      Result.Exit.SelfModification.has_value() +
      Result.Exit.Budget.has_value() + Result.Exit.Syscall.has_value() +
      Result.Exit.ExternalCall.has_value() + Result.Exit.Trap.has_value();
  unsigned RequiredPayloadCount = 0;
  switch (Result.Exit.Reason) {
  case TranslationStopReason::MemoryFault:
    if (!Result.Exit.MemoryFault)
      return invalid("memory-fault exit is missing its typed payload");
    RequiredPayloadCount = 1;
    break;
  case TranslationStopReason::SelfModification:
    if (!Result.Exit.SelfModification)
      return invalid("self-modification exit is missing its typed payload");
    RequiredPayloadCount = 1;
    break;
  case TranslationStopReason::BudgetExhausted:
    if (!Result.Exit.Budget)
      return invalid("budget exit is missing its typed payload");
    RequiredPayloadCount = 1;
    break;
  case TranslationStopReason::Syscall:
    if (!Result.Exit.Syscall)
      return invalid("syscall exit is missing its typed payload");
    RequiredPayloadCount = 1;
    break;
  case TranslationStopReason::ExternalCall:
    if (!Result.Exit.ExternalCall)
      return invalid("external-call exit is missing its typed payload");
    RequiredPayloadCount = 1;
    break;
  case TranslationStopReason::Breakpoint:
  case TranslationStopReason::UnsupportedInstruction:
  case TranslationStopReason::Exception:
  case TranslationStopReason::Signal:
    if (!Result.Exit.Trap)
      return invalid("trap exit is missing its typed payload");
    RequiredPayloadCount = 1;
    break;
  default:
    break;
  }
  if (PayloadCount != RequiredPayloadCount)
    return invalid("translation exit carries an unrelated typed payload");

  if (Result.Exit.MemoryFault) {
    const MemoryFaultExit &Fault = *Result.Exit.MemoryFault;
    if (!isKnown(Fault.Access))
      return invalid("memory-fault exit has an unknown access kind");
    if (Fault.AccessWidthBits == 0)
      return invalid("memory-fault exit has a zero access width");
    if ((Fault.AccessWidthBits % 8) != 0)
      return invalid("memory-fault access width is not byte-multiple");
    if (Fault.RequiredAlignment != 0 &&
        (Fault.RequiredAlignment & (Fault.RequiredAlignment - 1)) != 0)
      return invalid("memory-fault alignment is not a power of two");
    const uint64_t AccessBytes = Fault.AccessWidthBits / 8;
    if (Fault.Address > MaxAddress ||
        AccessBytes - 1 > MaxAddress - Fault.Address)
      return invalid("memory-fault range exceeds the guest address space");
  }
  if (Result.Exit.SelfModification) {
    const SelfModificationExit &Change = *Result.Exit.SelfModification;
    if (Change.Size == 0)
      return invalid("self-modification exit has a zero size");
    if (Change.Address > MaxAddress ||
        Change.Size - 1 > MaxAddress - Change.Address)
      return invalid("self-modification range exceeds the guest address space");
    if (Change.NewGeneration <= Change.OldGeneration)
      return invalid("self-modification generation did not advance");
  }
  if (Result.Exit.Budget) {
    const BudgetExit &Budget = *Result.Exit.Budget;
    if (!isKnown(Budget.Kind))
      return invalid("budget exit has an unknown budget kind");
    if (Budget.Limit == 0)
      return invalid("budget exit has an unbounded limit");
    if (Budget.Observed < Budget.Limit)
      return invalid("budget exit observed work below its limit");
    uint64_t Counter = 0;
    switch (Budget.Kind) {
    case TranslationBudgetKind::GuestInstructions:
      Counter = Result.GuestInstructions;
      break;
    case TranslationBudgetKind::Blocks:
      Counter = Result.BlocksTranslated;
      break;
    case TranslationBudgetKind::GeneratedCodeBytes:
      Counter = Result.GeneratedCodeBytes;
      break;
    }
    if (Budget.Observed != Counter)
      return invalid("budget observation does not match its result counter");
  }
  if (Result.Exit.ExternalCall &&
      Result.Exit.ExternalCall->TargetAddress > MaxAddress)
    return invalid("external-call target exceeds the guest address space");
  if (Result.Exit.Trap) {
    if (!isKnown(Result.Exit.Trap->Kind))
      return invalid("trap exit has an unknown kind");
    if (Result.Exit.Trap->Address > MaxAddress)
      return invalid("trap address exceeds the guest address space");
    if (const std::optional<TranslationTrapKind> Required =
            requiredTrapKind(Result.Exit.Reason);
        Required && Result.Exit.Trap->Kind != *Required)
      return invalid("trap kind does not match the translation stop reason");
  }

  if (Result.Exit.Reason == TranslationStopReason::NotStarted &&
      (Result.GuestInstructions != 0 || Result.BlocksTranslated != 0 ||
       Result.GeneratedCodeBytes != 0 || Result.Exit.FallbackRequested))
    return invalid("not-started result contains translation work");
  return llvm::Error::success();
}

llvm::Error validateTranslationResult(const TranslationResult &Result,
                                      const TranslationOptions &Options) {
  if (llvm::Error Error = validateTranslationOptions(Options))
    return Error;
  if (llvm::Error Error = validateTranslationResult(Result))
    return Error;
  if (Result.Guest != Options.Guest)
    return invalid(
        "translation result guest does not match the translation request");
  if (Options.Target.Kind == HostTargetKind::Explicit &&
      (!Options.Target.Architecture ||
       Result.Host != *Options.Target.Architecture))
    return invalid(
        "translation result host does not match the explicit target");

  if (Result.Exit.FallbackRequested) {
    if (Options.UnsupportedInstructions !=
        UnsupportedInstructionPolicy::InterpreterFallback)
      return invalid(
          "translation result requests fallback forbidden by the request");
    if (!Result.Exit.Trap || !Result.Exit.Trap->Restartable)
      return invalid("interpreter fallback requires a restartable trap");
  }

  const std::optional<TranslationBudgetKind> ExhaustedKind =
      Result.Exit.Reason == TranslationStopReason::BudgetExhausted &&
              Result.Exit.Budget
          ? std::optional<TranslationBudgetKind>(Result.Exit.Budget->Kind)
          : std::nullopt;
  const auto CheckCounter = [&](uint64_t Counter, uint64_t Limit,
                                TranslationBudgetKind Kind,
                                const char *Name) -> llvm::Error {
    if (Limit != 0 && Counter > Limit && ExhaustedKind != Kind)
      return invalid(llvm::Twine("translation result ") + Name +
                     " exceeds the request budget");
    return llvm::Error::success();
  };
  if (llvm::Error Error = CheckCounter(
          Result.GuestInstructions, Options.InstructionBudget,
          TranslationBudgetKind::GuestInstructions, "guest-instruction count"))
    return Error;
  if (llvm::Error Error =
          CheckCounter(Result.BlocksTranslated, Options.BlockBudget,
                       TranslationBudgetKind::Blocks, "translated-block count"))
    return Error;
  if (llvm::Error Error = CheckCounter(
          Result.GeneratedCodeBytes, Options.GeneratedCodeByteBudget,
          TranslationBudgetKind::GeneratedCodeBytes,
          "generated-code byte count"))
    return Error;

  if (Result.Exit.Budget) {
    const BudgetExit &Budget = *Result.Exit.Budget;
    uint64_t RequestedLimit = 0;
    switch (Budget.Kind) {
    case TranslationBudgetKind::GuestInstructions:
      RequestedLimit = Options.InstructionBudget;
      break;
    case TranslationBudgetKind::Blocks:
      RequestedLimit = Options.BlockBudget;
      break;
    case TranslationBudgetKind::GeneratedCodeBytes:
      RequestedLimit = Options.GeneratedCodeByteBudget;
      break;
    }
    if (RequestedLimit == 0)
      return invalid(
          "translation result exhausted a budget unbounded by the request");
    if (Budget.Limit != RequestedLimit)
      return invalid(
          "translation result budget limit does not match the request");
  }
  return llvm::Error::success();
}

} // namespace neverd::translate
