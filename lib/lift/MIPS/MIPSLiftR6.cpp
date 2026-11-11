//===- MIPSLiftR6.cpp - MIPS32/64 Release 6 encodings -------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/ArchSupport.h"
#include "neverd/lift/MIPS/MIPSLifter.h"
#include "neverd/lift/MIPS/MIPSRegs.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/Support/MathExtras.h"

namespace neverd {

bool MIPSLifter::isR6Compact(uint32_t Word) {
  const unsigned Op = Word >> 26;
  switch (Op) {
  case 6:
  case 7:
  case 22:
  case 23:
    return ((Word >> 16) & 31) != 0;
  case 8:
  case 24:
  case 50:
  case 54:
  case 58:
  case 62:
    return true;
  default:
    return false;
  }
}

MIPSLifter::Encoding MIPSLifter::release6Encoding(uint32_t Word) const {
  const unsigned Op = Word >> 26, Fn = Word & 63, RT = (Word >> 16) & 31;
  // Reused majors must never enter the pre-r6 HI/LO, merge-load, COP1 or
  // branch-likely handlers. Only shared encodings are dispatched to that ISA.
  if (Op == 0 && Fn == 9) {
    if (((Word >> 11) & 31) == 0)
      Word = (Word & ~63u) | 8; // R6 JR is JALR zero,rs.
    return Encoding{Word, 4};
  }
  if ((Op == 1 &&
       (RT <= 1 || ((RT == 16 || RT == 17) && ((Word >> 21) & 31) == 0))) ||
      (Op >= 2 && Op <= 5) || ((Op == 6 || Op == 7) && !RT))
    return Encoding{Word, 4};
  if (Op == 0 && (Fn <= 4 || Fn == 6 || Fn == 7 || Fn == 15 || Fn == 20 ||
                  Fn == 22 || Fn == 23 || (Fn >= 32 && Fn <= 47) || Fn >= 56))
    return Encoding{Word};
  if ((Op >= 9 && Op <= 14) || Op == 25 || Op == 32 || Op == 33 || Op == 35 ||
      Op == 36 || Op == 37 || Op == 39 || Op == 40 || Op == 41 || Op == 43 ||
      Op == 55 || Op == 63)
    return Encoding{Word};
  return Encoding{.Word = Word, .R6 = true};
}

uint64_t MIPSLifter::r6BranchTarget(uint32_t Word, uint64_t PC) const {
  const unsigned Op = Word >> 26;
  const int64_t Offset = Op == 50 || Op == 58   ? llvm::SignExtend64<26>(Word)
                         : Op == 54 || Op == 62 ? llvm::SignExtend64<21>(Word)
                                                : int16_t(Word);
  const uint64_t Address = PC + 4 + Offset * 4;
  return isMIPS64(Target) ? Address : uint32_t(Address);
}

bool MIPSLifter::isR6ConditionalCompact(uint32_t Branch) {
  const unsigned Op = Branch >> 26;
  return isR6Compact(Branch) && Op != 50 && Op != 58 &&
         !((Op == 54 || Op == 62) && !(Branch & 0x03e00000));
}

std::optional<bool>
MIPSLifter::r6ForbiddenFollower(llvm::ArrayRef<uint8_t> Code, bool Little) {
  if (Code.size() < 4)
    return std::nullopt;
  const auto Branch = readTargetEndian<uint32_t>(Code.data(), Little);
  if (!isR6ConditionalCompact(Branch))
    return false;
  if (Code.size() < 8)
    return std::nullopt;
  const auto Next = readTargetEndian<uint32_t>(Code.data() + 4, Little);
  const unsigned Op = Next >> 26, RT = (Next >> 16) & 31,
                 RS = (Next >> 21) & 31;
  return isR6Compact(Next) || (Op >= 2 && Op <= 7) ||
         (Op == 0 && (Next & 63) == 9) ||
         (Op == 1 && (RT <= 1 || ((RT == 16 || RT == 17) && !RS))) ||
         ((Op == 17 || Op == 18) && (RS == 9 || RS == 13)) || Next == 0x140 ||
         Next == 0x42000018 || Next == 0x42000058 || Next == 0x4200001f ||
         (Next & 0xffe0003f) == 0x42000020;
}

bool MIPSLifter::liftR6Compact(State &S, uint32_t Word,
                               bool ForbiddenFollower) const {
  const unsigned Op = Word >> 26, RS = (Word >> 21) & 31,
                 RT = (Word >> 16) & 31;
  const bool Indirect = (Op == 54 || Op == 62) && !RS;
  const auto Zero = NdVar::cst(0, S.Width);
  NdVar TargetValue =
      Indirect ? S.binary(NdOp::INT_ADD, S.read(RT),
                          NdVar::cst(int64_t(int16_t(Word)), S.Width))
               : NdVar::cst(r6BranchTarget(Word, S.Addr), S.Width);
  const auto Next =
      NdVar::cst(S.Width == 4 ? uint32_t(S.Addr + 4) : S.Addr + 4, S.Width);
  std::optional<NdVar> Predicate;
  bool Link = Op == 58 || (Indirect && Op == 62);
  if (Op == 54 || Op == 62) {
    if (!Indirect)
      Predicate = S.binary(Op == 54 ? NdOp::INT_EQUAL : NdOp::INT_NOTEQUAL,
                           S.read(RS), Zero, 1);
  } else if (Op == 8 || Op == 24) {
    const bool Equal = Op == 8;
    if (RS >=
        RT) { // BOVC / BNVC: either invalid word input also counts as overflow.
      const auto A = S.read(RS, 4), B = S.read(RT, 4);
      const auto Sum =
          S.binary(NdOp::INT_ADD, S.resize(A, 8, true), S.resize(B, 8, true));
      auto Overflow = S.binary(NdOp::INT_NOTEQUAL, Sum,
                               S.resize(S.resize(Sum, 4), 8, true), 1);
      if (S.Width == 8)
        for (auto R : {RS, RT})
          Overflow = S.binary(NdOp::BOOL_OR, Overflow,
                              S.binary(NdOp::INT_NOTEQUAL, S.read(R),
                                       S.resize(S.read(R, 4), 8, true), 1),
                              1);
      Predicate = S.binary(Equal ? NdOp::INT_NOTEQUAL : NdOp::INT_EQUAL,
                           Overflow, NdVar::cst(0, 1), 1);
    } else {
      Link = !RS;
      Predicate = S.binary(Equal ? NdOp::INT_EQUAL : NdOp::INT_NOTEQUAL,
                           S.read(RS), S.read(RT), 1);
    }
  } else if (Op == 6 || Op == 7 || Op == 22 || Op == 23) {
    if (!RS || RS == RT) {
      Link = Op == 6 || Op == 7;
      const bool Less = (Op & 1) != 0;
      const auto V = S.read(RT);
      if (!RS)
        Predicate = Less ? S.binary(NdOp::INT_SLESS, Zero, V, 1)
                         : S.binary(NdOp::INT_SLESSEQUAL, V, Zero, 1);
      else
        Predicate = Less ? S.binary(NdOp::INT_SLESS, V, Zero, 1)
                         : S.binary(NdOp::INT_SLESSEQUAL, Zero, V, 1);
    } else {
      const bool Signed = Op >= 22;
      Predicate = (Op & 1) ? S.binary(Signed ? NdOp::INT_SLESS : NdOp::INT_LESS,
                                      S.read(RS), S.read(RT), 1)
                           : S.binary(Signed ? NdOp::INT_SLESSEQUAL
                                             : NdOp::INT_LESSEQUAL,
                                      S.read(RT), S.read(RS), 1);
    }
  }
  if (Link && Predicate && RT == 31)
    return false; // conditional link must be restartable
  if (Link)
    S.write(31, Next);
  if (ForbiddenFollower)
    S.emitIntrinsic(Intrinsic::MIPSRequireTakenBranch, {}, {*Predicate});
  if (Link) {
    if (Predicate)
      S.emit(
          NdOp::COND_BR, {},
          {Next, S.binary(NdOp::INT_EQUAL, *Predicate, NdVar::cst(0, 1), 1)});
    S.emit(Indirect ? NdOp::INDIR_CALL : NdOp::CALL,
           NdVar::reg(mipsreg::V0, S.Width), {TargetValue});
  } else if (Predicate) {
    S.emit(NdOp::COND_BR, {}, {TargetValue, *Predicate});
  } else {
    S.emit(!Indirect            ? NdOp::BRANCH
           : Word == 0xd81f0000 ? NdOp::RETURN
                                : NdOp::INDIR_BR,
           {}, {TargetValue});
  }
  return true;
}

bool MIPSLifter::liftRelease6(State &S, uint32_t Word) const {
  const unsigned Op = Word >> 26, RS = (Word >> 21) & 31,
                 RT = (Word >> 16) & 31;
  const unsigned RD = (Word >> 11) & 31, SA = (Word >> 6) & 31, Fn = Word & 63;
  const bool Wide = S.Width == 8;

  if (Op == 15 || (Op == 29 && Wide && RS) ||
      (Op == 1 && Wide && (RT == 6 || RT == 30))) {
    const unsigned Shift = Op != 1 ? 16 : RT == 6 ? 32 : 48;
    const uint16_t Bytes = Op == 15 ? 4 : 8;
    S.write(
        Op == 1 ? RS : RT,
        S.binary(NdOp::INT_ADD, S.read(RS, Bytes),
                 NdVar::cst(uint64_t(int64_t(int16_t(Word))) << Shift, Bytes)));
    return true;
  }
  if (Op == 59) { // PCREL
    const unsigned Kind = (Word >> 19) & 3;
    uint64_t Address = S.Addr;
    unsigned Bytes = 0;
    if (Kind < 3) {
      if (Kind == 2 && !Wide)
        return false;
      Address += llvm::SignExtend64<19>(Word) * 4;
      Bytes = Kind ? 4 : 0;
    } else if (((Word >> 18) & 7) == 6 && Wide) {
      Address = (Address & ~uint64_t(7)) + llvm::SignExtend64<18>(Word) * 8;
      Bytes = 8;
    } else if (RT == 30 || RT == 31) {
      Address += int64_t(int16_t(Word)) * 65536;
      if (RT == 31)
        Address &= ~uint64_t(0xffff);
    } else
      return false;
    NdVar Value = NdVar::cst(Wide ? Address : uint32_t(Address), S.Width);
    if (Bytes) {
      const auto Loaded = S.makeTemp(Bytes);
      S.emit(NdOp::LOAD, Loaded, {Value});
      Value = Loaded;
    }
    S.write(RS, Value, Kind != 2);
    return true;
  }
  if (Op == 31) {
    const uint16_t Bytes = Fn == 36 ? 8 : 4;
    if ((Fn == 32 || Fn == 36) && Bytes <= S.Width) {
      const unsigned BP = SA & (Bytes - 1);
      if (SA == 8 + BP) { // ALIGN: (rt << 8*bp) | (rs >> 8*(bytes-bp)).
        auto V = S.read(RT, Bytes);
        if (BP)
          V = S.binary(NdOp::INT_OR,
                       S.binary(NdOp::INT_LEFT, V, NdVar::cst(8 * BP, Bytes)),
                       S.binary(NdOp::INT_RIGHT, S.read(RS, Bytes),
                                NdVar::cst(8 * (Bytes - BP), Bytes)));
        S.write(RD, V);
        return true;
      }
      if (!SA && !RS) {
        auto V = S.read(RT, Bytes);
        for (unsigned N : {1u, 2u, 4u}) {
          const auto Mask = NdVar::cst(UINT64_MAX / ((1u << N) + 1), Bytes);
          V = S.binary(
              NdOp::INT_OR,
              S.binary(NdOp::INT_LEFT, S.binary(NdOp::INT_AND, V, Mask),
                       NdVar::cst(N, Bytes)),
              S.binary(NdOp::INT_AND,
                       S.binary(NdOp::INT_RIGHT, V, NdVar::cst(N, Bytes)),
                       Mask));
        }
        S.write(RD, V);
        return true;
      }
    }
    return liftBitfield(S, Word);
  }
  if (Op != 0)
    return false;
  if ((Fn == 53 || Fn == 55) && !SA) {
    const auto Cond = S.binary(Fn == 53 ? NdOp::INT_EQUAL : NdOp::INT_NOTEQUAL,
                               S.read(RT), NdVar::cst(0, S.Width), 1);
    S.write(RD, S.select(Cond, S.read(RS), NdVar::cst(0, S.Width)));
    return true;
  }
  if ((Fn == 5 || (Fn == 21 && Wide)) && SA < 4) {
    const uint16_t Bytes = Fn == 5 ? 4 : 8;
    S.write(RD, S.binary(NdOp::INT_ADD,
                         S.binary(NdOp::INT_LEFT, S.read(RS, Bytes),
                                  NdVar::cst(SA + 1, Bytes)),
                         S.read(RT, Bytes)));
    return true;
  }
  if (Fn >= 16 && Fn <= 19 && SA == 1 && !RT) {
    const uint16_t Bytes = Fn < 18 ? 4 : 8;
    if (Bytes > S.Width)
      return false;
    if (Bytes == 4)
      S.requireWord(RS);
    auto X = S.read(RS, Bytes);
    if (Fn & 1) {
      const auto V = S.makeTemp(Bytes);
      S.emit(NdOp::INT_NOT, V, {X});
      X = V;
    }
    const auto Count = S.makeTemp(Bytes);
    S.emit(NdOp::LZCOUNT, Count, {X});
    S.write(RD, Count);
    return true;
  }
  if (Fn < 24 || Fn > 31 || (SA != 2 && SA != 3))
    return false;
  const uint16_t Bytes = Fn & 4 ? 8 : 4;
  if (Bytes > S.Width)
    return false;
  const bool Signed = !(Fn & 1), High = SA == 3;
  const auto A = S.read(RS, Bytes), B = S.read(RT, Bytes);
  if (Bytes == 4 && Wide && (High || (Fn & 2))) {
    for (unsigned R : {RS, RT}) {
      if (Signed) {
        S.requireWord(R);
        continue;
      }
      const auto Input = S.read(R), Low = S.read(R, 4);
      const auto ZExt = S.binary(NdOp::INT_EQUAL, Input, S.resize(Low, 8), 1);
      S.emitIntrinsic(Intrinsic::MIPSRequireWord, {},
                      {S.select(ZExt, S.resize(Low, 8, true), Input)});
    }
  }
  if (!(Fn & 2)) {
    if (!High) {
      S.write(RD, S.binary(NdOp::INT_MULT, A, B));
      return true;
    }
    const auto Product =
        S.binary(NdOp::INT_MULT, S.resize(A, Bytes * 2, Signed),
                 S.resize(B, Bytes * 2, Signed));
    const auto V = S.makeTemp(Bytes);
    S.emit(NdOp::SUBBYTES, V, {Product, NdVar::cst(Bytes, 4)});
    S.write(RD, V);
    return true;
  }
  const auto Zero = S.binary(NdOp::INT_EQUAL, B, NdVar::cst(0, Bytes), 1);
  auto Avoid = Zero;
  if (Signed) {
    const auto Min =
        S.binary(NdOp::INT_EQUAL, A,
                 NdVar::cst(uint64_t(1) << (Bytes * 8 - 1), Bytes), 1);
    Avoid = S.binary(
        NdOp::BOOL_OR, Zero,
        S.binary(NdOp::BOOL_AND, Min,
                 S.binary(NdOp::INT_EQUAL, B, NdVar::cst(UINT64_MAX, Bytes), 1),
                 1),
        1);
  }
  // MIN/-1 has the low quotient MIN and remainder zero. Replacing that
  // divisor with +1 gives both results without host or LLVM divide overflow.
  const auto Safe = S.select(Avoid, NdVar::cst(1, Bytes), B);
  const auto Value = S.binary(High ? (Signed ? NdOp::INT_SREM : NdOp::INT_REM)
                                   : (Signed ? NdOp::INT_SDIV : NdOp::INT_DIV),
                              A, Safe);
  const auto Result = S.makeTemp(8);
  S.emitIntrinsic(Intrinsic::MIPSRelease6DivideResult, Result,
                  {Zero, S.resize(Value, 8, true)});
  S.write(RD, S.resize(Result, Bytes));
  return true;
}
} // namespace neverd
