//===- MedCallingConvDetail.h - Calling convention queries -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_MED_MEDCALLINGCONVDETAIL_H
#define NEVERD_IR_MED_MEDCALLINGCONVDETAIL_H

#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/med/MedIR.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLFunctionalExtras.h"

#include <optional>
#include <set>
#include <tuple>

namespace neverd::med_calling_conv_detail {

using ValueKey = std::tuple<MedVar::VarKind, int, int>;
using ValueSet = llvm::DenseSet<ValueKey>;

/// An authenticated byte read, including instruction-owned FP memory sources.
/// Scalar FP sources can use a proven immutable incoming argument. Packed
/// sources retain their instruction-owned memory/alignment effects and homes.
struct StackMemoryRead {
  unsigned AddressInput;
  unsigned Bytes;
  bool ScalarFP = false;
};
std::optional<StackMemoryRead> stackMemoryRead(const MedOp &Op);
void preserveFPStackHomes(
    const MedFunc &Func, int64_t Base, unsigned Slot,
    llvm::function_ref<std::optional<int64_t>(const MedVar &)> Trace,
    std::set<int64_t> &MutableSlots);
void recoverFPStackReads(
    MedFunc &Func, Arch Architecture, int64_t Base, unsigned Slot,
    unsigned ParameterBase,
    llvm::function_ref<std::optional<int64_t>(const MedVar &)> Trace,
    const std::set<int64_t> &MutableSlots);

ValueKey valueKey(const MedVar &V);
/// Internal register-ABI stack recovery, including a bounded variadic prefix.
void detectStackParams(MedFunc &Func, Arch TargetArch, BinaryFormat Format,
                       int64_t MaxStackOff = 0);
bool containsValue(const ValueSet &Values, const MedVar &V);

ValueSet computeForwardValueClosure(
    const MedFunc &Func, llvm::ArrayRef<MedVar> Seeds,
    llvm::function_ref<bool(const MedOp &, unsigned)> ForwardsInput);

uint16_t findFirstUseSize(const MedFunc &Func, uint64_t ParamRegOff,
                          const TargetRegInfo &TRI);

/// True for an entry-block self-copy of \p RegOff, which declares the
/// register's incoming value.
bool isEntryLiveInCopy(const MedOp &Op, uint64_t RegOff);

/// True for an entry live-in copy whose output is a new version, so the
/// incoming value it reads is a separate SSA value with uses of its own.
bool isRenamedEntryLiveInCopy(const MedOp &Op, uint64_t RegOff);

/// An i386 parameter register whose live-in value only feeds scratch idioms
/// (MedCallingConvX86.cpp).
bool liveInOnlyFeedsScratch(const MedFunc &Func, uint64_t ParamRegOff);

/// Whether \p RegOff's incoming value reaches a use other than being pushed
/// or returned (MedCallingConvX86.cpp): a push that only reserves a stack
/// slot stores the register without reading it as a value, and a return
/// reads the return register whatever the function returns.
bool liveInReachesNonPushUse(const MedFunc &Func, uint64_t RegOff);

} // namespace neverd::med_calling_conv_detail

#endif // NEVERD_IR_MED_MEDCALLINGCONVDETAIL_H
