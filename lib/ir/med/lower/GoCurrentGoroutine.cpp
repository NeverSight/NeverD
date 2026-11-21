//===- GoCurrentGoroutine.cpp - The goroutine Go code runs on ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Read the goroutine register of Go's internal ABI as the goroutine it holds.
///
//===----------------------------------------------------------------------===//

#include "GoCurrentGoroutine.h"

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/ir/med/MedIR.h"

#include "llvm/ADT/STLExtras.h"

#include <map>
#include <optional>
#include <set>
#include <utility>
#include <vector>

namespace neverd {
namespace {
/// The goroutine is a pointer.
constexpr uint16_t GoroutineBytes = 8;

MedOp goroutineRead(const MedVar &Output, va_t Addr) {
  MedOp Read;
  Read.Opcode = NdOp::INTRINSIC;
  Read.Addr = Addr;
  Read.Output = Output;
  Read.addInput(MedVar::makeConst(
      static_cast<uint64_t>(Intrinsic::GoCurrentGoroutine), 2));
  return Read;
}
} // namespace

void defineGoCurrentGoroutine(MedFunc &Func, uint64_t Register) {
  if (Func.Blocks.empty())
    return;
  const auto Goroutine = [&](const MedVar &V) {
    return V.Kind == MedVar::Reg && V.RegOff == Register &&
           V.Size == GoroutineBytes && V.Id >= 0;
  };
  using Key = std::pair<int, int>;
  const auto KeyOf = [](const MedVar &V) { return Key{V.Id, V.SSAVer}; };
  // The entry's leading copies name the registers the function takes
  // (findLiveInParamRegs), so a read at the entry follows them all.  One of
  // them may copy the incoming value.
  MedBlock &Entry = Func.Blocks.front();
  size_t Copies = 0;
  while (Copies < Entry.Ops.size() && Entry.Ops[Copies].Opcode == NdOp::COPY)
    ++Copies;
  std::optional<size_t> CopyAt;
  for (size_t I = 0; I < Copies && !CopyAt; ++I) {
    const MedOp &Op = Entry.Ops[I];
    if (Goroutine(Op.Output) && Op.NumInputs == 1 &&
        Op.Inputs[0].Kind == MedVar::Reg && Op.Inputs[0].Id == Op.Output.Id &&
        Op.Inputs[0].SSAVer == 0)
      CopyAt = I;
  }
  // The versions of the register something reads but that copy.  A read
  // only dead code makes, or only a PHI no operation reads, becomes a read
  // dead code elimination drops later (intrinsicReadIsDiscardable): which
  // values a return or a call reads, arm64 binds only after this.
  std::set<Key> Read;
  std::optional<MedVar> Any;
  for (const MedBlock &Block : Func.Blocks) {
    for (const PhiNode &Phi : Block.Phis)
      for (const auto &[Pred, Arg] : Phi.Args)
        if (Goroutine(Arg)) {
          Read.insert(KeyOf(Arg));
          Any = Arg;
        }
    for (size_t I = 0; I < Block.Ops.size(); ++I) {
      if (&Block == &Entry && CopyAt && I == *CopyAt)
        continue;
      const MedOp &Op = Block.Ops[I];
      for (unsigned In = 0; In < Op.NumInputs; ++In)
        if (Goroutine(Op.Inputs[In])) {
          Read.insert(KeyOf(Op.Inputs[In]));
          Any = Op.Inputs[In];
        }
    }
  }
  if (!Any)
    return;
  const int Id = Any->Id;
  // The incoming value: the read takes the copy's place, or defines it where
  // no copy names it, and every other read of it reads the read's result,
  // which the read at the entry dominates.
  const bool CopyRead = CopyAt && Read.count(KeyOf(Entry.Ops[*CopyAt].Output));
  const bool IncomingRead = Read.count({Id, 0}) != 0;
  if (CopyRead || IncomingRead) {
    MedVar Defined = *Any;
    Defined.SSAVer = 0;
    if (CopyAt) {
      Defined = Entry.Ops[*CopyAt].Output;
      Entry.Ops.erase(Entry.Ops.begin() + *CopyAt);
      --Copies;
    }
    // Another of the entry's copies that reads the incoming value follows
    // the read, which defines it: a copy has no effect to keep in place.
    std::vector<MedOp> Readers;
    for (size_t I = 0; I < Copies;) {
      const MedOp &Op = Entry.Ops[I];
      if (Op.NumInputs == 1 && Goroutine(Op.Inputs[0]) &&
          Op.Inputs[0].SSAVer == 0) {
        Readers.push_back(Op);
        Entry.Ops.erase(Entry.Ops.begin() + I);
        --Copies;
      } else {
        ++I;
      }
    }
    Entry.Ops.insert(Entry.Ops.begin() + Copies,
                     goroutineRead(Defined, Entry.StartAddr));
    Entry.Ops.insert(Entry.Ops.begin() + Copies + 1, Readers.begin(),
                     Readers.end());
    if (Defined.SSAVer != 0) {
      const auto Rename = [&](MedVar &V) {
        if (Goroutine(V) && V.SSAVer == 0)
          V.SSAVer = Defined.SSAVer;
      };
      for (MedBlock &Block : Func.Blocks) {
        for (PhiNode &Phi : Block.Phis)
          for (auto &[Pred, Arg] : Phi.Args)
            Rename(Arg);
        for (MedOp &Op : Block.Ops)
          for (unsigned In = 0; In < Op.NumInputs; ++In)
            Rename(Op.Inputs[In]);
      }
    }
  }
  // Each call's clobber of the register that something reads: a read just
  // after the call defines it.
  std::map<uint32_t, MedVar> Clobbered;
  for (const MedCallClobber &Clobber : Func.CallClobbers)
    if (Goroutine(Clobber.Value) && Clobber.PreservedPrefixSize == 0 &&
        Read.count(KeyOf(Clobber.Value)))
      Clobbered.emplace(Clobber.CallSiteId, Clobber.Value);
  if (Clobbered.empty())
    return;
  std::set<uint32_t> Placed;
  for (MedBlock &Block : Func.Blocks) {
    const auto Clobbers = [&](const MedOp &Op) {
      return (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL) &&
             Clobbered.count(Op.CallSiteId) && !Placed.count(Op.CallSiteId);
    };
    if (llvm::none_of(Block.Ops, Clobbers))
      continue;
    std::vector<MedOp> Ops;
    Ops.reserve(Block.Ops.size() + Clobbered.size());
    for (MedOp &Op : Block.Ops) {
      const bool Reads = Clobbers(Op);
      const uint32_t Site = Op.CallSiteId;
      const va_t Addr = Op.Addr;
      Ops.push_back(std::move(Op));
      if (Reads && Placed.insert(Site).second)
        Ops.push_back(goroutineRead(Clobbered.at(Site), Addr));
    }
    Block.Ops = std::move(Ops);
  }
  std::vector<MedCallClobber> Kept;
  for (MedCallClobber &Clobber : Func.CallClobbers) {
    const auto It = Clobbered.find(Clobber.CallSiteId);
    if (!(Goroutine(Clobber.Value) && It != Clobbered.end() &&
          Placed.count(Clobber.CallSiteId) &&
          KeyOf(It->second) == KeyOf(Clobber.Value)))
      Kept.push_back(std::move(Clobber));
  }
  Func.CallClobbers = std::move(Kept);
}
} // namespace neverd
