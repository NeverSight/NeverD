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
#include "neverd/ir/low/CallRegisterEffects.h"
#include "neverd/ir/med/MedABIPass.h"
#include "neverd/ir/med/MedCallConvention.h"
#include "neverd/ir/med/MedIR.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/STLFunctionalExtras.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

namespace neverd {

struct BinaryImage;

/// Whether an operation prevents recovering an earlier ABI value: where it
/// ends a later call's argument setup (endsCallArgumentSetup), as call
/// argument collection in MedToHigh has it too.
bool isAbiRecoveryBarrier(const MedOp &Op);

/// Recover the sole ECX argument of an exact, source-checked PE32 cookie call.
/// Incidental register values and inferred callee signatures do not extend it.
bool recoverI386RegistrationCookieCall(const MedFunc &Func,
                                       const MedBlock &Block, size_t CallIndex,
                                       const TargetRegInfo &TRI,
                                       MedCallInfo &Info);

/// Physical pre-call SP definitions, using the shared ABI recovery walk.
/// Source clients charge all walks to one budget and prove entry offsets
/// separately. Missing or ambiguous bases provide no stack location.
std::map<const MedOp *, MedVar> sourceCallStackPointers(const MedFunc &Func,
                                                        Arch Architecture,
                                                        size_t &Remaining);
/// Also publishes each block's incoming physical SP under the same fixed
/// point; absent entries have no unique reaching definition.
std::map<const MedOp *, MedVar>
sourceCallStackPointers(const MedFunc &Func, Arch Architecture,
                        size_t &Remaining, std::map<int, MedVar> &BlockEntries);

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
/// The blocks of one function by id, for a pass that adds or removes no
/// blocks while it uses the index; operations and PHIs may change.
class MedBlockIdIndex {
public:
  explicit MedBlockIdIndex(const MedFunc &Func) {
    for (const MedBlock &Block : Func.Blocks) {
      if (!First.emplace(Block.Id, &Block).second)
        Unique = false;
      Last[Block.Id] = &Block;
    }
  }
  /// Whether no two blocks share an id.
  bool unique() const { return Unique; }
  /// The first block with \p Id, or null.
  const MedBlock *first(int Id) const {
    const auto It = First.find(Id);
    return It == First.end() ? nullptr : It->second;
  }
  /// The last block with \p Id, or null.
  const MedBlock *last(int Id) const {
    const auto It = Last.find(Id);
    return It == Last.end() ? nullptr : It->second;
  }

private:
  std::map<int, const MedBlock *> First;
  std::map<int, const MedBlock *> Last;
  bool Unique = true;
};

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
  /// Func's blocks by id, when the caller keeps them; null builds them.
  const MedBlockIdIndex *Blocks = nullptr;
};

/// An original scalar or an index into CallSetupStackJoins::Nodes. Plans use
/// no SSA identity until a required argument actually consumes them.
using CallSetupStackValue = std::variant<MedVar, size_t>;
struct CallSetupStackJoin {
  int BlockId;
  uint16_t Bytes;
  std::vector<std::pair<int, CallSetupStackValue>> Args;
};
struct CallSetupStackJoins {
  std::vector<CallSetupStackJoin> Nodes;
  std::map<int64_t, size_t> Slots;
};

/// Outgoing scalar stores that agree along every predecessor path
/// to a call. Unknown writes, calls, independent entries and cycles stop the
/// proof. Offsets are relative to the call's SP, using its exact SSA basis.
/// When requested, Joins keeps complete same-width definitions through nested
/// joins. Nodes refer only to earlier nodes, and Slots names the final roots.
/// The caller materializes only the nodes a required argument reaches.
std::map<int64_t, MedVar>
callSetupStackStores(const AbiCallContext &C,
                     CallSetupStackJoins *Joins = nullptr);

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

/// Where a value is defined: its one operation or PHI anywhere in the
/// function, or Ambiguous when more than one defines it.
struct MedDefSite {
  const MedBlock *Block = nullptr;
  const MedOp *Op = nullptr;
  const PhiNode *Phi = nullptr;
  bool Ambiguous = false;
};

/// The definition site of each value of a function, as a scan of the whole
/// function finds it, indexed on first use. Adding or moving operations or
/// PHIs requires invalidate().
class MedDefSiteIndex {
public:
  explicit MedDefSiteIndex(const MedFunc &Func) : Func(Func) {}
  MedDefSite find(const MedVar &V);
  void invalidate() {
    Sites.clear();
    StackDeltas.clear();
    Built = false;
  }

  /// A stackPtrDelta answer that no depth cap shaped: the offset, and how
  /// many levels below the value its walk went.  A query at a depth where
  /// that walk stays under the cap gets the same answer, so an answer serves
  /// every query for as long as the definitions do.
  struct StackDelta {
    std::optional<int64_t> Delta;
    int Height = 0;
  };
  const StackDelta *stackDelta(const MedVar &V, uint64_t StackPointer) const {
    const auto It = StackDeltas.find({key(V), StackPointer});
    return It == StackDeltas.end() ? nullptr : &It->second;
  }
  void rememberStackDelta(const MedVar &V, uint64_t StackPointer,
                          StackDelta Answer) {
    StackDeltas.try_emplace(StackDeltaKey{key(V), StackPointer}, Answer);
  }

private:
  using Identity = std::tuple<int, int, int, uint64_t>;
  using StackDeltaKey = std::pair<Identity, uint64_t>;
  static Identity key(const MedVar &V) {
    return {static_cast<int>(V.Kind), V.Id, V.SSAVer, V.RegOff};
  }

