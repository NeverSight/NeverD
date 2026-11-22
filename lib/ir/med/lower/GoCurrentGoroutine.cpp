//===- GoCurrentGoroutine.cpp - The goroutine Go code runs on ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Read the goroutine and closure context registers of Go's internal ABI as
/// the values they hold.
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
/// The goroutine and the closure context are pointers.
constexpr uint16_t GoroutineBytes = 8;

MedOp goroutineRead(Intrinsic Id, const MedVar &Output, va_t Addr) {
  MedOp Read;
  Read.Opcode = NdOp::INTRINSIC;
  Read.Addr = Addr;
  Read.Output = Output;
  Read.addInput(MedVar::makeConst(static_cast<uint64_t>(Id), 2));
  return Read;
}

/// Give the reads of \p Register in \p Func the value \p Id reads: the
/// incoming value, and where \p AfterCalls, each call's clobber of it.
void defineGoRegisterValue(MedFunc &Func, uint64_t Register, Intrinsic Id,
                           bool AfterCalls) {
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
  const int RegisterId = Any->Id;
  // The incoming value is the version nothing in the function defines:
  // version 0, unless an operation, a PHI or a call's clobber defines it, as
  // the first call's clobber does where the function reads the register only
  // after that call.
  const auto DefinesVersionZero = [&](const MedVar &V) {
    return V.Kind == MedVar::Reg && V.Id == RegisterId && V.SSAVer == 0;
  };
  bool VersionZeroDefined =
      llvm::any_of(Func.CallClobbers, [&](const MedCallClobber &Clobber) {
        return DefinesVersionZero(Clobber.Value);
      });
  for (const MedBlock &Block : Func.Blocks) {
    if (VersionZeroDefined)
      break;
    VersionZeroDefined = llvm::any_of(Block.Phis,
                                      [&](const PhiNode &Phi) {
                                        return DefinesVersionZero(Phi.Output);
                                      }) ||
                         llvm::any_of(Block.Ops, [&](const MedOp &Op) {
                           return DefinesVersionZero(Op.Output);
                         });
  }
  // The incoming value: the read takes the copy's place, or defines it where
  // no copy names it, and every other read of it reads the read's result,
  // which the read at the entry dominates.
  const bool CopyRead = CopyAt && Read.count(KeyOf(Entry.Ops[*CopyAt].Output));
  const bool IncomingRead =
      !VersionZeroDefined && Read.count({RegisterId, 0}) != 0;
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
                     goroutineRead(Id, Defined, Entry.StartAddr));
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
  if (!AfterCalls)
    return;
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
        Ops.push_back(goroutineRead(Id, Clobbered.at(Site), Addr));
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
} // namespace

bool addressesThroughEntryRegister(const MedFunc &Func, uint64_t Register) {
  if (Func.Blocks.empty())
    return false;
  using Key = std::pair<int, int>;
  const auto KeyOf = [](const MedVar &V) { return Key{V.Id, V.SSAVer}; };
  const auto Incoming = [&](const MedVar &V) {
    return V.Kind == MedVar::Reg && V.RegOff == Register &&
           V.Size == GoroutineBytes && V.Id >= 0 && V.SSAVer == 0;
  };
  // The incoming value is the version nothing but an entry self-copy
  // defines, and the values a copy or a constant offset makes of it.
  std::set<Key> Derived;
  for (const MedBlock &Block : Func.Blocks) {
    for (const PhiNode &Phi : Block.Phis)
      if (Incoming(Phi.Output))
        return false;
    for (const MedOp &Op : Block.Ops)
      if (Incoming(Op.Output) &&
          !(Op.Opcode == NdOp::COPY && Op.NumInputs == 1 &&
            Op.Inputs[0].Kind == MedVar::Reg &&
            Op.Inputs[0].Id == Op.Output.Id && Op.Inputs[0].SSAVer == 0))
        return false;
  }
  if (llvm::any_of(Func.CallClobbers, [&](const MedCallClobber &Clobber) {
        return Incoming(Clobber.Value);
      }))
    return false;
  const auto Through = [&](const MedVar &V) {
    return !V.isConst() && V.Id >= 0 &&
           (Incoming(V) || Derived.count(KeyOf(V)));
  };
  // Operations are in no dominance order here; a few rounds reach every
  // offset of an offset.
  for (unsigned Round = 0, Grew = 1; Grew && Round < 4; ++Round) {
    Grew = 0;
    for (const MedBlock &Block : Func.Blocks)
      for (const MedOp &Op : Block.Ops) {
        if (Op.Output.Id < 0 || Derived.count(KeyOf(Op.Output)) ||
            Incoming(Op.Output))
          continue;
        const bool Copy = Op.Opcode == NdOp::COPY && Op.NumInputs == 1 &&
                          Through(Op.Inputs[0]);
        const bool Offset =
            (Op.Opcode == NdOp::INT_ADD || Op.Opcode == NdOp::INT_SUB) &&
            Op.NumInputs == 2 && Through(Op.Inputs[0]) &&
            Op.Inputs[1].isConst();
        if (Copy || Offset) {
          Derived.insert(KeyOf(Op.Output));
          Grew = 1;
        }
      }
  }
  for (const MedBlock &Block : Func.Blocks)
    for (const MedOp &Op : Block.Ops)
      if ((Op.Opcode == NdOp::LOAD || Op.Opcode == NdOp::STORE) &&
          Op.NumInputs >= 1 && Through(Op.Inputs[0]))
        return true;
  return false;
}

void defineGoCurrentGoroutine(MedFunc &Func, uint64_t Register) {
  defineGoRegisterValue(Func, Register, Intrinsic::GoCurrentGoroutine,
                        /*AfterCalls=*/true);
}

void defineGoClosureContext(MedFunc &Func, uint64_t Register) {
  defineGoRegisterValue(Func, Register, Intrinsic::GoClosureContext,
                        /*AfterCalls=*/false);
}
} // namespace neverd
