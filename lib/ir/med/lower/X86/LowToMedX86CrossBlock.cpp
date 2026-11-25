//===- LowToMedX86CrossBlock.cpp - x86 sub-register fixups for LowIR→MedIR
//-------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// x86/x86-64-specific sub-register repair passes for the LowIR→MedIR
/// conversion.  The architecture-generic framework lives in
/// LowToMedSubReg.cpp and dispatches to the functions here by target arch
/// (LLVM target-dispatch pattern).
///
//===----------------------------------------------------------------------===//

#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/support/GraphCycles.h"

#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <tuple>
#include <vector>

namespace neverd {

// Phase B2x: cross-block preserving partial write → wide parent definition.
//
// Phase B2 (above) only merges a preserving partial write into a wider read in
// the *same* block.  Examples are a 16-bit induction-variable seed whose GPR
// parent is consumed in a successor, and a legacy XMM write whose YMM parent is
// stored after a branch.  Without an eager parent definition, buildSsa carries
// the stale entry value across that edge.
//
// Mirror Phase A's eager-zext model for the partial-write case: right after
// such a write, define the wide parent as `(parent & ~mask) | (zext(narrow) <<
// shift)` so buildSsa propagates the merged value across the block boundary.
// Gated to partial writes whose wide parent is actually read in a different
// block, so the straight-line and intra-block cases (already covered by Phase
// B2) are untouched.
void LowToMedConverter::mergePartialWritesCrossBlockX86(MedFunc &Func) {
  if (TargetArch != Arch::X64 && TargetArch != Arch::X86)
    return;

  const auto &TRI = getTargetRegInfo(TargetArch);

  auto isPreservingPartialWrite = [&](uint64_t Off, uint16_t Sz) {
    if (Sz == 0 || TRI.writeZeroExtends(Off, Sz))
      return false;
    return TRI.findWideReg(Off, Sz).second > Sz;
  };

  // Every register read region with the block it occurs in.  buildSsa has not
  // run yet, so there are no phis; plain operand reads carry the cross-block
  // use.
  struct ReadRegion {
    uint64_t Off;
    uint16_t Sz;
    size_t Block;
  };
  std::vector<ReadRegion> Reads;
  for (size_t BI = 0; BI < Func.Blocks.size(); ++BI)
    for (const auto &MOp : Func.Blocks[BI].Ops)
      for (uint8_t I = 0; I < MOp.NumInputs; ++I) {
        const MedVar &Inp = MOp.Inputs[I];
        if (Inp.Kind == MedVar::Reg && Inp.Size > 0)
          Reads.push_back({Inp.RegOff, Inp.Size, BI});
      }

  // A loop-local read can use the narrow loop PHI. A read after the loop
  // may see the parent from any latch, including one that wrote only a byte.
  size_t N = Func.Blocks.size();
  std::map<int, size_t> IdToIdx;
  for (size_t I = 0; I < N; ++I)
    IdToIdx[Func.Blocks[I].Id] = I;
  std::vector<std::vector<size_t>> SuccIdx(N);
  for (size_t I = 0; I < N; ++I)
    for (int Su : Func.Blocks[I].Succs)
      if (auto It = IdToIdx.find(Su); It != IdToIdx.end())
        SuccIdx[I].push_back(It->second);
  const std::vector<bool> InLoop = nodesOnCycles(SuccIdx);

  // Wide-parent regions that are loop-carried via preserving partial writes
  // inside a loop (e.g. BL/BH, or a legacy XMM write into YMM). For those the
  // parent is reconstructed from loop-carried sub-register phis by Phase B2 /
  // mergeLoopCarriedPartialReads; an eager seed definition would make the
  // parent a wide loop phi and disable that machinery, so the seed merge is
  // ceded.
  std::set<uint64_t> LoopNarrowCarried;
  for (size_t BI = 0; BI < N; ++BI) {
    if (!InLoop[BI])
      continue;
    for (const auto &MOp : Func.Blocks[BI].Ops) {
      if (MOp.Output.Kind != MedVar::Reg ||
          !isPreservingPartialWrite(MOp.Output.RegOff, MOp.Output.Size))
        continue;
      if (MOp.Opcode == NdOp::SUBBYTES && MOp.NumInputs >= 1) {
        const MedVar &In0 = MOp.Inputs[0];
        if (In0.Kind == MedVar::Reg && In0.RegOff <= MOp.Output.RegOff &&
            In0.RegOff + In0.Size >= MOp.Output.RegOff + MOp.Output.Size)
          continue;
      }
      LoopNarrowCarried.insert(
          TRI.findWideReg(MOp.Output.RegOff, MOp.Output.Size).first);
    }
  }

  // Wide-parent regions that get a genuine whole-parent definition (a full
  // register write or an architecturally zero-extending sub-register write)
  // inside a loop. Such a parent forms its
  // own wide loop phi (clang uses the full register as scratch on some arms — a
  // `mov`/`lea`/`call` result), so every loop arm that updates only its low
  // byte
  // (`subb %al` / `incb %al`) must ALSO merge that byte back into the wide
  // parent; otherwise the wide phi carries a stale low byte on the unmerged
  // arms and races the narrow phi (OptStress30 swcallacc/swnested, OptStress31
  // crc16).  Keyed by the widest-register offset so AL/AX/EAX writes all map to
  // the same RAX-class key the partial-write loop below looks up.
  std::set<uint64_t> WideFullWrittenInLoop;
  for (size_t BI = 0; BI < N; ++BI) {
    if (!InLoop[BI])
      continue;
    for (const auto &MOp : Func.Blocks[BI].Ops) {
      if (MOp.Output.Kind != MedVar::Reg || MOp.Output.Size == 0)
        continue;
      auto [WideOff, WideSz] =
          TRI.findWideReg(MOp.Output.RegOff, MOp.Output.Size);
      if (MOp.Output.Size >= WideSz ||
          TRI.writeZeroExtends(MOp.Output.RegOff, MOp.Output.Size))
        WideFullWrittenInLoop.insert(WideOff);
    }
  }

  // True when a strictly wider read fully containing the partial write's bytes
  // exists in a different block (so it observes the merged parent, not the
  // narrow write directly).  Per region, the first two distinct blocks with
  // such a read decide that for every block.
  struct WideReadBlocks {
    std::optional<size_t> First, Second;
  };
  std::map<std::tuple<uint64_t, uint16_t, uint64_t, uint16_t>, WideReadBlocks>
      WideReadsByRegion;
  auto wideParentReadElsewhere = [&](uint64_t POff, uint16_t PSz, uint64_t WOff,
                                     uint16_t WSz, size_t Block) -> bool {
    auto [It, Inserted] = WideReadsByRegion.try_emplace({POff, PSz, WOff, WSz});
    WideReadBlocks &Found = It->second;
    if (Inserted)
      for (const auto &R : Reads) {
        if (R.Sz <= PSz)
          continue;
        if (R.Off <= POff && R.Off + R.Sz >= POff + PSz && R.Off >= WOff &&
            R.Off + R.Sz <= WOff + WSz) {
          if (!Found.First) {
            Found.First = R.Block;
          } else if (R.Block != *Found.First) {
            Found.Second = R.Block;
            break;
          }
        }
      }
    return (Found.First && *Found.First != Block) || Found.Second;
  };

  // A narrow PHI in a loop header only repairs local reads. If another
  // block observes the register after leaving the loop, carry the updated
  // parent on the writing edge. This also keeps a separately joined byte
  // consistent with its initialized parent when the exit reads only the byte.
  using Region = std::pair<uint64_t, uint16_t>;
  std::map<Region, std::vector<bool>> ReachesExitRead;
  auto readAfterLoop = [&](uint64_t Off, uint16_t Size, uint64_t Parent,
                           uint16_t Width, size_t Writer) {
    auto [It, Added] = ReachesExitRead.try_emplace({Off, Size});
    auto &LiveOut = It->second;
    if (Added) {
      LiveOut.resize(N);
      std::vector<bool> Kills(N), ReadsNarrow(N), ReadsWider(N);
      std::vector<size_t> Pending;
      for (size_t I = 0; I < N; ++I) {
        for (const MedOp &Op : Func.Blocks[I].Ops) {
          if (!InLoop[I])
            for (unsigned J = 0; J < Op.NumInputs; ++J) {
              const auto &Read = Op.Inputs[J];
              if (Read.Kind != MedVar::Reg || !Read.Size || Read.Size > Width)
                continue;
              // Either byte of DX can have its own exit PHI after a DX write.
              if (Off <= Read.RegOff && Read.RegOff + Read.Size <= Off + Size)
                ReadsNarrow[I] = true;
              else if (Read.RegOff <= Off &&
                       Off + Size <= Read.RegOff + Read.Size)
                ReadsWider[I] = true;
            }
          const auto &Write = Op.Output;
          if (Op.Opcode == NdOp::COPY && Op.NumInputs == 1 &&
              Op.Inputs[0].Kind == MedVar::Reg &&
              Op.Inputs[0].RegOff == Parent && Op.Inputs[0].Size == Write.Size)
            continue;
          if (Write.Kind == MedVar::Reg && Write.RegOff == Parent &&
              (Write.Size == Width ||
               TRI.writeZeroExtends(Parent, Write.Size))) {
            Kills[I] = true;
            break;
          }
        }
      }
      // A whole-register definition on even one incoming edge gives the
      // joined parent a live range. Splice the narrow writes on the other
      // edges into it before building SSA, including a partial loop seed.
      std::vector<bool> HasParent(N);
      for (size_t I = 0; I < N; ++I)
        if (Kills[I])
          Pending.push_back(I);
      while (!Pending.empty()) {
        size_t I = Pending.back();
        Pending.pop_back();
        for (size_t Succ : SuccIdx[I])
          if (!HasParent[Succ]) {
            HasParent[Succ] = true;
            Pending.push_back(Succ);
          }
      }
      for (size_t I = 0; I < N; ++I)
        if (ReadsWider[I] || (ReadsNarrow[I] && HasParent[I]))
          Pending.push_back(I);
      while (!Pending.empty()) {
        auto I = Pending.back();
        Pending.pop_back();
        for (int Pred : Func.Blocks[I].Preds) {
          auto Pos = IdToIdx.find(Pred);
          if (Pos != IdToIdx.end() && !LiveOut[Pos->second]) {
            LiveOut[Pos->second] = true;
            if (!Kills[Pos->second])
              Pending.push_back(Pos->second);
          }
        }
      }
    }
    return bool(LiveOut[Writer]);
  };

  // True when the wide parent is (re)defined at full width earlier in the same
  // block (a SUBBYTES self-slice is a read-view, not a real definition).  Such
  // a parent holds a fresh in-iteration value at the partial write — clang uses
  // it as scratch (compute the new byte in a temp register, then transfer it
  // via a full-width `mov`) — so merging is safe even inside a loop.  A parent
  // carried across iterations otherwise needs either a wide observation or
  // a loop-carried narrow PHI.
  auto parentFreshInBlock = [&](const MedBlock &MB, size_t OI, uint64_t WOff,
                                uint16_t WSz) -> bool {
    for (size_t J = 0; J < OI; ++J) {
      const auto &O = MB.Ops[J];
      if (O.Output.Kind != MedVar::Reg || O.Output.Size < WSz ||
          O.Output.RegOff > WOff ||
          O.Output.RegOff + O.Output.Size < WOff + WSz)
        continue;
      if (O.Opcode == NdOp::SUBBYTES && O.NumInputs >= 1) {
        const MedVar &In0 = O.Inputs[0];
        if (In0.Kind == MedVar::Reg && In0.RegOff <= O.Output.RegOff &&
            In0.RegOff + In0.Size >= O.Output.RegOff + O.Output.Size)
          continue; // self-slice read-view
      }
      return true;
    }
    return false;
  };

  struct MergeInsert {
    size_t Block, AfterOp;
    std::vector<MedOp> Seq;
  };
  std::vector<MergeInsert> Pending;
  for (size_t BI = 0; BI < Func.Blocks.size(); ++BI) {
    const auto &MB = Func.Blocks[BI];
    for (size_t OI = 0; OI < MB.Ops.size(); ++OI) {
      const MedOp &MOp = MB.Ops[OI];
      // Use the target's alias table rather than a size test: a 32-bit GPR
      // write zero-extends its parent, while a 16-byte legacy XMM write still
      // preserves the high half of the 32-byte YMM slot.
      if (MOp.Output.Kind != MedVar::Reg ||
          !isPreservingPartialWrite(MOp.Output.RegOff, MOp.Output.Size))
        continue;

      // A SUBBYTES that self-slices its own wider register is a read-view, not
      // a partial write; merging it back would be circular.
      if (MOp.Opcode == NdOp::SUBBYTES && MOp.NumInputs >= 1) {
        const MedVar &In0 = MOp.Inputs[0];
        if (In0.Kind == MedVar::Reg && In0.RegOff <= MOp.Output.RegOff &&
            In0.RegOff + In0.Size >= MOp.Output.RegOff + MOp.Output.Size)
          continue;
      }

      uint64_t POff = MOp.Output.RegOff;
      uint16_t PSz = MOp.Output.Size;
      auto [WOff, WSz] = TRI.findWideReg(POff, PSz);
      if (WSz <= PSz)
        continue;
      int ByteOff = TRI.subRegByteOffset(POff, PSz, WOff, WSz);
      if (ByteOff < 0)
        continue;
      // A wider cross-block read already uses the parent for a forward seed,
      // a whole-register loop PHI or a fresh in-block definition. Otherwise
      // check whether a previously narrow loop value escapes to a later read.
      const bool HasWideUse =
          wideParentReadElsewhere(POff, PSz, WOff, WSz, BI) &&
          ((!InLoop[BI] && !LoopNarrowCarried.contains(WOff)) ||
           WideFullWrittenInLoop.contains(WOff) ||
           parentFreshInBlock(MB, OI, WOff, WSz));
      if (!HasWideUse &&
          !(LoopNarrowCarried.contains(WOff) && TRI.isGeneralReg(WOff) &&
            readAfterLoop(POff, PSz, WOff, WSz, BI)))
        continue;

      // The reconstructed wide parent must carry the canonical SSA id of its
      // (offset,size) register class so buildSsa folds this definition into the
      // register's existing live range.  Leaving the id 0 forges a second
      // lineage that races the real value through a duplicate phi — e.g. an
      // `incb %al` seed reconstructed into RAX would otherwise shadow the
      // function's RAX return value (VectorAlgo14 memcmp).
      auto WideKey = std::make_pair(WOff, WSz);
      int WideId;
      if (auto WIt = RegVarMap.find(WideKey); WIt != RegVarMap.end())
        WideId = WIt->second;
      else {
        WideId = allocVarId();
        RegVarMap[WideKey] = WideId;
      }

      std::vector<MedOp> Ops;

      MedVar WideIn;
      WideIn.Kind = MedVar::Reg;
      WideIn.Id = WideId;
      WideIn.RegOff = WOff;
      WideIn.Size = WSz;
      WideIn.TheArch = TargetArch;

      MedVar Combined = spliceX86Subregister(
          WideIn, MOp.Output, static_cast<uint16_t>(ByteOff), MOp.Addr, Ops);

      MedVar WideOut;
      WideOut.Kind = MedVar::Reg;
      WideOut.Id = WideId;
      WideOut.RegOff = WOff;
      WideOut.Size = WSz;
      WideOut.TheArch = TargetArch;
      MedOp Copy;
      Copy.Opcode = NdOp::COPY;
      Copy.Addr = MOp.Addr;
      Copy.Output = WideOut;
      Copy.addInput(Combined);
      Ops.push_back(std::move(Copy));

      Pending.push_back({BI, OI, std::move(Ops)});
    }
  }
  // Query the original reads and definitions in every block before inserting
  // synthetic parent writes. Reverse order keeps the recorded indices valid.
  for (auto It = Pending.rbegin(); It != Pending.rend(); ++It) {
    auto &Ops = Func.Blocks[It->Block].Ops;
    Ops.insert(Ops.begin() + static_cast<long>(It->AfterOp + 1),
               It->Seq.begin(), It->Seq.end());
  }
}

} // namespace neverd
