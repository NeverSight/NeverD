//===- X86LiftBoundedLeaf.cpp - Bounded SGX and SMX machine state --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Retain unresolved full state until the CFG proves a returning leaf.
//===----------------------------------------------------------------------===//

#include "X86LiftDetail.h"

#include "neverd/lift/X86/X86BoundedLeafState.h"
#include "neverd/support/ISAEncoding.h"

namespace neverd {

void liftBoundedLeafState(LiftStateBase &S, Arch A, Intrinsic Id,
                          std::optional<uint64_t> Ax) {
  const bool Pending = isX86BoundedLeafPending(Id);
  const auto Contract = Ax ? x86BoundedLeafContract(Id, *Ax) : std::nullopt;
  if ((A != Arch::X86 && A != Arch::X64) ||
      (!Pending && (!Contract || !isX86BoundedLeafIntrinsic(Id))))
    llvm::report_fatal_error(X86BoundedLeafUnproven);
  const uint16_t Word = A == Arch::X64 ? 8 : 4;
  const unsigned Reads = Pending ? 0xf : Contract->Reads;
  const unsigned Writes = Pending ? 0xf : Contract->Writes;
  const auto Flags = Pending ? X86BoundedLeafFlags::Capture : Contract->Flags;
  const std::array<uint64_t, 4> Registers = {x86reg::RAX, x86reg::RBX,
                                             x86reg::RCX, x86reg::RDX};
  const std::array<uint64_t, 6> FlagRegisters = {
      x86reg::CF, x86reg::PF, x86reg::AF, x86reg::ZF, x86reg::SF, x86reg::OF};

  NdVar Status = NdVar::scalar(0, X86ArithmeticFlagsBytes);
  if (Flags == X86BoundedLeafFlags::Capture)
    for (unsigned I = 0; I < FlagRegisters.size(); ++I) {
      const auto Boolean = S.makeTemp(1);
      S.emit(NdOp::INT_NOTEQUAL, Boolean,
             {NdVar::reg(FlagRegisters[I], 1), NdVar::scalar(0, 1)});
      const auto Wide = S.makeTemp(X86ArithmeticFlagsBytes);
      S.emit(NdOp::INT_ZEXT, Wide, {Boolean});
      const auto Shifted = S.makeTemp(X86ArithmeticFlagsBytes);
      S.emit(NdOp::INT_LEFT, Shifted,
             {Wide, NdVar::scalar(X86ArithmeticFlagBits[I],
                                  X86ArithmeticFlagsBytes)});
      const auto Next = S.makeTemp(X86ArithmeticFlagsBytes);
      S.emit(NdOp::INT_OR, Next, {Status, Shifted});
      Status = Next;
    }
  std::array<NdVar, X86BoundedLeafRegisterCount> Inputs, Outputs;
  Inputs[0] = Ax ? NdVar::scalar(*Ax, Word) : NdVar::reg(x86reg::RAX, Word);
  for (unsigned I = 1; I < Inputs.size(); ++I)
    Inputs[I] = Reads & (1u << I) ? NdVar::reg(Registers[I], Word)
                                  : NdVar::scalar(0, Word);
  Outputs[0] = S.makeTemp(Word);
  S.emitIntrinsic(Id, Outputs[0],
                  {Inputs[0], Inputs[1], Inputs[2], Inputs[3], Status});
  for (unsigned I = 1; I < Outputs.size(); ++I) {
    Outputs[I] = S.makeTemp(Word);
    S.emit(NdOp::COPY, Outputs[I], {S.makeTemp(Word)});
  }
  const auto ReturnedFlags = S.makeTemp(X86ArithmeticFlagsBytes);
  S.emit(NdOp::COPY, ReturnedFlags, {S.makeTemp(X86ArithmeticFlagsBytes)});
  for (unsigned I = 0; I < Outputs.size(); ++I)
    if (Writes & (1u << I))
      S.emit(NdOp::COPY, NdVar::reg(Registers[I], Word), {Outputs[I]});
  if (Flags != X86BoundedLeafFlags::Preserve)
    for (unsigned I = 0; I < FlagRegisters.size(); ++I) {
      const auto Shifted = S.makeTemp(X86ArithmeticFlagsBytes);
      S.emit(NdOp::INT_RIGHT, Shifted,
             {ReturnedFlags, NdVar::scalar(X86ArithmeticFlagBits[I],
                                           X86ArithmeticFlagsBytes)});
      const auto Low = S.makeTemp(1);
      S.emit(NdOp::SUBBYTES, Low, {Shifted, NdVar::scalar(0, 4)});
      S.emit(NdOp::INT_AND, NdVar::reg(FlagRegisters[I], 1),
             {Low, NdVar::scalar(1, 1)});
    }
}

bool liftBoundedLeaf(X86Lifter &L, X86Lifter::LiftState &S, const cs_insn *Insn,
                     const cs_x86 &X) {
  const bool Enclu = Insn->id == X86_INS_ENCLU;
  const bool Wide = L.targetArch() == Arch::X64;
  const unsigned OpcodeBytes = Enclu ? 3 : 2;
  if ((!Enclu && Insn->id != X86_INS_GETSEC) || !Insn->detail ||
      Insn->size < OpcodeBytes || Insn->size > x86::kMaxInstructionLen ||
      X.op_count || X.encoding.imm_size || X.encoding.disp_size)
    return false;
  const unsigned End = Insn->size - OpcodeBytes;
  for (unsigned I = 0; I < End; ++I) {
    const auto B = Insn->bytes[I];
    if (B != 0x67 && B != 0x26 && B != 0x2e && B != 0x36 && B != 0x3e &&
        B != 0x64 && B != 0x65 && !(Wide && B >= 0x40 && B <= 0x4f))
      return false;
  }
  if (Insn->bytes[End] != 0x0f || X.opcode[0] != 0x0f ||
      Insn->bytes[End + 1] != (Enclu ? 0x01 : 0x37) ||
      X.opcode[1] != Insn->bytes[End + 1] ||
      (Enclu ? (Insn->bytes[End + 2] != 0xd7 || X.modrm != 0xd7 ||
                X.encoding.modrm_offset != End + 2)
             : (X.modrm || X.encoding.modrm_offset)))
    return false;
  liftBoundedLeafState(S, L.targetArch(),
                       Enclu ? Intrinsic::X86EncluPending
                             : Intrinsic::X86GetsecPending);
  return true;
}

} // namespace neverd
