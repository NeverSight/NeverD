//===- RegistrationOpaqueStack.cpp - Call-independent stack restoration --===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Keep explicit register restoration independent of unused IAT assumptions.
//===----------------------------------------------------------------------===//

#include "RegistrationOpaqueStack.h"

#include "RegistrationABIPrivate.h"
#include "RegistrationFrame.h"
#include "RegistrationFrameEffects.h"

#include "neverd/decode/Decoder.h"
#include "neverd/ir/low/LowIR.h"
#include "neverd/lift/X86/X86Regs.h"
#include "neverd/loader/BinaryImage.h"

#include "llvm/Support/Error.h"

#include <deque>
#include <map>
#include <set>
#include <utility>

namespace neverd::registration_abi {

std::optional<uint32_t> projectOpaqueCallReturnStack(const BinaryImage &Image,
                                                     const LowFunc &Function,
                                                     size_t &Work) {
  using namespace registration_state;
  if (Image.Arch != Arch::X86 || Image.Bits != Bitness::Bits32 ||
      Image.Format != BinaryFormat::COFF || Function.ExceptionMetadata ||
      !Function.hasCompleteLiftCoverage() ||
      !isPlainRegistrationCalleeEntry(Image, Function.Entry))
    return std::nullopt;

  // Admission only: the complete transfer below still proves the anchor.
  // Avoid a second dataflow traversal for leaves and ordinary call chains.
  bool HasCall = false, HasRestoration = false;
  for (const auto &Block : Function.Blocks) {
    if (!chargeCalleeWork(Work, Block.Ops.size() + 1))
      return std::nullopt;
    for (const auto &Op : Block.Ops) {
      HasCall |= Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL;
      HasRestoration |=
          Op.Opcode == NdOp::COPY && Op.Output == NdVar::reg(x86reg::RSP, 4) &&
          Op.NumInputs == 1 && Op.Inputs[0] != NdVar::reg(x86reg::RSP, 4);
    }
  }
  if (!HasCall || !HasRestoration)
    return std::nullopt;
  if (auto Error = validateLowInstructionBoundaries(
          Function, LowInstructionBoundaryRequirement::Required)) {
    llvm::consumeError(std::move(Error));
    return std::nullopt;
  }
  Decoder Dec;
  if (!Dec.init(Image))
    return std::nullopt;
  std::map<int, const LowBlock *> Blocks;
  const LowBlock *Entry = nullptr;
  for (const auto &Block : Function.Blocks) {
    if (!chargeCalleeWork(Work, 1) || !Blocks.emplace(Block.Id, &Block).second)
      return std::nullopt;
    if (Block.StartAddr == Function.Entry) {
      if (Entry)
        return std::nullopt;
      Entry = &Block;
    }
  }
  if (!Entry)
    return std::nullopt;

  FrameState Initial;
  Initial.ImportBindingsIntact = false;
  Initial.Registers[x86reg::RSP / x86reg::GeneralRegStride] =
      FrameValue::frame(0);
  std::map<int, FrameState> Incoming{{Entry->Id, Initial}};
  std::deque<int> Pending{Entry->Id};
  std::set<int> Queued{Entry->Id}, Checked;
  std::optional<uint32_t> Pop;
  while (!Pending.empty()) {
    const int Id = Pending.front();
    Pending.pop_front();
    Queued.erase(Id);
    const auto &Block = *Blocks.at(Id);
    if (Checked.insert(Id).second)
      for (const auto &Boundary : Block.InstructionBoundaries)
        if (!chargeCalleeWork(Work, Boundary.Size + 1) ||
            !hasImmutableCalleeInstruction(Image, Boundary, Dec))
          return std::nullopt;
    FrameState State = Incoming.at(Id);
    FrameTransfer Transfer(State, 0);
    for (const auto &Op : Block.Ops) {
      size_t Width = Op.Output.Size;
      for (unsigned I = 0; I != Op.NumInputs; ++I)
        Width += Op.Inputs[I].Size;
      if (!chargeCalleeWork(Work, Width + 1) || Op.Opcode == NdOp::INDIR_BR ||
          (Op.Opcode == NdOp::INTRINSIC && !isFramePreservingIntrinsic(Op)))
        return std::nullopt;
      Transfer.beginInstruction(Op.Addr);
      if (Op.Opcode == NdOp::RETURN) {
        const auto Current = checkedNearReturnStackPop(Image, Op.Addr);
        if (!Current || (Pop && *Pop != *Current) ||
            State.Registers[x86reg::RSP / x86reg::GeneralRegStride].Offset !=
                0 ||
            &Op != &Block.Ops.back() || !Block.Succs.empty())
          return std::nullopt;
        Pop = Current;
      }
      // The shared frame authority forgets ESP and volatile lanes at every
      // call. No import load, nested summary, branch result or memory fact is
      // supplied here. Only subsequent register instructions can recover ESP.
      Transfer.write(Op, Transfer.evaluate(Op, false));
    }
    if (Block.Ops.empty() ||
        (Block.Succs.empty() && Block.Ops.back().Opcode != NdOp::RETURN))
      return std::nullopt;
    for (int Successor : Block.Succs) {
      if (!chargeCalleeWork(Work, State.cellCount() + 1) ||
          !Blocks.count(Successor))
        return std::nullopt;
      auto [It, New] = Incoming.emplace(Successor, State);
      if ((New || It->second.merge(State)) && Queued.insert(Successor).second)
        Pending.push_back(Successor);
    }
  }
  return Pop;
}

} // namespace neverd::registration_abi
