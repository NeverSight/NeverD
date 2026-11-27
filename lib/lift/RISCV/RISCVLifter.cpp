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
#include "RISCVOpcode.h"

#include "neverd/decode/Decoder.h"
#include "neverd/ir/intrinsics/IntegerMemory.h"
#include "neverd/ir/intrinsics/RISCV/RISCVCycle.h"

#include "llvm/Support/Endian.h"
#include "llvm/Support/MathExtras.h"

namespace neverd::riscv {
namespace {
using llvm::SignExtend32;
using llvm::SignExtend64;

va_t atXLen(Arch A, va_t Value) {
  return isRISCV32(A) ? uint32_t(Value) : Value;
}

va_t auipcAddress(Arch A, uint32_t Word, va_t PC) {
  return atXLen(A, PC + SignExtend64<32>(Word & 0xfffff000));
}

va_t jalrAddress(Arch A, va_t Base, uint32_t Word) {
  return atXLen(A, Base + SignExtend64<12>(Word >> 20)) & ~va_t(1);
}

std::optional<uint32_t> word(Arch A, RISCVProfile P, const cs_insn &I) {
  if (I.size == 4)
    return llvm::support::endian::read32le(I.bytes);
  if (I.size == 2 && P.hasCompressed())
    return expandCompressed(llvm::support::endian::read16le(I.bytes),
                            isRISCV64(A), P);
  return std::nullopt;
}

IntegerControl branch(Arch A, uint32_t W, va_t PC) {
  const unsigned Op = W & 127, Rd = W >> 7 & 31, F = W >> 12 & 7,
                 Rs = W >> 15 & 31;
  if (Op == opcode::Jalr && !F) {
    if (!Rs) {
      // JALR through x0 has an absolute signed 12-bit destination.
      return {Rd == 1 ? NdOp::CALL : NdOp::BRANCH, jalrAddress(A, 0, W)};
    }
    if (!Rd && Rs == 1 && !(W >> 20))
      return {NdOp::RETURN};
    return {Rd == 1 ? NdOp::INDIR_CALL : NdOp::INDIR_BR};
  }
  va_t Target;
  NdOp Kind;
  if (Op == opcode::Jal) {
    const unsigned J = (W >> 31) << 20 | (W & 0xff000) | (W >> 20 & 1) << 11 |
                       (W >> 21 & 0x3ff) << 1;
    Target = PC + SignExtend32<21>(J);
    Kind = Rd == 1 ? NdOp::CALL : NdOp::BRANCH;
  } else if (Op == opcode::Branch && (F < 2 || F >= 4)) {
    const unsigned B = (W >> 31) << 12 | (W >> 7 & 1) << 11 |
                       (W >> 25 & 63) << 5 | (W >> 8 & 15) << 1;
    Target = PC + SignExtend32<13>(B);
    Kind = NdOp::COND_BR;
  } else
    return {};
  return {Kind, atXLen(A, Target)};
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

bool basicBitManipulation(State &S, uint32_t W, bool Imm, bool Word,
                          unsigned Rd, NdVar X, NdVar Y) {
  const unsigned Unary = W & opcode::UnaryFormMask;
  switch (Unary) {
  case opcode::Ctz:
  case opcode::CtzW:
    S.write(Rd, S.countTrailingZeros(S.resize(X, Word ? 4 : S.Width)));
    return true;
  case opcode::Clz:
  case opcode::ClzW:
  case opcode::Cpop:
  case opcode::CpopW:
    S.write(Rd,
            S.op(Unary == opcode::Clz || Unary == opcode::ClzW ? NdOp::LZCOUNT
                                                               : NdOp::POPCOUNT,
                 S.resize(X, Word ? 4 : S.Width)));
    return true;
  case opcode::SextB:
  case opcode::SextH:
    S.write(Rd, S.resize(X, Unary == opcode::SextB ? 1 : 2), true);
    return true;
  case opcode::ZextHRV32:
  case opcode::ZextHRV64:
    if ((Unary == opcode::ZextHRV64) != (S.Width == 8))
      return false;
    S.write(Rd, S.resize(X, 2));
    return true;
  case opcode::Rev8RV32:
  case opcode::Rev8RV64:
    if ((Unary == opcode::Rev8RV64) != (S.Width == 8))
      return false;
    S.write(Rd, S.byteSwap(X));
    return true;
  case opcode::OrcB: {
    // Adding 0x7f to the low seven bits marks each nonzero byte in bit 7.
    // No byte can carry into its neighbour. Broadcast each mark to its byte.
    constexpr uint64_t ByteOnes = UINT64_MAX / UINT8_MAX;
    NdVar LowBits = S.imm(ByteOnes * (UINT8_MAX >> 1));
    NdVar Mark =
        S.op(NdOp::INT_OR, X,
             S.op(NdOp::INT_ADD, S.op(NdOp::INT_AND, X, LowBits), LowBits));
    NdVar Flags = S.op(NdOp::INT_AND, S.op(NdOp::INT_RIGHT, Mark, S.imm(7)),
                       S.imm(ByteOnes));
    S.write(Rd, S.op(NdOp::INT_MULT, Flags, S.imm(UINT8_MAX)));
    return true;
  }
  }

  const unsigned Key =
      W & (Imm && !Word && S.Width == 8 ? opcode::WideImmediateFormMask
                                        : opcode::RegisterFormMask);
  switch (Key) {
  case opcode::AndN:
  case opcode::OrN:
  case opcode::Xnor:
    S.write(Rd, S.op(Key == opcode::AndN  ? NdOp::INT_AND
                     : Key == opcode::OrN ? NdOp::INT_OR
                                          : NdOp::INT_XOR,
                     X, S.op(NdOp::INT_NOT, Y)));
    return true;
  case opcode::Min:
  case opcode::MinU:
  case opcode::Max:
  case opcode::MaxU: {
    const bool Signed = Key == opcode::Min || Key == opcode::Max;
    const bool Min = Key == opcode::Min || Key == opcode::MinU;
    NdVar Less = S.op(Signed ? NdOp::INT_SLESS : NdOp::INT_LESS, X, Y, 1);
    S.write(Rd, S.select(Less, Min ? X : Y, Min ? Y : X));
    return true;
  }
  case opcode::Rol:
  case opcode::RolW:
  case opcode::Ror:
  case opcode::RorW:
  case opcode::Rori:
  case opcode::RoriW: {
    const unsigned Bytes = Word ? 4 : S.Width;
    NdVar Count = Imm ? NdVar::scalar((W >> 20) & (Bytes * 8 - 1), Bytes)
                      : S.resize(Y, Bytes);
    S.write(Rd,
            S.rotate(S.resize(X, Bytes), Count,
                     Key != opcode::Rol && Key != opcode::RolW),
            Word);
    return true;
  }
  default:
    return false;
  }
}

bool bitManipulation(State &S, RISCVProfile P, uint32_t W, bool Imm, bool Word,
                     unsigned Rd, NdVar X, NdVar Y) {
  const unsigned IndexMask = S.Width * 8 - 1;
  // RV64 encodes shamt[5] in bit 25. It is an opcode bit on RV32 and
  // in register forms on both targets.
  const unsigned Key = W & (Imm && S.Width == 8 ? opcode::WideImmediateFormMask
                                                : opcode::RegisterFormMask);
  const unsigned Shamt = (W >> 20) & IndexMask;
  enum class BitOp { ShiftAdd, ShiftUW, Clear, Extract, Invert, Set };
  BitOp Op;
  unsigned Shift = 0;
  switch (Key) {
#define NEVERD_RISCV_SHIFT_ADD(Name, Encoding, Count)                          \
  case Encoding:                                                               \
    Op = BitOp::ShiftAdd;                                                      \
    Shift = Count;                                                             \
    break;
#define NEVERD_RISCV_SHIFT_UW(Name, Encoding)                                  \
  case Encoding:                                                               \
    Op = BitOp::ShiftUW;                                                       \
    Shift = Shamt;                                                             \
    break;
#define NEVERD_RISCV_SINGLE_BIT(Name, Funct, Kind)                             \
  case Funct | opcode::Op:                                                     \
  case Funct | opcode::OpImm:                                                  \
    Op = BitOp::Kind;                                                          \
    break;
#include "RISCVBitManipulation.def"
  default:
    return false;
  }

  if (Op == BitOp::ShiftAdd || Op == BitOp::ShiftUW) {
    if (!P.Zba)
      return false;
    if (Word)
      X = S.resize(S.resize(X, 4), S.Width);
    if (Shift)
      X = S.op(NdOp::INT_LEFT, X, S.imm(Shift));
    S.write(Rd, Op == BitOp::ShiftAdd ? S.op(NdOp::INT_ADD, X, Y) : X);
    return true;
  }
  if (!P.Zbs)
    return false;
  NdVar N = Imm ? S.imm(Shamt) : S.op(NdOp::INT_AND, Y, S.imm(IndexMask));
  if (Op == BitOp::Extract) {
    S.write(Rd, S.op(NdOp::INT_AND, S.op(NdOp::INT_RIGHT, X, N), S.imm(1)));
  } else {
    NdVar Mask = S.op(NdOp::INT_LEFT, S.imm(1), N);
    if (Op == BitOp::Clear)
      Mask = S.op(NdOp::INT_XOR, Mask, S.imm(-1));
    S.write(Rd, S.op(Op == BitOp::Clear ? NdOp::INT_AND
                     : Op == BitOp::Set ? NdOp::INT_OR
                                        : NdOp::INT_XOR,
                     X, Mask));
  }
  return true;
}

// Floating loads, stores, moves and sign injection transfer the IEEE encoding
// without changing rounding mode or exception flags. Preserve the payload bits.
bool floating(State &S, RISCVProfile P, uint32_t W) {
  const unsigned Op = W & 127, Rd = W >> 7 & 31, F = W >> 12 & 7;
  const unsigned R1 = W >> 15 & 31, R2 = W >> 20 & 31, F7 = W >> 25;
  const unsigned FLen = P.DoubleFloat ? 8 : 4;
  const auto Reg = [FLen](unsigned Index) {
    return NdVar::reg(integerreg::riscv::fpr(Index), FLen);
  };
  const auto Write = [&](unsigned Dest, NdVar Value) {
    if (Value.Size < FLen)
      Value = S.op(NdOp::CONCAT, NdVar::scalar(UINT32_MAX, 4), Value, 8);
    S.emit(NdOp::COPY, Reg(Dest), {Value});
  };
  if (Op == opcode::LoadFP || Op == opcode::StoreFP) {
    if (!((F == 2 && P.SingleFloat) || (F == 3 && P.DoubleFloat)))
      return false;
    const bool Load = Op == opcode::LoadFP;
    const unsigned Bytes = 1u << F;
    const uint64_t Disp = SignExtend64<12>(Load ? W >> 20 : (F7 << 5) | Rd);
    NdVar Address = S.read(R1);
    if (Disp)
      Address = S.op(NdOp::INT_ADD, Address, S.imm(Disp));
    if (Load)
      Write(Rd, S.load(Address, Bytes));
    else
      S.emit(NdOp::STORE, {}, {Address, S.resize(Reg(R2), Bytes)});
    return true;
  }
  const unsigned Format = F7 & 3;
  if (!(Format == 0 ? P.SingleFloat : Format == 1 && P.DoubleFloat))
    return false;
  const unsigned Bytes = Format ? 8 : 4;
  const unsigned Function = F7 >> 2;
  if ((Function == 28 || Function == 30) && !F && !R2 && Bytes <= S.Width) {
    if (Function == 28) // FMV.X.W / FMV.X.D
      S.write(Rd, S.resize(Reg(R1), Bytes), true);
    else // FMV.W.X / FMV.D.X
      Write(Rd, S.resize(S.read(R1), Bytes));
    return true;
  }
  if (Function != 4 || F > 2)
    return false;
  const auto Read = [&](unsigned Source) {
    NdVar Bits = Reg(Source);
    if (Bytes == FLen)
      return Bits;
    // A narrow arithmetic operand with invalid NaN boxing is a canonical NaN.
    NdVar Top = S.makeTemp(4);
    S.emit(NdOp::SUBBYTES, Top, {Bits, NdVar::scalar(4, 4)});
    return S.select(S.op(NdOp::INT_EQUAL, Top, NdVar::scalar(UINT32_MAX, 4), 1),
                    S.resize(Bits, Bytes), NdVar::scalar(0x7fc00000, 4));
  };
  NdVar Left = Read(R1);
  if (R1 == R2) {
    if (F != 0)
      Left = S.op(F == 1 ? NdOp::FLOAT_NEG : NdOp::FLOAT_ABS, Left);
    Write(Rd, Left);
    return true;
  }
  const NdVar SignMask = NdVar::scalar(uint64_t(1) << (Bytes * 8 - 1), Bytes);
  NdVar Signs = Read(R2);
  if (F == 1)
    Signs = S.op(NdOp::INT_NOT, Signs);
  else if (F == 2)
    Signs = S.op(NdOp::INT_XOR, Signs, Left);
  const NdVar Magnitude =
      S.op(NdOp::INT_AND, Left, S.op(NdOp::INT_NOT, SignMask));
  Write(Rd,
        S.op(NdOp::INT_OR, Magnitude, S.op(NdOp::INT_AND, Signs, SignMask)));
  return true;
}

bool integer(State &S, Arch A, RISCVProfile P, uint32_t W) {
  const unsigned Op = W & 127, Rd = W >> 7 & 31, F = W >> 12 & 7;
  const unsigned R1 = W >> 15 & 31, R2 = W >> 20 & 31, F7 = W >> 25;
  if (Op == opcode::LoadFP || Op == opcode::StoreFP || Op == opcode::OpFP)
    return floating(S, P, W);
  const uint64_t Immediate = SignExtend64<12>(W >> 20);
  NdVar X = S.read(R1);
  NdVar Y = S.read(R2);
  if (IntegerControl Flow = branch(A, W, S.Addr); Flow.Opcode != NdOp::NOP) {
    NdVar Dest;
    if (Flow.Target == InvalidVA) {
      Dest = S.op(NdOp::INT_AND, S.op(NdOp::INT_ADD, X, S.imm(Immediate)),
                  S.imm(~uint64_t(1)));
      if (!P.hasCompressed())
        S.emitIntrinsic(Intrinsic::RequireAligned, {},
                        {S.resize(Dest, 8), NdVar::scalar(4, 8)});
    } else {
      if (!P.hasCompressed() && (Flow.Target & 3))
        return false;
      Dest = NdVar::codeAddress(Flow.Target, S.Width);
    }
    if (Op == opcode::Branch) {
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
  if (Op == opcode::Lui || Op == opcode::Auipc) {
    if (Op == opcode::Lui)
      S.write(Rd, S.imm(SignExtend64<32>(W & 0xfffff000)));
    else
      S.write(Rd, NdVar::addressFragment(auipcAddress(A, W, S.Addr), S.Width));
    return true;
  }
  if (Op == opcode::Load || Op == opcode::Store) {
    const bool Store = Op == opcode::Store;
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
  if (Op == opcode::System) {
    if (!P.Zicntr)
      return false;
    Intrinsic Id;
    switch (W & opcode::ExceptRdMask) {
#define NEVERD_RISCV_CYCLE(Name, Value, Word, High, Text)                      \
  case Word:                                                                   \
    Id = Intrinsic::Name;                                                      \
    break;
#include "neverd/ir/intrinsics/RISCV/RISCVCycle.def"
    default:
      return false;
    }
    if (!cycleRead(Id)->admits(A))
      return false;
    S.emitIntrinsic(Id, Rd ? S.reg(Rd) : S.makeTemp(S.Width), {});
    return true;
  }
  if (Op == opcode::Atomic) {
    if (F != 2 && F != 3)
      return false;
    const unsigned Key = (W >> 25) << 1 | (F - 2);
    auto M = integer_memory::form(A, Intrinsic::RISCVAtomic, Key);
    if (!M || (M->Op == integer_memory::Operation::Reserve && R2))
      return false;
    const bool Reservation = M->Op == integer_memory::Operation::Reserve ||
                             M->Op == integer_memory::Operation::Conditional;
    if (!P.Atomic && !(Reservation ? P.Zalrsc : P.Zaamo))
      return false;
    NdVar Out = Rd ? S.reg(Rd) : S.makeTemp(S.Width);
    NdVar Selector = NdVar::scalar(Key, 1);
    if (M->inputs() == 1)
      S.emitIntrinsic(Intrinsic::RISCVAtomic, Out, {Selector, X});
    else
      S.emitIntrinsic(Intrinsic::RISCVAtomic, Out, {Selector, X, Y});
    return true;
  }
  if (Op == opcode::MiscMem && !F && !Rd && !R1 && !(W >> 28)) {
    S.emitVoidIntrinsic(Intrinsic::RISCVFence,
                        {NdVar::scalar((W >> 20) & 255, 1)});
    return true;
  }

  const bool Imm = Op == opcode::OpImm || Op == opcode::OpImm32;
  const bool Word = Op == opcode::OpImm32 || Op == opcode::Op32;
  if ((!Imm && Op != opcode::Op && Op != opcode::Op32) ||
      (Word && S.Width != 8))
    return false;
  if (F7 && P.Zbb && basicBitManipulation(S, W, Imm, Word, Rd, X, Y))
    return true;
  if (F7 && (P.Zba || P.Zbs) && bitManipulation(S, P, W, Imm, Word, Rd, X, Y))
    return true;
  const unsigned Bytes = Word ? 4 : S.Width;
  X = S.resize(X, Bytes);
  Y = Imm ? S.resize(S.imm(Immediate), Bytes) : S.resize(Y, Bytes);
  if (!Imm && F7 == 1) {
    if (!(P.Multiply || (P.Zmmul && F < 4)) || (Word && F > 0 && F < 4))
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
    else if (Function) {
      const unsigned Form = W & opcode::RegisterFormMask;
      if (!P.Zicond || (Form != opcode::CzeroEqz && Form != opcode::CzeroNez))
        return false;
      NdVar Zero = S.imm(0);
      NdOp Test =
          Form == opcode::CzeroEqz ? NdOp::INT_EQUAL : NdOp::INT_NOTEQUAL;
      S.write(Rd, S.select(S.op(Test, Y, Zero, 1), Zero, X));
      return true;
    }
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

std::optional<va_t> pcRelativeCall(Arch A, RISCVProfile P, uint32_t Upper,
                                   const cs_insn &I) {
  if ((Upper & 127) != opcode::Auipc)
    return std::nullopt;
  const unsigned Base = Upper >> 7 & 31;
  const auto W = word(A, P, I);
  if (!Base || !W || branch(A, *W, I.address).Opcode != NdOp::INDIR_CALL ||
      ((*W >> 15) & 31) != Base)
    return std::nullopt;
  return jalrAddress(A, auipcAddress(A, Upper, I.address - 4), *W);
}

IntegerControl control(Arch A, RISCVProfile P, const cs_insn &I) {
  if (auto W = word(A, P, I)) {
    auto Flow = branch(A, *W, I.address);
    if (Flow.Target == InvalidVA || P.hasCompressed() || !(Flow.Target & 3))
      return Flow;
  }
  return {};
}

void lift(Arch A, RISCVProfile P, const cs_insn &I, std::vector<LowOp> &Ops) {
  State S(I.address, I.size, Ops, isRISCV64(A) ? 8 : 4);
  if (auto W = word(A, P, I); W && integer(S, A, P, *W)) {
    if (S.OpsStart == Ops.size())
      S.emit(NdOp::NOP, {}, {});
    return;
  }
  Ops.resize(S.OpsStart);
  throw UnliftedInstruction(I.address, I.mnemonic, I.op_str);
}
} // namespace neverd::riscv
