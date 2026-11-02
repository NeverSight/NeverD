//===- RuntimeX87Helpers.cpp - Sealed portable translation step -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/translate/X86/RuntimeX87Helpers.h"

#include "neverd/ir/X86/X87Capabilities.h"

#include "llvm/Support/Errc.h"

#include <type_traits>
#include <utility>

namespace neverd::translate {
namespace {

llvm::Error invalid(llvm::StringRef Message) {
  return llvm::createStringError(llvm::errc::invalid_argument, "%s",
                                 Message.str().c_str());
}

bool consumeInvalid(llvm::Error Error) {
  if (!Error)
    return false;
  llvm::consumeError(std::move(Error));
  return true;
}

uint32_t execute(RuntimeCallFrameV1 *Frame, RuntimeGuestStateX86_64X87V1 *State,
                 uint32_t Selector, uint64_t GuestIP, uint64_t NextPC,
                 uint64_t GuestDP) {
  constexpr uint32_t Unsupported =
      static_cast<uint32_t>(BlockExitKindV1::Unsupported);
  if (!Frame || !Frame->Memory || !State)
    return Unsupported;
  if (consumeInvalid(validateRuntimeGuestStateX86_64X87V1(*State)) ||
      consumeInvalid(validateRuntimeCodeCredentialV1(Frame->Published,
                                                     Frame->Validated)) ||
      consumeInvalid(validateRuntimeControlBlockExactSpanV1(Frame->Control)) ||
      Frame->Memory->codeInvalidationPolicy() ==
          CodeInvalidationPolicy::ValidateBeforeDispatch ||
      Frame->Control.CodeInvalidation !=
          static_cast<uint32_t>(Frame->Memory->codeInvalidationPolicy()) ||
      Frame->Control.Exit.Kind != RuntimeABIExitKindV1::None ||
      State->LastStep.Attempted || State->Integer.RIP != GuestIP ||
      NextPC <= GuestIP)
    return Unsupported;

  Frame->Control = Frame->Memory->snapshotControlBlock();
  if (Frame->Control.Exit.Kind != RuntimeABIExitKindV1::None)
    return static_cast<uint32_t>(Frame->Control.Exit.Kind);

  auto &Last = State->LastStep;
  Last = {};
  Last.Attempted = 1;
  Last.Selector = Selector;
  Last.PC = GuestIP;
  Last.NextPC = NextPC;
  Last.DataAddress = GuestDP;
  Last.Status = PortableX87StepStatusV1::UnsupportedInstruction;
  if (!isRuntimeX87InstructionV1(Selector))
    return Unsupported;

  const auto Result = executePortableX87RuntimeStepV1(
      State->X87, *Frame->Memory, Selector, GuestIP, GuestDP);
  Last.Status = Result.Status;
  Last.InstructionCompleted = Result.InstructionCompleted;
  Last.HasScalarResult = Result.HasScalarResult;
  Last.ScalarResult = Result.HasScalarResult ? Result.ScalarResult : 0;
  if (Result.InstructionCompleted) {
    const auto Spec = x87ContextInstruction(Selector);
    if (Spec->WritesAX) {
      auto &RAX =
          State->Integer.GPR[static_cast<unsigned>(RuntimeX86_64GPRV1::RAX)];
      RAX = (RAX & ~uint64_t{UINT16_MAX}) | (Result.ScalarResult & UINT16_MAX);
    }
    State->Integer.RIP = NextPC;
  }
  // Invalidate even if a concurrent cancellation replaces SelfModification
  // in the control snapshot. The architectural store and pop already committed.
  if (Result.Status == PortableX87StepStatusV1::SelfModification)
    Frame->Validated = {};
  Frame->Control = Frame->Memory->snapshotControlBlock();
  if (Frame->Control.Exit.Kind != RuntimeABIExitKindV1::None)
    return static_cast<uint32_t>(Frame->Control.Exit.Kind);
  return static_cast<uint32_t>(Result.InstructionCompleted
                                   ? BlockExitKindV1::Continue
                                   : BlockExitKindV1::Unsupported);
}

} // namespace

bool isRuntimeX87InstructionV1(uint32_t Selector) {
  return isPortableX87RuntimeInstruction(Selector);
}

llvm::Error validateRuntimeX87AfterInvocationV1(
    const RuntimeGuestStateX86_64X87V1 &State, const RuntimeCallFrameV1 &Frame,
    uint32_t Status,
    const std::optional<RuntimeX87StepContractV1> &ExpectedStep) {
  if (llvm::Error Error = validateRuntimeGuestStateX86_64X87V1(State))
    return Error;
  if (llvm::Error Error = validateRuntimeControlBlockExactSpanV1(Frame.Control))
    return Error;
  if (!Frame.Memory ||
      Frame.Control.CodeInvalidation !=
          static_cast<uint32_t>(Frame.Memory->codeInvalidationPolicy()) ||
      Frame.Memory->codeInvalidationPolicy() ==
          CodeInvalidationPolicy::ValidateBeforeDispatch)
    return invalid("portable x87 invocation has an unsupported memory policy");
  const auto Kind = Frame.Control.Exit.Kind;
  if (Status < kBlockExitKindBaseV1) {
    if (Kind == RuntimeABIExitKindV1::None ||
        Status != static_cast<uint32_t>(Kind))
      return invalid(
          "portable x87 runtime status disagrees with the control record");
  } else if (Status > static_cast<uint32_t>(BlockExitKindV1::Trap) ||
             Kind != RuntimeABIExitKindV1::None) {
    return invalid(
        "portable x87 block status disagrees with the control record");
  }
  const auto &Last = State.LastStep;
  if (!ExpectedStep) {
    if (Last.Attempted)
      return invalid("integer block produced an unbound x87 diagnostic");
    return validateRuntimeCodeCredentialV1(Frame.Published, Frame.Validated);
  }
  if (!Last.Attempted) {
    if (Kind != RuntimeABIExitKindV1::Cancelled ||
        State.Integer.RIP != ExpectedStep->PC)
      return invalid("x87 boundary did not produce its step diagnostic");
    return validateRuntimeCodeCredentialV1(Frame.Published, Frame.Validated);
  }
  if (Last.Selector != ExpectedStep->Selector || Last.PC != ExpectedStep->PC ||
      Last.NextPC != ExpectedStep->NextPC ||
      State.Integer.RIP != (Last.InstructionCompleted ? Last.NextPC : Last.PC))
    return invalid("portable x87 step does not match its bound instruction or "
                   "completion PC");
  if (!isRuntimeX87InstructionV1(Last.Selector))
    return invalid("portable x87 invocation used an unqualified instruction");
  if (Last.Status == PortableX87StepStatusV1::SelfModification) {
    if (Frame.Validated != RuntimeCodeCredentialV1{})
      return invalid(
          "completed executable x87 write retained a stale credential");
  } else if (llvm::Error Error = validateRuntimeCodeCredentialV1(
                 Frame.Published, Frame.Validated)) {
    return Error;
  }
  switch (Last.Status) {
  case PortableX87StepStatusV1::InvalidState:
  case PortableX87StepStatusV1::InvalidInstruction:
  case PortableX87StepStatusV1::UnsupportedInstruction:
  case PortableX87StepStatusV1::InvalidMemory:
  case PortableX87StepStatusV1::InternalError:
    return invalid("portable x87 helper failed its admitted runtime contract");
  default:
    break;
  }
  const auto Spec = x87ContextInstruction(Last.Selector);
  if (Last.InstructionCompleted && Spec->WritesAX &&
      (State.Integer.GPR[static_cast<unsigned>(RuntimeX86_64GPRV1::RAX)] &
       UINT16_MAX) != Last.ScalarResult)
    return invalid("portable x87 scalar result was not committed to AX");
  if (Kind == RuntimeABIExitKindV1::Cancelled)
    return llvm::Error::success();
  switch (Last.Status) {
  case PortableX87StepStatusV1::Completed:
    if (Status != static_cast<uint32_t>(BlockExitKindV1::Continue))
      return invalid("completed x87 step returned the wrong block boundary");
    break;
  case PortableX87StepStatusV1::MemoryFault:
    if (Kind != RuntimeABIExitKindV1::MemoryFault || Spec->MemoryBytes == 0 ||
        Frame.Control.Exit.Address != Last.DataAddress ||
        Frame.Control.Exit.Size != Spec->MemoryBytes)
      return invalid("x87 memory fault does not match the current operand");
    break;
  case PortableX87StepStatusV1::SelfModification:
    if (Kind != RuntimeABIExitKindV1::SelfModification || !Spec->WritesMemory ||
        Frame.Control.Exit.Address != Last.DataAddress ||
        Frame.Control.Exit.Size != Spec->MemoryBytes)
      return invalid(
          "x87 executable write does not match the completed operand");
    break;
  default:
    if (Status != static_cast<uint32_t>(BlockExitKindV1::Unsupported))
      return invalid("refused x87 step returned a success or memory boundary");
    break;
  }
  return llvm::Error::success();
}

} // namespace neverd::translate

static_assert(std::is_same_v<neverd::translate::RuntimeABIX87StepHelperV1,
                             decltype(&nvd_rt_x87_v1_step)>);

extern "C" uint32_t nvd_rt_x87_v1_step(void *Runtime, void *State,
                                       uint32_t Selector, uint64_t GuestIP,
                                       uint64_t NextPC,
                                       uint64_t GuestDP) noexcept {
  using namespace neverd::translate;
  auto *Frame = static_cast<RuntimeCallFrameV1 *>(Runtime);
  auto *Guest = static_cast<RuntimeGuestStateX86_64X87V1 *>(State);
  try {
    return execute(Frame, Guest, Selector, GuestIP, NextPC, GuestDP);
  } catch (...) {
    if (Guest && Guest->LastStep.Attempted &&
        !Guest->LastStep.InstructionCompleted)
      Guest->LastStep.Status = PortableX87StepStatusV1::InternalError;
    return static_cast<uint32_t>(BlockExitKindV1::Unsupported);
  }
}
