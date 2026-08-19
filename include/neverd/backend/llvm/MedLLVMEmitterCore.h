//===- MedLLVMEmitterCore.h - MedIR to LLVM IR emitter --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Emitter that translates MedIR functions to LLVM IR modules.  Include
/// MedLLVMEmitter.h (the umbrella) rather than this header directly.
///
/// The MedLLVMEmitter class is large, so its member functions are implemented
/// across several translation units grouped by concern, in four directories of
/// lib/backend/llvm/:
///
///   emit/    MedLLVMEmitter.cpp (types, the memory-pointer primitive,
///            function declaration and module emit), MedLLVMFuncBody.cpp
///            (per-function body emission), MedLLVMOpEmitter.cpp (the MedOp
///            opcode-switch dispatch), MedLLVMFloatEmitter.cpp, MedLLVMCall.cpp
///            and MedLLVMReturn.cpp, MedLLVMIntrinsic.cpp (INTRINSIC dispatch
///            plus the inline-asm helper), MedLLVMSwitchIndex.cpp and
///            MedLLVMSwitch.cpp (jump-table index recovery and lowering).
///   resolve/ MedLLVMAddrResolve.cpp (shared SSA/base tracing),
///            MedLLVMCodePtrResolve.cpp (code-pointer tables and references),
///            MedLLVMFrameResolve.cpp, MedLLVMVarAccess.cpp (getVar/setVar).
///   data/    MedLLVMSegmentEmbed.cpp, MedLLVMWritableData.cpp,
///            MedLLVMSymbolizedPtr.cpp, MedLLVMSpillPredicate.cpp,
///            MedLLVMGlobalData.cpp (the constant-address entry point),
///            MedLLVMLiteralTable.cpp, MedLLVMIndexedGlobal.cpp, and the
///            MedLLVMConst{Class,Use,WalkedBase}.cpp classifiers.
///   eh/      MedLLVMExceptionMetadata.cpp and the MedLLVMNative*EH.cpp
///            native lowerings.
///
/// Architecture-specific intrinsic emitters and address recognizers (e.g. the
/// i386 PIC helpers in X86/MedLLVMX86GlobalData.cpp) live under X86/, AArch64/,
/// and ARM/.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_LLVM_MEDLLVMEMITTERCORE_H
#define NEVERD_BACKEND_LLVM_MEDLLVMEMITTERCORE_H
#include "neverd/backend/llvm/MedLLVMEmitterState.h"
#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/ir/med/MedIR.h"
#include "neverd/loader/BinaryImage.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace neverd {

class MedLLVMEmitterTestPeer;
class MedLLVMProvenanceTestPeer;

class MedLLVMEmitter {
public:
  /// Emit \p Funcs into a fresh module in \p LCtx.
  ///
  /// For parallel (sharded) emission each worker owns its own LLVMContext,
  /// emitter and module, emits a *contiguous body range* of the shared function
  /// list, and the shard modules are later linked into one.  Two knobs support
  /// that:
  ///   - \p MergeableGlobals gives every synthesized global (embedded segments,
  ///     string / code-pointer tables) linkonce_odr linkage with a name that is
  ///     a pure function of its address, so identical globals emitted by two
  ///     shards collapse to one at link time.  Crucially this keeps a WRITABLE
  ///     .data/.bss segment a single shared mutable object across shards, so a
  ///     store in one function and a load in another still alias.
  ///   - \p BodyMask (when non-null, sized to \p Funcs) restricts which
  ///     functions get *bodies*: index i is emitted only if BodyMask[i] != 0.
  ///     ALL functions are still declared (so a body may reference a sibling
  ///     defined in another shard); functions the mask omits remain
  ///     declarations the linker resolves against their defining shard.  A
  ///     null mask emits every body.
  /// \p Imports carries loader-native object symbol spellings.  The emitter
  /// converts them to target LLVM global names at its object/IR boundary.
  /// The defaults (MergeableGlobals=false, null mask) reproduce the original
  /// single-module behavior byte-for-byte.
  std::unique_ptr<llvm::Module>
  emit(const std::vector<MedFunc> &Funcs, llvm::LLVMContext &LCtx,
       const std::string &ModName = "neverd_module", Arch TheArch = Arch::X64,
       const std::vector<std::pair<va_t, std::string>> &Imports = {},
       const BinaryImage *Img = nullptr, BinaryFormat Fmt = BinaryFormat::ELF,
       bool MergeableGlobals = false,
       const std::vector<char> *BodyMask = nullptr);

  uint64_t unhandledValueIntrinsicCount() const {
    return UnhandledValueIntrinsicCount;
  }

private:
  friend class MedLLVMEmitterTestPeer;
  friend class MedLLVMProvenanceTestPeer;

  struct AddressProvenanceWorkCounts {
    uint64_t EdgeClassifications = 0;
    uint64_t RecurrenceProofs = 0;
    uint64_t StableOffsetProofs = 0;
    uint64_t IndexedBaseProofs = 0;
  };

  // Deterministic test observability for the recursive provenance proofs. The
  // counters never participate in an emission decision.
  mutable AddressProvenanceWorkCounts AddressProvenanceWork;

  /// Linkage for a synthesized data/table global: linkonce_odr in mergeable
  /// (sharded) mode so identical per-address globals from sibling shards merge;
  /// internal otherwise (the original standalone-module form).
  llvm::GlobalValue::LinkageTypes dataLinkage() const {
    return MergeableGlobals ? llvm::GlobalValue::LinkOnceODRLinkage
                            : llvm::GlobalValue::InternalLinkage;
  }
  /// A synthesized global emitted in mergeable (sharded) mode is defined in
  /// this same recompiled object, so mark it dso_local — codegen then addresses
  /// it PC-relative, exactly as the internal-linkage serial form did.  Without
  /// this a linkonce_odr global is not dso_local and, under the PIC relocation
  /// model, is loaded through a GOT slot the recompiled object carries no
  /// loader to populate (the round-trip harness reads it as unmapped memory).
  /// No-op in standalone mode (internal linkage is already local).
  void markSharedLocal(llvm::GlobalValue *GV) const {
    if (MergeableGlobals)
      GV->setDSOLocal(true);
  }
  /// True while emitting a shard whose globals must be link-mergeable.
  bool MergeableGlobals = false;
  uint64_t UnhandledValueIntrinsicCount = 0;
  /// Create (or return the existing) LLVM function declaration for \p Func with
  /// its recovered signature, but no body.  Emitting all declarations before
  /// any body lets a body reference a not-yet-emitted sibling — e.g. a
  /// function-pointer table whose `ptrtoint @func` entries name leaf functions
  /// the dispatcher (emitted first) would otherwise not yet see.
  llvm::Function *declareFunc(const MedFunc &Func);
  llvm::Function *emitFunc(const MedFunc &Func);
  void emitExceptionMetadata(const MedFunc &Func, llvm::Function &LLVMFunc);
  bool emitNativeSEH(const MedFunc &Func, llvm::Function &LLVMFunc,
                     const std::map<int, llvm::BasicBlock *> &OriginalBlockMap);
  bool
  emitNativeCxxEH(const MedFunc &Func, llvm::Function &LLVMFunc,
                  const std::map<int, llvm::BasicBlock *> &OriginalBlockMap);
  /// Lower a decoded Itanium LSDA to the landing-pad EH model: a personality,
  /// an `invoke` for every call the call-site table protects, and a
  /// `landingpad` carrying the clauses the action chain names.  Rust rides the
  /// same tables on every non-MSVC target, so this covers it too.
  bool emitNativeItaniumEH(
      const MedFunc &Func, llvm::Function &LLVMFunc,
      const std::map<int, llvm::BasicBlock *> &OriginalBlockMap);
  void emitOp(const MedOp &Op, llvm::IRBuilder<> &Builder, int BlockId,
              int OpIdx);
  /// CALL/INDIR_CALL and RETURN lowering, carved out of the emitOp opcode
  /// switch into MedLLVMCall.cpp and MedLLVMReturn.cpp to keep emitOp a
  /// readable dispatcher.
  void emitCallOp(const MedOp &Op, llvm::IRBuilder<> &Builder, int BlockId,
                  int OpIdx);
  void defineCallClobbers(const MedOp &Op, llvm::IRBuilder<> &Builder);
  void emitReturnOp(const MedOp &Op, llvm::IRBuilder<> &Builder);

  llvm::Value *toVec(llvm::Value *V, llvm::Type *VTy,
                     llvm::IRBuilder<> &Builder);
  llvm::Value *fromVec(llvm::Value *V, llvm::IRBuilder<> &Builder);
  llvm::Value *widenToI128(llvm::Value *V, llvm::IRBuilder<> &Builder);

  llvm::Value *emitIntrinsic(const MedOp &Op, llvm::IRBuilder<> &Builder);
  llvm::Value *emitFloatOp(const MedOp &Op, llvm::IRBuilder<> &Builder);
  llvm::Value *emitAesIntrinsic(const MedOp &Op, Intrinsic IC,
                                llvm::IRBuilder<> &Builder);
  llvm::Value *emitShaIntrinsic(const MedOp &Op, Intrinsic IC,
                                llvm::IRBuilder<> &Builder);
  llvm::Value *emitGfniIntrinsic(const MedOp &Op, Intrinsic IC,
                                 llvm::IRBuilder<> &Builder);
  llvm::Value *emitShuffleIntrinsic(const MedOp &Op, Intrinsic IC,
                                    llvm::IRBuilder<> &Builder);
  llvm::Value *emitPshufb(const MedOp &Op, llvm::IRBuilder<> &Builder);
  llvm::Value *emitUnpackShuffle(const MedOp &Op, Intrinsic IC,
                                 llvm::IRBuilder<> &Builder);
  llvm::Value *emitImmShuffle(const MedOp &Op, Intrinsic IC,
                              llvm::IRBuilder<> &Builder);
  llvm::Value *emitMovDup(const MedOp &Op, Intrinsic IC,
                          llvm::IRBuilder<> &Builder);
  llvm::Value *emitPshufw(const MedOp &Op, llvm::IRBuilder<> &Builder);
  llvm::Value *emitVBroadcast(const MedOp &Op, Intrinsic IC,
                              llvm::IRBuilder<> &Builder);
  llvm::Value *emitPermd(const MedOp &Op, llvm::IRBuilder<> &Builder);
  llvm::Value *emitPackedShift(const MedOp &Op, Intrinsic IC,
                               llvm::IRBuilder<> &Builder);
  llvm::Value *emitMiscSimd(const MedOp &Op, Intrinsic IC,
                            llvm::IRBuilder<> &Builder);
  llvm::Value *emitPcmpStr(const MedOp &Op, Intrinsic IC,
                           llvm::IRBuilder<> &Builder);
  llvm::Value *emitMaskedMemOp(const MedOp &Op, Intrinsic IC,
                               llvm::IRBuilder<> &Builder);
  llvm::Value *emitBitManipSimd(const MedOp &Op, Intrinsic IC,
                                llvm::IRBuilder<> &Builder);
  llvm::Value *emitPmovmskb(const MedOp &Op, llvm::IRBuilder<> &Builder);
  llvm::Value *emitPhminposuw(const MedOp &Op, llvm::IRBuilder<> &Builder);
  llvm::Value *emitDpps(const MedOp &Op, llvm::IRBuilder<> &Builder);
  llvm::Value *emitMpsadbw(const MedOp &Op, llvm::IRBuilder<> &Builder);

