//===- RegistrationCalleeStack.cpp - PE32 returning stack facts -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "RegistrationABIPrivate.h"
#include "RegistrationCRTData.h"
#include "RegistrationCRTFrame.h"
#include "RegistrationFrame.h"
#include "RegistrationFrameBranch.h"
#include "RegistrationFrameEffects.h"
#include "RegistrationImportLoad.h"
#include "RegistrationMemoryPermissions.h"
#include "RegistrationOrdinaryGraph.h"
#include "RegistrationPrecisionArguments.h"
#include "RegistrationStackOwner.h"

#include "neverd/Limits.h"
#include "neverd/decode/InstructionRelocations.h"
#include "neverd/ir/Limits.h"
#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/low/RegistrationCallEffects.h"
#include "neverd/ir/low/RegistrationSharedCall.h"
#include "neverd/lift/X86Regs.h"
#include "neverd/loader/ReadOnlyBytes.h"
#include "neverd/loader/RegistrationScopeGraph.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/ScopeExit.h"

#include <deque>
#include <functional>
#include <map>
#include <set>
#include <vector>

namespace neverd::registration_abi {
thread_local StackProofOwnerScope *StackProofOwnerScope::Current = nullptr;

StackProofOwnerScope::StackProofOwnerScope(RegistrationCallCalleeIndex &Index,
                                           size_t &Work)
    : Index(Index), Work(Work), Level(x86SEHStateAnalysisLevel()),
      Previous(Current) {
  Current = this;
}

StackProofOwnerScope::~StackProofOwnerScope() { Current = Previous; }

const detail::AbsoluteRelocationRootIndex *
StackProofOwnerScope::relocationRoots(const BinaryImage &Image) {
  return Current && Current->Index.ownsImage(Image)
             ? &Current->Index.absoluteRelocationRoots()
             : nullptr;
}

RegistrationCallCalleeIndex *
StackProofOwnerScope::find(const BinaryImage &Image, size_t *CumulativeWork) {
  if (!Current || !CumulativeWork || !Current->Index.ownsImage(Image) ||
      CumulativeWork != &Current->Work ||
      Current->Level != x86SEHStateAnalysisLevel() || hasActiveCxxCalleeProof())
    return nullptr;
  return &Current->Index;
}

bool hasImmutableCalleeInstruction(const BinaryImage &Image,
                                   const LowInstructionBoundary &Boundary,
                                   Decoder &Decoder) {
  if (readImmutableCodeBytes(Image, Boundary.Address, Boundary.Size))
    return true;
  const auto *Bytes = Image.readVA(Boundary.Address, Boundary.Size);
  DecodedInsn Insn;
  if (!Bytes ||
      !Decoder.decodeOne(Bytes, Boundary.Size, Boundary.Address, Insn) ||
      Insn.Size != Boundary.Size || !Insn.Raw || !Insn.Raw->detail)
    return false;
  const auto &X86 = Insn.Raw->detail->x86;
  std::vector<va_t> AbsoluteOperands;
  for (unsigned I = 0; I != X86.op_count; ++I) {
    const auto &Operand = X86.operands[I];
    if (Operand.type == X86_OP_MEM && Operand.mem.base == X86_REG_INVALID &&
        X86.encoding.disp_size == 4)
      AbsoluteOperands.push_back(Boundary.Address + X86.encoding.disp_offset);
    if (Operand.type == X86_OP_IMM && X86.encoding.imm_size == 4 &&
        (Insn.Id == X86_INS_MOV || Insn.Id == X86_INS_PUSH ||
         Insn.Id == X86_INS_ADD || Insn.Id == X86_INS_SUB))
      AbsoluteOperands.push_back(Boundary.Address + X86.encoding.imm_offset);
  }
  if (!readImmutablePE32CodeBytes(Image, Boundary.Address, Boundary.Size,
                                  AbsoluteOperands))
    return false;
  // The stronger frame/memory proof consumes LowIR address identities. A
  // checked opcode alone does not authorize freezing an unclassified fixup's
  // preferred bits. The stack-only proof below may instead forget those bits.
  // Relocated ADD/SUB immediates retain the lifter's exact address identity;
  // accepting their encoding never turns an image-address difference into a
  // constant displacement or a private-frame address.
  const auto Relocations = instructionRelocations(Image, Insn);
  for (va_t Field : AbsoluteOperands)
    if (!readImmutableCodeBytes(Image, Field, 4) &&
        llvm::count_if(Relocations.Addresses, [&](const auto &Operand) {
          return Operand.FieldVA == Field && Operand.Width == 4 &&
                 !Operand.PCRelativeFromInstructionEnd &&
                 Operand.EncodedValue ==
                     readLE<uint32_t>(Bytes + Field - Boundary.Address) &&
                 isExactAddressProvenance(Operand.Provenance);
        }) != 1)
      return false;
  return true;
}

} // namespace neverd::registration_abi

namespace neverd {
namespace {
using registration_abi::chargeCalleeWork;
using registration_state::FrameState;
using registration_state::FrameTransfer;
using registration_state::FrameValue;
using NestedStackProof =
    std::function<std::optional<RegistrationCalleeStackContract>(va_t)>;

bool endsInReturn(const LowBlock &Block) {
  return !Block.Ops.empty() && Block.Ops.back().Opcode == NdOp::RETURN;
}

std::optional<uint32_t> nearReturnPop(const BinaryImage &Image, va_t Address) {
  const auto Byte = readImmutableCodeBytes(Image, Address, 1);
  if (!Byte || ((*Byte)[0] != 0xc3 && (*Byte)[0] != 0xc2))
    return std::nullopt;
  if ((*Byte)[0] == 0xc3)
    return 0;
  const auto Encoding = readImmutableCodeBytes(Image, Address, 3);
  if (!Encoding)
    return std::nullopt;
  return readLE<uint16_t>(Encoding->data() + 1);
}

struct StackBody {
  const BinaryImage &Image;
  const LowFunc &Source;
  va_t FunctionEntry;
  size_t &Work;
  Decoder Dec;
  std::map<int, const LowBlock *> Blocks;
  std::map<va_t, int> Entries;
  std::map<int, bool> Immutable;
  std::map<va_t, std::set<uint32_t>> VariantConstants;
  std::map<std::pair<va_t, int>, va_t> ImportLoads;
  std::map<std::pair<va_t, int>, va_t> ImportDataLoads;
  std::set<va_t> ImportSlots;
  uint32_t ArgumentReadBytes = 0;
  bool CheckedArgumentReadBytes = false;
  bool PreservesImportBindings = false;
  bool Valid = false;

