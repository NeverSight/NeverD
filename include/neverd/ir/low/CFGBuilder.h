//===- CFGBuilder.h - Control-flow graph construction -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Declares CFGBuilder for single-function CFG construction via recursive
/// descent disassembly.  See FuncDetector.h for entry-point discovery.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_LOW_CFGBUILDER_H
#define NEVERD_IR_LOW_CFGBUILDER_H

#include "neverd/decode/Decoder.h"
#include "neverd/ir/low/CircleRange.h"
#include "neverd/ir/low/FuncDetector.h"
#include "neverd/ir/low/LowIR.h"
#include "neverd/loader/BinaryImage.h"

#include "llvm/ADT/DenseSet.h"

#include <map>
#include <optional>
#include <set>
#include <vector>

namespace neverd {

class CFGBuilder {
public:
  /// Build CFG for a single function starting at EntryAddr.
  LowFunc build(const BinaryImage &Img, Decoder &Dec, va_t EntryAddr,
                const std::string &FuncName = "");

  /// Provide the set of known function entry addresses so that
  /// sanity checking can reject targets that overlap other functions.
  void setKnownFuncEntries(const std::set<va_t> *Entries) {
    KnownFuncEntries = Entries;
  }

private:
  struct InsnRecord {
    va_t Addr;
    uint16_t Size;
    std::vector<LowOp> Ops;
    bool IsBranch = false;
    bool IsCond = false;
    bool IsCall = false;
    bool IsRet = false;
    bool IsIndirect = false;
    /// A direct CALL to a known no-return libc function
    /// (longjmp/abort/exit/...). Such a call terminates control flow, so
    /// exploration stops after it and no fall-through successor edge is wired
    /// to the following block.
    bool IsNoReturnCall = false;
    va_t BranchTarget = InvalidVA;
    std::vector<va_t> JumpTableTargets;

    /// x87 stack-top (TOP) at this instruction's entry/exit in lift (worklist)
    /// order, and whether it absolute-resets TOP (FNINIT/FNCLEX). fixupFpuStack
    /// uses these to re-base ST(i) references into control-flow order.
    int FpuTopIn = 0;
    int FpuTopOut = 0;
    bool FpuReset = false;
  };

  void explore(const BinaryImage &Img, Decoder &Dec, va_t Addr);
  void splitBlocks();
  void linkSuccessors(LowFunc &Func, const std::map<va_t, int> &AddrToBlock);
  void linkExceptionalSuccessors(LowFunc &Func);
  void classifyInsn(InsnRecord &Rec);

  /// Re-base x87 ST(i) register references so each block's TOP matches its CFG
  /// predecessor's exit TOP rather than the worklist lift order.  The lifter
  /// names ST(i) as physical slot (TOP+i)&7 while advancing TOP in lift order;
  /// when a branch leaves a value on the stack and an arm net-changes the
  /// depth, the other arm is lifted with the wrong TOP.  A no-op (per-block
  /// offset 0) for straight-line / stack-balanced code, so only the mistracked
  /// cases move.
  void fixupFpuStack(LowFunc &Func);

  /// Whether \p Target is the entry of a *different* known function — i.e. an
  /// unconditional direct branch to it is a tail call, not intra-function flow.
  bool isTailCallTarget(va_t Target) const;

  /// Whether the direct CALL in \p Rec targets a known no-return libc function
  /// (longjmp/abort/exit/...).  The target is resolved through the image's
  /// imports (Mach-O __stubs / ELF PLT stub VA == the call target) and symbols
  /// (statically linked).  A true result makes explore() stop: the bytes after
  /// the call belong to the next function at -O2, not this one.
  bool isNoReturnCall(const InsnRecord &Rec) const;

  /// Rewrite an unconditional-branch instruction record into an explicit
  /// CALL + RETURN pair (tail call to another function).
  void rewriteAsTailCall(InsnRecord &Rec);

