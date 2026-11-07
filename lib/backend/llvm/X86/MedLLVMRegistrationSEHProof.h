//===- MedLLVMRegistrationSEHProof.h - PE32 SEH preflight -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_LLVM_X86_MEDLLVMREGISTRATIONSEHPROOF_H
#define NEVERD_BACKEND_LLVM_X86_MEDLLVMREGISTRATIONSEHPROOF_H

#include "neverd/ir/med/MedIR.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLFunctionalExtras.h"

#include <map>
#include <optional>
#include <set>
#include <tuple>
#include <vector>

namespace llvm {
class BasicBlock;
class CallInst;
class Function;
class Instruction;
class LoadInst;
class StoreInst;
} // namespace llvm

namespace neverd {
struct X86RegistrationCallbackRequest;
}

namespace neverd::x86_registration {

/// The common LowIR provenance, closed over synthetic MedIR copies and PHIs.
struct SEHFrameValues {
  using Key = std::tuple<MedVar::VarKind, int, int>;
  std::set<Key> Values;
  bool contains(const MedVar &V) const {
    return !V.isConst() && Values.count({V.Kind, V.Id, V.SSAVer});
  }
};

struct SEHFrameOffsets {
  struct Offset {
    int32_t Bytes;
    uint16_t Width;
  };
  std::map<SEHFrameValues::Key, Offset> Values;
  std::map<SEHFrameValues::Key, uint16_t> UniqueDefinitions;

  std::optional<int32_t> get(const MedVar &Value) const {
    const auto Found = Values.find({Value.Kind, Value.Id, Value.SSAVer});
    if (Found == Values.end() || Found->second.Width != Value.Size)
      return std::nullopt;
    return Found->second.Bytes;
  }
};

/// Checked frame coordinates and unique definitions in the current SSA graph.
std::optional<SEHFrameOffsets> getSEHFrameOffsets(const MedFunc &Source);

/// Carry authenticated runtime scalars through exact block-local frame spills.
/// Unknown stores, calls and partial overwrites discard the memory fact.
bool extendSEHScalarSpills(const MedFunc &Source,
                           const SEHFrameOffsets &Offsets,
                           std::set<SEHFrameValues::Key> &Scalars,
                           size_t &Work);

/// Rebind exact LowIR frame offsets to unique current SSA definitions. This
/// only discharges source preflight; COFF still proves the emitted addresses.
std::optional<std::set<const MedOp *>>
getSEHFrameStores(const MedFunc &Source, uint64_t EntrySP, uint64_t FrameBytes);
std::optional<SEHFrameValues> getPrivateSEHFrameValues(
    const MedFunc &Func, const RegistrationStateAnalysis &States,
    llvm::function_ref<bool(const MedOp &)> IsPrivateStore);

std::optional<std::set<const llvm::StoreInst *>> getCheckedSEHCallbackStores(
    llvm::ArrayRef<X86RegistrationCallbackRequest> Requests);

std::optional<std::set<llvm::Instruction *>> getCheckedSEHChainInstructions(
    llvm::Function &Parent, const RegistrationStateAnalysis &States,
    const std::map<std::pair<va_t, int>, llvm::Instruction *>
        &RegistrationChainIR,
    const std::set<const llvm::LoadInst *> &ThreadReads);

struct SourceOperation {
  llvm::Instruction *Instruction;
  const MedOp *Operation;
  int Block;
  uint8_t Kind;
};
struct SEHSourceOperations {
  std::vector<SourceOperation> Operations;
  std::set<llvm::CallInst *> CookieCheckCalls;
};
std::optional<SEHSourceOperations> getCheckedSEHSourceOperations(
    const MedFunc &Func,
    const std::map<int, llvm::BasicBlock *> &OriginalBlockMap,
    const std::map<std::pair<va_t, int>, llvm::Instruction *>
        &RegistrationMemoryIR,
    const std::map<const llvm::CallInst *, va_t> &CallSiteAddrs);

bool collectDiscardedRegistrationResult(llvm::CallInst &Call,
                                        std::vector<llvm::Instruction *> &Dead);

} // namespace neverd::x86_registration

#endif // NEVERD_BACKEND_LLVM_X86_MEDLLVMREGISTRATIONSEHPROOF_H