  StackBody(const BinaryImage &Image, const LowFunc &Function, size_t &Work,
            va_t OrdinaryEntry = InvalidVA)
      : Image(Image), Source(Function), FunctionEntry(Function.Entry),
        Work(Work) {
    if (!Function.hasCompleteLiftCoverage() || !Dec.init(Image))
      return;
    if (auto Error = validateLowInstructionBoundaries(
            Function, LowInstructionBoundaryRequirement::Required)) {
      llvm::consumeError(std::move(Error));
      return;
    }
    for (const auto &Block : Function.Blocks) {
      if (!chargeCalleeWork(Work, Block.Ops.size() + 1) ||
          !Blocks.emplace(Block.Id, &Block).second ||
          !Entries.emplace(Block.StartAddr, Block.Id).second)
        return;
    }
    if (OrdinaryEntry != InvalidVA) {
      const auto Root = Entries.find(OrdinaryEntry);
      if (Root == Entries.end())
        return;
      std::vector<int> Pending{Root->second};
      std::set<int> Reached;
      while (!Pending.empty()) {
        const int Id = Pending.back();
        Pending.pop_back();
        if (!chargeCalleeWork(Work, 1) || !Blocks.count(Id))
          return;
        if (!Reached.insert(Id).second)
          continue;
        const auto &Successors = Blocks.at(Id)->Succs;
        if (!chargeCalleeWork(Work, Successors.size()))
          return;
        Pending.insert(Pending.end(), Successors.begin(), Successors.end());
      }
      // Other address-taken entries remain in the source CFG. This ordinary
      // invocation cannot execute their calls or depend on their IAT slots.
      // A separately called interior gets its own view rooted at that entry.
      if (!chargeCalleeWork(Work, Blocks.size() + Entries.size()))
        return;
      std::erase_if(Blocks,
                    [&](const auto &B) { return !Reached.count(B.first); });
      std::erase_if(Entries,
                    [&](const auto &E) { return !Reached.count(E.second); });
    }
    bool HasCachedCall = false;
    bool HasCRTData = false;
    for (const auto &[Id, Block] : Blocks) {
      if (!chargeCalleeWork(Work, Block->Ops.size() + 1))
        return;
      for (const auto &Op : Block->Ops) {
        HasCachedCall |= Op.Opcode == NdOp::INDIR_CALL && Op.NumInputs == 1 &&
                         Op.Inputs[0].Size == 4 && !Op.Inputs[0].isConst();
        // A possible absolute MOV requests the complete shared instruction
        // receipt below. A literal matching an IAT address proves nothing.
        if (!HasCRTData && OrdinaryEntry != InvalidVA &&
            !Function.ExceptionMetadata && Op.Opcode == NdOp::COPY &&
            Op.NumInputs == 1 && Op.Inputs[0].isConst() &&
            Op.Inputs[0].Size == 4)
          HasCRTData = registration_abi::isCheckedRegistrationCRTDataSlot(
              Image, Op.Inputs[0].Offset);
      }
    }
    if (HasCachedCall || HasCRTData)
      for (const auto &[Id, Block] : Blocks)
        for (const auto &Boundary : Block->InstructionBoundaries) {
          if (auto Load = registration_abi::checkedRegistrationImportLoad(
                  Image, *Block, Boundary, Work,
                  HasCRTData ? registration_abi::RegistrationImportLoadPolicy::
                                   IncludeCRTData
                             : registration_abi::RegistrationImportLoadPolicy::
                                   CallableOnly)) {
            auto &Loads =
                Load->Storage ==
                        registration_abi::RegistrationImportLoad::Kind::CRTData
                    ? ImportDataLoads
                    : ImportLoads;
            if (!Loads
                     .emplace(std::make_pair(Boundary.Address, Load->OpSeq),
                              Load->Slot)
                     .second)
              return;
            ImportSlots.insert(Load->Slot);
          }
          // An absent match must not hide exhaustion inside the query.
          if (!chargeCalleeWork(Work, 1))
            return;
        }
    for (const auto &[Id, Block] : Blocks) {
      if (!chargeCalleeWork(Work, Block->Ops.size() + 1))
        return;
      // Both stack cleanup and cached targets depend on the live binding.
      for (const auto &Op : Block->Ops)
        if (Op.Opcode == NdOp::INDIR_CALL && Op.NumInputs == 1 &&
            Op.Inputs[0].isConst() && Op.Inputs[0].Size == 4 &&
            Op.Inputs[0].Offset <= UINT32_MAX)
          ImportSlots.insert(Op.Inputs[0].Offset);
    }
    Valid = true;
  }

  bool hasOnlyReturnTerminals(va_t Entry) {
    std::vector<int> Pending{Entries.at(Entry)};
    std::set<int> Visited;
    while (!Pending.empty()) {
      const int Id = Pending.back();
      Pending.pop_back();
      if (!chargeCalleeWork(Work, 1) || !Blocks.count(Id))
        return false;
      if (!Visited.insert(Id).second)
        continue;
      const auto &Block = *Blocks.at(Id);
      if (Block.Succs.empty() && !endsInReturn(Block))
        return false;
      if (!chargeCalleeWork(Work, Block.Succs.size()))
        return false;
      Pending.insert(Pending.end(), Block.Succs.begin(), Block.Succs.end());
    }
    return true;
  }

  bool hasPrecisionResultCandidate() {
    // This is an admission to bounded path checking, not a result fact.
    // Actual arguments, current bindings and branch feasibility are proved
    // during transfer. Other unsupported terminals retain the fast refusal
    // before recursive callee discovery consumes a sibling's work allowance.
    for (const auto &[Id, Block] : Blocks) {
      if (!chargeCalleeWork(Work, Block->Ops.size() + 1))
        return false;
      for (const auto &Op : Block->Ops)
        if ((Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL) &&
            registration_state::isCheckedControlFPCall(Image, Op, Work))
          return true;
    }
    return false;
  }

  bool collectCalleeImports(const NestedStackProof &Nested) {
    // Collect before transfer: a store can invalidate a dependency of a
    // later direct helper, including a veneer hidden behind another helper.
    for (const auto &[Id, Block] : Blocks) {
      if (!chargeCalleeWork(Work, Block->Ops.size() + 1))
        return false;
      for (const auto &Op : Block->Ops)
        if (Op.Opcode == NdOp::CALL && Op.NumInputs == 1 &&
            Op.Inputs[0].isConst() && Op.Inputs[0].Size == 4) {
          if (const auto Callee = Nested(Op.Inputs[0].Offset)) {
            if (!chargeCalleeWork(Work, Callee->RequiredImports.size() + 1))
              return false;
            ImportSlots.insert(Callee->RequiredImports.begin(),
                               Callee->RequiredImports.end());
          }
          if (!chargeCalleeWork(Work, 1))
            return false;
        }
    }
    return true;
  }

  bool immutableInstruction(const LowInstructionBoundary &Boundary) {
    if (readImmutableCodeBytes(Image, Boundary.Address, Boundary.Size))
      return true;
    const auto *Bytes = Image.readVA(Boundary.Address, Boundary.Size);
    DecodedInsn Insn;
    if (!Bytes ||
        !Dec.decodeOne(Bytes, Boundary.Size, Boundary.Address, Insn) ||
        Insn.Size != Boundary.Size || !Insn.Raw || !Insn.Raw->detail)
      return false;
    const auto &X86 = Insn.Raw->detail->x86;
    std::vector<va_t> Operands;
    bool Memory = false, Immediate = false;
    for (unsigned I = 0; I != X86.op_count; ++I) {
      const auto &Operand = X86.operands[I];
      Memory |= Operand.type == X86_OP_MEM && Operand.mem.base != X86_REG_EIP &&
                Operand.mem.base != X86_REG_RIP;
      Immediate |= Operand.type == X86_OP_IMM;
    }
    if (Memory && X86.encoding.disp_size == 4)
      Operands.push_back(Boundary.Address + X86.encoding.disp_offset);
    // HIGHLOW is an absolute field, never a relative branch/call encoding.
    if (Immediate && X86.encoding.imm_size == 4 &&
        Boundary.Control == LowInstructionControl::None)
      Operands.push_back(Boundary.Address + X86.encoding.imm_offset);
    if (!readImmutablePE32CodeBytes(Image, Boundary.Address, Boundary.Size,
                                    Operands))
      return false;
    // The opcode is immutable, but these operand values may change with the
    // load address. The stack-only query below forgets unclassified values;
    // exact addresses retain their separate identity in the frame domain.
    // This grants no callee memory, return-value or native-EH authority.
    auto &Constants = VariantConstants[Boundary.Address];
    for (va_t Operand : Operands)
      Constants.insert(readLE<uint32_t>(Bytes + Operand - Boundary.Address));
    return true;
  }