  /// Rewrite an unconditional indirect branch (`bx reg` / `br reg` / `jmp *reg`
  /// through a function pointer, not a jump table) into an INDIR_CALL + RETURN pair
  /// — an indirect tail call.  Without this the unresolved INDIR_BR lowers to
  /// a fall-through `ret 0`, dropping the call.
  void rewriteAsIndirectTailCall(InsnRecord &Rec);

  /// After all jump-table resolution, convert any remaining unconditional
  /// unresolved indirect branch into an indirect tail call and rebuild blocks.
  void convertIndirectTailCalls(LowFunc &Func);

  /// Make the block beginning at Func.Entry block 0 and remap every CFG edge
  /// through the same stable permutation.  Block byte ranges are still built
  /// in address order; only their public vector/ID order is normalized.
  void normalizeEntryBlock(LowFunc &Func);

  /// (Re)build Func.Blocks from the current Insns/BlockStarts, then link
  /// successors and extract jump tables.  Shared by the initial build, the
  /// multi-stage re-resolution, and the indirect-tail-call conversion.
  void rebuildBlocks(LowFunc &Func);
  void extractJumpTables(LowFunc &Func);
  std::vector<va_t> resolveJumpTable(const BinaryImage &Img,
                                     const InsnRecord &Rec);

  //===--------------------------------------------------------------------===//
  // JumpTableInfo — shared metadata extracted during resolution and reused
  // by extractJumpTables to populate LowFunc::JumpTables.
  //===--------------------------------------------------------------------===//

  struct JumpTableInfo {
    va_t BaseAddr = 0;
    uint16_t EntrySize = 0;
    uint32_t MaxEntries = 0;
    bool IsRelative = false;
    bool IsSigned = false;

    /// Set when the entries are absolute code pointers identified by a run of
    /// loader-applied relocations (computed goto / threaded dispatch).  The run
    /// length gives the exact MaxEntries, so the comparison-guard bound search
    /// (which such a table lacks) is skipped.
    bool RelocAbsolute = false;

    /// Set when MaxEntries was derived from a relocation run (absolute OR the
    /// PC-relative entries of a `switch(x % N)` table, whose modulus bounds the
    /// index with no `cmp`/range guard).  The run length is the exact count, so
    /// the guard search and normalization/stride adjustments are skipped — but
    /// unlike RelocAbsolute the entry decoding (relative/signed) is unchanged.
    bool RelocBounded = false;

    /// Compact-table form (AArch64 `ldrb`/`ldrh` + `adr anchor` + `add anchor,
    /// entry, lsl #k` + `br`): entries are read from BaseAddr but each target
    /// is `TargetBase + entry * EntryScale` rather than `BaseAddr + entry`.
    /// When TargetBase is 0 the relative base is BaseAddr and EntryScale is 1.
    va_t TargetBase = 0;
    uint32_t EntryScale = 1;

    /// Normalization parameters: table_index = (switch_var - NormBase) >>
    /// NormShift.
    int64_t NormBase = 0;
    uint32_t NormShift = 0;

    /// Stride of the switch variable deduced from AND masks or known-zero
    /// low bits.
    uint32_t Stride = 1;

    /// Set for a pre-scaled computed goto (the index register already holds the
    /// byte offset `entry * EntrySize`).  Propagated to JumpTable so the
    /// emitter dispatches on the resolver index register instead of the
    /// backward scan.
    bool PreScaledIndex = false;

    /// Runtime-selected table base: the dispatch loads from `(cond ? A :
    /// B)[idx]` with two adjacent code-pointer tables merged into one at
    /// BaseAddr; the emitter synthesizes a byte-offset selector.  See JumpTable
    /// for details.
    bool TwoTableSelect = false;

