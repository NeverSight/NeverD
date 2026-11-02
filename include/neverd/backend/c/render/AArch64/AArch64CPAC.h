//===- AArch64CPAC.h - Checked pointer C projection ------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Preserve checked arithmetic in both C routes without predicting SCTLR2.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_C_RENDER_AARCH64_AARCH64CPAC_H
#define NEVERD_BACKEND_C_RENDER_AARCH64_AARCH64CPAC_H

#include "neverd/backend/c/render/AArch64/AArch64PAuthC.h"
#include "neverd/ir/intrinsics/AArch64/AArch64CPA.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/FormatVariadic.h"

#include <stdexcept>
#include <string>

namespace neverd {

#define NEVERD_A64_CPA_C_TEXT(Name, Text) inline constexpr char Name[] = Text;
#include "neverd/backend/c/render/AArch64/AArch64CPAC.def"

inline std::string aarch64CPAC(const AArch64CPAOperation &Operation,
                               llvm::ArrayRef<std::string> Args) {
  if (Args.size() != Operation.Sources)
    throw std::invalid_argument(kAArch64CPAMalformedOperation);
  const std::string Name = aarch64PAuthTemporary(
      kAArch64CPACTemporary,
      {Args[0], Args[1],
       Args.size() == 3 ? llvm::StringRef(Args[2]) : llvm::StringRef()});
  if (Args.size() == 2)
    return llvm::formatv(kAArch64CPACBinary, Name, Operation.MachineAsm,
                         Args[0], Args[1])
        .str();
  return llvm::formatv(kAArch64CPACTernary, Name, Operation.MachineAsm, Args[0],
                       Args[1], Args[2])
      .str();
}

} // namespace neverd

#endif // NEVERD_BACKEND_C_RENDER_AARCH64_AARCH64CPAC_H
