#ifndef NEVERD_IR_LOW_DIRECTTAILCALL_H
#define NEVERD_IR_LOW_DIRECTTAILCALL_H

#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/low/LowIR.h"

namespace neverd {
/// Canonical operations for an independently classified direct tail transfer.
/// The CFG owns classification; source replay must authenticate it anew from
/// the original instruction and current external function entry. This models
/// no architectural link-register write or synthetic stack push.
inline std::vector<LowOp> directTailCallOperations(Arch Architecture, va_t At,
                                                   va_t Target) {
  const auto &TRI = getTargetRegInfo(Architecture);
  const auto Result =
      NdVar::reg(TRI.IntReturnReg, TRI.integerReturnWordBytes());
  LowOp Call;
  Call.Opcode = NdOp::CALL;
  Call.Output = Result;
  Call.addInput(NdVar::cst(Target, TRI.PointerSize));
  Call.Addr = At;
  LowOp Return;
  Return.Opcode = NdOp::RETURN;
  Return.addInput(Result);
  Return.Addr = At;
  return {Call, Return};
}
} // namespace neverd
#endif
