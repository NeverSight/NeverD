//===- SourceScalarResult.h - MIPS scalar call result carriers -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_MIPS_SOURCESCALARRESULT_H
#define NEVERD_IR_MIPS_SOURCESCALARRESULT_H

#include "neverd/ir/FloatingCallArguments.h"
#include "neverd/ir/TargetRegInfo.h"

namespace neverd::mips {

/// The descriptor's logical result occupies f0, v0 or v0/v1. Components
/// follow object byte order. The first word in memory is v0 in either byte
/// order. TargetRegInfo supplies its significance when building IR.
inline void placeScalarResult(const TargetRegInfo &Registers,
                              const FloatingCallArguments &Signature,
                              SourceABIValueLocation &Single,
                              std::vector<SourceABIValueLocation> &Pieces) {
  Single = {};
  Pieces.clear();
  if (Signature.ReturnKind == NdTypeKind::Float) {
    Single = {SourceABICarrierKind::FloatingRegister, Registers.FPReturnReg, 0,
              Signature.ReturnBytes};
  } else if (Signature.ReturnKind != NdTypeKind::Void) {
    if (Signature.ReturnBytes <= Registers.FullRegWidth) {
      Single = {SourceABICarrierKind::IntegerRegister, Registers.IntReturnReg,
                0, Signature.ReturnBytes};
    } else {
      for (const auto Offset :
           {Registers.IntReturnReg, Registers.IntReturnReg2})
        Pieces.push_back({SourceABICarrierKind::IntegerRegister, Offset, 0,
                          Registers.FullRegWidth});
    }
  }
}
} // namespace neverd::mips

#endif
