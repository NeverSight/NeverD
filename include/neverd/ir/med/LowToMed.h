//===- LowToMed.h - LowIR to MedIR conversion ---------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Declares LowToMedConverter which transforms a LowFunc into a MedFunc
/// by performing stack analysis, SSA construction, flag elimination,
/// dead code elimination, copy/constant propagation, and calling
/// convention detection.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_MED_LOWTOMED_H
#define NEVERD_IR_MED_LOWTOMED_H

#include "neverd/ir/low/LowIR.h"
#include "neverd/ir/med/MedIR.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <utility>
#include <vector>

namespace neverd {

class LowToMedConverter {
public:
  MedFunc convert(const LowFunc &Low, Arch TheArch,
                  BinaryFormat Fmt = BinaryFormat::ELF);

  /// Provide the per-callee callee-cleanup pop map (entry VA -> x86 `ret imm`
  /// bytes).  When set, a direct CALL to such a callee gets a post-call stack-
  /// pointer increment so the caller's later stack accesses use the corrected
  /// SP (the callee popped its hidden sret pointer).  Optional; null = no-op.
  void setCalleePopMap(const std::map<va_t, int> *M) { CalleePopMap = M; }

  /// Provide the set of GOT/pointer-slot VAs that hold a stack-probe import
  /// (`____chkstk_darwin`), derived by the loader from the binary's import
  /// pointer tables.  Apple clang guards a frame larger than one page with a
  /// GOT-indirect probe call in the prologue, BEFORE the incoming argument
  /// registers are spilled.  The probe preserves every register except x16/x17,
  /// but the lifter models the `blr` as an ordinary call returning in x0; when
  /// the frame size is a compile-time constant (so no argument is read before
  /// the probe, unlike a VLA) that spurious x0 definition kills the live-in
  /// argument register in SSA liveness and the function loses its parameters.
  /// When set, the converter clears such a probe call's modeled output before
  /// SSA construction so liveness is preserved (the emitter still elides the
  /// call by its target).  Mach-O only (the slot table is empty otherwise);
  /// optional, null = no-op.
  void setStackProbeSlots(const std::set<va_t> *S) { StackProbeSlots = S; }

  /// Provide the set of direct-call targets proven to return a 64-bit integer
  /// in the register pair (i386 EDX:EAX, ARM32 R1:R0).  When set, a direct CALL
  /// to such a callee is modeled as defining the pair BEFORE SSA construction,
  /// so buildSsa places the loop-carried high-half PHI that a post-SSA patch
  /// cannot (an i64 accumulator threaded `acc = f(acc, ...)` through the call).
  /// The set is only known after a first whole-program return-type inference
  /// pass, so the pipeline re-converts the affected callers with it set.
  /// Optional; null = no-op (the heuristic post-SSA modelCallWideIntReturn
  /// still runs).
  void setI64Callees(const std::set<va_t> *S) { I64Callees = S; }

  /// Provide the set of INDIRECT call-site addresses (INDIR_CALL) proven to return
  /// a register-pair i64 whose result is threaded as a loop accumulator.  An
  /// indirect call has no constant target to look up in setI64Callees, so the
  /// pipeline identifies the threaded site post-SSA (its low result is carried
  /// on a back-edge while an address-taken i64 callee is the plausible target)
  /// and re-converts the caller with the site addresses set, so the same
  /// pre-SSA register-pair modeling runs and buildSsa places the high-half loop
  /// PHI. Optional; null = no-op.
  void setI64IndirectSites(const std::set<va_t> *S) { I64IndirectSites = S; }

  struct StackSlot {
    int64_t Offset;
    uint16_t Size;
    int VarId;
  };

private:
  /// Most-recent write to a (RegOff, Size) register slot, tracked while the
  /// table-driven sub-register fixup walks a block.  Shared with the
  /// architecture-specific fixup helpers (LowToMedX86.cpp / LowToMedARM.cpp).
  struct RegWriteInfo {
    int Id = -1;
    uint16_t Size = 0;
    uint64_t RegOff = 0;
    size_t Ord = 0;
  };
  using RegWriteMap = std::map<std::pair<uint64_t, uint16_t>, RegWriteInfo>;

