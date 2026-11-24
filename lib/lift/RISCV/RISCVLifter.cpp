//===- RISCVLifter.cpp - RISC-V integer semantics ---------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Lift physical I/M/A instructions and integer C expansions. All target
/// arithmetic wraps at XLEN.
///
//===----------------------------------------------------------------------===//

#include "neverd/lift/RISCV/RISCVLifter.h"

#include "../IntegerLiftState.h"
#include "RISCVCompressed.h"

#include "neverd/decode/Decoder.h"
#include "neverd/ir/intrinsics/IntegerMemory.h"

#include "llvm/Support/Endian.h"
#include "llvm/Support/MathExtras.h"

namespace neverd::riscv {
namespace {
using llvm::SignExtend32;
using llvm::SignExtend64;

std::optional<uint32_t> word(Arch A, RISCVProfile P, const cs_insn &I) {
  if (I.size == 4)
    return llvm::support::endian::read32le(I.bytes);
  if (I.size == 2 && P.Compressed)
    return expandCompressed(llvm::support::endian::read16le(I.bytes),
                            A == Arch::RISCV64);
  return std::nullopt;
}

IntegerControl branch(Arch A, uint32_t W, va_t PC) {
  const unsigned Op = W & 127, Rd = W >> 7 & 31, F = W >> 12 & 7,
                 Rs = W >> 15 & 31;
  if (Op == 0x67 && !F) {
    if (!Rd && Rs == 1 && !(W >> 20))
      return {NdOp::RETURN};
    return {Rd == 1 ? NdOp::INDIR_CALL : NdOp::INDIR_BR};
  }
  va_t Target;
  NdOp Kind;
  if (Op == 0x6f) {
    const unsigned J = (W >> 31) << 20 | (W & 0xff000) | (W >> 20 & 1) << 11 |
                       (W >> 21 & 0x3ff) << 1;
    Target = PC + SignExtend32<21>(J);
    Kind = Rd == 1 ? NdOp::CALL : NdOp::BRANCH;
  } else if (Op == 0x63 && (F < 2 || F >= 4)) {
    const unsigned B = (W >> 31) << 12 | (W >> 7 & 1) << 11 |
                       (W >> 25 & 63) << 5 | (W >> 8 & 15) << 1;
    Target = PC + SignExtend32<13>(B);
    Kind = NdOp::COND_BR;
  } else
    return {};
  return {Kind, A == Arch::RISCV32 ? uint32_t(Target) : Target};
}

struct State : IntegerLiftState {
  using IntegerLiftState::IntegerLiftState;
  NdVar read(unsigned Rs) { return Rs ? reg(Rs) : imm(0); }
  void write(unsigned Rd, NdVar V, bool Signed = false) {
    if (Rd)
      emit(NdOp::COPY, reg(Rd), {resize(V, Width, Signed)});
  }
};

NdVar mulDiv(State &S, unsigned F, NdVar A, NdVar B) {
  if (!F)
    return S.op(NdOp::INT_MULT, A, B);
  if (F <= 3)
    return S.multiplyHigh(A, B, F != 3, F == 1);
  const bool Signed = !(F & 1), Rem = F >= 6;
  NdVar Zero = S.op(NdOp::INT_EQUAL, B, NdVar::scalar(0, A.Size), 1);
  const NdVar Ones =
      NdVar::scalar(llvm::maskTrailingOnes<uint64_t>(A.Size * 8), A.Size);
  NdVar Exceptional = Zero;
  if (Signed) {
    NdVar Min = NdVar::scalar(uint64_t(1) << (A.Size * 8 - 1), A.Size);
    NdVar Overflow = S.op(NdOp::BOOL_AND, S.op(NdOp::INT_EQUAL, A, Min, 1),
                          S.op(NdOp::INT_EQUAL, B, Ones, 1), 1);
    Exceptional = S.op(NdOp::BOOL_OR, Zero, Overflow, 1);
  }
  // Keep even the unselected LLVM division free of zero and signed overflow.
  NdVar Divisor = S.select(Exceptional, NdVar::scalar(1, A.Size), B);
  NdOp Op = Rem ? (Signed ? NdOp::INT_SREM : NdOp::INT_REM)
                : (Signed ? NdOp::INT_SDIV : NdOp::INT_DIV);
  return S.select(Zero, Rem ? A : Ones, S.op(Op, A, Divisor));
}

bool integer(State &S, Arch A, RISCVProfile P, uint32_t W) {
  const unsigned Op = W & 127, Rd = W >> 7 & 31, F = W >> 12 & 7;
  const unsigned R1 = W >> 15 & 31, R2 = W >> 20 & 31, F7 = W >> 25;
  const uint64_t Immediate = SignExtend64<12>(W >> 20);
  NdVar X = S.read(R1);
  NdVar Y = S.read(R2);
  if (IntegerControl Flow = branch(A, W, S.Addr); Flow.Opcode != NdOp::NOP) {
    NdVar Dest;
    if (Op == 0x67) {
      Dest = S.op(NdOp::INT_AND, S.op(NdOp::INT_ADD, X, S.imm(Immediate)),
                  S.imm(~uint64_t(1)));
      if (!P.Compressed)
        S.emitIntrinsic(Intrinsic::RequireAligned, {},
                        {S.resize(Dest, 8), NdVar::scalar(4, 8)});
    } else {
      if (!P.Compressed && (Flow.Target & 3))
        return false;
      Dest = NdVar::codeAddress(Flow.Target, S.Width);
    }
    if (Op == 0x63) {
      NdOp Cmp = F == 0   ? NdOp::INT_EQUAL
                 : F == 1 ? NdOp::INT_NOTEQUAL
                 : F == 4 ? NdOp::INT_SLESS
                 : F == 5 ? NdOp::INT_SLESSEQUAL
                 : F == 6 ? NdOp::INT_LESS
                          : NdOp::INT_LESSEQUAL;
      bool Swap = F == 5 || F == 7;
      S.emit(NdOp::COND_BR, {},
             {Dest, S.op(Cmp, Swap ? Y : X, Swap ? X : Y, 1)});
      return true;
    }
    const va_t Next = S.Addr + S.InsnSize;
    S.write(Rd,
            NdVar::codeAddress(S.Width == 4 ? uint32_t(Next) : Next, S.Width));
    const bool Call =
        Flow.Opcode == NdOp::CALL || Flow.Opcode == NdOp::INDIR_CALL;
    S.emit(Flow.Opcode, Call ? S.reg(10) : NdVar{}, {Dest});
    return true;
  }
  if (Op == 0x37 || Op == 0x17) {
    const uint64_t Upper = SignExtend64<32>(W & 0xfffff000);
    if (Op == 0x37)
      S.write(Rd, S.imm(Upper));
    else {
      const va_t Addr = S.Addr + Upper;
      S.write(Rd, NdVar::addressFragment(S.Width == 4 ? uint32_t(Addr) : Addr,
                                         S.Width));
    }
    return true;
  }
  if (Op == 0x03 || Op == 0x23) {
    const bool Store = Op == 0x23;
    unsigned Bytes = 1u << (F & 3);
    if ((Store && F >= 4) || F == 7 || Bytes > S.Width ||
        (F == 6 && S.Width == 4))
      return false;
    uint64_t Disp = Store ? SignExtend64<12>((F7 << 5) | Rd) : Immediate;
    NdVar EA = Disp ? S.op(NdOp::INT_ADD, X, S.imm(Disp)) : X;
    if (Store)
      S.emit(NdOp::STORE, {}, {EA, S.resize(Y, Bytes)});
    else
      S.write(Rd, S.load(EA, Bytes), F < 4);
    return true;
  }
  if (Op == 0x2f) {
    if (!P.Atomic || (F != 2 && F != 3))
      return false;
    const unsigned Key = (W >> 25) << 1 | (F - 2);
    auto M = integer_memory::form(A, Intrinsic::RISCVAtomic, Key);
    if (!M || (M->Op == integer_memory::Operation::Reserve && R2))
      return false;
    NdVar Out = Rd ? S.reg(Rd) : S.makeTemp(S.Width);
    NdVar Selector = NdVar::scalar(Key, 1);
    if (M->inputs() == 1)
      S.emitIntrinsic(Intrinsic::RISCVAtomic, Out, {Selector, X});
    else
      S.emitIntrinsic(Intrinsic::RISCVAtomic, Out, {Selector, X, Y});
    return true;
  }
  if (Op == 0x0f && !F && !Rd && !R1 && !(W >> 28)) {
    S.emitVoidIntrinsic(Intrinsic::RISCVFence,
                        {NdVar::scalar((W >> 20) & 255, 1)});
    return true;
  }

  const bool Imm = Op == 0x13 || Op == 0x1b;
  const bool Word = Op == 0x1b || Op == 0x3b;
  if ((!Imm && Op != 0x33 && Op != 0x3b) || (Word && S.Width != 8))
    return false;
  const unsigned Bytes = Word ? 4 : S.Width;
  X = S.resize(X, Bytes);
  Y = Imm ? S.resize(S.imm(Immediate), Bytes) : S.resize(Y, Bytes);
  if (!Imm && F7 == 1) {
    if (!P.Multiply || (Word && F > 0 && F < 4))
      return false;
    S.write(Rd, mulDiv(S, F, X, Y), Word);
    return true;
  }
  if (Word && F != 0 && F != 1 && F != 5)
    return false;
  const bool Shift = F == 1 || F == 5;
  bool Alternate = false;
  if (!Imm || Shift) {
    unsigned Function = W >> 25;
    if (Imm && Bytes == 8)
      Function &= ~1u;
    if (Function == 32 && (F == 5 || (F == 0 && !Imm)))
      Alternate = true;
    else if (Function)
      return false;
  }
  constexpr NdOp ALU[] = {NdOp::INT_ADD,  NdOp::INT_LEFT, NdOp::INT_SLESS,
                          NdOp::INT_LESS, NdOp::INT_XOR,  NdOp::INT_RIGHT,
                          NdOp::INT_OR,   NdOp::INT_AND};
  NdOp Kind = Alternate ? (F == 0 ? NdOp::INT_SUB : NdOp::INT_ASHR) : ALU[F];
  if (Shift) {
    NdVar Mask = NdVar::scalar(Bytes * 8 - 1, Bytes);
    Y = Imm ? NdVar::scalar((W >> 20) & Mask.Offset, Bytes)
            : S.op(NdOp::INT_AND, Y, Mask);
  }
  S.write(Rd, S.op(Kind, X, Y, F == 2 || F == 3 ? 1 : Bytes), Word);
  return true;
}
} // namespace

IntegerControl control(Arch A, RISCVProfile P, const cs_insn &I) {
  if (auto W = word(A, P, I)) {
    auto Flow = branch(A, *W, I.address);
    if (Flow.Target == InvalidVA || P.Compressed || !(Flow.Target & 3))
      return Flow;
  }
  return {};
}

void lift(Arch A, RISCVProfile P, const cs_insn &I, std::vector<LowOp> &Ops) {
  State S(I.address, I.size, Ops, A == Arch::RISCV64 ? 8 : 4);
  if (auto W = word(A, P, I); W && integer(S, A, P, *W)) {
    if (S.OpsStart == Ops.size())
      S.emit(NdOp::NOP, {}, {});
    return;
  }
  Ops.resize(S.OpsStart);
  throw UnliftedInstruction(I.address, I.mnemonic, I.op_str);
}
} // namespace neverd::riscv
