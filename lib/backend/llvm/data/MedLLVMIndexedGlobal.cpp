//===- MedLLVMIndexedGlobal.cpp - Indexed/induction globals ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Runtime-indexed and induction-pointer global resolution for
/// MedLLVMEmitter.
///
//===----------------------------------------------------------------------===//

#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/support/Diagnostic.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Instructions.h"

#include <functional>
#include <map>
#include <optional>
#include <set>
#include <tuple>
#include <vector>

namespace neverd {

llvm::Value *
MedLLVMEmitter::tryResolveInductionGlobalPtr(const MedVar &AddrVar,
                                             uint16_t SizeHint, bool FailClosed,
                                             llvm::IRBuilder<> &Builder) {
  if (!CurMedFunc || !Img || AddrVar.isConst())
    return nullptr;

  const uint16_t PointerSize = getTargetRegInfo(TargetArch).PointerSize;
  auto findPhi = [&](const MedVar &V) { return lookupPhi(V); };
  auto findDef = [&](const MedVar &V) { return lookupDef(V); };

  // The access address `EA` is the induction pointer plus a displacement
  // (`INT_ADD(p, disp)`); walk INT_ADD/INT_SUB/COPY back to the defining PHI so
  // `tab[i].field` resolves.  The displacement may be a constant
  // (`tab[i].field`) or itself a runtime index (`base_phi + (i%n)*stride`, the
  // rolled-loop value table): in either case getVar(EA) below captures the full
  // address, so the `Cur - Base` offset stays exact — only reaching the PHI
  // matters here.  When both addends are runtime the loop-carried base is the
  // first operand. Collect every induction PHI reachable from the access
  // address through COPY / INT_ADD / INT_SUB chains.  Either operand of an
  // ADD/SUB can carry the pointer: the strength-reduced `tab[(i+k)%n]` modulo
  // walk forms `base+running_index - n*(idx/n)`, and clang may emit the
  // `n*(idx/n)` subtrahend as the first ADD operand (x86) or the pointer first
  // (AArch64), so both sides are explored.  Each candidate is validated below
  // by an incoming rodata base; a non-induction PHI (e.g. a loop counter)
  // simply fails that check, so over-collecting is safe.
  auto constInRodata = [&](uint64_t C) {
    return isMaterializableReadOnlyDataAddress(C);
  };
  auto failAmbiguousAddress = [&]() {
    if (!FailClosed)
      return;
    if (!FatalDataPointerResolution)
      syncError() << "med_llvm_emitter: ambiguous reachable read-only table-"
                     "base address "
                  << AddrVar.display() << " in " << CurMedFunc->Name
                  << "; refusing stale-address fallback\n";
    FatalDataPointerResolution = true;
  };
  auto failAmbiguousPhi = [&](const PhiNode &Phi) {
    if (FailClosed)
      failAmbiguousDataPointerPhi(Phi);
  };
  std::vector<const PhiNode *> Candidates;
  uint64_t DagRodataBase = 0;
  bool HaveDagRodata = false;
  // SELECT-merged base candidates without a PHI (the unrolled `p = cond ? &W :
  // p+1` reset on ARM32, where the loop-invariant literal-pool base is carried
  // through SELECT, not a PHI).  Their bases are recovered by the literal-pool
  // / indexed detectors below when no PHI candidate yields a base.
  std::vector<std::pair<MedVar, MedVar>> SelectBasePairs;
  bool SawSelect = false;
  bool SawInvalidTableBlend = false;
  {
    std::vector<MedVar> Work{AddrVar};
    std::set<std::tuple<int, int, int>> Seen;
    int Budget = 256;
    while (!Work.empty() && Budget-- > 0) {
      MedVar Cur = Work.back();
      Work.pop_back();
      if (Cur.isConst())
        continue;
      if (!Seen.insert({(int)Cur.Kind, Cur.Id, Cur.SSAVer}).second)
        continue;
      if (const PhiNode *P = findPhi(Cur)) {
        Candidates.push_back(P);
        // Also walk the PHI's incoming values: a pointer PHI's reset arm may
        // fold its rodata base to an inline VA constant (the string-walk
        // wrap-around `p = phi(p+1, &W)`), which the DagRodata fallback below
        // anchors when the per-arg base detectors miss the bare-VA form.
        for (const auto &[Pred, Arg] : P->Args)
          if (phiIncomingEdgeFeasible(*P, Pred))
            Work.push_back(Arg);
        continue;
      }
      const MedOp *Def = findDef(Cur);
      if (!Def)
        continue;
      // Descend through an operation only when it preserves the complete
      // unsigned pointer value; then inspect pointer-valued SELECT arms.
      if (auto Forwarded = pointerPreservingInput(*Def))
        Work.push_back(*Forwarded);
      else if ((Def->Opcode == NdOp::INT_ADD || Def->Opcode == NdOp::INT_SUB) &&
               Def->NumInputs >= 2) {
        // A loop-invariant literal-pool base (`load(@pool) + PC_const`, the
        // ARM32 `ldr rN,[pc]; add rN,pc` idiom) anchors a forward/backward
        // array walk whose varying offset is a separate induction term — the
        // address has no pointer PHI, only an offset PHI whose arms expose no
        // rodata base.  The bare-VA detectors miss it because the base folds
        // through a LOAD, so recover it here; the @run anchoring below then
        // redirects via the address's own original VA (e.g. revwalk's
        // `&bc[last] - 2*i`).
        if (!HaveDagRodata) {
          bool SawLoad = false;
          if (auto C = traceTableBaseConst(Cur, 0, &SawLoad);
              C && SawLoad && constInRodata(*C)) {
            DagRodataBase = *C;
            HaveDagRodata = true;
          }
        }
        Work.push_back(Def->Inputs[0]);
        Work.push_back(Def->Inputs[1]);
      } else if (selectPreservesPointerValues(*Def)) {
        SawSelect = true;
        SelectBasePairs.emplace_back(Def->Inputs[1], Def->Inputs[2]);
        Work.push_back(Def->Inputs[1]);
        Work.push_back(Def->Inputs[2]);
      } else if (Def->Opcode == NdOp::SELECT) {
        // A truncating/mixed-width SELECT may contain rodata-looking arms, but
        // it does not transport their complete pointer values.
        for (uint8_t I = 1; I < Def->NumInputs && I < 3; ++I) {
          bool SawLoad = false;
          bool SawArithmetic = false;
          auto C = traceTableBaseConst(Def->Inputs[I], 0, &SawLoad, nullptr,
                                       &SawArithmetic);
          if (C && (SawLoad || !SawArithmetic) && constInRodata(*C))
            SawInvalidTableBlend = true;
        }
      } else if (Def->Opcode == NdOp::INT_OR) {
        // x86 lowers a pointer wrap-reset to the branchless masked-select idiom
        // `OR(AND(x, m), AND(y, ~m))`; its value arms carry the induction base
        // and the advanced pointer, so descend through them like a SELECT.
        MedVar Cond, ArmT, ArmF;
        if (isMaskedSelectOr(*Def, Cond, ArmT, ArmF)) {
          SawSelect = true;
          SelectBasePairs.emplace_back(ArmT, ArmF);
          Work.push_back(ArmT);
          Work.push_back(ArmF);
        } else {
          // A blend-shaped address with a rodata value arm is still dangerous
          // when the strict SELECT proof rejects its boolean/width semantics.
          // Do not let it fall through to a stale absolute LOAD merely because
          // the malformed matcher no longer grants provenance.
          const MedOp *Left = findDef(Def->Inputs[0]);
          const MedOp *Right = findDef(Def->Inputs[1]);
          if (Left && Right && Left->Opcode == NdOp::INT_AND &&
              Right->Opcode == NdOp::INT_AND && Left->NumInputs >= 2 &&
              Right->NumInputs >= 2) {
            for (const MedOp *And : {Left, Right})
              for (uint8_t I = 0; I < 2; ++I) {
                bool SawLoad = false;
                bool SawArithmetic = false;
                auto C = traceTableBaseConst(And->Inputs[I], 0, &SawLoad,
                                             nullptr, &SawArithmetic);
                if (C && (SawLoad || !SawArithmetic) && constInRodata(*C))
                  SawInvalidTableBlend = true;
              }
          }
        }
      } else if (Def->Opcode == NdOp::LOAD && Def->NumInputs >= 1) {
        // Stack spill/reload: a register-constrained target (ARM32) spills the
        // loop-invariant literal-pool base (`ldr[pc]; add pc`) to a frame slot
        // and reloads it inside the neighbourhood walk (clang's 3x3 stencil).
        // The walk would otherwise stop at the reload. Follow only the exact
        // sources proven to reach it on every structural CFG path; scanning all
        // same-slot stores admits stores after the load and bypassed stores as
        // false table provenance.
        std::vector<MedVar> Sources;
        if (collectFrameReloadSources(*Def, Sources))
          Work.insert(Work.end(), Sources.begin(), Sources.end());
      }
    }
  }

  if (SawInvalidTableBlend) {
    failAmbiguousAddress();
    return nullptr;
  }

  // A bare (or forwarding-cast wrapped) non-recursive PHI has no outer
  // INT_ADD/INT_SUB for tryResolveIndexedGlobalPtr to validate. Require its
  // all-feasible-arm proof here before the induction fallback may inspect any
  // one arm. Multi-base direct PHIs are handled by the select-merge resolver,
  // which runs before this function.
  const PhiNode *DirectAddressPhi = nullptr;
  {
    MedVar Cur = AddrVar;
    for (int Depth = 0; Depth < 8; ++Depth) {
      if (const PhiNode *Phi = findPhi(Cur)) {
        DirectAddressPhi = Phi;
        break;
      }
      const MedOp *Def = findDef(Cur);
      if (!Def)
        break;
      auto Forwarded = pointerPreservingInput(*Def);
      if (!Forwarded)
        break;
      Cur = *Forwarded;
    }
  }
  // Audit every non-recursive PHI reached by the address DAG, not only a bare
  // top-level PHI. A SELECT or arithmetic wrapper must not hide a raw table arm
  // paired with a live scalar from the all-feasible-arm proof.
  for (const PhiNode *Phi : Candidates) {
    if (phiIsSelfRecurrent(*Phi))
      continue;
    uint64_t AuditedBase = 0;
    bool HaveAuditedBase = false;
    bool SawAmbiguousPhi = false;
    std::vector<MedVar> AuditedTerms;
    bool Proven = collectIndexedGlobalBase(
        Phi->Output, AuditedBase, HaveAuditedBase, AuditedTerms, /*Depth=*/0,
        FailClosed, &SawAmbiguousPhi);
    if (FatalDataPointerResolution)
      return nullptr;
    // Proof rejection and module-fatal policy are deliberately independent.
    // A speculative integer argument must remain non-fatal, but an ambiguous
    // nested PHI must still poison this induction candidate rather than being
    // skipped while a sibling recurrent PHI supplies the only accepted base.
    if (SawAmbiguousPhi)
      return nullptr;
    if (Phi == DirectAddressPhi && (!Proven || !HaveAuditedBase))
      return nullptr;
  }

  if (Candidates.empty() && !HaveDagRodata && !SawSelect)
    return nullptr;

  // A NON-RECURRENT PHI incoming value loaded from a rebuilt data-pointer
  // table already carries a resolved `ptrtoint(@global)` pointer, not a raw VA
  // to anchor. This is the 32-bit switch-returning-string shape, where the
  // dispatch merges the default string pointer and absolute `.data.rel.ro`
  // table loads through one PHI; bail and let that direct merge use the
  // resolved pointer. A recurrent PHI must continue into recoverBase below:
  // its step arithmetic needs the explicit Symbolized address model or the
  // later iterations fall back to stale integer addresses.
  if (Img && (!Img->DataPtrRelocSlots.empty() ||
              !EffectiveImportStorageSlots.empty() ||
              !ConflictingImportStorageSlots.empty() ||
              !Img->RuntimeCallablePointerSlots.empty())) {
    auto loadsFromDataPtrTable = [&](const MedVar &Start) {
      MedVar Cur = Start;
      for (int D = 0; D < 8; ++D) {
        const MedOp *Def = findDef(Cur);
        if (!Def)
          return false;
        if (auto Forwarded = pointerPreservingInput(*Def)) {
          Cur = *Forwarded;
          continue;
        }
        if (Def->Opcode != NdOp::LOAD || Def->NumInputs < 1)
          return false;
        uint64_t LB = 0;
        bool HaveLB = false;
        std::vector<MedVar> LIdx;
        if (!collectIndexedGlobalBase(Def->Inputs[0], LB, HaveLB, LIdx) ||
            !HaveLB) {
          LB = 0;
          HaveLB = false;
          LIdx.clear();
          collectLiteralPoolBase(Def->Inputs[0], LB, HaveLB, LIdx);
        }
        if (!HaveLB || LB == 0)
          return false;
        const Segment *LSeg = Img->getSegmentFor(LB);
        if (!LSeg)
          return false;
        for (uint64_t S : Img->DataPtrRelocSlots)
          if (S >= LSeg->VA && S < LSeg->VA + LSeg->Data.size())
            return true;
        for (const auto &[S, Binding] : EffectiveImportStorageSlots) {
          (void)Binding;
          if (S >= LSeg->VA && S < LSeg->VA + LSeg->Data.size())
            return true;
        }
        for (va_t S : ConflictingImportStorageSlots)
          if (S >= LSeg->VA && S < LSeg->VA + LSeg->Data.size())
            return true;
        for (const RuntimeCallablePointerSlot &Slot :
             Img->RuntimeCallablePointerSlots)
          if (Slot.SlotVA >= LSeg->VA &&
              Slot.SlotVA < LSeg->VA + LSeg->Data.size())
            return true;
        return false;
      }
      return false;
    };
    for (const PhiNode *Phi : Candidates) {
      if (phiIsSelfRecurrent(*Phi))
        continue;
      for (const auto &[Pred, Arg] : Phi->Args)
        if (phiIncomingEdgeFeasible(*Phi, Pred) && loadsFromDataPtrTable(Arg))
          return nullptr;
    }
  }

  // Every feasible initialization value must expose a compatible base inside
  // a read-only segment, reached
  // either through a literal-pool / rip-relative LOAD or as a bare constant
  // address (a `lea rip`/`adrp+add` materialization of a .rodata table base
  // folded to its VA).  This runs only for a LOAD address that walks back to a
  // PHI, so a bare rodata-VA constant here is a genuine table pointer, never a
  // plain integer that merely equals a rodata VA (e.g. a loop bound) — a loop
  // counter PHI is not a load address and never reaches this resolver.  A
  // frame-derived base is skipped so a stack-array walk is left absolute.  The
  // init is either the bare base (`&tab`, unrolled loop) or the base already
  // advanced by a runtime offset (`&tab + i*stride`, when clang pre-scales the
  // first iteration); collectLiteralPoolBase peels the runtime addends off the
  // latter, and the `Cur - Base` offset below still recovers the exact element.
  uint64_t Base = 0;
  bool HaveBase = false;
  // The resolver must follow the address model actually emitted for every
  // initialization arm. A direct PHI constant bypasses getVar and is raw, but
  // a non-constant COPY/ZEXT may still be raw (low VA or narrow source) or may
  // already contain ptrtoint(@global). Syntax alone cannot distinguish them.
  auto baseInRodata = [&](uint64_t B) {
    return isMaterializableReadOnlyDataAddress(B);
  };
  enum class AddressModel { Raw, Symbolized };
  std::optional<AddressModel> BaseAddressModel;
  struct RecoveredPhiBase {
    std::set<uint64_t> VAs;
    AddressModel Model = AddressModel::Raw;
  };

  // For an indexed init, locate the exact constant leaf that owns the proven
  // base and classify it with getVar's shared predicate at that leaf's width.
  // Constants copied directly by a nested PHI edge bypass getVar; literal-pool
  // arithmetic likewise carries original-image numeric values. Any mixture is
  // ambiguous rather than an invitation to pick whichever base was seen first.
  auto classifyIndexedBaseModel =
      [&](const MedVar &Start,
          uint64_t ExpectedBase) -> std::optional<AddressModel> {
    using Item = std::pair<MedVar, bool>; // value, bypasses getVar
    std::vector<Item> Work{{Start, Start.isConst()}};
    std::set<std::tuple<int, int, int, bool>> Seen;
    bool SawRaw = false;
    bool SawSymbolized = false;
    int Budget = 256;
    while (!Work.empty() && Budget-- > 0) {
      auto [Cur, BypassesGetVar] = Work.back();
      Work.pop_back();
      if (Cur.isConst()) {
        bool AddressBearing = Cur.ConstVal == ExpectedBase ||
                              hasObjectDataProvenance(Cur.ConstVal);
        if (!AddressBearing)
          continue;
        bool Symbolized = dataOccurrenceSymbolizes(Cur, BypassesGetVar);
        SawRaw |= !Symbolized;
        SawSymbolized |= Symbolized;
        continue;
      }
      auto Key = std::make_tuple(static_cast<int>(Cur.Kind), Cur.Id, Cur.SSAVer,
                                 BypassesGetVar);
      if (!Seen.insert(Key).second)
        continue;
      if (const PhiNode *Nested = findPhi(Cur)) {
        for (const auto &[Pred, Arg] : Nested->Args)
          if (phiIncomingEdgeFeasible(*Nested, Pred))
            Work.emplace_back(Arg, Arg.isConst());
        continue;
      }
      const MedOp *Def = findDef(Cur);
      if (!Def)
        continue;
      if (auto Forwarded = pointerPreservingInput(*Def)) {
        Work.emplace_back(*Forwarded, false);
        continue;
      }
      if (Def->Opcode == NdOp::LOAD) {
        std::set<uint64_t> PointerTargets;
        if (recoverAbsoluteDataPointerLoadTargets(Cur, PointerTargets)) {
          SawSymbolized = true;
          continue;
        }
        bool SawLoad = false;
        (void)traceTableBaseConst(Cur, 0, &SawLoad);
        if (SawLoad) {
          SawRaw = true;
          continue;
        }
        std::vector<MedVar> Sources;
        if (collectFrameReloadSources(*Def, Sources))
          for (const MedVar &Source : Sources)
            Work.emplace_back(Source, false);
        continue;
      }
      switch (Def->Opcode) {
      case NdOp::INT_ADD:
      case NdOp::INT_SUB:
      case NdOp::INT_AND:
        if (Def->NumInputs >= 2) {
          Work.emplace_back(Def->Inputs[0], false);
          Work.emplace_back(Def->Inputs[1], false);
        }
        break;
      case NdOp::INT_OR: {
        MedVar Cond, ArmT, ArmF;
        if (isMaskedSelectOr(*Def, Cond, ArmT, ArmF)) {
          Work.emplace_back(ArmT, false);
          Work.emplace_back(ArmF, false);
        } else if (Def->NumInputs >= 2) {
          Work.emplace_back(Def->Inputs[0], false);
          Work.emplace_back(Def->Inputs[1], false);
        }
        break;
      }
      case NdOp::SELECT:
        if (Def->NumInputs >= 3) {
          Work.emplace_back(Def->Inputs[1], false);
          Work.emplace_back(Def->Inputs[2], false);
        }
        break;
      default:
        break;
      }
    }
    if (SawRaw == SawSymbolized)
      return std::nullopt;
    return SawSymbolized ? AddressModel::Symbolized : AddressModel::Raw;
  };

  auto recoverBase = [&](const MedVar &Arg, bool DirectConstantBypassesGetVar)
      -> std::optional<RecoveredPhiBase> {
    if (varIsFrameDerived(Arg))
      return std::nullopt;

    // A relocation-table LOAD produces the target pointer, not the numeric
    // contents/source address of its slot.  Recover this before the generic
    // literal-pool fold: absolute slots are rebuilt through a pointer mirror
    // and are therefore already symbolized; a PC-relative table follows the
    // exact base model reported by its dedicated recognizer.  Preserve every
    // possible target so multi-arm/run validation never depends on an
    // arbitrary first table entry.
    std::set<uint64_t> PointerTargets;
    if (recoverAbsoluteDataPointerLoadTargets(Arg, PointerTargets))
      return RecoveredPhiBase{std::move(PointerTargets),
                              AddressModel::Symbolized};
    bool RelativeSymbolized = false;
    if (recoverRelativeDataPointerTargets(Arg, PointerTargets,
                                          RelativeSymbolized))
      return RecoveredPhiBase{std::move(PointerTargets),
                              RelativeSymbolized ? AddressModel::Symbolized
                                                 : AddressModel::Raw};

    bool SawLoad = false;
    bool SawArithmetic = false;
    uint16_t OriginSize = Arg.Size;
    if (auto C =
            traceTableBaseConst(Arg, 0, &SawLoad, &OriginSize, &SawArithmetic);
        C && baseInRodata(*C) && (SawLoad || !SawArithmetic)) {
      AddressModel Model = AddressModel::Raw;
      if (!SawLoad && !SawArithmetic) {
        // A pure forwarding chain is an address identity only when it
        // preserves every pointer bit.  In particular, COPY/ZEXT from a
        // narrow constant cannot authorize the masked numeric value produced
        // by traceTableBaseConst when the original mapped VA did not fit in
        // that leaf.  Keep this decision shared with the other read-only base
        // consumers instead of re-inferring provenance from the folded value.
        auto Identity =
            pureReadOnlyBaseIdentity(Arg, DirectConstantBypassesGetVar);
        if (!Identity || Identity->VA != *C)
          return std::nullopt;
        Model =
            Identity->Symbolized ? AddressModel::Symbolized : AddressModel::Raw;
      } else if (SawArithmetic) {
        // A folded literal load proves only that one arithmetic leaf is a raw
        // numeric displacement.  Audit every sibling address-bearing leaf as
        // well: `load(pool) + ptrtoint(@table)` is a mixed model even when the
        // whole expression folds back to an original table VA.
        auto Classified = classifyIndexedBaseModel(Arg, *C);
        if (!Classified)
          return std::nullopt;
        Model = *Classified;
      }
      return RecoveredPhiBase{{*C}, Model};
    }

    uint64_t LpBase = 0;
    bool HaveLp = false;
    std::vector<MedVar> LpIdx;
    if (collectLiteralPoolBase(Arg, LpBase, HaveLp, LpIdx) && HaveLp &&
        baseInRodata(LpBase))
      return RecoveredPhiBase{{LpBase}, AddressModel::Raw};

    // Direct const-base init advanced by a runtime offset: `&tab + index`
    // where clang folds `lea tab(%rip)` / `adrp+add` to the base VA and
    // pre-adds the first iteration's index (the strength-reduced
    // `tab[(i+k)%n]` modulo walk keeps a `base + running_index` pointer).
    uint64_t IgBase = 0;
    bool HaveIg = false;
    std::vector<MedVar> IgIdx;
    if (collectIndexedGlobalBase(Arg, IgBase, HaveIg, IgIdx) && HaveIg &&
        baseInRodata(IgBase))
      if (auto Model = classifyIndexedBaseModel(Arg, IgBase))
        return RecoveredPhiBase{{IgBase}, *Model};
    return std::nullopt;
  };

  auto losesMappedAddressBits = [&](const MedVar &Arg) {
    if (PointerSize == 0 || Arg.Size != PointerSize)
      return false;
    MedVar Current = Arg;
    std::set<std::tuple<int, int, int, uint16_t>> Seen;
    for (int Depth = 0; Depth <= 16; ++Depth) {
      if (Current.isConst()) {
        if (Current.Size == 0 || Current.Size >= PointerSize ||
            Current.Size >= 8)
          return false;
        const unsigned Bits = Current.Size * 8;
        return (Current.ConstVal >> Bits) != 0 &&
               isMaterializableReadOnlyDataAddress(Current.ConstVal);
      }
      auto Key = std::make_tuple(static_cast<int>(Current.Kind), Current.Id,
                                 Current.SSAVer, Current.Size);
      if (!Seen.insert(Key).second)
        return false;
      const MedOp *Def = findDef(Current);
      if (!Def || Def->NumInputs < 1 ||
          (Def->Opcode != NdOp::COPY && Def->Opcode != NdOp::INT_ZEXT) ||
          Def->Output.Size != Current.Size ||
          Def->Inputs[0].Size > Def->Output.Size)
        return false;
      Current = Def->Inputs[0];
    }
    return false;
  };

  const std::set<const PhiNode *> CandidateSet(Candidates.begin(),
                                               Candidates.end());
  std::map<const PhiNode *, std::set<const PhiNode *>> ForwardPhiEdges;
  auto forwardedPhi = [&](const MedVar &Value) {
    MedVar Current = Value;
    std::set<std::tuple<int, int, int>> Seen;
    for (int Depth = 0; Depth <= 64 && !Current.isConst(); ++Depth) {
      if (const PhiNode *Phi = findPhi(Current))
        return Phi;
      auto Key = std::make_tuple(static_cast<int>(Current.Kind), Current.Id,
                                 Current.SSAVer);
      if (!Seen.insert(Key).second)
        break;
      const MedOp *Def = findDef(Current);
      if (!Def)
        break;
      auto Forwarded = pointerPreservingInput(*Def);
      if (!Forwarded || Forwarded->Size != Current.Size)
        break;
      Current = *Forwarded;
    }
    return static_cast<const PhiNode *>(nullptr);
  };
  for (const PhiNode *Phi : CandidateSet) {
    auto &Edges = ForwardPhiEdges[Phi];
    for (const auto &[Pred, Arg] : Phi->Args) {
      if (!phiIncomingEdgeFeasible(*Phi, Pred))
        continue;
      const PhiNode *Source = forwardedPhi(Arg);
      if (Source && CandidateSet.count(Source))
        Edges.insert(Source);
    }
  }
  auto inSameForwardingComponent = [&](const PhiNode *A, const PhiNode *B) {
    if (A == B)
      return true;
    std::vector<const PhiNode *> Work{A};
    std::set<const PhiNode *> Seen;
    while (!Work.empty()) {
      const PhiNode *Current = Work.back();
      Work.pop_back();
      if (!Seen.insert(Current).second)
        continue;
      if (Current == B)
        return true;
      if (auto It = ForwardPhiEdges.find(Current); It != ForwardPhiEdges.end())
        Work.insert(Work.end(), It->second.begin(), It->second.end());
      for (const auto &[Owner, Sources] : ForwardPhiEdges)
        if (Sources.count(Current))
          Work.push_back(Owner);
    }
    return false;
  };

  struct PhiBaseEvidence {
    const PhiNode *Phi = nullptr;
    bool Recurrent = false;
    bool Raw = false;
    bool Symbolized = false;
    std::set<uint64_t> Bases;
  };
  std::vector<PhiBaseEvidence> Evidence;
  std::set<const PhiNode *> ProcessedCandidates;
  for (const PhiNode *Phi : Candidates) {
    if (!ProcessedCandidates.insert(Phi).second)
      continue;
    bool IsRecurrent = phiIsSelfRecurrent(*Phi);
    if (!IsRecurrent && Phi != DirectAddressPhi &&
        !phiHasPureForwardingCycle(*Phi))
      continue;

    bool SawInitialization = false;
    bool SawInvalidInitialization = false;
    bool SawTableShapedInvalidInitialization = false;
    bool SawRawInitialization = false;
    bool SawSymbolizedInitialization = false;
    std::set<uint64_t> CandidateBases;
    for (const auto &[Pred, Arg] : Phi->Args) {
      if (!phiIncomingEdgeFeasible(*Phi, Pred) ||
          phiIncomingIsRecurrent(*Phi, Pred, Arg))
        continue;
      SawInitialization = true;
      auto Recovered = recoverBase(Arg, /*DirectConstantBypassesGetVar=*/true);
      if (!Recovered) {
        SawInvalidInitialization = true;
        // A fully computed init whose resulting value lands in rodata is still
        // table-shaped evidence even when its operands do not prove a pointer
        // base (for example COPY(ADD(0x400,0x480)) == 0x880). Silently treating
        // every such init as "no candidate" would leave the final memory access
        // on a stale absolute VA. Dynamic non-table pointer loops remain
        // ordinary unresolved accesses.
        auto Value = traceValueVA(Arg);
        SawTableShapedInvalidInitialization |=
            (Value && baseInRodata(*Value)) || losesMappedAddressBits(Arg);
        continue;
      }
      CandidateBases.insert(Recovered->VAs.begin(), Recovered->VAs.end());
      SawRawInitialization |= Recovered->Model == AddressModel::Raw;
      SawSymbolizedInitialization |=
          Recovered->Model == AddressModel::Symbolized;
    }
    if (!SawInitialization)
      continue;
    if (CandidateBases.empty()) {
      if (SawTableShapedInvalidInitialization) {
        failAmbiguousPhi(*Phi);
        return nullptr;
      }
      continue;
    }
    if (SawInvalidInitialization ||
        (SawRawInitialization && SawSymbolizedInitialization)) {
      failAmbiguousPhi(*Phi);
      return nullptr;
    }
    Evidence.push_back({Phi, IsRecurrent, SawRawInitialization,
                        SawSymbolizedInitialization,
                        std::move(CandidateBases)});
  }

  auto isRegisterAlias = [&](const PhiNode &A, const PhiNode &B) {
    const MedVar &AV = A.Output;
    const MedVar &BV = B.Output;
    if (AV.Kind != MedVar::Reg || BV.Kind != MedVar::Reg ||
        AV.TheArch != BV.TheArch || AV.Size == 0 || BV.Size == 0)
      return false;
    const TargetRegInfo &TRI = getTargetRegInfo(AV.TheArch);
    return TRI.isSubRegOf(AV.RegOff, AV.Size, BV.RegOff, BV.Size) ||
           TRI.isSubRegOf(BV.RegOff, BV.Size, AV.RegOff, AV.Size);
  };
  auto isSelectArmComponent = [&](const PhiNode &Phi,
                                  const MedVar &Arm) {
    if (Phi.Output == Arm)
      return true;
    const PhiNode *ArmPhi = findPhi(Arm);
    return ArmPhi && inSameForwardingComponent(&Phi, ArmPhi);
  };
  auto isSelectArmPair = [&](const PhiNode &A, const PhiNode &B) {
    return std::any_of(SelectBasePairs.begin(), SelectBasePairs.end(),
                       [&](const auto &Pair) {
                         return (isSelectArmComponent(A, Pair.first) &&
                                 isSelectArmComponent(B, Pair.second)) ||
                                (isSelectArmComponent(A, Pair.second) &&
                                 isSelectArmComponent(B, Pair.first));
                       });
  };

  llvm::Constant *ProvenRun = nullptr;
  uint64_t ProvenRunStart = 0;
  const PhiNode *PrimaryPhi = nullptr;
  std::set<uint64_t> InitializationBases;
  bool SawRawEvidence = false;
  bool SawSymbolizedEvidence = false;
  if (!Evidence.empty()) {
    PrimaryPhi = Evidence.front().Phi;
    for (const PhiBaseEvidence &Item : Evidence) {
      // Multiple table-bearing recurrence candidates are only one pointer
      // component when their outputs are overlapping register aliases. Two
      // independent pointer PHIs under ADD/SUB are not a base+index. Exact
      // SELECT arms are allowed only when both stay in one forwarding component.
      if (Item.Phi != PrimaryPhi && !isRegisterAlias(*PrimaryPhi, *Item.Phi) &&
          !inSameForwardingComponent(PrimaryPhi, Item.Phi) &&
          !isSelectArmPair(*PrimaryPhi, *Item.Phi)) {
        failAmbiguousPhi(*Item.Phi);
        return nullptr;
      }
      // A pure-forwarding PHI component is one loop-carried pointer identity,
      // so each member must expose the same external initialization bases.
      // Multiple alternatives on one PHI remain valid when they share a
      // relocatable run, but a nested reset PHI may not silently replace the
      // component's owner with a different address in that run.
      if (Item.Phi != PrimaryPhi && !isRegisterAlias(*PrimaryPhi, *Item.Phi) &&
          inSameForwardingComponent(PrimaryPhi, Item.Phi) &&
          Item.Bases != Evidence.front().Bases) {
        failAmbiguousPhi(*Item.Phi);
        return nullptr;
      }
      InitializationBases.insert(Item.Bases.begin(), Item.Bases.end());
      SawRawEvidence |= Item.Raw;
      SawSymbolizedEvidence |= Item.Symbolized;
    }
    if (SawRawEvidence && SawSymbolizedEvidence) {
      failAmbiguousPhi(*PrimaryPhi);
      return nullptr;
    }

    // A wide PHI may prove recurrence through a mutually recursive narrow
    // register view. Audit that alias component's own initialization arms too;
    // a low subregister constant is compatible only when it equals the low
    // bits of one proven full-width base.
    for (const PhiNode *Phi : Candidates) {
      if (Phi == PrimaryPhi || !phiIsSelfRecurrent(*Phi) ||
          !isRegisterAlias(*PrimaryPhi, *Phi))
        continue;
      bool SawAliasInitialization = false;
      for (const auto &[Pred, Arg] : Phi->Args) {
        if (!phiIncomingEdgeFeasible(*Phi, Pred) ||
            phiIncomingIsRecurrent(*Phi, Pred, Arg))
          continue;
        SawAliasInitialization = true;
        // Classify alias initializers with the same provenance recovery used
        // for the primary PHI.  In particular, an i386 pointer-table LOAD
        // followed by a stable offset is already symbolized; treating every
        // SawLoad fold as raw here disagrees with recoverBase and falsely
        // rejects the ordinary wide/narrow register views of one recurrence.
        auto Recovered =
            recoverBase(Arg, /*DirectConstantBypassesGetVar=*/true);
        std::set<uint64_t> AliasBases;
        AddressModel AliasModel = AddressModel::Raw;
        if (Recovered) {
          AliasBases = Recovered->VAs;
          AliasModel = Recovered->Model;
        } else {
          // A narrow register view may contain only the low bits of a mapped
          // full-width base (for example W0=0x800 for X0=0x100000800), so it is
          // intentionally not a standalone mapped address recoverBase can
          // resolve. Admit only a pure constant/forwarding chain here, then
          // validate those bits against the primary bases below; arithmetic or
          // LOAD provenance must use the shared structured recovery above.
          bool SawLoad = false;
          bool SawArithmetic = false;
          uint16_t OriginSize = Arg.Size;
          auto Value = traceTableBaseConst(Arg, 0, &SawLoad, &OriginSize,
                                           &SawArithmetic);
          if (!Value || SawLoad || SawArithmetic) {
            failAmbiguousPhi(*Phi);
            return nullptr;
          }
          AliasBases.insert(*Value);
          if (dataOccurrenceSymbolizes(Arg))
            AliasModel = AddressModel::Symbolized;
        }
        bool AliasSymbolized = AliasModel == AddressModel::Symbolized;
        if (AliasSymbolized != SawSymbolizedEvidence) {
          failAmbiguousPhi(*Phi);
          return nullptr;
        }
        unsigned Bits = Arg.Size > 0 ? Arg.Size * 8 : Phi->Output.Size * 8;
        uint64_t Mask = Bits >= 64 ? ~uint64_t(0) : ((uint64_t(1) << Bits) - 1);
        for (uint64_t AliasBase : AliasBases) {
          bool Compatible = false;
          for (uint64_t B : InitializationBases)
            Compatible |= ((AliasBase & Mask) == (B & Mask));
          if (!Compatible) {
            failAmbiguousPhi(*Phi);
            return nullptr;
          }
        }
      }
      if (!SawAliasInitialization) {
        failAmbiguousPhi(*Phi);
        return nullptr;
      }
    }

    Base = *InitializationBases.begin();
    HaveBase = true;
    BaseAddressModel =
        SawRawEvidence ? AddressModel::Raw : AddressModel::Symbolized;

    // A raw/current-VA anchor can cover multiple initialization bases only
    // when every base resolves to the exact same canonical materialized run.
    // Uniformly symbolized values need no anchor and are handled directly
    // below.
    if (InitializationBases.size() > 1 &&
        BaseAddressModel == AddressModel::Raw) {
      for (uint64_t B : InitializationBases) {
        const Segment *Seg = Img->getSegmentFor(B);
        auto [RunGV, RunStart] =
            Seg ? materializeReadOnlyDataRun(Seg)
                : std::pair<llvm::Constant *, uint64_t>{nullptr, 0};
        if (!RunGV ||
            (ProvenRun && (RunGV != ProvenRun || RunStart != ProvenRunStart))) {
          if (!FatalCodePointerResolution && !FatalDataPointerResolution)
            failAmbiguousPhi(*PrimaryPhi);
          return nullptr;
        }
        ProvenRun = RunGV;
        ProvenRunStart = RunStart;
      }
    }
  }
  auto failAmbiguousFallback = [&]() {
    if (PrimaryPhi) {
      failAmbiguousPhi(*PrimaryPhi);
      return;
    }
    failAmbiguousAddress();
  };

  // Fallback for a SELECT-merged base with no induction PHI: the ARM32 unrolled
  // `p = cond ? &W : p+1` reset carries a loop-invariant literal-pool base
  // (`base = PC_const + ldr[pc]`) through SELECT, not a PHI, so the per-PHI
  // scan above never reaches it. Recover every provable rodata base from the
  // SELECT arms and classify it by the same rule getVar uses. A direct SELECT
  // constant is an operation input and therefore does pass through getVar,
  // unlike a direct PHI edge constant.
  if (!HaveBase && SawSelect) {
    std::set<uint64_t> SelectBases;
    using SelectionEvidence = std::vector<RecoveredPhiBase>;
    using SelectionSeen = std::set<std::tuple<int, int, int>>;
    std::function<std::optional<SelectionEvidence>(const MedVar &, int,
                                                   SelectionSeen)>
        recoverSelectionArm =
            [&](const MedVar &Arg, int Depth,
                SelectionSeen Seen) -> std::optional<SelectionEvidence> {
      if (Depth > 16 || varIsFrameDerived(Arg))
        return std::nullopt;
      if (!Arg.isConst()) {
        auto Key =
            std::make_tuple(static_cast<int>(Arg.Kind), Arg.Id, Arg.SSAVer);
        if (!Seen.insert(Key).second)
          return std::nullopt;
        if (const MedOp *Def = findDef(Arg)) {
          if (auto Forwarded = pointerPreservingInput(*Def))
            return recoverSelectionArm(*Forwarded, Depth + 1, Seen);
          MedVar ArmT, ArmF;
          bool IsSelection = false;
          if (selectPreservesPointerValues(*Def)) {
            ArmT = Def->Inputs[1];
            ArmF = Def->Inputs[2];
            IsSelection = true;
          } else if (Def->Opcode == NdOp::INT_OR) {
            MedVar Cond;
            IsSelection = isMaskedSelectOr(*Def, Cond, ArmT, ArmF);
          }
          if (IsSelection) {
            auto T = recoverSelectionArm(ArmT, Depth + 1, Seen);
            auto F = recoverSelectionArm(ArmF, Depth + 1, Seen);
            if (!T || !F)
              return std::nullopt;
            T->insert(T->end(), F->begin(), F->end());
            return T;
          }
        }
      }
      auto Recovered = recoverBase(Arg, /*DirectConstantBypassesGetVar=*/false);
      if (!Recovered)
        return std::nullopt;
      return SelectionEvidence{*Recovered};
    };
    auto selectionArmIsTableShaped = [&](const MedVar &Arg) {
      auto Value = traceValueVA(Arg);
      return Value && baseInRodata(*Value);
    };

    for (const auto &[Left, Right] : SelectBasePairs) {
      auto L = recoverSelectionArm(Left, 0, {});
      auto R = recoverSelectionArm(Right, 0, {});
      // Once either feasible arm proves a table base, every sibling arm must
      // prove a compatible pointer model. A live scalar cannot be discarded
      // merely because another arm happens to be recoverable.
      if (static_cast<bool>(L) != static_cast<bool>(R)) {
        failAmbiguousFallback();
        return nullptr;
      }
      if (!L) {
        // Two equally unprovable arms are not evidence that no table pointer is
        // involved. Constant arithmetic can make both arms land in rodata while
        // deliberately hiding the owning base (for example 0x400 + 0x480).
        // Once the resulting values are table-shaped, refusing the selection is
        // the only safe alternative to retaining their original absolute VAs.
        if (selectionArmIsTableShaped(Left) ||
            selectionArmIsTableShaped(Right)) {
          failAmbiguousFallback();
          return nullptr;
        }
        continue;
      }
      L->insert(L->end(), R->begin(), R->end());
      for (const RecoveredPhiBase &Recovered : *L) {
        if (BaseAddressModel && *BaseAddressModel != Recovered.Model) {
          failAmbiguousFallback();
          return nullptr;
        }
        BaseAddressModel = Recovered.Model;
        SelectBases.insert(Recovered.VAs.begin(), Recovered.VAs.end());
      }
    }
    if (!SelectBases.empty()) {
      Base = *SelectBases.begin();
      HaveBase = true;
      if (SelectBases.size() > 1 && BaseAddressModel == AddressModel::Raw) {
        for (uint64_t B : SelectBases) {
          const Segment *Seg = Img->getSegmentFor(B);
          auto [RunGV, RunStart] =
              Seg ? materializeReadOnlyDataRun(Seg)
                  : std::pair<llvm::Constant *, uint64_t>{nullptr, 0};
          if (!RunGV || (ProvenRun &&
                         (RunGV != ProvenRun || RunStart != ProvenRunStart))) {
            if (!FatalCodePointerResolution && !FatalDataPointerResolution)
              failAmbiguousFallback();
            return nullptr;
          }
          ProvenRun = RunGV;
          ProvenRunStart = RunStart;
        }
      }
    }
  }
  // Fallback for a pointer with no induction PHI but a rodata-segment base
  // folded inline (the unrolled string-walk wrap-around `p = cond ? &W : p+1`,
  // whose SELECT arms carry `&W`'s VA and `&W + offset`).  The frame-derived
  // guard keeps a stack access whose displacement merely lands in a rodata VA
  // range absolute; the segment anchor below recovers the exact element since
  // getVar(addr) still computes the original absolute VA.
  if (!HaveBase && HaveDagRodata && !varIsFrameDerived(AddrVar)) {
    Base = DagRodataBase;
    HaveBase = true;
    BaseAddressModel = AddressModel::Raw;
  }
  if (!HaveBase || !BaseAddressModel)
    return nullptr;

  // When getVar already symbolizes the base constant to a relocatable global,
  // getVar(AddrVar) is ALREADY a valid recompiled pointer
  // (`ptrtoint(@global + off) + index`), so emit a plain load through it rather
  // than `@run + (val - Anchor)` — the latter adds the global a SECOND time
  // (the
  // `- Anchor` only cancels at the lift-time VA, so once the relinked object
  // moves @run the two references no longer cancel and the access reads far out
  // of bounds).  This covers the C-string walk AND the i386/ARM32 PIC GOTOFF /
  // literal-pool table access (`GOT_base(0) + idx + field@GOTOFF`, whose field
  // displacement the loader records in RelocDataAddrs), plus any high-VA
  // pointer base.  x86-64/AArch64 non-PIC keep the base a bare origVA constant
  // getVar leaves numeric (not flagged), so they fall through to the @run
  // anchoring.
  if (*BaseAddressModel == AddressModel::Symbolized) {
    llvm::Value *Cur = getVar(AddrVar, Builder);
    if (!Cur)
      return nullptr;
    if (Cur->getType()->isPointerTy())
      return Cur;
    return Builder.CreateIntToPtr(Cur, llvm::PointerType::get(*Ctx, 0),
                                  "indrawptr");
  }

  // Anchor to the canonical contiguous data run, not a single string/segment
  // global.  The induction value can range over the WHOLE region — a
  // switch-to-string table yields any of several strings spread across
  // `.rodata.str1.1`, while a RELRO table must retain every relocated pointer
  // in its mirror.  The run preserves the original relative layout, so
  // `@run + (Cur - run_start)` lands on the correct element for any VA in the
  // region. Falls back to the single-base global only when no canonical run is
  // available.
  llvm::Constant *G = ProvenRun;
  uint64_t Anchor = ProvenRun ? ProvenRunStart : Base;
  if (!G) {
    if (const Segment *BaseSeg = Img->getSegmentFor(Base)) {
      if (auto [RunGV, RunStart] = materializeReadOnlyDataRun(BaseSeg); RunGV) {
        G = RunGV;
        Anchor = RunStart;
      }
    }
  }
  if (!G) {
    G = tryResolveGlobalData(Base, SizeHint);
    Anchor = Base;
    if (!G)
      return nullptr;
    if (auto *GV = llvm::dyn_cast<llvm::GlobalVariable>(G->stripPointerCasts()))
      if (!GV->isConstant())
        return nullptr;
  }

  // GEP by (current pointer - anchor): the pointer still carries the original
  // VA, so the difference is the element byte offset, valid against the global
  // the recompiled object places at its own VA.
  unsigned Bits = AddrVar.Size > 0 ? AddrVar.Size * 8 : 64;
  auto *Ty = llvm::IntegerType::get(*Ctx, Bits);
  llvm::Value *Cur = getVar(AddrVar, Builder);
  if (!Cur)
    return nullptr;
  if (Cur->getType()->isPointerTy())
    Cur = Builder.CreatePtrToInt(Cur, Ty);
  else if (Cur->getType() != Ty)
    Cur = Builder.CreateZExtOrTrunc(Cur, Ty);
  llvm::Value *Off =
      Builder.CreateSub(Cur, llvm::ConstantInt::get(Ty, Anchor), "indoff");
  return Builder.CreateGEP(llvm::Type::getInt8Ty(*Ctx), G, Off, "indptr");
}

bool MedLLVMEmitter::isReadOnlyDataSymbol(uint64_t VA) {
  if (!Img || VA == 0)
    return false;
  if (RodataSymbolsFor != CurMedFunc) {
    RodataSymbolsFor = CurMedFunc;
    RodataSymbolVAs.clear();
    for (const auto &Sym : Img->Symbols) {
      if (Sym.Addr == 0 || Sym.IsFunc)
        continue;
      if (isMaterializableReadOnlyDataAddress(Sym.Addr))
        RodataSymbolVAs.insert(Sym.Addr);
    }
  }
  return RodataSymbolVAs.count(VA) > 0;
}

llvm::Value *
MedLLVMEmitter::tryResolveIndexedGlobalPtr(const MedVar &AddrVar,
                                           uint16_t SizeHint, bool FailClosed,
                                           llvm::IRBuilder<> &Builder) {
  if (!CurMedFunc || !Img || AddrVar.isConst())
    return nullptr;

  // Locate the defining INT_ADD/INT_SUB and the index operand.
  const MedOp *Def = lookupDef(AddrVar);
  if (!Def || Def->NumInputs < 2 ||
      (Def->Opcode != NdOp::INT_ADD && Def->Opcode != NdOp::INT_SUB))
    return nullptr;
  auto isBoundedScalarLookup = [&](const MedVar &Value) {
    const MedOp *Load = lookupDef(Value);
    if (!Load || Load->Opcode != NdOp::LOAD || Load->NumInputs < 1)
      return false;
    auto sameValue = [](const MedVar &A, const MedVar &B) {
      return !A.isConst() && !B.isConst() && A.Kind == B.Kind &&
             A.Id == B.Id && A.SSAVer == B.SSAVer;
    };
    bool HasSmallBound = false;
    for (const MedBlock &Block : CurMedFunc->Blocks)
      for (const MedOp &Use : Block.Ops)
        for (uint8_t I = 0; I < Use.NumInputs; ++I) {
          if (!sameValue(Use.Inputs[I], Value))
            continue;
          const uint8_t Other = I ^ 1u;
          const bool IsCompare =
              Use.Opcode == NdOp::INT_EQUAL ||
              Use.Opcode == NdOp::INT_NOTEQUAL ||
              Use.Opcode == NdOp::INT_LESS ||
              Use.Opcode == NdOp::INT_SLESS ||
              Use.Opcode == NdOp::INT_LESSEQUAL ||
              Use.Opcode == NdOp::INT_SLESSEQUAL;
          if (IsCompare) {
            if (Use.NumInputs >= 2 && Use.Inputs[Other].isConst() &&
                Use.Inputs[Other].ConstVal <= 0x100000)
              HasSmallBound = true;
            continue;
          }
          switch (Use.Opcode) {
          case NdOp::COPY:
          case NdOp::SUBBYTES:
          case NdOp::INT_ZEXT:
          case NdOp::INT_SEXT:
          case NdOp::INT_ADD:
          case NdOp::INT_SUB:
          case NdOp::INT_AND:
          case NdOp::INT_OR:
          case NdOp::INT_XOR:
          case NdOp::INT_LEFT:
          case NdOp::INT_RIGHT:
          case NdOp::INT_ASHR:
          case NdOp::INT_CARRY:
          case NdOp::INT_SOVF:
          case NdOp::INT_SBOR:
          case NdOp::BOOL_NOT:
            continue;
          default:
            return false;
          }
        }
    return HasSmallBound;
  };
  // Decompose the address into one global base constant plus the runtime index
  // addends.  Handles both the one-level `INT_ADD(base,index)` form and a base
  // nested under multi-dimensional indexing (`base + row*stride + col`).
  uint64_t Base = 0;
  bool HaveBase = false;
  std::vector<MedVar> IdxTerms;
  std::optional<MedVar> DynamicSubtrahend;
  // clang's branchless final-element lookup can form `table_base - (0|-1)`;
  // after lowering, the subtrahend is a non-constant scalar flag DAG.  The
  // generic decomposition intentionally accepts only a constant subtrahend
  // because IdxTerms are additive, so prove the dynamic value independently
  // and retain its negative role for emission below.
  if (Def->Opcode == NdOp::INT_SUB && !traceSSAConst(Def->Inputs[1]) &&
      Def->Output.Size != 0 && Def->Inputs[0].Size != 0 &&
      Def->Output.Size >= Def->Inputs[0].Size &&
      (valueIsStableAddressOffset(Def->Inputs[1]) ||
       isBoundedScalarLookup(Def->Inputs[1])) &&
      collectIndexedGlobalBase(Def->Inputs[0], Base, HaveBase, IdxTerms,
                               /*Depth=*/0, FailClosed) &&
      HaveBase)
    DynamicSubtrahend = Def->Inputs[1];

  if (!DynamicSubtrahend) {
    Base = 0;
    HaveBase = false;
    IdxTerms.clear();
    if (!collectIndexedGlobalBase(AddrVar, Base, HaveBase, IdxTerms,
                                  /*Depth=*/0, FailClosed))
      return nullptr;
  }
  bool OwnerConflict = false;
  std::optional<uint64_t> BaseOwner =
      HaveBase ? uniqueDataAddressOwner(AddrVar, Base, OwnerConflict)
               : std::nullopt;
  if (OwnerConflict || !HaveBase || (Base == 0 && !BaseOwner) ||
      (IdxTerms.empty() && !DynamicSubtrahend))
    return nullptr;

  // A base at a real read-only data symbol is a genuine lookup table (the .o's
  // rodata reference went through a relocation to that symbol).  This is an
  // exact signal, so it bypasses the heuristic guards below that protect
  // against frame-synthesized absolute addresses misread as table bases.
  bool BaseIsRodataSymbol = BaseOwner || isReadOnlyDataSymbol(Base);

  unsigned BaseBits = AddrVar.Size > 0 ? AddrVar.Size * 8 : 64;
  if (!BaseIsRodataSymbol && isFrameRelativeDisplacement(Base, BaseBits))
    return nullptr;

  // `pop`/epilogue pointer arithmetic is `INT_ADD(stack_ptr, k)` where the
  // small increment `k` looks like a read-only segment VA and the stack pointer
  // looks like the index.  A real table index is a data value, never a stack
  // pointer, so reject when any runtime addend is frame-derived (it stays a
  // stack load).
  for (const auto &T : IdxTerms) {
    if (varIsFrameDerived(T))
      return nullptr;
    if (!valueIsStableAddressOffset(T) &&
        !isBoundedScalarLookup(T)) {
      if (FailClosed) {
        if (!FatalDataPointerResolution)
          syncError() << "med_llvm_emitter: read-only table address "
                      << AddrVar.display() << " in " << CurMedFunc->Name
                      << " has an unproved runtime offset; refusing stale-"
                         "address fallback\n";
        FatalDataPointerResolution = true;
      }
      return nullptr;
    }
  }

  // A genuine lookup table is read-only.  If this function performs ANY indexed
  // store to a constant base, it has a read-write array that the frame analysis
  // may have modelled with an absolute address colliding with .rodata (e.g.
  // delta's `int v[64]` stored at 0x40, then reloaded by index).  Redirecting
  // those reloads into a .rodata global breaks the store/load pair, so be
  // conservative: only convert indexed loads in functions with no such stores.
  // crc8's CRC table (no stores) still converts; arrays keep absolute access.
  // A proven rodata symbol base is exempt: it is a real table, not a spilled
  // array, even when the function also indexes-stores to its own stack frame
  // (whose negative frame displacements would otherwise poison StoredConstBases
  // and disable all redirection — the base64 table-hoist case).
  if (!BaseIsRodataSymbol) {
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
    if (!StoredConstBases.empty())
      return nullptr;
  }

  auto *G = BaseOwner ? tryResolveOwnedGlobalData(Base, *BaseOwner, SizeHint)
                      : tryResolveGlobalData(Base, SizeHint);
  if (!G)
    return nullptr;
  // Only redirect into genuinely read-only globals; writable/BSS resolutions
  // are data the program mutates and must keep absolute addressing.
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
    IdxVal = IdxVal ? Builder.CreateAdd(IdxVal, TV, "tblidx") : TV;
  }
  if (DynamicSubtrahend) {
    llvm::Value *TV = getVar(*DynamicSubtrahend, Builder);
    if (!TV)
      return nullptr;
    if (TV->getType()->isPointerTy())
      TV = Builder.CreatePtrToInt(TV, IdxTy);
    else if (TV->getType() != IdxTy)
      TV = Builder.CreateZExtOrTrunc(TV, IdxTy);
    IdxVal = IdxVal ? Builder.CreateSub(IdxVal, TV, "tblidx")
                    : Builder.CreateSub(llvm::ConstantInt::get(IdxTy, 0), TV,
                                        "tblidx");
  }
  if (!IdxVal)
    return nullptr;
  return Builder.CreateGEP(llvm::Type::getInt8Ty(*Ctx), G, IdxVal, "tblptr");
}

} // namespace neverd
