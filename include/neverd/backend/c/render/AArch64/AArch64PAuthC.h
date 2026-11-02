//===- AArch64PAuthC.h - Shared pointer authentication C text ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Both C projections preserve machine PAC/AUT's tied pointer and keep a
/// control-flow authentication observable even when its value is unused.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_C_RENDER_AARCH64_AARCH64PAUTHC_H
#define NEVERD_BACKEND_C_RENDER_AARCH64_AARCH64PAUTHC_H

#include "neverd/backend/c/render/CTypeFormat.h"
#include "neverd/ir/intrinsics/AArch64/AArch64PAuth.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/FormatVariadic.h"

#include <stdexcept>
#include <string>

namespace neverd {

#define NEVERD_A64_PAUTH_C_TEXT(Name, Text) inline constexpr char Name[] = Text;
#include "neverd/backend/c/render/AArch64/AArch64PAuthC.def"

/// The initializer and asm inputs must refer to their enclosing C scope.
/// Grow the name until it occurs in none of the supplied operand expressions.
inline std::string
aarch64PAuthTemporary(llvm::StringRef Stem,
                      llvm::ArrayRef<llvm::StringRef> Inputs) {
  std::string Name = Stem.str();
  while (llvm::any_of(
      Inputs, [&](llvm::StringRef Input) { return Input.contains(Name); }))
    Name += '_';
  return Name;
}

inline std::string aarch64PAuthMachineC(const AArch64PAuthOperation &Operation,
                                        llvm::StringRef Pointer,
                                        llvm::StringRef Modifier = {}) {
  const std::string Name =
      aarch64PAuthTemporary(kAArch64PAuthCPointer, {Pointer, Modifier});
  return "({ " +
         llvm::formatv(kAArch64PAuthCPointerDeclaration, Name, Pointer).str() +
         llvm::formatv(
             kAArch64PAuthCMachineAsm, Operation.Mnemonic,
             Operation.Strip ? kAArch64PAuthCStripOperands
                             : kAArch64PAuthCOperands,
             Name,
             Operation.Strip
                 ? std::string()
                 : llvm::formatv(kAArch64PAuthCModifier, Modifier).str())
             .str() +
         Name + "; })";
}

inline std::string aarch64PAuthLRC(const AArch64PAuthLROperation &Operation,
                                   llvm::ArrayRef<std::string> Args) {
  if (Args.size() != Operation.operandCount())
    throw std::invalid_argument(kAArch64PAuthMalformedOperation);
  llvm::SmallVector<llvm::StringRef, 4> Inputs(Args.begin(), Args.end());
  const std::string Name = aarch64PAuthTemporary(kAArch64PAuthCPointer, Inputs);
  if (Operation.ConditionalPrefix)
    return llvm::formatv(kAArch64PACMMachineC, Name,
                         escapeCString(Operation.machineAsm(/*C=*/true)),
                         Args[0], Args[1], Args[2], Args[3])
        .str();
  return llvm::formatv(kAArch64PAuthLRMachineC, Name,
                       escapeCString(Operation.llvmAsm()), Args[0], Args[1],
                       Args[2])
      .str();
}

inline std::string aarch64PAuthAuthenticateC(llvm::StringRef Pointer,
                                             llvm::StringRef Key,
                                             llvm::StringRef Modifier) {
  return llvm::formatv(kAArch64PAuthCAuthenticate,
                       intrinsicCName(Intrinsic::A64_AuthBranch), Pointer, Key,
                       Modifier)
      .str();
}

/// A C function pointer uses IA/0 under arm64e. Auth-and-resign lets Clang
/// combine that implicit authentication with the original call's key and
/// discriminator, without exposing a raw pointer or authenticating twice.
inline std::string
aarch64PAuthAuthenticatedCalleeC(llvm::StringRef FunctionPointerType,
                                 llvm::StringRef Pointer, llvm::StringRef Key,
                                 llvm::StringRef Modifier) {
  return llvm::formatv(kAArch64PAuthCAuthenticatedCallee, FunctionPointerType,
                       Pointer, Key, Modifier)
      .str();
}

/// An ordinary machine BLR consumes a raw address. A compiler using arm64e
/// must not add its default IA/0 check to that address. Signing immediately
/// before the implicit check retains the machine's unchecked call contract.
inline std::string aarch64PAuthRawCalleeC(llvm::StringRef Callee) {
  return llvm::formatv(kAArch64PAuthCRawCall, Callee).str();
}

inline std::string aarch64PAuthGenericSignC(llvm::StringRef Value,
                                            llvm::StringRef Discriminator) {
  const std::string Name =
      aarch64PAuthTemporary(kAArch64PAuthCPointer, {Value, Discriminator});
  return "({ " +
         llvm::formatv(kAArch64PAuthCGenericSign, Name,
                       intrinsicAsmMnemonic(Intrinsic::A64_Pacga), Value,
                       Discriminator)
             .str() +
         Name + "; })";
}

inline std::string aarch64PAuthTargetUseC(llvm::StringRef Target) {
  return llvm::formatv(kAArch64PAuthCTargetUse,
                       kAArch64AuthenticatedTargetUseAsm, Target)
      .str();
}

} // namespace neverd

#endif // NEVERD_BACKEND_C_RENDER_AARCH64_AARCH64PAUTHC_H
