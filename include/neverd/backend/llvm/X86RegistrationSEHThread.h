//===- X86RegistrationSEHThread.h - PE32 thread reads -----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Source occurrence and actual LLVM address checks for current-thread reads.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_LLVM_X86REGISTRATIONSEHTHREAD_H
#define NEVERD_BACKEND_LLVM_X86REGISTRATIONSEHTHREAD_H

#include "neverd/ir/med/X86RegistrationSEHThread.h"

#include <map>
#include <optional>
#include <set>
#include <utility>

namespace llvm {
class Function;
class Instruction;
class LoadInst;
} // namespace llvm

namespace neverd {
/// Recheck source-bound DWORD loads and their exact Self-derived pointers.
/// Volatile distinguishes initial emission from anchored native source reads.
/// No other dereference or write acquires runtime-memory authority.
std::optional<std::set<const llvm::LoadInst *>> getCheckedX86SEHThreadReads(
    const MedFunc &Source, const llvm::Function &Parent,
    const std::map<std::pair<va_t, int>, const llvm::Instruction *> &Operations,
    bool Volatile);

} // namespace neverd

#endif // NEVERD_BACKEND_LLVM_X86REGISTRATIONSEHTHREAD_H
