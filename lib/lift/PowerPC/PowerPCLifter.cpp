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
#include "neverd/ir/intrinsics/IntegerMemory.h"
#include "neverd/ir/intrinsics/PowerPC/PowerPCDivision.h"
#include "neverd/lift/PowerPC/PowerPCPrefixed.h"
#include "neverd/loader/ELF/PowerPCABI.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/Support/MathExtras.h"

namespace neverd::ppc {
namespace {
using namespace integerreg::ppc;

namespace opcode {
enum : unsigned {
#define NEVERD_PPC_OPCODE(Name, Value) Name = Value,
#include "PowerPCIntegerOpcodes.def"
};
} // namespace opcode

namespace branch_option {
#include "PowerPCBranchOptions.inc"
} // namespace branch_option

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
    emit(Less, cr(Field * 4 + CRLT), {A, B});
    emit(Less, cr(Field * 4 + CRGT), {B, A});
    emit(NdOp::INT_EQUAL, cr(Field * 4 + CREQ), {A, B});
    emit(NdOp::COPY, cr(Field * 4 + CRSO), {bit(special(XER), XERSO)});
  }
  void result(unsigned D, NdVar V, bool Record) {
    emit(NdOp::COPY, reg(D), {V});
    if (Record)
      compare(0, reg(D), imm(0), true);
  }
  void transfer(unsigned T, NdVar EA, unsigned Bytes, bool Store, bool Signed,
                bool Reversed = false) {
    NdVar V = Store ? resize(reg(T), Bytes) : load(EA, Bytes);
    if (Reversed)
      V = byteSwap(V);
    if (Store)
      emit(NdOp::STORE, {}, {EA, V});
    else
      result(T, resize(V, Width, Signed), false);
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
    xerBit(XEROV, V);
    xerBit(XERSO, op(NdOp::BOOL_OR, bit(special(XER), XERSO), V, 1));
  }
  void fixedPointResult(unsigned T, Intrinsic Id, NdVar X, NdVar Y,
                        bool Record) {
    emitIntrinsic(Id, reg(T), {X, Y});
    if (!Record)
      return;
    // The recorded comparison belongs to this execution. Its three bits and
    // the upper GPR word may all be unspecified in 64-bit mode.
    const NdVar CR = makeTemp(Width);
    emit(NdOp::COPY, CR, {makeTemp(Width)});
    for (unsigned I = 0; I != CRSO; ++I)
      emit(NdOp::COPY, cr(I), {bit(CR, crShift(I))});
    emit(NdOp::COPY, cr(CRSO), {bit(special(XER), XERSO)});
  }
};

bool testsCondition(unsigned BO) {
  using namespace branch_option;
  return (BO & (IgnoreCR | IgnoreCTR)) != (IgnoreCR | IgnoreCTR);
}

va_t branchAddress(Arch A, va_t Address) {
  return isPowerPC64(A) ? Address : uint32_t(Address);
}

IntegerControl branch(Arch A, uint32_t W, va_t PC) {
  using namespace branch_option;
  const unsigned Op = W >> 26;
  if (Op != opcode::B && Op != opcode::BC && Op != opcode::XL)
    return {};
  const bool Linked = W & Link;
  const unsigned BO = W >> 21 & 31;
  const bool Conditional = Op != opcode::B && testsCondition(BO);
  // The prediction encoding 01 is reserved for CR-only and CTR-only tests.
  if (Conditional &&
      ((BO & IgnoreCTR)
           ? (BO & (BranchIfCTRZero | PredictionTaken)) == PredictionTaken
           : (BO & IgnoreCR) &&
                 (BO & (BranchIfCRSet | PredictionTaken)) == PredictionTaken))
    return {};
  const va_t Next = branchAddress(A, PC + ppc_elf::InstructionAlignment);
  if (Op != opcode::XL) {
    const int64_t Disp =
        Op == opcode::B ? llvm::SignExtend64<26>(W & DirectDisplacement)
                        : llvm::SignExtend64<16>(W & ConditionalDisplacement);
    const va_t Dest = branchAddress(A, ((W & AbsoluteAddress) ? 0 : PC) + Disp);
    // Both outcomes of a link to the next instruction materialize the PC.
    if (Linked && Dest == Next)
      return {NdOp::COPY, Next};
    return {Linked        ? NdOp::CALL
            : Conditional ? NdOp::COND_BR
                          : NdOp::BRANCH,
            Dest};
  }
  const unsigned XO = W >> 1 & 1023;
  const unsigned Hint = W & HintMask;
  if ((W & ReservedXLBits) || Hint == ReservedHint)
    return {};
  if (XO == opcode::BCLR)
    return {Linked ? NdOp::INDIR_CALL : NdOp::RETURN,
            Conditional && !Linked ? Next : InvalidVA};
  if (XO == opcode::BCCTR && (BO & IgnoreCTR) && Hint != LROnlyHint)
    return {Linked ? NdOp::INDIR_CALL : NdOp::INDIR_BR,
            Conditional && !Linked ? Next : InvalidVA};
  return {};
}