  uint32_t callerArgumentReadBytes() {
    if (CheckedArgumentReadBytes)
      return ArgumentReadBytes;
    CheckedArgumentReadBytes = true;
    std::optional<uint32_t> Pop;
    for (const auto &[Id, Block] : Blocks) {
      if (!chargeCalleeWork(Work, Block->InstructionBoundaries.size() + 1))
        return 0;
      for (const auto &Boundary : Block->InstructionBoundaries)
        if (Boundary.Control == LowInstructionControl::Return) {
          const auto Current = nearReturnPop(Image, Boundary.Address);
          if (!Current || (Pop && *Pop != *Current))
            return 0;
          Pop = Current;
        }
    }
    ArgumentReadBytes = Pop.value_or(0);
    return ArgumentReadBytes;
  }

  LowOp stackOperation(const LowOp &Op) const {
    LowOp Abstract = Op;
    const auto Constants = VariantConstants.find(Op.Addr);
    for (unsigned I = 0; I != Abstract.NumInputs; ++I) {
      auto &Input = Abstract.Inputs[I];
      if (Input.isConst() && !isExactAddressProvenance(Input.Provenance) &&
          (isAddressProvenance(Input.Provenance) ||
           (Constants != VariantConstants.end() &&
            Constants->second.count(uint32_t(Input.Offset)))))
        // RAM is an unknown abstract value to FrameTransfer. This temporary
        // view is never emitted, and cannot turn a relocated literal (or a
        // numerically equal internal operand) into a fixed frame offset.
        Input = NdVar::ram(0, Input.Size);
    }
    return Abstract;
  }

