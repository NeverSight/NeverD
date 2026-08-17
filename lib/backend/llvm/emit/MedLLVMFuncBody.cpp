//===- MedLLVMFuncBody.cpp - Per-function body emission --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Per-function LLVM body emission for MedLLVMEmitter: the synthetic frame
/// alloca and parameter seeding, basic-block creation and terminator
/// selection, the operation dispatch loop, the deferred computed-goto
/// dispatch stores, phi placement with critical-edge splitting, and the
/// native EH lowering hand-off.  The emitter core (types, the
/// memory-pointer primitive, function declaration and module emission)
/// lives in MedLLVMEmitter.cpp.
///
/// This is one indivisible member function plus the synthetic-stack
/// overflow helpers it alone uses, so it is a single translation unit.
///
//===----------------------------------------------------------------------===//

#include "neverd/ArchSupport.h"
#include "neverd/Common.h"
#include "neverd/Limits.h"
#include "neverd/backend/llvm/LanguageEHMetadata.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/ir/TargetRegInfo.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/Support/ErrorHandling.h"

#include <algorithm>
#include <limits>
#include <map>
#include <optional>
#include <tuple>
#include <utility>
#include <vector>

namespace neverd {

namespace {

uint64_t checkedSyntheticStackAdd(uint64_t Left, uint64_t Right) {
  if (Left > std::numeric_limits<uint64_t>::max() - Right)
    llvm::report_fatal_error("LLVM synthetic stack size overflow");
  return Left + Right;
}

uint64_t checkedSyntheticStackMul(uint64_t Left, uint64_t Right) {
  if (Right != 0 && Left > std::numeric_limits<uint64_t>::max() / Right)
    llvm::report_fatal_error("LLVM synthetic stack size overflow");
  return Left * Right;
}

uint64_t alignSyntheticStack(uint64_t Size) {
  constexpr uint64_t Mask = kSyntheticStackAlignment - 1;
  return checkedSyntheticStackAdd(Size, Mask) & ~Mask;
}

uint64_t positiveRangeEnd(int64_t Start, uint64_t Size) {
  if (Start >= 0)
    return checkedSyntheticStackAdd(static_cast<uint64_t>(Start), Size);
  uint64_t Distance = static_cast<uint64_t>(-(Start + 1)) + 1;
  return Size > Distance ? Size - Distance : 0;
}

bool isPredicatedObservableEffect(NdOp Opcode) {
  switch (Opcode) {
  case NdOp::LOAD:
  case NdOp::STORE:
  case NdOp::ATOMIC_XCHG:
  case NdOp::ATOMIC_ADD:
  case NdOp::ATOMIC_CMPXCHG:
  case NdOp::INTRINSIC:
  case NdOp::INDIR_BR:
  case NdOp::CALL:
  case NdOp::INDIR_CALL:
  case NdOp::RETURN:
    return true;
  default:
    return false;
  }
}

/// Find the end of a flattened predicated instruction with an observable
/// effect.  LowToMed keeps every operation's source address, so an internal
/// guard and the effects it skips remain one same-address slice after SSA
/// construction.  Loads are observable here as well as stores and controls:
/// even a discarded load may fault or touch MMIO.  Ordinary conditional
/// branches have no later observable effect at that address and are not
/// matched.
std::optional<size_t> predicatedEffectEnd(const MedBlock &Block,
                                          size_t GuardIndex) {
  if (GuardIndex >= Block.Ops.size() ||
      Block.Ops[GuardIndex].Opcode != NdOp::COND_BR)
    return std::nullopt;

  size_t End = GuardIndex + 1;
  while (End < Block.Ops.size() &&
         Block.Ops[End].Addr == Block.Ops[GuardIndex].Addr)
    ++End;
  for (size_t I = GuardIndex + 1; I < End; ++I)
    if (isPredicatedObservableEffect(Block.Ops[I].Opcode))
      return End;
  return std::nullopt;
}

} // anonymous namespace

llvm::Function *MedLLVMEmitter::emitFunc(const MedFunc &Func) {
  if (Func.Blocks.empty())
    return nullptr;

  CurMedFunc = &Func;

  auto *LLVMFunc = declareFunc(Func);
  emitExceptionMetadata(Func, *LLVMFunc);
  CurFunc = LLVMFunc;
  VarAllocs.clear();
  CallSiteAddrs.clear();
  ParamArgs.clear();
  ParamRegoffMap.clear();
  DynVlaBases.clear();
  PendingDispatchStores.clear();
  FrameAlloca = nullptr;
  FrameBaseInt = nullptr;
  EHExceptionAlloca = nullptr;
  EHSelectorAlloca = nullptr;

  unsigned PI = 0;
  for (auto &Arg : LLVMFunc->args()) {
    if (PI < Func.Params.size()) {
      std::string PName;
      if (Func.hasTypeInfo() && PI < Func.TypedParams.size())
        PName = Func.TypedParams[PI].Name;
      else
        PName = Func.Params[PI].display();
      Arg.setName(PName);
      ParamArgs[PName] = &Arg;
      std::string OrigName = Func.Params[PI].display();
      if (OrigName != PName)
        ParamArgs[OrigName] = &Arg;
      if ((Func.Params[PI].Kind == MedVar::Param ||
           Func.Params[PI].Kind == MedVar::Reg) &&
          Func.Params[PI].RegOff != kNoParamReg)
        ParamRegoffMap[Func.Params[PI].RegOff] = &Arg;
    }
    ++PI;
  }

  const bool NeedsWindowsEHPrologue =
      TargetArch == Arch::X64 && TargetFormat == BinaryFormat::COFF &&
      Func.ExceptionMetadata &&
      (Func.ExceptionMetadata->Personality ==
           ExceptionPersonality::CSpecificHandler ||
       Func.ExceptionMetadata->Personality ==
           ExceptionPersonality::CxxFrameHandler3);
  const bool NeedsFrameSetup = Func.FrameSize > 0 || Func.FrameHeadroom > 0 ||
                               Func.IsVariadic ||
                               !Func.MutableStackParamHomes.empty();
  llvm::BasicBlock *FrameSetupBB = nullptr;
  if (NeedsFrameSetup || NeedsWindowsEHPrologue) {
    llvm::BasicBlock *InsertBefore =
        LLVMFunc->empty() ? nullptr : &LLVMFunc->getEntryBlock();
    FrameSetupBB = llvm::BasicBlock::Create(*Ctx, kFrameSetupBlock, LLVMFunc,
                                            InsertBefore);
    if (NeedsFrameSetup) {
      const auto &TRI = getTargetRegInfo(TargetArch);
      llvm::IRBuilder<> FrameB(FrameSetupBB);
      // Preserve the target ABI's entry-SP residue: AArch64 enters aligned,
      // x86-64 is 8 mod 16, and Darwin i386 is 12 mod 16.
      uint64_t AlignedFrameSize = alignSyntheticStack(
          Func.FrameSize > 0 ? static_cast<uint64_t>(Func.FrameSize) : 0);
      uint64_t EntryResidue =
          syntheticEntryStackResidue(TargetArch, TargetFormat);
      uint64_t FrameBaseOffset =
          checkedSyntheticStackAdd(AlignedFrameSize, EntryResidue);
      // A variadic function reads its overflow (incoming-stack) arguments at
      // entry_sp + base + i*slot, above frame_end.  Reserve headroom there
      // (kept separate from frame_end so the SP self-copy stays at frame_end)
      // and spill the recovered overflow stack parameters into it below.
      uint64_t Headroom = 0;
      // If no overflow arguments were recovered as LLVM parameters, the
      // variadic walk still reads the caller's native entry-stack area. Putting
      // generic positive stack slots in local headroom would shadow that area
      // with uninitialised storage.  Explicitly seeded homes below remain safe.
      uint64_t MaxHomeEnd = 0;
      if (!(Func.IsVariadic && Func.VariadicOverflowCount == 0) &&
          Func.FrameHeadroom > 0)
        MaxHomeEnd = static_cast<uint64_t>(Func.FrameHeadroom);
      if (Func.IsVariadic && Func.VariadicOverflowCount > 0) {
        uint64_t OverflowBytes = checkedSyntheticStackMul(
            static_cast<uint64_t>(Func.VariadicOverflowCount),
            static_cast<uint64_t>(TRI.PointerSize));
        MaxHomeEnd =
            std::max(MaxHomeEnd, positiveRangeEnd(Func.VariadicOverflowBase,
                                                  OverflowBytes));
      }
      // Written incoming stack-argument home slots (a parameter updated in
      // place) also live above frame_end and are seeded at entry below, so the
      // headroom must cover them too.
      for (const auto &Home : Func.MutableStackParamHomes)
        MaxHomeEnd = std::max(MaxHomeEnd,
                              positiveRangeEnd(Home.second, TRI.PointerSize));
      if (MaxHomeEnd > 0) {
        Headroom = alignSyntheticStack(checkedSyntheticStackAdd(
            MaxHomeEnd, static_cast<uint64_t>(limits::kVariadicOverflowSlop)));
      }
      uint64_t StorageSize =
          checkedSyntheticStackAdd(FrameBaseOffset, Headroom);
      auto *FrameTy =
          llvm::ArrayType::get(llvm::Type::getInt8Ty(*Ctx), StorageSize);
      FrameAlloca = FrameB.CreateAlloca(FrameTy, nullptr, "frame");
      FrameAlloca->setAlignment(llvm::Align(16));
      auto *FrameEnd = FrameB.CreateInBoundsGEP(
          llvm::Type::getInt8Ty(*Ctx), FrameAlloca,
          llvm::ConstantInt::get(llvm::Type::getInt64Ty(*Ctx), FrameBaseOffset),
          "frame_end");
      FrameBaseInt = FrameB.CreatePtrToInt(
          FrameEnd, llvm::Type::getInt64Ty(*Ctx), kRspInitValue);

      // Spill the variadic overflow stack parameters (the trailing parameters)
      // into the headroom at entry_sp + base + i*slot, so the unchanged va_arg
      // walk — which the LLVM optimizer resolves to those addresses — reads the
      // caller's overflow arguments instead of uninitialised frame memory.
      if (Func.IsVariadic && Func.VariadicOverflowCount > 0) {
        const int K = Func.VariadicOverflowCount;
        const int NumParams = static_cast<int>(Func.Params.size());
        std::vector<llvm::Argument *> OverflowArgs;
        int PIdx = 0;
        for (auto &A : LLVMFunc->args()) {
          if (PIdx >= NumParams - K && PIdx < NumParams)
            OverflowArgs.push_back(&A);
          ++PIdx;
        }
        for (int I = 0; I < static_cast<int>(OverflowArgs.size()); ++I) {
          int64_t Off = Func.VariadicOverflowBase +
                        static_cast<int64_t>(I) * TRI.PointerSize;
          auto *AddrInt = FrameB.CreateAdd(
              FrameBaseInt, llvm::ConstantInt::get(llvm::Type::getInt64Ty(*Ctx),
                                                   static_cast<uint64_t>(Off)));
          auto *AddrPtr =
              FrameB.CreateIntToPtr(AddrInt, llvm::PointerType::get(*Ctx, 0));
          FrameB.CreateStore(OverflowArgs[I], AddrPtr);
        }
      }

      // Seed each written incoming stack-argument home slot with its parameter
      // so the in-IR memory loads/stores through [frame_end + Off] read the
      // argument and observe later in-place writes (mirrors the variadic
      // overflow spill).
      if (!Func.MutableStackParamHomes.empty()) {
        const int NumParams = static_cast<int>(Func.Params.size());
        std::vector<llvm::Argument *> ArgPtrs;
        for (auto &A : LLVMFunc->args())
          ArgPtrs.push_back(&A);
        for (const auto &[PIdx, Off] : Func.MutableStackParamHomes) {
          if (PIdx < 0 || PIdx >= NumParams ||
              PIdx >= static_cast<int>(ArgPtrs.size()))
            continue;
          auto *AddrInt = FrameB.CreateAdd(
              FrameBaseInt, llvm::ConstantInt::get(llvm::Type::getInt64Ty(*Ctx),
                                                   static_cast<uint64_t>(Off)));
          auto *AddrPtr =
              FrameB.CreateIntToPtr(AddrInt, llvm::PointerType::get(*Ctx, 0));
          FrameB.CreateStore(ArgPtrs[PIdx], AddrPtr);
        }
      }
    }
  }

  DataSizeHints.clear();
  {
    constexpr uint64_t kMin = limits::kMinGlobalDataAddr;
    std::map<std::pair<int, int>, uint64_t> ConstMap;
    for (const auto &Blk : Func.Blocks) {
      for (const auto &Op : Blk.Ops) {
        if (Op.Opcode == NdOp::COPY && Op.NumInputs >= 1 &&
            Op.Inputs[0].isConst() && Op.Inputs[0].ConstVal > kMin) {
          ConstMap[{Op.Output.Id, Op.Output.SSAVer}] = Op.Inputs[0].ConstVal;
        }
        uint64_t AddrVal = 0;
        uint16_t DataWidth = 0;
        if (Op.Opcode == NdOp::LOAD && Op.NumInputs >= 1) {
          DataWidth = Op.Output.Size;
          if (Op.Inputs[0].isConst() && Op.Inputs[0].ConstVal > kMin)
            AddrVal = Op.Inputs[0].ConstVal;
          else {
            auto It = ConstMap.find({Op.Inputs[0].Id, Op.Inputs[0].SSAVer});
            if (It != ConstMap.end())
              AddrVal = It->second;
          }
        } else if ((Op.Opcode == NdOp::STORE ||
                    Op.Opcode == NdOp::ATOMIC_XCHG ||
                    Op.Opcode == NdOp::ATOMIC_ADD ||
                    Op.Opcode == NdOp::ATOMIC_CMPXCHG) &&
                   Op.NumInputs >= 2) {
          DataWidth = Op.Inputs[1].Size;
          if (Op.Inputs[0].isConst() && Op.Inputs[0].ConstVal > kMin)
            AddrVal = Op.Inputs[0].ConstVal;
          else {
            auto It = ConstMap.find({Op.Inputs[0].Id, Op.Inputs[0].SSAVer});
            if (It != ConstMap.end())
              AddrVal = It->second;
          }
        }
        if (AddrVal && DataWidth > 0) {
          auto &Cur = DataSizeHints[AddrVal];
          if (DataWidth > Cur)
            Cur = DataWidth;
        }
      }
    }
  }

  std::map<int, llvm::BasicBlock *> BBMap;
  std::map<int, std::vector<llvm::BasicBlock *>> ConceptualExits;
  llvm::DenseMap<va_t, llvm::BasicBlock *> AddressToBlock;
  llvm::DenseMap<int, va_t> BlockToAddress;
  for (auto &Blk : Func.Blocks) {
    auto Prepared = PreparedFuncBlocks.find({Func.Entry, Blk.Id});
    if (Prepared == PreparedFuncBlocks.end())
      llvm::report_fatal_error("missing pre-created MedLLVM block skeleton");
    llvm::BasicBlock *BB = Prepared->second;
    BBMap[Blk.Id] = BB;
    ConceptualExits[Blk.Id].push_back(BB);
    const va_t Address = Blk.StartAddr != 0 || Blk.Ops.empty()
                             ? Blk.StartAddr
                             : Blk.Ops.front().Addr;
    AddressToBlock.try_emplace(Address, BB);
    BlockToAddress.try_emplace(Blk.Id, Address);
  }

  auto blockAtAddress = [&](va_t Address) -> llvm::BasicBlock * {
    auto It = AddressToBlock.find(Address);
    return It != AddressToBlock.end() ? It->second : nullptr;
  };

  if (!Func.Blocks.empty()) {
    int EntryId = Func.Blocks.front().Id;
    if (FrameSetupBB) {
      llvm::IRBuilder<> FB(FrameSetupBB);
      FB.CreateBr(BBMap[EntryId]);
    } else {
      bool EntryHasPreds = false;
      for (auto &Blk : Func.Blocks) {
        for (int SId : Blk.Succs) {
          if (SId == EntryId) {
            EntryHasPreds = true;
            break;
          }
        }
        if (EntryHasPreds)
          break;
      }
      if (EntryHasPreds) {
        auto *RealEntry = &LLVMFunc->getEntryBlock();
        auto *LoopTarget = BBMap[EntryId];
        auto *NewEntry =
            llvm::BasicBlock::Create(*Ctx, "entry", LLVMFunc, RealEntry);
        llvm::IRBuilder<> EntryB(NewEntry);
        EntryB.CreateBr(LoopTarget);
        LoopTarget->moveAfter(NewEntry);
      }
    }
  }

  SubRegPropMap.clear();

  // Pre-pass: create allocas for ALL phi outputs *and* all op outputs across
  // ALL blocks *before* emitting any ops.  A block that appears earlier in
  // Func.Blocks order (e.g. a loop latch) can use a value defined in a later
  // block (the loop header) — this is valid SSA whenever the defining block
  // dominates the using block.  If that value's alloca hasn't been created yet
  // when the earlier block's ops are emitted, getVar() fails the (Id,SSAVer)
  // lookup and wrongly falls back to reading the overlapping *wide* register (a
  // stale, loop-invariant value), silently dropping loop-carried values.
  //
  // This previously only covered phi outputs (bug #210: `add w0, w10, w0` where
  // w0 is both an argument sub-register and the accumulator).  It must also
  // cover non-phi op outputs: e.g. a `cmovg`-derived running max defined in the
  // loop header (`SUBBYTES EDX.5 RDX.2`) but read in the latch's `add edx, ecx`
  // — when the latch is ordered before the header, EDX.5's alloca is missing
  // and getVar falls back to the wide RDX register, which still holds the
  // loop-entry INT_MIN, folding the max back to its initial value.
  // Op outputs that are live-ins (SSAVer == 0) must NOT be pre-created here:
  // those are resolved through the parameter / entry-register path in getVar
  // (which keys off `!HasLocalDef && SSAVer == 0`), and creating an alloca for
  // them would shadow the incoming argument with an uninitialized slot.
  //
  // For op outputs we also zero-initialize the slot (like getVar's no-def
  // fallback at the bottom of getVar): the previous wide-register fallback used
  // to mask non-dominating cross-block reads by returning an already-defined
  // wide register; reading a bare uninitialized alloca instead would yield
  // poison and let the optimizer fold the whole function to `unreachable`.
  auto preCreatePhiAlloca = [&](const MedVar &Out) {
    if (Out.isConst() || Out.Size == 0)
      return;
    auto Key = std::make_pair(Out.Id, Out.SSAVer);
    if (VarAllocs.find(Key) != VarAllocs.end())
      return;
    auto &Entry = CurFunc->getEntryBlock();
    llvm::IRBuilder<> AllocBuilder(&Entry, Entry.begin());
    auto *Ty = sizeToType(Out.Size);
    VarAllocs[Key] = AllocBuilder.CreateAlloca(Ty, nullptr, Out.display());
  };
  auto preCreateOpAlloca = [&](const MedVar &Out) {
    if (Out.isConst() || Out.Size == 0 || Out.SSAVer == 0)
      return;
    auto Key = std::make_pair(Out.Id, Out.SSAVer);
    if (VarAllocs.find(Key) != VarAllocs.end())
      return;
    auto &Entry = CurFunc->getEntryBlock();
    llvm::IRBuilder<> AllocBuilder(&Entry, Entry.begin());
    auto *Ty = sizeToType(Out.Size);
    auto *Alloca = AllocBuilder.CreateAlloca(Ty, nullptr, Out.display());
    AllocBuilder.CreateStore(llvm::ConstantInt::get(Ty, 0), Alloca);
    VarAllocs[Key] = Alloca;
  };
  for (auto &Blk : Func.Blocks) {
    for (auto &Phi : Blk.Phis)
      preCreatePhiAlloca(Phi.Output);
    for (auto &Op : Blk.Ops)
      preCreateOpAlloca(Op.Output);
  }
  for (const MedCallClobber &Clobber : Func.CallClobbers)
    preCreatePhiAlloca(Clobber.Value);

  for (auto &Blk : Func.Blocks) {
    auto *BB = BBMap[Blk.Id];
    llvm::IRBuilder<> Builder(BB);

    for (size_t OI = 0; OI < Blk.Ops.size(); ++OI) {
      auto &Op = Blk.Ops[OI];
      if (Op.Opcode == NdOp::BRANCH) {
        if (!Blk.Succs.empty()) {
          auto SuccIt = BBMap.find(Blk.Succs[0]);
          if (SuccIt != BBMap.end())
            Builder.CreateBr(SuccIt->second);
        }
        break;
      }
      if (Op.Opcode == NdOp::COND_BR) {
        if (Op.NumInputs >= 2) {
          auto *Cond = getVar(Op.Inputs[1], Builder);
          if (Cond->getType() != llvm::Type::getInt1Ty(*Ctx)) {
            auto *Zero = llvm::ConstantInt::get(Cond->getType(), 0);
            Cond = Builder.CreateICmpNE(Cond, Zero, "cond");
          }

          // ARM predication is one decoded instruction represented by a
          // `COND_BR next, !predicate` followed by the same-address effects it
          // skips.  Lower it to a real LLVM micro-CFG.  This is required not
          // only for calls and returns: executing a discarded load or a
          // read-modify-write store on a substitute address can still fault,
          // touch MMIO, fire a watchpoint, or dirty a page.
          if (std::optional<size_t> EffectEnd = predicatedEffectEnd(Blk, OI)) {
            llvm::BasicBlock *SkipBB = nullptr;
            if (Op.Inputs[0].isConst())
              SkipBB = blockAtAddress(Op.Inputs[0].ConstVal);
            if (!SkipBB && Blk.Succs.size() == 1) {
              auto SkipIt = BBMap.find(Blk.Succs.front());
              if (SkipIt != BBMap.end())
                SkipBB = SkipIt->second;
            }

            if (SkipBB) {
              auto *EffectBB = llvm::BasicBlock::Create(
                  *Ctx,
                  "predeffect_" + std::to_string(Blk.Id) + "_" +
                      std::to_string(OI),
                  CurFunc, SkipBB);
              ConceptualExits[Blk.Id].push_back(EffectBB);
              Builder.CreateCondBr(Cond, SkipBB, EffectBB);

              llvm::IRBuilder<> EffectBuilder(EffectBB);
              for (size_t EI = OI + 1; EI < *EffectEnd; ++EI) {
                const MedOp &Effect = Blk.Ops[EI];
                if (Effect.Opcode == NdOp::INDIR_BR) {
                  if (!emitJumpTableSwitch(Blk, Effect, BBMap, EffectBuilder)) {
                    // A guest virtual address cannot be used as an LLVM
                    // blockaddress.  When recovery did not prove its finite
                    // destination set, fail loudly on the selected path; the
                    // guard-taken continuation remains executable.
                    EffectBuilder.CreateIntrinsic(llvm::Type::getVoidTy(*Ctx),
                                                  llvm::Intrinsic::trap, {});
                    EffectBuilder.CreateUnreachable();
                  }
                  break;
                }
                if (Effect.Opcode == NdOp::RETURN) {
                  emitOp(Effect, EffectBuilder, Blk.Id, static_cast<int>(EI));
                  break;
                }
                emitOp(Effect, EffectBuilder, Blk.Id, static_cast<int>(EI));
                if (!EffectBB->empty() && EffectBB->back().isTerminator())
                  break;
              }
              if (EffectBB->empty() || !EffectBB->back().isTerminator())
                EffectBuilder.CreateBr(SkipBB);
              break;
            }
          }

          llvm::BasicBlock *TakenBB = nullptr;
          llvm::BasicBlock *FallthroughBB = nullptr;
          if (Blk.Succs.size() >= 2) {
            // Use the COND_BR target address to determine which
            // successor is "taken" (cond=true). The ARM32 lifter
            // emits COND_BR with the fallthrough address and an
            // inverted condition, so the target may correspond to
            // Succs[0] rather than Succs[1].
            uint64_t CbrTarget =
                Op.Inputs[0].isConst() ? Op.Inputs[0].ConstVal : 0;
            int TakenId = Blk.Succs[1], FallId = Blk.Succs[0];
            if (CbrTarget != 0) {
              auto AddressIt = BlockToAddress.find(Blk.Succs[0]);
              if (AddressIt != BlockToAddress.end() &&
                  AddressIt->second == CbrTarget) {
                TakenId = Blk.Succs[0];
                FallId = Blk.Succs[1];
              }
            }
            auto ItFall = BBMap.find(FallId);
            auto ItTaken = BBMap.find(TakenId);
            if (ItFall != BBMap.end())
              FallthroughBB = ItFall->second;
            if (ItTaken != BBMap.end())
              TakenBB = ItTaken->second;
          } else if (Blk.Succs.size() == 1) {
            auto It0 = BBMap.find(Blk.Succs[0]);
            llvm::BasicBlock *ContBB =
                (It0 != BBMap.end()) ? It0->second : nullptr;
            // Conditional return idiom (`bxCC lr` / predicated `pop {pc}` used
            // as a loop exit): the block ends in COND_BR(target, cond) followed
            // by a RETURN, but the CFG gives it a single successor (the COND_BR
            // target) because the return edge has no block.  Synthesize a
            // return block for the not-taken (cond-false) edge so the
            // predicated return survives — otherwise both CondBr edges aimed at
            // the lone successor and the RETURN was dropped, turning the loop
            // exit into an infinite loop (#385 swfall).
            int RetOpIdx = -1;
            for (size_t RI = OI + 1; RI < Blk.Ops.size(); ++RI)
              if (Blk.Ops[RI].Opcode == NdOp::RETURN) {
                RetOpIdx = static_cast<int>(RI);
                break;
              }
            if (ContBB && RetOpIdx >= 0) {
              auto *RetBB = llvm::BasicBlock::Create(*Ctx, "condret", CurFunc);
              Builder.CreateCondBr(Cond, ContBB, RetBB);
              llvm::IRBuilder<> RetBuilder(RetBB);
              emitOp(Blk.Ops[RetOpIdx], RetBuilder, Blk.Id, RetOpIdx);
            } else if (ContBB) {
              TakenBB = FallthroughBB = ContBB;
            }
          }
          if (TakenBB && FallthroughBB)
            Builder.CreateCondBr(Cond, TakenBB, FallthroughBB);
        }
        break;
      }
      if (Op.Opcode == NdOp::INDIR_BR) {
        if (!emitJumpTableSwitch(Blk, Op, BBMap, Builder)) {
          // Every INDIR_BR that survives tail-call conversion must have a
          // proved finite destination set.  This includes the selected half of
          // a predicated PC write: it may legitimately have zero recovered
          // successors.  Falling through or returning in either case invents
          // control flow, so fail loudly whenever switch reconstruction fails.
          Builder.CreateIntrinsic(llvm::Type::getVoidTy(*Ctx),
                                  llvm::Intrinsic::trap, {});
          Builder.CreateUnreachable();
        }
        break;
      }
      if (Op.Opcode == NdOp::RETURN) {
        emitOp(Op, Builder, Blk.Id, static_cast<int>(OI));
        break;
      }
      emitOp(Op, Builder, Blk.Id, static_cast<int>(OI));
      // Architecture side-effect emitters may terminate the block themselves
      // (for example AArch64 BRK/HLT).  Do not append later lifted operations
      // or the CFG's conservative fallthrough branch after an LLVM terminator.
      if (!BB->empty() && BB->back().isTerminator())
        break;
    }

    if (BB->empty() || !BB->back().isTerminator()) {
      auto EmitDefaultRet = [&]() {
        if (CurFunc->getReturnType()->isVoidTy())
          Builder.CreateRetVoid();
        else
          Builder.CreateRet(
              llvm::ConstantInt::get(CurFunc->getReturnType(), 0));
      };

      if (Blk.Succs.size() == 1) {
        auto SuccIt = BBMap.find(Blk.Succs[0]);
        if (SuccIt != BBMap.end())
          Builder.CreateBr(SuccIt->second);
        else
          EmitDefaultRet();
      } else if (Blk.Succs.empty()) {
        if (Func.DoesNotReturn)
          Builder.CreateUnreachable();
        else
          EmitDefaultRet();
      } else {
        auto SuccIt = BBMap.find(Blk.Succs[0]);
        if (SuccIt != BBMap.end())
          Builder.CreateBr(SuccIt->second);
        else
          EmitDefaultRet();
      }
    }
  }

  // Emit the deferred per-predecessor index stores for shared -O0 computed-goto
  // dispatches recovered as switches (synthesizeSharedDispatchIndex): each
  // predecessor stores its own index into the common slot before its
  // terminator. All op-output allocas exist and each predecessor's body is
  // fully emitted, so getVar reads each index correctly (its def dominates the
  // predecessor's terminator) and the store reaches the dispatch's slot load on
  // that edge. Done before the phi/critical-edge pass: the store stays in the
  // predecessor block regardless of any later edge split, so the dispatch load
  // still observes it on the taken edge.
  for (auto &PD : PendingDispatchStores) {
    for (auto &PredIdx : PD.Preds) {
      auto PIt = BBMap.find(PredIdx.first);
      if (PIt == BBMap.end())
        continue;
      auto *Term = PIt->second->getTerminator();
      if (!Term)
        continue;
      llvm::IRBuilder<> SB(Term);
      llvm::Value *V = getVar(PredIdx.second, SB);
      if (!V || !V->getType()->isIntegerTy())
        continue;
      unsigned Have = V->getType()->getIntegerBitWidth();
      unsigned Want = PD.Ty->getIntegerBitWidth();
      if (Have > Want)
        V = SB.CreateTrunc(V, PD.Ty);
      else if (Have < Want)
        V = SB.CreateZExt(V, PD.Ty);
      SB.CreateStore(V, PD.Slot);
    }
  }

  // Phi emission with critical-edge splitting.  When a predecessor has
  // multiple successors (conditional branch), phi copies for one edge
  // must not clobber values needed by another edge.  We split such
  // edges by inserting an intermediate block that holds the copies.
  {
    // True when the predecessor block writes a register that overlaps `Narrow`
    // at the same offset but is wider — i.e. the narrow phi must re-resolve
    // through the wider register on this edge rather than self-reference.
    auto widerRegWrittenInBlock = [&](int PredId,
                                      const MedVar &Narrow) -> bool {
      if (!CurMedFunc || Narrow.Kind != MedVar::Reg || Narrow.Size == 0)
        return false;
      for (auto &Blk : CurMedFunc->Blocks) {
        if (Blk.Id != PredId)
          continue;
        for (auto &Op : Blk.Ops)
          if (Op.Output.Kind == MedVar::Reg &&
              Op.Output.RegOff == Narrow.RegOff && Op.Output.Size > Narrow.Size)
            return true;
        return false;
      }
      return false;
    };

    // Group: (PredId, TargetBlockId) → list of (PhiOutput, PhiArg)
    using EdgeKey = std::pair<int, int>;
    std::map<EdgeKey, std::vector<std::pair<MedVar, MedVar>>> EdgePhis;
    auto IsExceptionalEdge = [&](int PredId, int TargetId) {
      for (const MedBlock &Pred : Func.Blocks) {
        if (Pred.Id != PredId)
          continue;
        return std::any_of(Pred.ExceptionalSuccs.begin(),
                           Pred.ExceptionalSuccs.end(),
                           [&](const ExceptionalEdge &Edge) {
                             return Edge.BlockId == TargetId;
                           });
      }
      return false;
    };
    for (auto &Blk : Func.Blocks) {
      for (auto &Phi : Blk.Phis) {
        for (auto &[PredId, Var] : Phi.Args) {
          // A throwing call never reaches its ordinary block terminator.
          // These copies are emitted immediately before the call when it is
          // converted to an invoke by emitNativeItaniumEH().
          if (IsExceptionalEdge(PredId, Blk.Id))
            continue;
          EdgePhis[{PredId, Blk.Id}].emplace_back(Phi.Output, Var);
        }
      }
    }

    for (auto &[Edge, Copies] : EdgePhis) {
      auto [PredId, TargetId] = Edge;
      auto TargetIt = BBMap.find(TargetId);
      auto ExitsIt = ConceptualExits.find(PredId);
      if (ExitsIt == ConceptualExits.end() || TargetIt == BBMap.end())
        continue;
      auto *TargetBB = TargetIt->second;
      for (llvm::BasicBlock *PredBB : ExitsIt->second) {
        auto *Term = PredBB->getTerminator();
        if (!Term)
          continue;

        if (!llvm::isa<llvm::UncondBrInst, llvm::CondBrInst>(Term) &&
            !llvm::isa<llvm::SwitchInst>(Term) &&
            !llvm::isa<llvm::IndirectBrInst>(Term))
          continue;

        bool ReachesTarget = false;
        for (unsigned I = 0; I < Term->getNumSuccessors(); ++I)
          ReachesTarget |= Term->getSuccessor(I) == TargetBB;
        if (!ReachesTarget)
          continue;

        const bool NeedSplit = Term->getNumSuccessors() > 1;
        llvm::BasicBlock *InsertBB = PredBB;
        if (NeedSplit) {
          auto *SplitBB = llvm::BasicBlock::Create(PredBB->getContext(),
                                                   PredBB->getName() + ".phi." +
                                                       TargetBB->getName(),
                                                   CurFunc, TargetBB);

          for (unsigned I = 0; I < Term->getNumSuccessors(); ++I)
            if (Term->getSuccessor(I) == TargetBB)
              Term->setSuccessor(I, SplitBB);

          llvm::UncondBrInst::Create(TargetBB, SplitBB);
          InsertBB = SplitBB;
        }

        auto *InsTerm = InsertBB->getTerminator();
        if (!InsTerm)
          continue;
        llvm::IRBuilder<> InsBuilder(InsTerm);

        std::vector<std::pair<MedVar, llvm::Value *>> Pending;
        Pending.reserve(Copies.size());
        for (auto &[Dst, Src] : Copies) {
          llvm::Value *Val;
          if (Src.isConst()) {
            Val = llvm::ConstantInt::get(sizeToType(Dst.Size),
                                         static_cast<int64_t>(Src.ConstVal),
                                         /*isSigned=*/true);
          } else {
            auto SrcKey = std::make_pair(Src.Id, Src.SSAVer);
            auto DstKey = std::make_pair(Dst.Id, Dst.SSAVer);
            llvm::AllocaInst *HiddenAlloca = nullptr;
            // A self-edge X=X normally means X is loop-invariant on this edge,
            // so the copy is a no-op self-reference.  Only when a *wider*
            // overlapping register is written in this predecessor block must
            // the narrow view re-resolve through it (hide the alloca so getVar
            // reaches the wide register).  Hiding unconditionally would, for a
            // register that is genuinely loop-invariant in an inner loop, force
            // getVar's wide fallback to pull a different (outer-loop) wide
            // version and corrupt the value.
            if (SrcKey == DstKey && Src.Kind == MedVar::Reg &&
                widerRegWrittenInBlock(PredId, Src)) {
              auto It = VarAllocs.find(SrcKey);
              if (It != VarAllocs.end()) {
                HiddenAlloca = It->second;
                VarAllocs.erase(It);
              }
            }
            Val = getVar(Src, InsBuilder);
            if (HiddenAlloca)
              VarAllocs[SrcKey] = HiddenAlloca;
          }
          Pending.emplace_back(Dst, Val);
        }

        for (auto &[Dst, Val] : Pending)
          setVar(Dst, Val, InsBuilder);
      }
    }
  }

  // The models are mutually exclusive by target: the two Windows lowerings
  // need an x64 COFF frame with a Windows personality, and the Itanium one
  // needs an LSDA, so the first that recognizes the function is the only one
  // that can.
  if (!emitNativeSEH(Func, *LLVMFunc, BBMap) &&
      !emitNativeCxxEH(Func, *LLVMFunc, BBMap))
    emitNativeItaniumEH(Func, *LLVMFunc, BBMap);

  for (llvm::BasicBlock &Block : *LLVMFunc)
    for (llvm::Instruction &Instruction : Block)
      Instruction.setMetadata(language_eh_md::InternalSourceCallAttachment,
                              nullptr);

  for (auto &BB : *CurFunc) {
    if (BB.empty() || !BB.back().isTerminator()) {
      llvm::IRBuilder<> FixBuilder(&BB);
      if (CurFunc->getReturnType()->isVoidTy())
        FixBuilder.CreateRetVoid();
      else
        FixBuilder.CreateRet(
            llvm::ConstantInt::get(CurFunc->getReturnType(), 0));
    }
  }

  return LLVMFunc;
}

} // namespace neverd