NdVar decrementCTR(State &S) {
  NdVar Counter = S.special(CTR);
  S.emit(NdOp::INT_SUB, Counter, {Counter, S.imm(1)});
  return Counter;
}

NdVar condition(State &S, unsigned BO, unsigned BI) {
  using namespace branch_option;
  NdVar Cond = NdVar::scalar(1, 1);
  if (!(BO & IgnoreCTR)) {
    NdVar Counter = decrementCTR(S);
    Cond = S.op((BO & BranchIfCTRZero) ? NdOp::INT_EQUAL : NdOp::INT_NOTEQUAL,
                Counter, S.imm(0), 1);
  }
  if (!(BO & IgnoreCR)) {
    NdVar Flag = S.cr(BI);
    if (!(BO & BranchIfCRSet))
      Flag = S.op(NdOp::BOOL_XOR, Flag, NdVar::scalar(1, 1), 1);
    Cond = (BO & IgnoreCTR) ? Flag : S.op(NdOp::BOOL_AND, Cond, Flag, 1);
  }
  return Cond;
}

std::optional<unsigned> barrierKey(uint32_t Word) {
  switch (Word) {
#define NEVERD_PPC_BARRIER(Encoding, Key, Mnemonic)                            \
  case Encoding:                                                               \
    return Key;
#include "neverd/ir/intrinsics/IntegerMemory.def"
  default:
    return std::nullopt;
  }
}

std::optional<unsigned> reservationKey(unsigned XO) {
  switch (XO) {
#define NEVERD_PPC_RESERVATION(Encoding, Key, ...)                             \
  case Encoding:                                                               \
    return Key;
#include "neverd/ir/intrinsics/IntegerMemory.def"
  default:
    return std::nullopt;
  }
}

struct Transfer {
  unsigned Bytes;
  bool Store;
  bool Signed;
  bool Update;
  bool Requires64;
  bool Reversed = false;
};

std::optional<Transfer> memoryTransfer(unsigned Op, unsigned XO, unsigned DS) {
  if (Op == opcode::X) {
    switch (XO) {
#define NEVERD_PPC_DX_TRANSFER(Name, D, X, Bytes, Store, Signed)               \
  case X:                                                                      \
  case X | opcode::IndexedUpdate:                                              \
    return Transfer{Bytes, Store, Signed, bool(XO & opcode::IndexedUpdate),    \
                    false};
#define NEVERD_PPC_X64_TRANSFER(Name, X, Bytes, Store, Signed)                 \
  case X:                                                                      \
  case X | opcode::IndexedUpdate:                                              \
    return Transfer{Bytes, Store, Signed, bool(XO & opcode::IndexedUpdate),    \
                    true};
#define NEVERD_PPC_REVERSED_TRANSFER(Name, X, Bytes, Store)                    \
  case X:                                                                      \
    return Transfer{Bytes, Store, false, false, Bytes == 8, true};
#include "PowerPCIntegerForms.def"
    default:
      return std::nullopt;
    }
  }
  if (Op == opcode::LoadDS || Op == opcode::StoreDS) {
    switch (Op << 2 | DS) {
#define NEVERD_PPC_DS_TRANSFER(Name, Primary, Variant, Bytes, Store, Signed,   \
                               Update)                                         \
  case opcode::Primary << 2 | Variant:                                         \
    return Transfer{Bytes, Store, Signed, Update, true};
#include "PowerPCIntegerForms.def"
    default:
      return std::nullopt;
    }
  }
  switch (Op & ~1u) {
#define NEVERD_PPC_DX_TRANSFER(Name, D, X, Bytes, Store, Signed)               \
  case D:                                                                      \
    return Transfer{Bytes, Store, Signed, bool(Op & 1), false};
#include "PowerPCIntegerForms.def"
  default:
    return std::nullopt;
  }
}