  bool emitSideeffectIntrinsic(const MedOp &Op, Intrinsic IC,
                               llvm::IRBuilder<> &Builder);

  // x86 side-effect dispatch and helpers
  bool emitX86Sideeffect(const MedOp &Op, Intrinsic IC,
                         llvm::IRBuilder<> &Builder);
  bool emitX86DebugTrap(const MedOp &Op, Intrinsic IC,
                        llvm::IRBuilder<> &Builder);
  bool emitX86Fence(const MedOp &Op, Intrinsic IC, llvm::IRBuilder<> &Builder);
  bool emitX86CacheOp(const MedOp &Op, Intrinsic IC,
                      llvm::IRBuilder<> &Builder);
  bool emitX86Privileged(const MedOp &Op, Intrinsic IC,
                         llvm::IRBuilder<> &Builder);
  /// Operand-carrying privileged/system instructions whose operands are
  /// captured at lift time (descriptor-table loads/stores, INVLPG, XABORT).
  bool emitX86SystemAsm(const MedOp &Op, Intrinsic IC,
                        llvm::IRBuilder<> &Builder);
  /// Emit `Mn ($0)` inline asm with the captured address operand
  /// (Op.Inputs[1]) as a pointer, or bare `Mn` when no address was captured.
  void emitX86MemPtrAsm(const char *Mn, const MedOp &Op,
                        llvm::IRBuilder<> &Builder);
  /// IN acc, port — read an I/O port into the accumulator (value-producing).
  llvm::Value *emitX86PortIn(const MedOp &Op, llvm::IRBuilder<> &Builder);
  /// OUT port, acc — write the accumulator to an I/O port (side-effect).
  void emitX86PortOut(const MedOp &Op, llvm::IRBuilder<> &Builder);
  /// SLDT/STR/SMSW with a register destination — store a system register into
  /// a GPR (value-producing).
  llvm::Value *emitX86SysRegStore(const MedOp &Op, Intrinsic IC,
                                  llvm::IRBuilder<> &Builder);

  // AArch64 side-effect dispatch and helpers
  bool emitAArch64Sideeffect(const MedOp &Op, Intrinsic IC,
                             llvm::IRBuilder<> &Builder);
  bool emitAArch64Barrier(const MedOp &Op, Intrinsic IC,
                          llvm::IRBuilder<> &Builder);
  bool emitAArch64Hint(const MedOp &Op, Intrinsic IC,
                       llvm::IRBuilder<> &Builder);
  bool emitAArch64Exception(const MedOp &Op, Intrinsic IC,
                            llvm::IRBuilder<> &Builder);

  // ARM side-effect dispatch and helpers
  bool emitARMSideeffect(const MedOp &Op, Intrinsic IC,
                         llvm::IRBuilder<> &Builder);
  bool emitARMBarrier(const MedOp &Op, Intrinsic IC,
                      llvm::IRBuilder<> &Builder);
  bool emitARMException(const MedOp &Op, Intrinsic IC,
                        llvm::IRBuilder<> &Builder);

  // x86 value-producing INTRINSIC
  llvm::Value *emitX86IntrinsicValue(const MedOp &Op, Intrinsic IC,
                                     llvm::IRBuilder<> &Builder);
  llvm::Value *emitRdtscValue(const MedOp &Op, Intrinsic IC,
                              llvm::IRBuilder<> &Builder);
  llvm::Value *emitCpuidValue(const MedOp &Op, llvm::IRBuilder<> &Builder);
  llvm::Value *emitXgetbvValue(const MedOp &Op, llvm::IRBuilder<> &Builder);
  llvm::Value *emitRepString(const MedOp &Op, Intrinsic IC,
                             llvm::IRBuilder<> &Builder);

  // ARM value-producing INTRINSIC
  llvm::Value *emitARMIntrinsicValue(const MedOp &Op, Intrinsic IC,
                                     llvm::IRBuilder<> &Builder);

  // AArch64 value-producing INTRINSIC
  llvm::Value *emitAArch64IntrinsicValue(const MedOp &Op, Intrinsic IC,
                                         llvm::IRBuilder<> &Builder);
  void emitVoidInlineAsm(const char *Mnemonic, const MedOp &Op,
                         llvm::IRBuilder<> &Builder);

  // x86-64 wide DIV/IDIV (MedLLVMX86ValueEmitter.cpp)
  llvm::Value *emitX86WideDivRem(llvm::IRBuilder<> &Builder,
                                 llvm::Value *Dividend, llvm::Value *Divisor,
                                 bool IsSigned, bool WantRem);

  llvm::Type *sizeToType(uint16_t Size);
  llvm::Type *mapNdtype(const TypeRef &Ty);
  llvm::Value *getVar(const MedVar &V, llvm::IRBuilder<> &Builder);
  /// Return the exact primary data-address model getVar applies before
  /// materializing a constant: true means ptrtoint(@global), false means the
  /// original numeric VA unless trusted pointer intent requires emission to
  /// fail closed. Address resolvers use this same predicate so a
  /// forwarded/COPY arm is never classified merely by its SSA shape.
  bool getVarSymbolizesDataConstant(uint64_t Val, uint16_t Size) const;
  /// The non-recursive core of getVarSymbolizesDataConstant.  This excludes
  /// induction-string closure so the induction recognizer can ask how a
  /// constant would otherwise be emitted without recursively justifying
  /// itself.
  bool getVarDirectlySymbolizesDataConstant(uint64_t Val, uint16_t Size) const;
  /// Pointer intent before the materialization-route check.  This is kept
  /// separate so getVar can fail closed for a trusted mapped/anchored pointer
  /// whose required global cannot be built, while ignoring stray loader hints
  /// for unmapped or unreadable scalar values.
  bool getVarHasDirectDataSymbolizationIntent(uint64_t Val,
                                              uint16_t Size) const;
  /// True when getVar may replace the original numeric bit pattern with a
  /// relocatable data/function address.  Constant control folding must stop at
  /// such a leaf: comparisons on original image VAs are not invariant after
  /// the rebuilt object is linked at a different address.
  bool getVarMayRelocateConstant(uint64_t Val, uint16_t Size) const;
  void setVar(const MedVar &V, llvm::Value *Val, llvm::IRBuilder<> &Builder);

  /// Lower an INDIR_BR with a resolved jump table into an LLVM switch on the
  /// table index.  Returns false when no matching table is found so the
  /// caller can fall back to a plain branch.
  bool emitJumpTableSwitch(const MedBlock &Blk, const MedOp &BrOp,
                           std::map<int, llvm::BasicBlock *> &BBMap,
                           llvm::IRBuilder<> &Builder);

  /// Synthesize the byte-offset switch selector for a two-table dispatch
  /// (`base = cond ? A : B; jmp *base[idx]`, see JumpTable::TwoTableSelect):
  /// the merged table is indexed by `idx_bytes + (cond selects the higher table
  /// ? TwoTableOffset : 0)`, recovered from the runtime base select (a clean
  /// SELECT or an `(A&M)|(B&~M)` mask blend) without re-folding the table
  /// addresses. Returns the selector value, or null when the pattern is not
  /// matched.
  llvm::Value *synthesizeTwoTableSelector(const MedBlock &Blk,
                                          const MedOp &BrOp,
                                          const JumpTable &JT,
                                          llvm::IRBuilder<> &Builder);

  /// Trace an INDIR_BR target var back to the scaled index of its table load,
  /// forwarding through a stack spill of the target (a peeled first switch
  /// iteration stores the table-loaded target to a slot and reloads it before
  /// the indirect branch) so the genuine switch variable is recovered.
  std::optional<MedVar> findSwitchIndex(const MedBlock &Blk,
                                        const MedVar &Target) const;

  /// The frame-slot key (addrSlotKey form) that a dispatch block's INDIR_BR
  /// target is reloaded from, for the -O0 computed-goto shape `t = load [slot];
  /// br t`.  Empty if the target is not a frame reload.
  std::optional<std::pair<std::pair<int, int>, int64_t>>
  reloadSlotKeyOf(const MedBlock &Blk, const MedOp &BrOp) const;

  /// In predecessor goto-site block \p Pred, find its store to \p SlotKey and
  /// trace the stored (table-loaded) value back to its scaled index.
  std::optional<MedVar> tracePredSwitchIndex(
      const MedBlock &Pred,
      const std::pair<std::pair<int, int>, int64_t> &SlotKey) const;

  /// Shared core for constant-index computed-goto recovery: trace \p Val (a
  /// stored/branched table-loaded target) through \p defOf to its LOAD, fold
  /// the load address to a pure constant VA, and map it to a \p JT table entry
  /// (BaseAddr/EntrySize) — returning that entry's switch-case value as a
  /// constant index.  Returns nullopt when the address is not a pure constant
  /// inside \p JT's table (a variable-index load has a non-constant leaf and
  /// does not fold; a constant into a different table maps out of range), so it
  /// never produces a spurious index.
  std::optional<MedVar> constIndexFromTableLoad(
      const MedVar &Val,
      const std::function<const MedOp *(const MedVar &)> &defOf,
      const JumpTable &JT) const;

  /// A CONSTANT-index goto-site (`goto *tab[k]` with literal k) is folded by
  /// clang -O0 to a load from a constant table-slot address, leaving no scaled
  /// index for tracePredSwitchIndex to recover.  In predecessor \p Pred, find
  /// its store to \p SlotKey and resolve the constant index via
  /// constIndexFromTableLoad.  (The -O0 shared-dispatch shape, e.g. AArch64,
  /// where every goto-site funnels through one dispatch block.)
  std::optional<MedVar> tracePredConstDispatchIndex(
      const MedBlock &Pred,
      const std::pair<std::pair<int, int>, int64_t> &SlotKey,
      const JumpTable &JT) const;