    /// Two-level (index-byte) table dispatch: a compact byte/halfword *index*
    /// table maps the switch variable to a small entry index, which then
    /// indexes the real address table — `target = jmptab[idxtab[switchvar]]`
    /// (the classic MSVC sparse-switch lowering).  The composed per-case
    /// targets are precomputed into ExplicitTargets (indexed by the switch
    /// variable, positions 0..N), and BaseAddr/EntrySize describe the *address*
    /// table (jmptab).  The emitter dispatches on IndexReg (the real switch
    /// variable that indexes idxtab), not on the intermediate table index.
    bool TwoLevelIndex = false;
    /// Byte distance between the two merged tables (entries(lo) * EntrySize).
    uint32_t TwoTableOffset = 0;
    /// Whether the higher table is selected by the positive (M / SELECT-true)
    /// arm.
    bool TwoTableHiPositive = false;

    /// Set when a stack-materialised computed-goto table is overwritten after
    /// its constant initializer copy with non-positional values (a runtime
    /// table permutation), so the static targets no longer match the runtime
    /// mapping. Propagated to JumpTable::MutatedUnsafe; the emitter traps
    /// rather than dispatching on a stale index->target map.  See JumpTable for
    /// details.
    bool MutatedUnsafe = false;

    /// Register holding the table index, when known from the load address.
    /// Used to confine guard-bound analysis to the switch variable so that
    /// unrelated masks (e.g. parity-flag `and x,1`) are not mistaken for the
    /// table bound.
    uint64_t IndexReg = InvalidVA;

    /// The actual table slot index of each kept target, in Targets order.  A
    /// bounded table may skip don't-care slots whose entry points outside the
    /// function (a peeled switch clang proved unreachable points its dead cases
    /// past the function), so the kept targets are *not* always at consecutive
    /// indices — recoverCaseLabels must use these real indices for case values
    /// rather than the compacted Targets position.
    std::vector<uint32_t> EntryIndices;

    /// The complete, ordered target set of a table whose entries do not lie in
    /// one contiguous run and therefore cannot be reconstructed by a single
    /// base+offset read (a runtime-selected `cond ? A : B` dispatch over two
    /// *non-adjacent* code-pointer tables: positions [0,N) are the lower
    /// table's targets, [N,2N) the higher table's).  When non-empty,
    /// resolveJumpTable uses it verbatim and skips the contiguous-read /
    /// guard / emulation machinery, all of which assume a single base.
    std::vector<va_t> ExplicitTargets;

    /// Precise modular-arithmetic value range for the switch variable.
    /// When non-empty, MaxEntries is derived from this range.
    CircleRange GuardRange;
  };

