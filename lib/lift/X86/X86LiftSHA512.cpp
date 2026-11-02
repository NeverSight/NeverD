//===- X86LiftSHA512.cpp - SHA-512 message and round semantics -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Integer LowIR for Intel SHA512, including source/destination aliasing and
/// the dependent message recurrence. Specification: Intel 319433-052, the
/// VSHA512MSG1/MSG2/RNDS2 instruction entries (March 2024).
//===----------------------------------------------------------------------===//

#include "X86LiftDetail.h"

#include <array>

namespace neverd {
namespace {

bool vectorOperand(const cs_x86_op &Operand, uint16_t Size, unsigned Number) {
  return Operand.type == X86_OP_REG && Operand.size == Size &&
         Operand.reg == (Size == 32 ? X86_REG_YMM0 : X86_REG_XMM0) + Number &&
         !Operand.avx_zero_opmask && Operand.avx_bcast == X86_AVX_BCAST_INVALID;
}

class SHA512Builder {
  X86Lifter::LiftState &S;

public:
  explicit SHA512Builder(X86Lifter::LiftState &State) : S(State) {}

  NdVar binary(NdOp Op, NdVar A, NdVar B) {
    const NdVar Result = S.makeTemp(8);
    S.emit(Op, Result, {A, B});
    return Result;
  }
  NdVar add(NdVar A, NdVar B) { return binary(NdOp::INT_ADD, A, B); }
  NdVar xorValue(NdVar A, NdVar B) { return binary(NdOp::INT_XOR, A, B); }
  NdVar andValue(NdVar A, NdVar B) { return binary(NdOp::INT_AND, A, B); }
  NdVar rotate(NdVar V, unsigned Count) {
    return binary(NdOp::INT_OR,
                  binary(NdOp::INT_RIGHT, V, NdVar::scalar(Count, 8)),
                  binary(NdOp::INT_LEFT, V, NdVar::scalar(64 - Count, 8)));
  }
  NdVar sigma(NdVar V, unsigned A, unsigned B, unsigned C, bool Shift) {
    return xorValue(xorValue(rotate(V, A), rotate(V, B)),
                    Shift ? binary(NdOp::INT_RIGHT, V, NdVar::scalar(C, 8))
                          : rotate(V, C));
  }
  NdVar lane(NdVar V, unsigned Index) {
    const NdVar Result = S.makeTemp(8);
    S.emit(NdOp::SUBBYTES, Result, {V, NdVar::scalar(8 * Index, 4)});
    return Result;
  }
};

} // namespace

bool liftSHA512(X86Lifter &L, X86Lifter::LiftState &S, const cs_insn *Insn,
                const cs_x86 &X86) {
  const bool Rounds = Insn->id == X86_INS_VSHA512RNDS2;
  const bool Message2 = Insn->id == X86_INS_VSHA512MSG2;
  if (!Rounds && !Message2 && Insn->id != X86_INS_VSHA512MSG1)
    return false;
  CanonicalVex3EncodingInfo E;
  if (!parseCanonicalVex3EncodingInfo(Insn, X86, L.targetArch(), E) ||
      (E.P0 & 31) != 2 || (E.P1 & 0x87) != 7 ||
      E.Opcode != (Rounds     ? 0xcb
                   : Message2 ? 0xcd
                              : 0xcc) ||
      !validateCanonicalVex3RegisterTail(Insn, X86, E) ||
      X86.op_count != (Rounds ? 3 : 2) || X86.avx_sae ||
      X86.avx_rm != X86_AVX_RM_INVALID || X86.encoding.imm_offset ||
      X86.encoding.imm_size)
    return false;
  if ((!Rounds && (E.P1 & 0x78) != 0x78) ||
      !vectorOperand(X86.operands[0], 32, E.regIndex()) ||
      !vectorOperand(X86.operands[Rounds ? 2 : 1], Message2 ? 32 : 16,
                     E.rmIndex()) ||
      (Rounds && !vectorOperand(X86.operands[1], 32, E.vvvvIndex())))
    return false;

  SHA512Builder B(S);
  const NdVar Destination = L.operandWrite(X86.operands[0]);
  const NdVar Other = L.operandRead(S, X86.operands[1]);
  std::array<NdVar, 4> Input, Output;
  for (unsigned I = 0; I < 4; ++I)
    Input[I] = B.lane(Destination, I);
  if (!Rounds) {
    for (unsigned I = 0; I < 4; ++I) {
      const NdVar Next = Message2
                             ? (I < 2 ? B.lane(Other, I + 2) : Output[I - 2])
                             : (I < 3 ? Input[I + 1] : B.lane(Other, 0));
      Output[I] = B.add(Input[I], Message2 ? B.sigma(Next, 19, 61, 6, true)
                                           : B.sigma(Next, 1, 8, 7, true));
    }
  } else {
    // In low-to-high lane order the two state bundles hold H,G,D,C and F,E,B,A.
    std::array<NdVar, 8> State = {
        B.lane(Other, 3), B.lane(Other, 2), Input[3], Input[2],
        B.lane(Other, 1), B.lane(Other, 0), Input[1], Input[0]};
    const NdVar Words = L.operandRead(S, X86.operands[2]);
    for (unsigned Round = 0; Round < 2; ++Round) {
      const NdVar Choice = B.xorValue(
          State[6], B.andValue(State[4], B.xorValue(State[5], State[6])));
      const NdVar Majority =
          B.xorValue(B.andValue(State[0], State[1]),
                     B.andValue(B.xorValue(State[0], State[1]), State[2]));
      const NdVar T1 =
          B.add(B.add(State[7], B.lane(Words, Round)),
                B.add(Choice, B.sigma(State[4], 14, 18, 41, false)));
      const NdVar T2 = B.add(Majority, B.sigma(State[0], 28, 34, 39, false));
      State = {B.add(T1, T2),       State[0], State[1], State[2],
               B.add(State[3], T1), State[4], State[5], State[6]};
    }
    Output = {State[5], State[4], State[1], State[0]};
  }
  const NdVar Low = S.makeTemp(16), High = S.makeTemp(16);
  S.emit(NdOp::CONCAT, Low, {Output[1], Output[0]});
  S.emit(NdOp::CONCAT, High, {Output[3], Output[2]});
  S.emit(NdOp::CONCAT, Destination, {High, Low});
  return true;
}

} // namespace neverd
