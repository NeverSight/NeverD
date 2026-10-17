//===- RegistrationCalleeStack.cpp - PE32 returning stack facts -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "RegistrationABIPrivate.h"
#include "RegistrationFrame.h"
#include "RegistrationFrameEffects.h"

#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/lift/X86Regs.h"
#include "neverd/loader/ReadOnlyBytes.h"
#include "neverd/loader/RegistrationScopeGraph.h"
#include "neverd/support/BinaryEncoding.h"

#include <deque>
#include <functional>
#include <map>
#include <set>

namespace neverd::registration_abi {
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
        (Insn.Id == X86_INS_MOV || Insn.Id == X86_INS_PUSH))
      AbsoluteOperands.push_back(Boundary.Address + X86.encoding.imm_offset);
  }
  return readImmutablePE32CodeBytes(Image, Boundary.Address, Boundary.Size,
                                    AbsoluteOperands)
      .has_value();
}

} // namespace neverd::registration_abi

namespace neverd {
namespace {
using registration_abi::chargeCalleeWork;
using registration_state::FrameState;
using registration_state::FrameTransfer;
using registration_state::FrameValue;

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
  va_t FunctionEntry;
  size_t &Work;
  Decoder Dec;
  std::map<int, const LowBlock *> Blocks;
  std::map<va_t, int> Entries;
  std::map<int, bool> Immutable;
  bool Valid = false;

  StackBody(const BinaryImage &Image, const LowFunc &Function, size_t &Work)
      : Image(Image), FunctionEntry(Function.Entry), Work(Work) {
    if (!Function.hasCompleteLiftCoverage() || !Dec.init(Image))
      return;
    if (auto Error = validateLowInstructionBoundaries(
            Function, LowInstructionBoundaryRequirement::Required)) {
      llvm::consumeError(std::move(Error));
      return;
    }
    for (const auto &Block : Function.Blocks)
      if (!chargeCalleeWork(Work, Block.Ops.size() + 1) ||
          !Blocks.emplace(Block.Id, &Block).second ||
          !Entries.emplace(Block.StartAddr, Block.Id).second)
        return;
    Valid = true;
  }

