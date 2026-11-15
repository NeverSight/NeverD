//===- MIPSLiftSystem.cpp - MIPS system and board-visible transfers -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Normalize CP0 views, interrupt state, cache requests and hardware register
/// selectors from the qualified MIPS16e2 and Release 6 instruction profiles.
//===----------------------------------------------------------------------===//

#include "neverd/ir/intrinsics/MIPS/MIPSScalar.h"
#include "neverd/lift/MIPS/MIPSLifter.h"

#include "llvm/Support/MathExtras.h"

namespace neverd {
bool MIPSLifter::liftSystem(State &S, uint32_t Word) const {
  const unsigned Opcode = Word >> 26, RS = (Word >> 21) & 31,
                 RT = (Word >> 16) & 31, RD = (Word >> 11) & 31;
  const bool R6 = ISARevision == 6;
  if (Word == 0x00000140) {
    S.emitIntrinsic(Intrinsic::MIPSPause, {});
    return true;
  }
  if (Opcode == 16 && !(Word & 0x7f8)) {
    if (const auto C = mips::cp0TransferEncoding(RS)) {
      const unsigned Key = ((Word & 7) << 5) | RD;
      if (C->Bytes > S.Width || (!R6 && RS != 0 && RS != 4) ||
          (!HasMT && !R6 && mips::cp0RequiresMT(Key)))
        return false;
      if (C->Write)
        S.emitIntrinsic(C->Id, {}, {NdVar::cst(Key, 1), S.read(RT, C->Bytes)});
      else {
        const auto Result = S.makeTemp(C->Bytes);
        S.emitIntrinsic(C->Id, Result, {NdVar::cst(Key, 1)});
        S.write(RT, Result);
      }
      return true;
    }
  }
  const unsigned Function = Word & 127;
  if ((!R6 && (Opcode == 47 || Opcode == 51)) ||
      (R6 && Opcode == 31 && (Function == 0x25 || Function == 0x35))) {
    const bool Prefetch = R6 ? Function == 0x35 : Opcode == 51;
    if (R6 && Prefetch && RT >= 24)
      return false;
    const auto Offset =
        R6 ? llvm::SignExtend64<9>(Word >> 7) : int64_t(int16_t(Word));
    const auto Address =
        S.binary(NdOp::INT_ADD, S.read(RS), NdVar::cst(Offset, S.Width));
    S.emitIntrinsic(Prefetch ? Intrinsic::MIPSPrefetch : Intrinsic::MIPSCache,
                    {}, {NdVar::cst(RT, 1), Address});
    return true;
  }
  if (HasMT && ((Word & 0xffe0ffdf) == 0x41600bc1 ||
                (Word & 0xffe0ffdf) == 0x41600001)) {
    const bool Thread = Word & 0x800;
    const auto Id = Thread ? (Word & 32 ? Intrinsic::MIPSEnableMT
                                        : Intrinsic::MIPSDisableMT)
                           : (Word & 32 ? Intrinsic::MIPSEnableVPE
                                        : Intrinsic::MIPSDisableVPE);
    const auto Result = S.makeTemp(4);
    S.emitIntrinsic(Id, Result);
    S.write(RT, Result);
    return true;
  }
  if (!R6 && (Opcode == 48 || Opcode == 56)) {
    const auto Address = S.binary(NdOp::INT_ADD, S.read(RS),
                                  NdVar::cst(int64_t(int16_t(Word)), 4));
    const auto Result = S.makeTemp(4);
    if (Opcode == 48)
      S.emitIntrinsic(Intrinsic::MIPSLoadLinkedWord, Result, {Address});
    else
      S.emitIntrinsic(Intrinsic::MIPSStoreConditionalWord, Result,
                      {Address, S.read(RT)});
    S.write(RT, Result);
    return true;
  }
  if ((Word & (R6 ? 0xffe0063fu : 0xffe007ffu)) == 0x7c00003b) {
    const unsigned Key = RD | (R6 ? (Word >> 1) & 0xe0 : 0);
    if (R6 && !mips::supportedR6HardwareRegister(Key))
      return false;
    const auto Result = S.makeTemp(S.Width);
    S.emitIntrinsic(R6 ? Intrinsic::MIPSReadR6HardwareRegister
                       : Intrinsic::MIPSReadHardwareRegister,
                    Result, {NdVar::cst(Key, 1)});
    S.write(RT, Result);
    return true;
  }
  if ((Word & 0xffe0ffdf) == 0x41606000) {
    const auto Result = S.makeTemp(4);
    S.emitIntrinsic(Word & 32 ? Intrinsic::MIPSEnableInterrupts
                              : Intrinsic::MIPSDisableInterrupts,
                    Result);
    S.write(RT, Result);
    return true;
  }
  return false;
}
} // namespace neverd
