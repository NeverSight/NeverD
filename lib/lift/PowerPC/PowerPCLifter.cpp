//===- PowerPCLifter.cpp - Classic PowerPC scalar semantics -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Lift integer operations directly from the original target-endian word,
/// including CR, XER, LR, CTR and update-form effective addresses.
///
//===----------------------------------------------------------------------===//

#include "neverd/lift/PowerPC/PowerPCLifter.h"

#include "../IntegerLiftState.h"

#include "neverd/ArchSupport.h"
#include "neverd/decode/Decoder.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/Support/MathExtras.h"

namespace neverd::ppc {
namespace {
using namespace integerreg::ppc;

struct State : IntegerLiftState {
  using IntegerLiftState::IntegerLiftState;
  NdVar special(uint64_t Offset) { return NdVar::reg(Offset, Width); }
  NdVar cr(unsigned Bit) { return NdVar::reg(crBit(Bit), 1); }
  NdVar bit(NdVar X, unsigned N) {
    return resize(op(NdOp::INT_AND,
                     op(NdOp::INT_RIGHT, X, NdVar::scalar(N, X.Size)),
                     NdVar::scalar(1, X.Size)),
                  1);
  }
  void compare(unsigned Field, NdVar A, NdVar B, bool Signed) {
    NdOp Less = Signed ? NdOp::INT_SLESS : NdOp::INT_LESS;
    emit(Less, cr(Field * 4), {A, B});
    emit(Less, cr(Field * 4 + 1), {B, A});
    emit(NdOp::INT_EQUAL, cr(Field * 4 + 2), {A, B});
    emit(NdOp::COPY, cr(Field * 4 + 3), {bit(special(XER), 31)});
  }
  void result(unsigned D, NdVar V, bool Record) {
    emit(NdOp::COPY, reg(D), {V});
    if (Record)
      compare(0, reg(D), imm(0), true);
  }
  NdVar inverse(NdVar V) {
    return op(
        NdOp::INT_XOR, V,
        NdVar::scalar(llvm::maskTrailingOnes<uint64_t>(V.Size * 8), V.Size));
  }
  void xerBit(unsigned N, NdVar V) {
    NdVar Old = op(NdOp::INT_AND, special(XER), imm(~(uint64_t(1) << N)));
    NdVar New = op(NdOp::INT_LEFT, resize(V, Width), imm(N));
    emit(NdOp::INT_OR, special(XER), {Old, New});
  }
  void overflow(NdVar V) {
    xerBit(30, V);
    xerBit(31, op(NdOp::BOOL_OR, bit(special(XER), 31), V, 1));
  }
};

IntegerControl branch(Arch A, uint32_t W, va_t PC) {
  unsigned Op = W >> 26;
  bool Link = W & 1;
  if (Op == 18 || Op == 16) {
    bool Conditional = Op == 16 && ((W >> 21 & 20) != 20);
    // A conditional call needs a predicated link/call edge.
    if (Conditional && Link)
      return {};
    int64_t Disp = Op == 18 ? llvm::SignExtend64<26>(W & 0x3fffffc)
                            : llvm::SignExtend64<16>(W & 0xfffc);
    va_t Dest = ((W & 2) ? 0 : PC) + Disp;
    return {Link          ? NdOp::CALL
            : Conditional ? NdOp::COND_BR
                          : NdOp::BRANCH,
            isPowerPC64(A) ? Dest : uint32_t(Dest)};
  }
  if (Op != 19 || (W >> 21 & 20) != 20 || (W & 0xe000))
    return {};
  unsigned XO = W >> 1 & 1023;
  if (XO == 16)
    return {Link ? NdOp::INDIR_CALL : NdOp::RETURN};
  if (XO == 528)
    return {Link ? NdOp::INDIR_CALL : NdOp::INDIR_BR};
  return {};
}

NdVar condition(State &S, unsigned BO, unsigned BI) {
  NdVar Cond = NdVar::scalar(1, 1);
  if (!(BO & 4)) {
    NdVar Counter = S.special(CTR);
    S.emit(NdOp::INT_SUB, Counter, {Counter, S.imm(1)});
    Cond = S.op((BO & 2) ? NdOp::INT_EQUAL : NdOp::INT_NOTEQUAL, Counter,
                S.imm(0), 1);
  }
  if (!(BO & 16)) {
    NdVar Flag = S.cr(BI);
    if (!(BO & 8))
      Flag = S.op(NdOp::BOOL_XOR, Flag, NdVar::scalar(1, 1), 1);
    Cond = (BO & 4) ? Flag : S.op(NdOp::BOOL_AND, Cond, Flag, 1);
  }
  return Cond;
}

struct Transfer {
  unsigned Bytes;
  bool Store;
  bool Signed;
  bool Update;
};
std::optional<Transfer> memoryTransfer(unsigned Op, unsigned XO, unsigned DS) {
  if (Op >= 32 && Op <= 45) {
    constexpr unsigned Bytes[] = {4, 1, 4, 1, 2, 2, 2};
    return Transfer{Bytes[(Op - 32) / 2], (Op >= 36 && Op <= 39) || Op >= 44,
                    Op == 42 || Op == 43, bool(Op & 1)};
  }
  if (Op == 58 && DS < 3)
    return Transfer{DS == 2 ? 4u : 8u, false, DS == 2, DS == 1};
  if (Op == 62 && DS < 2)
    return Transfer{8, true, false, DS == 1};
  if (Op != 31)
    return std::nullopt;
  bool Update = XO & 32;
  switch (XO & ~32u) {
  case 23:
    return Transfer{4, false, false, Update};
  case 87:
    return Transfer{1, false, false, Update};
  case 151:
    return Transfer{4, true, false, Update};
  case 215:
    return Transfer{1, true, false, Update};
  case 279:
    return Transfer{2, false, false, Update};
  case 343:
    return Transfer{2, false, true, Update};
  case 407:
    return Transfer{2, true, false, Update};
  case 21:
    return Transfer{8, false, false, Update};
  case 149:
    return Transfer{8, true, false, Update};
  case 341:
    return Transfer{4, false, true, Update}; // LWAX/LWAUX
  default:
    return std::nullopt;
  }
}

uint64_t mask(unsigned Begin, unsigned End, unsigned Bits) {
  uint64_t Left = llvm::maskTrailingOnes<uint64_t>(Bits - Begin);
  uint64_t Right = ~llvm::maskTrailingOnes<uint64_t>(Bits - 1 - End);
  return (Begin <= End ? Left & Right : Left | Right) &
         llvm::maskTrailingOnes<uint64_t>(Bits);
}

NdVar rotate(State &S, NdVar V, NdVar Shift) {
  NdVar CountMask = NdVar::scalar(V.Size * 8 - 1, V.Size);
  NdVar Amount = S.op(NdOp::INT_AND, Shift, CountMask);
  NdVar Complement =
      S.op(NdOp::INT_AND, S.op(NdOp::INT_SUB, NdVar::scalar(0, V.Size), Amount),
           CountMask);
  return S.op(NdOp::INT_OR, S.op(NdOp::INT_LEFT, V, Amount),
              S.op(NdOp::INT_RIGHT, V, Complement));
}

bool integer(State &S, Arch A, uint32_t W) {
  const unsigned Op = W >> 26, T = W >> 21 & 31, RA = W >> 16 & 31,
                 RB = W >> 11 & 31;
  const unsigned XO = W >> 1 & 1023;
  const bool Wide = S.Width == 8, Rc = W & 1;
  const uint64_t SI = llvm::SignExtend64<16>(W & 0xffff);
  NdVar X = S.reg(RA), Y = S.reg(RB), Src = S.reg(T);
  NdVar Base = RA ? X : S.imm(0);
  if (auto Flow = branch(A, W, S.Addr); Flow.Opcode != NdOp::NOP) {
    NdVar Dest;
    if (Op == 19)
      Dest = S.op(NdOp::INT_AND, S.special(XO == 16 ? LR : CTR),
                  S.imm(~uint64_t(3)));
    else
      Dest = NdVar::codeAddress(Flow.Target, S.Width);
    if (Flow.Opcode == NdOp::COND_BR)
      S.emit(NdOp::COND_BR, {}, {Dest, condition(S, T, RA)});
    else {
      if (Rc) {
        va_t Next = S.Addr + 4;
        S.emit(NdOp::COPY, S.special(LR),
               {NdVar::codeAddress(Wide ? Next : uint32_t(Next), S.Width)});
      }
      S.emit(Flow.Opcode, Rc ? S.reg(3) : NdVar{}, {Dest});
    }
    return true;
  }
  if (W == 0x7c0004ac || W == 0x7c2004ac || W == 0x4c00012c ||
      W == 0x7c0006ac) {
    unsigned Key = W == 0x7c0004ac   ? 0
                   : W == 0x7c2004ac ? 1
                   : W == 0x4c00012c ? 2
                                     : 3;
    S.emitVoidIntrinsic(Intrinsic::PPCBarrier, {NdVar::scalar(Key, 1)});
    return true;
  }
  if (Op == 31 && (XO == 20 || XO == 84 || XO == 150 || XO == 214)) {
    bool Double = XO == 84 || XO == 214, Store = XO == 150 || XO == 214;
    if ((Double && !Wide) || Store != Rc)
      return false;
    NdVar Key = NdVar::scalar(unsigned(Store) * 2 + unsigned(Double), 1);
    NdVar EA = S.op(NdOp::INT_ADD, Base, Y);
    if (!Store)
      S.emitIntrinsic(Intrinsic::PPCReservation, Src, {Key, EA});
    else {
      NdVar Success = S.makeTemp(S.Width);
      S.emitIntrinsic(Intrinsic::PPCReservation, Success, {Key, EA, Src});
      S.emit(NdOp::COPY, S.cr(0), {NdVar::scalar(0, 1)});
      S.emit(NdOp::COPY, S.cr(1), {NdVar::scalar(0, 1)});
      S.emit(NdOp::COPY, S.cr(2), {S.resize(Success, 1)});
      S.emit(NdOp::COPY, S.cr(3), {S.bit(S.special(XER), 31)});
    }
    return true;
  }
  if (auto M = memoryTransfer(Op, XO, W & 3)) {
    if ((!Wide && (M->Bytes == 8 || (Op == 58 || Op == 62) ||
                   (Op == 31 && (XO & ~32u) == 341))) ||
        (M->Update && (!RA || (!M->Store && RA == T))) || (Op == 31 && Rc))
      return false;
    uint64_t Disp = (Op == 58 || Op == 62) ? SI & ~uint64_t(3) : SI;
    NdVar EA = Op == 31 ? S.op(NdOp::INT_ADD, Base, Y)
               : Disp   ? S.op(NdOp::INT_ADD, Base, S.imm(Disp))
                        : Base;
    if (M->Store)
      S.emit(NdOp::STORE, {}, {EA, S.resize(Src, M->Bytes)});
    else
      S.result(T, S.resize(S.load(EA, M->Bytes), S.Width, M->Signed), false);
    if (M->Update)
      S.emit(NdOp::COPY, X, {EA});
    return true;
  }
  if (Op == 14 || Op == 15) {
    S.result(T, S.op(NdOp::INT_ADD, Base, S.imm(Op == 15 ? SI << 16 : SI)),
             false);
    return true;
  }
  if (Op >= 24 && Op <= 29) {
    constexpr NdOp Logical[] = {NdOp::INT_OR, NdOp::INT_XOR, NdOp::INT_AND};
    NdVar V = S.op(Logical[(Op - 24) / 2], Src,
                   S.imm(uint64_t(W & 0xffff) << ((Op & 1) ? 16 : 0)));
    S.result(RA, V, Op >= 28);
    return true;
  }
  if (Op == 10 || Op == 11 || (Op == 31 && (XO == 0 || XO == 32) && !Rc)) {
    if ((T & 2) || ((T & 1) && !Wide))
      return false;
    bool Signed = Op == 11 || (Op == 31 && XO == 0);
    unsigned Size = (T & 1) ? 8 : 4;
    NdVar B = Op == 31 ? Y : S.imm(Signed ? SI : (W & 0xffff));
    S.compare(T >> 2, S.resize(X, Size), S.resize(B, Size), Signed);
    return true;
  }
  if (Op == 7) {
    S.result(T, S.op(NdOp::INT_MULT, X, S.imm(SI)), false);
    return true;
  }
  if (Op == 20 || Op == 21 || Op == 23 || (Wide && Op == 30)) {
    unsigned MB = W >> 6 & 31, ME = W >> 1 & 31;
    NdVar V;
    bool Insert = Op == 20;
    if (Op == 30) {
      const unsigned MD = W >> 2 & 7, SH = RB | (W & 2) << 4;
      MB |= W & 32;
      NdVar Shift = S.imm(SH);
      if (MD == 0)
        ME = 63; // RLDICL
      else if (MD == 1) {
        ME = MB;
        MB = 0;
      } // RLDICR
      else if (MD == 2 || MD == 3) {
        ME = 63 - SH;
        Insert = MD == 3;
      } else if ((W >> 1 & 15) == 8) {
        ME = 63;
        Shift = Y;
      } else if ((W >> 1 & 15) == 9) {
        ME = MB;
        MB = 0;
        Shift = Y;
      } else
        return false;
      V = rotate(S, Src, Shift);
    } else {
      V = rotate(S, S.resize(Src, 4),
                 Op == 23 ? S.resize(Y, 4) : NdVar::scalar(RB, 4));
      if (Wide) {
        if (MB > ME) {
          NdVar Repeated = S.makeTemp(8);
          S.emit(NdOp::CONCAT, Repeated, {V, V});
          V = Repeated;
        } else
          V = S.resize(V, 8);
        MB += 32;
        ME += 32;
      }
    }
    uint64_t Mask = mask(MB, ME, S.Width * 8);
    V = S.op(NdOp::INT_AND, V, S.imm(Mask));
    if (Insert)
      V = S.op(NdOp::INT_OR, V, S.op(NdOp::INT_AND, X, S.imm(~Mask)));
    S.result(RA, V, Rc);
    return true;
  }

  if (Op == 19 && !Rc) {
    if (XO == 0 && !(W & 0x0063f800)) { // MCRF
      for (unsigned I = 0; I < 4; ++I)
        S.emit(NdOp::COPY, S.cr((T & ~3u) + I), {S.cr((RA & ~3u) + I)});
      return true;
    }
    NdVar L = S.cr(RA), R = S.cr(RB);
    NdOp Kind;
    switch (XO) {
    case 225:
    case 129:
    case 257:
      Kind = NdOp::BOOL_AND;
      break;
    case 193:
    case 289:
      Kind = NdOp::BOOL_XOR;
      break;
    case 33:
    case 417:
    case 449:
      Kind = NdOp::BOOL_OR;
      break;
    default:
      return false;
    }
    if (XO == 129 || XO == 417)
      R = S.op(NdOp::BOOL_XOR, R, NdVar::scalar(1, 1));
    NdVar Result = S.op(Kind, L, R);
    if (XO == 33 || XO == 225 || XO == 289)
      Result = S.op(NdOp::BOOL_XOR, Result, NdVar::scalar(1, 1));
    S.emit(NdOp::COPY, S.cr(T), {Result});
    return true;
  }

  if (Op == 31 && !Rc && (XO == 339 || XO == 467)) { // MFSPR / MTSPR
    unsigned SPR = RA | RB << 5;
    uint64_t Offset = SPR == 8 ? LR : SPR == 9 ? CTR : SPR == 1 ? XER : 0;
    // ISA 3.0 added CA32 and OV32 to a full XER read. ELFv2 alone does
    // not establish their presence. Arithmetic users of CA/OV/SO are common.
    if (!Offset || (Wide && Offset == XER && XO == 339))
      return false;
    if (XO == 339)
      S.result(T, S.special(Offset), false);
    else
      S.emit(NdOp::COPY, S.special(Offset), {Src});
    return true;
  }
  if (Op == 8 || Op == 12 || Op == 13 ||
      (Op == 31 && ((XO & 511) == 266 || (XO & 511) == 10 || (XO & 511) == 40 ||
                    (XO & 511) == 8))) {
    const bool Sub =
        Op == 8 || (Op == 31 && ((XO & 511) == 40 || (XO & 511) == 8));
    const bool Carry = Op != 31 || (XO & 511) == 10 || (XO & 511) == 8;
    const bool Overflow = Op == 31 && (XO & 512);
    NdVar B = Op == 31 ? Y : S.imm(SI);
    NdVar Result = Sub ? S.op(NdOp::INT_SUB, B, X) : S.op(NdOp::INT_ADD, X, B);
    if (Carry)
      S.xerBit(29, Sub ? S.op(NdOp::INT_LESSEQUAL, X, B, 1)
                       : S.op(NdOp::INT_CARRY, X, B, 1));
    if (Overflow) {
      NdVar V =
          Sub ? S.op(NdOp::INT_SBOR, B, X, 1) : S.op(NdOp::INT_SOVF, X, B, 1);
      S.overflow(V);
    }
    S.result(T, Result, Op == 13 || (Op == 31 && Rc));
    return true;
  }
  if (Op != 31)
    return false;
  const unsigned Arithmetic = XO & 511;
  if (Arithmetic == 136 || Arithmetic == 138 || Arithmetic == 200 ||
      Arithmetic == 202 || Arithmetic == 232 || Arithmetic == 234) {
    // SUBFE/ADDE, SUBFZE/ADDZE and SUBFME/ADDME add the old XER.CA.
    const bool Pair = Arithmetic < 200;
    if (!Pair && RB)
      return false;
    NdVar Left = (Arithmetic & 2) ? X : S.inverse(X);
    NdVar Right = Pair ? Y : S.imm(Arithmetic >= 232 ? UINT64_MAX : 0);
    NdVar Carry = S.resize(S.bit(S.special(XER), 29), S.Width);
    NdVar Partial = S.op(NdOp::INT_ADD, Left, Right);
    NdVar Sum = S.op(NdOp::INT_ADD, Partial, Carry);
    S.xerBit(29, S.op(NdOp::BOOL_OR, S.op(NdOp::INT_CARRY, Left, Right, 1),
                      S.op(NdOp::INT_CARRY, Partial, Carry, 1), 1));
    if (XO & 512) {
      // The sum overflows if both addends' signs differ from its sign.
      NdVar Signs = S.op(NdOp::INT_AND, S.op(NdOp::INT_XOR, Left, Sum),
                         S.op(NdOp::INT_XOR, Right, Sum));
      S.overflow(S.bit(Signs, 8 * S.Width - 1));
    }
    S.result(T, Sum, Rc);
    return true;
  }
  if (XO == 19 && !Rc && !(W & 0x001ff800)) { // MFCR
    NdVar Value = S.imm(0);
    for (unsigned B = 0; B < 32; ++B)
      Value =
          S.op(NdOp::INT_OR, Value,
               S.op(NdOp::INT_LEFT, S.resize(S.cr(B), S.Width), S.imm(31 - B)));
    S.result(T, Value, false);
    return true;
  }
  if (XO == 144 && !Rc && !(W & 0x00100800)) { // MTCRF
    const unsigned Fields = W >> 12 & 255;
    for (unsigned B = 0; B < 32; ++B)
      if (Fields & (0x80 >> (B / 4)))
        S.emit(NdOp::COPY, S.cr(B), {S.bit(Src, 31 - B)});
    return true;
  }
  if ((XO & 31) == 15 && !Rc) { // ISEL uses literal zero for RA=0.
    S.result(T, S.select(S.cr((W >> 6) & 31), Base, Y), false);
    return true;
  }
  NdVar V;
  switch (XO) {
  case 28:
  case 60:
  case 124:
  case 284:
  case 316:
  case 412:
  case 444:
  case 476: {
    NdOp Kind = XO == 28 || XO == 60 || XO == 476 ? NdOp::INT_AND
                : XO == 284 || XO == 316          ? NdOp::INT_XOR
                                                  : NdOp::INT_OR;
    V = S.op(Kind, Src, (XO == 60 || XO == 412) ? S.inverse(Y) : Y);
    if (XO == 124 || XO == 284 || XO == 476)
      V = S.inverse(V);
    break;
  }
  case 922:
  case 954:
  case 986: // EXTSH / EXTSB / EXTSW
    if (RB || (XO == 986 && !Wide))
      return false;
    V = S.resize(S.resize(Src, XO == 954   ? 1
                               : XO == 922 ? 2
                                           : 4),
                 S.Width, true);
    break;
  case 26:
  case 58: { // CNTLZW / CNTLZD
    if (RB || (XO == 58 && !Wide))
      return false;
    NdVar In = S.resize(Src, XO == 26 ? 4 : 8);
    NdVar Count = S.makeTemp(In.Size);
    S.emit(NdOp::LZCOUNT, Count, {In});
    V = S.resize(Count, S.Width);
    break;
  }
  case 24:
  case 536:
  case 27:
  case 539: { // SLW/SRW/SLD/SRD
    unsigned Size = XO == 24 || XO == 536 ? 4 : 8;
    if (Size > S.Width)
      return false;
    NdVar Count = S.op(NdOp::INT_AND, Y, S.imm(Size * 16 - 1));
    NdVar Shift =
        S.resize(S.op(NdOp::INT_AND, Count, S.imm(Size * 8 - 1)), Size);
    NdVar Shifted = S.op(XO < 32 ? NdOp::INT_LEFT : NdOp::INT_RIGHT,
                         S.resize(Src, Size), Shift);
    V = S.select(S.op(NdOp::INT_LESS, Count, S.imm(Size * 8), 1),
                 S.resize(Shifted, S.Width), S.imm(0));
    break;
  }
  case 792:
  case 824:
  case 794:
  case 826:
  case 827: { // SRAW[I] / SRAD[I]
    const bool Double = XO == 794 || XO == 826 || XO == 827;
    if (Double && !Wide)
      return false;
    const unsigned Bytes = Double ? 8 : 4, Bits = Bytes * 8;
    NdVar In = S.resize(Src, Bytes);
    NdVar Count;
    if (XO == 792 || XO == 794)
      Count = S.resize(S.op(NdOp::INT_AND, Y, S.imm(2 * Bits - 1)), Bytes);
    else
      Count = NdVar::scalar(RB | ((XO & 1) << 5), Bytes);
    NdVar Large =
        S.op(NdOp::INT_LESSEQUAL, NdVar::scalar(Bits, Bytes), Count, 1);
    NdVar Bounded = S.select(Large, NdVar::scalar(Bits - 1, Bytes), Count);
    NdVar Shifted = S.op(NdOp::INT_ASHR, In, Bounded);
    NdVar Restored = S.op(NdOp::INT_LEFT, Shifted, Bounded);
    NdVar Lost = S.op(NdOp::BOOL_OR, Large,
                      S.op(NdOp::INT_NOTEQUAL, In, Restored, 1), 1);
    S.xerBit(29, S.op(NdOp::BOOL_AND, Lost, S.bit(In, Bits - 1), 1));
    V = S.resize(Shifted, S.Width, true);
    break;
  }
  case 9:
  case 11:
  case 73:
  case 75: { // MULHDU, MULHWU, MULHD, MULHW
    // Word variants leave the high half of a 64-bit destination undefined.
    if ((XO == 9 || XO == 73) != Wide)
      return false;
    const bool Signed = XO == 73 || XO == 75;
    S.result(T, S.multiplyHigh(X, Y, Signed, Signed), Rc);
    return true;
  }
  case 104:
    if (RB)
      return false;
    S.result(T, S.op(NdOp::INT_SUB, S.imm(0), X), Rc);
    return true;
  case 235:
  case 233: {
    if (XO == 233 && !Wide)
      return false;
    // MULLW on PPC64 leaves the high word undefined.
    if (XO == 235 && Wide)
      return false;
    S.result(T, S.op(NdOp::INT_MULT, X, Y), Rc);
    return true;
  }
  default:
    return false;
  }
  S.result(RA, V, Rc);
  return true;
}
} // namespace

IntegerControl control(Arch A, const cs_insn &I) {
  return I.size == 4
             ? branch(A,
                      readTargetEndian<uint32_t>(I.bytes, archLittleEndian(A)),
                      I.address)
             : IntegerControl{};
}
void lift(Arch A, const cs_insn &I, std::vector<LowOp> &Ops) {
  State S(I.address, I.size, Ops, isPowerPC64(A) ? 8 : 4);
  if (I.size == 4 &&
      integer(S, A, readTargetEndian<uint32_t>(I.bytes, archLittleEndian(A))))
    return;
  Ops.resize(S.OpsStart);
  throw UnliftedInstruction(I.address, I.mnemonic, I.op_str);
}
} // namespace neverd::ppc
