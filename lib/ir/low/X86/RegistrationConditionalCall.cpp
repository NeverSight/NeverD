//===- RegistrationConditionalCall.cpp - Bound SEH call effects -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Check invocation-specific SEH frame inputs and bounded effects.

#include "RegistrationABIPrivate.h"
#include "RegistrationConditionalFrame.h"
#include "RegistrationOriginalImage.h"

#include "neverd/ir/Limits.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/lift/X86/X86Regs.h"
#include "neverd/loader/ReadOnlyBytes.h"
#include "neverd/support/BinaryEncoding.h"

namespace neverd::registration_abi {
using namespace registration_state;

bool hasClosedRegistrationRaise(const BinaryImage &Image, const LowOp &Op,
                                const FrameState &State) {
  if (Op.Opcode != NdOp::INDIR_CALL || Op.NumInputs != 1 || Op.Seq < 0 ||
      !Op.Inputs[0].isConst() || Op.Inputs[0].Size != 4 ||
      Op.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
      Op.MemoryOrdering != NdMemoryOrdering::None || Op.Addr > UINT32_MAX - 6 ||
      !isCheckedRegistrationRaiseSlot(Image, Op.Inputs[0].Offset))
    return false;
  const auto Bytes =
      readImmutablePE32CodeBytes(Image, Op.Addr, 6, {Op.Addr + 2});
  if (!Bytes || (*Bytes)[0] != 0xff || (*Bytes)[1] != 0x15 ||
      readLE<uint32_t>(Bytes->data() + 2) != Op.Inputs[0].Offset)
    return false;
  const auto &SP = State.Registers[x86reg::RSP / x86reg::GeneralRegStride];
  std::array<FrameValue, 4> Args;
  if (SP.Offset && *SP.Offset <= INT32_MAX - 16)
    for (unsigned I = 0; I != Args.size(); ++I)
      Args[I] = State.load(*SP.Offset + I * 4, 4);
  else if (SP.CallbackAddress &&
           State.callbackMemoryIsPrivate(*SP.CallbackAddress, 16, true))
    for (unsigned I = 0; I != Args.size(); ++I)
      Args[I] = State.loadCallback(SP.CallbackAddress->Offset + I * 4, 4);
  else
    return false;
  // A closed invocation lends no argument array. Dispatch effects remain
  // obligations of the enclosing language solver, rather than an ABI fact.
  return !Args[0].MayBeFrame && Args[1].Constant && *Args[1].Constant <= 1 &&
         Args[2].Constant == 0 && Args[3].Constant == 0;
}

bool hasConditionalCallFrame(const BinaryImage &Image, const LowOp &Op,
                             const FrameState &State, size_t &Work,
                             const std::set<int32_t> *Initialized,
                             std::optional<FrameValue> *ReturnValue,
                             BoundPlainCalleeEffect *Effect,
                             OriginalImageFrameProof *OriginalImage) {
  if (ReturnValue)
    ReturnValue->reset();
  if (Effect)
    *Effect = {};
  if (OriginalImage && !Effect)
    return false;
  if (hasClosedRegistrationRaise(Image, Op, State))
    return !Effect && chargeCalleeWork(Work, 10);
  if (Op.Opcode != NdOp::CALL || Op.NumInputs != 1 || !Op.Inputs[0].isConst() ||
      Op.Inputs[0].Size != 4)
    return false;
  const va_t Target = Op.Inputs[0].Offset;
  if (Effect && Image.ExceptionMetadata.findFunction(Target))
    return false;
  const auto SP =
      State.Registers[x86reg::RSP / x86reg::GeneralRegStride].Offset;
  constexpr int32_t ArgumentLimit = limits::ir::
      kRegistrationConditionalCallHasConditionalCallFrameOffsetLimit;
  if (!SP || *SP > INT32_MAX - ArgumentLimit)
    return false;
  std::map<unsigned, FrameValue> Registers;
  for (unsigned Index = 0; Index != State.Registers.size(); ++Index) {
    if (!chargeCalleeWork(Work, 1))
      return false;
    const auto &Value = State.Registers[Index];
    if (isX86VolatileFrameRegister(Index) && !Value.MayBeFrame)
      Registers.emplace(Index, Value);
  }
  std::map<int32_t, FrameValue> Arguments;
  for (int32_t Offset = 8; Offset <= ArgumentLimit; Offset += 4) {
    if (!chargeCalleeWork(Work, 1))
      return false;
    const auto Value =
        readCallerFrameWord(State, *SP + Offset - 8, Initialized, Work);
    if (Value && !Value->MayBeFrame)
      Arguments.emplace(Offset, *Value);
  }
  if (Arguments.empty() && Registers.empty())
    return false;
  Decoder Dec;
  if (!Dec.init(Image))
    return false;
  CFGBuilder Builder;
  auto Callee =
      Builder.buildIntegerFrameProof(Image, Dec, Target, "bound-seh-call");
  for (const auto &Block : Callee.Blocks)
    for (const auto &Insn : Block.InstructionBoundaries)
      if (!chargeCalleeWork(Work, 1) ||
          !hasImmutableCalleeInstruction(Image, Insn, Dec))
        return false;
  registration_abi::ImageFrameEffects Effects;
  std::optional<FrameValue> Returned;
  if (Callee.ExceptionMetadata || !Callee.hasCompleteLiftCoverage() ||
      !registration_abi::chargeDecodedCallee(Work, Callee) ||
      !registration_abi::hasPrivateCallerFrame(
          Callee, Image, Work, Effects,
          {.ScalarArguments = &Arguments,
           .ScalarRegisters = &Registers,
           .ReturnValue = ReturnValue || Effect ? &Returned : nullptr,
           .ExcludeCalls = Effect != nullptr,
           .OriginalImage = OriginalImage}) ||
      !Effects.Writes.empty() || !Effects.CallerPCWrites.empty() ||
      (!Effect && !Effects.Reads.empty()))
    return false;
  if (OriginalImage && !OriginalImage->recordCode(Callee, Work))
    return false;
  if (Effect && !Returned)
    return false;
  std::optional<uint32_t> Pop;
  if ((ReturnValue || Effect) && Returned) {
    // A recovered RETURN is not evidence for an unresolved tail path. Reuse
    // the closed original-body proof before projecting any result to a caller.
    Pop = getCheckedX86CalleeStackPop(Image, Target, &Work);
    if (!Pop)
      return false;
  }
  if (Effect) {
    if (!chargeCalleeWork(Work, Effects.Reads.size() + 1))
      return false;
    Effect->StackPopBytes = *Pop;
    copyExtents(Effects.Reads, Effect->ImageReads);
    Effect->ReturnValue = projectX86CallReturnValue(*Returned);
  }
  if (ReturnValue && Returned)
    *ReturnValue = projectX86CallReturnValue(*Returned);
  return true;
}

} // namespace neverd::registration_abi
