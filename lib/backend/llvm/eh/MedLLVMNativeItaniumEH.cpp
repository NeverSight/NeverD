//===- MedLLVMNativeItaniumEH.cpp - Native Itanium EH -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Native LLVM landing-pad and invoke lowering for decoded Itanium EH tables.
///
//===----------------------------------------------------------------------===//

#include "MedLLVMEHHelpers.h"

#include "neverd/Common.h"
#include "neverd/backend/ExceptionRewriteContract.h"
#include "neverd/backend/llvm/LLVMName.h"
#include "neverd/backend/llvm/LanguageEHMetadata.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/loader/ExceptionInfo.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Metadata.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace neverd {

using med_llvm_eh::mdUInt;

namespace {

/// The canonical symbol for each Itanium personality NeverD can lower.  The
/// SJLJ personality is deliberately absent: its call-site table indexes call
/// sites rather than addresses, so nothing in it names code an `invoke` could
/// protect, and `IsCallSiteAddressForm` already reports that.
const char *itaniumPersonalitySymbol(ExceptionPersonality P) {
  switch (P) {
  case ExceptionPersonality::GxxPersonalityV0:
    return "__gxx_personality_v0";
  case ExceptionPersonality::GxxPersonalitySEH0:
    return "__gxx_personality_seh0";
  case ExceptionPersonality::GccPersonalityV0:
    return "__gcc_personality_v0";
  case ExceptionPersonality::GccPersonalitySEH0:
    return "__gcc_personality_seh0";
  case ExceptionPersonality::ObjCPersonalityV0:
    return "__objc_personality_v0";
  case ExceptionPersonality::RustEhPersonality:
    return "rust_eh_personality";
  case ExceptionPersonality::GnatPersonalityV0:
    return "__gnat_personality_v0";
  case ExceptionPersonality::GnatPersonalitySEH0:
    return "__gnat_personality_seh0";
  case ExceptionPersonality::DmdPersonalityV0:
    return "__dmd_personality_v0";
  case ExceptionPersonality::DRuntimeEhPersonality:
    return "_d_eh_personality";
  case ExceptionPersonality::GdcPersonalityV0:
    return "__gdc_personality_v0";
  case ExceptionPersonality::GdcPersonalitySEH0:
    return "__gdc_personality_seh0";
  default:
    return nullptr;
  }
}

} // namespace

