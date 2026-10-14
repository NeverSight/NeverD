//===- FloatingPointContract.h - C floating-point rounding -------*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Shared source contract for distinct floating-point rounding operations.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_C_FLOATINGPOINTCONTRACT_H
#define NEVERD_BACKEND_C_FLOATINGPOINTCONTRACT_H

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/raw_ostream.h"

namespace neverd::c_float {

/// Distinct source operations retain their distinct rounding. An explicit FMA
/// still uses its fused builtin. Source assemblers may admit this exact prefix;
/// it contains only compiler-specific pragmas, never conditional declarations.
inline constexpr llvm::StringLiteral ContractionPolicy =
    "#if defined(_MSC_VER) && !defined(__clang__)\n"
    "#pragma fp_contract(off)\n"
    "#elif defined(__GNUC__) && !defined(__clang__)\n"
    "#pragma GCC optimize (\"fp-contract=off\")\n"
    "#else\n"
    "#pragma STDC FP_CONTRACT OFF\n"
    "#endif\n\n";

inline void writeContractionPolicy(llvm::raw_ostream &OS) {
  OS << ContractionPolicy;
}

} // namespace neverd::c_float

#endif