  bool immutableBlock(int Id) {
    if (!chargeCalleeWork(Work, 1))
      return false;
    auto [It, New] = Immutable.emplace(Id, false);
    if (!New)
      return It->second;
    for (const auto &Boundary : Blocks.at(Id)->InstructionBoundaries)
      if (!chargeCalleeWork(Work, 1 + Boundary.Size) ||
          !immutableInstruction(Boundary))
        return false;
    return It->second = true;
  }
};

struct CxxStackPaths {
  std::optional<coff_loader::X86CxxPrologueCall> Entry;
  std::map<int, coff_loader::X86CxxEpilogueCall> Exits;
  std::set<std::pair<va_t, int>> NoReturnCalls;
};

std::optional<CxxStackPaths> cleanupCxxStackPaths(StackBody &Body,
                                                  const LowFunc &Function) {
  const auto &EH = *Function.ExceptionMetadata;
  const auto &States = *Function.RegistrationStates;
  if (!EH.Cxx || EH.CodeRange.Begin != Function.Entry ||
      !EH.Cxx->TryBlocks.empty() || !EH.Registration->Scopes.empty() ||
      States.Blocks.size() != Function.Blocks.size() ||
      !States.CallFrameEffectsComplete || !States.CleanupFrameEffectsComplete ||
      !States.CxxContinuationsComplete)
    return std::nullopt;
  CxxStackPaths Paths;
  if (!chargeCalleeWork(Body.Work, Function.Blocks.size() +
                                       States.ChainAccesses.size() + 1))
    return std::nullopt;
  Paths.Entry = getCheckedX86RegistrationCxxPrologueCall(Body.Image, Function);
  if (States.CxxPrologue && !Paths.Entry)
    return std::nullopt;
  const auto Handler = coff_loader::getCheckedX86CxxHandlerABI(Body.Image, EH);
  if (!Handler)
    return std::nullopt;
  Body.ImportSlots.insert(Handler->Dispatch.IATVA);
  for (const auto &Block : Function.Blocks) {
    if (!chargeCalleeWork(Body.Work,
                          Block.Ops.size() + States.ChainAccesses.size() + 1))
      return std::nullopt;
    if (Paths.Entry)
      if (auto Exit = getCheckedX86RegistrationCxxEpilogueCall(Body.Image,
                                                               Function, Block))
        Paths.Exits.emplace(Block.Id, *Exit);
    for (const auto &Op : Block.Ops) {
      const auto *Effect = States.callFrameEffect(Op.Addr, Op.Seq);
      if (!Effect || !Effect->DoesNotReturn)
        continue;
      if (Op.Opcode != NdOp::CALL || Op.NumInputs != 1 ||
          !Op.Inputs[0].isConst() || Op.Inputs[0].Size != 4 ||
          Effect->Target != Op.Inputs[0].Offset ||
          Effect->CalleeIndex >= States.CalleeContracts.size())
        return std::nullopt;
      const auto &Contract = States.CalleeContracts[Effect->CalleeIndex];
      if (Contract.isCxxParent()) {
        const auto Current = getCheckedX86RegistrationCxxCalleeABI(
            Body.Image, Effect->Target, &Body.Work);
        if (!Current || !Current->DoesNotReturn ||
            !hasSameX86RegistrationCxxEffects(Contract, *Current, Body.Work) ||
            Current->RequiredImports.empty() ||
            !chargeCalleeWork(Body.Work, Current->RequiredImports.size() + 1))
          return std::nullopt;
        Body.ImportSlots.insert(Current->RequiredImports.begin(),
                                Current->RequiredImports.end());
        Paths.NoReturnCalls.emplace(Op.Addr, Op.Seq);
        continue;
      }
      std::optional<va_t> Slot;
      if (Contract.isRuntimeThrow()) {
        if (const auto Import = getCheckedX86RegistrationThrowImportABI(
                Body.Image, Effect->Target, &Body.Work))
          Slot = Import->IATVA;
      } else if (const auto Throw = getCheckedX86RegistrationThrowCalleeABI(
                     Body.Image, Effect->Target, &Body.Work))
        Slot = Throw->ImportIATVA;
      if (!Slot || !chargeCalleeWork(Body.Work, 1))
        return std::nullopt;
      // Omitting a nonreturning path is itself a live import dependency.
      // A caller that replaced this slot cannot borrow the normal RET proof.
      Paths.NoReturnCalls.emplace(Op.Addr, Op.Seq);
      Body.ImportSlots.insert(*Slot);
    }
  }
  return Paths;
}

std::optional<uint32_t>
stackPopForBody(StackBody &Body, va_t Entry, const NestedStackProof &NestedPop,
                const LowFunc *Registration = nullptr,
                const std::set<int> *RegistrationParents = nullptr,
                const CxxStackPaths *CxxPaths = nullptr) {
  // Keep the cheap ordinary terminal refusal unless an authenticated CRT
  // call can supply a result for the bounded path proof. Only the reaching
  // invocation may prove that result; every reached terminal below still
  // requires its own return proof. Dispatcher paths retain their own rules.
  if (!Body.Valid || !Body.Entries.count(Entry) ||
      (!Registration && !CxxPaths && !Body.hasOnlyReturnTerminals(Entry) &&
       !Body.hasPrecisionResultCandidate()) ||
      !Body.collectCalleeImports(NestedPop))
    return std::nullopt;
  const auto &Image = Body.Image;
  size_t &Work = Body.Work;
  const auto &Blocks = Body.Blocks;
  const LowBlock *First = Blocks.at(Body.Entries.at(Entry));
  FrameState Initial;
  Initial.Registers[x86reg::RSP / x86reg::GeneralRegStride] =
      FrameValue::frame(Registration ? 4 : 0);
  std::map<int, FrameState> Incoming{{First->Id, Initial}};
  std::deque<int> Pending{First->Id};
  std::set<int> Queued{First->Id};
  std::set<int> ParentBlocks;
  if (Registration) {
    if (!RegistrationParents)
      return std::nullopt;
    ParentBlocks = *RegistrationParents;
    for (const auto &Scope :
         Registration->ExceptionMetadata->Registration->Scopes) {
      if (Scope.IsFinally)
        continue;
      const auto It = Body.Entries.find(Scope.HandlerVA);
      if (It == Body.Entries.end() || !ParentBlocks.count(It->second))
        continue;
      // The SEH dispatcher restores established EBP for an except body.
      // Its stack and memory values require independent evidence. In
      // particular, a saved-SP reload cannot inherit an opaque call's
      // unproved memory preservation through registration-state facts.
      FrameState Handler;
      Handler.ImportBindingsIntact = false;
      Handler.Registers[x86reg::RBP / x86reg::GeneralRegStride] =
          FrameValue::frame(0);
      auto [Root, New] = Incoming.emplace(It->second, Handler);
      if ((New || Root->second.merge(Handler)) &&
          Queued.insert(It->second).second)
        Pending.push_back(It->second);
    }
  }
  std::optional<uint32_t> Pop;
  bool PreservesEntryImports = !Registration && !CxxPaths;
  while (!Pending.empty()) {
    const int Id = Pending.front();
    Pending.pop_front();
    Queued.erase(Id);
    FrameState State = Incoming.at(Id);
    const auto &Block = *Blocks.at(Id);
    if (!Body.immutableBlock(Id) || (Registration && !ParentBlocks.count(Id)))
      return std::nullopt;
    FrameTransfer Transfer(State, 0);
    registration_abi::PrecisionArguments Arguments;
    registration_abi::CRTDataStoreProof CRTData;
    bool DoesNotReturn = false;
    for (const auto &Op : Block.Ops) {
      size_t Width = Op.Output.Size;
      for (unsigned I = 0; I != Op.NumInputs; ++I)
        Width += Op.Inputs[I].Size;
      if (!chargeCalleeWork(Work, 1 + Width))
        return std::nullopt;
      Transfer.beginInstruction(Op.Addr);
      const auto Abstract = Body.stackOperation(Op);
      if (PreservesEntryImports && Op.Opcode != NdOp::CALL &&
          Op.Opcode != NdOp::INDIR_CALL &&
          !registration_abi::preservesOrdinaryRegistrationBindings(
              Image, Abstract, Transfer, Body.callerArgumentReadBytes(), Work))
        PreservesEntryImports = false;
      bool CRTDataStore = false;
      if (!Body.ImportDataLoads.empty() && !Registration && !CxxPaths) {
        const auto DataLoad = Body.ImportDataLoads.find({Op.Addr, Op.Seq});
        const bool ClosedRead =
            Op.Opcode == NdOp::LOAD &&
            registration_abi::preservesOrdinaryRegistrationBindings(
                Image, Abstract, Transfer, Body.callerArgumentReadBytes(),
                Work);
        if (!CRTData.transfer(Abstract,
                              DataLoad == Body.ImportDataLoads.end()
                                  ? std::nullopt
                                  : std::optional<va_t>{DataLoad->second},
                              State.ImportBindingsIntact, ClosedRead, Work,
                              CRTDataStore))
          return std::nullopt;
      }
      if (!Registration && !CxxPaths && State.ImportBindingsIntact &&
          !Body.ImportSlots.empty() && !CRTDataStore &&
          (Op.Opcode == NdOp::LOAD || Op.Opcode == NdOp::STORE)) {
        const bool CurrentImportRead =
            Op.Opcode == NdOp::LOAD &&
            (Body.ImportLoads.count({Op.Addr, Op.Seq}) ||
             Body.ImportDataLoads.count({Op.Addr, Op.Seq}));
        if (!CurrentImportRead &&
            !registration_abi::hasClosedOrdinaryRegistrationMemoryAccess(
                Image, Abstract, Transfer, Body.callerArgumentReadBytes(),
                Work))
          // A continuing read/write-fault handler can replace an IAT without
          // changing the physical GPR context. Spatial disjointness alone
          // supplies no access permission; earlier resident targets stay
          // separate from fresh loads and direct slot calls.
          State.ImportBindingsIntact = false;
      }
      if (!Registration && !CxxPaths && Op.Opcode != NdOp::CALL &&
          Op.Opcode != NdOp::INDIR_CALL &&
          !Arguments.observe(Abstract, Transfer, Work))
        return std::nullopt;
      if (Op.Opcode == NdOp::INTRINSIC) {
        if (Op.NumInputs &&
            Op.Inputs[0] ==
                NdVar::cst(
                    static_cast<uint64_t>(Intrinsic::X86RequireDivPrecondition),
                    2) &&
            intrinsicX86DivPreconditionShapeIsValid(
                Intrinsic::X86RequireDivPrecondition,
                x86DivPreconditionLowShape(Op, Arch::X86))) {
          // Normal continuation preserves the physical integer coordinates.
          // A continuing divide-fault handler can still replace an IAT by
          // repairing memory alone. Keep earlier resident callable identities,
          // but no fresh binding; the source guard and EH policy stay intact.
          State.ImportBindingsIntact = false;
          continue;
        }
        if (!registration_state::isFramePreservingIntrinsic(Op))
          return std::nullopt;
      }
      if (!CRTDataStore && !Body.ImportSlots.empty()) {
        if (!chargeCalleeWork(Work, Body.ImportSlots.size() + 1))
          return std::nullopt;
        Transfer.invalidateImportBindings(Op, Body.ImportSlots);
      }
      if (CxxPaths && Op.Opcode == NdOp::CALL) {
        if (CxxPaths->Entry && Op.Addr == CxxPaths->Entry->CallVA) {
          if (State.Registers[x86reg::RSP / x86reg::GeneralRegStride].Offset !=
              0)
            return std::nullopt;
          State.Registers[x86reg::RBP / x86reg::GeneralRegStride] =
              FrameValue::frame(0);
          State.Registers[x86reg::RSP / x86reg::GeneralRegStride] =
              FrameValue::frame(CxxPaths->Entry->PostCallStackOffset);
          State.Registers[x86reg::RAX / x86reg::GeneralRegStride] =
              FrameValue::frame(CxxPaths->Entry->RegistrationOffset);
          State.OtherRegisterBytes.clear();
          continue;
        }
        if (const auto Exit = CxxPaths->Exits.find(Id);
            Exit != CxxPaths->Exits.end() &&
            Op.Addr == Exit->second.Caller.Begin) {
          if (State.Registers[x86reg::RBP / x86reg::GeneralRegStride].Offset !=
              0)
            return std::nullopt;
          // Saved-register words and PC need a separate source receipt.
          // Keep only the helper's physical returning ESP here.
          for (unsigned Register : {3, 5, 6, 7})
            State.Registers[Register] = {};
          State.Registers[x86reg::RCX / x86reg::GeneralRegStride] = {};
          State.Registers[x86reg::RSP / x86reg::GeneralRegStride] =
              FrameValue::frame(4);
          if (Exit->second.CookieCheck)
            State.OtherRegisterBytes.clear();
          continue;
        }
        if (CxxPaths->NoReturnCalls.count({Op.Addr, Op.Seq})) {
          if (!State.ImportBindingsIntact)
            return std::nullopt;
          DoesNotReturn = true;
          break;
        }
      }
      if (Op.Opcode == NdOp::RETURN) {
        if (State.Registers[x86reg::RSP / x86reg::GeneralRegStride].Offset !=
            (Registration ? 4 : 0))
          return std::nullopt;
        // Authenticate each near return, including zero versus nonzero pop.
        // LowFunc::CalleePopBytes is only a maximum and cannot prove agreement.
        const auto Bytes = nearReturnPop(Image, Op.Addr);
        if (!Bytes || (Pop && *Pop != *Bytes))
          return std::nullopt;
        Pop = Bytes;
      }
      // Deliberately do not store memory facts: an alias, nested call or a
      // saved-SP reload cannot supply an unproved restoration. Register-only
      // affine transfers share the registration state's authoritative rules.
      std::optional<uint32_t> CalleePop;
      bool ImportedCall = false;
      bool PreserveImportBindings = false;
      if (Op.Opcode == NdOp::CALL && Op.NumInputs == 1 &&
          Op.Inputs[0].isConst() && Op.Inputs[0].Size == 4 &&
          State.Registers[x86reg::RSP / x86reg::GeneralRegStride].Offset)
        if (const auto Callee = NestedPop(Op.Inputs[0].Offset);
            Callee &&
            (Callee->RequiredImports.empty() || State.ImportBindingsIntact)) {
          CalleePop = Callee->StackPopBytes;
          ImportedCall = Callee->ImportVeneer;
          PreserveImportBindings = Callee->PreservesImportBindings;
        }
      if (Op.Opcode == NdOp::INDIR_CALL && Op.NumInputs == 1 &&
          Op.Inputs[0].Size == 4 &&
          Op.MemoryAddressSpace == NdMemoryAddressSpace::Default &&
          Op.MemoryOrdering == NdMemoryOrdering::None) {
        const auto Slot =
            Op.Inputs[0].isConst()
                ? std::optional<va_t>{Op.Inputs[0].Offset}
                : std::optional<va_t>{Transfer.read(Op.Inputs[0]).ImportTarget};
        if (Slot && (!Op.Inputs[0].isConst() || State.ImportBindingsIntact))
          CalleePop =
              registration_abi::checkedRegistrationImportStackPop(Image, *Slot);
        ImportedCall = CalleePop.has_value() &&
                       (!Op.Inputs[0].isConst() || State.ImportBindingsIntact);
        PreserveImportBindings =
            ImportedCall &&
            registration_abi::preservesRegistrationImportBindings(Image, *Slot);
      }
      std::optional<FrameValue> CallSP;
      if (CalleePop) {
        LowOp Adjust;
        Adjust.Opcode = NdOp::INT_ADD;
        Adjust.Output = NdVar::reg(x86reg::RSP, 4);
        Adjust.addInput(Adjust.Output);
        Adjust.addInput(NdVar::cst(*CalleePop, 4));
        CallSP = Transfer.evaluate(Adjust, false);
      }
      const bool PrecisionSuccess =
          !Registration && !CxxPaths && ImportedCall && CalleePop == 0 &&
          Arguments.provesSuccessfulCall(Image, Op, State, Work);
      if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL)
        PreservesEntryImports &= PreserveImportBindings;
      const auto Load = Body.ImportLoads.find({Op.Addr, Op.Seq});
      Transfer.write(Abstract,
                     Transfer.evaluate(Abstract, false,
                                       Load == Body.ImportLoads.end()
                                           ? std::nullopt
                                           : std::optional<va_t>{Load->second}),
                     0, ImportedCall, PreserveImportBindings);
      if (CallSP)
        State.Registers[x86reg::RSP / x86reg::GeneralRegStride] = *CallSP;
      if (PrecisionSuccess)
        // The source CALL and its FP effects remain. This exact invocation's
        // zero ordinary result supplies only the following stack-proof branch.
        State.Registers[x86reg::RAX / x86reg::GeneralRegStride] =
            FrameValue::constant(0);
      if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL)
        Arguments.clear();
    }
    if (DoesNotReturn)
      continue;
    if (Block.Succs.empty() && !endsInReturn(Block))
      return std::nullopt;
    std::optional<va_t> KnownSuccessor;
    if (!Registration && !CxxPaths) {
      if (!chargeCalleeWork(Work, Block.Succs.size() + 1))
        return std::nullopt;
      KnownSuccessor = registration_state::knownFrameBranchTarget(
          Block, Transfer, [&](va_t Address) -> std::optional<int> {
            const auto Found = Body.Entries.find(Address);
            return Found == Body.Entries.end()
                       ? std::nullopt
                       : std::optional<int>{Found->second};
          });
    }
    for (int Successor : Block.Succs) {
      if (!Blocks.count(Successor) ||
          !chargeCalleeWork(Work, State.OtherRegisterBytes.size() + 9))
        return std::nullopt;
      // Both physical edges were checked by the shared frame-branch owner.
      // An unknown condition still visits both. Dispatcher-entered paths keep
      // their separate complete-parent graph obligation above.
      if (KnownSuccessor && *KnownSuccessor != Blocks.at(Successor)->StartAddr)
        continue;
      auto [It, New] = Incoming.emplace(Successor, State);
      if ((New || It->second.merge(State)) && Queued.insert(Successor).second)
        Pending.push_back(Successor);
    }
  }
  if (Registration)
    for (int Id : ParentBlocks)
      if (!Incoming.count(Id))
        return std::nullopt;
  // A single reached effect withdraws this summary for the entire invocation.
  // Revisited joins can only weaken it; no partial traversal publishes it.
  Body.PreservesImportBindings = Pop && PreservesEntryImports;
  return Pop;
}

