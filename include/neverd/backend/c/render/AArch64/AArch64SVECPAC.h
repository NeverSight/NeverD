//===- AArch64SVECPAC.h - Scalable checked pointer C emission -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Real CPA instructions over canonical local Z/P values. This does not
/// supply a scalable calling convention or a fixed-width substitute for Z.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_C_RENDER_AARCH64_AARCH64SVECPAC_H
#define NEVERD_BACKEND_C_RENDER_AARCH64_AARCH64SVECPAC_H

#include "neverd/backend/c/render/AArch64/AArch64PAuthC.h"
#include "neverd/ir/intrinsics/AArch64/AArch64SVEVector.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/FormatVariadic.h"

#include <stdexcept>
#include <string>

namespace neverd {

#define NEVERD_A64_SVE_CPA_C_TEXT(Name, Value)                                 \
  inline constexpr char Name[] = Value;
#include "neverd/backend/c/render/AArch64/AArch64SVECPAC.def"

inline std::string aarch64SVECPAC(const aarch64_sve::CPAOperation &Operation,
                                  llvm::ArrayRef<std::string> Args) {
  const auto Roles = aarch64_sve::cpaRoles(Operation.Form);
  if (Args.size() != Roles.Count)
    throw std::invalid_argument(aarch64_sve::kMalformedVector);
  const auto Third =
      Args.size() > 2 ? llvm::StringRef(Args[2]) : llvm::StringRef();
  const auto Name =
      aarch64PAuthTemporary(kCPATemporary, {Args[0], Args[1], Third});
  if (Operation.Form == aarch64_sve::CPAForm::Binary)
    return llvm::formatv(kCPABinary, Name, Operation.MachineAsm, Args[0],
                         Args[1])
        .str();
  const char *Format = Operation.Form == aarch64_sve::CPAForm::Merging
                           ? kCPAMerging
                           : kCPAMultiply;
  return llvm::formatv(Format, Name, Operation.MachineAsm, Args[0], Args[1],
                       Third)
      .str();
}

} // namespace neverd

#endif // NEVERD_BACKEND_C_RENDER_AARCH64_AARCH64SVECPAC_H
