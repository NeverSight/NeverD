//===- MedABIPassDetail.h - Shared ABI-recovery slicing helpers -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Internal data-flow and stack-offset tracing utilities shared between the
/// ABI recovery driver (MedABIPass.cpp), its support routines
/// (MedABIPassSupport.cpp) and the calling-convention steps
/// (MedABIPassWin64.cpp, MedABIPassI386.cpp).  These back the per-call-site
/// register/stack
/// argument scans in recoverCallAbi: resolving an indirect call target to a
/// constant address, mapping an outgoing store to its distance from the call
/// stack pointer, and finding the value reaching an argument register across
/// the CFG.
///
/// This header is an implementation detail of the med/ library and should NOT
/// be included by code outside lib/ir/med/.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_MED_MEDABIPASSDETAIL_H
#define NEVERD_IR_MED_MEDABIPASSDETAIL_H

#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/med/MedIR.h"

#include "llvm/ADT/STLFunctionalExtras.h"

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <vector>

namespace neverd {

struct BinaryImage;

/// Whether an operation prevents recovering an earlier ABI value. Explicit
/// numerical/MXCSR effects do not call a callee or write argument storage.
/// Unknown intrinsic effects retain the conservative boundary.
bool isAbiRecoveryBarrier(const MedOp &Op);

struct AbiSpillContext {
  const MedFunc &Func;
  const TargetRegInfo &TRI;
  const std::set<va_t> *FrameLocalLeafCallees;
};

//===----------------------------------------------------------------------===//
// Calling-convention steps
//===----------------------------------------------------------------------===//

/// One call's argument recovery in recoverCallAbi, as the steps of its
/// calling convention's AbiCallPolicy see it.
struct AbiCallContext {
  const MedFunc &Func;
  const MedBlock &Blk;
  /// The call's index in Blk.Ops.
  int CallIdx = 0;
  const MedCallInfo &CI;
  const TargetRegInfo &TRI;
  Arch TheArch = Arch::Unknown;
  /// The integer argument registers and stack layout of the call.
  const IntegerArgumentLayout &Layout;
  /// The recovered argument of each position and whether it is known, and
  /// whether the stack-store scan recovered it.
  std::vector<MedVar> &Found;
  std::vector<bool> &FoundMask;
  std::vector<bool> &FromStackScan;
  int MaxArgs = 0;
  /// The offset of a stack address from the stack pointer at the call.
  llvm::function_ref<std::optional<int64_t>(const MedVar &)> CallStackOffset;
};

/// Outgoing scalar stores that agree along every predecessor path
/// to a call. Unknown writes, calls, independent entries and cycles stop the
/// proof. Offsets are relative to the call's SP, using its exact SSA basis.
std::map<int64_t, MedVar> callSetupStackStores(const AbiCallContext &C);

/// Every reaching path leaves this incoming argument register untouched.
bool incomingArgumentReachesCall(const AbiCallContext &C, int ArgIndex);

/// The steps of call-ABI recovery that belong to one calling convention,
/// each optional.  A convention defines its policy in its own file
/// (MedABIPassWin64.cpp, MedABIPassI386.cpp) and abiCallPolicy() lists it;
/// recoverCallAbi runs a step at its point when the policy has one.
struct AbiCallPolicy {
  Arch TheArch = Arch::Unknown;
  /// The image format, or Unknown for every format of the architecture.
  BinaryFormat Format = BinaryFormat::Unknown;
  /// For a direct call: recover the first argument when an earlier call in
  /// the block clobbered its register.  \p Arg0FromInBlock says whether the
  /// block wrote it after that call.
  void (*ResolveFirstArgAfterCall)(AbiCallContext &C,
                                   bool &Arg0FromInBlock) = nullptr;
  /// For an indirect call without stack arguments: take the consecutive
  /// argument registers its paths write.
  void (*TakeIndirectCallRegisters)(AbiCallContext &C) = nullptr;
  /// For a virtual call whose object is the first argument: take the result
  /// buffer the convention passes after it.
  void (*TakeVirtualCallResultBuffer)(AbiCallContext &C) = nullptr;
  /// An empty first stack slot below a stored later one is an unused argument
  /// the compiler did not store, not the end of the argument list.
  bool LeadingStackGapIsUnusedArgument = false;
};

/// The call-ABI policy for code of \p A in a \p F image, or null.  An entry
/// for that exact format wins over one for every format.
const AbiCallPolicy *abiCallPolicy(Arch A, BinaryFormat F);

//===----------------------------------------------------------------------===//
// Indirect-target resolution
//===----------------------------------------------------------------------===//

/// Resolve an indirect call target to a constant code address when the call
/// goes through a function pointer that provably holds a known function.
/// Returns the resolved address, or 0 when not provable.
va_t resolveIndirectTargetAddr(const MedBlock &Blk, int FromIdx,
                               const MedVar &V, int Depth,
                               const AbiSpillContext *Context = nullptr);

/// Resolve an indirect-call target (or another value) back to the incoming
/// integer argument register from which it originated.  Follows the same
/// copy/cast and spill/reload chains as resolveIndirectTargetAddr.  nullopt
/// when the provenance is not provable within the call's block.
std::optional<int>
resolveIndirectTargetArgIdx(const MedBlock &Blk, int FromIdx,
                            const TargetRegInfo &TRI, const MedVar &V,
                            const IntegerArgumentLayout &Layout, int Depth = 0);

/// Object pointer of an MSVC vfptr slot `(*(*obj + imm))`.  The slot load,
/// `+imm`, and vfptr load must sit in \p Blk before \p CallIdx.  nullopt when
/// the target is not that shape (a raw function pointer, an import, ...).
std::optional<MedVar> vtableCallObject(const MedBlock &Blk, int CallIdx,
                                       const MedVar &Target);

//===----------------------------------------------------------------------===//
// Stack-pointer offset tracing
//===----------------------------------------------------------------------===//

/// Offset of \p V relative to the function-entry stack pointer, following the
/// SP definition chain through constant add/sub decrements and width casts.
/// nullopt when \p V does not derive from the SP.
std::optional<int64_t> stackPtrDelta(const MedFunc &Func,
                                     const TargetRegInfo &TRI, const MedVar &V,
                                     int Depth = 0);

/// Whether \p V is, or derives from (through copies, width casts and a constant
/// add/sub), a frame register -- the stack OR frame pointer.
bool derivesFromFrameReg(const MedBlock &Blk, const TargetRegInfo &TRI,
                         const MedVar &V, int Depth = 0);

/// Identifies an SSA stack-pointer value (Id, version, register offset).
using SpOffsetKey = std::tuple<int, int, uint64_t>;

/// Records, for each stack-pointer value on the call-site SP's definition
/// chain, its byte offset *above* the call SP (call SP = 0).  Used to place
/// pushed arguments relative to the call SP when the absolute entry-relative
/// delta is unavailable.
void buildCallSpOffsets(const MedFunc &Func, const TargetRegInfo &TRI,
                        const MedVar &V, int64_t Off,
                        std::map<SpOffsetKey, int64_t> &Map, int Depth);

/// Offset of store-address \p V above the call SP, resolved against the call
/// SP's offset map.  nullopt when the address is not stack-pointer derived.
std::optional<int64_t> relStackOff(const MedFunc &Func,
                                   const TargetRegInfo &TRI, const MedVar &V,
                                   const std::map<SpOffsetKey, int64_t> &Map,
                                   int Depth);

//===----------------------------------------------------------------------===//
// Argument-register recovery
//===----------------------------------------------------------------------===//

/// Name of the callee reached by the branch relocation at \p InsnAddr (for a
/// relocatable object whose direct-call target operand is a placeholder), or an
/// empty string when no such relocation exists.
std::string relocCalleeName(const BinaryImage &Img, va_t InsnAddr);

/// The value an argument-register-defining op at index \p J contributes to its
/// slot, resolving a low-half sub-register sync to the paired full-width write
/// so a wide (pointer/struct) argument keeps its high bits.
MedVar argRegSourceValueInBlock(const MedBlock &Blk, int J,
                                const TargetRegInfo &TRI,
                                const IntegerArgumentLayout &Layout);

/// Select the authoritative PHI for integer argument slot \p ArgIdx in
/// \p Block.  A PHI with a value on every function-entry edge wins over an
/// alias that is undef on the first iteration; otherwise the widest register
/// view wins.  Centralizing this rule keeps in-block and cross-block argument
/// recovery independent of PHI insertion order.
const PhiNode *selectAuthoritativeArgPhi(const MedFunc &Func,
                                         const MedBlock &Block,
                                         const TargetRegInfo &TRI, int ArgIdx,
                                         const IntegerArgumentLayout &Layout);

/// Value reaching argument register \p ArgIdx at the call in block \p BlockId,
/// found by walking the CFG backwards into predecessor blocks (nearest write,
/// then a block PHI, then -- when \p AllowUnknownLiveIn -- an incoming
/// parameter live-in).  \p FromLiveIn, when non-null, is set true if the value
/// came from the live-in fallback.  nullopt when the register is never set on
/// any path to the call.
std::optional<MedVar>
findReachingArgReg(const MedFunc &Func, const TargetRegInfo &TRI, Arch TheArch,
                   int BlockId, int ArgIdx, const IntegerArgumentLayout &Layout,
                   bool AllowUnknownLiveIn = false, bool *FromLiveIn = nullptr,
                   bool *FoundDef = nullptr);

/// Unique predecessor's `if (p)` / `if (!p)` pointer.  After an intervening
/// Win64 thiscall clobbers rcx. The callee this is the guarded table, not
/// live-in parent this.
std::optional<MedVar> uniquePredNonNullGuard(const MedFunc &Func,
                                             const MedBlock &Blk);

} // namespace neverd

#endif // NEVERD_IR_MED_MEDABIPASSDETAIL_H