bool isPE32(const BinaryImage &Image) {
  return Image.Arch == Arch::X86 && Image.Bits == Bitness::Bits32 &&
         Image.Format == BinaryFormat::COFF;
}

bool preservesFinallyFrame(StackBody &Body, va_t Entry) {
  if (!Body.Valid || !Body.Entries.count(Entry))
    return false;
  std::vector<int> Pending{Body.Entries.at(Entry)};
  std::set<int> Seen;
  while (!Pending.empty()) {
    const int Id = Pending.back();
    Pending.pop_back();
    if (!chargeCalleeWork(Body.Work, 1) || !Body.Blocks.count(Id))
      return false;
    if (!Seen.insert(Id).second)
      continue;
    const auto &Block = *Body.Blocks.at(Id);
    for (const auto &Op : Block.Ops) {
      if (!chargeCalleeWork(Body.Work, 1) || Op.Opcode == NdOp::CALL ||
          Op.Opcode == NdOp::INDIR_CALL ||
          (Op.Opcode == NdOp::INTRINSIC &&
           !registration_state::isFramePreservingIntrinsic(Op)) ||
          Op.Opcode == NdOp::INDIR_BR ||
          (Op.Output.isReg() && Op.Output.Size &&
           Op.Output.Offset < x86reg::RBP + 4 &&
           x86reg::RBP < Op.Output.Offset + Op.Output.Size))
        return false;
    }
    if (!chargeCalleeWork(Body.Work, Block.Succs.size()))
      return false;
    Pending.insert(Pending.end(), Block.Succs.begin(), Block.Succs.end());
  }
  return true;
}

