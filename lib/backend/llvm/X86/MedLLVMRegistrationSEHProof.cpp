//===- MedLLVMRegistrationSEHProof.cpp - PE32 SEH preflight ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "MedLLVMRegistrationSEHProof.h"

#include "WindowsRegistrationFramePrivate.h"

#include "neverd/Limits.h"
#include "neverd/backend/llvm/X86RegistrationSEHBoundCall.h"
#include "neverd/backend/llvm/X86RegistrationSEHControl.h"
#include "neverd/backend/llvm/X86RegistrationSEHException.h"
#include "neverd/backend/llvm/X86RegistrationSEHJump.h"
#include "neverd/ir/med/X86RegistrationSEHLocalUnwind.h"

#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"

namespace neverd::x86_registration {

std::optional<SEHFrameValues> getPrivateSEHFrameValues(
    const MedFunc &Func, const RegistrationStateAnalysis &States,
    llvm::function_ref<bool(const MedOp &)> IsPrivateStore) {
  // A synthetic source frame must stay private. Otherwise an unknown callee
  // can modify a source registration field without changing LLVM's live node.
  // Consume the shared LowIR frame domain, including spill/reload provenance,
  // then carry that fact through synthetic MedIR copies and PHIs.
  using ValueKey = std::tuple<MedVar::VarKind, int, int>;
  auto Key = [](const MedVar &V) { return ValueKey{V.Kind, V.Id, V.SSAVer}; };
  std::set<std::pair<va_t, int>> FrameOccurrences;
  for (const auto &Value : States.FrameValues)
    FrameOccurrences.emplace(Value.Address, Value.OpSeq);
  SEHFrameValues Result;
  auto &FrameValues = Result.Values;
  for (const MedBlock &Block : Func.Blocks)
    for (const MedOp &Op : Block.Ops)
      if (!Op.Output.isConst() && Op.Output.Size &&
          (FrameOccurrences.count({Op.Addr, Op.OriginSeq}) ||
           Op.RegistrationRoot != MedOp::RegistrationRootKind::None))
        FrameValues.insert(Key(Op.Output));
  auto IsFrame = [&](const MedVar &V) {
    return !V.isConst() && FrameValues.count(Key(V));
  };
  bool Changed = true;
  size_t Work = 0;
  while (Changed) {
    Changed = false;
    for (const MedBlock &Block : Func.Blocks) {
      for (const PhiNode &Phi : Block.Phis) {
        if (++Work > limits::kMaxRegistrationEHStateWork)
          return std::nullopt;
        for (const auto &[Pred, Value] : Phi.Args)
          if (IsFrame(Value))
            Changed |= FrameValues.insert(Key(Phi.Output)).second;
      }
      for (const MedOp &Op : Block.Ops) {
        if (++Work > limits::kMaxRegistrationEHStateWork)
          return std::nullopt;
        if (!Op.Output.Size || Op.Output.isConst() || Op.Opcode == NdOp::LOAD ||
            Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL)
          continue;
        for (unsigned I = 0; I < Op.NumInputs; ++I)
          if (IsFrame(Op.Inputs[I]))
            Changed |= FrameValues.insert(Key(Op.Output)).second;
      }
    }
  }
  // LowIR conservatively retains possible address bits in exception DWORDs.
  // Their values come from the current runtime record, not from a relocated
  // source frame. Discharge that taint only for a pure scalar use closure;
  // combining it with any actual frame address keeps the existing restriction.
  const auto ExceptionAccesses = getX86RegistrationSEHExceptionAccesses(Func);
  const auto Bound = getX86RegistrationSEHBoundCalls(Func);
  if (!ExceptionAccesses || !Bound)
    return std::nullopt;
  std::set<ValueKey> RuntimeScalars;
  for (const MedBlock &Block : Func.Blocks)
    for (const MedOp &Op : Block.Ops) {
      const auto Access = ExceptionAccesses->find({Op.Addr, Op.OriginSeq});
      if (Access != ExceptionAccesses->end() &&
          (Access->second.Field ==
               RegistrationSEHExceptionAccess::Kind::ExceptionCode ||
           Access->second.Field ==
               RegistrationSEHExceptionAccess::Kind::ExceptionFlags))
        RuntimeScalars.insert(Key(Op.Output));
    }
  for (const auto &[Address, Call] : *Bound) {
    if (++Work > limits::kMaxRegistrationEHStateWork)
      return std::nullopt;
    // The original atomic word is a runtime scalar. Keep its real CALL and
    // reuse the scalar use/spill closure; no frame coordinate follows.
    if (Call.Source->CalleeKind ==
        RegistrationSEHBoundCall::Kind::InterlockedCompareExchange)
      RuntimeScalars.insert(Key(Call.Operation->Output));
  }
  auto IsScalar = [&](const MedVar &V) {
    return !V.isConst() && RuntimeScalars.count(Key(V));
  };
  auto IsUnrelocated = [&](const MedVar &V) {
    return !IsFrame(V) || IsScalar(V);
  };
  const auto FrameOffsets =
      RuntimeScalars.empty() ? std::nullopt : getSEHFrameOffsets(Func);
  Changed = !RuntimeScalars.empty();
  while (Changed) {
    Changed = false;
    for (const MedBlock &Block : Func.Blocks) {
      for (const PhiNode &Phi : Block.Phis) {
        if (++Work > limits::kMaxRegistrationEHStateWork)
          return std::nullopt;
        if (llvm::all_of(
                Phi.Args,
                [&](const auto &Arg) { return IsUnrelocated(Arg.second); }) &&
            llvm::any_of(Phi.Args,
                         [&](const auto &Arg) { return IsScalar(Arg.second); }))
          Changed |= RuntimeScalars.insert(Key(Phi.Output)).second;
      }
      for (const MedOp &Op : Block.Ops) {
        if (++Work > limits::kMaxRegistrationEHStateWork)
          return std::nullopt;
        if (!Op.Output.Size || Op.Output.isConst() || Op.Opcode == NdOp::LOAD ||
            Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL ||
            Op.RegistrationRoot != MedOp::RegistrationRootKind::None)
          continue;
        const auto Inputs = llvm::ArrayRef(Op.Inputs).take_front(Op.NumInputs);
        if (llvm::all_of(Inputs, IsUnrelocated) &&
            llvm::any_of(Inputs, IsScalar))
          Changed |= RuntimeScalars.insert(Key(Op.Output)).second;
      }
    }
    if (FrameOffsets) {
      const auto PreviousSize = RuntimeScalars.size();
      if (!extendSEHScalarSpills(Func, *FrameOffsets, RuntimeScalars, Work))
        return std::nullopt;
      Changed |= RuntimeScalars.size() != PreviousSize;
    }
  }
  for (const auto &Value : RuntimeScalars)
    FrameValues.erase(Value);
  const auto Jumps = getX86RegistrationSEHJumpCalls(Func);
  const auto LocalUnwinds = getX86RegistrationSEHLocalUnwinds(Func);
  if (!Jumps || !Bound || !LocalUnwinds)
    return std::nullopt;
  for (const MedCallInfo &Call : Func.CallInfos) {
    const auto Block = llvm::find_if(
        Func.Blocks, [&](const auto &B) { return B.Id == Call.BlockId; });
    const auto *Operation = Block != Func.Blocks.end() && Call.OpIdx >= 0 &&
                                    size_t(Call.OpIdx) < Block->Ops.size()
                                ? &Block->Ops[Call.OpIdx]
                                : nullptr;
    const auto Jump = Operation ? Jumps->find(Operation->Addr) : Jumps->end();
    const auto Local =
        Operation ? LocalUnwinds->find(Operation->Addr) : LocalUnwinds->end();
    // The cookie is decoded by XOR with EBP, so conservative address taint
    // reaches its recovered ECX argument. The exact source occurrence proves
    // that value is the image cookie; native lowering replaces this check with
    // the compiler's physical-frame check instead of passing a frame address.
    const bool CookieCheck =
        Operation && Operation->Opcode == NdOp::CALL &&
        Operation->NumInputs == 1 && Operation->Inputs[0].isConst() &&
        Operation->Inputs[0].ConstVal == States.CookieCheckVA &&
        States.SecurityCookiesComplete &&
        States.cookieCheck(Operation->Addr, Operation->OriginSeq) &&
        Call.Args.size() == 1 && Call.Args.front().Size == 4;
    for (size_t I = 0; I < Call.Args.size(); ++I)
      if (IsFrame(Call.Args[I]) &&
          !(Operation && Bound->count(Operation->Addr)) &&
          !(Local != LocalUnwinds->end() &&
            Local->second.Operation == Operation &&
            I == (Local->second.Source->Runtime.Protocol ==
                          RegistrationLocalUnwindContract::Kind::EH4
                      ? 1u
                      : 0u)) &&
          (I != 0 || (Jump == Jumps->end() && !CookieCheck)))
        return std::nullopt;
  }
  for (const MedBlock &Block : Func.Blocks)
    for (const MedOp &Op : Block.Ops) {
      // Indexed source-memory receipts currently describe ordinary loads and
      // stores. Do not admit an unbound read-modify-write effect.
      if (Op.Opcode == NdOp::ATOMIC_XCHG || Op.Opcode == NdOp::ATOMIC_ADD ||
          Op.Opcode == NdOp::ATOMIC_CMPXCHG)
        return std::nullopt;
      if ((Op.Opcode == NdOp::RETURN || Op.Opcode == NdOp::COND_BR ||
           Op.Opcode == NdOp::INDIR_BR || Op.Opcode == NdOp::CALL ||
           Op.Opcode == NdOp::INDIR_CALL) &&
          llvm::any_of(llvm::ArrayRef(Op.Inputs).take_front(Op.NumInputs),
                       IsFrame))
        return std::nullopt;
      if ((Op.Opcode == NdOp::ATOMIC_XCHG || Op.Opcode == NdOp::ATOMIC_ADD ||
           Op.Opcode == NdOp::ATOMIC_CMPXCHG) &&
          llvm::any_of(llvm::ArrayRef(Op.Inputs).take_front(Op.NumInputs),
                       IsFrame))
        return std::nullopt;
      if (Op.Opcode == NdOp::STORE && Op.NumInputs >= 2 &&
          IsFrame(Op.Inputs[1]) &&
          Op.MemoryAddressSpace != NdMemoryAddressSpace::X86FS) {
        if (!IsPrivateStore(Op))
          return std::nullopt;
      }
    }

  return Result;
}

std::optional<std::set<const llvm::StoreInst *>> getCheckedSEHCallbackStores(
    llvm::ArrayRef<X86RegistrationCallbackRequest> Requests) {
  std::set<const llvm::StoreInst *> Stores;
  size_t Work = 0;
  for (const auto &Request : Requests) {
    if (!Request.Frame.InferStackBounds)
      continue;
    auto Frame = Request.Frame;
    Frame.StackBytes = limits::kMaxRegistrationEHStateWork;
    Frame.StackPointerOffset = Frame.StackBytes;
    std::set<llvm::BasicBlock *> Body(Request.Blocks.begin(),
                                      Request.Blocks.end());
    std::map<llvm::StoreInst *, X86RegistrationRootKind> Seeds;
    for (const auto &Seed : Frame.RootSeeds)
      Seeds.emplace(Seed.Definition, Seed.Kind);
    std::set<llvm::AllocaInst *> Slots;
    if (auto Error = checkPrivateStack(
            Request.Entry->getModule()->getDataLayout(), *Request.Entry, Body,
            Seeds, Frame, Slots, Work, nullptr, {}, &Stores)) {
      llvm::consumeError(std::move(Error));
      return std::nullopt;
    }
  }
  return Stores;
}

std::optional<std::set<llvm::Instruction *>> getCheckedSEHChainInstructions(
    llvm::Function &Parent, const RegistrationStateAnalysis &States,
    const std::map<std::pair<va_t, int>, llvm::Instruction *>
        &RegistrationChainIR) {
  // Authenticate every emitted FS memory instruction, not just entries that
  // happen to survive in an occurrence map. Duplicate emission is unknown.
  std::set<llvm::Instruction *> ChainInstructions;
  for (const RegistrationChainAccess &Access : States.ChainAccesses) {
    auto It = RegistrationChainIR.find({Access.Address, Access.OpSeq});
    if (It == RegistrationChainIR.end() || !It->second ||
        !ChainInstructions.insert(It->second).second ||
        It->second->getFunction() != &Parent)
      return std::nullopt;
    auto *Load = llvm::dyn_cast<llvm::LoadInst>(It->second);
    auto *Store = llvm::dyn_cast<llvm::StoreInst>(It->second);
    const bool Read =
        Access.AccessKind == RegistrationChainAccess::Kind::ReadPreviousHead ||
        Access.AccessKind == RegistrationChainAccess::Kind::ReadInstalledHead;
    llvm::Value *Pointer = Load    ? Load->getPointerOperand()
                           : Store ? Store->getPointerOperand()
                                   : nullptr;
    const auto *Offset =
        llvm::dyn_cast_or_null<llvm::ConstantPointerNull>(Pointer);
    if (Read != (Load != nullptr) || !Pointer || !Offset ||
        Pointer->getType()->getPointerAddressSpace() != 257 ||
        (Load && (!Load->getType()->isIntegerTy(32) || Load->isAtomic() ||
                  Load->isVolatile())) ||
        (Store && (!Store->getValueOperand()->getType()->isIntegerTy(32) ||
                   Store->isAtomic() || Store->isVolatile())))
      return std::nullopt;
  }
  for (llvm::BasicBlock &Block : Parent)
    for (llvm::Instruction &Instruction : Block) {
      if (Instruction.isEHPad() || llvm::isa<llvm::InvokeInst>(Instruction) ||
          llvm::isa<llvm::CallBrInst>(Instruction) ||
          llvm::isa<llvm::IndirectBrInst>(Instruction) ||
          llvm::isa<llvm::ResumeInst>(Instruction))
        return std::nullopt;
      const llvm::Value *Pointer = nullptr;
      if (const auto *Load = llvm::dyn_cast<llvm::LoadInst>(&Instruction))
        Pointer = Load->getPointerOperand();
      if (const auto *Store = llvm::dyn_cast<llvm::StoreInst>(&Instruction))
        Pointer = Store->getPointerOperand();
      if (Pointer && Pointer->getType()->getPointerAddressSpace() == 257 &&
          !ChainInstructions.count(&Instruction))
        return std::nullopt;
      for (const llvm::Use &Operand : Instruction.operands())
        if (Operand->getType()->isPointerTy() &&
            Operand->getType()->getPointerAddressSpace() == 257 &&
            !ChainInstructions.count(&Instruction))
          return std::nullopt;
    }

  return ChainInstructions;
}

std::optional<SEHSourceOperations> getCheckedSEHSourceOperations(
    const MedFunc &Func,
    const std::map<int, llvm::BasicBlock *> &OriginalBlockMap,
    const std::map<std::pair<va_t, int>, llvm::Instruction *>
        &RegistrationMemoryIR,
    const std::map<const llvm::CallInst *, va_t> &CallSiteAddrs) {
  const auto &States = *Func.RegistrationStates;
  // Ordinary near returns use the outlined callback's C return ABI. Other
  // nonlocal exits still require their own replacement continuation contract.
  if (!getX86RegistrationSEHBoundCalls(Func) ||
      !getX86RegistrationSEHJumpCalls(Func) ||
      !getX86RegistrationSEHLocalUnwinds(Func))
    return std::nullopt;
  const auto ExceptionAccesses = getX86RegistrationSEHExceptionAccesses(Func);
  if (!ExceptionAccesses)
    return std::nullopt;
  for (const auto &[Identity, Access] : *ExceptionAccesses) {
    const auto Found = RegistrationMemoryIR.find(Identity);
    const auto *Load = Found == RegistrationMemoryIR.end()
                           ? nullptr
                           : llvm::dyn_cast<llvm::LoadInst>(Found->second);
    if (!Load || !Load->getType()->isIntegerTy(32) || Load->isAtomic())
      return std::nullopt;
  }
  const auto Returns = getX86RegistrationSEHReturnBlocks(Func);
  if (!Returns)
    return std::nullopt;
  for (const auto &[Id, Entry] : *Returns) {
    const auto Block = OriginalBlockMap.find(Id);
    if (Block == OriginalBlockMap.end() || !Block->second ||
        !llvm::isa<llvm::ReturnInst>(Block->second->getTerminator()))
      return std::nullopt;
  }

  std::vector<SourceOperation> SourceOperations;
  std::set<llvm::CallInst *> CookieCheckCalls;
  size_t MemoryOperations = 0;
  for (const MedBlock &Block : Func.Blocks)
    for (const MedOp &Op : Block.Ops) {
      llvm::Instruction *Instruction = nullptr;
      uint8_t Kind;
      if ((Op.Opcode == NdOp::LOAD || Op.Opcode == NdOp::STORE) &&
          Op.MemoryAddressSpace == NdMemoryAddressSpace::Default) {
        const auto It = RegistrationMemoryIR.find({Op.Addr, Op.OriginSeq});
        if (It == RegistrationMemoryIR.end() || !It->second)
          return std::nullopt;
        Instruction = It->second;
        Kind = Op.Opcode == NdOp::STORE;
        ++MemoryOperations;
      } else if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL) {
        for (const auto &[Call, Address] : CallSiteAddrs)
          if (Address == Op.Addr) {
            if (Instruction)
              return std::nullopt;
            Instruction = const_cast<llvm::CallInst *>(Call);
          }
        if (!Instruction)
          return std::nullopt;
        Kind = States.cookieCheck(Op.Addr, Op.OriginSeq) ? 3 : 2;
        if (Kind == 3) {
          auto *Call = llvm::cast<llvm::CallInst>(Instruction);
          if (!States.SecurityCookiesComplete || !States.CookieCheckVA ||
              Op.NumInputs != 1 || !Op.Inputs[0].isConst() ||
              Op.Inputs[0].ConstVal != States.CookieCheckVA ||
              !Call->use_empty() || Call->isMustTailCall())
            return std::nullopt;
          CookieCheckCalls.insert(Call);
        }
      } else
        continue;
      if (Op.OriginSeq < 0 ||
          Instruction->getParent() != OriginalBlockMap.at(Block.Id))
        return std::nullopt;
      SourceOperations.push_back({Instruction, &Op, Block.Id, Kind});
    }
  if (MemoryOperations != RegistrationMemoryIR.size())
    return std::nullopt;
  if (CookieCheckCalls.size() != States.CookieChecks.size())
    return std::nullopt;

