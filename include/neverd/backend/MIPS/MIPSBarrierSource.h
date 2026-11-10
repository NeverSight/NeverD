//===- MIPSBarrierSource.h - Native ordering barrier identity -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Keep execution hazards and all SYNC stypes native. Classic helpers keep
/// barrier assembly valid when the surrounding source uses compressed modes.
//===----------------------------------------------------------------------===//
#ifndef NEVERD_BACKEND_MIPS_MIPSBARRIERSOURCE_H
#define NEVERD_BACKEND_MIPS_MIPSBARRIERSOURCE_H

#include "neverd/backend/MIPS/MIPSInstructionModeSource.h"
#include "neverd/ir/intrinsics/MIPS/MIPSScalar.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Instructions.h"

#include <optional>
#include <stdexcept>
#include <string>

namespace neverd::mips {
#define NEVERD_MIPS_BARRIER_TEXT(Name, Text)                                   \
  inline constexpr char Name[] = Text;
#include "neverd/backend/MIPS/MIPSBarrierSource.def"

inline std::string barrierAssembly(Intrinsic Id, unsigned Stype = 0) {
  return Id == Intrinsic::MIPSExecutionHazardBarrier
             ? std::string(ExecutionHazardAsm)
             : std::string(SyncAsmPrefix) + std::to_string(Stype);
}

inline std::optional<llvm::StringRef>
nativeBarrierSource(const llvm::CallInst &Call) {
  auto *Asm = llvm::dyn_cast<llvm::InlineAsm>(Call.getCalledOperand());
  if (!Asm)
    return std::nullopt;
  const auto &Text = Asm->getAsmString();
  bool Recognized = Text == ExecutionHazardAsm;
  if (!Recognized && !llvm::StringRef(Text).starts_with(SyncAsmPrefix))
    return std::nullopt;
  for (unsigned Stype = 0; !Recognized && Stype != 32; ++Stype)
    Recognized = Text == barrierAssembly(Intrinsic::MIPSSync, Stype);
  if (!Recognized)
    return std::nullopt;
  if (!Asm->hasSideEffects() || Asm->isAlignStack() || Asm->canThrow() ||
      Asm->getDialect() != llvm::InlineAsm::AD_ATT ||
      Asm->getConstraintString() != BarrierConstraints || Call.arg_size() ||
      !Call.getType()->isVoidTy() ||
      !Call.getFunction()->hasFnAttribute(BarrierSourceMarker) ||
      !Call.getFunction()->hasFnAttribute(llvm::Attribute::NoInline) ||
      instructionModeFromLLVM(*Call.getFunction()) !=
          InstructionMode::MIPSClassic)
    throw std::invalid_argument(MalformedBarrierSource);
  return Text;
}
} // namespace neverd::mips

#endif // NEVERD_BACKEND_MIPS_MIPSBARRIERSOURCE_H
