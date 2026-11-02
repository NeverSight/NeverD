//===- X86LiftSgx.cpp - SGX supervisor/VMM completion --------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Preserve implicit registers, leaf-dependent flags and native DS accesses.
/// Address/segment/REX overrides are ignored; LOCK/66/REP forms are invalid.
//===----------------------------------------------------------------------===//

#include "X86LiftDetail.h"

#include "neverd/ir/intrinsics/X86/X86Sgx.h"
#include "neverd/ir/intrinsics/X86/X86SgxLeaves.h"
#include "neverd/lift/X86/X86Lifter.h"
#include "neverd/support/ISAEncoding.h"

namespace neverd {

bool liftSgx(X86Lifter &L, X86Lifter::LiftState &S, const cs_insn *Insn,
             const cs_x86 &X) {
  const bool Wide = L.targetArch() == Arch::X64;
  const bool Supervisor = Insn->id == X86_INS_ENCLS;
  if ((!Supervisor && Insn->id != X86_INS_ENCLV) || !Insn->detail ||
      Insn->size < 3 || Insn->size > x86::kMaxInstructionLen || X.op_count ||
      X.encoding.imm_size || X.encoding.disp_size)
    return false;
  const unsigned End = Insn->size - 3;
  for (unsigned I = 0; I < End; ++I) {
    const auto B = Insn->bytes[I];
    if (B != 0x67 && B != 0x26 && B != 0x2e && B != 0x36 && B != 0x3e &&
        B != 0x64 && B != 0x65 && !(Wide && B >= 0x40 && B <= 0x4f))
      return false;
  }
  if (Insn->bytes[End] != 0x0f || Insn->bytes[End + 1] != 0x01 ||
      Insn->bytes[End + 2] != (Supervisor ? 0xcf : 0xc0) ||
      X.opcode[0] != 0x0f || X.opcode[1] != 0x01 ||
      X.modrm != Insn->bytes[End + 2] || X.encoding.modrm_offset != End + 2)
    return false;

  const uint16_t Word = Wide ? 8 : 4;
  const auto Leaf = S.makeTemp(4);
  S.emit(NdOp::COPY, Leaf, {NdVar::reg(x86reg::RAX, 4)});
  const auto Shift = S.makeTemp(4);
  S.emit(NdOp::INT_AND, Shift,
         {Leaf, NdVar::scalar(X86SgxLeafMaskBits - 1, 4)});
  const auto InRange = S.makeTemp(1);
  S.emit(NdOp::INT_LESS, InRange, {Leaf, NdVar::scalar(X86SgxLeafMaskBits, 4)});
  auto Member = [&](uint32_t Mask) {
    const auto Bits = S.makeTemp(4);
    S.emit(NdOp::INT_RIGHT, Bits, {NdVar::scalar(Mask, 4), Shift});
    const auto Bit = S.makeTemp(4);
    S.emit(NdOp::INT_AND, Bit, {Bits, NdVar::scalar(1, 4)});
    const auto Set = S.makeTemp(1);
    S.emit(NdOp::INT_NOTEQUAL, Set, {Bit, NdVar::scalar(0, 4)});
    const auto Match = S.makeTemp(1);
    S.emit(NdOp::INT_AND, Match, {InRange, Set});
    return Match;
  };
  uint32_t PreservedFlags = 0;
  if (Supervisor)
    for (const auto L : X86SgxPreservedFlagLeaves)
      PreservedFlags |= uint32_t{1} << L;
  const auto Preserve = Member(PreservedFlags);
  const auto &Masks =
      Supervisor ? X86EnclsInputLeafMasks : X86EnclvInputLeafMasks;
  const std::array<uint64_t, 3> Registers = {x86reg::RBX, x86reg::RCX,
                                             x86reg::RDX};
  std::array<NdVar, Registers.size()> Arguments;
  for (unsigned I = 0; I < Arguments.size(); ++I) {
    const auto Used = Member(Masks[I]);
    Arguments[I] = S.makeTemp(Word);
    S.emit(NdOp::SELECT, Arguments[I],
           {Used, NdVar::reg(Registers[I], Word), NdVar::scalar(0, Word)});
  }
  S.emit(NdOp::INTRINSIC, NdVar::reg(x86reg::RAX, Word),
         {NdVar::cst(static_cast<unsigned>(Supervisor ? Intrinsic::X86Encls
                                                      : Intrinsic::X86Enclv),
                     2),
          NdVar::reg(x86reg::RAX, Word), Arguments[0], Arguments[1],
          Arguments[2]});
  // Keep the auxiliary transport consecutive: Low-to-Med records these
  // identities before optimizations can remove or forward the copies.
  const auto Bx = S.makeTemp(Word);
  S.emit(NdOp::COPY, Bx, {S.makeTemp(Word)});
  const auto Flags = S.makeTemp(X86SgxFlagBytes);
  S.emit(NdOp::COPY, Flags, {S.makeTemp(X86SgxFlagBytes)});
  if (Supervisor) {
    const auto DebugRead = S.makeTemp(1);
    S.emit(NdOp::INT_EQUAL, DebugRead,
           {Leaf, NdVar::scalar(X86SgxDebugReadLeaf, 4)});
    const auto OldBx = NdVar::reg(x86reg::RBX, Word);
    S.emit(NdOp::SELECT, OldBx, {DebugRead, Bx, OldBx});
  }
  for (const auto Reg : {x86reg::CF, x86reg::ZF, x86reg::PF, x86reg::AF,
                         x86reg::SF, x86reg::OF}) {
    NdVar Value = NdVar::scalar(0, 1);
    if (Reg == x86reg::CF || Reg == x86reg::ZF) {
      Value = S.makeTemp(1);
      S.emit(NdOp::SUBBYTES, Value,
             {Flags, NdVar::scalar(Reg == x86reg::ZF ? 1 : 0, 4)});
    }
    const auto Flag = NdVar::reg(Reg, 1);
    S.emit(NdOp::SELECT, Flag, {Preserve, Flag, Value});
  }
  return true;
}

} // namespace neverd
