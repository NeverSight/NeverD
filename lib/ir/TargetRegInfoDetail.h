//===- TargetRegInfoDetail.h - Shared register table decls ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Internal declarations shared between the architecture-generic register
/// queries and dispatch (TargetRegInfo.cpp) and the per-target register
/// description tables (TargetRegInfoX86.cpp, TargetRegInfoAArch64.cpp,
/// TargetRegInfoARM.cpp).
///
/// This header is an implementation detail of the ir library and should NOT
/// be included by code outside lib/ir/.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_TARGETREGINFODETAIL_H
#define NEVERD_IR_TARGETREGINFODETAIL_H

#include "neverd/ir/TargetRegInfo.h"

namespace neverd {

// The per-architecture register description tables, returned by
// getTargetRegInfo().  Non-const on purpose; see the NOTE in initSubRegs().

/// x86-64 register info (TargetRegInfoX86.cpp).
extern TargetRegInfo X64RegInfo;

/// i386 register info (TargetRegInfoX86.cpp).
extern TargetRegInfo X86RegInfo;

/// AArch64 register info (TargetRegInfoAArch64.cpp).
extern TargetRegInfo A64RegInfo;

/// ARM32 register info (TargetRegInfoARM.cpp).
extern TargetRegInfo ARMRegInfo;

// The table fields that cannot be set from the static initializer — the
// sub-register tables, the return-register sequences and the limits.h
// alignment constants — are filled in by these per-architecture functions.
// They run exactly once, together, under the function-local static in
// TargetRegInfo.cpp's initSubRegs().

/// Complete X64RegInfo and X86RegInfo (TargetRegInfoX86.cpp).
void initX86RegInfoTables();

/// Complete A64RegInfo (TargetRegInfoAArch64.cpp).
void initAArch64RegInfoTables();

/// Complete ARMRegInfo (TargetRegInfoARM.cpp).
void initARMRegInfoTables();

} // namespace neverd

#endif // NEVERD_IR_TARGETREGINFODETAIL_H