bool MedLLVMEmitter::emitNativeItaniumEH(
    const MedFunc &Func, llvm::Function &LLVMFunc,
    const std::map<int, llvm::BasicBlock *> &OriginalBlockMap) {
  if (!Func.ExceptionMetadata)
    return false;
  const ExceptionFunction &EH = *Func.ExceptionMetadata;
  const char *PersonalitySymbol = itaniumPersonalitySymbol(EH.Personality);
  if (!PersonalitySymbol || !EH.Itanium || !EH.Itanium->IsCallSiteAddressForm ||
      EH.ParseStatus != ExceptionParseStatus::Complete)
    return false;
  const ItaniumEHInfo &Itanium = *EH.Itanium;

  // Preflight is deliberately data-only.  In particular it does not resolve
  // type-info globals, edit noreturn edges, create unwind blocks, or
  // materialize exceptional phi copies.  A false return therefore leaves the
  // ordinary CFG exactly as the emitter built it.
  if (!EH.CodeRange.isValid())
    return false;

  // The native addresses below are related to LLVM blocks through this map.
  // Treat it as a bijection over the recovered source blocks: silently
  // skipping a duplicate id, a missing entry, or two ids naming one LLVM block
  // can otherwise attach an invoke to the wrong predecessor.
  if (OriginalBlockMap.size() != Func.Blocks.size())
    return false;
  std::set<int> SourceBlockIDs;
  std::set<const llvm::BasicBlock *> MappedBlocks;
  for (const MedBlock &Block : Func.Blocks) {
    if (!SourceBlockIDs.insert(Block.Id).second)
      return false;
    auto Mapped = OriginalBlockMap.find(Block.Id);
    if (Mapped == OriginalBlockMap.end() || !Mapped->second ||
        Mapped->second->getParent() != &LLVMFunc ||
        !MappedBlocks.insert(Mapped->second).second)
      return false;
  }

  for (const ItaniumCallSite &Site : Itanium.CallSites)
    if (!Site.GuardedRange.isValid() ||
        !EH.CodeRange.contains(Site.GuardedRange))
      return false;

  // Address-form LSDA call-site rows partition code.  Overlapping rows do not
  // have a defined priority in the ABI, so choosing an "innermost" row would
  // invent semantics.  Equal endpoints are valid adjacent ranges.
  std::vector<const ItaniumCallSite *> OrderedSites;
  OrderedSites.reserve(Itanium.CallSites.size());
  for (const ItaniumCallSite &Site : Itanium.CallSites)
    OrderedSites.push_back(&Site);
  std::sort(
      OrderedSites.begin(), OrderedSites.end(),
      [](const ItaniumCallSite *Left, const ItaniumCallSite *Right) {
        return std::tie(Left->GuardedRange.Begin, Left->GuardedRange.End) <
               std::tie(Right->GuardedRange.Begin, Right->GuardedRange.End);
      });
  for (size_t I = 1; I < OrderedSites.size(); ++I)
    if (OrderedSites[I - 1]->GuardedRange.End >
        OrderedSites[I]->GuardedRange.Begin)
      return false;

  std::map<uint64_t, const ItaniumAction *> Actions;
  for (const ItaniumAction &Action : Itanium.Actions)
    if (!Actions.emplace(Action.TableOffset, &Action).second)
      return false;
  std::map<uint64_t, const ItaniumTypeEntry *> Types;
  for (const ItaniumTypeEntry &Type : Itanium.TypeTable)
    if (!Types.emplace(Type.Index, &Type).second)
      return false;
  std::map<uint64_t, const ItaniumExceptionSpec *> Specs;
  for (const ItaniumExceptionSpec &Spec : Itanium.ExceptionSpecs)
    if (!Specs.emplace(Spec.Index, &Spec).second)
      return false;

  enum class ClauseKind : uint8_t { Catch, Filter };
  struct ClausePlan {
    ClauseKind Kind = ClauseKind::Catch;
    std::vector<const ItaniumTypeEntry *> Types;
  };
  struct PadSemantics {
    bool Cleanup = false;
    std::vector<ClausePlan> Clauses;
  };
  auto SameSemantics = [](const PadSemantics &Left, const PadSemantics &Right) {
    if (Left.Cleanup != Right.Cleanup ||
        Left.Clauses.size() != Right.Clauses.size())
      return false;
    for (size_t I = 0; I < Left.Clauses.size(); ++I) {
      const ClausePlan &A = Left.Clauses[I];
      const ClausePlan &B = Right.Clauses[I];
      if (A.Kind != B.Kind || A.Types != B.Types)
        return false;
    }
    return true;
  };
  auto ResolveType = [&](uint64_t Index) -> const ItaniumTypeEntry * {
    auto It = Types.find(Index);
    if (It == Types.end())
      return nullptr;
    const ItaniumTypeEntry *Type = It->second;
    if (!Type->IsCatchAll && Type->TypeInfoSlotVA == 0 && Type->TypeInfoVA == 0)
      return nullptr;
    return Type;
  };
  auto PlanSemantics = [&](const ItaniumCallSite &Site, PadSemantics &Out) {
    if (!Site.FirstActionOffset) {
      Out.Cleanup = true;
      return true;
    }
    std::set<uint64_t> Visited;
    std::optional<uint64_t> Offset = Site.FirstActionOffset;
    while (Offset) {
      if (!Visited.insert(*Offset).second)
        return false;
      auto ActionIt = Actions.find(*Offset);
      if (ActionIt == Actions.end())
        return false;
      const ItaniumAction &Action = *ActionIt->second;
      if (Action.isCleanup()) {
        Out.Cleanup = true;
      } else if (Action.isCatch()) {
        const ItaniumTypeEntry *Type =
            ResolveType(static_cast<uint64_t>(Action.TypeFilter));
        if (!Type)
          return false;
        Out.Clauses.push_back({ClauseKind::Catch, {Type}});
      } else {
        if (Action.TypeFilter == std::numeric_limits<int64_t>::min())
          return false;
        const uint64_t SpecIndex = static_cast<uint64_t>(-Action.TypeFilter);
        auto SpecIt = Specs.find(SpecIndex);
        if (SpecIt == Specs.end())
          return false;
        ClausePlan Clause;
        Clause.Kind = ClauseKind::Filter;
        for (uint64_t Index : SpecIt->second->TypeIndices) {
          const ItaniumTypeEntry *Type = ResolveType(Index);
          if (!Type)
            return false;
          Clause.Types.push_back(Type);
        }
        Out.Clauses.push_back(std::move(Clause));
      }
      Offset = Action.NextActionOffset;
    }
    if (Out.Clauses.empty())
      Out.Cleanup = true;
    return true;
  };

  // Parse every source call-site action chain, including sites not selected by
  // emitted calls.  Unused handlers need no IR block, but malformed source
  // metadata must never be reported as completely lowered.
  std::map<const ItaniumCallSite *, PadSemantics> SiteSemantics;
  for (const ItaniumCallSite &Site : Itanium.CallSites) {
    if (Site.LandingPadVA == 0) {
      if (Site.FirstActionOffset)
        return false;
      continue;
    }
    if (!EH.CodeRange.contains(Site.LandingPadVA))
      return false;
    PadSemantics Semantics;
    if (!PlanSemantics(Site, Semantics))
      return false;
    SiteSemantics.emplace(&Site, std::move(Semantics));
  }

  // Every source CALL that became an LLVM CallInst carries an independent,
  // transient address marker. Validate the complete map before either copy
  // can influence protected-call selection.
  auto MarkedSourceCalls =
      med_llvm_eh::collectExactSourceCallAddresses(LLVMFunc, CallSiteAddrs);
  if (!MarkedSourceCalls)
    return false;

  struct ProtectedCall {
    llvm::CallInst *Call = nullptr;
    int PredId = -1;
    const ItaniumCallSite *Site = nullptr;
  };
  std::vector<ProtectedCall> ProtectedCalls;
  std::set<const llvm::CallInst *> SeenProtectedCalls;

  // A predicated ARM call is emitted in a synthetic effect block rather than
  // in OriginalBlockMap's entry for its source MedBlock.  Recover ownership
  // from the source identity carried by MedIR and the transient LLVM marker,
  // not from the LLVM block that happens to contain the call.  Requiring a
  // one-to-one source address keeps exceptional phi selection exact: two
  // source calls at one address, or two markers claiming one source call,
  // cannot be assigned a predecessor without inventing semantics.
  std::map<va_t, int> SourceCallOwners;
  for (const MedBlock &MedBB : Func.Blocks)
    for (const MedOp &Op : MedBB.Ops) {
      if (Op.Opcode != NdOp::CALL && Op.Opcode != NdOp::INDIR_CALL)
        continue;
      if (!SourceCallOwners.emplace(Op.Addr, MedBB.Id).second)
        return false;
    }
  std::set<va_t> SeenSourceAddresses;

  auto InnermostSite = [&](va_t Address,
                           bool &Ambiguous) -> const ItaniumCallSite * {
    const ItaniumCallSite *Best = nullptr;
    uint64_t BestSize = std::numeric_limits<uint64_t>::max();
    Ambiguous = false;
    for (const ItaniumCallSite &Site : Itanium.CallSites) {
      if (!Site.GuardedRange.contains(Address))
        continue;
      const uint64_t Size = Site.GuardedRange.size();
      if (Size < BestSize) {
        Best = &Site;
        BestSize = Size;
        Ambiguous = false;
      } else if (Size == BestSize) {
        Ambiguous = true;
      }
    }
    return Best;
  };

  for (const auto &[MarkedCall, Address] : *MarkedSourceCalls) {
    auto Owner = SourceCallOwners.find(Address);
    if (Owner == SourceCallOwners.end() ||
        !SeenSourceAddresses.insert(Address).second)
      return false;
    auto *Call = const_cast<llvm::CallInst *>(MarkedCall);
    if (!Call->getParent() || Call->getFunction() != &LLVMFunc)
      return false;
    if (Call->doesNotThrow() || Call->isMustTailCall() ||
        llvm::isa<llvm::IntrinsicInst>(Call))
      continue;
    bool Ambiguous = false;
    const ItaniumCallSite *Site = InnermostSite(Address, Ambiguous);
    if (Ambiguous)
      return false;
    if (Site && Site->LandingPadVA != 0) {
      if (!SeenProtectedCalls.insert(Call).second)
        return false;
      ProtectedCalls.push_back({Call, Owner->second, Site});
    }
  }

  const uint64_t RequiredCalls = ProtectedCalls.size();
  if (RequiredCalls == 0) {
    LLVMFunc.setMetadata(
        language_eh_md::ItaniumAttachment,
        llvm::MDNode::get(*Ctx, {llvm::MDString::get(*Ctx, PersonalitySymbol),
                                 mdUInt(*Ctx, 0, 32), mdUInt(*Ctx, 0, 32),
                                 mdUInt(*Ctx, 0, 32), mdUInt(*Ctx, 0, 32)}));
    exception_rewrite::setContract(LLVMFunc,
                                   exception_rewrite::SourceState::Complete,
                                   exception_rewrite::LoweringState::Complete);
    return true;
  }

  struct PadPlan {
    va_t Address = 0;
    llvm::BasicBlock *Block = nullptr;
    int BlockId = -1;
    PadSemantics Semantics;
    llvm::BasicBlock *UnwindBlock = nullptr;
    std::vector<llvm::Constant *> Clauses;
  };
  struct CallPlan {
    llvm::CallInst *Call = nullptr;
    int PredId = -1;
    PadPlan *Pad = nullptr;
  };
  std::vector<std::unique_ptr<PadPlan>> Pads;
  std::map<const ItaniumCallSite *, PadPlan *> SitePads;
  std::vector<CallPlan> Calls;
  Calls.reserve(ProtectedCalls.size());

  auto ResolveHandler = [&](va_t Address, llvm::BasicBlock *&Block,
                            int &BlockId) {
    const MedBlock *Match = nullptr;
    for (const MedBlock &Candidate : Func.Blocks) {
      if (Candidate.StartAddr != Address)
        continue;
      if (Match)
        return false;
      Match = &Candidate;
    }
    if (!Match)
      return false;
    auto It = OriginalBlockMap.find(Match->Id);
    if (It == OriginalBlockMap.end() ||
        It->second == &LLVMFunc.getEntryBlock() || It->second->isLandingPad())
      return false;
    Block = It->second;
    BlockId = Match->Id;
    return true;
  };

  for (const ProtectedCall &Protected : ProtectedCalls) {
    PadPlan *Pad = nullptr;
    auto Known = SitePads.find(Protected.Site);
    if (Known != SitePads.end()) {
      Pad = Known->second;
    } else {
      auto PlannedSemantics = SiteSemantics.find(Protected.Site);
      if (PlannedSemantics == SiteSemantics.end())
        return false;
      PadSemantics Semantics = PlannedSemantics->second;
      for (const std::unique_ptr<PadPlan> &Candidate : Pads)
        if (Candidate->Address == Protected.Site->LandingPadVA &&
            SameSemantics(Candidate->Semantics, Semantics)) {
          Pad = Candidate.get();
          break;
        }
      if (!Pad) {
        auto NewPad = std::make_unique<PadPlan>();
        NewPad->Address = Protected.Site->LandingPadVA;
        NewPad->Semantics = std::move(Semantics);
        if (!ResolveHandler(NewPad->Address, NewPad->Block, NewPad->BlockId))
          return false;
        Pad = NewPad.get();
        Pads.push_back(std::move(NewPad));
      }
      SitePads.emplace(Protected.Site, Pad);
    }

    if (!Protected.Call->getParent() || !Protected.Call->getNextNode() ||
        !Protected.Call->getParent()->getTerminator())
      return false;
    const MedBlock *Target = nullptr;
    for (const MedBlock &Block : Func.Blocks)
      if (Block.Id == Pad->BlockId) {
        Target = &Block;
        break;
      }
    if (!Target)
      return false;
    for (const PhiNode &Phi : Target->Phis)
      for (const auto &[IncomingPred, Incoming] : Phi.Args)
        if (IncomingPred == Protected.PredId &&
            (Phi.Output.isConst() || Phi.Output.Size == 0 ||
             Incoming.Size == 0))
          return false;
    Calls.push_back({Protected.Call, Protected.PredId, Pad});
  }
  if (Calls.size() != RequiredCalls)
    return false;

  auto *PtrTy = llvm::PointerType::get(*Ctx, 0);
  auto *I8Ty = llvm::Type::getInt8Ty(*Ctx);
  auto *I32Ty = llvm::Type::getInt32Ty(*Ctx);
  auto *PersonalityTy = llvm::FunctionType::get(I32Ty, {}, true);

  auto typeInfoNameForLLVM = [&](const ItaniumTypeEntry &Entry) {
    llvm::StringRef Name(Entry.TypeName);
    if (!Img || TargetFormat != BinaryFormat::MachO || Name.empty())
      return Name.str();

    bool IsObjectName = false;
    for (const Symbol &Sym : Img->Symbols)
      if (Sym.Addr == Entry.TypeInfoVA && Sym.Name == Name) {
        IsObjectName = true;
        break;
      }
    if (!IsObjectName && Entry.TypeInfoSlotVA != 0) {
      if (const Import *Imp = Img->findImportAt(Entry.TypeInfoSlotVA))
        IsObjectName = Imp->Name == Name;
      if (!IsObjectName)
        if (auto It = Img->ImportPtrSlots.find(Entry.TypeInfoSlotVA);
            It != Img->ImportPtrSlots.end())
          IsObjectName = It->second == Name;
    }
    if (!IsObjectName)
      for (const RelocationEntry &Rel : Img->Relocations)
        if ((Rel.Address == Entry.TypeInfoVA ||
             (Entry.TypeInfoSlotVA != 0 &&
              Rel.Address == Entry.TypeInfoSlotVA)) &&
            Rel.SymbolName == Name) {
          IsObjectName = true;
          break;
        }

    return IsObjectName ? llvm_name::fromObjectSymbol(Name, TargetFormat).str()
                        : Name.str();
  };

  // getOrInsertFunction and GlobalVariable construction can otherwise create
  // bitcasted or auto-renamed symbols after the CFG has already been edited.
  // Check every name and the source identity behind it while preflight is
  // still side-effect free.
  llvm::GlobalVariable *ImportedPersonalityPlaceholder = nullptr;
  if (llvm::GlobalValue *Existing = Mod->getNamedValue(PersonalitySymbol)) {
    if (const auto *Function = llvm::dyn_cast<llvm::Function>(Existing)) {
      if (Function->getFunctionType() != PersonalityTy)
        return false;
    } else {
      auto Placeholder = ImportedSymbolPlaceholders.find(PersonalitySymbol);
      if (Placeholder == ImportedSymbolPlaceholders.end() ||
          Placeholder->second != Existing ||
          !Placeholder->second->isDeclaration() ||
          Placeholder->second->getValueType() != I8Ty)
        return false;
      ImportedPersonalityPlaceholder = Placeholder->second;
    }
  }

  struct TypeInfoIdentity {
    bool IsSlot = false;
    va_t Address = 0;

    bool operator<(const TypeInfoIdentity &Other) const {
      return std::tie(IsSlot, Address) < std::tie(Other.IsSlot, Other.Address);
    }
    bool operator==(const TypeInfoIdentity &Other) const {
      return IsSlot == Other.IsSlot && Address == Other.Address;
    }
  };
  std::map<std::string, TypeInfoIdentity> IdentityByName;
  std::map<TypeInfoIdentity, std::string> NameByIdentity;
  std::set<const ItaniumTypeEntry *> ReferencedTypes;
  for (const std::unique_ptr<PadPlan> &Pad : Pads)
    for (const ClausePlan &Clause : Pad->Semantics.Clauses)
      ReferencedTypes.insert(Clause.Types.begin(), Clause.Types.end());
  for (const ItaniumTypeEntry *Entry : ReferencedTypes) {
    if (Entry->IsCatchAll)
      continue;
    const TypeInfoIdentity Identity{
        Entry->TypeInfoSlotVA != 0,
        Entry->TypeInfoSlotVA != 0 ? Entry->TypeInfoSlotVA : Entry->TypeInfoVA};
    llvm::StringRef DecodedName(Entry->TypeName);
    const bool IsRTTISymbol =
        getItaniumTypeTableEntryKind(EH.Personality) ==
            ItaniumTypeTableEntryKind::CxxRTTI &&
        (DecodedName.starts_with("_ZTI") || DecodedName.starts_with("__ZTI"));
    const std::string Name =
        Identity.IsSlot ? makeNdDataSymbol(Identity.Address)
                        : (IsRTTISymbol ? typeInfoNameForLLVM(*Entry)
                                        : makeNdDataSymbol(Identity.Address));
    auto [NameIt, NewName] = IdentityByName.emplace(Name, Identity);
    if (!NewName && !(NameIt->second == Identity))
      return false;
    auto [IdentityIt, NewIdentity] = NameByIdentity.emplace(Identity, Name);
    if (!NewIdentity && IdentityIt->second != Name)
      return false;
    if (llvm::GlobalValue *Existing = Mod->getNamedValue(Name)) {
      const auto *Global = llvm::dyn_cast<llvm::GlobalVariable>(Existing);
      if (!Global || Global->getValueType() != I8Ty)
        return false;
    }
  }

  // Commit begins here.  Every operation below is total for the preflighted
  // plan, so there is no rollback path and no partially lowered false return.
  auto TypeInfoConstant =
      [&](const ItaniumTypeEntry &Entry) -> llvm::Constant * {
    if (Entry.IsCatchAll)
      return llvm::ConstantPointerNull::get(PtrTy);
    if (Entry.TypeInfoSlotVA != 0) {
      std::string Name = makeNdDataSymbol(Entry.TypeInfoSlotVA);
      llvm::GlobalVariable *GV = Mod->getNamedGlobal(Name);
      if (!GV)
        GV = new llvm::GlobalVariable(*Mod, I8Ty, /*isConstant=*/true,
                                      llvm::GlobalValue::ExternalLinkage,
                                      /*Initializer=*/nullptr, Name);
      return GV;
    }
    if (llvm::Constant *Mapped = tryResolveGlobalData(Entry.TypeInfoVA, 1))
      return Mapped;
    llvm::StringRef DecodedName(Entry.TypeName);
    const bool IsRTTISymbol =
        getItaniumTypeTableEntryKind(EH.Personality) ==
            ItaniumTypeTableEntryKind::CxxRTTI &&
        (DecodedName.starts_with("_ZTI") || DecodedName.starts_with("__ZTI"));
    std::string Name = IsRTTISymbol ? typeInfoNameForLLVM(Entry)
                                    : makeNdDataSymbol(Entry.TypeInfoVA);
    llvm::GlobalVariable *GV = Mod->getNamedGlobal(Name);
    if (!GV)
      GV = new llvm::GlobalVariable(*Mod, I8Ty, /*isConstant=*/true,
                                    llvm::GlobalValue::ExternalLinkage, nullptr,
                                    Name);
    return GV;
  };

  for (const std::unique_ptr<PadPlan> &Pad : Pads) {
    for (const ClausePlan &Clause : Pad->Semantics.Clauses) {
      if (Clause.Kind == ClauseKind::Catch) {
        assert(Clause.Types.size() == 1);
        Pad->Clauses.push_back(TypeInfoConstant(*Clause.Types.front()));
        continue;
      }
      std::vector<llvm::Constant *> Permitted;
      Permitted.reserve(Clause.Types.size());
      for (const ItaniumTypeEntry *Type : Clause.Types)
        Permitted.push_back(TypeInfoConstant(*Type));
      Pad->Clauses.push_back(llvm::ConstantArray::get(
          llvm::ArrayType::get(PtrTy, Permitted.size()), Permitted));
    }
  }

  auto RemoveNoreturnFallthroughs = [](llvm::BasicBlock *PadBlock) {
    llvm::SmallVector<llvm::BasicBlock *, 2> Preds(llvm::pred_begin(PadBlock),
                                                   llvm::pred_end(PadBlock));
    for (llvm::BasicBlock *Pred : Preds) {
      auto *Branch =
          llvm::dyn_cast_or_null<llvm::UncondBrInst>(Pred->getTerminator());
      if (!Branch || Branch->getSuccessor(0) != PadBlock)
        continue;
      bool HasNoreturnCall = false;
      for (llvm::Instruction &Inst : *Pred) {
        if (&Inst == Branch)
          break;
        if (auto *Call = llvm::dyn_cast<llvm::CallBase>(&Inst);
            Call && Call->doesNotReturn()) {
          HasNoreturnCall = true;
          break;
        }
      }
      if (!HasNoreturnCall)
        continue;
      Branch->eraseFromParent();
      llvm::IRBuilder<> Builder(Pred);
      Builder.CreateUnreachable();
    }
  };

  std::set<llvm::BasicBlock *> CleanedHandlers;
  for (const std::unique_ptr<PadPlan> &Pad : Pads) {
    if (CleanedHandlers.insert(Pad->Block).second)
      RemoveNoreturnFallthroughs(Pad->Block);
    Pad->UnwindBlock = llvm::BasicBlock::Create(
        *Ctx, "eh.pad." + llvm::utohexstr(Pad->Address), &LLVMFunc, Pad->Block);
    llvm::IRBuilder<> Builder(Pad->UnwindBlock);
    Builder.CreateBr(Pad->Block);
  }

  auto EmitExceptionalPhiCopies = [&](const CallPlan &Plan) {
    const MedBlock *Target = nullptr;
    for (const MedBlock &Block : Func.Blocks)
      if (Block.Id == Plan.Pad->BlockId) {
        Target = &Block;
        break;
      }
    assert(Target && "preflight lost the exceptional phi target");
    llvm::IRBuilder<> Builder(Plan.Call);
    std::vector<std::pair<MedVar, llvm::Value *>> Pending;
    for (const PhiNode &Phi : Target->Phis)
      for (const auto &[IncomingPred, Incoming] : Phi.Args)
        if (IncomingPred == Plan.PredId)
          Pending.emplace_back(Phi.Output, getVar(Incoming, Builder));
    for (auto &[Output, Value] : Pending)
      setVar(Output, Value, Builder);
  };

  for (const CallPlan &Plan : Calls) {
    EmitExceptionalPhiCopies(Plan);
    llvm::Instruction *Next = Plan.Call->getNextNode();
    assert(Next && "preflighted call lost its continuation");
    llvm::BasicBlock *CallBlock = Plan.Call->getParent();
    llvm::BasicBlock *Continuation =
        CallBlock->splitBasicBlock(Next, CallBlock->getName() + ".eh.cont");
    llvm::Instruction *OldBranch = CallBlock->getTerminator();
    llvm::SmallVector<llvm::Value *, 8> Args;
    for (llvm::Use &Arg : Plan.Call->args())
      Args.push_back(Arg.get());
    llvm::SmallVector<llvm::OperandBundleDef, 2> Bundles;
    Plan.Call->getOperandBundlesAsDefs(Bundles);
    auto *Invoke = llvm::InvokeInst::Create(
        Plan.Call->getFunctionType(), Plan.Call->getCalledOperand(),
        Continuation, Plan.Pad->UnwindBlock, Args, Bundles,
        Plan.Call->getName(), OldBranch->getIterator());
    Invoke->setCallingConv(Plan.Call->getCallingConv());
    Invoke->setAttributes(Plan.Call->getAttributes());
    Invoke->setDebugLoc(Plan.Call->getDebugLoc());
    Invoke->copyMetadata(*Plan.Call);
    Plan.Call->replaceAllUsesWith(Invoke);
    CallSiteAddrs.erase(Plan.Call);
    Plan.Call->eraseFromParent();
    OldBranch->eraseFromParent();
  }

  if (ImportedPersonalityPlaceholder)
    ImportedPersonalityPlaceholder->setName(
        std::string(PersonalitySymbol) + ".import_data");
  llvm::FunctionCallee Personality =
      Mod->getOrInsertFunction(PersonalitySymbol, PersonalityTy);
  if (ImportedPersonalityPlaceholder) {
    auto *PersonalityFunction =
        llvm::cast<llvm::Function>(Personality.getCallee());
    ImportedPersonalityPlaceholder->replaceAllUsesWith(PersonalityFunction);
    ImportedPersonalityPlaceholder->eraseFromParent();
    ImportedSymbolPlaceholders.erase(PersonalitySymbol);
  }
  LLVMFunc.setPersonalityFn(
      llvm::cast<llvm::Constant>(Personality.getCallee()));

  auto *ResultTy = llvm::StructType::get(PtrTy, I32Ty);
  auto &Entry = CurFunc->getEntryBlock();
  llvm::IRBuilder<> AllocBuilder(&Entry, Entry.begin());
  const unsigned PointerSize = getTargetRegInfo(TargetArch).PointerSize;
  auto *ExceptionTy = sizeToType(static_cast<uint16_t>(PointerSize));
  EHExceptionAlloca =
      AllocBuilder.CreateAlloca(ExceptionTy, nullptr, "eh.exception.slot");
  EHSelectorAlloca =
      AllocBuilder.CreateAlloca(I32Ty, nullptr, "eh.selector.slot");
  for (const std::unique_ptr<PadPlan> &Pad : Pads) {
    llvm::IRBuilder<> Builder(Pad->UnwindBlock, Pad->UnwindBlock->begin());
    auto *LandingPad =
        Builder.CreateLandingPad(ResultTy, Pad->Clauses.size(), "eh.lpad");
    LandingPad->setCleanup(Pad->Semantics.Cleanup);
    for (llvm::Constant *Clause : Pad->Clauses)
      LandingPad->addClause(Clause);
    auto *Exception = Builder.CreateExtractValue(LandingPad, 0, "lp.ex");
    auto *Selector = Builder.CreateExtractValue(LandingPad, 1, "lp.sel");
    Builder.CreateStore(Builder.CreatePtrToInt(Exception, ExceptionTy),
                        EHExceptionAlloca);
    Builder.CreateStore(Selector, EHSelectorAlloca);
  }

  LLVMFunc.setMetadata(
      language_eh_md::ItaniumAttachment,
      llvm::MDNode::get(*Ctx,
                        {llvm::MDString::get(*Ctx, PersonalitySymbol),
                         mdUInt(*Ctx, Pads.size(), 32), mdUInt(*Ctx, 0, 32),
                         mdUInt(*Ctx, Calls.size(), 32),
                         mdUInt(*Ctx, RequiredCalls, 32)}));
  exception_rewrite::setContract(LLVMFunc,
                                 exception_rewrite::SourceState::Complete,
                                 exception_rewrite::LoweringState::Complete,
                                 RequiredCalls, Calls.size(), 0);
  return true;
}

} // namespace neverd