  bool sliceBackForTableBase(const InsnRecord &Rec, JumpTableInfo &Info);
  bool inferBoundsFromGuard(const InsnRecord &Rec, JumpTableInfo &Info);
  bool inferBoundsFromCFGGuards(const InsnRecord &Rec, JumpTableInfo &Info);
  void collectPredBlocks(va_t TargetBlockStart, const std::set<va_t> &Visited,
                         std::vector<va_t> &Out) const;
  bool tryRelativeTable(const BinaryImage &Img, const InsnRecord &Rec,
                        JumpTableInfo &Info);
  bool tryCrossInstrRelativeTable(const BinaryImage &Img, const InsnRecord &Rec,
                                  JumpTableInfo &Info);
  // Detect a constant-base absolute code-pointer table (`jmp *tab(,idx,W)`, the
  // non-PIC computed-goto / dense-switch shape) whose table load is decoupled
  // from the indirect branch by an -O0 spill/reload relay — the loaded target
  // is stored to a frame slot and the branch reloads it, so the table load and
  // the branch live in different instructions.  The table base is the constant
  // operand of the load-address `base + idx*W` sum; because it is a folded
  // constant (not a register), analyzeTableLoadAddr rejects it and
  // sliceBackForTableBase (which sees only the branch's own instruction) never
  // reaches it, so the dispatch would otherwise degrade to an indirect tail
  // call.  Recovery is gated on a run of absolute code-pointer relocations at
  // the base — the verifiable label-table signature no ordinary pointer load or
  // function-pointer tail call shares.  Only the table base, entry width, and
  // index register are recovered here; guard and normalization analysis is left
  // to the shared downstream flow, so a guarded switch keeps its (possibly
  // signed/offset) case labels while an unguarded computed goto is bounded by
  // the reloc run.  Handles both a single decoupled goto site (single-
  // predecessor relay) and a shared multi-site dispatch where several goto-site
  // predecessors read one common table.
  bool tryConstBaseAbsoluteTable(const BinaryImage &Img, const InsnRecord &Rec,
                                 JumpTableInfo &Info);
  /// When the table index register is a reload of a stack-spilled value
  /// (`str rX,[sp,#k]; ... ldr rIdx,[sp,#k]`, the -O0 shape where the guarded
  /// switch variable is spilled before the dispatch block), trace it back to
  /// the register stored to the same slot so guard/mask bound analysis keyed on
  /// the index register connects to the `cmp`/`and` on the original variable.
  /// Returns IndexReg unchanged when no unique frame-slot spill feeds it.
  uint64_t forwardIndexThroughStackSpill(const std::vector<LowOp> &BlockOps,
                                         int LoadIdx, uint64_t IndexReg,
                                         va_t BlkStart) const;
  /// A computed-goto whose label table is a *local* (non-`static`) array is
  /// materialised on the stack at -O0: clang copies the read-only initializer
  /// run (which carries the absolute code-pointer relocations) into a frame
  /// slot, then dispatches `ldr xT,[sp+slot, idx, scale]`.  The table-base
  /// register thus folds to a stack address (no segment), so the normal
  /// reloc-run resolution in tryCrossInstrRelativeTable cannot anchor on it.
  /// Given the resolved (frame) base register at the table load, trace the slot
  /// back to the STORE that initialised it from a constant `__const`/`.rodata`
  /// source and return that source VA — the real table base whose code-pointer
  /// reloc run bounds and decodes the targets.  Returns InvalidVA when the base
  /// is not a frame slot or no constant-source init store feeds it.
  /// \p MutatedOut (when non-null) is set true if the recovered stack table is
  /// overwritten after its constant initializer with non-positional values (a
  /// runtime permutation), making the static targets unsound for index
  /// dispatch.
  /// \p TableDisp is the constant load displacement that, added to the frame
  /// slot the base register resolves to, gives the table base offset.  It is 0
  /// on AArch64 (the offset is folded into the base register via `add
  /// xB,sp,#k`) and the load displacement on x86-64/i386 -O0 (`mov
  /// (%rbp,%idx,8),-0x30`, where analyzeTableLoadAddr reports base=rbp and
  /// Disp=-0x30).
  va_t resolveStackMaterializedTableSource(
      const BinaryImage &Img, const InsnRecord &Rec,
      const std::vector<LowOp> &Ops, int LoadIdx, uint64_t BaseReg,
      uint16_t LoadWidth, int64_t TableDisp, bool *MutatedOut = nullptr) const;
  bool tryAArch64CompactTable(const BinaryImage &Img, const InsnRecord &Rec,
                              JumpTableInfo &Info);
  bool analyzeTableLoadAddr(const std::vector<LowOp> &Ops, int FromIdx,
                            const NdVar &AddrV, uint64_t &BaseReg,
                            uint64_t &IndexReg, bool &HasScaledIndex,
                            uint64_t &Disp, va_t *AddrAddVA = nullptr) const;
  // Detect a size-optimized computed goto whose entry-size scale is folded into
  // the index (`shr idx,3; and idx,0x38; jmp *(table,idx)` — scale 1, the index
  // is already a byte offset).  Such a load carries no INT_MULT/INT_LEFT for
  // analyzeTableLoadAddr to anchor on, so it is matched here, gated on a run of
  // absolute code-pointer relocations at the folded base — the verifiable
  // signature of a label table that no plain pointer load or tail call shares.
  bool detectUnscaledRelocTableLoad(const BinaryImage &Img,
                                    const InsnRecord &Rec, uint64_t &BaseReg,
                                    uint64_t &IndexReg, uint16_t &LoadWidth,
                                    uint64_t &Disp, va_t &TableAddr) const;
  // Detect a runtime-selected table base (`base = cond ? A : B; jmp
  // *base[idx]`), where A and B are two adjacent code-pointer tables (clang
  // lowers a computed goto whose label table is chosen at runtime, e.g. `tbl =
  // c ? A : B`).  Folds both table addresses out of the
  // CMOV/CSEL/conditional-MOV select (a clean SELECT or an `(A&M)|(B&~M)` mask
  // blend), verifies their relocation runs are adjacent, and merges them into
  // one table at min(A,B).  The emitter rebuilds the runtime base select as a
  // single switch over the merged byte-offset index.
  bool tryTwoTableSelect(const BinaryImage &Img, const InsnRecord &Rec,
                         JumpTableInfo &Info);
  // Detect a two-level "index-byte" table (the classic MSVC sparse-switch
  // lowering): a compact byte/halfword index table maps the switch variable to
  // a small entry index, which then indexes the real address table
  // (`target = jmptab[idxtab[switchvar]]`).  Both loads are chained through the
  // dispatch, the address table (jmptab) carries a code-pointer relocation run,
  // and the index table (idxtab) holds byte/halfword entries all bounded by the
  // jmptab run length.  Composes the per-case target for each switch value into
  // Info.ExplicitTargets and dispatches on the real switch variable.
  bool tryTwoLevelIndexTable(const BinaryImage &Img, const InsnRecord &Rec,
                             JumpTableInfo &Info);
  // Decompose an index-table load address (`idxtab + switchvar[*scale]`) into
  // the folded constant table base, the index register, and the index scale
  // (1 for a byte index table, the entry width for a halfword one).  Tolerates
  // an unscaled index, which analyzeTableLoadAddr rejects.  Used by
  // tryTwoLevelIndexTable.
  bool decomposeIndexTableLoadAddr(const BinaryImage &Img,
                                   const InsnRecord &Rec,
                                   const std::vector<LowOp> &Ops, int LoadIdx,
                                   uint16_t EntryWidth, va_t &TableAddr,
                                   uint64_t &IndexReg, uint32_t &Scale) const;
  std::optional<uint64_t> foldRegConstant(const BinaryImage &Img,
                                          const InsnRecord &Rec, uint64_t Reg,
                                          va_t CutoffAddr = InvalidVA) const;
  bool tryARMTableBranch(const BinaryImage &Img, const InsnRecord &Rec,
                         JumpTableInfo &Info);
  bool tryDualPathRecovery(const InsnRecord &Rec, JumpTableInfo &Info);
  bool inferBoundsFromUnrolledGuard(const InsnRecord &Rec, JumpTableInfo &Info);
  /// Recover the entry count by propagating a range guard's implied value
  /// range backward, through the index's own normalization arithmetic, onto
  /// the table index register.  This is the last-resort guard strategy: it
  /// fires only when the direct comparison matchers found no bound, and it
  /// handles guards written against a shift/multiply/mask/offset-normalized
  /// form of the index (`t = idx>>k; cmp t,N`) that a value-preserving
  /// copy-chain match cannot reach.  Since the index register is by
  /// construction the value that scales the table address, the size of the
  /// range that lands on it is the exact number of entries.
  bool inferBoundsFromRangePullback(const InsnRecord &Rec, JumpTableInfo &Info);
  /// Recover the entry count from a range guard that constrains a *different*
  /// reload of the switch variable than the one feeding the table index.  At
  /// -O0 the switch variable is spilled and reloaded twice — once into the
  /// register the `cmp` guards (`ldr rG,[sp,#k]; cmp rG,N`) and again into the
  /// register that scales the table (`ldr rIdx,[sp,#k]; ldr t,[tab,rIdx,4]`).
  /// No value-preserving copy chain links rG to rIdx, so the register-identity
  /// matchers (findBestBound / refineRangeFromGuards / range-pullback) never see
  /// that the guard bounds the index.  But both registers are loaded from the
  /// same location, so — provided nothing writes that location between the two
  /// loads — they hold the same value and the guard bounds the index.  This is
  /// the last-resort guard strategy: it runs only after every register-keyed
  /// matcher failed, so it can only add a bound.  Soundness rests on exact value
  /// equivalence (identical load address + no intervening aliasing store) and a
  /// check that the comparison actually reaches a conditional branch.
  bool inferBoundsFromLoadAliasGuard(const InsnRecord &Rec,
                                     JumpTableInfo &Info);
  bool inferBoundsFromModulo(const BinaryImage &Img, const InsnRecord &Rec,
                             JumpTableInfo &Info);
  /// Recover the entry count from an AND mask that confines the table index.
  /// By default only a clean contiguous low-bit mask (`2^k - 1`) is accepted,
  /// since it exactly bounds the index to [0, 2^k).  With \p AllowNonContiguous
  /// set, an arbitrary mask is rounded up to its covering mask (all bits below
  /// the top set bit filled) — a sound upper bound on the raw masked value —
  /// which bounds a `switch(x & M)` table whose M is not `2^k - 1` (the table
  /// is then dense over the raw index with filler in the gaps).  The
  /// non-contiguous form is opt-in so it is used only as a last resort, where
  /// no other strategy bounded the table.
  uint32_t inferBoundsFromMask(const InsnRecord &Rec, const JumpTableInfo &Info,
                               bool AllowNonContiguous = false) const;
  void detectNormalization(const InsnRecord &Rec, JumpTableInfo &Info);
  void detectStride(const InsnRecord &Rec, JumpTableInfo &Info);
  uint32_t pullBackBound(uint32_t RawBound, const JumpTableInfo &Info) const;
  void recoverCaseLabels(JumpTable &JT, const JumpTableInfo &Info) const;
  std::vector<va_t>
  readTableEntries(const BinaryImage &Img, const JumpTableInfo &Info,
                   std::vector<uint32_t> *KeptIndices = nullptr);
  /// Emulate the dispatch arithmetic to recover targets, sidestepping static
  /// entry-layout classification.  When \p SelfBounding is true the scan is
  /// treated as unbounded (no comparison-guard / relocation-run bound): it
  /// stops on the first invalid target or a run of identical targets, and the
  /// result is accepted only if it holds at least kMinJumpTableEntries distinct
  /// targets — so an index-independent indirect tail call is not mismodeled as
  /// a switch.  With \p SelfBounding false (the default) it behaves as the
  /// original bounded fallback, iterating the known MaxEntries range.
  std::vector<va_t> tryEmulatedResolution(const BinaryImage &Img,
                                          const InsnRecord &Rec,
                                          const JumpTableInfo &Info,
                                          bool SelfBounding = false);
  /// Rebuild the target list by executing the *actual* dispatch arithmetic for
  /// each index in [0, Count), rather than reconstructing it from a classified
  /// entry layout (relative/absolute, signedness, scale, target-base).  The
  /// resolved index register is injected fresh immediately before its first use
  /// in the address computation — so the base materialisation that precedes it
  /// (a `lea`/`adr`/GOTOFF) is still folded naturally, while the injected value
  /// overrides whatever the code's own normalization produced — and the ops
  /// from that point through the INDIR_BR are emulated to yield the target.
  /// Because it reads the same table bytes and applies the same transform the
  /// CPU would, its result is ground truth for the recovered table.
  ///
  /// \p Grounded is set true only when every one of the \p Count indices
  /// executed a table LOAD at the resolved slot (BaseAddr + i*EntrySize) and
  /// produced a valid in-function target — i.e. the emulation provably read the
  /// same table the static decode did, so its (possibly different) targets can
  /// be trusted over a misclassified static decode.  When \p Grounded is false
  /// the returned targets must not be adopted.
  std::vector<va_t> emulateGroundedTargets(const BinaryImage &Img,
                                           const InsnRecord &Rec,
                                           const JumpTableInfo &Info,
                                           uint32_t Count, bool &Grounded);
  std::vector<LowOp> collectPathOps(va_t BranchBlockStart,
                                    va_t BranchInsnAddr) const;
  uint64_t findCommonSwitchVar(va_t BranchBlockStart,
                               uint64_t BranchIndReg) const;
  uint32_t traceCompoundGuard(const std::vector<LowOp> &Ops) const;
  CircleRange extractGuardRange(const std::vector<LowOp> &Ops, const LowOp &Op,
                                int VarSize) const;
  bool refineRangeFromGuards(const InsnRecord &Rec, JumpTableInfo &Info);
  bool guardUsesInclusiveCompare(const InsnRecord &Rec, uint64_t IndexReg,
                                 uint64_t Bound) const;
  bool isValidTarget(const BinaryImage &Img, va_t Target, va_t FuncEntry);
  bool sanityCheckTargets(const BinaryImage &Img,
                          std::vector<va_t> &Targets) const;

