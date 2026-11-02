//===- COFFRegistrationEHCallBoundary.cpp - Checked PE32 call entries -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "../../ReadOnlyBytesIndex.h"
#include "COFFRegistrationEHDetail.h"

#include "neverd/loader/Limits.h"

#include "llvm/ADT/ScopeExit.h"

#include <capstone/capstone.h>
#include <deque>

namespace neverd::coff_loader::registration_detail {

std::set<va_t> checkedRegistrationCallBoundaries(
    const BinaryImage &Img, const std::set<va_t> &PhysicalEntries,
    const std::map<va_t, va_t> &Candidates, size_t &Work) {
  if (Img.Arch != Arch::X86 || Img.Bits != Bitness::Bits32 ||
      Img.Format != BinaryFormat::COFF || Img.IsRelocatable ||
      Candidates.empty())
    return {};
  bool Exhausted = false;
  auto Charge = [&](size_t Amount) {
    const size_t Limit = limits::kMaxRegistrationEHStateWork;
    if (Amount > Limit - std::min(Work, Limit)) {
      Work = Limit;
      Exhausted = true;
      return false;
    }
    Work += Amount;
    return true;
  };
  if (!Charge(PhysicalEntries.size() + Candidates.size() + 1))
    return {};
  std::vector<PhysicalRegistrationEntry> KnownEntries;
  for (va_t Entry : PhysicalEntries)
    KnownEntries.push_back({Entry, std::nullopt});
  const FunctionRangeMap Boundaries(Img, KnownEntries);
  auto Roots = PhysicalEntries;
  if (isExecutableAddress(Img, Img.Entry))
    Roots.insert(Img.Entry);
  csh Handle = 0;
  if (cs_open(CS_ARCH_X86, CS_MODE_32, &Handle) != CS_ERR_OK)
    return {};
  const auto Close = llvm::scope_exit([&] { cs_close(&Handle); });
  if (cs_option(Handle, CS_OPT_DETAIL, CS_OPT_ON) != CS_ERR_OK)
    return {};
  auto *Instruction = cs_malloc(Handle);
  if (!Instruction)
    return {};
  const auto Free = llvm::scope_exit([&] { cs_free(Instruction, 1); });

  struct Node {
    va_t End;
    std::optional<va_t> Previous;
    std::vector<va_t> AbsoluteOperands;
  };
  std::map<va_t, Node> Decoded;
  std::set<va_t> Queued;
  std::deque<std::pair<va_t, std::optional<va_t>>> Pending;
  std::map<va_t, std::vector<va_t>> Calls;
  auto Queue = [&](va_t Address, std::optional<va_t> Previous) {
    if (!Charge(1))
      return;
    if (Address <= UINT32_MAX && Queued.insert(Address).second)
      Pending.emplace_back(Address, Previous);
  };
  for (va_t Entry : Roots)
    Queue(Entry, std::nullopt);
  // Breadth-first traversal retains short instruction witnesses. It follows
  // only decoded control edges, never byte scans, guessed entries, indirect
  // targets or the deferred candidates themselves as roots.
  while (!Pending.empty() && !Exhausted) {
    const auto [Begin, Previous] = Pending.front();
    Pending.pop_front();
    if (!Charge(1))
      break;
    const auto *Segment = Img.getSegmentFor(Begin);
    if (!Segment || !Segment->isExecutable() || Begin < Segment->VA ||
        Begin - Segment->VA >= Segment->Data.size())
      continue;
    size_t Size =
        std::min<uint64_t>(15, Segment->Data.size() - (Begin - Segment->VA));
    const uint8_t *Cursor = Img.readVA(Begin, Size);
    va_t End = Begin;
    if (!Cursor || !cs_disasm_iter(Handle, &Cursor, &Size, &End, Instruction) ||
        !Instruction->detail || End > uint64_t(UINT32_MAX) + 1 ||
        !Img.hasExecutableCodeOwnerRange(Begin, Instruction->size) ||
        !Charge(Instruction->size))
      continue;
    const auto Next = Decoded.lower_bound(Begin);
    if ((Next != Decoded.end() && Next->first < End) ||
        (Next != Decoded.begin() && std::prev(Next)->second.End > Begin))
      return {}; // Ambiguous instruction ownership supplies no fallback.
    const bool Call = cs_insn_group(Handle, Instruction, CS_GRP_CALL);
    const bool Jump = cs_insn_group(Handle, Instruction, CS_GRP_JUMP);
    const auto &X86 = Instruction->detail->x86;
    const bool Direct = X86.op_count == 1 && X86.operands[0].type == X86_OP_IMM;
    Node N{End, Previous, {}};
    // Absolute operands, including an indirect CALL's IAT address, may carry
    // HIGHLOW relocations. Direct control displacements may not: those decoded
    // targets are part of the witness. Indirect targets are never followed.
    if (!Call && !Jump && X86.encoding.imm_size == 4)
      N.AbsoluteOperands.push_back(Begin + X86.encoding.imm_offset);
    if (X86.encoding.disp_size == 4 && (!(Call || Jump) || !Direct))
      N.AbsoluteOperands.push_back(Begin + X86.encoding.disp_offset);
    Decoded.emplace(Begin, std::move(N));
    if (cs_insn_group(Handle, Instruction, CS_GRP_RET) ||
        cs_insn_group(Handle, Instruction, CS_GRP_IRET) ||
        cs_insn_group(Handle, Instruction, CS_GRP_INT) ||
        Instruction->id == X86_INS_UD0 || Instruction->id == X86_INS_UD1 ||
        Instruction->id == X86_INS_UD2 || Instruction->id == X86_INS_HLT ||
        Instruction->id == X86_INS_LCALL || Instruction->id == X86_INS_LJMP)
      continue;
    if ((Call || Jump) && Direct) {
      const va_t Target = uint32_t(X86.operands[0].imm);
      if (Call && Instruction->size == 5 && Instruction->bytes[0] == 0xe8 &&
          Candidates.count(Target))
        Calls[Target].push_back(Begin);
      Queue(Target, Begin);
    }
    if (Jump && (!Direct || Instruction->id == X86_INS_JMP))
      continue;
    // An ordinary call has a structural continuation. This establishes no
    // successful-return, register-preservation or stack-effect contract.
    Queue(End, Begin);
  }
  if (Exhausted)
    return {};

  // Recheck only the actual witness paths, coalescing adjacent instructions
  // before the shared immutable-storage/fixup query. Unrelated traversed code
  // supplies neither byte-copy nor instruction-ownership permission.
  IndexedPE32CodeBytes CodeBytes(Img, Work,
                                 limits::kMaxRegistrationEHStateWork);
  std::map<va_t, bool> CheckedPaths;
  auto CheckPath = [&](va_t Address) {
    const va_t Source = Address;
    std::vector<va_t> Path;
    for (;;) {
      if (!Charge(1))
        return false;
      if (const auto Checked = CheckedPaths.find(Address);
          Checked != CheckedPaths.end()) {
        if (!Checked->second)
          return false;
        break;
      }
      const auto It = Decoded.find(Address);
      if (It == Decoded.end())
        return false;
      Path.push_back(Address);
      if (Roots.count(Address))
        break;
      if (!It->second.Previous)
        return false;
      Address = *It->second.Previous;
    }
    std::sort(Path.begin(), Path.end());
    for (size_t I = 0; I < Path.size();) {
      const va_t Begin = Path[I];
      va_t End = Begin;
      std::vector<va_t> Operands;
      do {
        const auto &N = Decoded.at(Path[I]);
        End = N.End;
        Operands.insert(Operands.end(), N.AbsoluteOperands.begin(),
                        N.AbsoluteOperands.end());
        ++I;
      } while (I < Path.size() && Path[I] == End &&
               Decoded.at(Path[I]).End - Begin <=
                   ::neverd::limits::loader::kMaxImageReadBytes);
      if (!CodeBytes.read(Begin, uint32_t(End - Begin), Operands)) {
        CheckedPaths[Source] = false;
        return false;
      }
    }
    for (va_t Node : Path)
      CheckedPaths[Node] = true;
    return true;
  };
  std::set<va_t> Result;
  for (const auto &[Entry, Guess] : Candidates) {
    if (!Charge(1))
      break;
    auto Interior = Decoded.upper_bound(Guess);
    const auto Sources = Calls.find(Entry);
    if (Sources == Calls.end() || !Decoded.count(Entry))
      continue;
    bool Explained = false;
    if (Interior != Decoded.begin()) {
      --Interior;
      Explained = Interior->first < Guess && Interior->second.End > Guess &&
                  CheckPath(Interior->first);
    }
    if (!Explained && Decoded.count(Guess)) {
      // An exact loop instruction need not decode as a closed body when
      // falsely treated as its own entry: its backedge can precede the guess.
      // Start only at an earlier independently accepted physical entry and
      // require the complete immutable body to close before the candidate.
      const auto Previous = PhysicalEntries.upper_bound(Guess);
      if (Previous != PhysicalEntries.begin()) {
        const va_t Owner = *std::prev(Previous);
        Explained =
            Owner < Guess && !Boundaries.hasIndependentEntry(Owner, Entry) &&
            checkedRegistrationBodyOwnsInstruction(Img, {Owner, Entry}, Owner,
                                                   Guess, Work, CodeBytes);
      }
    }
    if (!Explained)
      continue;
    for (va_t Source : Sources->second)
      if (CheckPath(Source)) {
        Result.insert(Entry);
        break;
      }
  }
  // The shared traversal completed without conflicting decoded boundaries.
  // Each boundary needs its own complete witnesses; exhausting a
  // later query cannot revoke an earlier independently checked boundary.
  return Result;
}

} // namespace neverd::coff_loader::registration_detail
