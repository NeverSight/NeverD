//===- MIPSLiftDSPAdditional.cpp - DSP and DSPR2 integer semantics --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/ir/intrinsics/MIPS/MIPSDSP.h"
#include "neverd/lift/MIPS/MIPSLifter.h"
#include "neverd/lift/MIPS/MIPSRegs.h"

namespace neverd {
namespace {
enum class Form {
#define NEVERD_DSP_FORM(Name, Revision, Code, Zero) Name,
#include "neverd/lift/MIPS/MIPSDSPExtra.def"
};
} // namespace

bool MIPSLifter::liftDSPAdditional(State &S, uint32_t Word) const {
  Form Op;
  switch (Word & 0x7ff) {
#define NEVERD_DSP_FORM(Name, Revision, Code, Zero)                            \
  case Code:                                                                   \
    if ((Revision == 2 && !HasDSPR2) || (Word & Zero))                         \
      return false;                                                            \
    Op = Form::Name;                                                           \
    break;
#include "neverd/lift/MIPS/MIPSDSPExtra.def"
  default:
    return false;
  }
  const unsigned RS = Word >> 21 & 31, RT = Word >> 16 & 31;
  const unsigned RD = Word >> 11 & 31, Bank = RD & 3;
  const auto Discard = [&] {
    S.emit(NdOp::NOP, {}, {});
    return true;
  };
  const auto A = S.read(RS, 4), B = S.read(RT, 4);
  const auto Lane = [&](NdVar Source, unsigned I, uint16_t Bytes) {
    if (!I)
      return S.resize(Source, Bytes);
    const auto V = S.makeTemp(Bytes);
    S.emit(NdOp::SUBBYTES, V, {Source, NdVar::cst(I * Bytes, 4)});
    return V;
  };
  const auto Pack = [&](uint16_t Bytes, auto Element) {
    if (Bytes == 4)
      return Element(0);
    const auto Pair = [&](unsigned First) {
      return S.binary(NdOp::CONCAT, Element(First + 1), Element(First),
                      Bytes * 2);
    };
    if (Bytes == 2)
      return Pair(0);
    return S.binary(NdOp::CONCAT, Pair(2), Pair(0), 4);
  };
  const auto ReadControl = [&] {
    const auto C = S.makeTemp(4);
    S.emitIntrinsic(Intrinsic::MIPSReadDSPControl, C);
    return C;
  };
  const auto WriteControl = [&](NdVar C) {
    S.emitIntrinsic(Intrinsic::MIPSWriteDSPControl, {}, {C});
  };
  auto Overflow = NdVar::cst(0, 1);
  const auto Note = [&](NdVar Flag) {
    Overflow = S.binary(NdOp::BOOL_OR, Overflow, Flag, 1);
  };
  const auto Sticky = [&](unsigned Bit) {
    WriteControl(S.binary(
        NdOp::INT_OR, ReadControl(),
        S.binary(NdOp::INT_LEFT, S.resize(Overflow, 4), NdVar::cst(Bit, 4))));
  };
  const auto Clamp = [&](NdVar V, int64_t Min, int64_t Max) {
    const auto L = NdVar::cst(Min, V.Size), H = NdVar::cst(Max, V.Size);
    const auto Below = S.binary(NdOp::INT_SLESS, V, L, 1);
    const auto Above = S.binary(NdOp::INT_SLESS, H, V, 1);
    Note(S.binary(NdOp::BOOL_OR, Below, Above, 1));
    return S.select(Below, L, S.select(Above, H, V));
  };
  switch (Op) {
  case Form::HalveAddQB:
  case Form::HalveSubQB:
  case Form::RoundHalveAddQB:
  case Form::RoundHalveSubQB:
  case Form::HalveAddPH:
  case Form::HalveSubPH:
  case Form::RoundHalveAddPH:
  case Form::RoundHalveSubPH:
  case Form::HalveAddW:
  case Form::HalveSubW:
  case Form::RoundHalveAddW:
  case Form::RoundHalveSubW: {
    if (!RD)
      return Discard();
    const uint16_t Bytes = 1u << ((Word >> 9) & 3);
    const uint16_t Wide = Bytes * 2;
    const auto Result = Pack(Bytes, [&](unsigned I) {
      const auto X = S.resize(Lane(A, I, Bytes), Wide, Bytes != 1);
      const auto Y = S.resize(Lane(B, I, Bytes), Wide, Bytes != 1);
      auto Sum = S.binary((Word & 0x40) ? NdOp::INT_SUB : NdOp::INT_ADD, X, Y);
      if (Word & 0x80)
        Sum = S.binary(NdOp::INT_ADD, Sum, NdVar::cst(1, Wide));
      return S.resize(S.binary(NdOp::INT_ASHR, Sum, NdVar::cst(1, Wide)),
                      Bytes);
    });
    S.write(RD, Result);
    return true;
  }
  case Form::AddU_PH:
  case Form::SubU_PH:
  case Form::AddU_S_PH:
  case Form::SubU_S_PH: {
    const bool Sub = Op == Form::SubU_PH || Op == Form::SubU_S_PH;
    const auto Result = Pack(2, [&](unsigned I) {
      const auto X = Lane(A, I, 2), Y = Lane(B, I, 2);
      const auto Sum = S.binary(Sub ? NdOp::INT_SUB : NdOp::INT_ADD, X, Y);
      const auto Out = S.binary(NdOp::INT_LESS, Sub ? X : Sum, Sub ? Y : X, 1);
      Note(Out);
      return (Word & 0x100)
                 ? S.select(Out, NdVar::cst(Sub ? 0 : UINT16_MAX, 2), Sum)
                 : Sum;
    });
    Sticky(20);
    S.write(RD, Result);
    return true;
  }
  case Form::AbsQ_S_QB: {
    const auto Result = Pack(1, [&](unsigned I) {
      const auto X = Lane(B, I, 1);
      const auto Min = S.binary(NdOp::INT_EQUAL, X, NdVar::cst(0x80, 1), 1);
      Note(Min);
      return S.select(
          Min, NdVar::cst(127, 1),
          S.select(S.binary(NdOp::INT_SLESS, X, NdVar::cst(0, 1), 1),
                   S.binary(NdOp::INT_SUB, NdVar::cst(0, 1), X), X));
    });
    Sticky(20);
    S.write(RD, Result);
    return true;
  }
  case Form::MulPH:
  case Form::MulS_PH:
  case Form::MulQ_S_W:
  case Form::MulQ_RS_W:
  case Form::MulQ_S_PH: {
    const uint16_t Bytes =
        Op == Form::MulQ_S_W || Op == Form::MulQ_RS_W ? 4 : 2;
    const uint16_t Wide = Bytes * 2;
    const auto Result = Pack(Bytes, [&](unsigned I) {
      const auto X = S.resize(Lane(A, I, Bytes), Wide, true);
      const auto Y = S.resize(Lane(B, I, Bytes), Wide, true);
      auto Product = S.binary(NdOp::INT_MULT, X, Y);
      if (Op == Form::MulPH) {
        auto Low = S.resize(Product, Bytes);
        Note(S.binary(NdOp::INT_NOTEQUAL, Product, S.resize(Low, Wide, true),
                      1));
        return Low;
      }
      if (Op != Form::MulS_PH) {
        if (Op == Form::MulQ_RS_W)
          Product = S.binary(NdOp::INT_ADD, Product,
                             NdVar::cst(uint64_t{1} << 30, Wide));
        Product =
            S.binary(NdOp::INT_ASHR, Product, NdVar::cst(Bytes * 8 - 1, Wide));
      }
      const int64_t Sign = int64_t{1} << (Bytes * 8 - 1);
      return S.resize(Clamp(Product, -Sign, Sign - 1), Bytes);
    });
    Sticky(21);
    S.discardHILO();
    S.write(RD, Result);
    return true;
  }
  case Form::CmpGDU_EQ_QB:
  case Form::CmpGDU_LT_QB:
  case Form::CmpGDU_LE_QB: {
    auto Bits = NdVar::cst(0, 4);
    for (unsigned I = 0; I != 4; ++I) {
      const auto X = Lane(A, I, 1), Y = Lane(B, I, 1);
      auto Match = S.binary(Op == Form::CmpGDU_EQ_QB   ? NdOp::INT_EQUAL
                            : Op == Form::CmpGDU_LT_QB ? NdOp::INT_LESS
                                                       : NdOp::INT_LESSEQUAL,
                            X, Y, 1);
      Bits = S.binary(
          NdOp::INT_OR, Bits,
          S.binary(NdOp::INT_LEFT, S.resize(Match, 4), NdVar::cst(I, 4)));
    }
    WriteControl(S.binary(
        NdOp::INT_OR,
        S.binary(NdOp::INT_AND, ReadControl(), NdVar::cst(~0x0f000000u, 4)),
        S.binary(NdOp::INT_LEFT, Bits, NdVar::cst(24, 4))));
    S.write(RD, Bits);
    return true;
  }
  case Form::PackRLPH:
    if (!RD)
      return Discard();
    S.write(RD, S.binary(NdOp::CONCAT, Lane(A, 0, 2), Lane(B, 1, 2), 4));
    return true;
  case Form::PrecrQBPH:
    if (!RD)
      return Discard();
    S.write(RD, Pack(1, [&](unsigned I) {
              return Lane(I >= 2 ? A : B, 2 * (I % 2), 1);
            }));
    return true;
  case Form::PrecrSraPHW:
  case Form::PrecrSraRPHW: {
    if (!RT)
      return Discard();
    const auto Half = [&](NdVar V) {
      if (!RD)
        return S.resize(V, 2);
      V = S.resize(V, 8, true);
      if (Op == Form::PrecrSraRPHW)
        V = S.binary(NdOp::INT_ADD, V, NdVar::cst(uint64_t{1} << (RD - 1), 8));
      return S.resize(S.binary(NdOp::INT_ASHR, V, NdVar::cst(RD, 8)), 2);
    };
    S.write(RT, S.binary(NdOp::CONCAT, Half(B), Half(A), 4));
    return true;
  }
  case Form::ShrlPH:
  case Form::ShrlvPH:
  case Form::ShraQB:
  case Form::ShravQB:
  case Form::ShraRQB:
  case Form::ShravRQB: {
    if (!RD)
      return Discard();
    const bool Half = Op == Form::ShrlPH || Op == Form::ShrlvPH;
    const uint16_t Bytes = Half ? 2 : 1;
    const bool Variable = (Word & 0x80) != 0;
    const bool Round = !Half && (Word & 0x40);
    const auto Count =
        Variable ? S.binary(NdOp::INT_AND, A, NdVar::cst(Bytes * 8 - 1, 4))
                 : NdVar::cst(RS, 4);
    const auto Result = Pack(Bytes, [&](unsigned I) {
      auto X = S.resize(Lane(B, I, Bytes), 4, !Half);
      if (Round)
        X = S.binary(NdOp::INT_LEFT, X, NdVar::cst(1, 4));
      X = S.binary(Half ? NdOp::INT_RIGHT : NdOp::INT_ASHR, X, Count);
      if (Round)
        X = S.binary(NdOp::INT_ASHR,
                     S.binary(NdOp::INT_ADD, X, NdVar::cst(1, 4)),
                     NdVar::cst(1, 4));
      return S.resize(X, Bytes);
    });
    S.write(RD, Result);
    return true;
  }
  case Form::Balign:
  case Form::Append:
  case Form::Prepend: {
    if (!RT)
      return Discard();
    if (Op == Form::Balign && !(RD & 1)) {
      S.emitIntrinsic(Intrinsic::MIPSUnpredictable,
                      NdVar::reg(mipsreg::r(RT), 4));
    } else if (!RD) {
      S.write(RT, B);
    } else {
      const unsigned Shift = Op == Form::Balign ? 8 * RD : RD;
      const auto Mask = NdVar::cst((uint64_t{1} << Shift) - 1, 4);
      const auto X =
          Op == Form::Balign
              ? S.binary(NdOp::INT_RIGHT, A, NdVar::cst(32 - Shift, 4))
          : Op == Form::Append
              ? S.binary(NdOp::INT_AND, A, Mask)
              : S.binary(NdOp::INT_LEFT, A, NdVar::cst(32 - Shift, 4));
      const auto Y =
          S.binary(Op == Form::Prepend ? NdOp::INT_RIGHT : NdOp::INT_LEFT, B,
                   NdVar::cst(Shift, 4));
      S.write(RT, S.binary(NdOp::INT_OR, X, Y));
    }
    return true;
  }
  case Form::DpaWPH:
  case Form::DpsWPH:
  case Form::MulsaWPH:
  case Form::DpaxWPH:
  case Form::DpsxWPH:
  case Form::DpaqxS:
  case Form::DpsqxS:
  case Form::DpaqxSA:
  case Form::DpsqxSA: {
    const bool Cross = Word & 0x200, Fractional = Word & 0x400;
    const auto Product = [&](unsigned I) {
      const auto X = S.resize(Lane(A, I, 2), 8, true);
      const auto Y = S.resize(Lane(B, Cross ? 1 - I : I, 2), 8, true);
      auto P = S.binary(NdOp::INT_MULT, X, Y);
      if (Fractional) {
        P = S.binary(NdOp::INT_LEFT, P, NdVar::cst(1, 8));
        const auto Out =
            S.binary(NdOp::INT_EQUAL, P, NdVar::cst(0x80000000u, 8), 1);
        Note(Out);
        P = S.select(Out, NdVar::cst(INT32_MAX, 8), P);
      }
      return P;
    };
    auto Dot = S.binary(Op == Form::MulsaWPH ? NdOp::INT_SUB : NdOp::INT_ADD,
                        Product(1), Product(0));
    auto Result = S.binary((Word & 0x40) ? NdOp::INT_SUB : NdOp::INT_ADD,
                           S.accumulator(Bank), Dot);
    if (Op == Form::DpaqxSA || Op == Form::DpsqxSA)
      Result = Clamp(Result, INT32_MIN, INT32_MAX);
    if (Fractional)
      Sticky(16 + Bank);
    S.writeAccumulator(Bank, Result);
    return true;
  }
  case Form::Lwx:
  case Form::Lhx:
  case Form::Lbux: {
    const unsigned Opcode = Op == Form::Lwx ? 35 : Op == Form::Lhx ? 33 : 36;
    return liftMemory(S, (Opcode << 26) | (RD << 16),
                      S.binary(NdOp::INT_ADD, S.read(RS), S.read(RT)));
  }
  case Form::Shilo:
  case Form::Shilov: {
    const auto Raw =
        Op == Form::Shilo
            ? NdVar::cst(Word >> 20 & 63, 8)
            : S.resize(S.binary(NdOp::INT_AND, A, NdVar::cst(63, 4)), 8);
    const auto Left = S.binary(NdOp::INT_LESS, NdVar::cst(31, 8), Raw, 1);
    const auto Count =
        S.binary(NdOp::INT_AND, S.binary(NdOp::INT_SUB, NdVar::cst(0, 8), Raw),
                 NdVar::cst(63, 8));
    const auto Acc = S.accumulator(Bank);
    S.writeAccumulator(Bank,
                       S.select(Left, S.binary(NdOp::INT_LEFT, Acc, Count),
                                S.binary(NdOp::INT_RIGHT, Acc, Raw)));
    return true;
  }
  case Form::Insv: {
    if (!RT)
      return Discard();
    const auto C = ReadControl();
    const auto Pos = S.binary(NdOp::INT_AND, C, NdVar::cst(63, 4));
    const auto Size =
        S.binary(NdOp::INT_AND, S.binary(NdOp::INT_RIGHT, C, NdVar::cst(7, 4)),
                 NdVar::cst(63, 4));
    auto Invalid = S.binary(
        NdOp::BOOL_OR, S.binary(NdOp::INT_EQUAL, Size, NdVar::cst(0, 4), 1),
        S.binary(NdOp::INT_LESS, NdVar::cst(32, 4),
                 S.binary(NdOp::INT_ADD, Pos, Size), 1),
        1);
    if (S.Width == 8)
      for (unsigned R : {RS, RT}) {
        const auto NotWord = S.binary(NdOp::INT_NOTEQUAL, S.read(R),
                                      S.resize(S.read(R, 4), 8, true), 1);
        Invalid = S.binary(NdOp::BOOL_OR, Invalid, NotWord, 1);
      }
    const auto Last = S.binary(NdOp::INT_AND,
                               S.binary(NdOp::INT_SUB, NdVar::cst(32, 4), Size),
                               NdVar::cst(31, 4));
    const auto Shift = S.binary(NdOp::INT_AND, Pos, NdVar::cst(31, 4));
    const auto Mask = S.binary(
        NdOp::INT_LEFT,
        S.binary(NdOp::INT_RIGHT, NdVar::cst(UINT32_MAX, 4), Last), Shift);
    const auto Different =
        S.binary(NdOp::INT_XOR, B, S.binary(NdOp::INT_LEFT, A, Shift));
    const auto New =
        S.binary(NdOp::INT_XOR, B, S.binary(NdOp::INT_AND, Mask, Different));
    const auto Result = S.makeTemp(4);
    S.emitIntrinsic(Intrinsic::MIPSExtractResult, Result, {Invalid, New});
    S.write(RT, Result);
    return true;
  }
  case Form::CmpEQPH:
  case Form::CmpLTPH:
  case Form::CmpLEPH:
    S.emitIntrinsic(Op == Form::CmpEQPH   ? Intrinsic::MIPSCmpEqPH
                    : Op == Form::CmpLTPH ? Intrinsic::MIPSCmpLtPH
                                          : Intrinsic::MIPSCmpLePH,
                    {}, {A, B});
    return true;
  case Form::Mthlip:
    S.writeAccumulator(
        Bank,
        S.binary(NdOp::CONCAT,
                 S.resize(NdVar::reg(mipsreg::accumulatorLO(Bank), S.Width), 4),
                 A, 8));
    S.emitIntrinsic(Intrinsic::MIPSMthlipPosition, {});
    return true;
  }
  return false;
}
} // namespace neverd
