//===- AArch64AddressMemoryC.h - Original memory C contract -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Both C projections retain the same original memory instruction. An admitted
/// explicit raw SP selects the native SP for the single access and restores the
/// host SP on normal completion; it never becomes a general-register base.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_C_RENDER_AARCH64_AARCH64ADDRESSMEMORYC_H
#define NEVERD_BACKEND_C_RENDER_AARCH64_AARCH64ADDRESSMEMORYC_H

#include "neverd/backend/c/render/AArch64/AArch64PAuthC.h"
#include "neverd/ir/high/AArch64AddressMemory.h"
#include "neverd/ir/intrinsics/AArch64/AArch64AddressMemory.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/FormatVariadic.h"

#include <stdexcept>
#include <string>

namespace neverd {

#define NEVERD_A64_ADDRESS_MEMORY_C_TEXT(Name, Text)                           \
  inline constexpr char Name[] = Text;
#include "neverd/backend/c/render/AArch64/AArch64AddressMemoryC.def"

inline std::string
aarch64AddressMemoryC(const AArch64AddressMemoryInstruction &Instruction,
                      llvm::StringRef Base, llvm::StringRef Index,
                      llvm::StringRef Data = {}) {
  if (!aarch64AddressMemoryInstruction(Instruction.Word))
    throw std::invalid_argument(kAddressMemoryMalformed);
  if (Instruction.isLoad() != Data.empty())
    throw std::invalid_argument(kAddressMemoryMalformed);
  const auto *Asm = Instruction.isLoad()
                        ? (Instruction.stackBase() ? kAddressMemorySPLoadCAsm
                                                   : kAddressMemoryLoadCAsm)
                        : (Instruction.stackBase() ? kAddressMemorySPStoreCAsm
                                                   : kAddressMemoryStoreCAsm);
  const auto *Clobbers = Instruction.stackBase() ? kAddressMemorySPCClobbers
                                                 : kAddressMemoryCClobbers;
  const auto ValueName =
      aarch64PAuthTemporary(kAddressLoadCValue, {Base, Index, Data});
  const auto BaseName =
      aarch64PAuthTemporary(kAddressLoadCBase, {Base, Index, Data, ValueName});
  const auto IndexName = aarch64PAuthTemporary(
      kAddressLoadCIndex, {Base, Index, Data, ValueName, BaseName});
  const auto BaseInput = aarch64PAuthTemporary(
      kAddressLoadCBaseInput,
      {Base, Index, Data, ValueName, BaseName, IndexName});
  const auto IndexInput = aarch64PAuthTemporary(
      kAddressLoadCIndexInput,
      {Base, Index, Data, ValueName, BaseName, IndexName, BaseInput});
  if (!Instruction.isLoad()) {
    const auto DataInput = aarch64PAuthTemporary(
        kAddressStoreCDataInput, {Base, Index, Data, ValueName, BaseName,
                                  IndexName, BaseInput, IndexInput});
    if (Instruction.isVector())
      return llvm::formatv(
                 kAddressVectorStoreCExpression, ValueName, BaseName, IndexName,
                 Base, Index, Instruction.scratchWord(), BaseInput, IndexInput,
                 DataInput, Data,
                 llvm::formatv(kAddressVectorFromBitsC, DataInput).str(), Asm,
                 Clobbers)
          .str();
    return llvm::formatv(kAddressStoreCExpression, ValueName, BaseName,
                         IndexName, Base, Index, Instruction.scratchWord(),
                         BaseInput, IndexInput, DataInput, Data,
                         llvm::StringRef{}, Asm, Clobbers)
        .str();
  }
  if (Instruction.isVector())
    return llvm::formatv(kAddressVectorLoadCExpression, ValueName, BaseName,
                         IndexName, Base, Index, Instruction.scratchWord(),
                         BaseInput, IndexInput, Asm, Clobbers,
                         llvm::formatv(kAddressVectorToBitsC, ValueName).str())
        .str();
  return llvm::formatv(kAddressLoadCExpression, ValueName, BaseName, IndexName,
                       Base, Index, Instruction.scratchWord(), BaseInput,
                       IndexInput, Asm, Clobbers)
      .str();
}

} // namespace neverd

#endif // NEVERD_BACKEND_C_RENDER_AARCH64_AARCH64ADDRESSMEMORYC_H
