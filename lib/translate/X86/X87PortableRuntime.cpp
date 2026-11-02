//===- X87PortableRuntime.cpp - Checked portable x87 steps ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/translate/X86/X87PortableRuntime.h"

#include "neverd/ir/low/X86/X87Arithmetic.h"
#include "neverd/ir/low/X86/X87PortableContext.h"

#include "llvm/Support/Errc.h"
#include "llvm/Support/ErrorHandling.h"

#include <algorithm>
#include <span>

namespace neverd::translate {
namespace {

PortableX87StepStatusV1 status(X87PortableOutcome Outcome) {
  using S = PortableX87StepStatusV1;
  switch (Outcome) {
  case X87PortableOutcome::Completed:
    return S::Completed;
  case X87PortableOutcome::InvalidInstruction:
    return S::InvalidInstruction;
  case X87PortableOutcome::UnsupportedInstruction:
    return S::UnsupportedInstruction;
  case X87PortableOutcome::InvalidControl:
    return S::InvalidControl;
  case X87PortableOutcome::PendingException:
    return S::PendingException;
  case X87PortableOutcome::UnmaskedControl:
    return S::UnmaskedControl;
  case X87PortableOutcome::UnknownPayload:
    return S::UnknownPayload;
  case X87PortableOutcome::StackFault:
    return S::StackFault;
  case X87PortableOutcome::UnsupportedValue:
    return S::UnsupportedValue;
  case X87PortableOutcome::InvalidMemory:
    return S::InvalidMemory;
  }
  llvm_unreachable("unhandled portable x87 semantic outcome");
}

} // namespace

llvm::Error validatePortableX87StateV1(const PortableX87StateV1 &State) {
  if (State.Magic != kPortableX87StateMagicV1 ||
      State.Version != kPortableX87StateVersionV1 ||
      State.Size != kPortableX87StateSizeV1)
    return llvm::createStringError(llvm::errc::invalid_argument,
                                   "invalid portable x87 state identity");
  if (State.EnvironmentKnown > 1 ||
      std::any_of(std::begin(State.Reserved), std::end(State.Reserved),
                  [](uint8_t Byte) { return Byte != 0; }))
    return llvm::createStringError(llvm::errc::invalid_argument,
                                   "invalid portable x87 state provenance");
  return llvm::Error::success();
}

const char *portableX87StepStatusNameV1(PortableX87StepStatusV1 Status) {
  switch (Status) {
#define NEVERD_X87_PORTABLE_STATUS(CxxName, CName, Value, Diagnostic)          \
  case PortableX87StepStatusV1::CxxName:                                       \
    return Diagnostic;
#include "neverd/ir/X86/X87PortableStatus.def"
#undef NEVERD_X87_PORTABLE_STATUS
  }
  return "unknown-status";
}

PortableX87StepResultV1 executePortableX87RuntimeStepV1(
    PortableX87StateV1 &State, GuestMemoryRuntime &Memory, uint32_t Selector,
    uint64_t InstructionAddress, uint64_t DataAddress) {
  PortableX87StepResultV1 Result;
  if (llvm::Error Error = validatePortableX87StateV1(State)) {
    llvm::consumeError(std::move(Error));
    return Result;
  }
  if (!State.EnvironmentKnown) {
    Result.Status = PortableX87StepStatusV1::UnknownEnvironment;
    return Result;
  }
  Result.Status = status(preflightPortableX87Context(Selector, State.Image));
  if (Result.Status != PortableX87StepStatusV1::Completed)
    return Result;
  const auto Spec = x87ContextInstruction(Selector);
  X87Binary80Bytes Operand{};
  if (!Spec || Spec->MemoryBytes > Operand.size()) {
    Result.Status = PortableX87StepStatusV1::InvalidMemory;
    return Result;
  }
  auto Bytes = llvm::MutableArrayRef(Operand).take_front(Spec->MemoryBytes);
  if (Spec->WritesMemory) {
    Result.MemoryAccess = Memory.validateStoreBytes(DataAddress, Bytes.size());
    if (Result.MemoryAccess->Status != GuestMemoryAccessStatus::Completed) {
      Result.Status = PortableX87StepStatusV1::MemoryFault;
      return Result;
    }
  }
  if (Spec->ReadsMemory) {
    Result.MemoryAccess = Memory.loadBytes(DataAddress, Bytes);
    if (Result.MemoryAccess->Status != GuestMemoryAccessStatus::Completed) {
      Result.Status = PortableX87StepStatusV1::MemoryFault;
      return Result;
    }
  }

  auto Next = State;
  uint64_t Scalar = 0;
  Result.Status = status(
      executePortableX87Context(Selector, Next.Image, Next.KnownPayload,
                                std::span(Bytes.data(), Bytes.size()),
                                InstructionAddress, DataAddress, Scalar));
  if (Result.Status != PortableX87StepStatusV1::Completed)
    return Result;
  if (Spec->WritesMemory) {
    Result.MemoryAccess = Memory.storeBytes(DataAddress, Bytes);
    switch (Result.MemoryAccess->Status) {
    case GuestMemoryAccessStatus::Fault:
      Result.Status = PortableX87StepStatusV1::MemoryFault;
      return Result;
    case GuestMemoryAccessStatus::SelfModification:
      Result.Status = PortableX87StepStatusV1::SelfModification;
      break;
    case GuestMemoryAccessStatus::Completed:
      break;
    }
  }
  State = Next;
  Result.InstructionCompleted = true;
  Result.HasScalarResult = Spec->WritesAX || Spec->WritesFlags;
  if (Result.HasScalarResult)
    Result.ScalarResult = Scalar;
  return Result;
}

} // namespace neverd::translate
