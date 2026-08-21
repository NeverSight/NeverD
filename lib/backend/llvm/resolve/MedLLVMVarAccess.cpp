//===- MedLLVMVarAccess.cpp - Variable materialization ---------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Variable materialization for MedLLVMEmitter: getVar/setVar and the
/// sub-register write propagation they drive, plus the per-function def/phi
/// index those (and the constant classifiers) walk to find an SSA value's
/// defining op.  The constant pointer/integer classification that getVar
/// consults lives in MedLLVMConstClass.cpp; shared address folding in
/// MedLLVMAddrResolve.cpp; specialized table/global resolution in
/// MedLLVMLiteralTable.cpp, MedLLVMIndexedGlobal.cpp, and
/// MedLLVMCodePtrResolve.cpp; global-data embedding in MedLLVMGlobalData.cpp.
///
//===----------------------------------------------------------------------===//

#include "neverd/Common.h"
#include "neverd/Limits.h"
#include "neverd/backend/LLVMValueProvenance.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/support/Diagnostic.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Intrinsics.h"

#include <algorithm>
#include <set>
#include <tuple>
#include <utility>

namespace neverd {

static bool isIncompleteRelocatableConstant(const MedVar &V, Arch TargetArch) {
  if (!V.isConst())
    return false;
  if (V.Provenance == ConstantAddressProvenance::AddressFragment)
    return true;
  if (!isExactAddressProvenance(V.Provenance))
    return false;
  const unsigned PointerSize = getTargetRegInfo(TargetArch).PointerSize;
  return V.Size != 0 && PointerSize != 0 && V.Size < PointerSize;
}

//===----------------------------------------------------------------------===//
// Definition / PHI index
//
// Per-function map from an SSA value's identity to its defining op / PHI, so
// the constant classifiers and getVar avoid a linear block scan per query.
//===----------------------------------------------------------------------===//

// Pack a non-const MedVar's identity (Kind, Id, SSAVer) into the per-function
// definition-index key.  Lossless: the 32-bit Id and SSAVer share the int64
// .first, the small Kind enum is .second (never DenseMap's int sentinel).
static std::pair<int64_t, int> medVarDefKey(const MedVar &V) {
  return {static_cast<int64_t>(
              (static_cast<uint64_t>(static_cast<uint32_t>(V.Id)) << 32) |
              static_cast<uint32_t>(V.SSAVer)),
          static_cast<int>(V.Kind)};
}

void MedLLVMEmitter::ensureDefPhiIndex() const {
  if (DefPhiIndexFor == CurMedFunc)
    return;
  DefPhiIndexFor = CurMedFunc;
  DefIndex.clear();
  PhiIndex.clear();
  if (!CurMedFunc)
    return;
  // try_emplace keeps the FIRST op/phi defining each identity, matching the old
  // linear scans' "return the first match" behavior.
  for (const MedBlock &Blk : CurMedFunc->Blocks) {
    for (const MedOp &Op : Blk.Ops)
      if (!Op.Output.isConst())
        DefIndex.try_emplace(medVarDefKey(Op.Output), &Op);
    for (const PhiNode &P : Blk.Phis)
      if (!P.Output.isConst())
        PhiIndex.try_emplace(medVarDefKey(P.Output), &P);
  }
}

const MedOp *MedLLVMEmitter::lookupDef(const MedVar &V) const {
  if (V.isConst())
    return nullptr;
  ensureDefPhiIndex();
  auto It = DefIndex.find(medVarDefKey(V));
  return It == DefIndex.end() ? nullptr : It->second;
}

const PhiNode *MedLLVMEmitter::lookupPhi(const MedVar &V) const {
  if (V.isConst())
    return nullptr;
  ensureDefPhiIndex();
  auto It = PhiIndex.find(medVarDefKey(V));
  return It == PhiIndex.end() ? nullptr : It->second;
}

//===----------------------------------------------------------------------===//
// Variable access
//===----------------------------------------------------------------------===//

bool MedLLVMEmitter::getVarHasDirectDataSymbolizationIntent(
    uint64_t Val, uint16_t Size) const {
  // Untagged zero remains the null/scalar value even when a distinct exact
  // relocation occurrence authenticates data at VA 0 in the same image.
  if (!Img || Val == 0)
    return false;
  unsigned PtrSize = getTargetRegInfo(TargetArch).PointerSize;
  bool IsPointerWidth = Size == 0 || PtrSize == 0 || Size >= PtrSize;
  bool LoaderProven =
      ((Img->RelocDataAddrs.count(Val) || Img->RodataAnchorSeg.count(Val)) &&
       !constValueUsedAsInteger(Val) && !addrInCodePtrMirrorRun(Val)) ||
      (constIsRodataEndPointer(Val) && !constValueUsedAsInteger(Val) &&
       !addrInCodePtrMirrorRun(Val)) ||
      constIsWritableRunEndPointer(Val);
  bool Candidate =
      (IsPointerWidth && Val > limits::kMinGlobalDataAddr) || LoaderProven;
  if (!Candidate)
    return false;

  // Executable and mutable segments are collision-prone: a large arithmetic
  // immediate may merely land inside them. Match getVar's pointer-use gate so
  // the shared predicate describes the value that is actually emitted.
  const Segment *Seg = Img->getSegmentFor(Val);
  if ((!Seg || !Seg->isReadable()) && !LoaderProven)
    return false;
  bool Collidable = Seg && (Seg->isExecutable() || isMutableDataSeg(Seg));
  return !Collidable || constUsedAsPointer(Val);
}

bool MedLLVMEmitter::getVarDirectlySymbolizesDataConstant(uint64_t Val,
                                                          uint16_t Size) const {
  return getVarHasDirectDataSymbolizationIntent(Val, Size) &&
         canResolveGlobalDataConstant(Val);
}

bool MedLLVMEmitter::getVarSymbolizesDataConstant(uint64_t Val,
                                                  uint16_t Size) const {
  if (getVarDirectlySymbolizesDataConstant(Val, Size))
    return true;
  // Induction-string closure deliberately makes every arm of one walked
  // string pointer share the canonical rodata run.  Its recognizer asks the
  // direct classifier above, so this final closure cannot recurse into itself.
  return isInductionRodataStringBase(Val) && canResolveGlobalDataConstant(Val);
}

bool MedLLVMEmitter::dataOccurrenceSymbolizes(
    const MedVar &V, bool DirectPhiConstantBypassesGetVar) const {
  MedVar Cur = V;
  bool BypassesGetVar = DirectPhiConstantBypassesGetVar && Cur.isConst() &&
                        !isExactAddressProvenance(Cur.Provenance);
  std::set<std::tuple<int, int, int, uint16_t>> Seen;

  while (!Cur.isConst()) {
    auto Key = std::make_tuple(static_cast<int>(Cur.Kind), Cur.Id, Cur.SSAVer,
                               Cur.Size);
    if (!Seen.insert(Key).second)
      return false;
    const MedOp *Def = lookupDef(Cur);
    if (!Def)
      return false;
    auto Forwarded = pointerPreservingInput(*Def);
    if (!Forwarded || Forwarded->Size != Cur.Size)
      return false;
    Cur = *Forwarded;
    BypassesGetVar = false;
  }

  if (BypassesGetVar)
    return false;

  const unsigned PtrSize = getTargetRegInfo(TargetArch).PointerSize;
  const bool IsPointerWidth =
      Cur.Size == 0 || PtrSize == 0 || Cur.Size >= PtrSize;

  // getVar gives an exact architectural code address precedence over the data
  // routes. Keep data-table ownership audits from mistaking ptrtoint(@func)
  // for a data-global address merely because executable bytes are readable.
  if ((isCodeAddressProvenance(Cur.Provenance) ||
       (Cur.Provenance == ConstantAddressProvenance::Address && Img &&
        Img->isCodeAddress(Cur.ConstVal))) &&
      IsPointerWidth)
    return false;

  uint64_t Address = 0;
  if (isExactAddressProvenance(Cur.Provenance) &&
      resolveMaterializableDataAddress(Cur, Address))
    return true;

  if (Cur.Provenance != ConstantAddressProvenance::Unknown)
    return false;

  if (getVarSymbolizesDataConstant(Cur.ConstVal, Cur.Size))
    return true;

  // The ordinary function-address path also precedes writable/select-peer
  // data materialization in getVar.
  if (IsPointerWidth && Img && Mod &&
      (Cur.ConstVal >= limits::kMinGlobalDataAddr ||
       Img->CodeRefTargets.count(Cur.ConstVal)))
    if (resolveLiftedFunctionEntry(Cur.ConstVal))
      return false;

  if (symbolizesWritableRelocPtr(Cur.ConstVal, Cur.Size))
    return true;
  return IsPointerWidth && symbolizesSelectPeer(Cur.ConstVal);
}

bool MedLLVMEmitter::constantOccurrenceMayRelocate(
    const MedVar &V, bool DirectPhiConstantBypassesGetVar) const {
  if (dataOccurrenceSymbolizes(V, DirectPhiConstantBypassesGetVar))
    return true;

  MedVar Cur = V;
  bool BypassesGetVar = DirectPhiConstantBypassesGetVar && Cur.isConst() &&
                        !isExactAddressProvenance(Cur.Provenance);
  std::set<std::tuple<int, int, int, uint16_t>> Seen;
  while (!Cur.isConst()) {
    auto Key = std::make_tuple(static_cast<int>(Cur.Kind), Cur.Id, Cur.SSAVer,
                               Cur.Size);
    if (!Seen.insert(Key).second)
      return false;
    const MedOp *Def = lookupDef(Cur);
    if (!Def)
      return false;
    auto Forwarded = pointerPreservingInput(*Def);
    if (!Forwarded || Forwarded->Size != Cur.Size)
      return false;
    Cur = *Forwarded;
    BypassesGetVar = false;
  }
  if (BypassesGetVar)
    return false;

  if (Cur.Provenance == ConstantAddressProvenance::Scalar ||
      Cur.Provenance == ConstantAddressProvenance::AddressFragment)
    return false;

  const unsigned PtrSize = getTargetRegInfo(TargetArch).PointerSize;
  const bool IsPointerWidth =
      Cur.Size == 0 || PtrSize == 0 || Cur.Size >= PtrSize;
  if (!IsPointerWidth || !Img || !Mod)
    return false;

  const bool CodeIntent =
      isCodeAddressProvenance(Cur.Provenance) ||
      (Cur.Provenance == ConstantAddressProvenance::Address &&
       Img->isCodeAddress(Cur.ConstVal)) ||
      (Cur.Provenance == ConstantAddressProvenance::Unknown &&
       Cur.ConstVal != 0 &&
       (Cur.ConstVal >= limits::kMinGlobalDataAddr ||
        Img->CodeRefTargets.count(Cur.ConstVal)));
  if (!CodeIntent)
    return false;
  return resolveLiftedFunctionEntry(Cur.ConstVal) != nullptr;
}

MedLLVMEmitter::ConstantProvenanceSummary
MedLLVMEmitter::summarizeConstantProvenance(const MedVar &V) const {
  using Model = ConstantProvenanceSummary::ValueModel;
  struct Result {
    Model Kind = Model::Unknown;
    bool SawFragment = false;
  };
  using Key = std::tuple<int, int, int, uint16_t>;
  std::map<Key, Result> Memo;
  std::set<Key> Active;
  int Budget = 512;

  auto mergeValues = [&](Result A, const MedVar &AV, Result B,
                         const MedVar &BV) {
    Result Out{Model::Mixed, A.SawFragment || B.SawFragment};
    if (A.Kind == B.Kind) {
      Out.Kind = A.Kind;
      return Out;
    }
    // A PHI/SELECT/frame merge does not inherit arithmetic's
    // Unknown+Scalar=>Unknown rule.  The arms are alternative values, so
    // unlike an address plus a displacement they form a mixed occurrence that
    // must not re-enter a value-global pointer heuristic.
    auto isNullScalar = [&](Result R, const MedVar &Value) {
      if (R.Kind != Model::Scalar)
        return false;
      auto Folded = traceValueVA(Value);
      return Folded && *Folded == 0;
    };
    if ((A.Kind == Model::Unknown && isNullScalar(B, BV)) ||
        (B.Kind == Model::Unknown && isNullScalar(A, AV))) {
      Out.Kind = Model::Unknown;
      return Out;
    }
    if ((A.Kind == Model::Address && isNullScalar(B, BV)) ||
        (B.Kind == Model::Address && isNullScalar(A, AV))) {
      Out.Kind = Model::Address;
      return Out;
    }
    return Out;
  };

  std::function<Result(const MedVar &)> evaluate =
      [&](const MedVar &Cur) -> Result {
    if (--Budget < 0)
      // Exhaustion is analysis uncertainty, never evidence that a relocation
      // taint is absent.  Escape audits must fail closed for adversarially deep
      // forwarding chains instead of reopening legacy numeric heuristics.
      return {Model::Mixed, true};
    if (Cur.isConst()) {
      switch (Cur.Provenance) {
      case ConstantAddressProvenance::Unknown:
        return {Model::Unknown, false};
      case ConstantAddressProvenance::Scalar:
        return {Model::Scalar, false};
      case ConstantAddressProvenance::AddressFragment:
        return {Model::AddressFragment, true};
      case ConstantAddressProvenance::Address:
      case ConstantAddressProvenance::DataAddress:
      case ConstantAddressProvenance::CodeAddress:
        if (isIncompleteRelocatableConstant(Cur, TargetArch))
          return {Model::AddressFragment, true};
        return {Model::Address, false};
      }
    }

    Key K = std::make_tuple(static_cast<int>(Cur.Kind), Cur.Id, Cur.SSAVer,
                            Cur.Size);
    if (auto It = Memo.find(K); It != Memo.end())
      return It->second;
    if (!Active.insert(K).second)
      return {};

    Result Out;
    if (const PhiNode *Phi = lookupPhi(Cur)) {
      bool First = true;
      MedVar Previous;
      for (const auto &[Pred, Arg] : Phi->Args) {
        if (classifyPhiIncomingEdge(*Phi, Pred) ==
            PhiEdgeFeasibility::Infeasible)
          continue;
        Result Arm = evaluate(Arg);
        if (First) {
          Out = Arm;
          Previous = Arg;
          First = false;
        } else {
          Out = mergeValues(Out, Previous, Arm, Arg);
          Previous = Cur;
        }
      }
    } else if (const MedOp *Def = lookupDef(Cur)) {
      if (auto Forwarded = pointerPreservingInput(*Def)) {
        Out = evaluate(*Forwarded);
      } else if (Def->Opcode == NdOp::SELECT && Def->NumInputs >= 3) {
        Result T = evaluate(Def->Inputs[1]);
        Result F = evaluate(Def->Inputs[2]);
        Out = mergeValues(T, Def->Inputs[1], F, Def->Inputs[2]);
      } else if ((Def->Opcode == NdOp::INT_ADD ||
                  Def->Opcode == NdOp::INT_SUB) &&
                 Def->NumInputs >= 2) {
        Result A = evaluate(Def->Inputs[0]);
        Result B = evaluate(Def->Inputs[1]);
        Out.SawFragment = A.SawFragment || B.SawFragment;
        if (A.Kind == Model::Mixed || B.Kind == Model::Mixed) {
          Out.Kind = Model::Mixed;
        } else if (A.Kind == Model::AddressFragment) {
          Out.Kind = B.Kind == Model::Scalar || B.Kind == Model::Unknown
                         ? Model::AddressFragment
                         : Model::Mixed;
        } else if (Def->Opcode == NdOp::INT_ADD &&
                   B.Kind == Model::AddressFragment) {
          Out.Kind = A.Kind == Model::Scalar || A.Kind == Model::Unknown
                         ? Model::AddressFragment
                         : Model::Mixed;
        } else if (A.Kind == Model::Address) {
          if (B.Kind == Model::Address)
            Out.Kind =
                Def->Opcode == NdOp::INT_SUB ? Model::Scalar : Model::Mixed;
          else
            Out.Kind = Model::Address;
        } else if (Def->Opcode == NdOp::INT_ADD && B.Kind == Model::Address) {
          Out.Kind = A.Kind == Model::Address ? Model::Mixed : Model::Address;
        } else if (Def->Opcode == NdOp::INT_SUB && B.Kind == Model::Address) {
          Out.Kind = Model::Mixed;
        } else if (A.Kind == Model::Scalar && B.Kind == Model::Scalar) {
          Out.Kind = Model::Scalar;
        } else {
          Out.Kind = Model::Unknown;
        }
      } else if (Def->Opcode == NdOp::LOAD) {
        std::vector<MedVar> Sources;
        if (collectFrameReloadSources(*Def, Sources) && !Sources.empty()) {
          bool First = true;
          MedVar Previous;
          for (const MedVar &Source : Sources) {
            Result SourceResult = evaluate(Source);
            if (First) {
              Out = SourceResult;
              Previous = Source;
              First = false;
            } else {
              Out = mergeValues(Out, Previous, SourceResult, Source);
              Previous = Cur;
            }
          }
        }
      } else {
        switch (Def->Opcode) {
        case NdOp::INT_AND:
        case NdOp::INT_OR:
        case NdOp::INT_XOR:
        case NdOp::INT_LEFT:
        case NdOp::INT_RIGHT:
        case NdOp::INT_ASHR:
        case NdOp::INT_MULT:
        case NdOp::INT_NEGATE:
        case NdOp::INT_NEG2: {
          bool AllScalar = Def->NumInputs > 0;
          bool AllUnknownOrScalar = Def->NumInputs > 0;
          bool SawFragment = false;
          for (uint8_t I = 0; I < Def->NumInputs; ++I) {
            Result Input = evaluate(Def->Inputs[I]);
            SawFragment |= Input.SawFragment;
            AllScalar &= Input.Kind == Model::Scalar;
            AllUnknownOrScalar &=
                Input.Kind == Model::Unknown || Input.Kind == Model::Scalar;
          }
          Out.SawFragment = SawFragment;
          Out.Kind = AllScalar            ? Model::Scalar
                     : AllUnknownOrScalar ? Model::Unknown
                                          : Model::Mixed;
          break;
        }
        default:
          break;
        }
      }
    }

    Active.erase(K);
    Memo.emplace(K, Out);
    return Out;
  };

  Result Final = evaluate(V);
  return {Final.Kind, Final.SawFragment};
}

bool MedLLVMEmitter::occurrenceHasAddressFragmentTaint(const MedVar &V) const {
  if (V.isConst())
    return isIncompleteRelocatableConstant(V, TargetArch);
  if (!CurMedFunc || AddressFragmentTaintFor != CurMedFunc)
    return false;
  return AddressFragmentTaint.count(addressProvenanceVarKey(V)) != 0;
}

bool MedLLVMEmitter::addressFragmentCanStayInLocalFrameSlot(
    const MedVar &SlotAddr, uint16_t StoredSize) const {
  const auto Slot = canonicalFrameSlotKey(SlotAddr);
  if (!Slot || StoredSize == 0 || stackSlotAddressEscapes(SlotAddr))
    return false;

  auto overlaps = [](int64_t LeftOffset, uint16_t LeftSize, int64_t RightOffset,
                     uint16_t RightSize) {
    if (LeftSize == 0 || RightSize == 0)
      return true;
    // Ordered signed endpoints can be subtracted in unsigned space without
    // overflow: even INT64_MIN..INT64_MAX has the exact UINT64_MAX distance.
    // This keeps the alias proof portable to MSVC, which has no __int128.
    if (LeftOffset <= RightOffset)
      return static_cast<uint64_t>(RightOffset) -
                 static_cast<uint64_t>(LeftOffset) <
             LeftSize;
    return static_cast<uint64_t>(LeftOffset) -
               static_cast<uint64_t>(RightOffset) <
           RightSize;
  };
  auto readMayAliasStoredSlot = [&](const MedVar &ReadAddr, uint16_t ReadSize) {
    if (!varMayBeFrameAddress(ReadAddr))
      return false;
    const auto ReadSlot = canonicalFrameSlotKey(ReadAddr);
    if (!ReadSlot || ReadSlot->first != Slot->first)
      // An uncanonical frame address or a different live-in root may alias
      // after an unmodelled stack adjustment.  Lack of proof is not evidence
      // that a fragment cannot escape.
      return true;
    return overlaps(Slot->second, StoredSize, ReadSlot->second, ReadSize);
  };

  // The exemption is sound only when every reload of the slot has a complete
  // all-path source proof.  Otherwise a fragment store on one branch and an
  // uninitialized/partially-aliased path on another could make the may-flow
  // disappear from the function-wide taint graph and escape after the join.
  if (!CurMedFunc)
    return false;
  for (const MedBlock &Block : CurMedFunc->Blocks)
    for (const MedOp &Op : Block.Ops) {
      if (Op.Opcode != NdOp::LOAD || Op.NumInputs < 1)
        continue;
      if (!readMayAliasStoredSlot(Op.Inputs[0], Op.Output.Size))
        continue;
      std::vector<MedVar> Sources;
      if (!collectFrameReloadSources(Op, Sources) || Sources.empty())
        return false;
    }
  for (const MedBlock &Block : CurMedFunc->Blocks)
    for (const MedOp &Op : Block.Ops) {
      std::optional<uint8_t> AddressInput;
      if (Op.Opcode == NdOp::ATOMIC_XCHG || Op.Opcode == NdOp::ATOMIC_ADD ||
          Op.Opcode == NdOp::ATOMIC_CMPXCHG)
        AddressInput =
            Op.NumInputs >= 1 ? std::optional<uint8_t>(0) : std::nullopt;
      else if (Op.Opcode == NdOp::INTRINSIC)
        AddressInput = atomicIntrinsicAddressInput(Op);
      if (!AddressInput) {
        if (Op.Opcode == NdOp::INTRINSIC)
          for (uint8_t I = 1; I < Op.NumInputs; ++I)
            if (varMayBeFrameAddress(Op.Inputs[I]))
              return false;
        continue;
      }
      uint16_t AccessSize = Op.Output.Size;
      if (*AddressInput == 3)
        AccessSize = 16;
      else if (*AddressInput == 2 && Op.NumInputs > 3 && Op.Inputs[3].isConst())
        AccessSize = static_cast<uint16_t>(Op.Inputs[3].ConstVal);
      else if (*AddressInput == 1 && Op.NumInputs > 2 && Op.Inputs[2].isConst())
        AccessSize = static_cast<uint16_t>(Op.Inputs[2].ConstVal);
      if (readMayAliasStoredSlot(Op.Inputs[*AddressInput], AccessSize))
        // collectFrameReloadSources deliberately models plain LOAD only.  A
        // read/modify/write or exclusive read of the same slot can observe the
        // fragment, so do not exempt its STORE until atomic reaching-memory
        // flow has an equally complete all-path proof.
        return false;
    }
  return true;
}

bool MedLLVMEmitter::rejectEscapingAddressFragment(
    const MedVar &V, const char *SinkDescription) const {
  if (!occurrenceHasAddressFragmentTaint(V))
    return false;
  if (!FatalDataPointerResolution)
    syncError() << "med_llvm_emitter: incomplete relocatable address value "
                << V.display() << " escapes through " << SinkDescription
                << " in " << (CurMedFunc ? CurMedFunc->Name : "<unknown>")
                << "; refusing stale-address fallback\n";
  FatalDataPointerResolution = true;
  return true;
}

bool MedLLVMEmitter::getVarMayRelocateConstant(uint64_t Val,
                                               uint16_t Size) const {
  if (Val == 0)
    return false;
  if (getVarSymbolizesDataConstant(Val, Size) ||
      symbolizesWritableRelocPtr(Val, Size))
    return true;

  unsigned PtrSize = getTargetRegInfo(TargetArch).PointerSize;
  bool IsPointerWidth = Size == 0 || PtrSize == 0 || Size >= PtrSize;
  if (!IsPointerWidth)
    return false;

  if (symbolizesSelectPeer(Val))
    return true;
  if (!Img || !Mod ||
      (Val < limits::kMinGlobalDataAddr && !Img->CodeRefTargets.count(Val)))
    return false;
  return resolveLiftedFunctionEntry(Val) != nullptr;
}

llvm::Value *MedLLVMEmitter::materializeDataRelationConstant(
    uint64_t Val, uint16_t Size, llvm::IRBuilder<> &Builder, uint64_t OwnerVA) {
  // A non-zero hint selects the identity-preserving raw/run representation;
  // the compact unnamed-string form may legally merge equal contents at two
  // original VAs and therefore cannot represent address algebra.
  llvm::Constant *Global = OwnerVA != InvalidVA
                               ? tryResolveOwnedGlobalData(Val, OwnerVA, Size)
                               : tryResolveGlobalData(Val, Size);
  if (Global)
    return Builder.CreatePtrToInt(Global, sizeToType(Size), "gdatarel");

  if (!FatalCodePointerResolution && !FatalDataPointerResolution)
    syncError() << "med_llvm_emitter: relocatable data-relation constant 0x"
                << llvm::utohexstr(Val) << " in "
                << (CurMedFunc ? CurMedFunc->Name : "<unknown>")
                << " has no global-data materialization route; refusing "
                   "stale-address fallback\n";
  if (!FatalCodePointerResolution)
    FatalDataPointerResolution = true;
  return llvm::UndefValue::get(sizeToType(Size));
}

llvm::Value *
MedLLVMEmitter::getPhiIncomingConstant(const MedVar &V, uint16_t OutputSize,
                                       llvm::IRBuilder<> &Builder) {
  assert(V.isConst() && "PHI constant materializer requires a constant");
  if (isExactAddressProvenance(V.Provenance))
    return getVar(V, Builder);
  return llvm::ConstantInt::get(sizeToType(OutputSize),
                                static_cast<int64_t>(V.ConstVal),
                                /*isSigned=*/true);
}

llvm::Value *MedLLVMEmitter::getPhiIncomingValue(const MedVar &V,
                                                 uint16_t OutputSize,
                                                 llvm::IRBuilder<> &Builder) {
  if (codeIdentityOccurrenceMayRelocate(V))
    if (llvm::Value *Code =
            tryResolveCodeAddressValue(V, /*RequireCodeRole=*/false, Builder))
      return Code;
  if (V.isConst())
    return getPhiIncomingConstant(V, OutputSize, Builder);
  return getVar(V, Builder);
}

llvm::Value *MedLLVMEmitter::getVar(const MedVar &V,
                                    llvm::IRBuilder<> &Builder) {
  if (V.Kind == MedVar::EHException || V.Kind == MedVar::EHSelector) {
    auto &Entry = CurFunc->getEntryBlock();
    llvm::IRBuilder<> AllocBuilder(&Entry, Entry.begin());
    const unsigned PointerSize = getTargetRegInfo(TargetArch).PointerSize;
    llvm::Type *StorageTy = V.Kind == MedVar::EHException
                                ? sizeToType(static_cast<uint16_t>(PointerSize))
                                : llvm::Type::getInt32Ty(*Ctx);
    llvm::AllocaInst *&Slot =
        V.Kind == MedVar::EHException ? EHExceptionAlloca : EHSelectorAlloca;
    if (!Slot)
      Slot = AllocBuilder.CreateAlloca(StorageTy, nullptr,
                                       V.Kind == MedVar::EHException
                                           ? "eh.exception.slot"
                                           : "eh.selector.slot");
    llvm::Value *Value = Builder.CreateLoad(
        StorageTy, Slot,
        V.Kind == MedVar::EHException ? "eh_exception" : "eh_selector");
    auto *WantedTy = sizeToType(V.Size);
    if (Value->getType() != WantedTy) {
      unsigned Have = Value->getType()->getIntegerBitWidth();
      unsigned Want = WantedTy->getIntegerBitWidth();
      Value = Have > Want ? Builder.CreateTrunc(Value, WantedTy)
                          : Builder.CreateZExt(Value, WantedTy);
    }
    return Value;
  }

  if (V.isConst()) {
    constexpr uint64_t kMinGlobalDataAddr = limits::kMinGlobalDataAddr;
    unsigned PtrSz = getTargetRegInfo(TargetArch).PointerSize;
    // A pointer-width constant above the heuristic threshold, or one the
    // loader proved is a relocation target inside read-only data, is a real
    // pointer rather than an integer literal — resolve it to the embedded
    // global.  Requiring pointer width on the heuristic-only path keeps narrow
    // arithmetic immediates such as an x86-64 `and r32, 0x2000` from becoming
    // pointers when their value happens to fall inside a large rodata run.  The
    // relocation set
    // catches low-VA rodata bases (e.g. an i386 GOTOFF `.rodata` base walked by
    // an induction pointer) that the bare numeric threshold cannot tell apart
    // from a small integer.  EXCEPTION: a value that the function also uses as
    // a genuine integer (a loop bound / counter that happens to equal a rodata
    // VA, e.g. i386 trip count 160 == `.rodata` chunk VA 0xA0) must stay an
    // integer; such collisions are detected per-function and excluded. An
    // induction-pointer rodata C-string base is symbolized UNCONDITIONALLY —
    // including the advance arm's `base + k` constant the relocation set never
    // lists — so both PHI arms become the SAME canonical rodata run pointer
    // (paired with the string→run routing in tryResolveGlobalData and the plain
    // load in tryResolveInductionGlobalPtr).  Otherwise the reset arm (a reloc
    // target) is a recompiled VA while the advance arm stays an original VA,
    // and the merged pointer corrupts whichever model resolves it. A
    // reloc-target rodata VA that sits inside a code-pointer mirror RUN (a
    // `.rodata` chain tail contiguous with a `.data.rel.ro` pointer table) is
    // kept as the raw original VA — exactly like the `.data.rel.ro` head nodes,
    // which the loader never records in RelocDataAddrs.  Its access is re-
    // symbolized through the mirror as `mirror + (VA - runStart)`; redirecting
    // it here to a separate per-segment rodata global instead would make an
    // i386 PIC stack table of node pointers mix two addressing models and
    // corrupt the mirror-relative load (the head nodes raw, the rodata tail
    // recompiled).
    auto failUnresolvableDataConstant = [&]() -> llvm::Value * {
      if (!FatalCodePointerResolution && !FatalDataPointerResolution)
        syncError() << "med_llvm_emitter: relocatable data constant 0x"
                    << llvm::utohexstr(V.ConstVal) << " in " << CurMedFunc->Name
                    << " has no global-data materialization route; refusing "
                       "stale-address fallback\n";
      if (!FatalCodePointerResolution)
        FatalDataPointerResolution = true;
      return llvm::UndefValue::get(sizeToType(V.Size));
    };
    // Explicit scalar and page-fragment occurrences never enter the legacy
    // value-global classifier.  A fragment is allowed to exist as an
    // intermediate ADRP/page-base value; escape sinks reject it separately.
    if (V.Provenance == ConstantAddressProvenance::Scalar ||
        V.Provenance == ConstantAddressProvenance::AddressFragment)
      return llvm::ConstantInt::get(sizeToType(V.Size), V.ConstVal);

    // Explicit provenance owns the occurrence and never falls through to the
    // legacy value-global classifier. Role-neutral Address is resolved only
    // outside executable bytes; executable data and code must be identified
    // by DataAddress/CodeAddress respectively.
    if (isExactAddressProvenance(V.Provenance)) {
      const bool IsPointerWidth = V.Size == 0 || PtrSz == 0 || V.Size >= PtrSz;
      if (!IsPointerWidth)
        return llvm::ConstantInt::get(sizeToType(V.Size), V.ConstVal);

      if (isCodeAddressProvenance(V.Provenance)) {
        if (llvm::Constant *Code = resolveLiftedCodeAddress(V.ConstVal))
          return Builder.CreatePtrToInt(Code, sizeToType(V.Size),
                                        "codeptr.prov");
        if (!FatalCodePointerResolution && !FatalDataPointerResolution)
          syncError() << "med_llvm_emitter: relocatable code address 0x"
                      << llvm::utohexstr(V.ConstVal) << " in "
                      << CurMedFunc->Name
                      << " has no lifted code identity; refusing stale-address "
                         "fallback\n";
        FatalCodePointerResolution = true;
        return llvm::UndefValue::get(sizeToType(V.Size));
      }

      if (V.Provenance == ConstantAddressProvenance::Address && Img &&
          Img->isCodeAddress(V.ConstVal)) {
        // A role-neutral architectural PC is also used as the numeric base of
        // ARM switch/literal expressions.  Eagerly turning every such COPY
        // into blockaddress changes arithmetic semantics and can leave an
        // in-memory BlockAddress owned by a different cloning context.  Code
        // and data use sites own that decision; only an explicit CodeAddress
        // is materialized above.
        return llvm::ConstantInt::get(sizeToType(V.Size), V.ConstVal);
      }

      uint64_t Address = 0;
      uint64_t OwnerVA = InvalidVA;
      if (resolveMaterializableDataAddress(V, Address, &OwnerVA))
        return materializeDataRelationConstant(Address, V.Size, Builder,
                                               OwnerVA);
      if (V.Provenance == ConstantAddressProvenance::Address)
        return llvm::ConstantInt::get(sizeToType(V.Size), V.ConstVal);
      return failUnresolvableDataConstant();
    }
    if (getVarSymbolizesDataConstant(V.ConstVal, V.Size)) {
      // A constant that falls in an EXECUTABLE segment is a genuine pointer
      // only when it actually reaches a memory-access address — a
      // `.text`-embedded literal-pool load on ARM32.  An integer that merely
      // collides with a
      // `.text` VA (an AND mask / shift amount equal to a code offset, e.g.
      // 0x2000 inside an 11 KB `.text`) must stay an integer; symbolizing it to
      // a code-segment global corrupts the value after relink (the #456/#499
      // small-constant-collides-with-a-segment-VA family, executable arm).
      // Non-executable rodata/data pointers are unaffected.
      // A constant in an EXECUTABLE segment, or in a MUTABLE .bss/.data segment
      // whose VA range extends past the pointer-heuristic threshold, is a
      // genuine pointer only when it actually reaches a memory-access address.
      // A large `static` array in a relocatable .o sits at a low VA but spans
      // well past kMinGlobalDataAddr, so an integer that merely collides with
      // that range — an AArch64/ARM byte-offset loop bound `i != sizeof G`
      // landing inside the low-VA `.bss` run, or an AND mask/shift equal to a
      // `.text` offset — must stay an integer; symbolizing it to the segment
      // global corrupts the value after relink (the #456/#499/#512/#521
      // small-constant-collides-with-a- segment-VA family).  Read-only rodata
      // bases are dereferenced (so they pass constUsedAsPointer) or anchored
      // via RelocDataAddrs above, hence unaffected.
      if (auto *Global = tryResolveGlobalData(V.ConstVal))
        return Builder.CreatePtrToInt(Global, sizeToType(V.Size), "gdata");
      return failUnresolvableDataConstant();
    }
    // A constant equal to a function entry whose address was taken (recorded by
    // the loader/lift as a code reference, or above the heuristic VA threshold)
    // is a function pointer materialized by `lea rip`/`adrp+add`/literal pool —
    // emit a relocatable `ptrtoint @func` so the indirect call through it lands
    // on the recompiled function instead of the stale original VA.  Gating on
    // the recorded target set keeps a genuine integer that merely equals a low
    // function VA (e.g. 0) an integer.
    //
    // A function pointer is materialized at EXACTLY pointer width —
    // `lea`/`adrp` produce a whole register-sized address, never a sub-word. So
    // a constant narrower than the pointer cannot be a function pointer; it is
    // a plain integer that merely collides with an address-taken function's
    // entry VA (an `int` call argument `f(0x68)` where a callee happens to sit
    // at VA 0x68, the #456/#459/#499/#518 "small constant collides with a
    // reloc/code target VA" family).  On a 64-bit target the colliding `int` is
    // 4 bytes < 8, so the width guard keeps it an integer; the genuine
    // code-pointer call target is always materialized at the full pointer width
    // and is unaffected.
    if (Img && V.ConstVal != 0 &&
        (V.Size == 0 || PtrSz == 0 || V.Size >= PtrSz) &&
        (V.ConstVal >= kMinGlobalDataAddr ||
         Img->CodeRefTargets.count(V.ConstVal))) {
      if (llvm::Function *F = resolveLiftedFunctionEntry(V.ConstVal))
        return Builder.CreatePtrToInt(F, sizeToType(V.Size), "fnptr");
    }
    // A constant the loader proved is a taken address `&G` inside a WRITABLE
    // .data/.bss segment is symbolized to the same whole-segment writable run
    // the indexed accesses use (tryResolveGlobalData -> embedWritableRun), so a
    // &G broadcast into a SIMD lane (clang's `tab[i]=&G` vectorization) carries
    // a relocatable @G that survives relinking instead of the stale original
    // VA.
    if (symbolizesWritableRelocPtr(V.ConstVal, V.Size)) {
      if (auto *Global = tryResolveGlobalData(V.ConstVal))
        return Builder.CreatePtrToInt(Global, sizeToType(V.Size), "wgdata");
      return failUnresolvableDataConstant();
    }
    // A pointer-SELECT peer of an already-symbolized same-segment global —
    // `cond ? &A : &B` where &B relocates but &A is a PC-relative-only (AArch64
    // ADRP) sibling the loader left out of WritableRelocDataAddrs.  Symbolizing
    // it keeps both arms on one addressing model (gated inside on
    // used-as-pointer
    // + not a walked base).
    if (V.Size == 0 || PtrSz == 0 || V.Size >= PtrSz)
      if (symbolizesSelectPeer(V.ConstVal)) {
        if (auto *Global = tryResolveGlobalData(V.ConstVal))
          return Builder.CreatePtrToInt(Global, sizeToType(V.Size),
                                        "wgdatapeer");
        return failUnresolvableDataConstant();
      }
    if (!canResolveGlobalDataConstant(V.ConstVal)) {
      const Segment *Seg = Img ? Img->getSegmentFor(V.ConstVal) : nullptr;
      bool AnchoredIntent = Img && Img->RodataAnchorSeg.count(V.ConstVal) &&
                            !constValueUsedAsInteger(V.ConstVal) &&
                            !addrInCodePtrMirrorRun(V.ConstVal);
      bool ReadableDirectIntent =
          Seg && Seg->isReadable() &&
          getVarHasDirectDataSymbolizationIntent(V.ConstVal, V.Size) &&
          (constUsedAsPointer(V.ConstVal) ||
           (Img && Img->RelocDataAddrs.count(V.ConstVal)));
      bool RodataEndIntent = constIsRodataEndPointer(V.ConstVal) &&
                             !constValueUsedAsInteger(V.ConstVal) &&
                             !addrInCodePtrMirrorRun(V.ConstVal);
      bool WritableIntent =
          Seg && Seg->isReadable() &&
          writableRelocPtrHasSymbolizationIntent(V.ConstVal, V.Size);
      if (AnchoredIntent || ReadableDirectIntent || RodataEndIntent ||
          WritableIntent)
        return failUnresolvableDataConstant();
    }
    // Materialize the constant's raw Size-byte bit pattern.  V.ConstVal carries
    // the value in its low Size*8 bits, so a narrow constant whose top bit is
    // set (e.g. a 4-byte 0x9E3779B9) is larger than the signed range of that
    // width; passing it down the old isSigned int64 path now trips APInt's
    // "not an N-bit signed value" assertion.  Build the APInt at the exact
    // width with implicit truncation so the low bits are taken verbatim —
    // correct for both zero-extended unsigned constants and sign-extended
    // negative ones.
    auto *IntTy = llvm::cast<llvm::IntegerType>(sizeToType(V.Size));
    return llvm::ConstantInt::get(
        *Ctx, llvm::APInt(IntTy->getBitWidth(), V.ConstVal, /*isSigned=*/false,
                          /*implicitTrunc=*/true));
  }

  auto Key = std::make_pair(V.Id, V.SSAVer);

  auto ClobberIt = CurMedFunc
                       ? std::find_if(CurMedFunc->CallClobbers.begin(),
                                      CurMedFunc->CallClobbers.end(),
                                      [&](const MedCallClobber &Clobber) {
                                        return Clobber.Value == V;
                                      })
                       : std::vector<MedCallClobber>::const_iterator{};
  if (CurMedFunc && ClobberIt != CurMedFunc->CallClobbers.end()) {
    auto Alloc = VarAllocs.find(Key);
    assert(Alloc != VarAllocs.end() &&
           "call-clobber storage must be pre-created");
    auto *BaseTy = sizeToType(ClobberIt->Value.Size);
    llvm::Value *Base = Builder.CreateLoad(BaseTy, Alloc->second,
                                           V.display() + "_call_clobber_v");
    auto *WantTy = sizeToType(V.Size);
    if (Base->getType() != WantTy)
      Base = Builder.CreateZExtOrTrunc(Base, WantTy,
                                       V.display() + "_call_clobber_view");
    return Base;
  }

  auto AllocIt = VarAllocs.find(Key);
  bool HasLocalDef = (AllocIt != VarAllocs.end());

  // A single wide read that spans several adjacent i386 cdecl stack-argument
  // slots — clang -O2 forwards N consecutive 4-byte arguments with one
  // movaps/movups — resolves to the concatenation of the N parameters it
  // covers, not a bare zero-extension of the base slot (which would drop the
  // upper arguments).  Gated on N consecutive stack parameters actually
  // existing, so a genuine single wide parameter is left to the normal path.
  if (V.Kind == MedVar::Param && V.RegOff == kNoParamReg && CurMedFunc) {
    unsigned Slot = getTargetRegInfo(TargetArch).PointerSize;
    if (Slot > 0 && V.Size > Slot && (V.Size % Slot) == 0 &&
        V.Size / Slot <= 64) {
      unsigned N = V.Size / Slot;
      bool AllPresent = true;
      for (unsigned K = 0; K < N && AllPresent; ++K) {
        bool Has = false;
        for (const auto &P : CurMedFunc->Params)
          if (P.Kind == MedVar::Param && P.RegOff == kNoParamReg &&
              P.Id == V.Id + static_cast<int>(K)) {
            Has = true;
            break;
          }
        AllPresent = Has;
      }
      if (AllPresent && N > 1) {
        auto *WideTy = sizeToType(V.Size);
        llvm::Value *Acc = nullptr;
        for (unsigned K = 0; K < N; ++K) {
          MedVar Sub;
          Sub.Kind = MedVar::Param;
          Sub.Id = V.Id + static_cast<int>(K);
          Sub.Size = static_cast<uint16_t>(Slot);
          Sub.RegOff = kNoParamReg;
          Sub.TheArch = V.TheArch;
          llvm::Value *Lane = Builder.CreateZExt(getVar(Sub, Builder), WideTy);
          if (K > 0)
            Lane = Builder.CreateShl(
                Lane, llvm::ConstantInt::get(WideTy, static_cast<uint64_t>(K) *
                                                         Slot * 8));
          Acc = Acc ? Builder.CreateOr(Acc, Lane) : Lane;
        }
        return Acc;
      }
    }
  }

  // A Param is always a function argument, never a local — resolve it to its
  // incoming LLVM argument before any local-alloca lookup.  A stack parameter's
  // synthetic Id (its argument index) can collide with an unrelated SSA
  // variable's Id, and the alloca for that variable must not shadow the param.
  // Register-backed parameters are keyed by their architectural register:
  // promoted forwarders deliberately share the synthetic Id -1, so their
  // display names are not unique.
  if (V.Kind == MedVar::Param || (!HasLocalDef && V.SSAVer == 0)) {
    llvm::Value *ArgVal = nullptr;
    if ((V.Kind == MedVar::Param || V.Kind == MedVar::Reg) &&
        V.RegOff != kNoParamReg) {
      auto RIt = ParamRegoffMap.find(V.RegOff);
      if (RIt != ParamRegoffMap.end())
        ArgVal = RIt->second;
    }
    if (!ArgVal) {
      auto PIt = ParamArgs.find(V.display());
      if (PIt != ParamArgs.end())
        ArgVal = PIt->second;
    }
    if (ArgVal) {
      unsigned WantBits = V.Size > 0 ? V.Size * 8 : 64;
      auto *WantTy = llvm::IntegerType::get(*Ctx, WantBits);
      if (ArgVal->getType()->isPointerTy())
        return Builder.CreatePtrToInt(ArgVal, WantTy);
      if (ArgVal->getType() == WantTy)
        return ArgVal;
      if (ArgVal->getType()->getPrimitiveSizeInBits() == WantBits)
        return Builder.CreateBitCast(ArgVal, WantTy, "vec2int");
      if (ArgVal->getType()->isIntegerTy()) {
        if (ArgVal->getType()->getIntegerBitWidth() > WantBits)
          return Builder.CreateTrunc(ArgVal, WantTy);
        return Builder.CreateZExt(ArgVal, WantTy);
      }
      // A vector/FP argument (e.g. the <2 x i64> ABI form of an XMM/V register)
      // read at a different register-slice width: bridge through an integer of
      // the argument's own width, then narrow (a scalar in the low lane) or
      // widen.  A direct bitcast is only valid at equal width (handled above).
      if (unsigned ArgBits = ArgVal->getType()->getPrimitiveSizeInBits()) {
        llvm::Value *AsInt = Builder.CreateBitCast(
            ArgVal, llvm::IntegerType::get(*Ctx, ArgBits), "arg_bits");
        if (ArgBits > WantBits)
          return Builder.CreateTrunc(AsInt, WantTy);
        if (ArgBits < WantBits)
          return Builder.CreateZExt(AsInt, WantTy);
        return AsInt;
      }
      return Builder.CreateBitCast(ArgVal, WantTy, "arg_cast");
    }
  }

  if (HasLocalDef) {
    auto *LoadedTy = sizeToType(V.Size);
    return Builder.CreateLoad(LoadedTy, AllocIt->second, V.display() + "_v");
  }

  if (FrameBaseInt && V.Kind == MedVar::Reg && V.SSAVer == 0) {
    uint64_t SpOff = getTargetRegInfo(TargetArch).StackPointer;
    if (SpOff != 0 && V.RegOff == SpOff) {
      unsigned WantBits = V.Size > 0 ? V.Size * 8 : 64;
      auto *Val = FrameBaseInt;
      if (Val->getType()->getIntegerBitWidth() != WantBits) {
        auto *WantTy = llvm::IntegerType::get(*Ctx, WantBits);
        if (Val->getType()->getIntegerBitWidth() > WantBits)
          Val = Builder.CreateTrunc(Val, WantTy);
        else
          Val = Builder.CreateZExt(Val, WantTy);
      }
      return Val;
    }
  }

  // Sub-register fallback: when a small register (e.g. ECX, size=4)
  // has no definition but a larger overlapping register (RCX, size=8)
  // was defined at the same offset, truncate from the larger alloca.
  if (V.Kind == MedVar::Reg && V.Size > 0) {
    const auto &TRI = getTargetRegInfo(TargetArch);
    auto [WideOff, WideSz] = TRI.findWideReg(V.RegOff, V.Size);
    if (WideSz > V.Size && CurMedFunc) {
      for (auto &Blk : CurMedFunc->Blocks) {
        for (auto &Op : Blk.Ops) {
          if (Op.Output.Kind != MedVar::Reg || Op.Output.Size == 0)
            continue;
          if (Op.Output.RegOff == WideOff && Op.Output.Size == WideSz) {
            auto WKey = std::make_pair(Op.Output.Id, Op.Output.SSAVer);
            auto WIt = VarAllocs.find(WKey);
            if (WIt != VarAllocs.end()) {
              auto *WantTy = sizeToType(V.Size);
              auto *WideVal = Builder.CreateLoad(
                  sizeToType(WideSz), WIt->second, V.display() + "_wide");
              return Builder.CreateTrunc(WideVal, WantTy, V.display() + "_sub");
            }
          }
        }
        for (auto &Phi : Blk.Phis) {
          if (Phi.Output.Kind != MedVar::Reg || Phi.Output.Size == 0)
            continue;
          if (Phi.Output.RegOff == WideOff && Phi.Output.Size == WideSz) {
            auto WKey = std::make_pair(Phi.Output.Id, Phi.Output.SSAVer);
            auto WIt = VarAllocs.find(WKey);
            if (WIt != VarAllocs.end()) {
              auto *WantTy = sizeToType(V.Size);
              auto *WideVal = Builder.CreateLoad(
                  sizeToType(WideSz), WIt->second, V.display() + "_wide");
              return Builder.CreateTrunc(WideVal, WantTy, V.display() + "_sub");
            }
          }
        }
      }
    }

    // Pre-collect (once, O(ops)) the allocas that some wider Reg op/phi output
    // at THIS RegOff maps to, so the per-alloca test below is an O(1) set
    // lookup instead of a full-function rescan per alloca (was O(allocas * ops)
    // — the getVar hot path on large NEON functions, e.g. armv48_biggather).
    // The outer VarAllocs iteration order is preserved, so the alloca selected
    // is exactly the one the previous nested scan would have returned.
    const unsigned WantBits = V.Size * 8;
    std::set<llvm::AllocaInst *> WiderRegAllocas;
    if (CurMedFunc) {
      auto collectWider = [&](const MedVar &Out) {
        if (Out.Kind != MedVar::Reg || Out.RegOff != V.RegOff ||
            Out.Size <= V.Size)
          return;
        auto OIt = VarAllocs.find(std::make_pair(Out.Id, Out.SSAVer));
        if (OIt != VarAllocs.end())
          WiderRegAllocas.insert(OIt->second);
      };
      for (auto &Blk : CurMedFunc->Blocks) {
        for (auto &Op : Blk.Ops)
          collectWider(Op.Output);
        for (auto &Phi : Blk.Phis)
          collectWider(Phi.Output);
      }
    }

    for (auto &[AKey, AAlloca] : VarAllocs) {
      (void)AKey;
      auto *AllocTy = AAlloca->getAllocatedType();
      if (!AllocTy->isIntegerTy())
        continue;
      unsigned AllocBits = AllocTy->getIntegerBitWidth();
      if (AllocBits <= WantBits)
        continue;
      if (!WiderRegAllocas.count(AAlloca))
        continue;
      auto *WantTy = sizeToType(V.Size);
      auto *WideVal =
          Builder.CreateLoad(AllocTy, AAlloca, V.display() + "_wide");
      return Builder.CreateTrunc(WideVal, WantTy, V.display() + "_sub");
    }
  }

  // A live-in read of the link register (AArch64 x30 / ARM LR) used as a *data*
  // value is the function's return address — `__builtin_return_address(0)`.  A
  // leaf such as `void *ra(void){return __builtin_return_address(0);}` lifts to
  // `mov x0, x30` (x0 = the incoming LR), i.e. a value use of the live-in LR;
  // without modeling it the zero-init fallback below silently yields 0 (so a
  // `ra() != 0` guard wrongly fails).  Materialize `llvm.returnaddress(0)` so
  // the rewritten function returns its own valid, non-null return address.  The
  // intrinsic is pure (memory(none)); where the live-in LR only feeds a dead
  // prologue spill it is folded away by DCE, so this is a no-op outside genuine
  // return-address uses.  x86/x64 keep the return address on the stack
  // (LinkRegister == 0), so isLinkRegister() restricts this to AArch64/ARM;
  // only the full-pointer-width read is the return address.
  if (V.Kind == MedVar::Reg && V.SSAVer == 0) {
    const auto &TRI = getTargetRegInfo(TargetArch);
    if (TRI.isLinkRegister(V.RegOff) && TRI.PointerSize > 0 &&
        V.Size == TRI.PointerSize) {
      // `llvm.returnaddress` returns `llvm_anyptr_ty` (overloaded on the result
      // pointer type, matching upstream LLVM), so the concrete `ptr` must be
      // supplied as the overload type — `@llvm.returnaddress.p0(i32 0)`.
      auto *PtrTy = llvm::PointerType::getUnqual(*Ctx);
      auto *RA = Builder.CreateIntrinsic(
          llvm::Intrinsic::returnaddress, {PtrTy},
          {llvm::ConstantInt::get(llvm::Type::getInt32Ty(*Ctx), 0)});
      llvm_value_provenance::markSemanticProducer(*RA);
      return Builder.CreatePtrToInt(RA, sizeToType(V.Size), "retaddr");
    }
  }

  auto &Entry = CurFunc->getEntryBlock();
  llvm::IRBuilder<> AllocBuilder(&Entry, Entry.begin());
  auto *Ty = sizeToType(V.Size);
  auto *Alloca = AllocBuilder.CreateAlloca(Ty, nullptr, V.display());
  AllocBuilder.CreateStore(llvm::ConstantInt::get(Ty, 0), Alloca);
  VarAllocs[Key] = Alloca;
  return Builder.CreateLoad(Ty, Alloca, V.display() + "_v");
}

void MedLLVMEmitter::propagateToSubRegs(uint64_t RegOff, uint16_t WriteSize,
                                        llvm::Value *Val,
                                        llvm::IRBuilder<> &Builder) {
  auto It = SubRegPropMap.find(RegOff);
  if (It == SubRegPropMap.end())
    return;
  unsigned WriteBits = WriteSize * 8;
  for (auto &Info : It->second) {
    if (Info.Bits >= WriteBits)
      continue;
    auto *NarrowTy = llvm::IntegerType::get(*Ctx, Info.Bits);
    llvm::Value *Trunced = Val;
    if (Val->getType()->getIntegerBitWidth() > Info.Bits)
      Trunced = Builder.CreateTrunc(Val, NarrowTy, "subreg_prop");
    else if (Val->getType()->getIntegerBitWidth() < Info.Bits)
      Trunced = Builder.CreateZExt(Val, NarrowTy);
    Builder.CreateStore(Trunced, Info.Alloca);
  }
}

void MedLLVMEmitter::setVar(const MedVar &V, llvm::Value *Val,
                            llvm::IRBuilder<> &Builder) {
  if (V.isConst() || V.Size == 0)
    return;

  auto Key = std::make_pair(V.Id, V.SSAVer);
  auto It = VarAllocs.find(Key);
  llvm::AllocaInst *Alloca;
  if (It != VarAllocs.end()) {
    Alloca = It->second;
  } else {
    auto &Entry = CurFunc->getEntryBlock();
    llvm::IRBuilder<> AllocBuilder(&Entry, Entry.begin());
    auto *Ty = sizeToType(V.Size);
    Alloca = AllocBuilder.CreateAlloca(Ty, nullptr, V.display());
    VarAllocs[Key] = Alloca;
  }

  auto *TargetTy = sizeToType(V.Size);
  if (Val->getType() != TargetTy) {
    if (Val->getType()->isPointerTy())
      Val = Builder.CreatePtrToInt(Val, TargetTy);
    else if (Val->getType()->isFloatingPointTy() && TargetTy->isIntegerTy()) {
      unsigned FPBits = Val->getType()->getPrimitiveSizeInBits();
      unsigned IntBits = TargetTy->getIntegerBitWidth();
      auto *SameWidthInt = llvm::Type::getIntNTy(*Ctx, FPBits);
      Val = Builder.CreateBitCast(Val, SameWidthInt);
      if (FPBits < IntBits)
        Val = Builder.CreateZExt(Val, TargetTy);
      else if (FPBits > IntBits)
        Val = Builder.CreateTrunc(Val, TargetTy);
    } else if (Val->getType()->isIntegerTy() && TargetTy->isIntegerTy()) {
      if (Val->getType()->getIntegerBitWidth() > TargetTy->getIntegerBitWidth())
        Val = Builder.CreateTrunc(Val, TargetTy);
      else
        Val = Builder.CreateZExt(Val, TargetTy);
    } else if (!Val->getType()->isPointerTy() && !TargetTy->isPointerTy() &&
               Val->getType()->getPrimitiveSizeInBits() ==
                   TargetTy->getPrimitiveSizeInBits()) {
      // Same-width reinterpret (e.g. an FP/vector call result typed <2 x i64>
      // stored into the i128-tracked XMM return register).
      Val = Builder.CreateBitCast(Val, TargetTy);
    } else if (!Val->getType()->isPointerTy() && !TargetTy->isPointerTy() &&
               TargetTy->isIntegerTy() &&
               Val->getType()->getPrimitiveSizeInBits() >
                   TargetTy->getIntegerBitWidth()) {
      // A wide vector/FP value (e.g. a <2 x i64> call result) stored into a
      // narrower register-slice variable (the 8-byte D0 view of V0 when the
      // caller only ever reads the scalar low lane): reinterpret to an integer
      // of the source width and take the low bytes.
      unsigned WBits = Val->getType()->getPrimitiveSizeInBits();
      llvm::Value *AsInt =
          Val->getType()->isIntegerTy()
              ? Val
              : Builder.CreateBitCast(Val, llvm::Type::getIntNTy(*Ctx, WBits));
      Val = Builder.CreateTrunc(AsInt, TargetTy);
    }
  }
  Builder.CreateStore(Val, Alloca);

  if (V.Kind == MedVar::Reg && V.Size > 0 && Val->getType()->isIntegerTy()) {
    propagateToSubRegs(V.RegOff, V.Size, Val, Builder);
  }
}

} // namespace neverd