  bool immutableBlock(int Id) {
    if (!chargeCalleeWork(Work, 1))
      return false;
    auto [It, New] = Immutable.emplace(Id, false);
    if (!New)
      return It->second;
    for (const auto &Boundary : Blocks.at(Id)->InstructionBoundaries)
      if (!chargeCalleeWork(Work, 1) ||
          !registration_abi::hasImmutableCalleeInstruction(Image, Boundary,
                                                           Dec))
        return false;
    return It->second = true;
  }
};

std::optional<uint32_t>
stackPopForBody(StackBody &Body, va_t Entry,
                const std::function<std::optional<uint32_t>(va_t)> &NestedPop,
                const LowFunc *Registration = nullptr,
                const std::set<int> *RegistrationParents = nullptr) {
  if (!Body.Valid || !Body.Entries.count(Entry))
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
      Handler.Registers[x86reg::RBP / x86reg::GeneralRegStride] =
          FrameValue::frame(0);
      auto [Root, New] = Incoming.emplace(It->second, Handler);
      if ((New || Root->second.merge(Handler)) &&
          Queued.insert(It->second).second)
        Pending.push_back(It->second);
    }
  }
  std::optional<uint32_t> Pop;
  while (!Pending.empty()) {
    const int Id = Pending.front();
    Pending.pop_front();
    Queued.erase(Id);
    FrameState State = Incoming.at(Id);
    const auto &Block = *Blocks.at(Id);
    if (!Body.immutableBlock(Id) || (Registration && !ParentBlocks.count(Id)))
      return std::nullopt;
    FrameTransfer Transfer(State, 0);
    for (const auto &Op : Block.Ops) {
      size_t Width = Op.Output.Size;
      for (unsigned I = 0; I != Op.NumInputs; ++I)
        Width += Op.Inputs[I].Size;
      if (!chargeCalleeWork(Work, 1 + Width) ||
          (Op.Opcode == NdOp::INTRINSIC &&
           !registration_state::isFramePreservingIntrinsic(Op)))
        return std::nullopt;
      Transfer.beginInstruction(Op.Addr);
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
      if (Op.Opcode == NdOp::CALL && Op.NumInputs == 1 &&
          Op.Inputs[0].isConst() && Op.Inputs[0].Size == 4 &&
          State.Registers[x86reg::RSP / x86reg::GeneralRegStride].Offset)
        CalleePop = NestedPop(Op.Inputs[0].Offset);
      if (Op.Opcode == NdOp::INDIR_CALL && Op.NumInputs == 1 &&
          Op.Inputs[0].isConst() && Op.Inputs[0].Size == 4)
        if (const auto *Import = Image.findImportAt(Op.Inputs[0].Offset);
            Import && Import->IATAddr == Op.Inputs[0].Offset)
          CalleePop = registration_abi::checkedRegistrationImportStackPop(
              Image, Import->IATAddr);
      std::optional<FrameValue> CallSP;
      if (CalleePop) {
        LowOp Adjust;
        Adjust.Opcode = NdOp::INT_ADD;
        Adjust.Output = NdVar::reg(x86reg::RSP, 4);
        Adjust.addInput(Adjust.Output);
        Adjust.addInput(NdVar::cst(*CalleePop, 4));
        CallSP = Transfer.evaluate(Adjust, false);
      }
      Transfer.write(Op, Transfer.evaluate(Op, false));
      if (CallSP)
        State.Registers[x86reg::RSP / x86reg::GeneralRegStride] = *CallSP;
    }
    if (Block.Succs.empty() &&
        (Block.Ops.empty() || Block.Ops.back().Opcode != NdOp::RETURN))
      return std::nullopt;
    for (int Successor : Block.Succs) {
      if (!Blocks.count(Successor) ||
          !chargeCalleeWork(Work, State.OtherRegisterBytes.size() + 9))
        return std::nullopt;
      auto [It, New] = Incoming.emplace(Successor, State);
      if ((New || It->second.merge(State)) && Queued.insert(Successor).second)
        Pending.push_back(Successor);
    }
  }
  if (Registration)
    for (int Id : ParentBlocks)
      if (!Incoming.count(Id))
        return std::nullopt;
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

std::optional<uint32_t> stackPopForRegistration(
    StackBody &Body, const LowFunc &Function,
    const std::function<std::optional<uint32_t>(va_t)> &NestedPop) {
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
      if (!Pop || *Pop != 0)
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
  if (!Body.Valid || !EH || !EH->Registration || EH->Cxx ||
      EH->Registration->RealignedFrame || !States || !States->Complete ||
      !States->CallbackStatesComplete ||
      !States->RegistrationLifetimeComplete || !States->ChainOperationsComplete)
    return std::nullopt;
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
  return stackPopForBody(Body, Function.Entry, NestedPop, &Function, &Parents);
}

class ReturningStackProof {
  const BinaryImage &Image;
  size_t &Work;
  std::map<va_t, std::optional<uint32_t>> &Cache;
  RegistrationCallCalleeIndex &Owner;
  unsigned &Depth;
  StackBody *Parent;

public:
  ReturningStackProof(const BinaryImage &Image, size_t &Work,
                      std::map<va_t, std::optional<uint32_t>> &Cache,
                      RegistrationCallCalleeIndex &Owner, unsigned &Depth,
                      StackBody *Parent = nullptr)
      : Image(Image), Work(Work), Cache(Cache), Owner(Owner), Depth(Depth),
        Parent(Parent) {}