bool hasCanonicalEH3StackEntry(const BinaryImage &Image,
                               const LowFunc &Function) {
  const auto &EH = *Function.ExceptionMetadata;
  const auto &Chain = *EH.Registration;
  if (EH.Personality != ExceptionPersonality::ExceptHandler3 ||
      EH.Encoding != ExceptionEncoding::X86ScopeTableEH3 ||
      EH.ParseStatus != ExceptionParseStatus::Complete ||
      EH.CodeRange.Begin != Function.Entry || Chain.RegistrationOffset != -16 ||
      Chain.TryLevelOffset != -4 || Chain.SeededTryLevel != -1 ||
      Function.Entry > UINT32_MAX - 29 ||
      Chain.ChainInstallVA != Function.Entry + 22 ||
      !hasValidRegistrationScopeGraph(Chain, false))
    return false;
  const auto Bytes = readImmutablePE32CodeBytes(
      Image, Function.Entry, 29, {Function.Entry + 6, Function.Entry + 11});
  if (!Bytes)
    return false;
  const auto *P = Bytes->data();
  return P[0] == 0x55 &&
         ((P[1] == 0x8b && P[2] == 0xec) || (P[1] == 0x89 && P[2] == 0xe5)) &&
         P[3] == 0x6a && P[4] == 0xff && P[5] == 0x68 &&
         readLE<uint32_t>(P + 6) == Chain.ScopeTableVA && P[10] == 0x68 &&
         readLE<uint32_t>(P + 11) == Chain.HandlerVA && P[15] == 0x64 &&
         P[16] == 0xa1 && readLE<uint32_t>(P + 17) == 0 && P[21] == 0x50 &&
         P[22] == 0x64 && P[23] == 0x89 && P[24] == 0x25 &&
         readLE<uint32_t>(P + 25) == 0;
}

std::optional<std::set<int>> registrationParentBlocks(StackBody &Body,
                                                      const LowFunc &Function) {
  std::vector<va_t> Roots{Function.Entry};
  for (const auto &Scope : Function.ExceptionMetadata->Registration->Scopes)
    if (!Scope.IsFinally)
      Roots.push_back(Scope.HandlerVA);
  std::vector<int> Pending;
  for (va_t Root : Roots) {
    if (!chargeCalleeWork(Body.Work, 1) || !Body.Entries.count(Root))
      return std::nullopt;
    Pending.push_back(Body.Entries.at(Root));
  }
  std::set<int> Result;
  while (!Pending.empty()) {
    const int Id = Pending.back();
    Pending.pop_back();
    if (!chargeCalleeWork(Body.Work, 1) || !Body.Blocks.count(Id))
      return std::nullopt;
    if (!Result.insert(Id).second)
      continue;
    const auto &Successors = Body.Blocks.at(Id)->Succs;
    if (!chargeCalleeWork(Body.Work, Successors.size()))
      return std::nullopt;
    Pending.insert(Pending.end(), Successors.begin(), Successors.end());
  }
  return Result;
}

std::optional<uint32_t>
stackPopForOrdinaryCxxRegistration(StackBody &Body, const LowFunc &Function,
                                   const NestedStackProof &NestedPop) {
  const auto &EH = Function.ExceptionMetadata;
  // Cleanup-only C++ has no dispatcher-entered parent continuation. Exact
  // physical helper calls can prove returning ESP independently of native
  // frame ownership, object effects and registration-chain lifetime.
  if (!Body.Valid || !EH || !EH->Registration || !EH->Cxx ||
      EH->ParseStatus != ExceptionParseStatus::Complete ||
      EH->CodeRange.Begin != Function.Entry ||
      EH->Registration->RealignedFrame || !EH->Cxx->TryBlocks.empty() ||
      !EH->Registration->Scopes.empty() ||
      !Body.hasOnlyReturnTerminals(Function.Entry))
    return std::nullopt;
  CxxStackPaths Paths;
  Paths.Entry =
      getCheckedX86RegistrationCxxProloguePrefix(Body.Image, Function);
  const auto Handler = coff_loader::getCheckedX86CxxHandlerABI(Body.Image, *EH);
  if (!Paths.Entry || !Handler)
    return std::nullopt;
  std::set<int> Parents;
  for (const auto &[Id, Block] : Body.Blocks) {
    if (!chargeCalleeWork(Body.Work, 1))
      return std::nullopt;
    Parents.insert(Id);
    if (const auto Exit = getCheckedX86RegistrationCxxEpilogueSuffix(
            Body.Image, Function, *Block)) {
      // An ordinary cookie-failure path still needs the complete source
      // proof. Handler guards belong to exception dispatch; neither kind is
      // omitted or authorized by this conditional normal-return fact.
      if (Exit->CookieCheck)
        return std::nullopt;
      Paths.Exits.emplace(Id, *Exit);
    }
  }
  if (Paths.Exits.empty())
    return std::nullopt;
  Body.ImportSlots.insert(Handler->Dispatch.IATVA);
  return stackPopForBody(Body, Function.Entry, NestedPop, &Function, &Parents,
                         &Paths);
}

std::optional<uint32_t>
stackPopForRegistration(StackBody &Body, const LowFunc &Function,
                        const NestedStackProof &NestedPop) {
  const auto &EH = Function.ExceptionMetadata;
  const auto &States = Function.RegistrationStates;
  // A termination-only frame has no dispatcher-entered parent body. Its
  // ordinary entry and near returns can therefore use the register-only
  // proof directly, even when an incoming object prevents a memory proof.
  // Each separately called finally still needs its own checked returning
  // stack proof. A nonlocal finally cannot obtain one by this route.
  if (Body.Valid && EH && EH->Registration && !EH->Cxx &&
      EH->ParseStatus == ExceptionParseStatus::Complete &&
      !EH->Registration->RealignedFrame && !EH->Registration->Scopes.empty() &&
      llvm::all_of(EH->Registration->Scopes,
                   [](const auto &Scope) { return Scope.IsFinally; })) {
    for (const auto &Scope : EH->Registration->Scopes) {
      if (!chargeCalleeWork(Body.Work, 1) || Scope.FilterVA ||
          !EH->CodeRange.contains(Scope.HandlerVA) ||
          !preservesFinallyFrame(Body, Scope.HandlerVA))
        return std::nullopt;
      const auto Pop = NestedPop(Scope.HandlerVA);
      if (!Pop || Pop->StackPopBytes != 0)
        return std::nullopt;
    }
    return stackPopForBody(Body, Function.Entry, NestedPop);
  }
  if (Body.Valid && EH && EH->Registration && !EH->Cxx &&
      !EH->Registration->RealignedFrame &&
      chargeCalleeWork(Body.Work, 29 + EH->Registration->Scopes.size()) &&
      hasCanonicalEH3StackEntry(Body.Image, Function)) {
    // A language-state or incoming-memory proof is not needed to establish
    // a register-only returning ESP fact. The checked direct prologue owns
    // EBP, and EH3 establishes it independently at each except entry. Follow
    // every parent path, including handlers, without importing any saved
    // memory facts or mistaking filter/finally returns for parent returns.
    const auto Parents = registrationParentBlocks(Body, Function);
    if (!Parents)
      return std::nullopt;
    return stackPopForBody(Body, Function.Entry, NestedPop, &Function,
                           &*Parents);
  }
  if (!Body.Valid || !EH || !EH->Registration ||
      EH->Registration->RealignedFrame || !States || !States->Complete ||
      !States->CallbackStatesComplete ||
      !States->RegistrationLifetimeComplete || !States->ChainOperationsComplete)
    return std::nullopt;
  std::optional<CxxStackPaths> CxxPaths;
  if (EH->Cxx) {
    CxxPaths = cleanupCxxStackPaths(Body, Function);
    if (!CxxPaths)
      return std::nullopt;
  }
  std::set<int> Parents;
  for (const auto &State : States->Blocks) {
    if (!State.Reached)
      continue;
    if (!Body.Blocks.count(State.BlockId) || State.Unknown ||
        !Body.immutableBlock(State.BlockId))
      return std::nullopt;
    const auto &Block = *Body.Blocks.at(State.BlockId);
    if (State.Range.Begin != Block.StartAddr ||
        State.Range.End != Block.EndAddr)
      return std::nullopt;
    if (!State.CallbackOnly)
      Parents.insert(State.BlockId);
  }
  // Runtime filter/finally returns pop a dispatcher invocation, not this
  // function's caller. The complete state proof separates those paths; the
  // independent register-only proof still owns every parent return's ESP.
  return stackPopForBody(Body, Function.Entry, NestedPop, &Function, &Parents,
                         CxxPaths ? &*CxxPaths : nullptr);
}

