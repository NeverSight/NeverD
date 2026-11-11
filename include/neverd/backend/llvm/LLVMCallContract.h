//===- LLVMCallContract.h - Call argument contracts ------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Preserve physical argument locations and validate resolved LLVM calls.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_LLVM_LLVMCALLCONTRACT_H
#define NEVERD_BACKEND_LLVM_LLVMCALLCONTRACT_H

#include "neverd/ir/IntegerCallArguments.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace llvm {
class CallBase;
class Function;
class IRBuilderBase;
class Module;
class Value;
} // namespace llvm

namespace neverd {
inline constexpr llvm::StringLiteral
    LLVMArgumentBanksAttribute("neverd.argument-banks");
class LLVMSourceMap;
std::optional<std::pair<unsigned, unsigned>>
getLLVMArgumentBanks(const llvm::Function &Function);
std::optional<std::pair<unsigned, unsigned>>
getLLVMArgumentBanks(const llvm::CallBase &Call);
/// Stack byte locations following the recorded register banks.
struct LLVMArgumentStackLayout {
  std::vector<int64_t> Offsets;
  unsigned SlotBytes = 0;
};
LLVMArgumentStackLayout getLLVMArgumentStackLayout(const llvm::CallBase &Call);
/// Record independent register banks before argument values lose their
/// physical register identities to propagation or LLVM optimization.
void setLLVMArgumentBanks(llvm::Function &Function, unsigned Integers,
                          unsigned Floating);
/// A call also records where its stack values were stored: each one's byte
/// offset from the first stack argument, with the ABI's stack slot size.
/// Without offsets its stack values are one contiguous run of bytes.
void setLLVMArgumentBanks(llvm::CallBase &Call, unsigned Integers,
                          unsigned Floating,
                          llvm::ArrayRef<int64_t> StackOffsets = {},
                          unsigned SlotBytes = 0);
/// The first integer position of \p Call's target that it passes as zero.
void setLLVMFirstUnsetInteger(llvm::CallBase &Call, unsigned Position);
std::optional<unsigned> getLLVMFirstUnsetInteger(const llvm::CallBase &Call);
/// Map physical integer/stack positions to this call's actual LLVM operands.
/// Recorded banks may omit unused registers before stack arguments. Each
/// record piece must still name a complete carrier at its original byte offset.
/// Calls without bank metadata retain the canonical physical operand layout.
std::optional<IntegerCallArguments>
bindLLVMIntegerCallArguments(const llvm::CallBase &Call,
                             const IntegerCallArguments &Layout, Arch A,
                             BinaryFormat F);
/// Bind each recovered callee parameter to its register bank or stack slot.
/// A missing bank value fails; surplus values never shift another bank.
struct LLVMArgumentBinding {
  std::vector<llvm::Value *> Values;
  /// Original argument index, or -1 for a packed/sliced stack value whose
  /// source pointer interpretation and parameter attributes cannot be reused.
  std::vector<int> SourceIndices;
  /// The first integer position the call does not set, where it has any.
  std::optional<unsigned> FirstUnsetInteger;
};
/// A stack value placed by \p StackOffsets owns the bytes up to the end of
/// its last slot, or to the next value: padding a narrower store leaves.
///
/// A callee's stack parameters follow all of its integer register positions,
/// read or not, so a call that reaches them with fewer recovered integers
/// passes the rest as zero. That holds only where the callee does not read
/// them, which its body has yet to show: record the first such position on the
/// call with setLLVMFirstUnsetInteger, for validateResolvedLLVMCallSignatures.
std::optional<LLVMArgumentBinding> bindResolvedLLVMArguments(
    const llvm::Function &Target, llvm::ArrayRef<llvm::Value *> Values,
    unsigned Integers, unsigned Floating, llvm::IRBuilderBase &Builder,
    llvm::ArrayRef<int64_t> StackOffsets = {}, unsigned SlotBytes = 0);
/// A resolved call that normalizeResolvedLLVMCalls or
/// validateResolvedLLVMCallSignatures refuses: the function whose body makes
/// it, and the diagnostic.
struct ResolvedCallRefusal {
  llvm::Function *Caller = nullptr;
  std::string Reason;
};
/// Reconcile calls whose exact immutable target becomes known after emission.
/// Drop surplus fixed arguments and adapt scalar integer/pointer carriers.
/// Missing arguments, incompatible carriers and musttail changes fail clearly.
/// With \p Refusals, a unit that can leave a body out learns each function a
/// refused call is in, with why, and the calls of the other functions are
/// still reconciled; the result is false whenever a call was refused.
bool normalizeResolvedLLVMCalls(
    llvm::Module &Module, LLVMSourceMap *Sources = nullptr,
    bool AnalystSourceProjection = false,
    std::vector<ResolvedCallRefusal> *Refusals = nullptr);
/// Check identities after deferred import placeholders have been promoted.
/// Opaque pointers otherwise let a mismatched direct call pass LLVM's verifier.
/// Check both the type and calling convention after optimization and linking.
/// An inconsistent resolved target emits a diagnostic and returns false.
/// \p Refusals, as for normalizeResolvedLLVMCalls, collects the first such
/// call of each function and checks the others.
bool validateResolvedLLVMCallSignatures(
    llvm::Module &Module, bool AnalystSourceProjection = false,
    std::vector<ResolvedCallRefusal> *Refusals = nullptr);
} // namespace neverd

#endif // NEVERD_BACKEND_LLVM_LLVMCALLCONTRACT_H
