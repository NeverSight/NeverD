//===- AArch64PAuthLoadC.h - Combined load C projection ---------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// The fixed registers and original instruction preserve combined PAC failure,
/// memory ordering and writeback. A validated raw-SP source uses physical SP
/// within the original instruction and restores the host stack on completion.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_C_RENDER_AARCH64_AARCH64PAUTHLOADC_H
#define NEVERD_BACKEND_C_RENDER_AARCH64_AARCH64PAUTHLOADC_H

#include "neverd/backend/c/render/AArch64/AArch64PAuthC.h"
#include "neverd/ir/high/AArch64PAuthLoad.h"
#include "neverd/ir/intrinsics/AArch64/AArch64PAuthLoad.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/FormatVariadic.h"

#include <stdexcept>
#include <string>

namespace neverd {

#define NEVERD_A64_PAUTH_LOAD_C_TEXT(Name, Text)                               \
  inline constexpr char Name[] = Text;
#include "neverd/backend/c/render/AArch64/AArch64PAuthLoadC.def"

inline std::string
aarch64PAuthLoadC(const AArch64PAuthLoadInstruction &Instruction,
                  llvm::StringRef Base) {
  const auto Validated = aarch64PAuthLoadInstruction(Instruction.Word);
  if (!Validated || Validated->Id != Instruction.Id)
    throw std::invalid_argument(kPAuthLoadMalformed);
  const auto ValueName = aarch64PAuthTemporary(kPAuthLoadCValue, {Base});
  const auto BaseName =
      aarch64PAuthTemporary(kPAuthLoadCBase, {Base, ValueName});
  return llvm::formatv(kPAuthLoadCExpression, ValueName, BaseName, Base,
                       Instruction.scratchWord(),
                       Instruction.stackBase() ? kPAuthLoadCSPAsm
                                               : kPAuthLoadCAsm,
                       Instruction.stackBase() ? kPAuthLoadCSPClobbers
                                               : kPAuthLoadCClobbers)
      .str();
}

} // namespace neverd

#endif // NEVERD_BACKEND_C_RENDER_AARCH64_AARCH64PAUTHLOADC_H
