//===- X86RegistrationCatch.h -----------------------------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_LLVM_X86REGISTRATIONCATCH_H
#define NEVERD_BACKEND_LLVM_X86REGISTRATIONCATCH_H

#include <cstdint>
#include <map>
#include <optional>
#include <utility>
#include <vector>

namespace neverd {
struct ExceptionFunction;
struct MedFunc;
struct RegistrationStateAnalysis;
struct X86RegistrationFrameLayout;

/// Original FuncInfo try and clause indices identify a runtime invocation.
using X86RegistrationCatchIdentity = std::pair<uint32_t, uint32_t>;

struct X86RegistrationCatchHome {
  uint32_t Offset = 0;
  uint32_t ObjectSize = 0;
  bool Reference = false;

  uint32_t slotSize() const { return Reference ? 4 : ObjectSize; }
};

/// Absence of Home means that the runtime binds no object. It is distinct
/// from an invalid or missing source proof, which has no plan at all.
struct X86RegistrationCatchPlan {
  std::optional<X86RegistrationCatchHome> Home;
};

/// Project a catch into its checked parent allocation. Object and access
/// receipts are scoped to the exact source try and clause indices.
std::optional<X86RegistrationCatchPlan>
projectX86RegistrationCatch(const ExceptionFunction &EH,
                            const RegistrationStateAnalysis &State,
                            const X86RegistrationFrameLayout &Frame,
                            uint32_t TryIndex = 0, uint32_t CatchIndex = 0);

/// Partition the CFG and checked catch resumes by runtime invocation. A block
/// shared by distinct invocations needs cloning before native lowering and has
/// no unique owner.
std::optional<std::map<int, X86RegistrationCatchIdentity>>
projectX86RegistrationCatchBlocks(const MedFunc &Function);

/// Each try group has one lexical parent catch or belongs to the function.
/// Resolve it from checked invocation stacks, including exceptional resumes.
std::optional<std::vector<std::optional<X86RegistrationCatchIdentity>>>
projectX86RegistrationCatchParents(const MedFunc &Function);
} // namespace neverd

#endif // NEVERD_BACKEND_LLVM_X86REGISTRATIONCATCH_H