  std::optional<uint32_t> prove(va_t Target) {
    if (!chargeCalleeWork(Work, 1))
      return std::nullopt;
    // The parent may have dispatcher-entered returns. Its ordinary entry
    // cannot borrow the proof intended only for separately called interiors.
    if (Parent && Target == Parent->FunctionEntry)
      return std::nullopt;
    if (auto It = Cache.find(Target); It != Cache.end())
      return It->second;
    if (Depth == 32 || Cache.size() == 256 || Target > UINT32_MAX ||
        !Image.isCodeAddress(Target))
      return std::nullopt;
    if (Target <= UINT32_MAX - 6 && chargeCalleeWork(Work, 6)) {
      const auto Bytes =
          readImmutablePE32CodeBytes(Image, Target, 6, {Target + 2});
      if (Bytes && (*Bytes)[0] == 0xff && (*Bytes)[1] == 0x25)
        if (const auto Pop =
                registration_abi::checkedRegistrationImportStackPop(
                    Image, readLE<uint32_t>(Bytes->data() + 2))) {
          Cache.emplace(Target, Pop);
          return Pop;
        }
    }
    if ((!Parent || !Parent->Entries.count(Target)))
      if (const auto *EH = Image.ExceptionMetadata.findFunction(Target);
          EH &&
          (EH->Cxx || !EH->Registration || EH->Registration->RealignedFrame))
        return std::nullopt;
    // In-progress entries carry no fact: recursion cannot certify itself.
    // Even failed nested bodies consume the same cumulative work allowance.
    auto It = Cache.emplace(Target, std::nullopt).first;
    ++Depth;
    const auto Nested = [&](va_t Callee) { return prove(Callee); };
    if (Parent && Parent->Entries.count(Target)) {
      It->second = stackPopForBody(*Parent, Target, Nested);
    } else {
      Decoder Dec;
      if (Dec.init(Image)) {
        CFGBuilder Builder;
        Builder.setRegistrationCalleeIndex(&Owner);
        const LowFunc Callee =
            Builder.buildIntegerFrameProof(Image, Dec, Target, "abi-stack");
        StackBody Body(Image, Callee, Work);
        It->second = Callee.ExceptionMetadata
                         ? stackPopForRegistration(Body, Callee, Nested)
                         : stackPopForBody(Body, Target, Nested);
      }
    }
    --Depth;
    return It->second;
  }
};
} // namespace

std::optional<uint32_t> getCheckedX86CalleeStackPop(const BinaryImage &Image,
                                                    va_t Target,
                                                    size_t *CumulativeWork) {
  if (!isPE32(Image))
    return std::nullopt;
  return RegistrationCallCalleeIndex(Image, CumulativeWork).stackPop(Target);
}

std::optional<uint32_t> RegistrationCallCalleeIndex::stackPop(va_t Target) {
  if (!isPE32(Image))
    return std::nullopt;
  return ReturningStackProof(Image, Work, StackCache, *this, StackDepth)
      .prove(Target);
}

std::optional<std::vector<RegistrationCalleeStackContract>>
RegistrationCallCalleeIndex::stackContracts(const LowFunc &Function) {
  std::vector<RegistrationCalleeStackContract> Result;
  if (!isPE32(Image))
    return Result;
  std::set<std::pair<va_t, bool>> Targets;
  for (const auto &Block : Function.Blocks)
    for (const auto &Op : Block.Ops) {
      if (!chargeCalleeWork(Work, 1))
        return std::nullopt;
      if ((Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL) &&
          Op.NumInputs == 1 && Op.Inputs[0].isConst() && Op.Inputs[0].Size == 4)
        Targets.emplace(Op.Inputs[0].Offset, Op.Opcode == NdOp::INDIR_CALL);
      if (Targets.size() > 256)
        return std::nullopt;
    }
  // One graph and immutable-instruction cache for all interior callback
  // entries. Each entry still gets an independent reaching-register proof.
  StackBody Parent(Image, Function, Work);
  ReturningStackProof Proof(Image, Work, StackCache, *this, StackDepth,
                            &Parent);
  for (const auto &[Target, Indirect] : Targets) {
    if (Indirect) {
      // The registration runtime's existing provider contract. This gives
      // only stdcall cleanup, not permission to rewrite exception dispatch.
      const auto *Import = Image.findImportAt(Target);
      if (Import && Import->IATAddr == Target)
        if (auto Pop = registration_abi::checkedRegistrationImportStackPop(
                Image, Target))
          Result.push_back({Target, *Pop, true});
      continue;
    }
    if (Target == Function.Entry)
      continue;
    if (auto Pop = Proof.prove(Target))
      Result.push_back({Target, *Pop, false});
  }
  return Result;
}
} // namespace neverd