  void analyzeStack(const LowFunc &Low);
  MedVar ndVarToMedVar(const NdVar &VN);
  void fixupSubRegisters(MedFunc &Func);

  // --- Architecture-specific sub-register fixups (LLVM target-dispatch) ---
  // Each helper guards on TargetArch internally and is a no-op on other
  // targets.  The architecture-generic framework in fixupSubRegisters()
  // invokes them from LowToMedSubReg.cpp.

  /// x86/x86-64: merge a more-recent narrow partial write (AL/AH/AX/...) into a
  /// subsequent wider read of the parent register (LowToMedX86.cpp, Phase B2).
  void fixupPartialWritesX86(MedFunc &Func);
  /// x86/x86-64: when an 8/16-bit partial write's parent register is read in a
  /// *different* block (so Phase B2's in-block merge cannot reach it), define
  /// the wide parent right after the partial write so buildSsa carries the
  /// merged value across the block boundary / into successor phis
  /// (LowToMedX86.cpp, Phase B2x).
  void mergePartialWritesCrossBlockX86(MedFunc &Func);
  /// ARM/AArch64: reconstruct a full-width NEON Q read from its two more-recent
  /// 8-byte D halves within a block (LowToMedARM.cpp, Phase B3).
  void mergeWideVectorReadsARM(MedFunc &Func);
  /// ARM/AArch64: synthesize a Q = CONCAT(D_high, D_low) write after the last
  /// half write so cross-block wide reads observe the D values (Phase C2).
  void synthesizeWideVectorWritesARM(MedFunc &Func);
  /// ARM: redirect a `SUBBYTES(Q, off)` whose bytes fall entirely within a more
  /// recently written D/S sub-register to read that narrower register directly.
  /// Called from the generic Phase B loop; returns true when it rewrites the
  /// op.
  bool redirectWideSubpieceToNarrowARM(MedBlock &MB, size_t OI,
                                       const RegWriteMap &Writes);

  void mergeLoopCarriedPartialReads(MedFunc &Func);
  void mergeLoopCarriedVectorReads(MedFunc &Func);
  /// x86-64: a call returns scalar/vector floating point in XMM0, a
  /// caller-saved vector register the lifter did not model the call as defining
  /// (it modeled only the integer return register).  Where the result is
  /// consumed via the FP return register, redefine the call to write it at a
  /// fresh SSA version and rewire the consuming reads (straight-line and
  /// loop-carried PHI arguments). Runs before copy propagation so the read is
  /// not folded to the pre-call argument value.  No-op when the FP return
  /// register is not a vector register (ARM/AArch64 model the FP return in the
  /// integer return register).
  void modelCallFPReturn(MedFunc &Func);
  void modelCallX87Return(MedFunc &Func);
  /// 32-bit targets: model a direct CALL to a known i64-returning callee (in
  /// I64Callees) as producing the EDX:EAX / R1:R0 pair, BEFORE buildSsa, so SSA
  /// construction creates the loop-carried high-half PHI for a threaded i64
  /// accumulator.  No-op unless setI64Callees provided a non-empty set.
  void modelKnownWideCallReturns(MedFunc &Func);
  /// x86-64 / AArch64: a direct call returns a small struct by value across
  /// multiple registers (SysV eightbytes RAX/RDX/XMM0/XMM1, or AArch64 X0/X1 /
  /// HFA V0..V3) — the lifter modeled the call as defining only the integer
  /// return register, so the caller's reads of the other field registers
  /// resolve to stale pre-call values.  Where the caller consumes >=2 return
  /// registers straight-line / loop-carried (>=1 of them FP on x86-64),
  /// redefine the call to produce a flat wide-integer temp and SUBBYTES each
  /// field out into its return register (mirrors modelCallWideIntReturn,
  /// generalized to the cross-register-file aggregate).  The callee is re-typed
  /// to return the LLVM aggregate by the pipeline (which reads back this
  /// remodeling).  Runs before modelCallFPReturn so it claims the FP return
  /// register of a struct call.
  void modelCallStructReturn(MedFunc &Func);
  void eliminateFlags(MedFunc &Func);
  /// Clear the modeled output of a GOT-indirect `____chkstk_darwin` stack-probe
  /// call (one whose loaded slot VA is in StackProbeSlots) so SSA liveness does
  /// not treat the probe as defining/killing the live-in argument registers.
  /// Runs before buildSsa; no-op when StackProbeSlots is null/empty.  See
  /// setStackProbeSlots.
  void neutralizeStackProbeCalls(MedFunc &Func);
  void buildSsa(MedFunc &Func);
  void runDce(MedFunc &Func);
  void propagate(MedFunc &Func);
  void detectCc(MedFunc &Func, Arch TheArch, BinaryFormat Fmt);
  void simplifyCfg(MedFunc &Func);

