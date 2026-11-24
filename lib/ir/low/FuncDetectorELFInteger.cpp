//===- FuncDetectorELFInteger.cpp - RISC-V and PowerPC direct calls ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Walk physical instruction boundaries. Stop a sweep at an undecodable prefix
/// so its payload cannot supply a spurious call.
///
//===----------------------------------------------------------------------===//

#include "FuncDetectorDetail.h"

#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/lift/RISCV/RISCVLifter.h"

#include "llvm/Support/Endian.h"

#include <iterator>
#include <map>

namespace neverd::func_detect_detail {
void scanELFIntegerCalls(const BinaryImage &Img, Decoder &Dec,
                         const Segment *Seg, va_t Start, va_t End,
                         std::set<va_t> &Out,
                         std::vector<std::pair<va_t, va_t>> &CodeAddresses,
                         const Cancellation *Control) {
  std::optional<uint32_t> PreviousWord;
  for (va_t At = Start; At < End;) {
    checkCancellation(Control);
    DecodedInsn Insn;
    const int Size = Dec.decodeOneLight(Seg->Data.data() + (At - Seg->VA),
                                        End - At, At, Insn);
    if (Size <= 0)
      break;
    va_t Target = Dec.directCallTarget(Insn);
    if (Target != InvalidVA && Img.hasExecutableCodeOwnerRange(At, Size) &&
        Img.isFunctionEntryCandidateAt(Target))
      Out.insert(Target);
    if (isRISCV(Img.Arch)) {
      if (PreviousWord) {
        const auto Callee = riscv::pcRelativeCall(Img.Arch, Img.RISCV,
                                                  *PreviousWord, *Insn.Raw);
        if (Callee && Img.isFunctionEntryCandidateAt(*Callee) &&
            Img.hasExecutableCodeOwnerRange(At - 4, 4 + Size) &&
            !Img.hasUnappliedCodeRelocation(At - 4, 4 + Size))
          CodeAddresses.emplace_back(At, *Callee);
      }
      PreviousWord =
          Size == 4
              ? std::optional(llvm::support::endian::read32le(Insn.Raw->bytes))
              : std::nullopt;
    }
    At += Size;
  }
}

void recoverRISCVCallEntries(const BinaryImage &Img, Decoder &Dec,
                             llvm::ArrayRef<std::pair<va_t, va_t>> Pairs,
                             std::set<va_t> &Entries, std::set<va_t> &Calls,
                             const Cancellation *Control) {
  // A byte sweep identifies possible pairs. Admit a new callee only when the
  // reachable caller puts the AUIPC definition and JALR in the same block.
  std::map<va_t, va_t> Pending;
  std::set<va_t> Callers;
  for (const auto &[Site, Target] : Pairs) {
    if (Entries.contains(Target))
      continue;
    Pending.emplace(Site, Target);
    auto Next = Entries.upper_bound(Site);
    if (Next != Entries.begin())
      Callers.insert(*std::prev(Next));
  }
  CFGBuilder Builder;
  Builder.setKnownFuncEntries(&Entries);
  while (!Callers.empty() && !Pending.empty()) {
    checkCancellation(Control);
    const va_t Caller = *Callers.begin();
    Callers.erase(Callers.begin());
    const auto Body =
        Builder.build(Img, Dec, Caller, Img.getFunctionNameAt(Caller));
    for (const auto &Block : Body.Blocks) {
      std::optional<va_t> Upper;
      for (const auto &Op : Block.Ops) {
        if (Op.Opcode == NdOp::COPY && Op.NumInputs == 1 &&
            Op.Inputs[0].Provenance ==
                ConstantAddressProvenance::AddressFragment)
          Upper = Op.Addr;
        if (Op.Opcode != NdOp::INDIR_CALL || !Upper || Op.Addr - *Upper != 4)
          continue;
        const auto Found = Pending.find(Op.Addr);
        if (Found == Pending.end())
          continue;
        const va_t Target = Found->second;
        Pending.erase(Found);
        const auto [Root, Added] = Entries.insert(Target);
        if (!Added)
          continue;
        Calls.insert(Target);
        // A newly found callee may contain a second unrelaxed call.
        const auto Site = Pending.lower_bound(Target);
        const auto Next = std::next(Root);
        if (Site != Pending.end() &&
            (Next == Entries.end() || Site->first < *Next))
          Callers.insert(Target);
      }
    }
  }
}
} // namespace neverd::func_detect_detail
