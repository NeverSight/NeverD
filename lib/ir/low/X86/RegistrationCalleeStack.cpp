//===- RegistrationCalleeStack.cpp - PE32 returning stack facts -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "RegistrationABIPrivate.h"
#include "RegistrationFrame.h"

#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/lift/X86Regs.h"
#include "neverd/loader/ReadOnlyBytes.h"
#include "neverd/support/BinaryEncoding.h"

#include <deque>
#include <functional>
#include <map>
#include <set>

namespace neverd {
namespace {
using registration_abi::chargeCalleeWork;
using registration_state::FrameState;
using registration_state::FrameTransfer;
using registration_state::FrameValue;

bool hasImmutableInstruction(const BinaryImage &Image,
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
          !hasImmutableInstruction(Image, Boundary, Dec))
        return false;
    return It->second = true;
  }
};

std::optional<uint32_t>
stackPopForBody(StackBody &Body, va_t Entry,
                const std::function<std::optional<uint32_t>(va_t)> &NestedPop) {
  if (!Body.Valid || !Body.Entries.count(Entry))
    return std::nullopt;
  const auto &Image = Body.Image;
  size_t &Work = Body.Work;
  const auto &Blocks = Body.Blocks;
  const LowBlock *First = Blocks.at(Body.Entries.at(Entry));
  FrameState Initial;
  Initial.Registers[x86reg::RSP / x86reg::GeneralRegStride] =
      FrameValue::frame(0);
  std::map<int, FrameState> Incoming{{First->Id, Initial}};
  std::deque<int> Pending{First->Id};
  std::set<int> Queued{First->Id};
  std::optional<uint32_t> Pop;
  while (!Pending.empty()) {
    const int Id = Pending.front();
    Pending.pop_front();
    Queued.erase(Id);
    FrameState State = Incoming.at(Id);
    const auto &Block = *Blocks.at(Id);
    if (!Body.immutableBlock(Id))
      return std::nullopt;
    FrameTransfer Transfer(State, 0);
    for (const auto &Op : Block.Ops) {
      size_t Width = Op.Output.Size;
      for (unsigned I = 0; I != Op.NumInputs; ++I)
        Width += Op.Inputs[I].Size;
      if (!chargeCalleeWork(Work, 1 + Width) || Op.Opcode == NdOp::INTRINSIC)
        return std::nullopt;
      Transfer.beginInstruction(Op.Addr);
      if (Op.Opcode == NdOp::RETURN) {
        if (State.Registers[x86reg::RSP / x86reg::GeneralRegStride].Offset != 0)
          return std::nullopt;
        // Authenticate each near return, including zero versus nonzero pop.
        // LowFunc::CalleePopBytes is only a maximum and cannot prove agreement.
        const auto Byte = readImmutableCodeBytes(Image, Op.Addr, 1);
        if (!Byte || ((*Byte)[0] != 0xc3 && (*Byte)[0] != 0xc2))
          return std::nullopt;
        uint32_t Bytes = 0;
        if ((*Byte)[0] == 0xc2) {
          const auto Encoding = readImmutableCodeBytes(Image, Op.Addr, 3);
          if (!Encoding)
            return std::nullopt;
          Bytes = readLE<uint16_t>(Encoding->data() + 1);
        }
        if (Pop && *Pop != Bytes)
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
  return Pop;
}

bool isPE32(const BinaryImage &Image) {
  return Image.Arch == Arch::X86 && Image.Bits == Bitness::Bits32 &&
         Image.Format == BinaryFormat::COFF;
}

class ReturningStackProof {
  const BinaryImage &Image;
  size_t &Work;
  std::map<va_t, std::optional<uint32_t>> &Cache;
  StackBody *Parent;
  unsigned Depth = 0;

public:
  ReturningStackProof(const BinaryImage &Image, size_t &Work,
                      std::map<va_t, std::optional<uint32_t>> &Cache,
                      StackBody *Parent = nullptr)
      : Image(Image), Work(Work), Cache(Cache), Parent(Parent) {}

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
    // In-progress entries carry no fact: recursion cannot certify itself.
    // Even failed nested bodies consume the same cumulative work allowance.
    auto It = Cache.emplace(Target, std::nullopt).first;
    ++Depth;
    const auto Nested = [&](va_t Callee) { return prove(Callee); };
    if (Parent && Parent->Entries.count(Target)) {
      It->second = stackPopForBody(*Parent, Target, Nested);
    } else if (!Image.ExceptionMetadata.findFunction(Target)) {
      Decoder Dec;
      if (Dec.init(Image)) {
        CFGBuilder Builder;
        const LowFunc Callee = Builder.build(Image, Dec, Target, "abi-stack");
        if (!Callee.ExceptionMetadata) {
          StackBody Body(Image, Callee, Work);
          It->second = stackPopForBody(Body, Target, Nested);
        }
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
  size_t LocalWork = 0;
  size_t &Work = CumulativeWork ? *CumulativeWork : LocalWork;
  std::map<va_t, std::optional<uint32_t>> Cache;
  return ReturningStackProof(Image, Work, Cache).prove(Target);
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
  ReturningStackProof Proof(Image, Work, StackCache, &Parent);
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