  int allocVarId() { return NextVarId++; }
  int allocTempId() { return NextTempId++; }

  int NextVarId = 0;
  int NextTempId = 0;
  uint32_t NextCallSiteId = 1;
  Arch TargetArch = Arch::Unknown;

  std::vector<StackSlot> StackSlots;

  std::map<std::pair<uint64_t, uint16_t>, int> RegVarMap;
  std::map<uint64_t, int> TempVarMap;

  /// Per-callee callee-cleanup pop (entry VA -> bytes); see setCalleePopMap.
  const std::map<va_t, int> *CalleePopMap = nullptr;

  /// GOT/pointer-slot VAs holding a stack-probe import; see setStackProbeSlots.
  /// Null/empty until the pipeline (which has the loaded import tables)
  /// provides it.  Mach-O only.
  const std::set<va_t> *StackProbeSlots = nullptr;

  /// Direct-call targets proven to return i64 in the register pair; see
  /// setI64Callees.  Null until the pipeline re-converts an i64-accumulator
  /// caller with whole-program return-type knowledge.
  const std::set<va_t> *I64Callees = nullptr;

  /// Indirect (INDIR_CALL) call-site addresses to model as register-pair i64
  /// returns; see setI64IndirectSites.  Null unless the pipeline re-converts a
  /// caller whose function-pointer call threads an i64 accumulator.
  const std::set<va_t> *I64IndirectSites = nullptr;
};

/// Model a 32-bit target's 64-bit integer call return (i386 EDX:EAX, ARM32
/// R1:R0): rewrite a call whose result is consumed as i64 into one that
/// produces a 64-bit temp split into the low/high return registers, so the high
/// half is not dropped.  \p ForceI64Callees lists direct-call targets proven
/// (by callee return-type inference) to return i64; those are remodeled even
/// when the local straight-line/loop-carried heuristic cannot observe the high
/// half because it is consumed as the next call's argument (recovered only
/// later in the ABI pass).  Pass nullptr for the heuristic-only run during
/// low->med conversion.
void modelCallWideIntReturn(MedFunc &Func, Arch TheArch,
                            const std::set<va_t> *ForceI64Callees = nullptr);

/// Verify MedFunc structural invariants.  Returns true if OK in every build
/// mode so pipeline completeness reports cannot silently skip malformed IR.
bool verifyMedFunc(const MedFunc &Func, const char *PassName);

/// Run the same verifier only in debug builds.  Intermediate conversion passes
/// use this inexpensive release-mode wrapper; the pipeline invokes
/// verifyMedFunc() once on the final MedIR in every build mode.
inline bool debugVerifyMedFunc(const MedFunc &Func, const char *PassName) {
#ifdef NDEBUG
  (void)Func;
  (void)PassName;
  return true;
#else
  return verifyMedFunc(Func, PassName);
#endif
}

} // namespace neverd

#endif // NEVERD_IR_MED_LOWTOMED_H
