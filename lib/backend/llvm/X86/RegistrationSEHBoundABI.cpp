//===- RegistrationSEHBoundABI.cpp - Native SEH call signatures -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Materialize ABI types after the source invocation has been authenticated.

#include "neverd/backend/llvm/X86RegistrationSEHBoundCall.h"
#include "neverd/ir/med/MedIR.h"

#include "llvm/IR/DerivedTypes.h"

namespace neverd {

llvm::CallingConv::ID
getX86RegistrationSEHBoundConvention(const RegistrationSEHBoundCall &Call) {
  return Call.CalleeKind == RegistrationSEHBoundCall::Kind::RaiseException ||
                 Call.CalleeKind ==
                     RegistrationSEHBoundCall::Kind::GetStartupInfoW ||
                 Call.CalleeKind ==
                     RegistrationSEHBoundCall::Kind::HeapTermination ||
                 Call.CalleeKind == RegistrationSEHBoundCall::Kind::
                                        InterlockedCompareExchange ||
                 Call.CalleeKind == RegistrationSEHBoundCall::Kind::Sleep
             ? llvm::CallingConv::X86_StdCall
             : llvm::CallingConv::C;
}

llvm::StringRef
getX86RegistrationSEHBoundImport(const RegistrationSEHBoundCall &Call) {
  switch (Call.CalleeKind) {
  case RegistrationSEHBoundCall::Kind::PreservedHelper:
    return {};
  case RegistrationSEHBoundCall::Kind::RaiseException:
    return "RaiseException";
  case RegistrationSEHBoundCall::Kind::ControlFP:
    return "_controlfp_s";
  case RegistrationSEHBoundCall::Kind::GetStartupInfoW:
    return "GetStartupInfoW";
  case RegistrationSEHBoundCall::Kind::HeapTermination:
    return "HeapSetInformation";
  case RegistrationSEHBoundCall::Kind::InterlockedCompareExchange:
    return "InterlockedCompareExchange";
  case RegistrationSEHBoundCall::Kind::Sleep:
    return "Sleep";
  }
  llvm_unreachable("unvalidated native SEH callee kind");
}

bool usesX86RegistrationSEHImportSlot(const X86RegistrationSEHBoundCall &Call) {
  return Call.Source && Call.Operation &&
         !getX86RegistrationSEHBoundImport(*Call.Source).empty() &&
         Call.Operation->Opcode == NdOp::INDIR_CALL;
}

llvm::FunctionType *
getX86RegistrationSEHBoundType(const RegistrationSEHBoundCall &Call,
                               llvm::LLVMContext &Context) {
  auto *I32 = llvm::Type::getInt32Ty(Context);
  auto *Ptr = llvm::PointerType::getUnqual(Context);
  llvm::SmallVector<llvm::Type *, 8> Arguments;
  for (const auto &Arg : Call.Arguments) {
    using Kind = RegistrationSEHBoundArgument::Kind;
    Arguments.push_back(Arg.ArgumentKind == Kind::Object ||
                                Arg.ArgumentKind == Kind::JumpBuffer ||
                                Arg.ArgumentKind == Kind::ImagePointer ||
                                Arg.ArgumentKind == Kind::NullPointer
                            ? static_cast<llvm::Type *>(Ptr)
                            : I32);
  }
  auto *Return =
      Call.CalleeKind == RegistrationSEHBoundCall::Kind::RaiseException ||
              Call.CalleeKind ==
                  RegistrationSEHBoundCall::Kind::GetStartupInfoW ||
              Call.CalleeKind == RegistrationSEHBoundCall::Kind::Sleep
          ? llvm::Type::getVoidTy(Context)
          : I32;
  return llvm::FunctionType::get(Return, Arguments, false);
}

} // namespace neverd
