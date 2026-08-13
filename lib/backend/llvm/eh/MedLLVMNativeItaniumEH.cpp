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
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <string>
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

  auto *PtrTy = llvm::PointerType::get(*Ctx, 0);
  auto *I8Ty = llvm::Type::getInt8Ty(*Ctx);
  auto *I32Ty = llvm::Type::getInt32Ty(*Ctx);

  // A type-table slot resolves to the exact RTTI object the catch matches.
  // Prefer the mapped object over TypeName: for stripped local types the latter
  // is often only std::type_info::__type_name ("15CxxEhProbeError"), not a
  // linkage symbol.  Declaring that byte string as an external global creates
  // a different/invalid pointer in the regenerated LSDA.
  auto TypeInfoConstant = [&](uint64_t Index) -> llvm::Constant * {
    const ItaniumTypeEntry *Entry = nullptr;
    for (const ItaniumTypeEntry &Candidate : Itanium.TypeTable)
      if (Candidate.Index == Index) {
        Entry = &Candidate;
        break;
      }
    if (!Entry)
      return nullptr;
    if (Entry->IsCatchAll)
      return llvm::ConstantPointerNull::get(PtrTy);

    // LLVM emits PIC type tables with DW_EH_PE_indirect.  The encoded value
    // must therefore name the pointer cell, not the RTTI object loaded from
    // it.  Reusing the original cell also preserves dyld/ld.so rebasing (and
    // arm64e pointer authentication) in the patched image.
    if (Entry->TypeInfoSlotVA != 0) {
      std::string Name = makeNdDataSymbol(Entry->TypeInfoSlotVA);
      llvm::GlobalVariable *GV = Mod->getNamedGlobal(Name);
      if (!GV)
        GV = new llvm::GlobalVariable(*Mod, I8Ty, /*isConstant=*/true,
                                      llvm::GlobalValue::ExternalLinkage,
                                      /*Initializer=*/nullptr, Name);
      return GV;
    }

    if (Entry->TypeInfoVA == 0)
      return nullptr;
    if (llvm::Constant *Mapped = tryResolveGlobalData(Entry->TypeInfoVA, 1))
      return Mapped;

    // A real Itanium RTTI linkage name is still useful for an unmapped
    // externally supplied type.  A bare encoded type-name string is not.
    llvm::StringRef DecodedName(Entry->TypeName);
    bool IsRTTISymbol =
        DecodedName.starts_with("_ZTI") || DecodedName.starts_with("__ZTI");
    std::string Name =
        IsRTTISymbol ? Entry->TypeName : makeNdDataSymbol(Entry->TypeInfoVA);
    llvm::GlobalVariable *GV = Mod->getNamedGlobal(Name);
    if (!GV)
      GV = new llvm::GlobalVariable(*Mod, I8Ty, /*isConstant=*/true,
                                    llvm::GlobalValue::ExternalLinkage, nullptr,
                                    Name);
    return GV;
  };

  struct Pad {
    llvm::BasicBlock *Block = nullptr;
    llvm::BasicBlock *UnwindBlock = nullptr;
    va_t Address = 0;
    int BlockId = -1;
    bool Cleanup = false;
    std::vector<llvm::Constant *> Clauses;
    bool Usable = false;
  };
  std::map<va_t, Pad> Pads;

  auto BlockAt = [&](va_t Address) -> llvm::BasicBlock * {
    for (const MedBlock &Block : Func.Blocks) {
      if (Block.StartAddr != Address)
        continue;
      auto It = OriginalBlockMap.find(Block.Id);
      return It == OriginalBlockMap.end() ? nullptr : It->second;
    }
    return nullptr;
  };
  auto FindAction = [&](uint64_t Offset) -> const ItaniumAction * {
    for (const ItaniumAction &Action : Itanium.Actions)
      if (Action.TableOffset == Offset)
        return &Action;
    return nullptr;
  };
  auto AddClause = [](Pad &P, llvm::Constant *Clause) {
    if (std::find(P.Clauses.begin(), P.Clauses.end(), Clause) ==
        P.Clauses.end())
      P.Clauses.push_back(Clause);
  };

  // Collect what each pad has to select on.  Several call sites can share a
  // pad, and the clauses they name accumulate on the one `landingpad` the pad
  // block gets.
  bool TableFullyRead = true;
  for (const ItaniumCallSite &Site : Itanium.CallSites) {
    if (Site.LandingPadVA == 0)
      continue;
    Pad &P = Pads[Site.LandingPadVA];
    P.Address = Site.LandingPadVA;
    if (!Site.FirstActionOffset) {
      // The ABI defines a pad with no action record as an unconditional
      // cleanup, which is the shape of every destructor-only frame and of
      // every Rust drop-glue pad.
      P.Cleanup = true;
      continue;
    }
    std::optional<uint64_t> Offset = Site.FirstActionOffset;
    // A step budget of the action count terminates a cycle without being able
    // to cut a well-formed chain short.
    for (size_t Step = 0; Offset && Step <= Itanium.Actions.size(); ++Step) {
      const ItaniumAction *Action = FindAction(*Offset);
      if (!Action) {
        TableFullyRead = false;
        break;
      }
      if (Action->isCleanup()) {
        P.Cleanup = true;
      } else if (Action->isCatch()) {
        llvm::Constant *Info =
            TypeInfoConstant(static_cast<uint64_t>(Action->TypeFilter));
        if (!Info)
          TableFullyRead = false;
        else
          AddClause(P, Info);
      } else {
        const ItaniumExceptionSpec *Spec = nullptr;
        for (const ItaniumExceptionSpec &Candidate : Itanium.ExceptionSpecs)
          if (Candidate.Index == static_cast<uint64_t>(-Action->TypeFilter)) {
            Spec = &Candidate;
            break;
          }
        if (!Spec) {
          TableFullyRead = false;
        } else {
          // A filter clause is an array of the types the specification
          // permits; the empty array is `noexcept`, which permits none.
          std::vector<llvm::Constant *> Permitted;
          for (uint64_t Index : Spec->TypeIndices) {
            llvm::Constant *Info = TypeInfoConstant(Index);
            if (!Info) {
              TableFullyRead = false;
              break;
            }
            Permitted.push_back(Info);
          }
          if (Permitted.size() == Spec->TypeIndices.size())
            AddClause(P, llvm::ConstantArray::get(
                             llvm::ArrayType::get(PtrTy, Permitted.size()),
                             Permitted));
        }
      }
      Offset = Action->NextActionOffset;
    }
  }
  // An action this decoder could not resolve leaves the pad's clause list
  // short, and a short clause list does not merely describe less — it says the
  // pad selects on fewer types than it does.  Nothing here is emitted from a
  // table that could not be read through.
  if (Pads.empty() || !TableFullyRead)
    return false;

  // A native landing address may also have an ordinary predecessor.  Clang
  // uses this shape when one LSDA pad is a short branch into another pad's
  // shared handler.  LLVM requires the landingpad instruction itself to live
  // in an unwind-only block, so each used native pad gets a synthetic unwind
  // entry below; that entry stores the exception pair and then branches to the
  // recovered native handler block.
  //
  // Native CFG recovery conservatively gives a trap instruction a fallthrough
  // edge.  Clang emits exactly that shape after noreturn EH runtime calls
  // (`objc_exception_rethrow(); brk #1; <landing pad>`).  Once the trap has
  // become an LLVM noreturn call, remove its impossible unconditional edge so
  // the following block can correctly become an unwind-only landing pad.
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
      llvm::IRBuilder<> B(Pred);
      B.CreateUnreachable();
    }
  };

  size_t UsablePads = 0;
  for (auto &[PadVA, P] : Pads) {
    P.Block = BlockAt(PadVA);
    for (const MedBlock &Block : Func.Blocks)
      if (Block.StartAddr == PadVA) {
        P.BlockId = Block.Id;
        break;
      }
    if (P.Block)
      RemoveNoreturnFallthroughs(P.Block);
    if (!P.Block || P.Block == &LLVMFunc.getEntryBlock() ||
        P.Block->isLandingPad())
      continue;
    // Every pad the ABI can enter runs at least cleanup; a pad that named no
    // clause and no cleanup would be a `landingpad` LLVM rejects.
    if (P.Clauses.empty())
      P.Cleanup = true;
    P.Usable = true;
    ++UsablePads;
  }
  if (UsablePads == 0)
    return false;

  auto EnsureUnwindBlock = [&](Pad &P) {
    if (P.UnwindBlock)
      return P.UnwindBlock;
    P.UnwindBlock = llvm::BasicBlock::Create(
        *Ctx, "eh.pad." + llvm::utohexstr(P.Address), &LLVMFunc, P.Block);
    llvm::IRBuilder<> B(P.UnwindBlock);
    B.CreateBr(P.Block);
    return P.UnwindBlock;
  };

  // Match each emitted call to the innermost call-site range that covers the
  // address it came from.  The ranges a compiler emits do not overlap, so the
  // innermost test only ever disambiguates a table this decoder read loosely.
  auto PadForCall = [&](va_t Address) -> Pad * {
    Pad *Best = nullptr;
    uint64_t BestSize = std::numeric_limits<uint64_t>::max();
    for (const ItaniumCallSite &Site : Itanium.CallSites) {
      if (Site.LandingPadVA == 0 || !Site.GuardedRange.contains(Address) ||
          Site.GuardedRange.size() >= BestSize)
        continue;
      auto It = Pads.find(Site.LandingPadVA);
      if (It == Pads.end() || !It->second.Usable)
        continue;
      Best = &It->second;
      BestSize = Site.GuardedRange.size();
    }
    return Best;
  };

  auto EmitExceptionalPhiCopies = [&](int PredId, int TargetId,
                                      llvm::Instruction *Before) {
    const MedBlock *TargetBlock = nullptr;
    for (const MedBlock &Block : Func.Blocks)
      if (Block.Id == TargetId) {
        TargetBlock = &Block;
        break;
      }
    if (!TargetBlock)
      return;

    llvm::IRBuilder<> CopyBuilder(Before);
    std::vector<std::pair<MedVar, llvm::Value *>> Pending;
    for (const PhiNode &Phi : TargetBlock->Phis)
      for (const auto &[IncomingPred, Incoming] : Phi.Args)
        if (IncomingPred == PredId)
          Pending.emplace_back(Phi.Output, getVar(Incoming, CopyBuilder));
    // Parallel-copy semantics: read every incoming value before any phi output
    // is overwritten.
    for (auto &[Output, Value] : Pending)
      setVar(Output, Value, CopyBuilder);
  };

  size_t LoweredCalls = 0;
  for (const MedBlock &MedBB : Func.Blocks) {
    auto BBIt = OriginalBlockMap.find(MedBB.Id);
    if (BBIt == OriginalBlockMap.end())
      continue;
    llvm::SmallVector<llvm::CallInst *, 8> Calls;
    for (llvm::Instruction &Inst : *BBIt->second)
      if (auto *Call = llvm::dyn_cast<llvm::CallInst>(&Inst))
        if (!Call->doesNotThrow() && !Call->isMustTailCall() &&
            !llvm::isa<llvm::IntrinsicInst>(Call))
          Calls.push_back(Call);

    for (llvm::CallInst *Call : Calls) {
      auto AddrIt = CallSiteAddrs.find(Call);
      if (AddrIt == CallSiteAddrs.end())
        continue;
      Pad *Target = PadForCall(AddrIt->second);
      if (!Target)
        continue;
      llvm::BasicBlock *UnwindBlock = EnsureUnwindBlock(*Target);
      EmitExceptionalPhiCopies(MedBB.Id, Target->BlockId, Call);
      llvm::Instruction *Next = Call->getNextNode();
      if (!Next)
        continue;
      llvm::BasicBlock *CallBB = Call->getParent();
      llvm::BasicBlock *Cont =
          CallBB->splitBasicBlock(Next, CallBB->getName() + ".eh.cont");
      llvm::Instruction *OldBranch = CallBB->getTerminator();
      llvm::SmallVector<llvm::Value *, 8> Args;
      for (llvm::Use &Arg : Call->args())
        Args.push_back(Arg.get());
      llvm::SmallVector<llvm::OperandBundleDef, 2> Bundles;
      Call->getOperandBundlesAsDefs(Bundles);
      auto *Invoke = llvm::InvokeInst::Create(
          Call->getFunctionType(), Call->getCalledOperand(), Cont, UnwindBlock,
          Args, Bundles, Call->getName(), OldBranch->getIterator());
      Invoke->setCallingConv(Call->getCallingConv());
      Invoke->setAttributes(Call->getAttributes());
      Invoke->setDebugLoc(Call->getDebugLoc());
      Invoke->copyMetadata(*Call);
      Call->replaceAllUsesWith(Invoke);
      CallSiteAddrs.erase(Call);
      Call->eraseFromParent();
      OldBranch->eraseFromParent();
      ++LoweredCalls;
    }
  }
  if (LoweredCalls == 0)
    return false;

  auto *PersonalityTy = llvm::FunctionType::get(I32Ty, {}, true);
  llvm::FunctionCallee Personality =
      Mod->getOrInsertFunction(PersonalitySymbol, PersonalityTy);
  LLVMFunc.setPersonalityFn(
      llvm::cast<llvm::Constant>(Personality.getCallee()));

  // The pad's `landingpad` has to precede the recovered body, because LLVM
  // requires it to be the block's first non-PHI instruction and because the
  // unwinder has already run by the time that body executes.
  auto *ResultTy = llvm::StructType::get(PtrTy, I32Ty);
  size_t LoweredPads = 0;
  for (auto &[PadVA, P] : Pads) {
    if (!P.Usable || !P.UnwindBlock || llvm::pred_empty(P.UnwindBlock))
      continue;
    llvm::IRBuilder<> B(P.UnwindBlock, P.UnwindBlock->begin());
    auto *LP = B.CreateLandingPad(ResultTy, P.Clauses.size(), "eh.lpad");
    LP->setCleanup(P.Cleanup);
    for (llvm::Constant *Clause : P.Clauses)
      LP->addClause(Clause);

    // Preserve the Itanium landing-pad pair for the recovered handler body.
    // LowToMed represents the native exceptional live-ins (the first two
    // integer return registers) as EHException/EHSelector rather than ordinary
    // function parameters; getVar() loads those values from these slots.
    auto &Entry = CurFunc->getEntryBlock();
    llvm::IRBuilder<> AllocBuilder(&Entry, Entry.begin());
    const unsigned PointerSize = getTargetRegInfo(TargetArch).PointerSize;
    auto *ExceptionTy = sizeToType(static_cast<uint16_t>(PointerSize));
    if (!EHExceptionAlloca)
      EHExceptionAlloca =
          AllocBuilder.CreateAlloca(ExceptionTy, nullptr, "eh.exception.slot");
    if (!EHSelectorAlloca)
      EHSelectorAlloca =
          AllocBuilder.CreateAlloca(I32Ty, nullptr, "eh.selector.slot");
    auto *LPEx = B.CreateExtractValue(LP, 0, "lp.ex");
    auto *LPSel = B.CreateExtractValue(LP, 1, "lp.sel");
    B.CreateStore(B.CreatePtrToInt(LPEx, ExceptionTy), EHExceptionAlloca);
    B.CreateStore(LPSel, EHSelectorAlloca);
    ++LoweredPads;
  }
  if (LoweredPads == 0) {
    // No invoke reached a pad after all, which leaves a personality on a
    // function that has no landing pad.  Undo it rather than describe a
    // dispatch that is not there.
    LLVMFunc.setPersonalityFn(nullptr);
    return false;
  }

  LLVMFunc.setMetadata(
      language_eh_md::ItaniumAttachment,
      llvm::MDNode::get(*Ctx, {llvm::MDString::get(*Ctx, PersonalitySymbol),
                               mdUInt(*Ctx, LoweredPads, 32),
                               mdUInt(*Ctx, Pads.size() - LoweredPads, 32),
                               mdUInt(*Ctx, LoweredCalls, 32)}));
  return true;
}

} // namespace neverd
