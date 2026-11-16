//===- LLVMControlFlowFailure.h - Unproved control-flow exits -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Keep a recovery failure distinct from a machine trap in LLVM and C output.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_LLVMCONTROLFLOWFAILURE_H
#define NEVERD_BACKEND_LLVMCONTROLFLOWFAILURE_H

#include "llvm/IR/Constants.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Module.h"

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <vector>

namespace neverd::llvm_control_flow_failure {

inline constexpr llvm::StringLiteral SiteBundle("neverd.control-flow.failure");

enum class Reason : uint32_t { IndirectTargets = 1, SwitchDefault = 2 };

struct Site {
  Reason Why;
  uint64_t Address;
};

inline llvm::CallInst *create(llvm::IRBuilder<> &Builder, Reason Why,
                              uint64_t Address) {
  auto *Trap = llvm::Intrinsic::getOrInsertDeclaration(
      Builder.GetInsertBlock()->getModule(), llvm::Intrinsic::trap);
  // Unlike ordinary metadata, the bundle prevents optimization from merging
  // distinct failure sites and attributing both paths to only one address.
  llvm::OperandBundleDef Bundle(
      SiteBundle.str(),
      std::vector<llvm::Value *>{Builder.getInt32(static_cast<uint32_t>(Why)),
                                 Builder.getInt64(Address)});
  return Builder.CreateCall(Trap, {}, {Bundle});
}

inline std::optional<Site> get(const llvm::CallBase &Call) {
  const auto Bundle = Call.getOperandBundle(SiteBundle);
  if (!Bundle)
    return std::nullopt;
  const auto *Function = Call.getCalledFunction();
  const auto *Why = Bundle->Inputs.size() == 2
                        ? llvm::dyn_cast<llvm::ConstantInt>(Bundle->Inputs[0])
                        : nullptr;
  const auto *Address =
      Bundle->Inputs.size() == 2
          ? llvm::dyn_cast<llvm::ConstantInt>(Bundle->Inputs[1])
          : nullptr;
  if (!Function || Function->getIntrinsicID() != llvm::Intrinsic::trap ||
      !Call.arg_empty() || Call.getNumOperandBundles() != 1 || !Why ||
      !Why->getType()->isIntegerTy(32) || !Address ||
      !Address->getType()->isIntegerTy(64) ||
      (Why->getZExtValue() != static_cast<uint32_t>(Reason::IndirectTargets) &&
       Why->getZExtValue() != static_cast<uint32_t>(Reason::SwitchDefault)))
    throw std::invalid_argument("malformed control-flow failure site");
  return Site{static_cast<Reason>(Why->getZExtValue()),
              Address->getZExtValue()};
}

} // namespace neverd::llvm_control_flow_failure

#endif // NEVERD_BACKEND_LLVMCONTROLFLOWFAILURE_H