enum class PrefixedOp { Add, Load, SignedLoad, Store };

bool prefixed(State &S, uint32_t Prefix, uint32_t Suffix) {
  const unsigned T = Suffix >> 21 & 31, RA = Suffix >> 16 & 31;
  const bool Relative = Prefix & prefix::Relative;
  if (Relative && RA)
    return false;

  PrefixedOp Op;
  unsigned Bytes;
  // Include reserved prefix bits in the match; only R and the immediate vary.
  const uint32_t Format =
      Prefix & ~(prefix::Relative | prefix::HighImmediateMask);
  switch (Format | (Suffix >> 26)) {
#define NEVERD_PPC_PREFIXED(Name, Format, SuffixOp, Kind, Width)               \
  case prefix::Format | SuffixOp:                                              \
    Op = PrefixedOp::Kind;                                                     \
    Bytes = Width;                                                             \
    break;
#include "PowerPCPrefixed.def"
  default:
    return false;
  }

  const int64_t Disp = prefix::displacement(Prefix, Suffix);
  NdVar EA;
  if (RA) {
    EA = Disp ? S.op(NdOp::INT_ADD, S.reg(RA), S.imm(Disp)) : S.reg(RA);
  } else {
    const va_t Target = (Relative ? S.Addr : 0) + Disp;
    if (Op != PrefixedOp::Add)
      EA = NdVar::dataAddress(Target, S.Width);
    else
      EA = Relative ? NdVar::address(Target, S.Width) : S.imm(Disp);
  }
  if (Op == PrefixedOp::Add)
    S.result(T, EA, false);
  else
    S.transfer(T, EA, Bytes, Op == PrefixedOp::Store,
               Op == PrefixedOp::SignedLoad);
  return true;
}

struct Logical {
  NdOp Kind;
  bool InvertRight;
  bool InvertResult;
};

