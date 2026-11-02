//===- RegistrationStateOccurrences.cpp - x86 EH source occurrences -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "../LowInstructionSlice.h"
#include "RegistrationStateSolver.h"

#include "neverd/Limits.h"
#include "neverd/lift/X86/X86Regs.h"

#include <algorithm>
#include <utility>

namespace neverd::registration_state {

void RegistrationStateSolver::collectOccurrences() {
  for (const LowBlock &Block : Function.Blocks) {
    for (const LowInstructionBoundary &Boundary : Block.InstructionBoundaries) {
      if (!charge(1))
        break;
      if (Boundary.Size == 0 || Boundary.Address < Block.StartAddr ||
          Boundary.Address >= Block.EndAddr ||
          Boundary.Size > Block.EndAddr - Boundary.Address ||
          !Boundaries
               .emplace(Boundary.Address, std::make_pair(Block.Id, Boundary))
               .second)
        CompleteChainOperations = false;
      // A supplied operation slice remains evidence even if a corrupted
      // address would clear instruction-local frame facts before its store.
      // Address-only legacy observations do not claim operation ownership.
      if (Boundary.OpCount) {
        if (!charge(Boundary.OpCount))
          break;
        if (!detail::checkedInstructionOperations(Block, Boundary)) {
          InvalidBlocks[Index.at(Block.Id)] = true;
          CompleteChainOperations = false;
        }
      }
    }
    for (const LowOp &Op : Block.Ops) {
      if (!charge(1))
        break;
      const auto Memory = lowMemoryOperands(Op);
      if (CookieCheckVA && Op.Opcode == NdOp::CALL && Op.NumInputs == 1 &&
          Op.Inputs[0].isConst() && Op.Inputs[0].Offset == CookieCheckVA &&
          !CookieCheckOccurrences.emplace(Op.Addr, Op.Seq).second)
        CompleteCookies = false;
      if (Op.MemoryAddressSpace == NdMemoryAddressSpace::X86FS) {
        if (!Memory.Complete ||
            (Op.Opcode != NdOp::LOAD && Op.Opcode != NdOp::STORE) ||
            Op.Seq < 0 || !ChainOccurrences.emplace(Op.Addr, Op.Seq).second)
          CompleteChainOperations = false;
      }
    }
    if (Exhausted)
      break;
  }
}

void RegistrationStateSolver::recordChainAccess(
    const LowOp &Op, const LowBlock &Block,
    RegistrationChainAccess::Kind Kind) {
  if (!charge(1))
    return;
  auto Boundary = Boundaries.find(Op.Addr);
  if (Op.Seq < 0 || Boundary == Boundaries.end() ||
      Boundary->second.first != Block.Id) {
    CompleteChainOperations = false;
    return;
  }
  const RegistrationChainAccess Access{
      Op.Addr, Boundary->second.second.Address + Boundary->second.second.Size,
      Op.Seq, Kind};
  auto [It, Inserted] =
      ChainAccesses.emplace(std::make_pair(Op.Addr, Op.Seq), Access);
  if (!Inserted && (It->second.AccessKind != Kind ||
                    It->second.EndAddress != Access.EndAddress))
    CompleteChainOperations = false;
}

} // namespace neverd::registration_state
