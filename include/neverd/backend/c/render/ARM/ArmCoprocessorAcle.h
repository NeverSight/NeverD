//===- ArmCoprocessorAcle.h - C text for ARM coprocessor moves --*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// HighC and LLVMC print an ARM coprocessor register transfer the same way:
// as ACLE's __arm_mrc, __arm_mcr, __arm_mrrc or __arm_mcrr, which take the
// instruction's coprocessor and register numbers as constants in the order
// the listing gives them (`mrc p15, #0, r0, c13, c0, #3` is
// `__arm_mrc(15, 0, 13, 0, 3)`).
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_C_RENDER_ARM_ARMCOPROCESSORACLE_H
#define NEVERD_BACKEND_C_RENDER_ARM_ARMCOPROCESSORACLE_H

#include "neverd/ir/intrinsics/IntrinsicInstructionText.h"

#include "llvm/ADT/StringRef.h"

#include <string>

namespace neverd {

/// The ACLE call that runs coprocessor transfer \p Id with \p Field, to
/// which an MCR passes the `uint32_t` \p Value and an MCRR the `uint64_t`
/// one; empty where armCoprocessorOperands has none.
inline std::string armCoprocessorAcleCall(Intrinsic Id, uint64_t Field,
                                          llvm::StringRef Value = {}) {
  const std::optional<ArmCoprocessorOperands> O =
      armCoprocessorOperands(Id, Field);
  if (!O)
    return {};
  const std::string Head =
      std::to_string(O->Coprocessor) + ", " + std::to_string(O->Opc1) + ", ";
  const std::string Registers = std::to_string(O->CRn) + ", " +
                                std::to_string(O->CRm) + ", " +
                                std::to_string(O->Opc2) + ")";
  switch (Id) {
  case Intrinsic::ArmMrc:
    return "__arm_mrc(" + Head + Registers;
  case Intrinsic::ArmMcr:
    return "__arm_mcr(" + Head + Value.str() + ", " + Registers;
  case Intrinsic::ArmMrrc:
    return "__arm_mrrc(" + Head + std::to_string(O->CRm) + ")";
  case Intrinsic::ArmMcrr:
    return "__arm_mcrr(" + Head + Value.str() + ", " + std::to_string(O->CRm) +
           ")";
  default:
    return {};
  }
}

} // namespace neverd

#endif // NEVERD_BACKEND_C_RENDER_ARM_ARMCOPROCESSORACLE_H
