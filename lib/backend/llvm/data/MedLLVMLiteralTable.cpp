//===- MedLLVMLiteralTable.cpp - Literal table resolution -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Literal-pool and select-merged read-only table resolution for
/// MedLLVMEmitter.
///
//===----------------------------------------------------------------------===//

#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/object/SectionNames.h"
#include "neverd/support/Diagnostic.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Instructions.h"

#include <algorithm>
#include <functional>
#include <limits>
#include <optional>
#include <set>
#include <tuple>
#include <vector>

namespace neverd {

llvm::Constant *MedLLVMEmitter::tryResolveReadOnlyDataOccurrence(
    const MedVar &Occurrence, uint64_t Address, uint16_t SizeHint) {
  if (!Img)
    return nullptr;
  const unsigned PtrSize = getTargetRegInfo(TargetArch).PointerSize;
  const uint64_t Mask = PtrSize == 0 || PtrSize >= 8
                            ? ~uint64_t(0)
                            : (uint64_t(1) << (PtrSize * 8)) - 1;
  // A 32-bit absolute pointer LOAD is an address-sized bit pattern.  The
  // generic literal tracer sign-extends 4-byte loads because PC-relative
  // displacements also use it; canonicalize this occurrence before applying
  // its exact relocation owner.
  const uint64_t CanonicalAddress = Address & Mask;
  bool OwnerConflict = false;
  std::optional<uint64_t> Owner;
  std::set<DataAddressIdentity> Identities;
  if (recoverAbsoluteDataPointerLoadIdentities(Occurrence, Identities)) {
    for (const DataAddressIdentity &Identity : Identities) {
      if ((Identity.VA & Mask) != CanonicalAddress)
        continue;
      if (Identity.OwnerVA == InvalidVA ||
          (Owner && *Owner != Identity.OwnerVA)) {
        OwnerConflict = true;
        break;
      }
      Owner = Identity.OwnerVA;
    }
    // An authenticated absolute pointer LOAD may never degrade to a
    // value-only lookup, even when legacy input omitted its target owner.
    if (!Owner)
      OwnerConflict = true;
  } else {
    Owner = uniqueDataAddressOwner(Occurrence, CanonicalAddress, OwnerConflict);
  }
  if (OwnerConflict || (CanonicalAddress == 0 && !Owner))
    return nullptr;
  if (Owner) {
    if (!isMaterializableReadOnlyDataAddress(CanonicalAddress, *Owner))
      return nullptr;
    return tryResolveOwnedGlobalData(CanonicalAddress, *Owner, SizeHint);
  }
  if (!isMaterializableReadOnlyDataAddress(CanonicalAddress))
    return nullptr;
  return tryResolveGlobalData(CanonicalAddress, SizeHint);
}

llvm::Value *MedLLVMEmitter::tryResolveLiteralPoolTable(
    const MedVar &AddrVar, uint16_t SizeHint, llvm::IRBuilder<> &Builder) {
  if (!CurMedFunc || !Img || AddrVar.isConst())
    return nullptr;

  const MedOp *Def = lookupDef(AddrVar);
  if (!Def)
    return nullptr;

  // An ARM32 predicated address-add (`addeq r,r,#k`) lowers to a SELECT of two
  // table addresses.  Resolve each arm to its rodata litptr and select between
  // the pointers, so the conditional table index is redirected into the rebuilt
  // rodata global instead of left as raw PC+literal arithmetic (which still
  // points at the original, un-relocated table address).
  if (selectPreservesPointerValues(*Def)) {
    llvm::Value *PT =
        tryResolveLiteralPoolTable(Def->Inputs[1], SizeHint, Builder);
    llvm::Value *PF =
        tryResolveLiteralPoolTable(Def->Inputs[2], SizeHint, Builder);
    if (!PT || !PF)
      return nullptr;
    llvm::Value *Cond = getVar(Def->Inputs[0], Builder);
    if (!Cond)
      return nullptr;
    if (!Cond->getType()->isIntegerTy(1))
      Cond = Builder.CreateICmpNE(
          Cond, llvm::ConstantInt::get(Cond->getType(), 0), "litselc");
    return Builder.CreateSelect(Cond, PT, PF, "litselptr");
  }

  if (Def->NumInputs < 2 ||
      (Def->Opcode != NdOp::INT_ADD && Def->Opcode != NdOp::INT_SUB))
    return nullptr;

  // The base must fold through a literal-pool LOAD (the ARM `ldr rN,[pc]; add
  // rN,pc` idiom); this gates the pattern so direct-const x86/AArch64 tables
  // and stack arrays never reach here.  Decompose the (possibly
  // multi-dimensional) address into the literal-pool base plus runtime index
  // addends.  The index must stay runtime.
  uint64_t Base = 0;
  bool HaveBase = false;
  std::vector<MedVar> IdxTerms;
  if (!collectLiteralPoolBase(AddrVar, Base, HaveBase, IdxTerms) || !HaveBase ||
      IdxTerms.empty())
    return tryResolveSelectBaseLitTable(AddrVar, SizeHint, Builder);

  // A real table index is a data value; a frame-derived addend is stack-pointer
  // arithmetic, not a table access.
  for (const auto &T : IdxTerms)
    if (varIsFrameDerived(T))
      return nullptr;

  // Only redirect into a genuine read-only table at this base, and never when
  // the function indexes-stores to it (a read-write array).
  if (StoredBasesFor != CurMedFunc) {
    StoredBasesFor = CurMedFunc;
    StoredConstBases.clear();
    for (const auto &Blk : CurMedFunc->Blocks)
      for (const auto &Op : Blk.Ops)
        if (Op.Opcode == NdOp::STORE &&
            Op.MemoryAddressSpace == NdMemoryAddressSpace::Default &&
            Op.NumInputs >= 1)
          if (auto SB = indexedConstBase(Op.Inputs[0]))
            StoredConstBases.insert(*SB);
  }
  if (StoredConstBases.count(Base))
    return nullptr;

  auto *G = tryResolveReadOnlyDataOccurrence(AddrVar, Base, SizeHint);
  if (!G)
    return nullptr;
  if (auto *GV = llvm::dyn_cast<llvm::GlobalVariable>(G->stripPointerCasts()))
    if (!GV->isConstant())
      return nullptr;

  // Sum the index addends at address width; GEP by the resulting byte offset.
  unsigned AddrBits = AddrVar.Size > 0 ? AddrVar.Size * 8 : 64;
  auto *IdxTy = llvm::IntegerType::get(*Ctx, AddrBits);
  llvm::Value *IdxVal = nullptr;
  for (const auto &T : IdxTerms) {
    llvm::Value *TV = getVar(T, Builder);
    if (!TV)
      return nullptr;
    if (TV->getType()->isPointerTy())
      TV = Builder.CreatePtrToInt(TV, IdxTy);
    else if (TV->getType() != IdxTy)
      TV = Builder.CreateZExtOrTrunc(TV, IdxTy);
    IdxVal = IdxVal ? Builder.CreateAdd(IdxVal, TV, "litidx") : TV;
  }
  if (!IdxVal)
    return nullptr;
  return Builder.CreateGEP(llvm::Type::getInt8Ty(*Ctx), G, IdxVal, "litptr");
}

llvm::Value *MedLLVMEmitter::tryResolveSelectBaseLitTable(
    const MedVar &AddrVar, uint16_t SizeHint, llvm::IRBuilder<> &Builder) {
  if (!CurMedFunc || !Img)
    return nullptr;

  auto findDef = [&](const MedVar &V) { return lookupDef(V); };

  // A two-way pointer select of distinct rodata table bases, in either form:
  //   * a clean SELECT(cond, A, B) — ARM/i386 predicated table-base load; or
  //   * a branchless bitwise blend `(A & m) | (B & ~m)` with m = -zext(cond)
  //     (all-ones iff cond), the x86-64 `cmov` lowering of `cond ? A : B`.
  // Recognize either and report the condition and the two arms (the arm
  // selected when cond is true, then the other).
  struct SelArms {
    MedVar Cond, ArmT, ArmF;
    bool Ok = false;
  };
  auto matchSel = [&](const MedVar &V) -> SelArms {
    const MedOp *Def = findDef(V);
    if (!Def)
      return {};
    if (selectPreservesPointerValues(*Def))
      return {Def->Inputs[0], Def->Inputs[1], Def->Inputs[2], true};
    MedVar Cond, ArmT, ArmF;
    if (isMaskedSelectOr(*Def, Cond, ArmT, ArmF))
      return {Cond, ArmT, ArmF, true};
    return {};
  };

  // Peel the runtime index addends off the add chain down to that select/blend.
  SelArms Sel;
  std::vector<MedVar> IdxTerms;
  std::function<bool(const MedVar &, int)> peel = [&](const MedVar &V,
                                                      int Depth) -> bool {
    if (Depth > 8)
      return false;
    if (SelArms S = matchSel(V); S.Ok) {
      Sel = S;
      return true;
    }
    const MedOp *Def = findDef(V);
    if (!Def)
      return false;
    if ((Def->Opcode == NdOp::INT_ADD || Def->Opcode == NdOp::INT_SUB) &&
        Def->NumInputs >= 2) {
      if (peel(Def->Inputs[0], Depth + 1)) {
        if (Def->Output.Size == 0 || Def->Inputs[0].Size == 0 ||
            Def->Output.Size < Def->Inputs[0].Size)
          return false;
        IdxTerms.push_back(Def->Inputs[1]);
        return true;
      }
      if (Def->Opcode == NdOp::INT_ADD && peel(Def->Inputs[1], Depth + 1)) {
        if (Def->Output.Size == 0 || Def->Inputs[1].Size == 0 ||
            Def->Output.Size < Def->Inputs[1].Size)
          return false;
        IdxTerms.push_back(Def->Inputs[0]);
        return true;
      }
    }
    return false;
  };
  if (!peel(AddrVar, 0) || !Sel.Ok || IdxTerms.empty())
    return nullptr;

  for (const auto &T : IdxTerms)
    if (varIsFrameDerived(T))
      return nullptr;

  if (StoredBasesFor != CurMedFunc) {
    StoredBasesFor = CurMedFunc;
    StoredConstBases.clear();
    for (const auto &Blk : CurMedFunc->Blocks)
      for (const auto &Op : Blk.Ops)
        if (Op.Opcode == NdOp::STORE &&
            Op.MemoryAddressSpace == NdMemoryAddressSpace::Default &&
            Op.NumInputs >= 1)
          if (auto SB = indexedConstBase(Op.Inputs[0]))
            StoredConstBases.insert(*SB);
  }

  unsigned AddrBits = AddrVar.Size > 0 ? AddrVar.Size * 8 : 64;
  auto *IdxTy = llvm::IntegerType::get(*Ctx, AddrBits);
  llvm::Value *IdxVal = nullptr;
  for (const auto &T : IdxTerms) {
    llvm::Value *TV = getVar(T, Builder);
    if (!TV)
      return nullptr;
    if (TV->getType()->isPointerTy())
      TV = Builder.CreatePtrToInt(TV, IdxTy);
    else if (TV->getType() != IdxTy)
      TV = Builder.CreateZExtOrTrunc(TV, IdxTy);
    IdxVal = IdxVal ? Builder.CreateAdd(IdxVal, TV, "litidx") : TV;
  }
  if (!IdxVal)
    return nullptr;

  // Resolve one select/blend arm to an indexed pointer.  An arm is a base in a
  // read-only data segment reached either through a literal-pool LOAD (ARM/i386
  // `ldr[pc]+pc`) or as a bare rip-relative constant (x86-64 `lea`); or itself
  // a nested select/blend of two such arms — `(c0?(c1?A:B):(c2?C:D))[i]` —
  // resolved recursively into a select of the two indexed pointers.
  std::function<llvm::Value *(const MedVar &, int)> armPtr =
      [&](const MedVar &Arm, int Depth) -> llvm::Value * {
    if (Depth > 8)
      return nullptr;
    if (SelArms S = matchSel(Arm); S.Ok) {
      llvm::Value *AT = armPtr(S.ArmT, Depth + 1);
      llvm::Value *AF = armPtr(S.ArmF, Depth + 1);
      if (!AT || !AF)
        return nullptr;
      llvm::Value *C = getVar(S.Cond, Builder);
      if (!C)
        return nullptr;
      if (!C->getType()->isIntegerTy(1))
        C = Builder.CreateICmpNE(C, llvm::ConstantInt::get(C->getType(), 0),
                                 "litselc");
      return Builder.CreateSelect(C, AT, AF, "litselptr");
    }
    bool SawLoad = false;
    bool SawArithmetic = false;
    auto VA = traceTableBaseConst(Arm, 0, &SawLoad, nullptr, &SawArithmetic);
    // The arm must fold to a constant VA in exact data/rodata. A literal-pool
    // LOAD is genuine even when its target lives inline in an instruction
    // section; a bare constant (x86-64 `lea rip` base) is accepted only for an
    // exact data address because it feeds a runtime-indexed pointer blend.
    // Whole-expression arithmetic may coincidentally land on a table address;
    // it does not prove that either operand owns that table's provenance. A
    // literal-pool LOAD is the only arithmetic fold that supplies such proof.
    if (!VA || (SawArithmetic && !SawLoad))
      return nullptr;
    if (StoredConstBases.count(*VA))
      return nullptr;
    auto *G = tryResolveReadOnlyDataOccurrence(Arm, *VA, SizeHint);
    if (!G)
      return nullptr;
    if (auto *GV = llvm::dyn_cast<llvm::GlobalVariable>(G->stripPointerCasts()))
      if (!GV->isConstant())
        return nullptr;
    return Builder.CreateGEP(llvm::Type::getInt8Ty(*Ctx), G, IdxVal, "litptr");
  };

  llvm::Value *PT = armPtr(Sel.ArmT, 0);
  llvm::Value *PF = armPtr(Sel.ArmF, 0);
  if (!PT || !PF)
    return nullptr;
  llvm::Value *Cond = getVar(Sel.Cond, Builder);
  if (!Cond)
    return nullptr;
  if (!Cond->getType()->isIntegerTy(1))
    Cond = Builder.CreateICmpNE(
        Cond, llvm::ConstantInt::get(Cond->getType(), 0), "litselc");
  return Builder.CreateSelect(Cond, PT, PF, "litselptr");
}

llvm::Value *MedLLVMEmitter::tryResolveSelectMergeTable(
    const MedVar &AddrVar, uint16_t SizeHint, bool FailClosed,
    llvm::IRBuilder<> &Builder, bool *SawAmbiguous) {
  if (!CurMedFunc || !Img || AddrVar.isConst())
    return nullptr;

  auto findDef = [&](const MedVar &V) { return lookupDef(V); };
  auto findPhi = [&](const MedVar &V) { return lookupPhi(V); };
  auto isReadOnlyTableBase = [&](uint64_t VA) {
    return isMaterializableReadOnlyDataAddress(VA);
  };

  struct ReadOnlyBaseIdentity {
    uint64_t VA = 0;
    uint64_t OwnerVA = InvalidVA;

    bool operator<(const ReadOnlyBaseIdentity &Other) const {
      return std::tie(VA, OwnerVA) < std::tie(Other.VA, Other.OwnerVA);
    }
  };
  auto isOwnedReadOnlyTableBase = [&](uint64_t VA, uint64_t OwnerVA) {
    if (OwnerVA == InvalidVA)
      return isReadOnlyTableBase(VA);
    const Segment *OwnerSeg = Img->getSegmentFor(OwnerVA);
    if (!OwnerSeg || !OwnerSeg->isReadable() || OwnerSeg->Data.empty())
      return false;
    const Section *OwnerSec = Img->getSectionFor(OwnerVA);
    const uint64_t Begin = OwnerSec ? OwnerSec->VA : OwnerSeg->VA;
    const uint64_t Size = OwnerSec ? OwnerSec->Size : OwnerSeg->Size;
    // Relocations can intentionally name a read-only symbol with a small
    // negative addend (`table - 1`) so a subsequent one-based index lands on
    // the first element.  The owner, not the adjusted numeric value, selects
    // the rebuilt run.  Admit at most one native word of look-behind; larger
    // cross-object arithmetic still needs an explicit complete-domain proof.
    const uint64_t PointerSize = getTargetRegInfo(TargetArch).PointerSize;
    const uint64_t LowerBound = Begin > PointerSize ? Begin - PointerSize : 0;
    if (Size > InvalidVA - Begin || VA < LowerBound || VA > Begin + Size)
      return false;

    bool HasReadOnlySectionEvidence = false;
    if (OwnerSec)
      HasReadOnlySectionEvidence =
          OwnerSec->isReadable() &&
          (Img->isMachO() ? !Img->isCodeAddress(OwnerVA)
                          : !OwnerSec->isExecutable()) &&
          (!OwnerSec->isWritable() ||
           section_names::isReadOnlyAfterRelocSectionName(OwnerSec->Name) ||
           section_names::isReadOnlyAfterRelocSectionName(
               OwnerSec->SegmentName));
    if (OwnerSeg->isWritable() && !isReadOnlyAfterReloc(OwnerSeg) &&
        !HasReadOnlySectionEvidence)
      return false;
    if (OwnerSeg->isExecutable() && Img->hasExecutableCodeOwnerAt(OwnerVA) &&
        !HasReadOnlySectionEvidence)
      return false;
    return true;
  };

  // i386 models a machine-width frame pointer through widened MedIR
  // temporaries (`zext esp -> rbp; rbp + negative_offset`).  The narrower
  // varIsFrameDerived predicate intentionally does not follow those width ops,
  // so prove this ordinary stack-address shape locally before auditing table
  // provenance.  Only direct, non-relocating numeric displacements are allowed:
  // `sp + table_base` must still enter the full walker and fail closed.
  using FrameSeen = std::set<std::tuple<int, int, int>>;
  const uint64_t StackPointer = getTargetRegInfo(TargetArch).StackPointer;
  std::function<bool(const MedVar &, int, FrameSeen)> isPureFrameAddress =
      [&](const MedVar &V, int Depth, FrameSeen Seen) -> bool {
    if (V.isConst() || Depth > 16)
      return false;
    if (V.Kind == MedVar::Reg && StackPointer != 0 && V.RegOff == StackPointer)
      return true;
    auto Key = std::make_tuple(static_cast<int>(V.Kind), V.Id, V.SSAVer);
    if (!Seen.insert(Key).second)
      return false;
    const MedOp *Def = findDef(V);
    if (!Def || Def->NumInputs < 1)
      return false;
    auto stableImmediate = [&](const MedVar &Input) {
      if (!Input.isConst())
        return false;
      if (Input.Provenance == ConstantAddressProvenance::Scalar)
        return true;
      if (isAddressProvenance(Input.Provenance) ||
          getVarMayRelocateConstant(Input.ConstVal, Input.Size))
        return false;
      return !hasObjectDataProvenance(Input.ConstVal);
    };
    switch (Def->Opcode) {
    case NdOp::COPY:
    case NdOp::INT_ZEXT:
    case NdOp::INT_SEXT:
      return isPureFrameAddress(Def->Inputs[0], Depth + 1, Seen);
    case NdOp::SUBBYTES:
      return Def->NumInputs >= 2 && Def->Inputs[1].isConst() &&
             Def->Inputs[1].ConstVal == 0 &&
             isPureFrameAddress(Def->Inputs[0], Depth + 1, Seen);
    case NdOp::INT_ADD:
      if (Def->NumInputs < 2)
        return false;
      return (stableImmediate(Def->Inputs[1]) &&
              isPureFrameAddress(Def->Inputs[0], Depth + 1, Seen)) ||
             (stableImmediate(Def->Inputs[0]) &&
              isPureFrameAddress(Def->Inputs[1], Depth + 1, Seen));
    case NdOp::INT_SUB:
      return Def->NumInputs >= 2 && stableImmediate(Def->Inputs[1]) &&
             isPureFrameAddress(Def->Inputs[0], Depth + 1, Seen);
    default:
      return false;
    }
  };
  if (isPureFrameAddress(AddrVar, 0, {}))
    return nullptr;

  // Analyze the complete address expression.  The outer ADD/SUB is part of the
  // proof: accepting one PHI side while silently treating another pointer PHI,
  // a relocatable constant, or a truncating arithmetic result as an index is
  // exactly the stale-address failure this resolver must prevent.
  MedVar BaseVar = AddrVar;

  // Walk the base DAG collecting every rodata-segment base constant.  Ownership
  // requires a complete pointer merge: a non-recurrent cross-block PHI, a
  // pointer-preserving SELECT/blend, or an all-path-proven frame reload with
  // distinct reaching bases.  Traverse the constructs clang emits around those
  // merges: PHI, SELECT, the bitwise blend (INT_OR/INT_AND), width casts, COPY,
  // and the literal-pool LOAD (folded by traceTableBaseConst).
  std::set<ReadOnlyBaseIdentity> Bases;
  bool SawPhi = false;
  bool SawNonRecurrentPhi = false;
  bool SawPointerValueMerge = false;
  bool SawNonBaseValueMerge = false;
  const PhiNode *EvidencePhi = nullptr;
  bool SawInvalidPointerExpression = false;
  bool SawTableShapedInvalidExpression = false;
  bool SawUnprovedFrameReload = false;
  bool SawRawOriginalBase = false;
  bool SawSymbolizedBase = false;
  bool AuditIncomplete = false;
  // Large flag-expanded scalar DAGs in real computed-goto loops use just under
  // 5K nodes. Keep the audit bounded while leaving headroom above that proven
  // valid shape; a partial walk still fails closed below.
  size_t RemainingAuditNodes = 8192;
  // A speculative integer call argument may merely resemble an address.
  // Its caller passes FailClosed=false so an ambiguous proof returns nullptr
  // and preserves the ordinary integer ABI path. Concrete memory accesses and
  // known pointer ABI parameters pass true because retaining a stale table VA
  // in those contexts would be unsafe.
  using SeenSet = std::set<std::tuple<int, int, int>>;
  // Cycle is a coinductive edge inside the address-expression SCC.  It is not
  // a base by itself: only a complete PHI/selection that also has a real
  // HasBase arm may discharge it.  Keeping it distinct from Invalid makes the
  // proof traversal-order independent without granting an unanchored cycle
  // pointer authority.
  enum class BaseProof { Invalid, NoBase, HasBase, Cycle };
  auto isExactSameVar = [](const MedVar &Left, const MedVar &Right) {
    if (Left.Kind != Right.Kind || Left.TheArch != Right.TheArch ||
        Left.RenameTag != Right.RenameTag || Left.Id != Right.Id ||
        Left.SSAVer != Right.SSAVer || Left.Size != Right.Size)
      return false;
    switch (Left.Kind) {
    case MedVar::Const:
      return Left.ConstVal == Right.ConstVal &&
             Left.Provenance == Right.Provenance &&
             Left.AddressOwnerVA == Right.AddressOwnerVA;
    case MedVar::Reg:
    case MedVar::Param:
      return Left.RegOff == Right.RegOff;
    case MedVar::Stack:
      return Left.StackOff == Right.StackOff;
    default:
      return true;
    }
  };
  auto readOnlyBaseIdentity =
      [&](const MedVar &Occurrence,
          uint64_t VA) -> std::optional<ReadOnlyBaseIdentity> {
    const uint16_t PointerSize = getTargetRegInfo(TargetArch).PointerSize;
    if (Occurrence.Size != 0 && PointerSize != 0 &&
        Occurrence.Size < PointerSize && Occurrence.Size < 8 &&
        (VA >> (Occurrence.Size * 8)) != 0)
      return std::nullopt;
    bool OwnerConflict = false;
    std::optional<uint64_t> Owner =
        uniqueDataAddressOwner(Occurrence, VA, OwnerConflict);
    if (OwnerConflict)
      return std::nullopt;
    const uint64_t OwnerVA = Owner.value_or(InvalidVA);
    if (!isOwnedReadOnlyTableBase(VA, OwnerVA))
      return std::nullopt;
    return ReadOnlyBaseIdentity{VA, OwnerVA};
  };

  // A run of ARM predicated instructions is split into synthetic effect
  // blocks before SSA construction.  The resulting address can look like
  //   PHI(default, literal_base) -> SELECT(pred, base + pc, default) -> LOAD
  // even though the LOAD effect block is reachable only when `pred` is true.
  // Prove that path correlation instead of rejecting the unreachable scalar
  // PHI arm, but keep the proof deliberately local to the exact synthetic CFG
  // shape produced by materializePredicatedEffects.
  struct ConditionSet {
    bool IsConstant = false;
    bool Constant = false;
    MedVar Root;
    uint64_t Max = 0;
    std::vector<std::pair<uint64_t, uint64_t>> Intervals;
  };
  auto constantCondition = [](bool Value) {
    ConditionSet Result;
    Result.IsConstant = true;
    Result.Constant = Value;
    return Result;
  };
  auto maxForSize = [](uint16_t Size) {
    return Size == 0 || Size >= 8 ? std::numeric_limits<uint64_t>::max()
                                  : (uint64_t(1) << (Size * 8)) - 1;
  };
  auto normalizeIntervals = [](std::vector<std::pair<uint64_t, uint64_t>> V) {
    std::sort(V.begin(), V.end());
    std::vector<std::pair<uint64_t, uint64_t>> Result;
    for (auto Range : V) {
      if (Range.first > Range.second)
        continue;
      if (Result.empty() ||
          (Result.back().second != std::numeric_limits<uint64_t>::max() &&
           Range.first > Result.back().second + 1)) {
        Result.push_back(Range);
      } else {
        Result.back().second = std::max(Result.back().second, Range.second);
      }
    }
    return Result;
  };
  auto complementCondition = [&](ConditionSet Input) {
    if (Input.IsConstant)
      return constantCondition(!Input.Constant);
    std::vector<std::pair<uint64_t, uint64_t>> Complement;
    uint64_t Next = 0;
    bool AtEnd = false;
    for (const auto &[Lo, Hi] : Input.Intervals) {
      if (Next < Lo)
        Complement.emplace_back(Next, Lo - 1);
      if (Hi == Input.Max) {
        AtEnd = true;
        break;
      }
      Next = Hi + 1;
    }
    if (!AtEnd)
      Complement.emplace_back(Next, Input.Max);
    Input.Intervals = normalizeIntervals(std::move(Complement));
    return Input;
  };
  auto combineConditions = [&](ConditionSet Left, ConditionSet Right,
                               bool IsOr) -> std::optional<ConditionSet> {
    if (Left.IsConstant)
      return IsOr ? (Left.Constant ? std::optional<ConditionSet>(Left)
                                   : std::optional<ConditionSet>(Right))
                  : (Left.Constant ? std::optional<ConditionSet>(Right)
                                   : std::optional<ConditionSet>(Left));
    if (Right.IsConstant)
      return IsOr ? (Right.Constant ? std::optional<ConditionSet>(Right)
                                    : std::optional<ConditionSet>(Left))
                  : (Right.Constant ? std::optional<ConditionSet>(Left)
                                    : std::optional<ConditionSet>(Right));
    if (!isExactSameVar(Left.Root, Right.Root) || Left.Max != Right.Max)
      return std::nullopt;
    if (IsOr) {
      Left.Intervals.insert(Left.Intervals.end(), Right.Intervals.begin(),
                            Right.Intervals.end());
      Left.Intervals = normalizeIntervals(std::move(Left.Intervals));
      return Left;
    }
    std::vector<std::pair<uint64_t, uint64_t>> Intersection;
    size_t I = 0, J = 0;
    while (I < Left.Intervals.size() && J < Right.Intervals.size()) {
      uint64_t Lo = std::max(Left.Intervals[I].first, Right.Intervals[J].first);
      uint64_t Hi =
          std::min(Left.Intervals[I].second, Right.Intervals[J].second);
      if (Lo <= Hi)
        Intersection.emplace_back(Lo, Hi);
      if (Left.Intervals[I].second < Right.Intervals[J].second)
        ++I;
      else
        ++J;
    }
    Left.Intervals = std::move(Intersection);
    return Left;
  };
  struct AffineRoot {
    MedVar Root;
    uint64_t Offset = 0;
    uint64_t Max = 0;
  };
  using CondSeen = std::set<std::tuple<int, int, int, uint16_t>>;
  std::function<std::optional<AffineRoot>(const MedVar &, int, CondSeen)>
      affineRoot = [&](const MedVar &Value, int Depth,
                       CondSeen Seen) -> std::optional<AffineRoot> {
    if (Value.isConst() || Depth > 16)
      return std::nullopt;
    auto Key = std::make_tuple(static_cast<int>(Value.Kind), Value.Id,
                               Value.SSAVer, Value.Size);
    if (!Seen.insert(Key).second)
      return AffineRoot{Value, 0, maxForSize(Value.Size)};
    const MedOp *Def = findDef(Value);
    if (!Def)
      return AffineRoot{Value, 0, maxForSize(Value.Size)};
    if (auto Forwarded = pointerPreservingInput(*Def)) {
      if (isExactSameVar(*Forwarded, Value))
        return AffineRoot{Value, 0, maxForSize(Value.Size)};
      return affineRoot(*Forwarded, Depth + 1, Seen);
    }
    if ((Def->Opcode == NdOp::INT_ADD || Def->Opcode == NdOp::INT_SUB) &&
        Def->NumInputs >= 2) {
      auto Right = traceValueVA(Def->Inputs[1]);
      if (Right) {
        auto Left = affineRoot(Def->Inputs[0], Depth + 1, Seen);
        if (!Left || Left->Max != maxForSize(Def->Output.Size))
          return std::nullopt;
        Left->Offset = Def->Opcode == NdOp::INT_ADD
                           ? (Left->Offset + *Right) & Left->Max
                           : (Left->Offset - *Right) & Left->Max;
        return Left;
      }
      if (Def->Opcode == NdOp::INT_ADD)
        if (auto LeftConst = traceValueVA(Def->Inputs[0])) {
          auto RightRoot = affineRoot(Def->Inputs[1], Depth + 1, Seen);
          if (!RightRoot || RightRoot->Max != maxForSize(Def->Output.Size))
            return std::nullopt;
          RightRoot->Offset = (RightRoot->Offset + *LeftConst) & RightRoot->Max;
          return RightRoot;
        }
    }
    return AffineRoot{Value, 0, maxForSize(Value.Size)};
  };
  std::function<std::optional<ConditionSet>(const MedVar &, int, CondSeen)>
      conditionSet = [&](const MedVar &Value, int Depth,
                         CondSeen Seen) -> std::optional<ConditionSet> {
    if (Depth > 24)
      return std::nullopt;
    if (Value.isConst())
      return constantCondition(Value.ConstVal != 0);
    auto Key = std::make_tuple(static_cast<int>(Value.Kind), Value.Id,
                               Value.SSAVer, Value.Size);
    if (!Seen.insert(Key).second)
      return std::nullopt;
    const MedOp *Def = findDef(Value);
    if (!Def)
      return std::nullopt;
    if (Def->Opcode == NdOp::COPY && Def->NumInputs >= 1 &&
        !isExactSameVar(Def->Inputs[0], Value))
      return conditionSet(Def->Inputs[0], Depth + 1, Seen);
    if (Def->Opcode == NdOp::BOOL_NOT && Def->NumInputs >= 1) {
      auto Input = conditionSet(Def->Inputs[0], Depth + 1, Seen);
      return Input ? std::optional<ConditionSet>(
                         complementCondition(std::move(*Input)))
                   : std::nullopt;
    }
    if ((Def->Opcode == NdOp::BOOL_AND || Def->Opcode == NdOp::BOOL_OR) &&
        Def->NumInputs >= 2) {
      auto Left = conditionSet(Def->Inputs[0], Depth + 1, Seen);
      auto Right = conditionSet(Def->Inputs[1], Depth + 1, Seen);
      if (!Left || !Right)
        return std::nullopt;
      return combineConditions(std::move(*Left), std::move(*Right),
                               Def->Opcode == NdOp::BOOL_OR);
    }
    if (Def->NumInputs < 2 ||
        (Def->Opcode != NdOp::INT_EQUAL && Def->Opcode != NdOp::INT_NOTEQUAL &&
         Def->Opcode != NdOp::INT_LESS && Def->Opcode != NdOp::INT_LESSEQUAL))
      return std::nullopt;

    auto makeAtomic = [&](const MedVar &Expr, uint64_t Constant,
                          bool RootOnLeft) -> std::optional<ConditionSet> {
      auto Affine = affineRoot(Expr, 0, {});
      if (!Affine)
        return std::nullopt;
      Constant &= Affine->Max;
      ConditionSet Result;
      Result.Root = Affine->Root;
      Result.Max = Affine->Max;
      if (Def->Opcode == NdOp::INT_EQUAL || Def->Opcode == NdOp::INT_NOTEQUAL) {
        uint64_t Match = (Constant - Affine->Offset) & Affine->Max;
        Result.Intervals.emplace_back(Match, Match);
        if (Def->Opcode == NdOp::INT_NOTEQUAL)
          Result = complementCondition(std::move(Result));
        return Result;
      }
      // Offset comparisons can wrap and are not one interval in the root
      // domain. They are unnecessary for the ARM predicate idiom, so refuse
      // them rather than weakening the proof.
      if (Affine->Offset != 0)
        return std::nullopt;
      if (Def->Opcode == NdOp::INT_LESS) {
        if (RootOnLeft) {
          if (Constant != 0)
            Result.Intervals.emplace_back(0, Constant - 1);
        } else if (Constant != Affine->Max) {
          Result.Intervals.emplace_back(Constant + 1, Affine->Max);
        }
      } else if (RootOnLeft) {
        Result.Intervals.emplace_back(0, Constant);
      } else {
        Result.Intervals.emplace_back(Constant, Affine->Max);
      }
      return Result;
    };
    if (auto Right = traceValueVA(Def->Inputs[1]))
      return makeAtomic(Def->Inputs[0], *Right, /*RootOnLeft=*/true);
    if (auto Left = traceValueVA(Def->Inputs[0]))
      return makeAtomic(Def->Inputs[1], *Left, /*RootOnLeft=*/false);
    return std::nullopt;
  };
  auto conditionsEquivalent = [&](const MedVar &Left, const MedVar &Right,
                                  bool InvertRight) {
    auto L = conditionSet(Left, 0, {});
    auto R = conditionSet(Right, 0, {});
    if (!L || !R)
      return false;
    if (InvertRight)
      *R = complementCondition(std::move(*R));
    if (L->IsConstant || R->IsConstant)
      return L->IsConstant && R->IsConstant && L->Constant == R->Constant;
    return isExactSameVar(L->Root, R->Root) && L->Max == R->Max &&
           L->Intervals == R->Intervals;
  };

  const MedBlock *PredicatedAccessBlock = nullptr;
  const MedOp *PredicatedAccessLoad = nullptr;
  const MedOp *PredicatedAccessGuard = nullptr;
  if (TargetArch == Arch::ARM) {
    for (const MedBlock &Block : CurMedFunc->Blocks)
      for (const MedOp &Op : Block.Ops)
        if (Op.Opcode == NdOp::LOAD && Op.NumInputs >= 1 &&
            isExactSameVar(Op.Inputs[0], AddrVar)) {
          if (PredicatedAccessLoad) {
            PredicatedAccessLoad = nullptr;
            PredicatedAccessBlock = nullptr;
            break;
          }
          PredicatedAccessLoad = &Op;
          PredicatedAccessBlock = &Block;
        }
    if (PredicatedAccessBlock && PredicatedAccessBlock->Preds.size() == 1) {
      const int GuardId = PredicatedAccessBlock->Preds.front();
      const MedBlock *GuardBlock = nullptr;
      for (const MedBlock &Block : CurMedFunc->Blocks)
        if (Block.Id == GuardId) {
          GuardBlock = &Block;
          break;
        }
      if (GuardBlock && GuardBlock->Succs.size() == 2 &&
          GuardBlock->Succs[1] == PredicatedAccessBlock->Id &&
          PredicatedAccessBlock->Succs.size() == 1 &&
          !GuardBlock->Ops.empty()) {
        const MedOp &Guard = GuardBlock->Ops.back();
        if (Guard.Opcode == NdOp::COND_BR && Guard.NumInputs >= 2 &&
            Guard.Addr == PredicatedAccessLoad->Addr &&
            PredicatedAccessBlock->StartAddr == Guard.Addr)
          PredicatedAccessGuard = &Guard;
      }
    }
  }

  auto blockById = [&](int Id) -> const MedBlock * {
    for (const MedBlock &Block : CurMedFunc->Blocks)
      if (Block.Id == Id)
        return &Block;
    return nullptr;
  };
  auto predicatedPhiInput = [&](const PhiNode &Phi) -> std::optional<MedVar> {
    if (!PredicatedAccessGuard || Phi.Args.size() != 2)
      return std::nullopt;
    const MedBlock *Merge = nullptr;
    for (const MedBlock &Block : CurMedFunc->Blocks)
      for (const PhiNode &Candidate : Block.Phis)
        if (&Candidate == &Phi)
          Merge = &Block;
    if (!Merge)
      return std::nullopt;
    for (const MedBlock &GuardBlock : CurMedFunc->Blocks) {
      if (GuardBlock.Succs.size() != 2 || GuardBlock.Succs[0] != Merge->Id ||
          GuardBlock.Ops.empty())
        continue;
      const MedOp &Guard = GuardBlock.Ops.back();
      if (Guard.Opcode != NdOp::COND_BR || Guard.NumInputs < 2)
        continue;
      const MedBlock *Effect = blockById(GuardBlock.Succs[1]);
      if (!Effect || Effect->Preds.size() != 1 ||
          Effect->Preds.front() != GuardBlock.Id || Effect->Succs.size() != 1 ||
          Effect->Succs.front() != Merge->Id || Effect->StartAddr != Guard.Addr)
        continue;
      const MedVar *DirectArg = nullptr;
      const MedVar *EffectArg = nullptr;
      for (const auto &[Pred, Arg] : Phi.Args) {
        if (Pred == GuardBlock.Id)
          DirectArg = &Arg;
        if (Pred == Effect->Id)
          EffectArg = &Arg;
      }
      if (!DirectArg || !EffectArg)
        continue;
      if (conditionsEquivalent(Guard.Inputs[1],
                               PredicatedAccessGuard->Inputs[1],
                               /*InvertRight=*/false))
        return *EffectArg; // both effect paths require their guard to be false
      if (conditionsEquivalent(Guard.Inputs[1],
                               PredicatedAccessGuard->Inputs[1],
                               /*InvertRight=*/true))
        return *DirectArg; // this direct path is the access guard's false path
    }
    return std::nullopt;
  };

  bool SawPredicatedLiteralLoad = false;
  using PredSeen = std::set<std::tuple<int, int, int, uint16_t>>;
  std::function<std::optional<uint64_t>(const MedVar &, int, PredSeen)>
      predicatedValue = [&](const MedVar &Value, int Depth,
                            PredSeen Seen) -> std::optional<uint64_t> {
    if (!PredicatedAccessGuard || Depth > 24)
      return std::nullopt;
    if (Value.isConst())
      return Value.ConstVal;
    auto Key = std::make_tuple(static_cast<int>(Value.Kind), Value.Id,
                               Value.SSAVer, Value.Size);
    if (!Seen.insert(Key).second)
      return std::nullopt;
    if (const PhiNode *Phi = findPhi(Value)) {
      auto Selected = predicatedPhiInput(*Phi);
      return Selected ? predicatedValue(*Selected, Depth + 1, Seen)
                      : std::nullopt;
    }
    const MedOp *Def = findDef(Value);
    if (!Def)
      return std::nullopt;
    if (auto Forwarded = pointerPreservingInput(*Def)) {
      if (isExactSameVar(*Forwarded, Value))
        return std::nullopt;
      return predicatedValue(*Forwarded, Depth + 1, Seen);
    }
    if (Def->Opcode == NdOp::SELECT && Def->NumInputs >= 3 &&
        selectPreservesPointerValues(*Def)) {
      if (conditionsEquivalent(Def->Inputs[0], PredicatedAccessGuard->Inputs[1],
                               /*InvertRight=*/true))
        return predicatedValue(Def->Inputs[1], Depth + 1, Seen);
      if (conditionsEquivalent(Def->Inputs[0], PredicatedAccessGuard->Inputs[1],
                               /*InvertRight=*/false))
        return predicatedValue(Def->Inputs[2], Depth + 1, Seen);
      return std::nullopt;
    }
    if ((Def->Opcode == NdOp::INT_ADD || Def->Opcode == NdOp::INT_SUB) &&
        Def->NumInputs >= 2) {
      auto Left = predicatedValue(Def->Inputs[0], Depth + 1, Seen);
      auto Right = predicatedValue(Def->Inputs[1], Depth + 1, Seen);
      if (!Left || !Right)
        return std::nullopt;
      uint64_t Result =
          Def->Opcode == NdOp::INT_ADD ? *Left + *Right : *Left - *Right;
      return Result & maxForSize(Def->Output.Size);
    }
    if (Def->Opcode == NdOp::LOAD) {
      bool SawLoad = false;
      auto Folded = traceTableBaseConst(Value, 0, &SawLoad);
      if (Folded && SawLoad) {
        SawPredicatedLiteralLoad = true;
        return Folded;
      }
    }
    return std::nullopt;
  };
  std::function<std::optional<uint64_t>(const MedVar &, int, PredSeen)>
      predicatedTableBase = [&](const MedVar &Value, int Depth,
                                PredSeen Seen) -> std::optional<uint64_t> {
    if (!PredicatedAccessGuard || Depth > 16)
      return std::nullopt;
    if (auto Folded = predicatedValue(Value, 0, {});
        Folded && isReadOnlyTableBase(*Folded))
      return Folded;
    if (Value.isConst())
      return std::nullopt;
    auto Key = std::make_tuple(static_cast<int>(Value.Kind), Value.Id,
                               Value.SSAVer, Value.Size);
    if (!Seen.insert(Key).second)
      return std::nullopt;
    const MedOp *Def = findDef(Value);
    if (!Def)
      return std::nullopt;
    if (auto Forwarded = pointerPreservingInput(*Def)) {
      if (!isExactSameVar(*Forwarded, Value))
        return predicatedTableBase(*Forwarded, Depth + 1, Seen);
      return std::nullopt;
    }
    if ((Def->Opcode != NdOp::INT_ADD && Def->Opcode != NdOp::INT_SUB) ||
        Def->NumInputs < 2)
      return std::nullopt;
    auto Left = predicatedValue(Def->Inputs[0], 0, {});
    if (Left && isReadOnlyTableBase(*Left) &&
        valueIsStableAddressOffset(Def->Inputs[1]))
      return Left;
    if (Def->Opcode == NdOp::INT_ADD) {
      auto Right = predicatedValue(Def->Inputs[1], 0, {});
      if (Right && isReadOnlyTableBase(*Right) &&
          valueIsStableAddressOffset(Def->Inputs[0]))
        return Right;
    }
    if (auto Nested = predicatedTableBase(Def->Inputs[0], Depth + 1, Seen);
        Nested && valueIsStableAddressOffset(Def->Inputs[1]))
      return Nested;
    if (Def->Opcode == NdOp::INT_ADD)
      if (auto Nested = predicatedTableBase(Def->Inputs[1], Depth + 1, Seen);
          Nested && valueIsStableAddressOffset(Def->Inputs[0]))
        return Nested;
    return std::nullopt;
  };
  auto markTableShapedInvalidFold = [&](const MedVar &Value) {
    if (auto Result = traceValueVA(Value);
        Result && isReadOnlyTableBase(*Result)) {
      SawInvalidPointerExpression = true;
      SawTableShapedInvalidExpression = true;
    }
  };
  std::function<BaseProof(const MedVar &, int, SeenSet)> walk =
      [&](const MedVar &V, int Depth, SeenSet Seen) -> BaseProof {
    if (RemainingAuditNodes == 0) {
      AuditIncomplete = true;
      return BaseProof::Invalid;
    }
    --RemainingAuditNodes;
    if (varIsFrameDerived(V))
      return BaseProof::Invalid;
    if (V.isConst()) {
      const uint16_t PointerSize = getTargetRegInfo(TargetArch).PointerSize;
      if (V.Size != 0 && PointerSize != 0 && V.Size < PointerSize &&
          V.Size < 8 && (V.ConstVal >> (V.Size * 8)) != 0 &&
          isReadOnlyTableBase(V.ConstVal)) {
        SawInvalidPointerExpression = true;
        SawTableShapedInvalidExpression = true;
        return BaseProof::Invalid;
      }
      if (auto Base = readOnlyBaseIdentity(V, V.ConstVal)) {
        Bases.insert(*Base);
        bool Symbolized = dataOccurrenceSymbolizes(V);
        SawRawOriginalBase |= !Symbolized;
        SawSymbolizedBase |= Symbolized;
        return BaseProof::HasBase;
      }
      // A small numeric index can collide with a relocatable object's low-VA
      // .text range or with a linked ELF's header PT_LOAD.  It remains a scalar
      // unless getVar itself will relocate it; exact object data is still
      // independent address provenance even when kept raw.
      if (V.Provenance == ConstantAddressProvenance::Scalar)
        return BaseProof::NoBase;
      // A role-neutral architectural-PC seed can be the numeric offset side
      // of a table address (`getpc + GOTOFF`).  It is not a second pointer
      // base when the ordinary emission path proves that this exact
      // occurrence stays numeric.  Keep explicit DataAddress/CodeAddress
      // occurrences, and any Address that a data/code owner would
      // materialize, on the fail-closed path below.
      if (V.Provenance == ConstantAddressProvenance::Address &&
          valueIsStableAddressOffset(V))
        return BaseProof::NoBase;
      if (isAddressProvenance(V.Provenance) ||
          getVarMayRelocateConstant(V.ConstVal, V.Size) ||
          hasObjectDataProvenance(V.ConstVal)) {
        SawInvalidPointerExpression = true;
        // This resolver owns data-table provenance, not executable-relative
        // jump-table anchors.  Keep a code address Invalid so combining it
        // with a proven data base still fails closed, but do not make a
        // code-only address fatal before the later code-table path can own it.
        SawTableShapedInvalidExpression |= isReadOnlyTableBase(V.ConstVal);
        return BaseProof::Invalid;
      }
      return BaseProof::NoBase;
    }
    // The table-base audit owns address provenance; its other operand may be
    // an arbitrarily lowered scalar recurrence.  Reuse the canonical all-path
    // scalar-domain proof before applying pointer-recurrence rules below.  It
    // accepts multiply/shift/flag-expanded induction DAGs, but rejects any
    // feasible data/code-address leaf, mixed PHI, incomplete frame reload, or
    // relocatable constant, so a second base cannot hide behind this shortcut.
    const MedOp *Def = findDef(V);
    const bool IsForwardingWrapper =
        Def && (Def->Opcode == NdOp::COPY ||
                Def->Opcode == NdOp::INT_ZEXT ||
                Def->Opcode == NdOp::INT_SEXT ||
                Def->Opcode == NdOp::SUBBYTES);
    if (!IsForwardingWrapper && valueIsStableAddressOffset(V))
      return BaseProof::NoBase;
    auto Key = std::make_tuple(static_cast<int>(V.Kind), V.Id, V.SSAVer);
    if (!Seen.insert(Key).second)
      return BaseProof::Cycle;

    // A pointer-table LOAD's source address belongs to the table segment, but
    // the value it produces belongs to one of the relocation targets.  Record
    // those targets before generic constant/load folding can mistake the source
    // table base for the resulting pointer's provenance.
    std::set<uint64_t> PointerTargets;
    bool RelativeSymbolized = false;
    if (recoverRelativeDataPointerTargets(V, PointerTargets,
                                          RelativeSymbolized)) {
      for (uint64_t Target : PointerTargets)
        Bases.insert({Target, InvalidVA});
      SawRawOriginalBase |= !RelativeSymbolized;
      SawSymbolizedBase |= RelativeSymbolized;
      return BaseProof::HasBase;
    }
    std::set<DataAddressIdentity> PointerIdentities;
    if (recoverAbsoluteDataPointerLoadIdentities(V, PointerIdentities)) {
      for (const DataAddressIdentity &Identity : PointerIdentities) {
        if (!isOwnedReadOnlyTableBase(Identity.VA, Identity.OwnerVA))
          return BaseProof::NoBase;
        Bases.insert({Identity.VA, Identity.OwnerVA});
      }
      SawSymbolizedBase = true;
      return BaseProof::HasBase;
    }

    bool SawLoad = false;
    bool SawArithmetic = false;
    uint16_t OriginSize = V.Size;
    if (auto C =
            traceTableBaseConst(V, 0, &SawLoad, &OriginSize, &SawArithmetic)) {
      // Do not let whole-expression constant folding hide operand provenance.
      // A plain ADD hidden under COPY/ZEXT must still be proven structurally;
      // only a literal-pool fold or a pure forwarding chain introduces a base.
      std::optional<PureReadOnlyBaseIdentity> PureIdentity;
      if (!SawLoad && !SawArithmetic)
        PureIdentity = pureReadOnlyBaseIdentity(
            V, /*DirectPhiConstantBypassesGetVar=*/false);
      bool FoldPreservesRole =
          SawLoad || (PureIdentity && PureIdentity->VA == *C);
      if (FoldPreservesRole) {
        if (auto Base = readOnlyBaseIdentity(V, *C)) {
          Bases.insert(*Base);
          // A non-constant arm is not automatically relocatable: low-VA COPYs
          // remain original numeric addresses. Classify it with the exact rule
          // getVar itself uses, while literal-pool arithmetic always stays raw.
          bool Symbolized = PureIdentity && PureIdentity->Symbolized;
          SawRawOriginalBase |= !Symbolized;
          SawSymbolizedBase |= Symbolized;
          return BaseProof::HasBase;
        }
      }
    }
    if (const PhiNode *Phi = findPhi(V)) {
      // A recurrent PHI still transports the provenance supplied by every
      // initialization edge.  Skip only an individually proven recurrence;
      // treating the whole PHI as scalar lets AND(p,mask) or ADD(p,other_base)
      // hide a table pointer from the fail-closed outer-expression audit.
      bool Recurrent = phiIsSelfRecurrent(*Phi);
      const bool StructuralCycle = phiHasPureForwardingCycle(*Phi);
      const std::set<ReadOnlyBaseIdentity> BasesBefore = Bases;
      auto markPointerPhi = [&]() {
        SawPhi = true;
        SawNonRecurrentPhi |= !Recurrent;
        if (!EvidencePhi)
          EvidencePhi = Phi;
      };
      bool SawFeasible = false;
      bool SawInitialization = false;
      bool SawBaseArm = false;
      bool SawScalarArm = false;
      bool SawCycleArm = false;
      for (const auto &[Pred, Arg] : Phi->Args) {
        if (!phiIncomingEdgeFeasible(*Phi, Pred))
          continue;
        SawFeasible = true;
        if (Recurrent && phiIncomingIsRecurrent(*Phi, Pred, Arg))
          continue;
        SawInitialization = true;
        if (Arg.isConst()) {
          std::optional<ReadOnlyBaseIdentity> Base =
              readOnlyBaseIdentity(Arg, Arg.ConstVal);
          if (!Base) {
            SawScalarArm = true;
            continue;
          }
          markPointerPhi();
          if (Phi->Output.Size == 0 || Arg.Size == 0 ||
              Phi->Output.Size < Arg.Size) {
            Bases.insert(*Base);
            SawInvalidPointerExpression = true;
            return BaseProof::Invalid;
          }
          Bases.insert(*Base);
          bool Symbolized = dataOccurrenceSymbolizes(
              Arg, /*DirectPhiConstantBypassesGetVar=*/true);
          SawRawOriginalBase |= !Symbolized;
          SawSymbolizedBase |= Symbolized;
          SawBaseArm = true;
          continue;
        }
        BaseProof Arm = walk(Arg, Depth + 1, Seen);
        if (Arm == BaseProof::Invalid)
          return BaseProof::Invalid;
        if (Arm == BaseProof::HasBase) {
          markPointerPhi();
          if (Phi->Output.Size == 0 || Arg.Size == 0 ||
              Phi->Output.Size < Arg.Size) {
            SawInvalidPointerExpression = true;
            return BaseProof::Invalid;
          }
          SawBaseArm = true;
        } else if (Arm == BaseProof::Cycle) {
          SawCycleArm = true;
        } else {
          SawScalarArm = true;
        }
      }
      if (!SawFeasible || !SawInitialization)
        return BaseProof::Invalid;
      if (StructuralCycle && !Recurrent) {
        size_t DistinctComponentBases = 0;
        for (const ReadOnlyBaseIdentity &Base : Bases)
          DistinctComponentBases += BasesBefore.count(Base) == 0;
        if (DistinctComponentBases > 1) {
          markPointerPhi();
          SawInvalidPointerExpression = true;
          return BaseProof::Invalid;
        }
      }
      if (SawBaseArm && SawScalarArm) {
        // A runtime value can legally share a pointer PHI with a fully
        // symbolized static base (for example an incoming caller pointer on
        // one path and &table on another).  Preserve the merge and decide its
        // address model after the complete walk: it is safe only when no raw
        // original-image base remains to be uniformly re-anchored.
        markPointerPhi();
        SawPointerValueMerge = true;
        SawNonBaseValueMerge = true;
        return BaseProof::HasBase;
      }
      if (SawBaseArm)
        return BaseProof::HasBase;
      if (SawCycleArm && SawScalarArm) {
        SawInvalidPointerExpression = true;
        return BaseProof::Invalid;
      }
      return SawCycleArm ? BaseProof::Cycle : BaseProof::NoBase;
    }
    if (!Def)
      return BaseProof::NoBase;
    if (auto Forwarded = pointerPreservingInput(*Def)) {
      // Calling-convention lowering can retain an entry live-in as an exact
      // identity COPY (for example `COPY EDI EDI`).  It has the same opaque
      // scalar provenance as a parameter with no defining op.  Keep this
      // shortcut narrower than MedVar::operator==: a width/arch change or a
      // longer forwarding cycle may still hide independently relocatable
      // pointer provenance and must remain fail-closed.
      if (Def->Opcode == NdOp::COPY && Def->NumInputs == 1 &&
          isExactSameVar(Def->Output, V) && isExactSameVar(*Forwarded, V))
        return BaseProof::NoBase;
      // Pure forwarding changes representation, not provenance complexity.
      // Keep the structural depth budget for branching/arithmetic nodes; Seen
      // already bounds COPY/ZEXT/SUBBYTES cycles and arbitrarily long valid
      // forwarding chains therefore cannot hide a table base.
      return walk(*Forwarded, Depth, Seen);
    }
    switch (Def->Opcode) {
    case NdOp::SELECT: {
      if (Def->NumInputs < 3)
        return BaseProof::NoBase;
      BaseProof T = walk(Def->Inputs[1], Depth + 1, Seen);
      BaseProof F = walk(Def->Inputs[2], Depth + 1, Seen);
      if (!selectPreservesPointerValues(*Def)) {
        if (T != BaseProof::NoBase || F != BaseProof::NoBase)
          SawInvalidPointerExpression = true;
        return T == BaseProof::NoBase && F == BaseProof::NoBase
                   ? BaseProof::NoBase
                   : BaseProof::Invalid;
      }
      if (T == BaseProof::Invalid || F == BaseProof::Invalid) {
        if (T != BaseProof::NoBase || F != BaseProof::NoBase)
          SawInvalidPointerExpression = true;
        return BaseProof::Invalid;
      }
      if (T == F)
        return T;
      if ((T == BaseProof::HasBase && F == BaseProof::Cycle) ||
          (F == BaseProof::HasBase && T == BaseProof::Cycle)) {
        SawPointerValueMerge = true;
        return BaseProof::HasBase;
      }
      if ((T == BaseProof::HasBase && F == BaseProof::NoBase) ||
          (F == BaseProof::HasBase && T == BaseProof::NoBase)) {
        SawPointerValueMerge = true;
        SawNonBaseValueMerge = true;
        return BaseProof::HasBase;
      }
      if (T != BaseProof::NoBase || F != BaseProof::NoBase)
        SawInvalidPointerExpression = true;
      return BaseProof::Invalid;
    }
    case NdOp::INT_OR: {
      if (Def->NumInputs < 2)
        return BaseProof::Invalid;
      // OR never preserves a pointer merely because one operand contains table
      // provenance. It is pointer-valued only when the whole operation is the
      // strict complementary-mask SELECT idiom and both selected value arms are
      // independently proven table pointers. In particular, masking a table PHI
      // on one side and a live scalar on the other must fail closed.
      MedVar Cond, ArmT, ArmF;
      if (isMaskedSelectOr(*Def, Cond, ArmT, ArmF)) {
        BaseProof T = walk(ArmT, Depth + 1, Seen);
        BaseProof F = walk(ArmF, Depth + 1, Seen);
        if ((T == BaseProof::HasBase || T == BaseProof::Cycle) &&
            (F == BaseProof::HasBase || F == BaseProof::Cycle) &&
            (T == BaseProof::HasBase || F == BaseProof::HasBase)) {
          SawPointerValueMerge = true;
          return BaseProof::HasBase;
        }
        if (T == BaseProof::Cycle && F == BaseProof::Cycle)
          return BaseProof::Cycle;
        if (T == BaseProof::NoBase && F == BaseProof::NoBase)
          return BaseProof::NoBase;
        SawInvalidPointerExpression = true;
        return BaseProof::Invalid;
      }
      BaseProof L = walk(Def->Inputs[0], Depth + 1, Seen);
      BaseProof R = walk(Def->Inputs[1], Depth + 1, Seen);
      if (L != BaseProof::NoBase || R != BaseProof::NoBase) {
        SawInvalidPointerExpression = true;
        return BaseProof::Invalid;
      }
      return BaseProof::NoBase;
    }
    case NdOp::INT_ADD: {
      if (Def->NumInputs < 2)
        return BaseProof::Invalid;
      BaseProof L = walk(Def->Inputs[0], Depth + 1, Seen);
      BaseProof R = walk(Def->Inputs[1], Depth + 1, Seen);
      const bool LeftPointer = L == BaseProof::HasBase || L == BaseProof::Cycle;
      const bool RightPointer =
          R == BaseProof::HasBase || R == BaseProof::Cycle;
      if (L == BaseProof::Invalid || R == BaseProof::Invalid ||
          (LeftPointer && RightPointer)) {
        if (L != BaseProof::NoBase || R != BaseProof::NoBase)
          SawInvalidPointerExpression = true;
        markTableShapedInvalidFold(V);
        return BaseProof::Invalid;
      }
      if (LeftPointer || RightPointer) {
        const MedVar &PointerInput =
            LeftPointer ? Def->Inputs[0] : Def->Inputs[1];
        const MedVar &OffsetInput =
            LeftPointer ? Def->Inputs[1] : Def->Inputs[0];
        if (!valueIsStableAddressOffset(OffsetInput)) {
          SawInvalidPointerExpression = true;
          return BaseProof::Invalid;
        }
        if (Def->Output.Size == 0 || PointerInput.Size == 0 ||
            Def->Output.Size < PointerInput.Size) {
          SawInvalidPointerExpression = true;
          markTableShapedInvalidFold(V);
          return BaseProof::Invalid;
        }
        return LeftPointer ? L : R;
      }
      if (auto Result = traceValueVA(V);
          Result && isReadOnlyTableBase(*Result)) {
        SawInvalidPointerExpression = true;
        SawTableShapedInvalidExpression = true;
        return BaseProof::Invalid;
      }
      return BaseProof::NoBase;
    }
    case NdOp::INT_SUB: {
      if (Def->NumInputs < 2)
        return BaseProof::Invalid;
      BaseProof L = walk(Def->Inputs[0], Depth + 1, Seen);
      BaseProof R = walk(Def->Inputs[1], Depth + 1, Seen);
      if (L == BaseProof::Invalid || R == BaseProof::Invalid) {
        if (L != BaseProof::NoBase || R != BaseProof::NoBase)
          SawInvalidPointerExpression = true;
        markTableShapedInvalidFold(V);
        return BaseProof::Invalid;
      }
      if ((L == BaseProof::HasBase || L == BaseProof::Cycle) &&
          R == BaseProof::NoBase) {
        if (!valueIsStableAddressOffset(Def->Inputs[1])) {
          SawInvalidPointerExpression = true;
          return BaseProof::Invalid;
        }
        if (Def->Output.Size == 0 || Def->Inputs[0].Size == 0 ||
            Def->Output.Size < Def->Inputs[0].Size) {
          SawInvalidPointerExpression = true;
          markTableShapedInvalidFold(V);
          return BaseProof::Invalid;
        }
        return L;
      }
      if (L == BaseProof::NoBase && R == BaseProof::NoBase) {
        if (auto Result = traceValueVA(V);
            Result && isReadOnlyTableBase(*Result)) {
          SawInvalidPointerExpression = true;
          SawTableShapedInvalidExpression = true;
          return BaseProof::Invalid;
        } else {
          return BaseProof::NoBase;
        }
      }
      SawInvalidPointerExpression = true;
      markTableShapedInvalidFold(V);
      return BaseProof::Invalid;
    }
    case NdOp::INT_AND:
    case NdOp::INT_XOR:
    case NdOp::INT_MULT:
    case NdOp::INT_DIV:
    case NdOp::INT_SDIV:
    case NdOp::INT_REM:
    case NdOp::INT_SREM:
    case NdOp::INT_RIGHT:
    case NdOp::INT_ASHR: {
      bool SawAddressInput = false;
      for (uint8_t I = 0; I < Def->NumInputs; ++I)
        SawAddressInput |=
            walk(Def->Inputs[I], Depth + 1, Seen) != BaseProof::NoBase;
      if (SawAddressInput) {
        SawInvalidPointerExpression = true;
        return BaseProof::Invalid;
      }
      if (auto Result = traceValueVA(V);
          Result && isReadOnlyTableBase(*Result)) {
        SawInvalidPointerExpression = true;
        SawTableShapedInvalidExpression = true;
        return BaseProof::Invalid;
      }
      return BaseProof::NoBase;
    }
    case NdOp::INT_LEFT: {
      // emitOp materializes direct SHL operands with GetRawInput: a mapped-VA
      // immediate is a bit pattern here, not a relocatable address.  Computed
      // operands still pass through getVar and therefore retain the ordinary
      // provenance audit.
      bool SawAddressInput = false;
      for (uint8_t I = 0; I < Def->NumInputs; ++I)
        if (!Def->Inputs[I].isConst())
          SawAddressInput |=
              walk(Def->Inputs[I], Depth + 1, Seen) != BaseProof::NoBase;
      if (SawAddressInput) {
        SawInvalidPointerExpression = true;
        return BaseProof::Invalid;
      }
      if (auto Result = traceValueVA(V);
          Result && isReadOnlyTableBase(*Result)) {
        SawInvalidPointerExpression = true;
        SawTableShapedInvalidExpression = true;
        return BaseProof::Invalid;
      }
      return BaseProof::NoBase;
    }
    case NdOp::COPY:
    case NdOp::INT_ZEXT:
    case NdOp::INT_SEXT:
    case NdOp::SUBBYTES: {
      if (Def->NumInputs < 1)
        return BaseProof::NoBase;
      BaseProof Input = walk(Def->Inputs[0], Depth + 1, Seen);
      if (Input != BaseProof::NoBase) {
        SawInvalidPointerExpression = true;
        return BaseProof::Invalid;
      }
      return BaseProof::NoBase;
    }
    case NdOp::LOAD: {
      // Stack spill/reload: a register-constrained target (i386 PIC) spills
      // each table base (GOT-relative `lea`) to a frame slot, then cmov-selects
      // the reloads. Use the same all-path reaching-store proof as the indexed
      // resolver so an unrelated or partial store cannot become provenance.
      std::vector<MedVar> Sources;
      const bool CompleteFrameSources =
          collectFrameReloadSources(*Def, Sources);
      if (!CompleteFrameSources || Sources.empty()) {
        // A pointer-width frame reload is an implicit value merge. If its
        // all-path reaching stores cannot be established, it may carry an
        // uninitialized or later-written table pointer. Record that uncertainty
        // so it cannot be treated as the scalar side of a separately proven
        // table base; by itself it is not evidence that this address contains
        // any original-image table VA, so ordinary unresolved memory remains
        // on the generic path.
        const unsigned PointerSize = getTargetRegInfo(TargetArch).PointerSize;
        if (Def->NumInputs >= 1 && Def->Output.Size == PointerSize &&
            varIsFrameDerived(Def->Inputs[0]))
          SawUnprovedFrameReload = true;
        return BaseProof::NoBase;
      }
      const unsigned PointerSize = getTargetRegInfo(TargetArch).PointerSize;
      if (PointerSize != 0 && Def->Output.Size < PointerSize) {
        // A narrow frame reload cannot itself transport a complete native
        // pointer, but its reaching stores may still contain the low fragment
        // of an independently relocatable table address.  Scan value-carrying
        // leaves once before recursing through the full merge audit.  Proven
        // scalar recurrences (including a later-iteration frame cycle) have no
        // address leaf and remain an offset; an address leaf or an incomplete
        // scan keeps the existing fail-closed path below.
        enum class NarrowLeafScan { ScalarOnly, AddressEvidence, Incomplete };
        auto scanNarrowSources = [&]() {
          std::vector<MedVar> Work = Sources;
          SeenSet ScanSeen;
          size_t Budget = 8192;
          while (!Work.empty()) {
            if (Budget-- == 0)
              return NarrowLeafScan::Incomplete;
            MedVar Current = Work.back();
            Work.pop_back();
            if (Current.isConst()) {
              if (Current.Provenance == ConstantAddressProvenance::Scalar)
                continue;
              if (readOnlyBaseIdentity(Current, Current.ConstVal) ||
                  isAddressProvenance(Current.Provenance) ||
                  getVarMayRelocateConstant(Current.ConstVal, Current.Size) ||
                  hasObjectDataProvenance(Current.ConstVal))
                return NarrowLeafScan::AddressEvidence;
              continue;
            }
            auto CurrentKey = std::make_tuple(static_cast<int>(Current.Kind),
                                              Current.Id, Current.SSAVer);
            if (!ScanSeen.insert(CurrentKey).second)
              continue;
            if (const PhiNode *Phi = findPhi(Current)) {
              for (const auto &[Pred, Arg] : Phi->Args)
                if (phiIncomingEdgeFeasible(*Phi, Pred))
                  Work.push_back(Arg);
              continue;
            }
            const MedOp *CurrentDef = findDef(Current);
            if (!CurrentDef)
              continue;
            if (CurrentDef->Opcode == NdOp::LOAD) {
              std::vector<MedVar> ReloadSources;
              if (!collectFrameReloadSources(*CurrentDef, ReloadSources))
                return NarrowLeafScan::Incomplete;
              Work.insert(Work.end(), ReloadSources.begin(),
                          ReloadSources.end());
              continue;
            }
            if (CurrentDef->Opcode == NdOp::SELECT &&
                CurrentDef->NumInputs >= 3) {
              Work.push_back(CurrentDef->Inputs[1]);
              Work.push_back(CurrentDef->Inputs[2]);
              continue;
            }
            switch (CurrentDef->Opcode) {
            case NdOp::COPY:
            case NdOp::INT_ZEXT:
            case NdOp::INT_SEXT:
            case NdOp::SUBBYTES:
            case NdOp::INT_ADD:
            case NdOp::INT_SUB:
            case NdOp::INT_AND:
            case NdOp::INT_OR:
            case NdOp::INT_XOR:
            case NdOp::INT_MULT:
            case NdOp::INT_DIV:
            case NdOp::INT_SDIV:
            case NdOp::INT_REM:
            case NdOp::INT_SREM:
            case NdOp::INT_LEFT:
            case NdOp::INT_RIGHT:
            case NdOp::INT_ASHR:
            case NdOp::INT_NEGATE:
            case NdOp::INT_NEG2:
              for (uint8_t I = 0; I < CurrentDef->NumInputs; ++I)
                Work.push_back(CurrentDef->Inputs[I]);
              break;
            default:
              break;
            }
          }
          return NarrowLeafScan::ScalarOnly;
        };
        const NarrowLeafScan Scan = scanNarrowSources();
        if (Scan == NarrowLeafScan::ScalarOnly)
          return BaseProof::NoBase;
        if (Scan == NarrowLeafScan::Incomplete) {
          SawInvalidPointerExpression = true;
          return BaseProof::Invalid;
        }
      }
      bool SawBaseSource = false;
      bool SawScalarSource = false;
      bool SawInvalidSource = false;
      bool SawCycleSource = false;
      const std::set<ReadOnlyBaseIdentity> BasesBefore = Bases;
      for (const MedVar &Source : Sources) {
        BaseProof SourceProof = walk(Source, Depth + 1, Seen);
        SawBaseSource |= SourceProof == BaseProof::HasBase;
        SawScalarSource |= SourceProof == BaseProof::NoBase;
        SawInvalidSource |= SourceProof == BaseProof::Invalid;
        SawCycleSource |= SourceProof == BaseProof::Cycle;
      }
      if (SawCycleSource && SawScalarSource) {
        SawInvalidPointerExpression = true;
        return BaseProof::Invalid;
      }
      if (SawBaseSource && SawScalarSource) {
        SawPointerValueMerge = true;
        SawNonBaseValueMerge = true;
      }
      size_t DistinctReloadBases = 0;
      for (const ReadOnlyBaseIdentity &Base : Bases)
        DistinctReloadBases += BasesBefore.count(Base) == 0;
      // A frame slot is an implicit value merge.  Multiple independently
      // reaching raw table bases need the same all-arms owner as an explicit
      // PHI/SELECT.  collectFrameReloadSources proved every structural path,
      // so the complete reload value can be relocated uniformly when all bases
      // share one embedded run; the run check below still fails closed across
      // segments.
      SawPointerValueMerge |= DistinctReloadBases > 1;
      if (SawInvalidSource)
        return BaseProof::Invalid;
      if (SawBaseSource)
        return BaseProof::HasBase;
      return SawCycleSource ? BaseProof::Cycle : BaseProof::NoBase;
    }
    default:
      return BaseProof::NoBase;
    }
  };
  BaseProof Proof = walk(BaseVar, 0, {});
  // An invalid relocatable leaf can be hidden by constant arithmetic whose
  // old-image value happens to fold back into a read-only table.  The child
  // walk intentionally rejects that leaf before recording a raw table base;
  // classify the complete expression as table-shaped here so it cannot fall
  // through to a narrower resolver and be emitted with a stale link-time
  // delta.  Keeping this at the root covers ADD/SUB and destructive bitwise
  // forms uniformly.
  if (Proof == BaseProof::Invalid && SawInvalidPointerExpression &&
      Bases.empty() && !SawTableShapedInvalidExpression)
    if (auto Result = traceValueVA(BaseVar);
        Result && isReadOnlyTableBase(*Result))
      SawTableShapedInvalidExpression = true;
  const bool NeedsPredicatedPathProof =
      Proof == BaseProof::Invalid ||
      (Proof == BaseProof::HasBase && SawNonBaseValueMerge &&
       SawRawOriginalBase && !SawSymbolizedBase);
  if (NeedsPredicatedPathProof && !AuditIncomplete &&
      !SawTableShapedInvalidExpression && PredicatedAccessGuard) {
    SawPredicatedLiteralLoad = false;
    if (auto PredicatedBase = predicatedTableBase(BaseVar, 0, {});
        PredicatedBase && SawPredicatedLiteralLoad) {
      // The path proof selected one concrete literal-pool base and excluded
      // every scalar PHI/SELECT arm at this LOAD. Anchor only that reachable
      // base; retaining bases observed on unreachable arms would reintroduce
      // the all-path ambiguity this proof just discharged.
      Bases.clear();
      Bases.insert({*PredicatedBase, InvalidVA});
      SawInvalidPointerExpression = false;
      SawNonBaseValueMerge = false;
      SawRawOriginalBase = true;
      SawSymbolizedBase = false;
      Proof = BaseProof::HasBase;
    }
  }
  auto failAmbiguousAddress = [&]() {
    if (SawAmbiguous)
      *SawAmbiguous = true;
    if (!FailClosed)
      return;
    if (EvidencePhi) {
      failAmbiguousDataPointerPhi(*EvidencePhi);
      return;
    }
    if (!FatalDataPointerResolution)
      syncError() << "med_llvm_emitter: ambiguous reachable read-only table-"
                     "base address "
                  << BaseVar.display() << " in " << CurMedFunc->Name
                  << "; refusing stale-address fallback\n";
    FatalDataPointerResolution = true;
  };
  if (AuditIncomplete) {
    failAmbiguousAddress();
    return nullptr;
  }
  if (Proof == BaseProof::Invalid && SawInvalidPointerExpression &&
      (!Bases.empty() || SawTableShapedInvalidExpression)) {
    failAmbiguousAddress();
    return nullptr;
  }
  if (Proof == BaseProof::HasBase && SawUnprovedFrameReload) {
    failAmbiguousAddress();
    return nullptr;
  }
  if (Proof == BaseProof::HasBase && SawRawOriginalBase && SawSymbolizedBase) {
    failAmbiguousAddress();
    return nullptr;
  }
  if (Proof == BaseProof::HasBase && SawNonBaseValueMerge &&
      SawRawOriginalBase) {
    failAmbiguousAddress();
    return nullptr;
  }
  if (Proof != BaseProof::HasBase)
    return nullptr;

  // A pure recurrent PHI still belongs to the induction resolver: it owns the
  // recurrence step as well as the initialization base.  Every other complete
  // proof is owned here, including a deeply nested single-base expression.
  // Deferring the latter to the shallower indexed recognizer makes address
  // safety depend on expression depth even though this audit has already
  // inspected every reachable operand.
  if (SawPhi && !SawNonRecurrentPhi && !SawPointerValueMerge)
    return nullptr;

  // PHI edge constants bypass getVar and still carry original-image VAs;
  // computed arms have already passed through normal LLVM emission and carry
  // relocatable ptrtoint values. Mixing the two representations cannot be
  // repaired by either a raw-VA anchor or a direct pointer use.
  if (SawRawOriginalBase && SawSymbolizedBase) {
    failAmbiguousAddress();
    return nullptr;
  }

  // The audit above is the authority for all reachable pointer-valued arms.
  // Once it proves that the expression is not a pointer merge and did not
  // itself encounter a pointer-valued PHI, let the one-base indexed owner
  // preserve the exact additive/subtractive index role and canonical `tblptr`
  // representation.  A PHI can have only one reachable/distinct base after
  // edge pruning, but it still owns the emitted current-pointer model; handing
  // that shape to the indexed resolver would either select one convenient arm
  // or re-anchor an already computed address.  Scalar index PHIs do not set
  // SawPhi because the scalar-domain proof consumes them before this pointer
  // audit, so ordinary `base +/- scalar_phi` remains eligible.
  if (!SawPointerValueMerge && !SawPhi) {
    if (llvm::Value *Indexed =
            tryResolveIndexedGlobalPtr(AddrVar, SizeHint, FailClosed, Builder))
      return Indexed;
    if (FatalDataPointerResolution || FatalCodePointerResolution)
      return nullptr;
  }

  if (StoredBasesFor != CurMedFunc) {
    StoredBasesFor = CurMedFunc;
    StoredConstBases.clear();
    for (const auto &Blk : CurMedFunc->Blocks)
      for (const auto &Op : Blk.Ops)
        if (Op.Opcode == NdOp::STORE &&
            Op.MemoryAddressSpace == NdMemoryAddressSpace::Default &&
            Op.NumInputs >= 1)
          if (auto SB = indexedConstBase(Op.Inputs[0]))
            StoredConstBases.insert(*SB);
  }
  for (const ReadOnlyBaseIdentity &B : Bases)
    if (StoredConstBases.count(B.VA)) {
      return nullptr;
    }

  unsigned Bits = AddrVar.Size > 0 ? AddrVar.Size * 8 : 64;
  auto *Ty = llvm::IntegerType::get(*Ctx, Bits);
  llvm::Value *Cur = getVar(AddrVar, Builder);
  if (!Cur)
    return nullptr;

  // Uniform computed/symbolized arms already contain recompiled addresses.
  // Re-anchoring them against an original VA would add the global base twice.
  if (SawSymbolizedBase && !SawRawOriginalBase) {
    if (Cur->getType()->isPointerTy())
      return Cur;
    if (Cur->getType() != Ty)
      Cur = Builder.CreateZExtOrTrunc(Cur, Ty);
    return Builder.CreateIntToPtr(Cur, llvm::PointerType::get(*Ctx, 0),
                                  "selmrgrawptr");
  }

  // All raw bases must share one read-only segment so a single materialized
  // global covers every table the select can reach. Mach-O keeps non-code data
  // such as __TEXT,__cstring inside an executable segment, while Mach-O/ELF
  // RELRO and COFF .rdata can carry relocated pointer slots. Preserve the same
  // representation owner as direct global-data resolution: executable bytes
  // use their bounded segment mirror, relocation-backed pointer tables use the
  // pointer mirror, and only pointer-free immutable data may use a raw rodata
  // embedding.
  const Segment *Seg = nullptr;
  std::optional<uint64_t> ExactOwner;
  bool SawUnownedBase = false;
  for (const ReadOnlyBaseIdentity &B : Bases) {
    const uint64_t LookupVA = B.OwnerVA == InvalidVA ? B.VA : B.OwnerVA;
    const Segment *BaseSeg = Img->getSegmentFor(LookupVA);
    if (!BaseSeg || (Seg && Seg != BaseSeg)) {
      failAmbiguousAddress();
      return nullptr;
    }
    Seg = BaseSeg;
    if (B.OwnerVA == InvalidVA) {
      SawUnownedBase = true;
      continue;
    }
    if (ExactOwner && *ExactOwner != B.OwnerVA) {
      failAmbiguousAddress();
      return nullptr;
    }
    ExactOwner = B.OwnerVA;
  }
  // An exact relocation occurrence and a value-only legacy candidate cannot
  // jointly prove one raw address model.  The same numeric VA may denote the
  // one-past end of one object and the beginning of another.
  if ((ExactOwner && SawUnownedBase) || !Seg) {
    failAmbiguousAddress();
    return nullptr;
  }

  // Anchor the whole access uniformly: the merged value still carries the
  // original VA of whichever table was selected, so
  // `@run + (addr - run_start)` lands on the correct element of the rebuilt
  // canonical run for any reachable table + index.
  auto [G, Anchor] = materializeReadOnlyDataRun(Seg);
  if (!G) {
    if (!FatalCodePointerResolution && !FatalDataPointerResolution)
      failAmbiguousAddress();
    else if (SawAmbiguous)
      *SawAmbiguous = true;
    return nullptr;
  }

  if (Cur->getType()->isPointerTy())
    Cur = Builder.CreatePtrToInt(Cur, Ty);
  else if (Cur->getType() != Ty)
    Cur = Builder.CreateZExtOrTrunc(Cur, Ty);
  llvm::Value *Off =
      Builder.CreateSub(Cur, llvm::ConstantInt::get(Ty, Anchor), "selmrgoff");
  return Builder.CreateGEP(llvm::Type::getInt8Ty(*Ctx), G, Off, "selmrgptr");
}

llvm::Value *MedLLVMEmitter::tryResolveLiteralPoolBase(
    const MedVar &AddrVar, uint16_t SizeHint, llvm::IRBuilder<> & /*Builder*/) {
  if (!CurMedFunc || !Img || AddrVar.isConst())
    return nullptr;

  // The whole address must fold to a constant VA *through* a literal-pool LOAD
  // (the ARM `ldr rN,[pc]; add rN,pc` address-of).  SawLoad distinguishes a
  // genuine PC-relative address-of from a plain computed constant that merely
  // equals a data VA, exactly as the function-pointer resolver does.
  bool SawLoad = false;
  auto VA = traceTableBaseConst(AddrVar, 0, &SawLoad);
  if (!VA || !SawLoad)
    return nullptr;

  // Redirect only into a genuine read-only data constant (a `.rodata` aggregate
  // initializer); a code VA is a function pointer and an executable literal
  // pool is left to the code-pointer path.
  // Never redirect a load aliasing an indexed store into the same base (a
  // read-write table the function mutates); the read-only gate above already
  // excludes it, but keep the symmetry with tryResolveLiteralPoolTable.
  if (StoredBasesFor != CurMedFunc) {
    StoredBasesFor = CurMedFunc;
    StoredConstBases.clear();
    for (const auto &Blk : CurMedFunc->Blocks)
      for (const auto &Op : Blk.Ops)
        if (Op.Opcode == NdOp::STORE &&
            Op.MemoryAddressSpace == NdMemoryAddressSpace::Default &&
            Op.NumInputs >= 1)
          if (auto SB = indexedConstBase(Op.Inputs[0]))
            StoredConstBases.insert(*SB);
  }
  if (StoredConstBases.count(*VA))
    return nullptr;

  auto *G = tryResolveReadOnlyDataOccurrence(AddrVar, *VA, SizeHint);
  if (!G)
    return nullptr;
  if (auto *GV = llvm::dyn_cast<llvm::GlobalVariable>(G->stripPointerCasts()))
    if (!GV->isConstant())
      return nullptr;
  return G;
}

} // namespace neverd