  /// Architecture-aware instruction alignment.  Returns the minimum
  /// alignment in bytes for the current target (e.g. 4 for AArch64).
  uint32_t getInsnAlignment() const;

  /// Multi-stage re-resolution: retry unresolved INDIR_BR after
  /// initial CFG construction provided more block coverage.
  void multiStageResolve(const BinaryImage &Img, Decoder &Dec, LowFunc &Func);

  /// Reconcile indirect branches that dispatch through the same rodata jump
  /// table (e.g. a clang-peeled first loop iteration and the loop body) so the
  /// copy in the messier block adopts the most complete recovery.  Returns true
  /// when any branch's target set changed.
  bool reconcileSharedTables(const BinaryImage &Img, Decoder &Dec);

  /// Cached JumpTableInfo per INDIR_BR address, filled during resolution
  /// and consumed by extractJumpTables to avoid duplicate analysis.
  std::map<va_t, JumpTableInfo> ResolvedTableInfo;

  std::map<va_t, InsnRecord> Insns;
  std::set<va_t> BlockStarts;
  // Per-instruction membership set probed once for every decoded instruction
  // and every branch target in explore(); a cache-friendly open-addressing set
  // (no per-node allocation, no red-black tree walk) is materially faster here
  // than std::set and needs no ordered access.  Real code VAs never hit the two
  // reserved DenseMap sentinels (~0 / ~0-1).
  llvm::DenseSet<va_t> ExploredAddrs;
  llvm::DenseSet<va_t> CallTargets;
  /// Executable addresses taken via a relocation-free PC-relative `lea` while
  /// exploring this function (same-section function pointers); copied into the
  /// LowFunc so the pipeline merges them into the image's CodeRefTargets (a
  /// sorted std::set), so this set's own iteration order does not affect output.
  llvm::DenseSet<va_t> DiscoveredCodeRefs;
  uint64_t DecodedInstructionCount = 0;
  uint64_t LiftedInstructionCount = 0;
  std::set<va_t> DecodeFailureAddresses;
  std::set<va_t> UnsupportedInstructionAddresses;
  std::set<va_t> TruncatedPathAddresses;
  va_t CurrentFuncEntry = 0;
  const BinaryImage *CurrentImg = nullptr;
  const std::set<va_t> *KnownFuncEntries = nullptr;
};

} // namespace neverd

#endif // NEVERD_IR_LOW_CFGBUILDER_H
