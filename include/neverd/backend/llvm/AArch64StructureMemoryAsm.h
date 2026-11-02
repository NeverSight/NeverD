//===- AArch64StructureMemoryAsm.h - Owned NEON assembly -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// The emitter and inverse C renderer share the exact private asm contract.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_LLVM_AARCH64STRUCTUREMEMORYASM_H
#define NEVERD_BACKEND_LLVM_AARCH64STRUCTUREMEMORYASM_H

#include "neverd/ir/intrinsics/AArch64/AArch64StructureMemory.h"

#include "llvm/Support/FormatVariadic.h"

#include <string>

namespace neverd {
struct AArch64StructureMemoryAsm {
  std::string Text;
  std::string Constraints;
  unsigned Inputs;
  unsigned Outputs;
};

inline AArch64StructureMemoryAsm
aarch64StructureMemoryAsm(const AArch64StructureMemoryInstruction &I) {
  const bool Data = !I.isLoad() || I.laneLoad();
  AArch64StructureMemoryAsm Asm{
      I.stackBase() ? (I.isLoad() ? (I.laneLoad() ? kStructureLaneLoadSPAsm
                                                  : kStructureLoadSPAsm)
                                  : kStructureStoreSPAsm)
                    : (I.isLoad() ? (I.laneLoad() ? kStructureLaneLoadAsm
                                                  : kStructureLoadAsm)
                                  : kStructureStoreAsm),
      {},
      kStructureDataOperand + (Data ? I.vectors() : 0),
      I.isLoad() ? I.vectors() + 1 : 1};
  if (I.isLoad())
    for (unsigned N = 0; N != I.vectors(); ++N)
      Asm.Constraints += llvm::formatv(kStructureVectorOutputConstraint,
                                       kStructureScratchVector + N)
                             .str();
  Asm.Constraints += llvm::formatv(kStructureBaseOutputConstraint).str();
  Asm.Constraints +=
      llvm::formatv(kStructureTiedConstraint, Asm.Outputs - 1).str();
  Asm.Constraints += llvm::formatv(kStructureIndexConstraint).str();
  if (Data)
    for (unsigned N = 0; N != I.vectors(); ++N)
      Asm.Constraints += I.isLoad()
                             ? llvm::formatv(kStructureTiedConstraint, N).str()
                             : llvm::formatv(kStructureVectorInputConstraint,
                                             kStructureScratchVector + N)
                                   .str();
  Asm.Constraints +=
      I.stackBase() ? kStructureSPConstraintEnd : kStructureConstraintEnd;
  Asm.Text.replace(
      Asm.Text.find(kStructureAsmImmediate), sizeof(kStructureAsmImmediate) - 1,
      llvm::formatv(kStructureAsmOperand, Asm.Outputs + Asm.Inputs - 1).str());
  return Asm;
}
} // namespace neverd

#endif // NEVERD_BACKEND_LLVM_AARCH64STRUCTUREMEMORYASM_H