  const MedFunc &Func;
  llvm::DenseMap<Identity, MedDefSite> Sites;
  llvm::DenseMap<StackDeltaKey, StackDelta> StackDeltas;
  bool Built = false;
};

/// Offset of \p V relative to the function-entry stack pointer, following the
/// SP definition chain through constant add/sub decrements and width casts.
/// nullopt when \p V does not derive from the SP. \p Defs, when given, must
/// index \p Func.
std::optional<int64_t>
stackPtrDelta(const MedFunc &Func, const TargetRegInfo &TRI, const MedVar &V,
              int Depth = 0, MedDefSiteIndex *Defs = nullptr,
              BinaryFormat Format = BinaryFormat::Unknown);

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
                        std::map<SpOffsetKey, int64_t> &Map, int Depth,
                        MedDefSiteIndex *Defs = nullptr);

/// Offset of store-address \p V above the call SP, resolved against the call
/// SP's offset map.  nullopt when the address is not stack-pointer derived.
std::optional<int64_t> relStackOff(const MedFunc &Func,
                                   const TargetRegInfo &TRI, const MedVar &V,
                                   const std::map<SpOffsetKey, int64_t> &Map,
                                   int Depth, MedDefSiteIndex *Defs = nullptr);

//===----------------------------------------------------------------------===//
// Argument-register recovery
//===----------------------------------------------------------------------===//

class RelocationSymbolIndex;

/// Name of the callee reached by the branch relocation at \p InsnAddr (for a
/// relocatable object whose direct-call target operand is a placeholder), or an
/// empty string when no such relocation exists.  \p Index, when given, indexes
/// \p Img's relocations.
std::string relocCalleeName(const BinaryImage &Img, va_t InsnAddr,
                            const RelocationSymbolIndex *Index = nullptr);

/// Select the authoritative PHI for integer argument slot \p ArgIdx in
/// \p Block.  A PHI with a value on every function-entry edge wins over an
/// alias that is undef on the first iteration; otherwise the widest register
/// view wins.  Centralizing this rule keeps in-block and cross-block argument
/// recovery independent of PHI insertion order.
const PhiNode *selectAuthoritativeArgPhi(const MedFunc &Func,
                                         const MedBlock &Block,
                                         const TargetRegInfo &TRI, int ArgIdx,
                                         const IntegerArgumentLayout &Layout);

/// The authoritative PHI for integer argument slot \p ArgIdx in \p Block
/// (selectAuthoritativeArgPhi), where it still holds the register at the op
/// \p CallIdx: no op before it in the block writes the register, and none is
/// a call that may change it (callClobbersRegister under \p Format), whose
/// leftover the register holds instead.
const PhiNode *argPhiReachingCall(const MedFunc &Func, const MedBlock &Block,
                                  int CallIdx, const TargetRegInfo &TRI,
                                  int ArgIdx,
                                  const IntegerArgumentLayout &Layout,
                                  BinaryFormat Format);

/// Value reaching argument register \p ArgIdx at the call in block \p BlockId,
/// found by walking the CFG backwards into predecessor blocks (nearest write,
/// then a block PHI, then -- when \p AllowUnknownLiveIn -- an incoming
/// parameter live-in).  \p FromLiveIn, when non-null, is set true if the value
/// came from the live-in fallback.  nullopt when the register is never set on
/// any path to the call.
///
/// A call that may change the register (callClobbersRegister under
/// \p Format) ends a path as a write does, and so does an exception
/// handler's entry, where the unwinder leaves the register unspecified.
/// Where every path ends so, the value is what the calls left there
/// (valueLeftByCall), which no parameter carries.  \p CallIdx, when not
/// negative, is the call's index in its block: the block's operations
/// before it are searched first.
std::optional<MedVar>
findReachingArgReg(const MedFunc &Func, const TargetRegInfo &TRI, Arch TheArch,
                   int BlockId, int ArgIdx, const IntegerArgumentLayout &Layout,
                   bool AllowUnknownLiveIn = false, bool *FromLiveIn = nullptr,
                   bool *FoundDef = nullptr, int CallIdx = -1,
                   BinaryFormat Format = BinaryFormat::Unknown,
                   const MedBlockIdIndex *Blocks = nullptr);

/// Unique predecessor's `if (p)` / `if (!p)` pointer.  After an intervening
/// Win64 thiscall clobbers rcx. The callee this is the guarded table, not
/// live-in parent this.
std::optional<MedVar> uniquePredNonNullGuard(const MedFunc &Func,
                                             const MedBlock &Blk);

} // namespace neverd

#endif // NEVERD_IR_MED_MEDABIPASSDETAIL_H
