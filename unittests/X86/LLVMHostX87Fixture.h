//===- LLVMHostX87Fixture.h - Host x87 fixtures ---------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Adapt the cloned host-compiler fixture without changing production IR.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_UNITTESTS_X86_LLVMHOSTX87FIXTURE_H
#define NEVERD_UNITTESTS_X86_LLVMHOSTX87FIXTURE_H

#include "neverd/backend/llvm/X86/LLVMX87NativeState.h"

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Module.h"
#include "llvm/TargetParser/Triple.h"

namespace neverd::test {

inline bool lowerHostX87State(llvm::Instruction &Instruction) {
  auto *Call = llvm::dyn_cast<llvm::CallBase>(&Instruction);
  if (!Call)
    return false;
  const auto Target = Call->getModule()->getTargetTriple().getArch();
  const Arch Mode = Target == llvm::Triple::x86      ? Arch::X86
                    : Target == llvm::Triple::x86_64 ? Arch::X64
                                                     : Arch::Unknown;
  const auto *Spec = classifyX87NativeState(*Call, Mode);
  if (!Spec)
    return false;

  std::string Assembly = Spec->Mnemonic;
  std::string Constraints;
  if (Spec->HasPointer) {
    Assembly += " ($0)";
    Constraints = "r,";
  } else if (Spec->ReturnsStatus) {
    Assembly += " $0";
    Constraints = "={ax},";
  }
  Constraints += x87StateConstraints(x87NativeStateEffect(*Spec)).str();
  auto *Operation = llvm::InlineAsm::get(Call->getFunctionType(), Assembly,
                                         Constraints, true);
  llvm::SmallVector<llvm::Value *, 1> Arguments(Call->args());
  llvm::IRBuilder<> Builder(Call);
  auto *Replacement = Builder.CreateCall(Operation, Arguments);
  Replacement->copyMetadata(*Call);
  Replacement->takeName(Call);
  Call->replaceAllUsesWith(Replacement);
  Call->eraseFromParent();
  return true;
}

} // namespace neverd::test

#endif // NEVERD_UNITTESTS_X86_LLVMHOSTX87FIXTURE_H
