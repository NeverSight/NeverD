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

bool MIPSLifter::liftMemory(State &S, uint32_t Word,
                            std::optional<NdVar> BaseOverride) const {
  const unsigned Opcode = Word >> 26;
  if (const auto Store = mips::mergeStoreOpcode(Opcode)) {
    if (Store->Bytes > S.Width)
      return false;
    const NdVar Address = S.binary(
        NdOp::INT_ADD, BaseOverride ? *BaseOverride : S.read((Word >> 21) & 31),
        NdVar::cst(uint64_t(int64_t(int16_t(Word))), S.Width));
    const NdVar Value = S.resize(S.read((Word >> 16) & 31), Store->Bytes);
    S.emitIntrinsic(Store->Id, {}, {Address, Value});
    return true;
  }
  if (const auto Load = mips::mergeLoadOpcode(Opcode)) {
    if (Load->Bytes > S.Width)
      return false;
    const unsigned Base = (Word >> 21) & 31;
    const unsigned Register = (Word >> 16) & 31;
    const auto Displacement = uint64_t(int64_t(int16_t(Word)));
    const NdVar Address =
        S.binary(NdOp::INT_ADD, BaseOverride ? *BaseOverride : S.read(Base),
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
  const auto Transfer = mips::integerMemoryAccess(Opcode);
  if (!Transfer || (Transfer->Wide && S.Width != 8))
    return false;
  const unsigned Bytes = Transfer->Bytes;
  const unsigned Base = (Word >> 21) & 31;
  const unsigned Register = (Word >> 16) & 31;
  const auto Displacement = uint64_t(int64_t(int16_t(Word)));
  NdVar Address =
      BaseOverride && BaseOverride->isConst() && !Displacement
          ? *BaseOverride
          : S.binary(NdOp::INT_ADD, BaseOverride ? *BaseOverride : S.read(Base),
                     NdVar::cst(Displacement, S.Width));
  if (TimedInstructions) {
    const NdVar Physical = S.makeTemp(S.Width);
    S.emitIntrinsic(Intrinsic::MIPSHistoricalAddressCheck, Physical, {Address});
    Address = Physical;
  } else if (Bytes > 1 && ISARevision != 6 &&
           (!Address.isConst() || (Address.Offset & (Bytes - 1))))
    S.emitIntrinsic(Intrinsic::RequireAligned, {},
                    {S.resize(Address, 8), NdVar::scalar(Bytes, 8)});
  if (Transfer->Store) {
    S.emit(NdOp::STORE, {}, {Address, S.read(Register, Bytes)});
  } else {
    NdVar Value = S.makeTemp(Bytes);
    S.emit(NdOp::LOAD, Value, {Address});
    S.write(Register, Value, Transfer->Signed);
  }
  return true;
}

} // namespace neverd
