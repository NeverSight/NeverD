//===- RegistrationConditionalCall.cpp - Bound SEH call effects -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Check invocation-specific SEH frame inputs and bounded effects.

#include "RegistrationABIPrivate.h"

#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/lift/X86Regs.h"
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
      checkedRegistrationImportStackPop(Image, Op.Inputs[0].Offset) != 16)
    return false;
  const auto *Imported = Image.findImportAt(Op.Inputs[0].Offset);
  if (!Imported || Imported->IsDelayImport ||
      !isFileBackedPEImportSlot(Image, Op.Inputs[0].Offset))
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
                             const FrameState &State, size_t &Work) {
  if (hasClosedRegistrationRaise(Image, Op, State))
    return chargeCalleeWork(Work, 10);
  if (Op.Opcode != NdOp::CALL || Op.NumInputs != 1 || !Op.Inputs[0].isConst() ||
      Op.Inputs[0].Size != 4)
    return false;
  const va_t Target = Op.Inputs[0].Offset;
  const auto SP =
      State.Registers[x86reg::RSP / x86reg::GeneralRegStride].Offset;
  if (!SP || *SP > INT32_MAX - 128 || Op.Opcode != NdOp::CALL)
    return false;
  std::map<int32_t, FrameValue> Arguments;
  for (int32_t Offset = 8; Offset <= 128; Offset += 4) {
    const auto It = State.Cells.find(*SP + Offset - 8);
    if (It != State.Cells.end() && !It->second.MayBeFrame)
      Arguments.emplace(Offset, It->second);
  }
  if (Arguments.empty())
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
  if (Callee.ExceptionMetadata || !Callee.hasCompleteLiftCoverage() ||
      !registration_abi::chargeDecodedCallee(Work, Callee) ||
      !registration_abi::hasPrivateCallerFrame(
          Callee, Image, Work, Effects, {.ScalarArguments = &Arguments}) ||
      !Effects.Writes.empty() || !Effects.CallerPCWrites.empty())
    return false;
  return true;
}

} // namespace neverd::registration_abi
