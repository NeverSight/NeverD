//===- AArch64StructureMemoryC.h - Native structure C ----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Complete private vector results do not impose a public vector calling ABI.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_C_RENDER_AARCH64_AARCH64STRUCTUREMEMORYC_H
#define NEVERD_BACKEND_C_RENDER_AARCH64_AARCH64STRUCTUREMEMORYC_H

#include "neverd/backend/c/render/AArch64/AArch64AddressMemoryC.h"
#include "neverd/backend/c/render/AArch64/AArch64PAuthC.h"
#include "neverd/ir/intrinsics/AArch64/AArch64StructureMemory.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/FormatVariadic.h"

#include <stdexcept>
#include <string>
#include <vector>

namespace neverd {
#define NEVERD_A64_STRUCTURE_C_TEXT(Name, Value)                               \
  inline constexpr char Name[] = Value;
#include "neverd/backend/c/render/AArch64/AArch64StructureMemoryC.def"

inline std::string aarch64StructureMemoryC(
    const AArch64StructureMemoryInstruction &I, llvm::StringRef Base,
    llvm::StringRef Index, llvm::ArrayRef<std::string> Data,
    llvm::ArrayRef<std::string> Outputs = {}, bool VectorValues = false) {
  if (!aarch64StructureMemoryInstruction(I.Word) ||
      Data.size() != ((!I.isLoad() || I.laneLoad()) ? I.vectors() : 0) ||
      Outputs.size() != (I.isLoad() ? I.vectors() + 1 : 0))
    throw std::invalid_argument(kAddressMemoryMalformed);
  std::vector<llvm::StringRef> Used{Base, Index};
  for (const auto &D : Data)
    Used.push_back(D);
  for (const auto &O : Outputs)
    Used.push_back(O);
  std::vector<std::string> Names;
  Names.reserve(2 * kStructureMaxVectors + 4);
  const auto Name = [&](llvm::StringRef Prefix) {
    Names.push_back(aarch64PAuthTemporary(Prefix, Used));
    Used.push_back(Names.back());
    return Names.back();
  };
  std::string Result =
      llvm::formatv(I.isLoad() ? kStructureCOpen : kStructureCExpressionOpen)
          .str();
  const auto BaseInput = Name(kStructureCInputName);
  const auto IndexInput = Name(kStructureCInputName);
  Result += llvm::formatv(kStructureCScalar, BaseInput, Base).str();
  Result += llvm::formatv(kStructureCScalar, IndexInput, Index).str();
  std::vector<std::string> DataInputs;
  for (const auto &D : Data) {
    DataInputs.push_back(Name(kStructureCInputName));
    Result += llvm::formatv(kStructureCData, DataInputs.back(), D).str();
  }
  const auto BaseRegister = Name(kStructureCBaseName);
  const auto IndexRegister = Name(kStructureCIndexName);
  Result +=
      llvm::formatv(kStructureCBaseRegister, BaseRegister, BaseInput).str();
  Result +=
      llvm::formatv(kStructureCIndexRegister, IndexRegister, IndexInput).str();
  std::vector<std::string> Values;
  std::string Out, In;
  const auto Append = [](std::string &To, llvm::StringRef Constraint,
                         llvm::StringRef Value) {
    if (!To.empty())
      To += kStructureCComma;
    To += llvm::formatv(kStructureCOutput, Constraint, Value).str();
  };
  for (unsigned N = 0; N != I.vectors(); ++N) {
    Values.push_back(Name(kStructureCValueName));
    const auto Initializer =
        Data.empty()
            ? std::string()
            : llvm::formatv(
                  kStructureCInitializer,
                  llvm::formatv(kAddressVectorFromBitsC, DataInputs[N]).str())
                  .str();
    Result += llvm::formatv(kStructureCVectorRegister, Values.back(),
                            kStructureScratchVector + N, Initializer)
                  .str();
    if (I.isLoad())
      Append(Out, I.laneLoad() ? kStructureCVectorInOut : kStructureCVectorOut,
             Values.back());
    else
      Append(In, kStructureCVectorIn, Values.back());
  }
  Append(Out, kStructureCBaseOut, BaseRegister);
  Append(In, kStructureCIndexIn, IndexRegister);
  // GNU numbers all read/write outputs once, before inputs. Tied inputs from
  // '+' constraints are implicit and do not change the immediate's number.
  const unsigned Immediate =
      (I.isLoad() ? I.vectors() + 1 : 1) + 1 + (I.isLoad() ? 0 : I.vectors());
  Result += llvm::formatv(I.stackBase() ? kStructureCSPAsm : kStructureCAsm,
                          Immediate, Out, In, I.scratchWord())
                .str();
  if (!I.isLoad())
    return Result +
           llvm::formatv(kStructureCExpressionClose, BaseRegister).str();
  for (unsigned N = 0; N != I.vectors(); ++N)
    if (!Outputs[N].empty())
      Result += llvm::formatv(
                    kStructureCAssignment, Outputs[N],
                    VectorValues
                        ? Values[N]
                        : llvm::formatv(kAddressVectorToBitsC, Values[N]).str())
                    .str();
  if (!Outputs.back().empty())
    Result += llvm::formatv(kStructureCAssignment, Outputs.back(), BaseRegister)
                  .str();
  return Result + kStructureCClose;
}
} // namespace neverd

#endif // NEVERD_BACKEND_C_RENDER_AARCH64_AARCH64STRUCTUREMEMORYC_H
