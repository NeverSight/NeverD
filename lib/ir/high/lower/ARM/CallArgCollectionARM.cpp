//===- CallArgCollectionARM.cpp - ARM32 call-argument ABI -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// ARM32 (AAPCS) stack-argument recovery.  r0–r3 are filled by the shared
/// register scan; remaining args are 4-byte stores off SP.  Slot width comes
/// from TRI.PointerSize (4), not the x86-64 8-byte default the combined
/// collector used to hard-code.
///
//===----------------------------------------------------------------------===//

#include "../CallArgCollectionDetail.h"

namespace neverd {
namespace call_args_detail {

void collectCallArgsARM(const CallArgScan &Scan, std::vector<ExprPtr> &Found,
                        std::vector<ExprPtr> &Args) {
  collectFullBankStackArgs(Scan, Found);
  (void)Args;
}

} // namespace call_args_detail
} // namespace neverd