// A subsidiary object/initializer query can start a new public index. Keep
// the complete active body path across that boundary, including its limit.
using StackQuery = std::pair<const BinaryImage *, va_t>;
thread_local std::set<StackQuery> ActiveStackProofs;

class ReturningStackProof {
  const BinaryImage &Image;
  size_t &Work;
  std::map<va_t, std::optional<RegistrationCalleeStackContract>> &Cache;
  RegistrationCallCalleeIndex &Owner;
  unsigned &Depth;
  StackBody *Parent;

public:
  ReturningStackProof(
      const BinaryImage &Image, size_t &Work,
      std::map<va_t, std::optional<RegistrationCalleeStackContract>> &Cache,
      RegistrationCallCalleeIndex &Owner, unsigned &Depth,
      StackBody *Parent = nullptr)
      : Image(Image), Work(Work), Cache(Cache), Owner(Owner), Depth(Depth),
        Parent(Parent) {}

  std::optional<RegistrationCalleeStackContract> prove(va_t Target) {
    const registration_abi::StackProofOwnerScope OwnerScope(Owner, Work);
    if (!chargeCalleeWork(Work, 1))
      return std::nullopt;
    // The parent may have dispatcher-entered returns. Its ordinary entry
    // cannot borrow the proof intended only for separately called interiors.
    if (Parent && Target == Parent->FunctionEntry)
      return std::nullopt;
    const bool LocalEntry = Parent && Parent->Entries.count(Target);
    // Local witnesses belong to this source graph. Neither a cached standalone
    // refusal nor a local success may answer the other entry's proof domain.
    if (!LocalEntry)
      if (auto It = Cache.find(Target); It != Cache.end()) {
        if (It->second &&
            !chargeCalleeWork(Work, It->second->RequiredImports.size() + 1))
          return std::nullopt;
        return It->second;
      }
    const StackQuery Identity{&Image, Target};
    if (Depth == ::neverd::limits::ir::kMaxReturningStackProofDepth ||
        ActiveStackProofs.size() >=
            ::neverd::limits::ir::kMaxReturningStackProofDepth ||
        ActiveStackProofs.count(Identity) ||
        Cache.size() ==
            ::neverd::limits::ir::kRegistrationCalleeStackProveCacheSizeLimit ||
        Target > UINT32_MAX || !Image.isCodeAddress(Target))
      return std::nullopt;
    if (Target <= UINT32_MAX - 6 && chargeCalleeWork(Work, 6)) {
      const auto Bytes =
          readImmutablePE32CodeBytes(Image, Target, 6, {Target + 2});
      if (Bytes && (*Bytes)[0] == 0xff && (*Bytes)[1] == 0x25)
        if (const auto Pop =
                registration_abi::checkedRegistrationImportStackPop(
                    Image, readLE<uint32_t>(Bytes->data() + 2))) {
          if (!chargeCalleeWork(Work, 2))
            return std::nullopt;
          RegistrationCalleeStackContract Result{
              Target, *Pop, false, {}, {readLE<uint32_t>(Bytes->data() + 2)}};
          Result.ImportVeneer = true;
          Result.PreservesImportBindings =
              registration_abi::preservesRegistrationImportBindings(
                  Image, Result.RequiredImports.front());
          if (!LocalEntry)
            Cache.emplace(Target, Result);
          return Result;
        }
    }
    if (!LocalEntry)
      if (const auto *EH = Image.ExceptionMetadata.findFunction(Target);
          EH && (!EH->Registration || EH->Registration->RealignedFrame ||
                 (EH->Cxx && !EH->Cxx->TryBlocks.empty())))
        return std::nullopt;
    // In-progress entries carry no fact: recursion cannot certify itself.
    // Even failed nested bodies consume the same cumulative work allowance.
    std::optional<RegistrationCalleeStackContract> LocalResult;
    auto &Result = LocalEntry
                       ? LocalResult
                       : Cache.emplace(Target, std::nullopt).first->second;
    ActiveStackProofs.insert(Identity);
    ++Depth;
    const llvm::scope_exit Leave([&] {
      --Depth;
      ActiveStackProofs.erase(Identity);
    });
    const auto Nested = [&](va_t Callee) {
      // An independently built callee cannot export a local witness through
      // its transitive dependencies either. Its children use the same entry
      // admission as a fresh standalone query.
      return LocalEntry ? prove(Callee)
                        : ReturningStackProof(Image, Work, Cache, Owner, Depth)
                              .prove(Callee);
    };
    const auto Record = [&](StackBody &Body, std::optional<uint32_t> Pop) {
      if (Pop && chargeCalleeWork(Work, Body.ImportSlots.size() + 1)) {
        Result = RegistrationCalleeStackContract{
            Target,
            *Pop,
            false,
            {},
            {Body.ImportSlots.begin(), Body.ImportSlots.end()}};
        Result->PreservesImportBindings = Body.PreservesImportBindings;
      }
    };
    if (LocalEntry) {
      StackBody EntryBody(Image, Parent->Source, Work, Target);
      Record(EntryBody, stackPopForBody(EntryBody, Target, Nested));
    } else {
      Decoder Dec;
      if (Dec.init(Image)) {
        CFGBuilder Builder;
        Builder.setRegistrationCalleeIndex(&Owner);
        if (const auto *EH = Image.ExceptionMetadata.findFunction(Target);
            EH && EH->Registration && EH->Cxx &&
            EH->CodeRange.Begin == Target &&
            EH->ParseStatus == ExceptionParseStatus::Complete &&
            EH->Cxx->TryBlocks.empty() && EH->Registration->Scopes.empty() &&
            !EH->Registration->RealignedFrame) {
          const LowFunc Ordinary = registration_abi::buildOrdinaryCxxFrameGraph(
              Image, Dec, Target, Owner);
          StackBody Body(Image, Ordinary, Work, Target);
          Record(Body,
                 stackPopForOrdinaryCxxRegistration(Body, Ordinary, Nested));
        }
        if (!Result && Work < limits::kMaxRegistrationEHStateWork) {
          const LowFunc Callee =
              Builder.buildIntegerFrameProof(Image, Dec, Target, "abi-stack");
          StackBody Body(Image, Callee, Work,
                         Callee.ExceptionMetadata ? InvalidVA : Target);
          Record(Body, Callee.ExceptionMetadata
                           ? stackPopForRegistration(Body, Callee, Nested)
                           : stackPopForBody(Body, Target, Nested));
        }
      }
    }
    if (Result && !chargeCalleeWork(Work, Result->RequiredImports.size() + 1))
      return std::nullopt;
    return Result;
  }
};
} // namespace

namespace {
thread_local RegistrationStackPopMemo *CurrentStackPopMemo = nullptr;
} // namespace

RegistrationStackPopMemoScope::RegistrationStackPopMemoScope(
    RegistrationStackPopMemo &Memo)
    : Previous(CurrentStackPopMemo) {
  CurrentStackPopMemo = &Memo;
}

RegistrationStackPopMemoScope::~RegistrationStackPopMemoScope() {
  CurrentStackPopMemo = Previous;
}