  /// A single-site constant-index goto (`jmp *tab+k`): the table read is in (or
  /// dominates) the dispatch block and has no scaled index, so the
  /// variable-index tracers miss it.  Trace \p BrOp's target through a
  /// function-wide def map and resolve the constant index via
  /// constIndexFromTableLoad.  Covers the shape where each goto-site is its own
  /// indirect branch rather than funneling into a shared dispatch (e.g. 32-bit
  /// x86/ARM -O0).  Returns nullopt unless the target folds to a pure constant
  /// entry of \p JT.
  std::optional<MedVar> traceConstBranchIndex(const MedOp &BrOp,
                                              const JumpTable &JT) const;

  /// The table base VA a predecessor goto-site's stored value is loaded from
  /// (the
  /// `&tab` in `tab[idx]`), restricted to a constant landing in a
  /// non-executable data segment.  Used to confirm every predecessor of a
  /// shared multi-site dispatch indexes the *same* table — clang -O0 funnels
  /// all `goto *p` in a function through ONE dispatch block, so two distinct
  /// tables (`goto *t1[i]` and `goto *t2[j]`) share it; a single switch on a
  /// merged index would then mis-route.  The data-segment filter excludes a
  /// PC-relative base's `.text` PC constant (ARM), which is per-site and not
  /// the table; such bases return nullopt and the caller treats them leniently
  /// (the common single-table threaded-dispatch shape keeps working).
  std::optional<uint64_t>
  predTableBaseVA(const MedBlock &Pred,
                  const std::pair<std::pair<int, int>, int64_t> &SlotKey) const;

  /// -O0 shared/decoupled computed-goto dispatch: the indirect branch block
  /// only reloads a spilled target (`t = load [slot]; br t`) while the scaled
  /// table load + index live in a *predecessor* goto-site block (`... load
  /// [base,idx,scale]; store [slot]; br dispatch`).  findSwitchIndex sees only
  /// the frame reload, so cross into the (single) predecessor: find its store
  /// to the same slot and trace the stored value back to its scaled index.
  /// Returns the index var (defined in the dominating predecessor, valid via
  /// getVar in the dispatch block).  Single-predecessor only; the
  /// multi-predecessor shared dispatch is handled by
  /// synthesizeSharedDispatchIndex.
  std::optional<MedVar> findSwitchIndexCrossBlock(const MedBlock &Blk,
                                                  const MedOp &BrOp) const;

  /// Multi-predecessor shared -O0 computed-goto dispatch: every goto-site
  /// stores its own index-selected target to a common slot, so no single index
  /// value dominates the dispatch.  Recover each predecessor's index, route
  /// them into a fresh common slot (a store per predecessor, deferred via
  /// PendingDispatch Stores), and return the dispatch-block load of that slot
  /// as the switch index.  Returns null when any predecessor's index cannot be
  /// recovered (the caller then leaves the branch unresolved/loud rather than
  /// mis-dispatching).
  llvm::Value *synthesizeSharedDispatchIndex(const MedBlock &Blk,
                                             const MedOp &BrOp,
                                             const JumpTable &JT,
                                             llvm::IRBuilder<> &Builder);

  llvm::Value *getMemoryPtr(llvm::Value *Addr, llvm::Type *ValType,
                            llvm::IRBuilder<> &Builder);

  /// True when \p Addr is a negative displacement within the lifted frame.
  bool isFrameRelativeDisplacement(uint64_t Addr, unsigned BitWidth) const;

  /// FrameBase + signed stack displacement; null if not applicable.
  llvm::Value *tryFrameRelativePtr(llvm::Value *Addr,
                                   llvm::IRBuilder<> &Builder);

  llvm::Constant *tryResolveGlobalData(uint64_t Addr,
                                       uint16_t DataSizeHint = 0);

  /// True when tryResolveGlobalData has a concrete address-local route for
  /// \p Addr. Loader relocation evidence identifies pointer intent, but it does
  /// not override mapping permissions, missing bytes, or segment-embedding
  /// limits. Context-sensitive routes may impose a stronger canonical-global
  /// invariant and fail closed. Keep classification and materialization on
  /// this contract so a value predicted as relocatable never silently falls
  /// back to its stale VA.
  bool canResolveGlobalDataConstant(uint64_t Addr) const;

  /// Compute the exact read-only segment run embedRodataRun would materialize.
  /// Returns false when \p SegVA is not an eligible segment start or the run
  /// cannot be represented by one bounded global.
  bool rodataRunBounds(uint64_t SegVA, uint64_t &RunStart,
                       uint64_t &RunEnd) const;

  /// Compute the exact writable segment run embedWritableRun would
  /// materialize. Returns false for non-mutable or oversized segments.
  bool writableRunBounds(uint64_t SegVA, uint64_t &RunStart,
                         uint64_t &RunEnd) const;

  /// Embed the maximal contiguous run of read-only, non-executable data
  /// segments containing the segment at \p SegVA as one internal global,
  /// preserving the original relative layout (alignment gaps zero-filled).
  /// Merging adjacent rodata segments keeps PC-relative references that cross a
  /// section boundary valid in the recompiled object (e.g. a switch-to-string
  /// `.rodata` offset table pointing into `.rodata.str1.1`).  Returns {global,
  /// run-start VA}, or {nullptr, 0} when SegVA is not an embeddable rodata
  /// segment or the run exceeds the embed cap (caller falls back to the
  /// single-segment path).  For a lone rodata segment the run is that segment,
  /// identical to the prior embed.
  std::pair<llvm::GlobalVariable *, uint64_t> embedRodataRun(uint64_t SegVA);

  /// Embed an EXECUTABLE segment's bytes (an ARM32 `.text`-embedded NEON
  /// constant pool, an x86/i386 PIC literal) as ONE read-only global so every
  /// constant load past the code resolves through a single GEP — the executable
  /// dual of embedRodataRun, replacing the per-constant O(N) overlapping copies
  /// each `vldr`/`adr` would otherwise spawn.  Cached per segment.  Returns
  /// {global, segment-start VA}, or {nullptr, 0} when the segment is empty or
  /// exceeds the embed cap.
  std::pair<llvm::GlobalVariable *, uint64_t>
  embedExecSegmentRun(const Segment *Seg);

  /// Recreate a WRITABLE data segment (.data / .bss) at \p SegVA as one
  /// cohesive mutable internal global, zero-filling holes and any zero-init
  /// (.bss) bytes. Cached per segment so every access into the segment — a
  /// direct constant load/store and a runtime-indexed load/store — resolves to
  /// the SAME global (the writable dual of embedRodataRun).  Returns {global,
  /// segment-start VA} or {nullptr, 0} when SegVA is not an embeddable writable
  /// segment.
  std::pair<llvm::GlobalVariable *, uint64_t> embedWritableRun(uint64_t SegVA);

  /// True when \p S is a genuine raw mutable-data segment (.data / .bss) the
  /// writable-data redirect owns.  Excludes RELRO (`.data.rel.ro`: writable in
  /// section flags but a relocated, read-only-after-reloc pointer table) and
  /// any segment carrying relocated pointer slots — those belong to the
  /// code/data- pointer-table machinery, not the raw-byte writable global path.
  bool isMutableDataSeg(const Segment *S) const;

  /// True when segment \p S carries any relocated code- or data-pointer slot
  /// (a `.data.rel.ro` pointer table or a string-pointer table).  Such a
  /// segment must be mirrored through buildCodePtrSegmentGlobal so every
  /// pointer entry is relocated to its recompiled target rather than emitted as
  /// a stale VA.
  bool segHasPtrRelocSlots(const Segment *S) const;

  /// True when segment \p S is read-only after relocation — a true read-only
  /// segment (.rodata) or a `.data.rel.ro` (writable in section flags but a
  /// relocated, read-only-after-reloc pointer table).  A genuinely mutable
  /// `.data`/`.bss` is excluded.  These are the segments a code-pointer mirror
  /// run may span (see buildCodePtrSegmentGlobal / readOnlyAfterRelocRun).
  bool isReadOnlyAfterReloc(const Segment *S) const;

  /// True when \p VA has object-level data provenance rather than merely
  /// falling inside a coarse readable load segment.  Exact section metadata
  /// wins when present; this keeps low immediates inside ELF/Mach-O headers
  /// from masquerading as data addresses while preserving segment-only images.
  bool hasObjectDataProvenance(uint64_t VA) const;

  /// True when \p VA names a file-backed data byte with immutable runtime
  /// semantics and a concrete global-data materialization route.  This unifies
  /// ordinary rodata (including data sections inside an executable Mach-O
  /// segment) with read-only-after-relocation pointer tables such as
  /// `__DATA_CONST` / `.data.rel.ro`; resolver provenance and emission must use
  /// the same predicate or one resolver can reject an address the next one is
  /// responsible for materializing.
  bool isMaterializableReadOnlyDataAddress(uint64_t VA) const;

  /// Compute the contiguous run of read-only-after-relocation segments (.rodata
  /// + adjacent .data.rel.ro, separated only by a small alignment gap) that
  /// contains \p Seg, returning [RunStart, RunEnd).  When \p Seg is not read-
  /// only-after-reloc, or the run would exceed the embed cap, the run is just
  /// \p Seg.  This is the span buildCodePtrSegmentGlobal mirrors so a clang-
  /// folded base+offset access that crosses the segment boundary stays in
  /// bounds.
  void readOnlyAfterRelocRun(const Segment *Seg, uint64_t &RunStart,
                             uint64_t &RunEnd) const;

  /// True when \p VA falls inside a contiguous read-only-after-relocation run
  /// that carries relocated pointer slots — i.e. the run
  /// buildCodePtrSegmentGlobal mirrors.  Addresses in such a run are
  /// re-symbolized at their access through the mirror, so getVar must keep them
  /// as the raw original VA (exactly as a
  /// `.data.rel.ro` reloc target, never recorded in RelocDataAddrs, already is)
  /// rather than redirect a `.rodata` tail to a separate per-segment global — a
  /// split that breaks an i386 PIC stack table of node pointers accessed as
  /// `mirror + (node - runStart)`.
  bool addrInCodePtrMirrorRun(uint64_t VA) const;

  /// If \p AddrVar provably indexes a single writable data segment, return that
  /// segment's start VA; 0 otherwise (no writable base, or the address mixes
  /// two distinct data segments).  Only constants in base position (an
  /// ADD/SUB/OR addend or the whole address) are considered; index sub-trees
  /// (MULT / AND / shift) are not descended, so a scale or mask that merely
  /// equals a .data VA is never mistaken for the base.
  ///
  /// When \p RequireRelocBase is set (the store/return VALUE context, as
  /// opposed to a load/store ADDRESS), a constant base must additionally be a
  /// recorded writable relocation target (Img->WritableRelocDataAddrs) to
  /// count.  On 32-bit targets a 4-byte data value is pointer-width, so an
  /// ordinary data immediate landing inside a wide low-VA run would otherwise
  /// be mistaken for a stored-pointer base; the relocation set is the ground
  /// truth for "address".
  uint64_t writableDataSegOf(const MedVar &AddrVar,
                             bool RequireRelocBase = false) const;

