//===- RegistrationCxxGuardedFrame.h - PE32 GS frames -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Close a cleanup-only C++ candidate's current frame and cookie protocol.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_LLVM_REGISTRATIONCXXGUARDEDFRAME_H
#define NEVERD_BACKEND_LLVM_REGISTRATIONCXXGUARDEDFRAME_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/Error.h"

#include <set>

namespace llvm {
class AllocaInst;
class Function;
class Instruction;
} // namespace llvm
namespace neverd {
struct BinaryImage;
struct MedFunc;
struct X86RegistrationFrameLayout;
namespace registration_frame {
struct CxxSourceCallBinding;
struct CxxCleanupCallBinding;

/// Run after all bodies, call normalization, chain projection and dead spill
/// removal. Every retained occurrence must still exist in the final module.
/// The consumer owns source execution order, full control flow and ordinary
/// call ABIs. This rechecks callee effects, source/current cookie coverage and
/// complete frame use, initialization, cookie protocol and image immutability.
/// Catch objects, incoming caller-frame accesses and direct runtime throws
/// require additional domains and remain excluded. Compiler-table and native
/// installation consumers must independently replay the current source/IR.
llvm::Error
validateCxxGuardedFrame(const BinaryImage &Image, const MedFunc &Source,
                        const llvm::Function &Parent,
                        const llvm::AllocaInst &Frame,
                        const X86RegistrationFrameLayout &Layout,
                        llvm::ArrayRef<CxxSourceCallBinding> Calls,
                        llvm::ArrayRef<CxxCleanupCallBinding> Cleanups,
                        const std::set<const llvm::Instruction *> &ChainReads);
} // namespace registration_frame
} // namespace neverd

#endif // NEVERD_BACKEND_LLVM_REGISTRATIONCXXGUARDEDFRAME_H
