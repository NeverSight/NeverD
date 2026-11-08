//===- MIPSLiftBitfield.cpp - MIPS r2 bitfields and shuffles --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Lift SPECIAL3 fields with architectural word restrictions. Register lane
/// positions are numeric bits, independent of the target's memory byte order.

#include "neverd/lift/MIPS/MIPSLifter.h"

#include "llvm/Support/MathExtras.h"

#include <array>
#include <optional>

namespace neverd {
namespace {

struct Field {
  uint16_t Bytes;
  unsigned PositionBias;
  unsigned LimitBias;
  bool Insert;
};

std::optional<Field> field(unsigned Function) {
  switch (Function) {
#define NEVERD_MIPS_FIELD(Name, Function, Bytes, PositionBias, LimitBias,      \
                          Insert)                                              \
  case Function:                                                               \
    return Field{Bytes, PositionBias, LimitBias, Insert};
#include "neverd/lift/MIPS/MIPSBitfield.def"
  default:
    return std::nullopt;
  }
}

enum class ShuffleKind { SignByte, SignHalf, SwapBytes, ReverseHalves };
struct Shuffle {
  uint16_t Bytes;
  ShuffleKind Kind;
};

std::optional<Shuffle> shuffle(unsigned Function, unsigned SA) {
  switch ((Function << 5) | SA) {
#define NEVERD_MIPS_SHUFFLE(Name, Function, SA, Bytes, Kind)                   \
  case (Function << 5) | SA:                                                   \
    return Shuffle{Bytes, ShuffleKind::Kind};
#include "neverd/lift/MIPS/MIPSBitfield.def"
  default:
    return std::nullopt;
  }
}

} // namespace

bool MIPSLifter::liftBitfield(State &S, uint32_t Word) const {
  const unsigned RS = (Word >> 21) & 31;
  const unsigned RT = (Word >> 16) & 31;
  const unsigned RD = (Word >> 11) & 31;
  const unsigned SA = (Word >> 6) & 31;
  const unsigned Function = Word & 63;
  if (const auto Form = field(Function)) {
    const unsigned Position = SA + Form->PositionBias;
    const unsigned Limit = RD + Form->LimitBias;
    if (Form->Bytes > S.Width || (Form->Insert && Limit <= Position))
      return false;
    const unsigned Size = Form->Insert ? Limit - Position : Limit;
    if (Size > Form->Bytes * 8 - Position)
      return false;
    if (Form->Bytes == 4) {
      S.requireWord(RS);
      if (Form->Insert)
        S.requireWord(RT);
    }
    const uint64_t Mask = llvm::maskTrailingOnes<uint64_t>(Size);
    const uint16_t Bytes = Form->Bytes;
    const auto Shift = NdVar::cst(Position, Bytes);
    NdVar Result;
    if (Form->Insert) {
      const NdVar Low =
          S.binary(NdOp::INT_AND, S.read(RS, Bytes), NdVar::cst(Mask, Bytes));
      Result = S.binary(NdOp::INT_OR, S.binary(NdOp::INT_LEFT, Low, Shift),
                        S.binary(NdOp::INT_AND, S.read(RT, Bytes),
                                 NdVar::cst(~(Mask << Position), Bytes)));
    } else {
      Result = S.binary(NdOp::INT_AND,
                        S.binary(NdOp::INT_RIGHT, S.read(RS, Bytes), Shift),
                        NdVar::cst(Mask, Bytes));
    }
    // EXT clears bits above its field within a word, then sign-extends that
    // word. In particular EXT width 32 differs from DEXT width 32 on MIPS64.
    S.write(RT, Result);
    return true;
  }
  const auto Form = shuffle(Function, SA);
  if (!Form || RS || Form->Bytes > S.Width)
    return false;
  if (Form->Bytes == 4)
    S.requireWord(RT);
  if (Form->Kind == ShuffleKind::SignByte ||
      Form->Kind == ShuffleKind::SignHalf) {
    S.write(RD, S.read(RT, Form->Kind == ShuffleKind::SignByte ? 1 : 2));
    return true;
  }
  const uint16_t Bytes = Form->Bytes;
  const NdVar Value = S.read(RT, Bytes);
  if (Form->Kind == ShuffleKind::SwapBytes) {
    const auto Mask = NdVar::cst(0x00ff00ff00ff00ffULL, Bytes);
    const auto Shift = NdVar::cst(8, Bytes);
    S.write(RD,
            S.binary(NdOp::INT_OR,
                     S.binary(NdOp::INT_LEFT,
                              S.binary(NdOp::INT_AND, Value, Mask), Shift),
                     S.binary(NdOp::INT_AND,
                              S.binary(NdOp::INT_RIGHT, Value, Shift), Mask)));
  } else {
    std::array<NdVar, 4> Parts;
    for (unsigned I = 0; I != Parts.size(); ++I) {
      Parts[I] = S.makeTemp(2);
      S.emit(NdOp::SUBBYTES, Parts[I], {Value, NdVar::cst(I * 2, 4)});
    }
    S.write(RD, S.binary(NdOp::CONCAT,
                         S.binary(NdOp::CONCAT, Parts[0], Parts[1], 4),
                         S.binary(NdOp::CONCAT, Parts[2], Parts[3], 4), 8));
  }
  return true;
}

} // namespace neverd