std::optional<RegistrationStackPopMemo::Answer>
RegistrationStackPopMemo::find(const BinaryImage &Other, va_t Target,
                               unsigned Level) {
  if (&Other != &Image)
    return std::nullopt;
  std::lock_guard<std::mutex> Lock(Mutex);
  const auto It = Answers.find({Target, Level});
  if (It == Answers.end())
    return std::nullopt;
  return It->second;
}

void RegistrationStackPopMemo::record(const BinaryImage &Other, va_t Target,
                                      unsigned Level, const Answer &Value) {
  if (&Other != &Image)
    return;
  std::lock_guard<std::mutex> Lock(Mutex);
  Answers.emplace(std::make_pair(Target, Level), Value);
}

namespace {
RegistrationStackPopMemo::Answer
freshStackPopAnswer(const BinaryImage &Image, va_t Target, size_t &Work,
                    const detail::AbsoluteRelocationRootIndex *Roots) {
  // A fresh index answers from the unchanged image and the SEH analysis
  // level alone, charging the same work every time; only the caller's
  // remaining budget decides whether that work fits. Every charge counts
  // toward that total, so it fits exactly when the recorded work does.
  RegistrationStackPopMemo *Memo =
      !ActiveStackProofs.empty() || registration_abi::hasActiveCxxCalleeProof()
          ? nullptr
          : CurrentStackPopMemo;
  const unsigned Level = x86SEHStateAnalysisLevel();
  if (Memo)
    if (const auto Known = Memo->find(Image, Target, Level)) {
      if (!registration_abi::chargeCalleeWork(Work, Known->Work))
        return {};
      return *Known;
    }
  const size_t Before = Work;
  const auto Proof =
      RegistrationCallCalleeIndex(Image, &Work, Roots).stackContract(Target);
  const RegistrationStackPopMemo::Answer Answer{
      Proof ? std::optional<uint32_t>{Proof->StackPopBytes} : std::nullopt,
      Work - Before, Proof && !Proof->RequiredImports.empty()};
  // A failed charge leaves Work at the limit, so a smaller total proves the
  // computation never ran out.
  if (Memo && Work < limits::kMaxRegistrationEHStateWork)
    Memo->record(Image, Target, Level, Answer);
  return Answer;
}
} // namespace

std::optional<uint32_t> getCheckedX86CalleeStackPop(const BinaryImage &Image,
                                                    va_t Target,
                                                    size_t *CumulativeWork) {
  if (!isPE32(Image))
    return std::nullopt;
  // Reuse only independently admitted entries from this invocation closure.
  // stackPop() retains the same recursion guards and import dependencies;
  // source-graph witnesses never enter its cache.
  if (auto *Owner =
          registration_abi::StackProofOwnerScope::find(Image, CumulativeWork))
    return Owner->stackPop(Target);
  size_t LocalWork = 0;
  size_t &Work = CumulativeWork ? *CumulativeWork : LocalWork;
  return freshStackPopAnswer(Image, Target, Work, nullptr).Pop;
}

std::optional<uint32_t> RegistrationCallCalleeIndex::stackPop(va_t Target) {
  const auto Proof = stackContract(Target);
  return Proof ? std::optional<uint32_t>{Proof->StackPopBytes} : std::nullopt;
}

std::optional<RegistrationCalleeStackContract>
RegistrationCallCalleeIndex::stackContract(va_t Target) {
  if (!isPE32(Image))
    return std::nullopt;
  return ReturningStackProof(Image, Work, StackCache, *this, StackDepth)
      .prove(Target);
}

std::optional<uint32_t> getCheckedX86IndependentCalleeStackPop(
    const BinaryImage &Image, va_t Target, size_t &Work, size_t Limit,
    const detail::AbsoluteRelocationRootIndex *Roots) {
  if (!isPE32(Image) || Work >= Limit || !ActiveStackProofs.empty() ||
      registration_abi::StackProofOwnerScope::active() ||
      registration_abi::hasActiveCxxCalleeProof() || x86SEHStateAnalysisLevel())
    return std::nullopt;
  // Embed the caller's remaining allowance in the existing cumulative work
  // account. Nested bodies share this exact counter and owner; they never
  // acquire a new allowance. A short outer budget stops the proof itself,
  // rather than merely rejecting an answer after unbounded work was done.
  const size_t Allowance =
      std::min(Limit - Work, limits::kMaxRegistrationEHStateWork);
  const size_t Before = limits::kMaxRegistrationEHStateWork - Allowance;
  size_t CalleeWork = Before;
  const auto Answer = freshStackPopAnswer(Image, Target, CalleeWork, Roots);
  Work += CalleeWork - Before;
  if (Answer.HasImportDependencies)
    return std::nullopt;
  return Answer.Pop;
}

std::optional<std::vector<RegistrationCalleeStackContract>>
RegistrationCallCalleeIndex::stackContracts(const LowFunc &Function) {
  std::vector<RegistrationCalleeStackContract> Result;
  if (!isPE32(Image))
    return Result;
  const auto FrameCalls =
      registration_abi::checkedRegistrationFrameCalls(Image, Function, Work);
  if (!FrameCalls)
    return std::nullopt;
  std::set<std::pair<va_t, bool>> Targets;
  for (const auto &Block : Function.Blocks)
    for (const auto &Op : Block.Ops) {
      if (!chargeCalleeWork(Work, 1))
        return std::nullopt;
      if (!FrameCalls->count(&Op) &&
          (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL) &&
          Op.NumInputs == 1 && Op.Inputs[0].isConst() && Op.Inputs[0].Size == 4)
        Targets.emplace(Op.Inputs[0].Offset, Op.Opcode == NdOp::INDIR_CALL);
      if (Targets.size() >
          ::neverd::limits::ir::
              kRegistrationCalleeStackRegistrationCallCalleeIndexStackContractsTargetsSizeLimit)
        return std::nullopt;
    }
  // Retain one source graph for all interior callback entries. Each queried
  // entry gets its own reachable instructions, import dependencies and
  // reaching-register proof.
  StackBody Parent(Image, Function, Work);
  if (!chargeCalleeWork(Work, 1))
    return std::nullopt;
  std::map<va_t, std::vector<std::pair<va_t, int>>> ImportLoads;
  if (Parent.Valid)
    for (const auto &[Occurrence, Slot] : Parent.ImportLoads) {
      if (!chargeCalleeWork(Work, 1))
        return std::nullopt;
      Targets.emplace(Slot, true);
      ImportLoads[Slot].push_back(Occurrence);
    }
  if (Targets.size() >
      ::neverd::limits::ir::
          kRegistrationCalleeStackRegistrationCallCalleeIndexStackContractsTargetsSizeLimit)
    return std::nullopt;
  ReturningStackProof Proof(Image, Work, StackCache, *this, StackDepth,
                            &Parent);
  for (const auto &[Target, Indirect] : Targets) {
    if (Indirect) {
      // Stack cleanup and normal binding preservation are separate facts;
      // neither permits rewriting exception dispatch or borrowing a frame.
      const auto *Import = Image.findImportAt(Target);
      if (Import && Import->IATAddr == Target)
        if (auto Pop = registration_abi::checkedRegistrationImportStackPop(
                Image, Target)) {
          RegistrationCalleeStackContract Contract{Target, *Pop, true,
                                                   ImportLoads[Target]};
          Contract.PreservesImportBindings =
              registration_abi::preservesRegistrationImportBindings(Image,
                                                                    Target);
          Result.push_back(std::move(Contract));
        }
      continue;
    }
    if (Target == Function.Entry)
      continue;
    if (auto Callee = Proof.prove(Target))
      Result.push_back(std::move(*Callee));
  }
  return Result;
}
} // namespace neverd