std::optional<Logical> logicalOperation(unsigned Op, unsigned XO) {
  switch (Op << 10 | XO) {
#define NEVERD_PPC_LOGICAL(Name, GPR, CR, Kind, InvertRight, InvertResult)     \
  case opcode::X << 10 | GPR:                                                  \
    return Logical{NdOp::INT_##Kind, InvertRight, InvertResult};               \
  case opcode::XL << 10 | CR:                                                  \
    return Logical{NdOp::BOOL_##Kind, InvertRight, InvertResult};
#include "PowerPCIntegerForms.def"
  default:
    return std::nullopt;
  }
}

enum class CarryAddend { Register, Zero, MinusOne };
struct CarryForm {
  bool ComplementLeft;
  CarryAddend Addend;
};

std::optional<CarryForm> carryForm(unsigned XO) {
  switch (XO) {
#define NEVERD_PPC_CARRY_FORM(Name, Encoding, ComplementLeft, Addend)          \
  case Encoding:                                                               \
    return CarryForm{ComplementLeft, CarryAddend::Addend};
#include "PowerPCIntegerForms.def"
  default:
    return std::nullopt;
  }
}

std::optional<uint64_t> specialRegister(unsigned SPR) {
  switch (SPR) {
#define NEVERD_PPC_SPR(Name, Selector)                                         \
  case Selector:                                                               \
    return Name;
#include "PowerPCIntegerForms.def"
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

// Count bits independently in each byte, word or doubleword of the GPR.
// CONCAT takes its high half first in either instruction byte order.
NdVar populationCount(State &S, NdVar V, unsigned LaneBytes) {
  if (V.Size == LaneBytes) {
    NdVar Count = S.makeTemp(V.Size);
    S.emit(NdOp::POPCOUNT, Count, {V});
    return Count;
  }
  const unsigned Half = V.Size / 2;
  NdVar Upper = S.makeTemp(Half);
  S.emit(NdOp::SUBBYTES, Upper, {V, NdVar::scalar(Half, 4)});
  const NdVar High = populationCount(S, Upper, LaneBytes);
  const NdVar Low = populationCount(S, S.resize(V, Half), LaneBytes);
  return S.op(NdOp::CONCAT, High, Low, V.Size);
}

bool integer(State &S, Arch A, uint32_t W) {
  const unsigned Op = W >> 26, T = W >> 21 & 31, RA = W >> 16 & 31,
                 RB = W >> 11 & 31;
  const unsigned XO = W >> 1 & 1023;
  const unsigned Arithmetic = XO & ~opcode::OverflowEnable;
  const bool OE = XO & opcode::OverflowEnable;
  const bool Wide = S.Width == 8, Rc = W & 1;
  const uint64_t SI = llvm::SignExtend64<16>(W & 0xffff);
  NdVar X = S.reg(RA), Y = S.reg(RB), Src = S.reg(T);
  NdVar Base = RA ? X : S.imm(0);
  if (auto Flow = branch(A, W, S.Addr); Flow.Opcode != NdOp::NOP) {
    if (Flow.Opcode == NdOp::COPY) {
      if (Op == opcode::BC && !(T & branch_option::IgnoreCTR))
        decrementCTR(S);
      S.emit(NdOp::COPY, S.special(LR),
             {NdVar::addressFragment(Flow.Target, S.Width)});
      return true;
    }
    NdVar Dest =
        Op == opcode::XL
            ? S.op(NdOp::INT_AND, S.special(XO == opcode::BCLR ? LR : CTR),
                   S.imm(-uint64_t(ppc_elf::InstructionAlignment)))
            : NdVar::codeAddress(Flow.Target, S.Width);
    if (Flow.Opcode == NdOp::COND_BR) {
      S.emit(NdOp::COND_BR, {}, {Dest, condition(S, T, RA)});
      return true;
    }
    // bclrl targets the old LR. LK writes the next PC and BO updates CTR on
    // both edges. Only the transfer and its call effects are predicated.
    const va_t Next = branchAddress(A, S.Addr + ppc_elf::InstructionAlignment);
    if (Rc)
      S.emit(NdOp::COPY, S.special(LR), {NdVar::codeAddress(Next, S.Width)});
    if (Op != opcode::B && testsCondition(T)) {
      NdVar Taken = condition(S, T, RA);
      NdVar Skip = S.makeTemp(1);
      S.emit(NdOp::BOOL_NOT, Skip, {Taken});
      S.emit(NdOp::COND_BR, {}, {NdVar::codeAddress(Next, S.Width), Skip});
    }
    S.emit(Flow.Opcode, Rc ? S.reg(3) : NdVar{}, {Dest});
    return true;
  }
  if (const auto Key = barrierKey(W)) {
    S.emitVoidIntrinsic(Intrinsic::PPCBarrier, {NdVar::scalar(*Key, 1)});
    return true;
  }
  if (const auto Selector =
          Op == opcode::X ? reservationKey(XO) : std::nullopt) {
    const auto M =
        integer_memory::form(A, Intrinsic::PPCReservation, *Selector);
    if (!M || (M->Op == integer_memory::Operation::Conditional) != Rc)
      return false;
    const bool Store = M->Op == integer_memory::Operation::Conditional;
    const NdVar Key = NdVar::scalar(*Selector, 1);
    NdVar EA = S.op(NdOp::INT_ADD, Base, Y);
    if (!Store)
      S.emitIntrinsic(Intrinsic::PPCReservation, Src, {Key, EA});
    else {
      NdVar Success = S.makeTemp(S.Width);
      S.emitIntrinsic(Intrinsic::PPCReservation, Success, {Key, EA, Src});
      S.emit(NdOp::COPY, S.cr(CRLT), {NdVar::scalar(0, 1)});
      S.emit(NdOp::COPY, S.cr(CRGT), {NdVar::scalar(0, 1)});
      S.emit(NdOp::COPY, S.cr(CREQ), {S.resize(Success, 1)});
      S.emit(NdOp::COPY, S.cr(CRSO), {S.bit(S.special(XER), XERSO)});
    }
    return true;
  }
  if (auto M = memoryTransfer(Op, XO, W & 3)) {
    if ((!Wide && M->Requires64) ||
        (M->Update && (!RA || (!M->Store && RA == T))) ||
        (Op == opcode::X && Rc))
      return false;
    uint64_t Disp = (Op == opcode::LoadDS || Op == opcode::StoreDS)
                        ? SI & ~uint64_t(3)
                        : SI;
    NdVar EA = Op == opcode::X ? S.op(NdOp::INT_ADD, Base, Y)
               : Disp          ? S.op(NdOp::INT_ADD, Base, S.imm(Disp))
                               : Base;
    S.transfer(T, EA, M->Bytes, M->Store, M->Signed, M->Reversed);
    if (M->Update)
      S.emit(NdOp::COPY, X, {EA});
    return true;
  }
  if (Op == opcode::ADDI || Op == opcode::ADDIS) {
    S.result(
        T,
        S.op(NdOp::INT_ADD, Base, S.imm(Op == opcode::ADDIS ? SI << 16 : SI)),
        false);
    return true;
  }
  switch (Op) {
#define NEVERD_PPC_LOGICAL_IMM(Name, Encoding, Kind, Shift, Record)            \
  case Encoding:                                                               \
    S.result(                                                                  \
        RA, S.op(NdOp::INT_##Kind, Src, S.imm(uint64_t(W & 0xffff) << Shift)), \
        Record);                                                               \
    return true;
#include "PowerPCIntegerForms.def"
  }
  if (Op == opcode::CMPLI || Op == opcode::CMPI ||
      (Op == opcode::X && (XO == opcode::CMP || XO == opcode::CMPL) && !Rc)) {
    if ((T & 2) || ((T & 1) && !Wide))
      return false;
    bool Signed = Op == opcode::CMPI || (Op == opcode::X && XO == opcode::CMP);
    unsigned Size = (T & 1) ? 8 : 4;
    NdVar B = Op == opcode::X ? Y : S.imm(Signed ? SI : (W & 0xffff));
    S.compare(T >> 2, S.resize(X, Size), S.resize(B, Size), Signed);
    return true;
  }
  if (Op == opcode::MULLI) {
    S.result(T, S.op(NdOp::INT_MULT, X, S.imm(SI)), false);
    return true;
  }
  if (Op == opcode::RLWIMI || Op == opcode::RLWINM || Op == opcode::RLWNM ||
      (Wide && Op == opcode::RotateDouble)) {
    unsigned MB = W >> 6 & 31, ME = W >> 1 & 31;
    NdVar V;
    bool Insert = Op == opcode::RLWIMI;
    if (Op == opcode::RotateDouble) {
      const unsigned MD = W >> 2 & 7, SH = RB | (W & 2) << 4;
      MB |= W & 32;
      NdVar Shift = S.imm(SH);
      if (MD == opcode::RLDICL)
        ME = 63; // RLDICL
      else if (MD == opcode::RLDICR) {
        ME = MB;
        MB = 0;
      } // RLDICR
      else if (MD == opcode::RLDIC || MD == opcode::RLDIMI) {
        ME = 63 - SH;
        Insert = MD == opcode::RLDIMI;
      } else if ((W >> 1 & 15) == opcode::RLDCL) {
        ME = 63;
        Shift = Y;
      } else if ((W >> 1 & 15) == opcode::RLDCR) {
        ME = MB;
        MB = 0;
        Shift = Y;
      } else
        return false;
      V = S.rotate(Src, Shift);
    } else {
      V = S.rotate(S.resize(Src, 4),
                   Op == opcode::RLWNM ? S.resize(Y, 4) : NdVar::scalar(RB, 4));
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

  if (const auto L = logicalOperation(Op, XO)) {
    const bool CR = Op == opcode::XL;
    if (CR && Rc)
      return false;
    const auto Invert = [&](NdVar V) {
      return CR ? S.op(NdOp::BOOL_XOR, V, NdVar::scalar(1, 1)) : S.inverse(V);
    };
    NdVar Left = CR ? S.cr(RA) : Src, Right = CR ? S.cr(RB) : Y;
    // mr rA,rS is or rA,rS,rS. Keep the register copy visible when it
    // establishes the frame pointer; result() still models the record bit.
    NdVar V = L->Kind == NdOp::INT_OR && !L->InvertRight && Left == Right
                  ? Left
                  : S.op(L->Kind, Left, L->InvertRight ? Invert(Right) : Right);
    if (L->InvertResult)
      V = Invert(V);
    if (CR)
      S.emit(NdOp::COPY, S.cr(T), {V});
    else
      S.result(RA, V, Rc);
    return true;
  }
  if (Op == opcode::XL && XO == opcode::MCRF && !Rc && !(W & 0x0063f800)) {
    for (unsigned I = 0; I < 4; ++I)
      S.emit(NdOp::COPY, S.cr((T & ~3u) + I), {S.cr((RA & ~3u) + I)});
    return true;
  }

  if (Op == opcode::X && !Rc &&
      (XO == opcode::MFSPR || XO == opcode::MTSPR)) { // MFSPR / MTSPR
    const auto Offset = specialRegister(RA | RB << 5);
    // ISA 3.0 added CA32 and OV32 to a full XER read. ELFv2 alone does
    // not establish their presence. Arithmetic users of CA/OV/SO are common.
    if (!Offset || (Wide && *Offset == XER && XO == opcode::MFSPR))
      return false;
    if (XO == opcode::MFSPR)
      S.result(T, S.special(*Offset), false);
    else
      S.emit(NdOp::COPY, S.special(*Offset), {Src});
    return true;
  }
  if (Op == opcode::SUBFIC || Op == opcode::ADDIC ||
      Op == opcode::ADDICRecord ||
      (Op == opcode::X &&
       (Arithmetic == opcode::ADD || Arithmetic == opcode::ADDC ||
        Arithmetic == opcode::SUBF || Arithmetic == opcode::SUBFC))) {
    const bool Sub = Op == opcode::SUBFIC ||
                     (Op == opcode::X && (Arithmetic == opcode::SUBF ||
                                          Arithmetic == opcode::SUBFC));
    const bool Carry = Op != opcode::X || Arithmetic == opcode::ADDC ||
                       Arithmetic == opcode::SUBFC;
    const bool Overflow = Op == opcode::X && OE;
    NdVar B = Op == opcode::X ? Y : S.imm(SI);
    NdVar Result = Sub ? S.op(NdOp::INT_SUB, B, X) : S.op(NdOp::INT_ADD, X, B);
    if (Carry)
      S.xerBit(XERCA, Sub ? S.op(NdOp::INT_LESSEQUAL, X, B, 1)
                          : S.op(NdOp::INT_CARRY, X, B, 1));
    if (Overflow) {
      NdVar V =
          Sub ? S.op(NdOp::INT_SBOR, B, X, 1) : S.op(NdOp::INT_SOVF, X, B, 1);
      S.overflow(V);
    }
    S.result(T, Result, Op == opcode::ADDICRecord || (Op == opcode::X && Rc));
    return true;
  }
  if (Op != opcode::X)
    return false;
  if (const auto D = divisionByOpcode(Arithmetic, Rc)) {
    if (!D->admits(A))
      return false;
    if (OE) {
      const NdVar Left = S.resize(X, D->Bytes), Right = S.resize(Y, D->Bytes);
      NdVar Overflow =
          S.op(NdOp::INT_EQUAL, Right, NdVar::scalar(0, D->Bytes), 1);
      if (D->Signed) {
        const NdVar Min =
            NdVar::scalar(uint64_t(1) << (D->Bytes * 8 - 1), D->Bytes);
        const NdVar MinusOne = NdVar::scalar(
            llvm::maskTrailingOnes<uint64_t>(D->Bytes * 8), D->Bytes);
        Overflow =
            S.op(NdOp::BOOL_OR, Overflow,
                 S.op(NdOp::BOOL_AND, S.op(NdOp::INT_EQUAL, Left, Min, 1),
                      S.op(NdOp::INT_EQUAL, Right, MinusOne, 1), 1),
                 1);
      }
      S.overflow(Overflow);
    }
    S.fixedPointResult(T, D->Id, X, Y, Rc);
    return true;
  }
  if (const auto Form = carryForm(Arithmetic)) {
    const bool Register = Form->Addend == CarryAddend::Register;
    if (!Register && RB)
      return false;
    NdVar Left = Form->ComplementLeft ? S.inverse(X) : X;
    NdVar Right =
        Register
            ? Y
            : S.imm(Form->Addend == CarryAddend::MinusOne ? UINT64_MAX : 0);
    NdVar Carry = S.resize(S.bit(S.special(XER), XERCA), S.Width);
    NdVar Partial = S.op(NdOp::INT_ADD, Left, Right);
    NdVar Sum = S.op(NdOp::INT_ADD, Partial, Carry);
    S.xerBit(XERCA, S.op(NdOp::BOOL_OR, S.op(NdOp::INT_CARRY, Left, Right, 1),
                         S.op(NdOp::INT_CARRY, Partial, Carry, 1), 1));
    if (OE) {
      // The sum overflows if both addends' signs differ from its sign.
      NdVar Signs = S.op(NdOp::INT_AND, S.op(NdOp::INT_XOR, Left, Sum),
                         S.op(NdOp::INT_XOR, Right, Sum));
      S.overflow(S.bit(Signs, 8 * S.Width - 1));
    }
    S.result(T, Sum, Rc);
    return true;
  }
  if (XO == opcode::MFCR && !Rc && !(W & 0x001ff800)) {
    NdVar Value = S.imm(0);
    for (unsigned B = 0; B < 32; ++B) {
      // An incoming CR bit occupies a byte of LowIR storage. Only bit zero
      // can contribute, even when the other seven storage bits are unknown.
      NdVar Bit = S.op(NdOp::INT_AND, S.cr(B), NdVar::scalar(1, 1));
      Value =
          S.op(NdOp::INT_OR, Value,
               S.op(NdOp::INT_LEFT, S.resize(Bit, S.Width), S.imm(crShift(B))));
    }
    S.result(T, Value, false);
    return true;
  }
  if (XO == opcode::MTCRF && !Rc && !(W & 0x00100800)) { // MTCRF
    const unsigned Fields = W >> 12 & 255;
    for (unsigned B = 0; B < 32; ++B)
      if (Fields & (0x80 >> (B / 4)))
        S.emit(NdOp::COPY, S.cr(B), {S.bit(Src, crShift(B))});
    return true;
  }
  if ((XO & 31) == opcode::ISEL && !Rc) { // ISEL uses literal zero for RA=0.
    S.result(T, S.select(S.cr((W >> 6) & 31), Base, Y), false);
    return true;
  }
  NdVar V;
  switch (XO) {
  case opcode::EXTSH:
  case opcode::EXTSB:
  case opcode::EXTSW:
    if (RB || (XO == opcode::EXTSW && !Wide))
      return false;
    V = S.resize(S.resize(Src, XO == opcode::EXTSB   ? 1
                               : XO == opcode::EXTSH ? 2
                                                     : 4),
                 S.Width, true);
    break;
#define NEVERD_PPC_POPULATION(Name, Encoding, LaneBytes)                       \
  case Encoding:                                                               \
    if (RB || Rc || LaneBytes > S.Width)                                       \
      return false;                                                            \
    V = populationCount(S, Src, LaneBytes);                                    \
    break;
#include "PowerPCIntegerForms.def"
  case opcode::CNTLZW:
  case opcode::CNTLZD:
  case opcode::CNTTZW:
  case opcode::CNTTZD: {
    const bool Double = XO == opcode::CNTLZD || XO == opcode::CNTTZD;
    if (RB || (Double && !Wide))
      return false;
    NdVar In = S.resize(Src, Double ? 8 : 4);
    NdVar Count = XO == opcode::CNTTZW || XO == opcode::CNTTZD
                      ? S.countTrailingZeros(In)
                      : S.op(NdOp::LZCOUNT, In);
    V = S.resize(Count, S.Width);
    break;
  }
  case opcode::SLW:
  case opcode::SRW:
    if (!Wide) {
      // A six-bit count can shift every bit out of a word. Widen first so
      // the shift is defined in LLVM and C even when the count exceeds 31.
      NdVar Word = S.resize(Src, sizeof(uint64_t));
      NdVar Count = S.op(NdOp::INT_AND, Y, S.imm(Word.Size * 8 - 1));
      NdVar Shifted = S.op(XO == opcode::SLW ? NdOp::INT_LEFT : NdOp::INT_RIGHT,
                           Word, S.resize(Count, Word.Size));
      V = S.resize(Shifted, S.Width);
      break;
    }
    [[fallthrough]];
  case opcode::SLD:
  case opcode::SRD: {
    unsigned Size = XO == opcode::SLW || XO == opcode::SRW ? 4 : 8;
    if (Size > S.Width)
      return false;
    NdVar Count = S.op(NdOp::INT_AND, Y, S.imm(Size * 16 - 1));
    NdVar Shift =
        S.resize(S.op(NdOp::INT_AND, Count, S.imm(Size * 8 - 1)), Size);
    NdVar Shifted =
        S.op((XO == opcode::SLW || XO == opcode::SLD) ? NdOp::INT_LEFT
                                                      : NdOp::INT_RIGHT,
             S.resize(Src, Size), Shift);
    V = S.select(S.op(NdOp::INT_LESS, Count, S.imm(Size * 8), 1),
                 S.resize(Shifted, S.Width), S.imm(0));
    break;
  }
  case opcode::SRAW:
  case opcode::SRAWI:
  case opcode::SRAD:
  case opcode::SRADI:
  case opcode::SRADI | 1: { // XS-form SH[5] occupies the low bit of XO.
    const bool Double =
        XO == opcode::SRAD || XO == opcode::SRADI || XO == (opcode::SRADI | 1);
    if (Double && !Wide)
      return false;
    const unsigned Bytes = Double ? 8 : 4, Bits = Bytes * 8;
    NdVar In = S.resize(Src, Bytes);
    NdVar Count;
    if (XO == opcode::SRAW || XO == opcode::SRAD)
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
    S.xerBit(XERCA, S.op(NdOp::BOOL_AND, Lost, S.bit(In, Bits - 1), 1));
    V = S.resize(Shifted, S.Width, true);
    break;
  }
  case opcode::MULHDU:
  case opcode::MULHWU:
  case opcode::MULHD:
  case opcode::MULHW: {
    const bool Double = XO == opcode::MULHDU || XO == opcode::MULHD;
    if (Double && !Wide)
      return false;
    const bool Signed = XO == opcode::MULHD || XO == opcode::MULHW;
    if (!Double && Wide) {
      const Intrinsic Id =
          Signed ? (Rc ? Intrinsic::PPCMulhwRecord : Intrinsic::PPCMulhw)
                 : (Rc ? Intrinsic::PPCMulhwuRecord : Intrinsic::PPCMulhwu);
      S.fixedPointResult(T, Id, X, Y, Rc);
      return true;
    }
    S.result(T, S.multiplyHigh(X, Y, Signed, Signed), Rc);
    return true;
  }
  case opcode::NEG:
    if (RB)
      return false;
    S.result(T, S.op(NdOp::INT_SUB, S.imm(0), X), Rc);
    return true;
  case opcode::MULLW:
  case opcode::MULLW | opcode::OverflowEnable:
  case opcode::MULLD:
  case opcode::MULLD | opcode::OverflowEnable: {
    const unsigned Bytes =
        Arithmetic == opcode::MULLW ? sizeof(int32_t) : sizeof(int64_t);
    if (Bytes > S.Width)
      return false;
    const unsigned ProductBytes = OE ? 2 * Bytes : S.Width;
    X = S.resize(S.resize(X, Bytes), ProductBytes, /*Signed=*/true);
    Y = S.resize(S.resize(Y, Bytes), ProductBytes, /*Signed=*/true);
    const NdVar Product = S.op(NdOp::INT_MULT, X, Y);
    const NdVar Result = S.resize(Product, S.Width);
    if (OE) {
      // OV tests the operand width even when PPC64 returns the full word
      // product.
      const NdVar Low = S.resize(Result, Bytes);
      const NdVar High = S.makeTemp(Bytes);
      S.emit(NdOp::SUBBYTES, High, {Product, NdVar::scalar(Bytes, 4)});
      const NdVar Sign =
          S.op(NdOp::INT_ASHR, Low, NdVar::scalar(Bytes * 8 - 1, Bytes));
      S.overflow(S.op(NdOp::INT_NOTEQUAL, High, Sign, 1));
    }
    S.result(T, Result, Rc);
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
  if (I.size == prefix::InstructionBytes && isPowerPC64(A)) {
    const bool Little = archLittleEndian(A);
    if (prefixed(
            S, readTargetEndian<uint32_t>(I.bytes, Little),
            readTargetEndian<uint32_t>(I.bytes + sizeof(uint32_t), Little)))
      return;
  }
  Ops.resize(S.OpsStart);
  throw UnliftedInstruction(I.address, I.mnemonic, I.op_str);
}
} // namespace neverd::ppc