  return SEHSourceOperations{std::move(SourceOperations),
                             std::move(CookieCheckCalls)};
}

bool collectDiscardedRegistrationResult(
    llvm::CallInst &Call, std::vector<llvm::Instruction *> &Dead) {
  std::set<llvm::Instruction *> Seen;
  std::vector<llvm::Value *> Pending{&Call};
  size_t Work = 0;
  while (!Pending.empty()) {
    auto *Value = Pending.back();
    Pending.pop_back();
    for (auto *User : Value->users()) {
      if (++Work > limits::kMaxRegistrationEHStateWork)
        return false;
      auto *Instruction = llvm::dyn_cast<llvm::Instruction>(User);
      if (!Instruction || Instruction->getFunction() != Call.getFunction())
        return false;
      if (!Seen.insert(Instruction).second)
        continue;
      if (auto *Cast = llvm::dyn_cast<llvm::CastInst>(Instruction);
          Cast && Cast->getSrcTy()->isIntegerTy() &&
          Cast->getDestTy()->isIntegerTy()) {
        Dead.push_back(Cast);
        Pending.push_back(Cast);
        continue;
      }
      auto *Store = llvm::dyn_cast<llvm::StoreInst>(Instruction);
      auto *Slot =
          Store ? llvm::dyn_cast<llvm::AllocaInst>(Store->getPointerOperand())
                : nullptr;
      if (!Store || Store->getValueOperand() != Value || Store->isVolatile() ||
          Store->isAtomic() || !Slot || !Slot->isStaticAlloca() ||
          !Slot->getAllocatedType()->isIntegerTy())
        return false;
      for (auto *SlotUser : Slot->users()) {
        if (++Work > limits::kMaxRegistrationEHStateWork)
          return false;
        if (auto *Read = llvm::dyn_cast<llvm::LoadInst>(SlotUser)) {
          if (Read->getPointerOperand() != Slot || Read->isVolatile() ||
              Read->isAtomic() || Read->getType() != Slot->getAllocatedType())
            return false;
          if (Seen.insert(Read).second) {
            Dead.push_back(Read);
            Pending.push_back(Read);
          }
          continue;
        }
        auto *Definition = llvm::dyn_cast<llvm::StoreInst>(SlotUser);
        if (!Definition || Definition->getPointerOperand() != Slot ||
            Definition->isVolatile() || Definition->isAtomic())
          return false;
      }
      Dead.push_back(Store);
    }
  }
  return true;
}

} // namespace neverd::x86_registration
