//===- X64Decoder.h - Machine-qualified x64 decoding -------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_X64DECODER_H
#define NEVERD_EMULATION_X64DECODER_H
#include "../../core/ExecutionDiagnostics.h"

#include "neverd/emulation/arch/x86_64/X64BranchModel.h"

#include "llvm/Support/ErrorHandling.h"

#include <capstone/capstone.h>
namespace neverd::emulation::x64 {
inline cs_mode decoderMode(X64BranchModel Model) {
  switch (Model) {
#define NEVERD_X64_BRANCH_MODEL(Name, Mode)                                    \
  case X64BranchModel::Name:                                                   \
    return cs_mode(CS_MODE_64 | Mode);
#include "neverd/emulation/arch/x86_64/X64BranchModels.def"
#undef NEVERD_X64_BRANCH_MODEL
  }
  llvm_unreachable(diagnostic::BranchModel);
}
} // namespace neverd::emulation::x64
#endif
