//===- MIPSLiftMemory.cpp - MIPS integer memory transfers ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Integer loads and stores retain alignment faults and use the image's target
/// byte order. Register values remain ordinary integers in every endian mode.
///
//===----------------------------------------------------------------------===//

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/ir/intrinsics/MIPS/MIPSScalar.h"
#include "neverd/lift/MIPS/MIPSLifter.h"

namespace neverd {

bool MIPSLifter::liftMemory(State &S, uint32_t Word) const {
  const unsigned Opcode = Word >> 26;
  if (const auto Load = mips::mergeLoadOpcode(Opcode)) {
    if (Load->Bytes > S.Width)
      return false;
    const unsigned Base = (Word >> 21) & 31;
    const unsigned Register = (Word >> 16) & 31;
    const auto Displacement = uint64_t(int64_t(int16_t(Word)));
    const NdVar Address = S.binary(NdOp::INT_ADD, S.read(Base),
                                   NdVar::cst(Displacement, S.Width));
    const NdVar Old = S.read(Register);
    NdVar Result = S.makeTemp(Load->Bytes);
    S.emitIntrinsic(Load->Id, Result, {Address, S.resize(Old, Load->Bytes)});
    if (Load->Id == Intrinsic::MIPSLwr && S.Width == 8 && Register) {
      // r2 permits two high-word results for a partial LWR. Keep that value
      // separate: a following LWL consumes only the exact low word and must
      // not inherit an observable precondition on discarded high bits.
      const NdVar High = S.makeTemp(4);
      S.emitIntrinsic(Intrinsic::MIPSLwrUpper, High, {Address, Old, Result});
      const NdVar Joined = S.makeTemp(8);
      S.emit(NdOp::CONCAT, Joined, {High, Result});
      Result = Joined;
    }
    S.write(Register, Result);
    return true;
  }
  unsigned Bytes = 0;
  bool Store = false;
  bool Signed = true;
  switch (Opcode) {
  case 32:
    Bytes = 1;
    break; // LB
  case 33:
    Bytes = 2;
    break; // LH
  case 35:
    Bytes = 4;
    break; // LW
  case 36:
    Bytes = 1;
    Signed = false;
    break; // LBU
  case 37:
    Bytes = 2;
    Signed = false;
    break; // LHU
  case 39:
    Bytes = 4;
    Signed = false;
    break; // LWU (MIPS64 only)
  case 55:
    Bytes = 8;
    break; // LD
  case 40:
    Bytes = 1;
    Store = true;
    break; // SB
  case 41:
    Bytes = 2;
    Store = true;
    break; // SH
  case 43:
    Bytes = 4;
    Store = true;
    break; // SW
  case 63:
    Bytes = 8;
    Store = true;
    break; // SD
  default:
    return false;
  }
  if ((Bytes == 8 || Opcode == 39) && S.Width != 8)
    return false;
  const unsigned Base = (Word >> 21) & 31;
  const unsigned Register = (Word >> 16) & 31;
  const auto Displacement = uint64_t(int64_t(int16_t(Word)));
  const NdVar Address =
      S.binary(NdOp::INT_ADD, S.read(Base), NdVar::cst(Displacement, S.Width));
  if (Bytes > 1)
    S.emitIntrinsic(Intrinsic::RequireAligned, {},
                    {S.resize(Address, 8), NdVar::scalar(Bytes, 8)});
  if (Store) {
    S.emit(NdOp::STORE, {}, {Address, S.read(Register, Bytes)});
  } else {
    NdVar Value = S.makeTemp(Bytes);
    S.emit(NdOp::LOAD, Value, {Address});
    S.write(Register, Value, Signed);
  }
  return true;
}

} // namespace neverd