  /// True when the stack slot identified by \p Key (an addrSlotKey result) has
  /// its ADDRESS escape the current function — passed as a call argument or
  /// stored to memory — so a callee may overwrite the slot with an already-
  /// symbolized (recompiled) pointer.  A store-to-load forward of such a slot's
  /// in-function constant base would double-relocate it (the escaping output-
  /// pointer shape), so the spill base recovery skips it.
  bool stackSlotAddressEscapes(const MedVar &SlotAddr) const;

  /// True when some LOAD reads the same constant stack slot (identical
  /// addrSlotKey) that \p StoreAddr writes.  Such a fixed-offset reload is
  /// where the store-to-load forward re-symbolizes a spilled global base at the
  /// use (GEP(@run, val-segVA)), so the spill must keep the original VA.  Its
  /// absence means the slot is read only through a runtime index (a local
  /// pointer array `t[k]`, whose indexed load key never matches the constant
  /// store key) and is never re-symbolized — so a global-address store to it
  /// must itself be symbolized, else the loaded pointer is a stale absolute VA.
  bool frameSlotHasMatchingKeyLoad(const MedVar &StoreAddr) const;

  /// Collect the exact-width values that can reach a LOAD from one local,
  /// non-escaping frame slot.  The backward CFG walk requires every ordinary
  /// and exceptional predecessor path to encounter a full-width STORE to the
  /// exact same addrSlotKey before any possibly-aliasing frame write.  It
  /// rejects bypass/uninitialized paths, partial or overlapping writes,
  /// ambiguous frame addresses, and cycles without a proven definition.  The
  /// returned values are provenance candidates only; callers must still prove
  /// what each value represents.
  bool collectFrameReloadSources(const MedOp &Load,
                                 std::vector<MedVar> &Sources) const;

  /// Best-effort fold of \p V to the absolute integer/address it computes,
  /// following the address arithmetic the lifter emits for an i386 PIC global
  /// reference — a base register that itself resolves to 0 (the get_pc_thunk
  /// GOT base, modelled as `0xC + 0xFFFFFFF4` in the relocatable view) plus a
  /// GOTOFF constant — through COPY / INT_ZEXT / INT_SEXT / SUBBYTES@0 /
  /// INT_ADD / INT_SUB with width-aware masking so a 32-bit add wraps like the
  /// hardware. Returns nullopt when any leaf is not a foldable constant chain.
  /// Used to see through the SUBBYTES(ZEXT(...)) widening that wraps a spilled
  /// pointer value (traceSSAConst only follows COPY and so cannot).
  /// Depth-bounded.
  std::optional<uint64_t> traceValueVA(const MedVar &V, int Depth = 0) const;

  /// True when a value reloaded from the stack slot \p StoreAddr writes is used
  /// LOCALLY — consumed by any non-forwarding op (a memory LOAD/STORE address,
  /// or an arithmetic/compare operand) rather than merely being forwarded
  /// (COPY/widen) to the return register.  A read-only pointer used locally
  /// (dereferenced `*p`, walked `p++`, or differenced `p - base`) must keep its
  /// original VA so the deref / pointer-difference resolvers resolve it exactly
  /// once; only a slot whose reload PURELY escapes (e.g. a switch returning
  /// const string pointers) may have its read-only pointer symbolized at the
  /// store.
  bool frameSlotReloadUsedLocally(const MedVar &StoreAddr) const;

  /// Drop the stack-slot address-predicate memo caches when \ref CurMedFunc
  /// changes, so each function starts with empty caches.
  void ensureAddrPredCache() const;

  /// Redirect a load/store address that lands in a writable data segment into
  /// the cohesive global from embedWritableRun, as GEP(@run, rawAddr -
  /// runStart) — getVar reproduces the original absolute address (a low
  /// writable base is not redirected), so any `or disjoint` low-bit assumption
  /// stays valid.
  /// \p IsValueOperand marks the call as resolving a stored/returned VALUE
  /// rather than a load/store ADDRESS; it requires a relocation-confirmed base
  /// (see writableDataSegOf) so a 32-bit data value is not mis-symbolized as a
  /// pointer.
  llvm::Value *tryResolveWritableData(const MedVar &AddrVar, uint16_t SizeHint,
                                      llvm::IRBuilder<> &Builder,
                                      bool IsValueOperand = false);

  /// True when \p AddrVar is (through COPY/ZEXT/SEXT) a reload of a
  /// non-escaping stack slot whose matching STORE value is itself a
  /// writable-global pointer the emitter already symbolizes — an induction
  /// pointer `q = &G[i]` spilled and reloaded at -O0.  getVar of such a reload
  /// is ALREADY the relocatable
  /// `@G + i` pointer, so tryResolveWritableData must use it directly instead
  /// of re-basing it against @G (which would reference the global twice, the
  /// #507 bug-A double-base shape applied to writable data).
  bool reloadsSymbolizedWritablePtr(const MedVar &AddrVar) const;

  /// i386-only: true when \p AddrVar is a walked stack-spilled pointer being
  /// dereferenced (`phi(slot_load, q+stride)`), needing GEP(@G,off) not
  /// inttoptr.
  bool i386WalkedPointerDeref(const MedVar &AddrVar) const;

  /// i386-only: true when the current function has at least one LOAD whose
  /// address is a walked stack-spilled pointer (`q++` search family).
  bool funcUsesI386WalkedPointerDeref() const;

  /// i386-only: true for the -O2 init-loop unroll store address
  /// (`ADD(RAX_peel_phi, {-12|-8|-4|0})` / bare `RAX_peel_phi`) where the PHI
  /// is the segment-offset induction base, not a walked stack-spilled `q`.
  bool i386PeeledInitStoreAddr(const MedVar &AddrVar, uint64_t SegVA) const;

  /// i386-only: `ADD(off, base_const)` where \p base_const lands inside the
  /// writable segment at \p SegVA.  Returns the runtime offset operand in
  /// \p OffVar and the matched base constant VA in \p BaseConstVA, so the
  /// caller can GEP `@G` by `off + (BaseConstVA - runStart)` — the in-segment
  /// field displacement (`&st.hist[i]` = `&st + 8 + i`) must NOT be dropped
  /// just because the base const is an interior address rather than the segment
  /// start.
  bool i386WritableSegBasePlusOff(const MedVar &AddrVar, uint64_t SegVA,
                                  MedVar &OffVar, uint64_t &BaseConstVA) const;

  /// True when \p AddrVar's MedIR expression carries a constant in the segment
  /// at \p SegVA that getVar already symbolizes to that segment's relocated
  /// global.  The global reference is hidden behind the virtual-register
  /// alloca/load in the emitted value, so this must be checked on the MedIR.
  /// Writable-data and code-pointer-segment resolvers use the result directly
  /// rather than re-basing it, which would apply the relocated base twice.
  bool addrHasSymbolizedSegConst(const MedVar &AddrVar, uint64_t SegVA) const;

  std::optional<uint64_t> traceSSAConst(const MedVar &V) const;

  /// Conservatively fold a side-effect-free integer SSA expression used as a
  /// control condition. Width changes, subpieces, arithmetic/bitwise ops,
  /// comparisons, boolean ops, and a SELECT with a proven condition are
  /// supported; PHIs, memory, calls, and any unrecognised operation stay
  /// unknown. This is control-feasibility evidence, never a value rewrite.
  std::optional<uint64_t> traceControlConst(const MedVar &V) const;

  /// Build the ordinary/exceptional CFG edges reachable from the function
  /// entry while pruning only COND_BR edges whose condition folds through
  /// traceControlConst. Unknown conditions retain both edges.
  void ensureFeasibleEdgeCache() const;

  /// Index PHI ownership and structural CFG edges for exact incoming-edge
  /// classification without rescanning every block for every proof query.
  void ensurePhiEdgeIndex() const;

  enum class PhiEdgeFeasibility { Infeasible, ProvenFeasible, Unknown };

  /// Classify a PHI incoming edge without converting malformed/missing CFG
  /// structure into positive reachability evidence. Provenance consumers treat
  /// Unknown as reachable uncertainty; recurrence proofs require
  /// ProvenFeasible.
  PhiEdgeFeasibility classifyPhiIncomingEdge(const PhiNode &Phi,
                                             int PredId) const;

  /// True unless the conservative feasible-edge graph proves this incoming
  /// edge dead. Malformed input remains reachable uncertainty so all-arms
  /// pointer proofs fail closed.
  bool phiIncomingEdgeFeasible(const PhiNode &Phi, int PredId) const;

  /// True when this particular proven-feasible PHI input carries an exact
  /// recurrence to the root output or through an overlapping wide/narrow
  /// register-alias sibling PHI.
  bool phiIncomingIsRecurrent(const PhiNode &Phi, int PredId,
                              const MedVar &Arg) const;

  bool phiIncomingIsRecurrentImpl(const PhiNode &Phi, int PredId,
                                  const MedVar &Arg) const;

  /// True when an incoming value's defining DAG reaches the PHI output or a
  /// mutually recurrent sibling PHI (for example, x86 wide/narrow register
  /// aliases). Such a loop-carried PHI belongs to the induction resolver and
  /// must not be treated as an ambiguous one-shot pointer selection.
  bool phiIsSelfRecurrent(const PhiNode &Phi) const;

  /// Prove that a value remains a numeric address offset in rebuilt IR.  Every
  /// feasible PHI/selection arm and arithmetic input must be scalar; a mapped
  /// data constant, relocatable constant, LOAD, unknown CFG arm, or optional
  /// forbidden recurrence makes the proof fail closed.
  bool valueIsStableAddressOffset(const MedVar &V,
                                  const MedVar *Forbidden = nullptr) const;

  bool valueIsStableAddressOffsetImpl(const MedVar &V,
                                      const MedVar *Forbidden) const;

  /// Return the sole input of an operation only when the emitted operation is
  /// guaranteed to preserve that input's complete unsigned pointer value.
  /// This deliberately rejects values truncated below the target pointer width,
  /// non-zero SUBBYTES, and sign extensions whose sign bit cannot be proved
  /// clear. A wider MedIR temporary narrowed back to the machine pointer width
  /// still preserves the complete target pointer payload.
  std::optional<MedVar> pointerPreservingInput(const MedOp &Op) const;

  /// True when SELECT's emitted coercion and output store preserve each value
  /// arm at one complete width. A truncating/mixed-width SELECT cannot
  /// transport pointer provenance even though its control semantics are valid.
  bool selectPreservesPointerValues(const MedOp &Op) const;

