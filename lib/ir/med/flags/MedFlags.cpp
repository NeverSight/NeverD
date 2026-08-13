//===- MedFlags.cpp - CPU flags analysis for MedIR -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// MedIR CPU flags modeling and simplification.  Eliminates flag register
/// operations by pattern-matching NdOp flag chains (BOOL_AND/OR/NOT over
/// individual flag bits) back to high-level CondCodes, then replacing
/// them with direct comparison operations.
///
/// Architecture-generic framework lives here; compound flag patterns
/// (e.g. x86 EFLAGS combinations) are dispatched to per-target files
/// via resolveCompoundFlagPattern().
///
/// Three passes per basic block:
///   1. COND_BR -- simplify branch conditions
///   2. SETCC  -- simplify INT_ZEXT from flag expressions
///   3. SELECT -- simplify SELECT condition operands
///
//===----------------------------------------------------------------------===//

#include "MedFlagsDetail.h"

#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/med/LowToMed.h"

#include <algorithm>
#include <map>
#include <set>
#include <string>

namespace neverd {

//===----------------------------------------------------------------------===//
// Sub-pattern classification (shared with per-target compound resolvers)
//===----------------------------------------------------------------------===//

SubPatInfo classifySubPat(const std::vector<MedOp> &Ops, const MedVar &Var,
                          int SearchStart) {
  if (Var.Kind == MedVar::Flag)
    return {FlagSubPat::DirectFlag, Var.RegOff};

  for (int M = SearchStart; M >= 0; --M) {
    if (Ops[M].Output.Id != Var.Id || Ops[M].Output.SSAVer != Var.SSAVer)
      continue;
    const auto &Def = Ops[M];
    if (Def.Opcode == NdOp::BOOL_NOT && Def.Inputs[0].Kind == MedVar::Flag)
      return {FlagSubPat::InvertedFlag, Def.Inputs[0].RegOff};

    auto BothFlags = [](const MedOp &Op) {
      return Op.NumInputs >= 2 &&
             (Op.Inputs[0].Kind == MedVar::Flag ||
              Op.Inputs[0].Kind == MedVar::Const) &&
             (Op.Inputs[1].Kind == MedVar::Flag ||
              Op.Inputs[1].Kind == MedVar::Const);
    };
    if (Def.Opcode == NdOp::INT_EQUAL && BothFlags(Def))
      return {FlagSubPat::FlagsEqual, 0};
    if (Def.Opcode == NdOp::INT_NOTEQUAL && BothFlags(Def))
      return {FlagSubPat::FlagsNotEqual, 0};
    break;
  }
  return {};
}

bool areBothFlags(const MedOp &Op) {
  return Op.NumInputs >= 2 &&
         (Op.Inputs[0].Kind == MedVar::Flag ||
          Op.Inputs[0].Kind == MedVar::Const) &&
         (Op.Inputs[1].Kind == MedVar::Flag ||
          Op.Inputs[1].Kind == MedVar::Const);
}

namespace {

//===----------------------------------------------------------------------===//
// Architecture dispatch for compound flag patterns
//===----------------------------------------------------------------------===//

CondCode resolveCompoundFlagPattern(const std::vector<MedOp> &Ops,
                                    const MedOp &Def, int DefIdx,
                                    const TargetRegInfo &TRI) {
  switch (TRI.TheArch) {
  case Arch::X64:
  case Arch::X86:
    return resolveCompoundFlagPatternX86(Ops, Def, DefIdx, TRI);
  case Arch::AArch64:
  case Arch::ARM:
    return resolveCompoundFlagPatternARM(Ops, Def, DefIdx, TRI);
  default:
    return CondCode::Invalid;
  }
}

//===----------------------------------------------------------------------===//
// Architecture dispatch for carry/overflow flag-source matching
//===----------------------------------------------------------------------===//

// Whether a carry/overflow condition's flag may be folded against \p Cmp.
// x86 restricts this to a CMP that actually produced CF/OF (MedFlagsX86.cpp);
// other architectures impose no such restriction here, so folding is allowed.
bool carryFlagMatchesCmp(const std::vector<MedOp> &Ops, int ConsumerIdx,
                         CondCode CC, const CmpSource &Cmp,
                         const TargetRegInfo &TRI) {
  switch (TRI.TheArch) {
  case Arch::X64:
  case Arch::X86:
    return carryFlagMatchesCmpX86(Ops, ConsumerIdx, CC, Cmp, TRI);
  default:
    return true;
  }
}

//===----------------------------------------------------------------------===//
// resolveCondFromChain -- walk backward through flag ops to determine CondCode
//===----------------------------------------------------------------------===//

CondCode resolveCondFromChain(const std::vector<MedOp> &Ops,
                              const MedVar &CondVar, int SearchStart,
                              const TargetRegInfo &TRI) {
  for (int J = SearchStart; J >= 0; --J) {
    const auto &Def = Ops[J];
    if (Def.Output.Id != CondVar.Id || Def.Output.SSAVer != CondVar.SSAVer)
      continue;

    // Single-flag patterns (architecture-generic via TRI)
    if (Def.Opcode == NdOp::COPY && Def.Inputs[0].Kind == MedVar::Flag)
      return TRI.singleFlagCond(Def.Inputs[0].RegOff, false);

    if (Def.Opcode == NdOp::BOOL_NOT && Def.Inputs[0].Kind == MedVar::Flag)
      return TRI.singleFlagCond(Def.Inputs[0].RegOff, true);

    // Two-flag equality (generic: SF==OF on x86, N==V on ARM → SGE)
    if (Def.Opcode == NdOp::INT_EQUAL && areBothFlags(Def))
      return CondCode::SGE;
    if (Def.Opcode == NdOp::INT_NOTEQUAL && areBothFlags(Def))
      return CondCode::SLT;

    // Compound flag patterns (architecture-specific dispatch)
    if ((Def.Opcode == NdOp::BOOL_AND || Def.Opcode == NdOp::BOOL_OR) &&
        Def.NumInputs >= 2) {
      CondCode CC = resolveCompoundFlagPattern(Ops, Def, J, TRI);
      if (CC != CondCode::Invalid)
        return CC;
    }

    // BOOL_NOT of a compound expression → invert the inner result
    if (Def.Opcode == NdOp::BOOL_NOT && Def.Inputs[0].Kind != MedVar::Flag) {
      CondCode Inner = resolveCondFromChain(Ops, Def.Inputs[0], J - 1, TRI);
      if (Inner != CondCode::Invalid)
        return invertCond(Inner);
    }

    break;
  }
  return CondCode::Invalid;
}

//===----------------------------------------------------------------------===//
// buildCmpOp -- construct a comparison MedOp from CondCode + operands
//===----------------------------------------------------------------------===//

MedOp buildCmpOp(CondCode CC, const MedVar &A, const MedVar &B,
                 const MedVar &OutputVar, va_t Addr) {
  MedOp Op;
  Op.Addr = Addr;
  Op.Opcode = condToOpcode(CC);
  if (condSwapsOperands(CC)) {
    Op.addInput(B);
    Op.addInput(A);
  } else {
    Op.addInput(A);
    Op.addInput(B);
  }
  Op.Output = OutputVar;
  Op.Output.Kind = MedVar::Temp;
  Op.Output.Size = 1;
  return Op;
}

//===----------------------------------------------------------------------===//
// markFlagChainDead -- mark flag-producing ops and condition chain as dead
//===----------------------------------------------------------------------===//

void markFlagChainDead(std::vector<MedOp> &Ops, int Start,
                       const MedVar &CondVar, int CurBlock,
                       const std::map<std::string, std::set<int>> &UseBlocks) {
  // A single flag-producing instruction may feed more than one consumer.
  //  * Same block: on ARM a `cmp` sets the carry read by *both* a predicated
  //    `cmphs` (a COND_BR we rewrite) *and* a predicated `mov rd,rm,lsl #n`
  //    (a SELECT whose carry Pass 3 cannot fold).
  //  * Across blocks: ARM keeps the NZCV flags live past the conditional
  //    branch that splits a `cmp` from a following predicated instruction
  //    (`cmp; beq L; subge ...` — the `subge` GE test reads N/V in the
  //    fall-through block).
  // Eliminating the flag for one consumer must not delete it while another
  // still reads it, or the surviving predicate silently reads a stale
  // loop-carried/live-in flag.  Keep any flag with a surviving use.
  auto hasSurvivingUse = [&](const MedVar &Out, int DefIdx) -> bool {
    auto It = UseBlocks.find(Out.display());
    if (It != UseBlocks.end())
      for (int B : It->second)
        if (B != CurBlock)
          return true; // consumed by another basic block
    for (int U = 0; U < static_cast<int>(Ops.size()); ++U) {
      if (U == DefIdx)
        continue;
      const auto &Op = Ops[U];
      if (Op.Dead)
        continue;
      for (uint8_t K = 0; K < Op.NumInputs; ++K) {
        const auto &In = Op.Inputs[K];
        if (In.Kind == Out.Kind && In.Id == Out.Id && In.SSAVer == Out.SSAVer &&
            In.RegOff == Out.RegOff)
          return true;
      }
    }
    return false;
  };
  for (int J = Start; J >= 0; --J) {
    auto &Prev = Ops[J];
    if (Prev.Output.Kind == MedVar::Flag) {
      if (!hasSurvivingUse(Prev.Output, J))
        Prev.Dead = true;
      continue;
    }
    if (Prev.Output.Id == CondVar.Id && Prev.Output.SSAVer == CondVar.SSAVer) {
      Prev.Dead = true;
      continue;
    }
    bool AllFlagInputs = Prev.NumInputs > 0;
    for (uint8_t K = 0; K < Prev.NumInputs; ++K) {
      if (Prev.Inputs[K].Kind != MedVar::Flag &&
          Prev.Inputs[K].Kind != MedVar::Temp) {
        AllFlagInputs = false;
        break;
      }
    }
    if (AllFlagInputs &&
        (Prev.Opcode == NdOp::BOOL_NOT || Prev.Opcode == NdOp::BOOL_AND ||
         Prev.Opcode == NdOp::BOOL_OR || Prev.Opcode == NdOp::INT_EQUAL ||
         Prev.Opcode == NdOp::INT_NOTEQUAL) &&
        !hasSurvivingUse(Prev.Output, J))
      // A compound boolean (e.g. the BOOL_NOT that CCMP/CCMN uses to derive its
      // conditional C flag, CmpC = !borrow) can still feed a live consumer such
      // as the ccmp's own `SELECT C,cond,CmpC,#nzcv`.  Only the flag-output
      // branch above guarded against surviving uses; mirroring that guard here
      // stops the cond-chain cleanup from killing a temp the result SELECTs
      // still read (genuine dead chain ops are killed consumer-first, so their
      // producers correctly see no surviving use on later iterations).
      Prev.Dead = true;
    if (Prev.Opcode == NdOp::INT_SUB || Prev.Opcode == NdOp::INT_AND)
      break;
  }
}

//===----------------------------------------------------------------------===//
// findCmpSource -- locate the CMP/TEST operands from SUB/AND before flags
//===----------------------------------------------------------------------===//

/// Conditions whose fold to a comparison of the flag source's operands is only
/// sound after a genuine subtraction (a real CMP/SUB):
///   * Carry/overflow conditions (ULT/ULE/UGT/UGE depend on CF, VS on OF).
///   * Signed relations (SLT/SGE read SF^OF; SLE/SGT read ZF|(SF^OF)) depend
///     on OF as well.
/// After a real CMP these map to `A <cmp> B` of the SUB operands.  After an ADD
/// (CF=INT_CARRY, OF=INT_SOVF), a register-writing SUB whose result is
/// re-read through a SUBBYTES (findCmpSource lands on its else branch with
/// B=0), or a logical op (CF=OF=0), the fold collapses to `result <cmp> 0`
/// which silently drops the carry/overflow bit and miscompiles on overflow — so
/// MedFlags must leave them unfolded for the emitter to lower the genuine flag
/// chain.  EQ/NE read ZF only (result==0) and remain foldable after any flag
/// source.
bool condNeedsGenuineSub(CondCode CC) {
  return CC == CondCode::ULT || CC == CondCode::ULE || CC == CondCode::UGT ||
         CC == CondCode::UGE || CC == CondCode::VS || CC == CondCode::SLT ||
         CC == CondCode::SGE || CC == CondCode::SLE || CC == CondCode::SGT;
}

/// True when \p CondVar resolves to a LONE sign flag — the single-flag pattern
/// COPY(NF) / BOOL_NOT(NF) that x86 `js`/`jns`/`sets`/`setns` and the ARM
/// N-flag conditions lift to.  resolveCondFromChain maps such a chain to
/// SLT/SGE, but a lone sign flag is the sign of the WRAPPED result `(A-B) <s
/// 0`, not the overflow-corrected signed comparison `A <s B` (= SF^OF, the
/// two-flag INT_NOTEQUAL(NF,VF) pattern).  They diverge exactly when A-B
/// overflows, so folding it to a comparison of the CMP operands drops the
/// overflow correction; keep it unfolded for the emitter to lower the genuine
/// sign-of-result chain.
bool condIsLoneSignFlag(const std::vector<MedOp> &Ops, const MedVar &CondVar,
                        int SearchStart, const TargetRegInfo &TRI) {
  if (CondVar.Kind == MedVar::Flag)
    return CondVar.RegOff == TRI.FlagNF;
  for (int J = SearchStart; J >= 0; --J) {
    const auto &Def = Ops[J];
    if (Def.Output.Id != CondVar.Id || Def.Output.SSAVer != CondVar.SSAVer)
      continue;
    if ((Def.Opcode == NdOp::COPY || Def.Opcode == NdOp::BOOL_NOT) &&
        Def.NumInputs >= 1 && Def.Inputs[0].Kind == MedVar::Flag)
      return Def.Inputs[0].RegOff == TRI.FlagNF;
    if (Def.Opcode == NdOp::BOOL_NOT && Def.NumInputs >= 1 &&
        Def.Inputs[0].Kind != MedVar::Flag)
      return condIsLoneSignFlag(Ops, Def.Inputs[0], J - 1, TRI);
    break;
  }
  return false;
}

CmpSource findCmpSource(const std::vector<MedOp> &Ops, int SearchEnd,
                        const TargetRegInfo &TRI) {
  CmpSource Result;
  for (int I = SearchEnd; I >= 0; --I) {
    const auto &Op = Ops[I];
    if (Op.Output.Kind == MedVar::Flag &&
        (Op.Opcode == NdOp::INT_EQUAL || Op.Opcode == NdOp::INT_SLESS) &&
        Op.NumInputs >= 2) {
      if (TRI.FlagPF != 0 && Op.Output.RegOff == TRI.FlagPF)
        continue;

      const auto &ResultVar = Op.Inputs[0];
      for (int J = I - 1; J >= 0; --J) {
        const auto &Prev = Ops[J];
        if (Prev.Output.Id != ResultVar.Id ||
            Prev.Output.SSAVer != ResultVar.SSAVer || Prev.NumInputs < 1)
          continue;
        if (Prev.Opcode == NdOp::INT_SUB) {
          Result.A = Prev.Inputs[0];
          Result.B = Prev.Inputs[1];
          Result.FromSub = true;
        } else if (Prev.Opcode == NdOp::INT_AND &&
                   Prev.Inputs[0] == Prev.Inputs[1]) {
          Result.A = Prev.Inputs[0];
          Result.B = MedVar::makeConst(0, Prev.Inputs[0].Size);
        } else if (Prev.Opcode == NdOp::INT_AND) {
          Result.A = ResultVar;
          Result.B = MedVar::makeConst(0, ResultVar.Size);
        } else {
          Result.A = Prev.Output;
          Result.B = MedVar::makeConst(0, Prev.Output.Size);
        }
        Result.Valid = true;
        break;
      }
      if (Result.Valid)
        break;
    }
  }
  if (Result.Valid && Result.A == Result.B)
    Result.B = MedVar::makeConst(0, Result.A.Size);
  return Result;
}

/// The flag bits a high-level condition tests (architecture-generic offsets via
/// TRI).  Shared by the conditional-flag and FP-flag guards below.
std::vector<uint64_t> flagsForCond(CondCode CC, const TargetRegInfo &TRI) {
  switch (CC) {
  case CondCode::EQ:
  case CondCode::NE:
    return {TRI.FlagZF};
  case CondCode::SLT:
  case CondCode::SGE:
    return {TRI.FlagNF, TRI.FlagVF};
  case CondCode::SLE:
  case CondCode::SGT:
    return {TRI.FlagZF, TRI.FlagNF, TRI.FlagVF};
  case CondCode::ULT:
  case CondCode::UGE:
    return {TRI.FlagCF};
  case CondCode::ULE:
  case CondCode::UGT:
    return {TRI.FlagCF, TRI.FlagZF};
  case CondCode::VS:
    return {TRI.FlagVF};
  default:
    return {};
  }
}

/// Whether the flag value defined at \p DefIdx originates from an FP compare.
/// UCOMISS/COMISD/FUCOMI write ZF/CF/PF as BOOL_OR/BOOL_NOT/COPY chains over
/// FLOAT_EQUAL/FLOAT_LESS/FLOAT_ISNAN temps; trace those temp producers back so a
/// flag set by an FP compare is recognised even when later partial-register
/// merges (a preceding SETcc's `reg & 0xFF..00`) sit between it and the
/// consumer.
bool flagDefTracesToFP(const std::vector<MedOp> &Ops, int DefIdx) {
  if (isFloatCompareOpcode(Ops[DefIdx].Opcode))
    return true;
  std::vector<MedVar> Work;
  auto pushTemps = [&](const MedOp &Op) {
    for (uint8_t K = 0; K < Op.NumInputs; ++K)
      if (Op.Inputs[K].Kind == MedVar::Temp)
        Work.push_back(Op.Inputs[K]);
  };
  pushTemps(Ops[DefIdx]);
  for (int Budget = 256; !Work.empty() && Budget > 0; --Budget) {
    MedVar V = Work.back();
    Work.pop_back();
    for (int J = DefIdx - 1; J >= 0; --J) {
      const auto &P = Ops[J];
      if (P.Output.Kind != MedVar::Temp || P.Output.Id != V.Id ||
          P.Output.SSAVer != V.SSAVer)
        continue;
      if (isFloatCompareOpcode(P.Opcode))
        return true;
      if (P.Opcode == NdOp::BOOL_OR || P.Opcode == NdOp::BOOL_AND ||
          P.Opcode == NdOp::BOOL_NOT || P.Opcode == NdOp::COPY)
        pushTemps(P);
      break;
    }
  }
  return false;
}

/// Whether the nearest definition of any flag read by \p CC is FP-derived.
/// findCmpSource only reconstructs integer CMP/TEST operands, so folding a
/// condition whose flag came from an FP compare (UCOMISS etc.) would substitute
/// the wrong source — keep it unfolded for the emitter to lower the FP flag
/// chain.  Walking to the actual flag definition (not a positional opcode scan)
/// is robust against an intervening SETcc partial-register-merge INT_AND/INT_OR
/// that would otherwise mask the FP compare.
bool condReadsFPFlag(const std::vector<MedOp> &Ops, int ConsumerIdx,
                     CondCode CC, const TargetRegInfo &TRI) {
  for (uint64_t Off : flagsForCond(CC, TRI))
    for (int J = ConsumerIdx; J >= 0; --J) {
      const auto &D = Ops[J];
      if (D.Output.Kind != MedVar::Flag || D.Output.RegOff != Off)
        continue;
      if (flagDefTracesToFP(Ops, J))
        return true;
      break; // only the nearest definition of this flag matters
    }
  return false;
}

/// A conditional compare (CCMP/CCMN/FCCMP) redefines NZCV through SELECT ops
/// *after* the CMP that produced its guard condition.  When the flag a
/// condition reads is most-recently defined by such a SELECT, findCmpSource's
/// CMP is stale and folding the condition to a comparison of that CMP's
/// operands is wrong (AArch64 `cmp; ccmp; csinc` — the csinc reads the ccmp's
/// conditional Z, not the cmp's).  Detect a SELECT-defined flag and leave the
/// condition unfolded so the emitter lowers the genuine NZCV chain.  Harmless
/// on x86 (no flag is ever SELECT-defined there).
bool condReadsConditionalFlag(const std::vector<MedOp> &Ops, int ConsumerIdx,
                              CondCode CC, const TargetRegInfo &TRI) {
  std::vector<uint64_t> Flags = flagsForCond(CC, TRI);
  if (Flags.empty())
    return false;
  for (uint64_t Off : Flags)
    for (int J = ConsumerIdx; J >= 0; --J) {
      const auto &D = Ops[J];
      if (D.Output.Kind != MedVar::Flag || D.Output.RegOff != Off)
        continue;
      if (D.Opcode == NdOp::SELECT)
        return true; // conditionally-set flag (ccmp); CMP is stale
      // A flag *transform* — an op that reads a flag register to produce this
      // flag (AArch64 XAFLAG/AXFLAG/CFINV, which rewrite NZCV from the existing
      // flags) — leaves the prior CMP stale.  Folding the condition back to
      // that CMP's operands would skip the transform, so leave it unfolded and
      // let the emitter lower the genuine flag chain.  Fresh comparison setters
      // (INT_EQUAL/INT_SLESS/INT_CARRY/... and ADC/SBC's BOOL_OR over carry
      // temps) read GP values, not flags, so they are unaffected.
      for (uint8_t K = 0; K < D.NumInputs; ++K)
        if (D.Inputs[K].Kind == MedVar::Flag)
          return true;
      break; // nearest def is a fresh comparison flag setter
    }
  return false;
}

} // anonymous namespace

//===----------------------------------------------------------------------===//
// eliminateFlags -- main entry point
//===----------------------------------------------------------------------===//

void LowToMedConverter::eliminateFlags(MedFunc &Func) {
  const auto &TRI = getTargetRegInfo(TargetArch);

  // Map every value version to the set of basic blocks that read it.  ARM
  // predication keeps CPU flags live across the branch that separates a `cmp`
  // from the predicated instruction it guards, so a flag must not be killed
  // locally while a successor block still consumes it.  Computed once up front
  // (conservative: never removes a use, so it can only keep extra defs alive).
  std::map<std::string, std::set<int>> UseBlocks;
  for (int B = 0; B < static_cast<int>(Func.Blocks.size()); ++B)
    for (auto &Op : Func.Blocks[B].Ops)
      for (uint8_t K = 0; K < Op.NumInputs; ++K)
        UseBlocks[Op.Inputs[K].display()].insert(B);

  for (int BlkIdx = 0; BlkIdx < static_cast<int>(Func.Blocks.size());
       ++BlkIdx) {
    auto &Blk = Func.Blocks[BlkIdx];
    // --- Pass 1: COND_BR condition simplification ---
    int CbranchIdx = -1;
    for (int I = static_cast<int>(Blk.Ops.size()) - 1; I >= 0; --I) {
      if (Blk.Ops[I].Opcode == NdOp::COND_BR) {
        CbranchIdx = I;
        break;
      }
    }

    CmpSource Cmp;
    if (CbranchIdx >= 0)
      Cmp = findCmpSource(Blk.Ops, CbranchIdx - 1, TRI);

    if (Cmp.Valid) {
      for (size_t I = 0; I < Blk.Ops.size(); ++I) {
        auto &Op = Blk.Ops[I];
        if (Op.Opcode != NdOp::COND_BR || Op.NumInputs < 2)
          continue;

        auto &CondVar = Op.Inputs[1];
        if (CondVar.isConst())
          continue;

        CondCode CC = resolveCondFromChain(Blk.Ops, CondVar,
                                           static_cast<int>(I) - 1, TRI);
        if (CC == CondCode::Invalid || Cmp.A.isConst())
          continue;
        if (condToOpcode(CC) == NdOp::NOP)
          continue;
        // An FP compare between this COND_BR and the integer CMP means the
        // branch reads FP flags (FUCOMI/UCOMISD set them via BOOL_OR over
        // FLOAT_EQUAL/FLOAT_LESS temps), which findCmpSource cannot represent;
        // folding to the earlier integer CMP would use the wrong operands.
        bool CbHasFP = false;
        for (int J = static_cast<int>(I) - 1; J >= 0 && !CbHasFP; --J) {
          auto &P = Blk.Ops[J];
          if (isFloatCompareOpcode(P.Opcode))
            CbHasFP = true;
          if (P.Opcode == NdOp::INT_SUB || P.Opcode == NdOp::INT_AND ||
              P.Opcode == NdOp::INT_ADD)
            break;
        }
        if (CbHasFP)
          continue;
        // Carry/overflow and signed-overflow conditions are only valid from a
        // real CMP (INT_SUB); an ADD's carry/overflow can't be reconstructed as
        // a comparison (the bogus `0<=result` collapse broke `addw;jae`
        // unsigned-saturation idioms, and dropped OF on `add;jl`/`cmn;blt`).
        if (condNeedsGenuineSub(CC) && !Cmp.FromSub)
          continue;
        if (condIsLoneSignFlag(Blk.Ops, CondVar, static_cast<int>(I) - 1, TRI))
          continue;
        if (!carryFlagMatchesCmp(Blk.Ops, static_cast<int>(I) - 1, CC, Cmp,
                                 TRI))
          continue;
        if (condReadsConditionalFlag(Blk.Ops, static_cast<int>(I) - 1, CC, TRI))
          continue;
        // Flag-definition walk (robust against a partial-register-merge INT_AND
        // hiding an FP compare from the positional CbHasFP scan above).
        if (condReadsFPFlag(Blk.Ops, static_cast<int>(I) - 1, CC, TRI))
          continue;

        auto CmpOp = buildCmpOp(CC, Cmp.A, Cmp.B, CondVar, Op.Addr);
        markFlagChainDead(Blk.Ops, static_cast<int>(I) - 1, CondVar, BlkIdx,
                          UseBlocks);

        Blk.Ops.insert(Blk.Ops.begin() + static_cast<long>(I), CmpOp);
        ++I;
      }
    }

    // --- Pass 2: SETCC (INT_ZEXT from flag expressions) ---
    // Each INT_ZEXT finds its own nearest CMP source by searching backward
    // from its position.  A single global CMP is wrong when multiple
    // CMP+CMOV pairs exist in the same basic block.
    for (size_t I = 0; I < Blk.Ops.size(); ++I) {
      auto &Op = Blk.Ops[I];
      if (Op.Opcode != NdOp::INT_ZEXT || Op.NumInputs < 1)
        continue;
      auto &SrcVar = Op.Inputs[0];
      if (SrcVar.Kind != MedVar::Temp || SrcVar.isConst())
        continue;

      // Skip if an FP compare sits between this SETcc and the nearest integer
      // CMP: findCmpSource only matches integer patterns, and an FP compare
      // (FUCOMI/UCOMISD) writes its flags through BOOL_OR over FLOAT_EQUAL/
      // FLOAT_LESS *temporaries*, so match the FP-compare opcode regardless of
      // whether its output is the flag or a temp (the flag is the BOOL_OR).
      bool HasFPFlag = false;
      for (int J = static_cast<int>(I) - 1; J >= 0 && !HasFPFlag; --J) {
        auto &P = Blk.Ops[J];
        if (isFloatCompareOpcode(P.Opcode))
          HasFPFlag = true;
        if (P.Opcode == NdOp::INT_SUB || P.Opcode == NdOp::INT_AND ||
            P.Opcode == NdOp::INT_ADD)
          break;
      }
      if (HasFPFlag)
        continue;

      CondCode CC =
          resolveCondFromChain(Blk.Ops, SrcVar, static_cast<int>(I) - 1, TRI);
      if (CC == CondCode::Invalid)
        continue;
      if (condToOpcode(CC) == NdOp::NOP)
        continue;

      CmpSource LocalCmp = findCmpSource(Blk.Ops, static_cast<int>(I) - 1, TRI);
      if (!LocalCmp.Valid)
        continue;
      // Carry/overflow and signed-overflow conditions need a genuine
      // subtraction; leaving the flag unfolded lets the emitter lower
      // INT_CARRY/INT_SOVF/INT_SBOR faithfully (fixes the
      // `addw;cmovae`/`setae` unsigned-saturation idiom — paddusw etc. — and
      // `add;setl`/`cmn;cset lt` signed-overflow folds that dropped OF).
      if (condNeedsGenuineSub(CC) && !LocalCmp.FromSub)
        continue;
      if (condIsLoneSignFlag(Blk.Ops, SrcVar, static_cast<int>(I) - 1, TRI))
        continue;
      if (!carryFlagMatchesCmp(Blk.Ops, static_cast<int>(I) - 1, CC, LocalCmp,
                               TRI))
        continue;
      if (condReadsConditionalFlag(Blk.Ops, static_cast<int>(I) - 1, CC, TRI))
        continue;
      // The positional HasFPFlag scan above stops at the first integer
      // SUB/AND/ADD, which a preceding SETcc's partial-register merge
      // (`reg & 0xFF..00`) supplies — masking an UCOMISS that set the real
      // flag. Re-check by walking to the actual flag definition so EQ/NE never
      // folds an FP-derived ZF to a stale integer compare (x86 `ucomiss; setae;
      // setne`).
      if (condReadsFPFlag(Blk.Ops, static_cast<int>(I) - 1, CC, TRI))
        continue;

      if (Op.Output.Size <= 1) {
        // SETCC byte result: the comparison directly produces the size-1
        // output, replacing the INT_ZEXT entirely.
        auto CmpOp = buildCmpOp(CC, LocalCmp.A, LocalCmp.B, Op.Output, Op.Addr);
        markFlagChainDead(Blk.Ops, static_cast<int>(I) - 1, SrcVar, BlkIdx,
                          UseBlocks);
        Op.Dead = true;
        Blk.Ops.insert(Blk.Ops.begin() + static_cast<long>(I) + 1, CmpOp);
        ++I;
      } else {
        // Wide INT_ZEXT (e.g. a CMOV mask base `CondExt = zext(cond)` that is
        // later read at the full width by INT_NEG2).  buildCmpOp forces its
        // output size to 1, so if we redefined this wide output directly it
        // would be written 1 byte but read N bytes downstream — a
        // store-i8/load-iN mismatch yielding undef upper bytes that the
        // optimizer exploits (it silently dropped a CRC iteration in the
        // crc8 -O2 roundtrip).  Instead, keep the INT_ZEXT and feed it the
        // comparison result (size 1), preserving the wide output's size.
        MedVar BoolOut = SrcVar; // size-1 boolean (the INT_ZEXT input)
        auto CmpOp = buildCmpOp(CC, LocalCmp.A, LocalCmp.B, BoolOut, Op.Addr);
        markFlagChainDead(Blk.Ops, static_cast<int>(I) - 1, BoolOut, BlkIdx,
                          UseBlocks);
        Blk.Ops.insert(Blk.Ops.begin() + static_cast<long>(I), CmpOp);
        ++I; // points at the (kept) INT_ZEXT; loop's ++I skips past it
      }
    }

    // --- Pass 3: SELECT condition simplification ---
    std::map<std::string, MedVar> ResolvedFlags;

    for (size_t I = 0; I < Blk.Ops.size(); ++I) {
      auto &Op = Blk.Ops[I];
      if (Op.Opcode != NdOp::SELECT || Op.NumInputs < 3)
        continue;

      auto &CondVar = Op.Inputs[0];
      if (CondVar.isConst())
        continue;

      if (CondVar.Kind == MedVar::Flag) {
        std::string Key = CondVar.display();
        auto Rit = ResolvedFlags.find(Key);
        if (Rit != ResolvedFlags.end()) {
          Op.Inputs[0] = Rit->second;
          continue;
        }
      }

      CondCode CC = CondCode::Invalid;
      if (CondVar.Kind == MedVar::Flag && CondVar.RegOff == TRI.FlagZF)
        CC = CondCode::EQ;

      if (CC == CondCode::Invalid)
        CC = resolveCondFromChain(Blk.Ops, CondVar, static_cast<int>(I) - 1,
                                  TRI);
      if (CC == CondCode::Invalid)
        continue;
      if (condToOpcode(CC) == NdOp::NOP)
        continue;

      // For an equality SELECT (e.g. FCMOVE/CMOVE reading ZF) the fold below
      // walks back to an INT_SUB/INT_AND, skipping over any non-comparison ZF
      // writer in between.  Require the *nearest* ZF definition to be the
      // integer compare (INT_EQUAL); otherwise the flag was set by something
      // else later (FUCOMI/FCOM write ZF via BOOL_OR over a FLOAT_EQUAL; SAHF
      // via INT_NOTEQUAL) and folding to an earlier compare uses wrong
      // operands.
      if (CC == CondCode::EQ || CC == CondCode::NE) {
        bool NearestIsIntCmp = false;
        for (int J = static_cast<int>(I) - 1; J >= 0; --J) {
          auto &P = Blk.Ops[J];
          if (P.Output.Kind == MedVar::Flag && P.Output.RegOff == TRI.FlagZF) {
            NearestIsIntCmp = (P.Opcode == NdOp::INT_EQUAL);
            break;
          }
          if (P.Opcode == NdOp::COND_BR)
            break;
        }
        if (!NearestIsIntCmp)
          continue;
      }

      CmpSource SelCmp;
      bool SelIsTst = false;
      MedVar SelAndResult;
      for (int J = static_cast<int>(I) - 1; J >= 0; --J) {
        auto &Prev = Blk.Ops[J];
        if (Prev.Output.Kind == MedVar::Flag &&
            (Prev.Opcode == NdOp::INT_EQUAL ||
             Prev.Opcode == NdOp::INT_SLESS) &&
            Prev.NumInputs >= 2) {
          const auto &ResVar = Prev.Inputs[0];
          for (int K = J - 1; K >= 0; --K) {
            auto &PP = Blk.Ops[K];
            if ((PP.Opcode == NdOp::INT_SUB || PP.Opcode == NdOp::INT_AND) &&
                PP.Output.Id == ResVar.Id &&
                PP.Output.SSAVer == ResVar.SSAVer && PP.NumInputs >= 2) {
              SelCmp.A = PP.Inputs[0];
              SelCmp.B = PP.Inputs[1];
              SelAndResult = PP.Output;
              SelIsTst = (PP.Opcode == NdOp::INT_AND);
              SelCmp.Valid = true;
              SelCmp.FromSub = (PP.Opcode == NdOp::INT_SUB);
              break;
            }
          }
          break;
        }
        if (Prev.Opcode == NdOp::COND_BR)
          break;
      }
      if (!SelCmp.Valid)
        continue;
      // Carry/overflow and signed-overflow conditions are only sound from a
      // real subtraction (CMOVcc/predicated selects after add/cmn lose OF).
      if (condNeedsGenuineSub(CC) && !SelCmp.FromSub)
        continue;
      if (condIsLoneSignFlag(Blk.Ops, CondVar, static_cast<int>(I) - 1, TRI))
        continue;
      if (!carryFlagMatchesCmp(Blk.Ops, static_cast<int>(I) - 1, CC, SelCmp,
                               TRI))
        continue;
      if (condReadsConditionalFlag(Blk.Ops, static_cast<int>(I) - 1, CC, TRI))
        continue;

      MedVar CmpA, CmpB;
      if (SelIsTst) {
        CmpA = SelAndResult;
        CmpB = MedVar::makeConst(0, SelAndResult.Size);
      } else {
        CmpA = SelCmp.A;
        CmpB = SelCmp.B;
      }

      auto CmpOp = buildCmpOp(CC, CmpA, CmpB, CondVar, Op.Addr);
      markFlagChainDead(Blk.Ops, static_cast<int>(I) - 1, CondVar, BlkIdx,
                        UseBlocks);

      if (CondVar.Kind == MedVar::Flag)
        ResolvedFlags[CondVar.display()] = CmpOp.Output;

      Blk.Ops.insert(Blk.Ops.begin() + static_cast<long>(I), CmpOp);
      ++I;

      auto &SelOp = Blk.Ops[I];
      if (SelOp.Inputs[0].Kind == MedVar::Flag)
        SelOp.Inputs[0] = CmpOp.Output;
    }

    // Remove dead ops
    Blk.Ops.erase(std::remove_if(Blk.Ops.begin(), Blk.Ops.end(),
                                 [](const MedOp &Op) { return Op.Dead; }),
                  Blk.Ops.end());
  }
}

} // namespace neverd
