//===- X86LiftVirtualization.cpp - Architectural VMX/SVM lifting ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Preserve VMX completion, conditional VMREAD destinations and explicit SVM
/// operands. Opaque processor state and instruction-owned memory accesses are
/// never replaced by ordinary loads, stores or assumed-success constants.
//===----------------------------------------------------------------------===//

#include "X86LiftDetail.h"

#include "neverd/ir/intrinsics/X86/X86Virtualization.h"
#include "neverd/lift/X86/X86Lifter.h"

#include <vector>

namespace neverd {
namespace {

bool wordRegister(const cs_x86_op &Operand, uint16_t Word) {
  return Operand.type == X86_OP_REG && Operand.size == Word &&
         mapCapstoneReg(static_cast<x86_reg>(Operand.reg)).Size == Word;
}

void writeVmxFlags(X86Lifter::LiftState &S, const NdVar &Flags) {
  S.emit(NdOp::SUBBYTES, NdVar::reg(x86reg::CF, 1),
         {Flags, NdVar::scalar(0, 8)});
  S.emit(NdOp::SUBBYTES, NdVar::reg(x86reg::ZF, 1),
         {Flags, NdVar::scalar(1, 8)});
  for (const auto Flag : {x86reg::PF, x86reg::AF, x86reg::SF, x86reg::OF})
    S.emit(NdOp::COPY, NdVar::reg(Flag, 1), {NdVar::scalar(0, 1)});
}

} // namespace

bool liftVirtualization(X86Lifter &L, X86Lifter::LiftState &S,
                        const cs_insn *Insn, const cs_x86 &X86) {
  const auto TargetArch = L.targetArch();
  if (TargetArch != Arch::X86 && TargetArch != Arch::X64)
    return false;
  const uint16_t Word = TargetArch == Arch::X64 ? 8 : 4;
  Intrinsic Id = Intrinsic::None;
  std::vector<NdVar> Inputs;
  NdMemoryAddressSpace Space = NdMemoryAddressSpace::Default;
  const cs_x86_op *ReadDestination = nullptr;
  auto Memory = [&](const cs_x86_op &Operand) {
    Space = S.memoryAddressSpace(Operand);
    return S.computeEA(Operand);
  };
  switch (Insn->id) {
  case X86_INS_VMREAD:
    if (X86.op_count != 2 || !wordRegister(X86.operands[1], Word))
      return false;
    if (wordRegister(X86.operands[0], Word)) {
      Id = Intrinsic::VmxRead;
      ReadDestination = &X86.operands[0];
      Inputs = {L.operandRead(S, X86.operands[1])};
    } else if (X86.operands[0].type == X86_OP_MEM &&
               X86.operands[0].size == Word) {
      Id = Intrinsic::VmxReadMemory;
      Inputs = {L.operandRead(S, X86.operands[1]), Memory(X86.operands[0])};
    } else
      return false;
    break;
  case X86_INS_VMWRITE:
    if (X86.op_count != 2 || !wordRegister(X86.operands[0], Word))
      return false;
    if (wordRegister(X86.operands[1], Word)) {
      Id = Intrinsic::VmxWrite;
      Inputs = {L.operandRead(S, X86.operands[0]),
                L.operandRead(S, X86.operands[1])};
    } else if (X86.operands[1].type == X86_OP_MEM &&
               X86.operands[1].size == Word) {
      Id = Intrinsic::VmxWriteMemory;
      Inputs = {L.operandRead(S, X86.operands[0]), Memory(X86.operands[1])};
    } else
      return false;
    break;
  case X86_INS_VMXON:
  case X86_INS_VMCLEAR:
  case X86_INS_VMPTRLD:
  case X86_INS_VMPTRST:
    if (X86.op_count != 1 || X86.operands[0].type != X86_OP_MEM ||
        X86.operands[0].size != 8)
      return false;
    Id = Insn->id == X86_INS_VMXON     ? Intrinsic::VmxOn
         : Insn->id == X86_INS_VMCLEAR ? Intrinsic::VmxClear
         : Insn->id == X86_INS_VMPTRLD ? Intrinsic::VmxPtrLoad
                                       : Intrinsic::VmxPtrStore;
    Inputs = {Memory(X86.operands[0])};
    break;
  case X86_INS_INVEPT:
  case X86_INS_INVVPID:
    if (X86.op_count != 2 || !wordRegister(X86.operands[0], Word) ||
        X86.operands[1].type != X86_OP_MEM || X86.operands[1].size != 16)
      return false;
    Id = Insn->id == X86_INS_INVEPT ? Intrinsic::VmxInvalidateEpt
                                    : Intrinsic::VmxInvalidateVpid;
    Inputs = {L.operandRead(S, X86.operands[0]), Memory(X86.operands[1])};
    break;
  case X86_INS_VMXOFF:
  case X86_INS_VMLAUNCH:
  case X86_INS_VMRESUME:
    if (X86.op_count)
      return false;
    Id = Insn->id == X86_INS_VMXOFF     ? Intrinsic::VmxOff
         : Insn->id == X86_INS_VMLAUNCH ? Intrinsic::VmxLaunch
                                        : Intrinsic::VmxResume;
    break;
  case X86_INS_VMFUNC:
    if (X86.op_count)
      return false;
    Id = Intrinsic::VmxFunction;
    Inputs = {NdVar::reg(x86reg::RAX, 4), NdVar::reg(x86reg::RCX, 4)};
    break;
  case X86_INS_VMLOAD:
  case X86_INS_VMSAVE:
  case X86_INS_INVLPGA:
    if ((S.AddressSize != 4 && S.AddressSize != 8) ||
        X86.op_count > (Insn->id == X86_INS_INVLPGA ? 2 : 1))
      return false;
    Id = Insn->id == X86_INS_VMLOAD   ? Intrinsic::SvmLoad
         : Insn->id == X86_INS_VMSAVE ? Intrinsic::SvmSave
                                      : Intrinsic::SvmInvalidate;
    Inputs = {NdVar::reg(x86reg::RAX, S.AddressSize)};
    if (Insn->id == X86_INS_INVLPGA)
      Inputs.push_back(NdVar::reg(x86reg::RCX, 4));
    break;
  case X86_INS_STGI:
  case X86_INS_CLGI:
    if (X86.op_count)
      return false;
    Id = Insn->id == X86_INS_STGI ? Intrinsic::SvmStgi : Intrinsic::SvmClgi;
    break;
  default:
    return false;
  }
  const auto Contract = x86VirtualizationContract(Id);
  auto Emit = [&](NdVar Output) {
    if (Inputs.size() == 2)
      S.emitIntrinsic(Id, Output, {Inputs[0], Inputs[1]},
                      NdMemoryOrdering::None, Space);
    else if (Inputs.size() == 1)
      S.emitIntrinsic(Id, Output, {Inputs[0]}, NdMemoryOrdering::None, Space);
    else
      S.emitIntrinsic(Id, Output, {}, NdMemoryOrdering::None, Space);
  };
  if (Contract.Result == X86VirtualizationResult::None) {
    Emit(NdVar());
    return true;
  }
  const bool Read = Contract.Result == X86VirtualizationResult::Read;
  const NdVar Result = S.makeTemp(Read ? 2 * Word : 2);
  Emit(Result);
  NdVar Flags = Result;
  if (Read) {
    Flags = S.makeTemp(2);
    S.emit(NdOp::SUBBYTES, Flags, {Result, NdVar::scalar(Word, 8)});
    const NdVar Value = S.makeTemp(Word);
    S.emit(NdOp::SUBBYTES, Value, {Result, NdVar::scalar(0, 8)});
    const NdVar Success = S.makeTemp(1);
    S.emit(NdOp::INT_EQUAL, Success, {Flags, NdVar::scalar(0, 2)});
    const RegInfo Destination =
        mapCapstoneReg(static_cast<x86_reg>(ReadDestination->reg));
    const NdVar Register = NdVar::reg(Destination.Offset, Word);
    S.emit(NdOp::SELECT, Register, {Success, Value, Register});
  }
  writeVmxFlags(S, Flags);
  return true;
}

} // namespace neverd