  /// Recognize the exact complementary-mask OR used to lower a branchless
  /// pointer SELECT: OR(AND(value0, mask), AND(value1, NOT(mask))).  The mask
  /// must cover the complete value/AND/OR width and derive from a proven
  /// boolean; returns the condition and the true/false value arms.
  bool isMaskedSelectOr(const MedOp &Or, MedVar &Cond, MedVar &ArmT,
                        MedVar &ArmF) const;

  /// Record one sticky module-level refusal for an ambiguous data-pointer PHI.
  void failAmbiguousDataPointerPhi(const PhiNode &Phi) const;

  /// Fold an indexed-table base to a constant, additionally resolving
  /// `INT_ADD` of two constants and a `LOAD` from a read-only segment (an ARM
  /// literal-pool `ldr rN,[pc,#imm]` feeding `add rN,pc` to a `.rodata` table).
  /// When \p SawLoad is non-null it is set if a literal-pool LOAD was folded.
  /// \p OriginSize receives the width of a forwarded constant leaf, and
  /// \p SawArithmetic records any ADD fold so callers cannot mistake
  /// COPY(ADD(...)) for a pure materialization.
  std::optional<uint64_t>
  traceTableBaseConst(const MedVar &V, int Depth = 0, bool *SawLoad = nullptr,
                      uint16_t *OriginSize = nullptr,
                      bool *SawArithmetic = nullptr) const;

  /// Recover the possible read-only data targets of a pointer-width LOAD from
  /// a loader-proven absolute data-pointer relocation table, including a
  /// width-preserving ADD/SUB by a proven numeric constant.  Such a value is
  /// emitted from the relocation mirror and therefore remains an already
  /// symbolized pointer, not the raw table-slot bytes.
  bool recoverAbsoluteDataPointerLoadTargets(const MedVar &V,
                                             std::set<uint64_t> &Targets) const;

  /// Recover `table_base + sext(load32(table_base + index))` when the loaded
  /// slots are loader-proven PC-relative data-pointer relocations.  Returns the
  /// possible target VAs and whether the table base is materialized by getVar
  /// as a relocatable global (otherwise the resulting pointer retains a raw
  /// VA).
  bool recoverRelativeDataPointerTargets(const MedVar &V,
                                         std::set<uint64_t> &Targets,
                                         bool &Symbolized) const;

  /// Resolve an ARM literal-pool value-table access `load[ (ldr[pc] + pc) +
  /// index*scale ]` into a GEP on the embedded read-only `.rodata` global.
  llvm::Value *tryResolveLiteralPoolTable(const MedVar &AddrVar,
                                          uint16_t SizeHint,
                                          llvm::IRBuilder<> &Builder);

  /// Resolve a `(cond ? A : B)[i]` literal-pool table access whose base is a
  /// SELECT of two distinct rodata globals — `INT_ADD(SELECT(baseA, baseB),
  /// idx)` — into `select(cond, &A[idx], &B[idx])` over the rebuilt globals.
  llvm::Value *tryResolveSelectBaseLitTable(const MedVar &AddrVar,
                                            uint16_t SizeHint,
                                            llvm::IRBuilder<> &Builder);

  /// Resolve a nested/chained multi-way table select whose base is a
  /// *cross-block PHI* merging two-way selects/blends of rodata table bases —
  /// `(c0 ? (c1?A:B) : (c2?C:D))[i]`, which clang -O2 lowers to branches plus a
  /// `PHI(blend1, blend2)` base rather than a flat select.  Every candidate
  /// table lives in one read-only segment (merged into one embedded global), so
  /// the access is anchored uniformly as `@run + (getVar(addr) - run_start)` —
  /// the PHI value still carries the original VA of whichever table was
  /// selected, so the offset is exact without resolving each arm in its
  /// predecessor block.
  llvm::Value *tryResolveSelectMergeTable(const MedVar &AddrVar,
                                          uint16_t SizeHint, bool FailClosed,
                                          llvm::IRBuilder<> &Builder,
                                          bool *SawAmbiguous = nullptr);

  /// Resolve a *direct* (no runtime index) literal-pool data pointer — the ARM
  /// `ldr rN,[pc]; add rN,pc; ldr/vld [rN]` address-of a read-only constant
  /// (e.g. a `.rodata.cst16` aggregate initializer) — to the embedded global at
  /// that VA.  The address folds to a pure constant through a literal-pool
  /// LOAD, which the table/induction resolvers (they require a runtime index /
  /// PHI) and the bare-constant path (the VA is computed, never a constant
  /// operand) both miss, leaving the load to read a stale absolute VA.
  llvm::Value *tryResolveLiteralPoolBase(const MedVar &AddrVar,
                                         uint16_t SizeHint,
                                         llvm::IRBuilder<> &Builder);

  /// Resolve a rodata-walking induction pointer `p = PHI(base, p + stride)`
  /// whose entry \c base folds (through a literal-pool / rip-relative load) to
  /// a constant read-only address, into a GEP on the embedded `.rodata` global.
  /// Higher register pressure makes clang reach a hoisted constant array
  /// through such a pointer instead of `base + index`, which the table
  /// resolvers above (they require an INT_ADD/INT_SUB address) cannot redirect.
  llvm::Value *tryResolveInductionGlobalPtr(const MedVar &AddrVar,
                                            uint16_t SizeHint, bool FailClosed,
                                            llvm::IRBuilder<> &Builder);

  /// Resolve a computed immutable-data address through the shared resolver
  /// arbitration order.  The generic all-arms proof audits every reachable
  /// source first, explicitly deferring a pure recurrent PHI to the induction
  /// resolver; an ambiguous merge stops before any narrower indexed/literal
  /// recognizer can claim it.  This single entry point keeps LOAD and pointer-
  /// argument lowering from drifting into different ownership/fail-closed
  /// behavior.
  llvm::Value *tryResolveReadOnlyDataPtr(const MedVar &AddrVar,
                                         uint16_t SizeHint, bool FailClosed,
                                         llvm::IRBuilder<> &Builder);

  /// Resolve a load whose address indexes a `.data.rel.ro` function-pointer
  /// table (the callback-table / vtable / threaded-dispatch code-pointer array)
  /// into a GEP on a synthesized `[N x iptr]` global of `ptrtoint @func`
  /// references.  Without this the table keeps the original absolute function
  /// VAs, which point nowhere once the recompiled object is relinked, so the
  /// indirect call through it faults.  Null when \p AddrVar is not such a
  /// table.
  llvm::Value *tryResolveCodePtrTablePtr(const MedVar &AddrVar,
                                         llvm::IRBuilder<> &Builder);

  /// Resolve a load/store whose address folds to a constant inside a segment
  /// holding relocated pointer slots — a *writable* function-pointer global
  /// (`static int (*fp)(int)=f; fp=cond?f:g;`) in plain .data, whose i386 PIC
  /// address `GOT_base + slot@GOTOFF` getVar folds to the slot VA but the
  /// lighter traceSSAConst does not (so the LOAD/STORE constant-address path
  /// misses it) and tryResolveWritableData rejects (a pointer-slot segment is
  /// not raw mutable data).  Mirrors the segment via buildCodePtrSegmentGlobal
  /// (writable, each slot relocated) and GEPs in by byte offset.  Null when the
  /// address does not fold into such a segment.
  llvm::Value *tryResolveCodePtrSegPtr(const MedVar &AddrVar,
                                       llvm::IRBuilder<> &Builder);

  /// When the table base is not a single liftable constant — a `cond ? A[i] :
  /// B[j]` pointer select lowers to a SELECT (or a branchless AND/OR mask) of
  /// two base constants, which collectIndexedGlobalBase cannot isolate — return
  /// the base VA of the SINGLE non-executable pointer-table segment that EVERY
  /// base-like constant reachable in \p AddrVar resolves into, so the whole
  /// address provably indexes that segment and can be redirected by its own
  /// value.  Returns 0 when the address has no such base, spans more than one
  /// such segment, or any base constant would itself be getVar-redirected (so
  /// the address value would not be the raw original VA).  Walks the address
  /// arithmetic (ADD/SUB/AND/OR/XOR/shift/mul/SELECT/PHI + width casts) and
  /// stops at a LOAD (a loaded value is not a base materialization).
  uint64_t ptrTableUniqueSegment(const MedVar &AddrVar) const;

  /// True when constant value \p Val is used in the current function as a
  /// genuine integer that merely coincides with a rodata relocation-target VA —
  /// specifically a loop counter: a PHI incoming constant whose PHI output is
  /// never a memory-address operand (only decremented/compared).  Such a value
  /// (e.g. an i386 trip count 160 == `.rodata` chunk VA 0xA0) must not be
  /// redirected to a global by getVar.  A genuine rodata pointer materialized
  /// into a PHI (an induction base) has its PHI output used as an address, so
  /// it is NOT flagged and stays redirected.
  bool constValueUsedAsInteger(uint64_t Val) const;

  /// True when \p Val is exactly one byte past the end of an embeddable
  /// read-only rodata segment (`&tab[N]`, a loop bound), and no other mapped
  /// segment begins there.  Such a one-past-the-end pointer must be symbolized
  /// to the recompiled run's end like the table's interior pointers, or a
  /// `p < &tab[N]` comparison mixes a recompiled pointer with a stale original
  /// VA bound and fires at the wrong element.
  bool constIsRodataEndPointer(uint64_t Val) const;

  /// True when constant \p Val is the one-past-the-end address of a WRITABLE
  /// mutable (.data/.bss) segment whose body is already on the
  /// recompiled-pointer model (some taken-address in it is symbolized —
  /// hasSymbolizedWritableSibling) AND \p Val is used as a pointer (a walk
  /// bound `for (p = G; p < G + N; p++)`). The writable counterpart of
  /// constIsRodataEndPointer: a walked pointer's init and reset are symbolized
  /// to `@run`, so the loop bound `&G[N]` must be symbolized to the run's
  /// one-past-end GEP too, or the `p < &G[N]` comparison mixes a recompiled
  /// pointer with a stale original-VA bound and fires at the wrong element (the
  /// ptrcmp family).  Gated on the segment already being symbolized so a
  /// raw-rebased walk (whose base stays an original VA) keeps its bound raw —
  /// both models stay internally consistent.
  bool constIsWritableRunEndPointer(uint64_t Val) const;

  /// Lazily (re)build the per-function (Kind,Id,SSAVer)->defining-op and ->phi
  /// indexes when \ref CurMedFunc changes.  Lets the const-classification
  /// backward walks resolve a variable's definition in O(1) instead of scanning
  /// every op in the function (the scan made constUsedAsPointer O(ops^3) on a
  /// large NEON function, the armv48_biggather lift hot path).
  void ensureDefPhiIndex() const;

