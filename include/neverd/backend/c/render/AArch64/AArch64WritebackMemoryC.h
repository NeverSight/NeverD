//===- AArch64WritebackMemoryC.h - Writeback C projection ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Shared C projection retains access and writeback in one native instruction.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_C_RENDER_AARCH64_AARCH64WRITEBACKMEMORYC_H
#define NEVERD_BACKEND_C_RENDER_AARCH64_AARCH64WRITEBACKMEMORYC_H

#include "neverd/backend/c/render/AArch64/AArch64AddressMemoryC.h"
#include "neverd/backend/c/render/AArch64/AArch64PAuthC.h"
#include "neverd/ir/high/AArch64WritebackMemory.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/FormatVariadic.h"

#include <array>
#include <stdexcept>
#include <string>

namespace neverd {

#define NEVERD_A64_WRITEBACK_C_TEXT(Name, Text)                                \
  inline constexpr char Name[] = Text;
#include "neverd/backend/c/render/AArch64/AArch64WritebackMemoryC.def"

inline const char *
aarch64WritebackMemoryCAsm(const AArch64WritebackMemoryInstruction &I) {
  return I.isPair() ? (I.stackBase() ? kPairCSPAsm : kPairCAsm)
                    : (I.stackBase() ? kWritebackCSPAsm : kWritebackCAsm);
}

inline const char *
aarch64WritebackMemoryCClobbers(const AArch64WritebackMemoryInstruction &I) {
  return I.stackBase() ? kAddressMemorySPCClobbers : kAddressMemoryCClobbers;
}

inline std::string aarch64PairMemoryCStatement(
    const AArch64WritebackMemoryInstruction &Instruction, llvm::StringRef Base,
    const std::array<std::string, kPairBaseIndex + 1> &Outputs,
    bool VectorValues = false) {
  if (!aarch64WritebackMemoryInstruction(Instruction.Word) ||
      !Instruction.isPair() || !Instruction.isLoad())
    throw std::invalid_argument(kWritebackMalformed);
  const auto First =
      aarch64PAuthTemporary(kWritebackCValue, {Base, Outputs[0], Outputs[1],
                                               Outputs[kPairBaseIndex]});
  const auto Second =
      aarch64PAuthTemporary(kPairCSecond, {Base, Outputs[0], Outputs[1],
                                           Outputs[kPairBaseIndex], First});
  const auto Updated = aarch64PAuthTemporary(
      kWritebackCBase,
      {Base, Outputs[0], Outputs[1], Outputs[kPairBaseIndex], First, Second});
  const auto Input = aarch64PAuthTemporary(
      kWritebackCBase, {Base, Outputs[0], Outputs[1], Outputs[kPairBaseIndex],
                        First, Second, Updated});
  std::string Assignments;
  const std::array<std::string, kPairBaseIndex + 1> Values{First, Second,
                                                           Updated};
  for (unsigned I = 0; I != Values.size(); ++I) {
    if (Outputs[I].empty())
      continue;
    const auto Value =
        Instruction.isVector() && I < kPairValues && !VectorValues
            ? llvm::formatv(kAddressVectorToBitsC, Values[I]).str()
            : Values[I];
    Assignments +=
        llvm::formatv(kWritebackCAssignment, Outputs[I], Value).str();
  }
  return llvm::formatv(Instruction.isVector() ? kPairVectorLoadCStatement
                                              : kPairLoadCStatement,
                       First, Second, Updated, Input, Base,
                       Instruction.scratchWord(), Assignments,
                       aarch64WritebackMemoryCAsm(Instruction),
                       aarch64WritebackMemoryCClobbers(Instruction))
      .str();
}

inline std::string
aarch64PairStoreMemoryC(const AArch64WritebackMemoryInstruction &Instruction,
                        llvm::StringRef Base, llvm::StringRef FirstData,
                        llvm::StringRef SecondData) {
  if (!aarch64WritebackMemoryInstruction(Instruction.Word) ||
      !Instruction.isPair() || Instruction.isLoad() || FirstData.empty() ||
      SecondData.empty())
    throw std::invalid_argument(kWritebackMalformed);
  const auto First =
      aarch64PAuthTemporary(kWritebackCValue, {Base, FirstData, SecondData});
  const auto Second =
      aarch64PAuthTemporary(kPairCSecond, {Base, FirstData, SecondData, First});
  const auto Updated = aarch64PAuthTemporary(
      kWritebackCBase, {Base, FirstData, SecondData, First, Second});
  const auto Input = aarch64PAuthTemporary(
      kWritebackCBase, {Base, FirstData, SecondData, First, Second, Updated});
  const auto FirstInput =
      aarch64PAuthTemporary(kPairCFirstInput, {Base, FirstData, SecondData,
                                               First, Second, Updated, Input});
  const auto SecondInput = aarch64PAuthTemporary(
      kPairCSecondInput,
      {Base, FirstData, SecondData, First, Second, Updated, Input, FirstInput});
  if (Instruction.isVector())
    return llvm::formatv(
               kPairVectorStoreCExpression, First, Second, Updated, Input,
               FirstInput, SecondInput, Base, FirstData, SecondData,
               Instruction.scratchWord(),
               llvm::formatv(kAddressVectorFromBitsC, FirstInput).str(),
               llvm::formatv(kAddressVectorFromBitsC, SecondInput).str(),
               aarch64WritebackMemoryCAsm(Instruction),
               aarch64WritebackMemoryCClobbers(Instruction))
        .str();
  return llvm::formatv(kPairStoreCExpression, First, Second, Updated, Input,
                       FirstInput, SecondInput, Base, FirstData, SecondData,
                       Instruction.scratchWord(),
                       aarch64WritebackMemoryCAsm(Instruction),
                       aarch64WritebackMemoryCClobbers(Instruction))
      .str();
}

inline std::string
aarch64WritebackMemoryC(const AArch64WritebackMemoryInstruction &Instruction,
                        llvm::StringRef Base, llvm::StringRef Data = {}) {
  if (!aarch64WritebackMemoryInstruction(Instruction.Word) ||
      Instruction.isLoad() != Data.empty())
    throw std::invalid_argument(kWritebackMalformed);
  const auto ValueName = aarch64PAuthTemporary(kWritebackCValue, {Base, Data});
  const auto BaseName =
      aarch64PAuthTemporary(kWritebackCBase, {Base, Data, ValueName});
  if (Instruction.isPair() || (Instruction.isVector() && Instruction.isLoad()))
    throw std::invalid_argument(kWritebackMalformed);
  if (Instruction.isLoad())
    return llvm::formatv(kWritebackLoadCExpression, ValueName, BaseName, Base,
                         Instruction.scratchWord(),
                         aarch64WritebackMemoryCAsm(Instruction),
                         aarch64WritebackMemoryCClobbers(Instruction))
        .str();
  const auto BaseInput =
      aarch64PAuthTemporary(kWritebackCBase, {Base, Data, ValueName, BaseName});
  const auto DataInput = aarch64PAuthTemporary(
      kWritebackCData, {Base, Data, ValueName, BaseName, BaseInput});
  if (Instruction.isVector())
    return llvm::formatv(
               kWritebackVectorStoreCExpression, ValueName, BaseName, Base,
               Instruction.scratchWord(), BaseInput, DataInput, Data,
               llvm::formatv(kAddressVectorFromBitsC, DataInput).str(),
               aarch64WritebackMemoryCAsm(Instruction),
               aarch64WritebackMemoryCClobbers(Instruction))
        .str();
  return llvm::formatv(kWritebackStoreCExpression, ValueName, BaseName, Base,
                       Instruction.scratchWord(), BaseInput, DataInput, Data,
                       aarch64WritebackMemoryCAsm(Instruction),
                       aarch64WritebackMemoryCClobbers(Instruction))
      .str();
}

inline std::string aarch64VectorWritebackMemoryCStatement(
    const AArch64WritebackMemoryInstruction &Instruction, llvm::StringRef Base,
    llvm::StringRef ValueOutput, llvm::StringRef BaseOutput,
    bool VectorValue = false) {
  if (!aarch64WritebackMemoryInstruction(Instruction.Word) ||
      !Instruction.isVector() || !Instruction.isLoad())
    throw std::invalid_argument(kWritebackMalformed);
  const auto Value =
      aarch64PAuthTemporary(kWritebackCValue, {Base, ValueOutput, BaseOutput});
  const auto Updated = aarch64PAuthTemporary(
      kWritebackCBase, {Base, ValueOutput, BaseOutput, Value});
  const auto Input = aarch64PAuthTemporary(
      kWritebackCBase, {Base, ValueOutput, BaseOutput, Value, Updated});
  std::string Assignments;
  if (!ValueOutput.empty())
    Assignments +=
        llvm::formatv(kWritebackCAssignment, ValueOutput,
                      VectorValue
                          ? Value
                          : llvm::formatv(kAddressVectorToBitsC, Value).str())
            .str();
  if (!BaseOutput.empty())
    Assignments +=
        llvm::formatv(kWritebackCAssignment, BaseOutput, Updated).str();
  return llvm::formatv(kWritebackVectorLoadCStatement, Value, Updated, Base,
                       Instruction.scratchWord(), Input, Assignments,
                       aarch64WritebackMemoryCAsm(Instruction),
                       aarch64WritebackMemoryCClobbers(Instruction))
      .str();
}

} // namespace neverd

#endif // NEVERD_BACKEND_C_RENDER_AARCH64_AARCH64WRITEBACKMEMORYC_H
