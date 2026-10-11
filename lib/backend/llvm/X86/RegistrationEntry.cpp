//===- RegistrationEntry.cpp - PE32 C++ parent entry ABI -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Select the LLVM convention for an authenticated physical entry signature.
//===----------------------------------------------------------------------===//
#include "neverd/backend/llvm/X86RegistrationEntry.h"
#include "neverd/ir/med/MedIR.h"
#include "neverd/ir/med/X86RegistrationEntry.h"

#include "llvm/IR/Function.h"

namespace neverd {
namespace {
std::optional<llvm::CallingConv::ID>
entryConvention(const MedFunc &Source, const llvm::Function &Function) {
  const auto ABI = projectX86RegistrationEntry(Source);
  if (!ABI || Source.Params.size() != Function.arg_size() ||
      Function.isVarArg())
    return std::nullopt;
  for (const auto &Arg : Function.args())
    if (!Arg.getType()->isIntegerTy(32))
      return std::nullopt;
  if (ABI->RegisterCount == 2)
    return llvm::CallingConv::X86_FastCall;
  if (ABI->RegisterCount == 1)
    return llvm::CallingConv::X86_ThisCall;
  return ABI->PopBytes ? llvm::CallingConv::X86_StdCall : llvm::CallingConv::C;
}
} // namespace

std::optional<llvm::CallingConv::ID>
getX86RegistrationCxxEntryABI(const MedFunc &Source,
                              const llvm::Function &Function) {
  for (const auto &Arg : Function.args())
    if (Function.getAttributes().hasParamAttrs(Arg.getArgNo()))
      return std::nullopt;
  return entryConvention(Source, Function);
}

bool hasX86RegistrationCxxEntryABI(const MedFunc &Source,
                                   const llvm::Function &Function) {
  const auto Convention = entryConvention(Source, Function);
  if (!Convention || Function.getCallingConv() != *Convention)
    return false;
  for (const auto &Arg : Function.args()) {
    const auto Attributes =
        Function.getAttributes().getParamAttrs(Arg.getArgNo());
    const bool InReg =
        *Convention == llvm::CallingConv::X86_FastCall && Arg.getArgNo() < 2;
    if (Attributes.getNumAttributes() != unsigned(InReg) ||
        (InReg && !Attributes.hasAttribute(llvm::Attribute::InReg)))
      return false;
  }
  return true;
}
} // namespace neverd