  /// O(1) definition / phi lookup for \p V (the first defining op/phi, matching
  /// the previous linear "return first match" scans).  Returns nullptr for a
  /// constant or an undefined variable.  Backed by \ref ensureDefPhiIndex.
  const MedOp *lookupDef(const MedVar &V) const;
  const PhiNode *lookupPhi(const MedVar &V) const;

  /// Recursive worker for \ref varIsFrameDerived.  Walks the operand DAG of
  /// \p V once (the \p Visited set collapses shared sub-expressions and breaks
  /// SSA back-edges), resolving each value's definition through the O(1)
  /// \ref lookupDef / \ref lookupPhi index rather than rescanning every op in
  /// the function.
  bool frameDerivedRec(const MedVar &V,
                       llvm::DenseSet<std::pair<int64_t, int>> &Visited) const;

  /// Per-function memoization for the two const classifiers below.  Both are
  /// pure functions of (CurMedFunc, Val), but each call is O(ops^2) and getVar
  /// invokes them once per constant operand, so the same ConstVal is recomputed
  /// many times on a large function — even after the def/phi index above the
  /// two classifiers were ~60% of emit time on the armv48_biggather lift hot
  /// path. ensureConstClassCache drops both caches when CurMedFunc changes; the
  /// public constUsedAsPointer / constValueUsedAsInteger are thin
  /// lookup-or-compute wrappers around these *Impl bodies (which hold the
  /// unchanged analysis).
  void ensureConstClassCache() const;
  bool constUsedAsPointerImpl(uint64_t Val) const;
  bool constValueUsedAsIntegerImpl(uint64_t Val) const;

  /// True when constant \p Val is used as a genuine pointer in the current
  /// function: it (or a value derived from it through address arithmetic) is a
  /// LOAD/STORE address, or it is compared / added / subtracted against a value
  /// that is itself a pointer (the `p != end`, `end - begin` idioms).  A
  /// constant that only flows into value computations — a SIMD lane immediate
  /// assembled into a vector register, a shift/or chain stored as data — is NOT
  /// a pointer even when it collides with a rodata segment's one-past-end VA.
  bool constUsedAsPointer(uint64_t Val) const;

  /// True when getVar symbolizes the constant \p Val (a taken address `&G` the
  /// loader proved is a relocation target in a WRITABLE .data/.bss segment) to
  /// a relocatable `ptrtoint @G` even though its pointer use is data-flow-
  /// disconnected — clang vectorizes `tab[i]=&G` into `movq %rax,%xmm` +
  /// `pshufd` so &G enters a SIMD lane, is stored as part of an i128 vector,
  /// and reloaded elsewhere; constUsedAsPointer cannot see through that
  /// store/reload but the relocation is ground truth.  Shared by getVar (to
  /// symbolize) and addrHasSymbolizedSegConst (so a store/load address whose
  /// base getVar already symbolized is not re-based a second time).  Gated on
  /// >= pointer width and !constValueUsedAsInteger so a sub-word or non-pointer
  /// integer that merely equals a reloc-target VA stays an integer.
  bool symbolizesWritableRelocPtr(uint64_t Val, uint16_t Size) const;
  /// The writable relocation predicate before checking whether its owning
  /// mutable run fits the embedding contract.
  bool writableRelocPtrHasSymbolizationIntent(uint64_t Val,
                                              uint16_t Size) const;

  /// True when some OTHER constant in the same writable segment as \p Val is a
  /// proven writable relocation target that getVar already symbolizes to a
  /// recompiled `@G` pointer (it is used as a pointer and is not flagged as an
  /// integer).  A sibling global address being symbolized means the segment's
  /// taken-address values are on the recompiled-pointer model, so a peer value
  /// the loader equally proved is a `&G` must join it for consistency —
  /// otherwise a branchless pointer blend `cond ? &A : &B` symbolizes one arm
  /// (`&B`, a clean pointer) while the degenerate-PHI integer heuristic
  /// mis-flags the other (`&A`, whose low VA clang hoisted into an invariant
  /// self-PHI), leaving a mixed addressing model.  A genuinely WALKED base (`p
  /// = &G; *(p ± k)`, pdtwo) has no symbolized sibling — its accesses stay raw
  /// VA through the embedded-run rebase — so it is never pulled onto the
  /// pointer model here. Result cached per function (SymbolizedWritableSegsFor
  /// / -Segs).
  bool hasSymbolizedWritableSibling(uint64_t Val) const;

  /// True when \p Val is one arm of a pointer SELECT (`cond ? &A : &B`) whose
  /// OTHER arm is a constant in the SAME writable segment that getVar already
  /// symbolizes to a recompiled `@G` pointer (symbolizesWritableRelocPtr).  On
  /// AArch64 (and any PC-relative-only reference) the loader records just the
  /// reloc-target arm in WritableRelocDataAddrs — a sibling base reached purely
  /// through an ADRP+ADD is `Wreloc=0`, so the reloc gate alone leaves it a raw
  /// VA while the peer arm symbolizes, a mixed model.  Being the select peer of
  /// a proven recompiled pointer in the same global is itself strong proof \p
  /// Val is that segment's taken-address, so it joins the recompiled-pointer
  /// model. Gated (by the caller) on \p Val being used as a pointer and NOT a
  /// walked base, so a colliding integer or an induction base is never pulled
  /// in.
  bool symbolizesSelectPeer(uint64_t Val) const;

  /// True when \p Val is the common base of a pointer DIFFERENCE — an INT_SUB
  /// whose two operands both derive (through address arithmetic) from the
  /// constant \p Val (`q - p` where `q = &G + i`, `p = &G + j`).  Such a base
  /// MUST keep its original VA: a difference cancels the base only when both
  /// pointers share one addressing model, so symbolizing the base to a
  /// recompiled
  /// `@G` here (while the walked pointers thread their original VA through the
  /// embedded-run rebase) corrupts the index the difference recovers.  This is
  /// the pdtwo/pdsearch/pdwalk family — a walked base the peer-consistency
  /// symbolization must not pull onto the recompiled-pointer model.
  bool valIsPointerDiffBase(uint64_t Val) const;

  /// True when \p Val is a base materialized into a self-advancing induction
  /// PHI
  /// (`p = PHI(&G + init, p ± stride)`, with an optional wrap-around SELECT
  /// reset to &G).  Such a walked base must keep its original VA so the
  /// embedded-run rebase stays consistent across every increment; symbolizing
  /// it to a recompiled `@G` at materialization mixes models with a sibling raw
  /// arm. Distinguishes a WALKED base (keep raw) from a loop-invariant CARRIED
  /// base a compiler hoists into a DEGENERATE self-PHI but re-forms as a fresh
  /// `cond ? &A : &B` pointer each iteration (symbolize).
  bool valIsAdvancingInductionBase(uint64_t Val) const;

  /// True when \p Val is the ORIGINAL VA of a read-only C-string that an
  /// induction pointer walks: a constant reachable from a dereferenced PHI's
  /// address arithmetic, in a non-writable / non-executable rodata segment (no
  /// relocated pointer slots) whose bytes form a C-string.  Such a walk on
  /// i386/ARM32 PIC materializes its base BOTH ways for one pointer — the reset
  /// arm via the GOTOFF/literal-pool relocation (getVar symbolizes it) and the
  /// advance arm via bare `base + k` arithmetic (getVar leaves it an integer) —
  /// so the PHI mixes a recompiled VA with an original VA and every addressing
  /// model corrupts one arm.  Flagging every such base lets getVar symbolize
  /// them UNIFORMLY to the one canonical rodata run global and the induction
  /// resolver emit a plain load through that consistent recompiled pointer.
  /// x86-64/AArch64 keep the base a bare origVA constant and never reach this.
  /// Result cached per function (InductionBasesFor / InductionBaseVAs).
  bool isInductionRodataStringBase(uint64_t Val) const;

  /// If \p V is a PC-relative materialization of a function's address that the
  /// optimizer left computed rather than folded to a constant (the ARM
  /// `ldr rN,[pc]; add rN,pc,rN` literal-pool idiom), resolve it to
  /// `ptrtoint @func`.  This is the non-constant dual of the function-pointer
  /// symbolization in getVar (which handles the folded-constant `lea rip` /
  /// `adrp+add` forms): a literal-pool load keeps an INT_ADD with a LOAD
  /// operand that never folds, so the constant path never sees it.  Null when
  /// \p V does not fold (through a literal-pool load) to a known function
  /// entry.
  llvm::Value *tryResolveCodeRefValue(const MedVar &V,
                                      llvm::IRBuilder<> &Builder);

  /// Resolve an original executable VA in an address-value context.  Callable
  /// entries become llvm::Function constants; relocation-proven labels inside
  /// an emitted function become llvm::BlockAddress constants.
  llvm::Constant *resolveLiftedCodeAddress(va_t Address);

  /// Build (and cache) a constant global mirroring the entire read-only data
  /// segment that contains \p SlotVA, with every code-pointer relocation slot
  /// emitted as a relocatable `ptrtoint @func` or
  /// `ptrtoint(blockaddress(...))` field and the surrounding bytes preserved as
  /// byte arrays in a packed struct.  This handles a compact pointer table, a
  /// strided struct-of-pointers array (a vtable), and the data fields beside
  /// the pointers uniformly, since any access GEPs into it by byte offset. Sets
  /// \p OutSegVA to the segment base.  Null when \p SlotVA is not applicable;
  /// an unresolved executable target also sets FatalCodePointerResolution so
  /// no caller may fall back to stale raw bytes.
  llvm::Constant *buildCodePtrSegmentGlobal(uint64_t SlotVA,
                                            uint64_t &OutSegVA);

  /// If \p AddrVar is defined by `INT_ADD(const_base, index)` (or the symmetric
  /// form) where const_base lands in a resolvable read-only/data segment — i.e.
  /// a compiler lookup table reached via `lea table; mov (table,index)` —
  /// return a pointer GEP'd into the table global by the variable index.  Null
  /// otherwise.  Keeps such tables in the recompiled object instead of emitting
  /// a bare `inttoptr <small absolute>` that reads unmapped memory.
  ///
  /// Only used for LOADs: a genuine lookup table is read-only.  A base that is
  /// also a STORE target in this function is a read-write array (often a stack
  /// array the frame analysis modelled with an absolute address) and is left as
  /// an absolute access so its store/load pair stays consistent.
  llvm::Value *tryResolveIndexedGlobalPtr(const MedVar &AddrVar,
                                          uint16_t SizeHint, bool FailClosed,
                                          llvm::IRBuilder<> &Builder);

  /// Speculatively symbolize an integer call argument that may be a pointer,
  /// mirroring the LOAD/STORE address resolvers.
  /// A computed `&global[index]` passed to a function would otherwise stay a
  /// raw `inttoptr(baseVA + index)` — the original table VA — so the callee's
  /// load/store lands at a stale absolute address while the caller's own direct
  /// accesses of the same global are symbolized, and the two stop aliasing.
  /// Tries the writable-data resolver first (a pointer arg may be written
  /// through), then the read-only table/induction resolvers, then a constant
  /// `&global`.  Null when \p AddrVar is not a resolvable global address (a
  /// stack pointer, an opaque runtime pointer) — the caller then retains its
  /// ordinary ABI value.  When \p FailClosed is true, an expression that mixes
  /// proven table provenance with an unproved arm is a module-level error;
  /// callers must set it only for an ABI parameter known to be dereferenced,
  /// never for speculative integer-argument recognition.
  llvm::Value *tryResolvePointerArg(const MedVar &AddrVar, bool FailClosed,
                                    llvm::IRBuilder<> &Builder);

  /// Constant base of `INT_ADD(const_base, index)` defining \p AddrVar, if any.
  std::optional<uint64_t> indexedConstBase(const MedVar &AddrVar) const;

  /// Walk the INT_ADD tree of an address expression, separating the single
  /// global-data base constant from the runtime index addends.  Handles a base
  /// nested under multi-dimensional indexing (`base + row*stride + col`), which
  /// a one-level `INT_ADD(const,index)` match misses.  Returns false if two
  /// base constants appear (not a simple table access); otherwise sets \p
  /// HaveBase /
  /// \p Base and appends every non-base addend to \p IdxTerms. When
  /// \p FailClosed is true, a reachable PHI that mixes a raw mapped base with
  /// an unproved arm marks emission fatal; speculative code-pointer and
  /// literal-pool probes leave it false so a later all-arms resolver can own
  /// the decision. \p SawAmbiguousPhi, when supplied, is set independently of
  /// that diagnostic policy so a speculative caller can reject the candidate
  /// without making the whole module fatal.
  bool collectIndexedGlobalBase(const MedVar &V, uint64_t &Base, bool &HaveBase,
                                std::vector<MedVar> &IdxTerms, int Depth = 0,
                                bool FailClosed = false,
                                bool *SawAmbiguousPhi = nullptr) const;

  /// Pure recursive implementation behind collectIndexedGlobalBase. Diagnostic
  /// policy is replayed by the wrapper so a speculative cached query cannot
  /// suppress a later strict query for the same value.
  bool collectIndexedGlobalBaseImpl(const MedVar &V, uint64_t &Base,
                                    bool &HaveBase,
                                    std::vector<MedVar> &IdxTerms, int Depth,
                                    bool *SawAmbiguousPhi,
                                    const PhiNode **AmbiguousPhi) const;

  /// Literal-pool variant of collectIndexedGlobalBase: the base must fold
  /// through a literal-pool LOAD (the ARM `ldr rN,[pc]; add rN,pc` idiom), and
  /// likewise descends an INT_ADD tree so a base nested under multi-dimensional
  /// indexing is found.  Each non-base operand becomes one index term.
  bool collectLiteralPoolBase(const MedVar &V, uint64_t &Base, bool &HaveBase,
                              std::vector<MedVar> &IdxTerms,
                              int Depth = 0) const;

  /// True when \p V is (or COPY/INT_ADD/INT_SUB-derives from) the stack-pointer
  /// register.  A genuine table index is a data value, never a stack pointer,
  /// so a frame-derived "index" marks `INT_ADD(sp, k)` epilogue pointer
  /// arithmetic (e.g. `pop`'s `rsp += 8`) that must not be rewritten into a
  /// table GEP.
  bool varIsFrameDerived(const MedVar &V, int Depth = 0) const;

  /// True when \p V derives from the entry stack pointer through the identity,
  /// width and adjustment ops the lifter threads SP through — COPY, SUBBYTES,
  /// INT_ZEXT/SEXT (the sub-register round-trips `mov`/`push` emit), the
  /// alignment INT_AND, and the INT_ADD/INT_SUB that move it.  Only the base
  /// operand is followed, so a data value never matches.  More permissive than
  /// varIsFrameDerived (which omits the width ops); used to recognise a dynamic
  /// stack allocation, where SP threads through those ops before the subtract.
  bool varIsStackPtrDerived(const MedVar &V, int Depth = 0) const;

  /// True when \p V reaches \p Target through identity / width ops — the
  /// backward dual of varIsStackPtrDerived, proving a dynamic adjustment's
  /// result is what the function installs as its new stack pointer.
  bool varReaches(const MedVar &V, const MedVar &Target, int Depth = 0) const;

  /// Recognise `sp = sp - runtime` (a variable-length array / `alloca`) and
  /// emit a real dynamic LLVM alloca for it, returning the allocation base as
  /// an integer of the op's width, or null for an ordinary subtraction.  The
  /// variable region becomes its own allocation so its accesses stay in bounds
  /// instead of reading below the fixed-size frame alloca, which has no
  /// headroom for a runtime-sized region.
  llvm::Value *tryEmitDynamicStackAlloc(const MedOp &Op,
                                        llvm::IRBuilder<> &Builder);

  /// Resolve an address computed as `stack_pointer - vla_size` (the variable
  /// region's base) back to the dynamic alloca created for that size.  clang
  /// also addresses the region's first element from the *old* stack pointer
  /// (`old_sp + (-size)`, an INT_ADD of the negated size) instead of the new
  /// SP register; without this it would resolve to the fixed frame and miss the
  /// dynamic allocation.  Returns the cached allocation base, or null.
  llvm::Value *tryResolveDynVlaAddr(const MedOp &Op,
                                    llvm::IRBuilder<> &Builder);

  /// True when \p Op is a call to a stack-probe runtime routine — Darwin's
  /// `____chkstk_darwin`, invoked GOT-indirect (`ldr xN,[got]; blr xN`) with
  /// its allocation size passed in a fixed register (x9) the lifter does not
  /// model. Such a probe is pure: it only touches guard pages, and the dynamic
  /// stack allocation it guards is independently lowered to a real `alloca`
  /// (tryEmitDynamicStackAlloc).  Re-emitting the call with the recovered
  /// (wrong) argument registers makes the probe walk off the stack, so the
  /// emitter elides it. Resolves the GOT slot the call target is loaded from
  /// via Img.ImportPtrSlots or Img.DyldBindSlots.
  bool isStackProbeCall(const MedOp &Op) const;

  /// Canonical identity of a runtime size value, looking through the width and
  /// copy casts a size threads through, so the size used to allocate a VLA and
  /// the size negated to address it map to the same key.
  std::pair<int, int> sizeRootKey(const MedVar &V, int Depth = 0) const;

  /// Resolve an address operand to a `(base var, byte displacement)` key by
  /// threading the constant INT_ADD/INT_SUB offsets and width/copy casts down
  /// to the anchoring register.  Lets a reloaded value's load slot be matched
  /// against the store that filled it.  Returns the key, or null.
  /// \p ThroughRegs threads register definitions too (addrSlotKey normally
  /// anchors at the first register); used to recognise a slot address copied
  /// into a parameter register before a call (escape detection).
  std::optional<std::pair<std::pair<int, int>, int64_t>>
  addrSlotKey(const MedVar &V, int Depth = 0, bool ThroughRegs = false) const;

  /// True when \p V is reloaded from a stack slot that a stack-pointer-derived
  /// value was spilled to (`mov [slot],sp ; ... ; mov reg,[slot]`).  clang
  /// spills the pre-`alloca` SP to the frame and rederives the VLA base from
  /// the reload, so the reload must still count as stack-pointer-derived for
  /// the dynamic-VLA resolution to map it back to the allocation rather than
  /// raw frame math.
  bool varIsReloadedStackPtr(const MedVar &V, int Depth = 0) const;

  /// True when \p VA is the address of a real data symbol in a non-writable
  /// segment.  Object-file rodata table references go through a relocation to
  /// such a symbol, whereas frame-analysis-synthesized stack-array addresses
  /// are never real symbols — this distinguishes a genuine lookup table from a
  /// spilled read-write array whose absolute base happens to land in .rodata.
  bool isReadOnlyDataSymbol(uint64_t VA);

  // Cache of constant bases used as STORE targets in the current function (a
  // base written here is a read-write array, not a read-only lookup table).
  const MedFunc *StoredBasesFor = nullptr;
  std::set<uint64_t> StoredConstBases;

  // Cache of read-only data symbol addresses (see isReadOnlyDataSymbol).
  // Keyed on the current function (rebuilt when it changes) rather than the
  // image pointer: a session reuses one emitter and may reload binaries into
  // the same BinaryImage storage, so an image-pointer key would go stale.
  const MedFunc *RodataSymbolsFor = nullptr;
  std::set<uint64_t> RodataSymbolVAs;

  // Cache of rodata C-string base VAs walked by an induction pointer (see
  // isInductionRodataStringBase); keyed on the current function.
  mutable const MedFunc *InductionBasesFor = nullptr;
  mutable std::set<uint64_t> InductionBaseVAs;

  using AddressProvenanceVarKey =
      std::tuple<int, int, int, int, int, uint16_t, uint64_t>;
  struct IndexedGlobalBaseProof {
    bool Proven = false;
    uint64_t Base = 0;
    bool HaveBase = false;
    std::vector<MedVar> IdxTerms;
    bool SawAmbiguousPhi = false;
    const PhiNode *AmbiguousPhi = nullptr;
  };

  /// Exact value identity used by the provenance caches. Constants include
  /// their payload and width; register/parameter/stack values include their
  /// physical payload in addition to SSA identity.
  static AddressProvenanceVarKey addressProvenanceVarKey(const MedVar &V) {
    uint64_t Payload = 0;
    switch (V.Kind) {
    case MedVar::Const:
      Payload = V.ConstVal;
      break;
    case MedVar::Reg:
    case MedVar::Param:
      Payload = V.RegOff;
      break;
    case MedVar::Stack:
      Payload = static_cast<uint64_t>(V.StackOff);
      break;
    default:
      break;
    }
    return {static_cast<int>(V.Kind),
            static_cast<int>(V.TheArch),
            V.RenameTag,
            V.Id,
            V.SSAVer,
            V.Size,
            Payload};
  }

  // Completed clean top-level indexed-base proofs for the current function.
  // The cached record contains only MedIR facts, never llvm::Value objects.
  mutable const MedFunc *IndexedGlobalBaseCacheFor = nullptr;
  mutable std::map<AddressProvenanceVarKey, IndexedGlobalBaseProof>
      IndexedGlobalBaseCache;

  mutable const MedFunc *PhiRecurrenceCacheFor = nullptr;
  mutable std::map<std::tuple<const PhiNode *, int, AddressProvenanceVarKey>,
                   bool>
      PhiRecurrenceCache;
  mutable const MedFunc *SelfRecurrenceCacheFor = nullptr;
  mutable std::map<const PhiNode *, bool> SelfRecurrenceCache;

  mutable const MedFunc *StableOffsetCacheFor = nullptr;
  mutable std::map<
      std::tuple<AddressProvenanceVarKey, bool, AddressProvenanceVarKey>, bool>
      StableOffsetCache;

  // Cache of writable-segment base VAs that contain at least one already-
  // symbolized writable-reloc pointer constant (see
  // hasSymbolizedWritableSibling); keyed on the current function.  Mutable so
  // the const symbolization helpers can fill it lazily.
  mutable const MedFunc *SymbolizedWritableSegsFor = nullptr;
  mutable std::set<uint64_t> SymbolizedWritableSegs;

  // Per-function (Kind,Id,SSAVer)->definition indexes, rebuilt lazily when
  // CurMedFunc changes (see ensureDefPhiIndex / lookupDef / lookupPhi).  The
  // key is {(Id<<32)|SSAVer, Kind}: lossless for the 32-bit Id/SSAVer, and the
  // small Kind enum in .second never collides with DenseMap's int
  // empty/tombstone sentinels.  Replaces the O(ops) linear def scans the
  // const-pointer classifier previously ran inside its per-access backward
  // walks.
  mutable const MedFunc *DefPhiIndexFor = nullptr;
  mutable llvm::DenseMap<std::pair<int64_t, int>, const MedOp *> DefIndex;
  mutable llvm::DenseMap<std::pair<int64_t, int>, const PhiNode *> PhiIndex;

  // Conservative feasible CFG edges after pruning only statically proven
  // constant conditional branches. Used to ignore a dead PHI input without
  // teaching every address resolver its own reachability walk.
  mutable const MedFunc *FeasibleEdgesFor = nullptr;
  mutable std::set<std::pair<int, int>> FeasibleEdges;
  mutable std::set<int> FeasibleBlocks;

  mutable const MedFunc *PhiEdgeIndexFor = nullptr;
  mutable std::map<const PhiNode *, int> PhiOwnerBlocks;
  mutable std::set<std::pair<int, int>> StructuralEdges;
  mutable std::map<std::pair<const PhiNode *, int>, PhiEdgeFeasibility>
      PhiEdgeClassCache;

  // Per-function memoized results of varIsFrameDerived, dropped when CurMedFunc
  // changes.  The "traces back to the stack pointer" property of an SSA value
  // is a pure function of the value graph (independent of the path/depth it is
  // reached at), so a top-level query for a given (Kind,Id,SSAVer) is cached
  // and reused across the many per-operand queries the op emitter makes — the
  // emit hot path (frameDerivedRec dominated MedLLVMEmitter::emit on large
  // functions).  Same {(Id<<32)|SSAVer, Kind} key scheme as DefIndex.
  mutable const MedFunc *FrameDerivedCacheFor = nullptr;
  mutable llvm::DenseMap<std::pair<int64_t, int>, bool> FrameDerivedCache;

  // Per-function memoized results of the stack-slot address predicates, dropped
  // when CurMedFunc changes (see ensureAddrPredCache).  Each answer depends
  // only on the resolved addrSlotKey and the (immutable during emit) function
  // body, so it is computed once per distinct slot and reused across the many
  // per-operand queries the address resolvers make — these whole-function slot
  // scans were a top serial cost of emit() after the frame-derived /
  // return-type walks were fixed.  Keyed by the addrSlotKey {(base Id,SSAVer),
  // byte offset}.
  mutable const MedFunc *AddrPredCacheFor = nullptr;
  mutable std::map<med_llvm::SlotKey, bool> SlotAddressEscapesCache;
  mutable std::map<med_llvm::SlotKey, bool> SlotMatchingKeyLoadCache;
  mutable std::map<med_llvm::SlotKey, bool> SlotReloadUsedLocallyCache;
  // writableDataSegOf result per (Kind,Id,SSAVer,RequireRelocBase).  The
  // writable-base search descends the whole address DAG (and chases stack
  // spills), so it is memoized per non-constant address value; keyed with the
  // RequireRelocBase flag since the value/address contexts differ.
  mutable std::map<std::tuple<int, int, int, bool>, uint64_t>
      WritableDataSegCache;
  // ptrTableUniqueSegment result per (Kind,Id,SSAVer) — a pure per-address DAG
  // walk over the (immutable during emit) function body.
  mutable std::map<std::pair<int64_t, int>, uint64_t> PtrTableUniqueSegCache;

  // Per-function memoized results of the two const classifiers, dropped when
  // CurMedFunc changes (see ensureConstClassCache).  std::unordered_map rather
  // than DenseMap so EVERY 64-bit ConstVal is a valid key — a
  // DenseMap<uint64_t> reserves ~0 and ~0-1 as its empty/tombstone sentinels,
  // which a genuine pointer constant can legitimately equal.
  mutable const MedFunc *ConstClassCacheFor = nullptr;
  mutable std::unordered_map<uint64_t, bool> ConstUsedAsPointerCache;
  mutable std::unordered_map<uint64_t, bool> ConstValueUsedAsIntegerCache;

  llvm::LLVMContext *Ctx = nullptr;
  llvm::Module *Mod = nullptr;
  const BinaryImage *Img = nullptr;
  Arch TargetArch = Arch::X64;
  BinaryFormat TargetFormat = BinaryFormat::Unknown;

  llvm::Function *CurFunc = nullptr;
  const MedFunc *CurMedFunc = nullptr;
  std::map<std::pair<int, int>, llvm::AllocaInst *> VarAllocs;

  /// Deferred stores for a shared -O0 computed-goto dispatch recovered as a
  /// switch (see med_llvm::PendingDispatchStore).
  std::vector<med_llvm::PendingDispatchStore> PendingDispatchStores;
  std::map<std::string, llvm::Value *> ParamArgs;
  std::map<uint64_t, llvm::Value *> ParamRegoffMap;
  /// Source address of every call this function emitted, which is what lets an
  /// Itanium call-site range be matched to the calls it protects.  Cleared per
  /// function alongside the other per-function emitter state.
  std::map<const llvm::CallInst *, va_t> CallSiteAddrs;
  /// LLVM symbol chosen for each lifted function body. Usually identical to
  /// MedFunc::Name; an address-backed native personality body uses its stable
  /// auto name so the canonical ABI name remains an external declaration.
  std::map<va_t, std::string> EmittedFuncNames;
  std::map<va_t, std::string> FuncNames;
  // Ordinary LLVM blocks are created for every body-emitted MedFunc before
  // any body operation runs.  This makes blockaddress resolution independent
  // of function emission order while leaving BodyMask-omitted functions as
  // declarations.
  std::map<std::pair<va_t, int>, llvm::BasicBlock *> PreparedFuncBlocks;
  std::map<va_t, llvm::BasicBlock *> LiftedCodeBlocks;
  // Sticky hard failure: a relocation-proven executable pointer must never
  // degrade to an embedded original VA.  emit() discards the module when set.
  bool FatalCodePointerResolution = false;
  // Sticky hard failure for any data-address contract violation: mixed or
  // incomplete provenance, an unmaterializable trusted pointer/relocation, or
  // a canonical run that cannot be represented. Mutable because some
  // address-decomposition helpers are logically const over MedIR while
  // recording an emission failure.
  mutable bool FatalDataPointerResolution = false;
  std::map<uint64_t, llvm::Constant *> GlobalDataCache;
  // One synthesized code-pointer mirror global per data segment base VA (see
  // buildCodePtrSegmentGlobal); reused across every access into that segment.
  std::map<uint64_t, llvm::Constant *> CodePtrTableGlobals;
  // External data declarations used while an imported pointer slot has proven
  // symbolic identity but no call has yet proven function type.  A later
  // direct call or native exception lowering promotes the placeholder to its
  // recovered llvm::Function and rewrites every pointer-table initializer use.
  std::map<std::string, llvm::GlobalVariable *> ImportedSymbolPlaceholders;
  // One embedded global per read-only segment (keyed by segment base VA): all
  // constants in the segment GEP into the SAME global instead of each embedding
  // its own [Addr, segment_end] copy (which duplicated the rodata O(N) times
  // and blew the recompiled .rodata up to tens of KB).
  std::map<uint64_t, llvm::GlobalVariable *> SegmentDataGlobals;
  // One mutable (non-const) embedded global per WRITABLE segment base VA (see
  // embedWritableRun): every direct and runtime-indexed access into the .data /
  // .bss segment GEPs into the SAME global so store/load pairs stay aliased.
  std::map<uint64_t, llvm::GlobalVariable *> WritableSegmentGlobals;
  std::map<uint64_t, uint16_t> DataSizeHints;
  // Addresses a size-0 (address-taken) access classified as a C-string.  A
  // later genuine multi-byte load of the same address is a table, not a string,
  // and must re-resolve as raw data instead of reusing the NUL-truncated
  // string.
  std::set<uint64_t> StringDataAddrs;
  int GlobalStrCounter = 0;

  llvm::AllocaInst *FrameAlloca = nullptr;
  llvm::Value *FrameBaseInt = nullptr;
  /// Shared landing-pad live-in slots.  MedIR models the Itanium ABI's
  /// exception object and selector as implicit values at exceptional roots;
  /// each emitted landingpad stores its pair here before the recovered handler
  /// body reads it.
  llvm::AllocaInst *EHExceptionAlloca = nullptr;
  llvm::AllocaInst *EHSelectorAlloca = nullptr;

  // Dynamic (VLA / alloca) allocation bases of the current function, keyed by
  // the canonical identity of their runtime size (sizeRootKey), so an address
  // re-derived as `stack_pointer - size` resolves to the same allocation.
  std::map<std::pair<int, int>, llvm::Value *> DynVlaBases;

  // Sub-register write propagation: when a wide register (e.g. RCX:8) is
  // written, also update narrower allocas at the same RegOff (e.g. ECX:4).
  // Maps RegOff → vector of (alloca_key, alloca_ptr, alloca_bit_width).
  std::map<uint64_t, std::vector<med_llvm::SubRegAllocInfo>> SubRegPropMap;
  void propagateToSubRegs(uint64_t RegOff, uint16_t WriteSize, llvm::Value *Val,
                          llvm::IRBuilder<> &Builder);

  llvm::Value *PendingIntrinsicOutputs[4] = {};
  unsigned PendingIntrinsicCount = 0;
};

} // namespace neverd

#endif // NEVERD_BACKEND_LLVM_MEDLLVMEMITTERCORE_H
