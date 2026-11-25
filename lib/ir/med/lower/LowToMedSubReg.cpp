//===- LowToMedSubReg.cpp - Generic sub-register fixups ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Architecture-generic sub-register fixups for LowIR to MedIR conversion.
///
//===----------------------------------------------------------------------===//

#include "neverd/ir/SubRegViews.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/low/CallRegisterEffects.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/ir/med/MedCallLeftovers.h"

#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <vector>

namespace neverd {

// SUBBYTES that re-views the low bytes of the SAME register it writes
// (EAX := low32(RAX), offset 0) is a projection, not a new definition.
// SUBBYTES of an unrelated temp or register into a GP (MUL/DIV high half
// stored to RDX, low half stored to RAX) is a real write: Phase A must
// zero-extend a 32-bit dest, and Phase C must project narrower aliases
// across a block edge (tokenize: `mulq` then a branch then `imul %edx`).
static bool isSelfLowSliceSubbytes(const MedOp &MOp) {
  if (MOp.Opcode != NdOp::SUBBYTES)
    return false;
  return MOp.NumInputs >= 1 && MOp.Inputs[0].Kind == MedVar::Reg &&
         isSameRegisterLowSlice(MOp.Output.RegOff, MOp.Output.Size,
                                MOp.Inputs[0].RegOff, MOp.Inputs[0].Size,
                                MOp.NumInputs < 2 || !MOp.Inputs[1].isConst()
                                    ? 0
                                    : MOp.Inputs[1].ConstVal);
}

// Puts each pending op before the op of \p Ops at its index, in one pass:
// what inserting them one at a time from the last gives.  \p Pending is in
// index order, with no index past Ops.size(); ops pending at one index keep
// their order.
template <typename PendingOp>
static void insertPending(std::vector<MedOp> &Ops,
                          std::vector<PendingOp> &Pending,
                          size_t PendingOp::*Index) {
  if (Pending.empty())
    return;
  std::vector<MedOp> Merged;
  Merged.reserve(Ops.size() + Pending.size());
  size_t Next = 0;
  for (size_t I = 0; I < Ops.size(); ++I) {
    for (; Next < Pending.size() && Pending[Next].*Index == I; ++Next)
      Merged.push_back(std::move(Pending[Next].Op));
    Merged.push_back(std::move(Ops[I]));
  }
  for (; Next < Pending.size(); ++Next)
    Merged.push_back(std::move(Pending[Next].Op));
  Ops = std::move(Merged);
}

// Calls \p Visit on each write of \p Writes, keyed by (offset, size), to a
// register wider than (\p Off, \p Size) that starts at \p Off, in key order.
template <typename WriteMap, typename VisitFn>
static void forEachWiderWriteAt(const WriteMap &Writes, uint64_t Off,
                                uint16_t Size, VisitFn Visit) {
  for (auto It = Writes.upper_bound({Off, Size});
       It != Writes.end() && It->first.first == Off; ++It)
    Visit(It->first, It->second);
}

// A leaf operating exclusively on whole registers has no narrower writer
// or reader to connect. Calls can also require an implicit preserved prefix.
static bool isWholeRegisterLeaf(const MedFunc &Func, const TargetRegInfo &TRI) {
  auto isWhole = [&](const MedVar &V) {
    return V.Kind != MedVar::Reg || (V.Size == TRI.FullRegWidth &&
                                     TRI.maxRegisterWidth(V.RegOff) <= V.Size);
  };
  for (const auto &Block : Func.Blocks) {
    if (!Block.Phis.empty())
      return false;
    for (const auto &Op : Block.Ops) {
      if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL ||
          !isWhole(Op.Output))
        return false;
      for (unsigned I = 0; I < Op.NumInputs; ++I)
        if (!isWhole(Op.Inputs[I]))
          return false;
    }
  }
  return true;
}

void LowToMedConverter::fixupSubRegisters(MedFunc &Func) {
  const auto &TRI = getTargetRegInfo(TargetArch);

  if (isWholeRegisterLeaf(Func, TRI))
    return;

  // Phase A: Implicit zero-extension insertion.
  // On x86-64 and AArch64, writing a 32-bit register implicitly zero-extends
  // to 64 bits.  Insert explicit INT_ZEXT so later passes see the dependency.
  struct PendingZext {
    size_t InsertBefore;
    MedOp Op;
  };
  for (auto &MB : Func.Blocks) {
    // Each zero-extension goes right after its write, once the block has
    // been read.
    std::vector<PendingZext> Zexts;
    for (size_t OI = 0; OI < MB.Ops.size(); ++OI) {
      auto &MOp = MB.Ops[OI];
      if (MOp.Output.Kind != MedVar::Reg || MOp.Output.Size == 0)
        continue;
      if (isSelfLowSliceSubbytes(MOp))
        continue;
      if (!TRI.writeZeroExtends(MOp.Output.RegOff, MOp.Output.Size))
        continue;

      auto [WideOff, WideSz] =
          TRI.findWideReg(MOp.Output.RegOff, MOp.Output.Size);
      if (WideSz <= MOp.Output.Size)
        continue;

      bool AlreadyZexted = false;
      if (OI + 1 < MB.Ops.size()) {
        auto &Next = MB.Ops[OI + 1];
        if (Next.Opcode == NdOp::INT_ZEXT && Next.NumInputs >= 1 &&
            Next.Inputs[0].Kind == MedVar::Reg &&
            Next.Inputs[0].RegOff == MOp.Output.RegOff &&
            Next.Inputs[0].Size == MOp.Output.Size &&
            Next.Output.Size == WideSz)
          AlreadyZexted = true;
      }
      if (AlreadyZexted)
        continue;

      auto WideKey = std::make_pair(WideOff, WideSz);
      int WideId;
      auto WideIt = RegVarMap.find(WideKey);
      if (WideIt != RegVarMap.end())
        WideId = WideIt->second;
      else {
        WideId = allocVarId();
        RegVarMap[WideKey] = WideId;
      }

      MedOp Zext;
      Zext.Opcode = NdOp::INT_ZEXT;
      Zext.Addr = MOp.Addr;
      Zext.Output.Kind = MedVar::Reg;
      Zext.Output.RegOff = WideOff;
      Zext.Output.Size = WideSz;
      Zext.Output.Id = WideId;
      Zext.Output.TheArch = TargetArch;
      Zext.addInput(MOp.Output);
      Zexts.push_back({OI + 1, std::move(Zext)});
    }
    insertPending(MB.Ops, Zexts, &PendingZext::InsertBefore);
  }

  // Phase B: Register-write → narrower-read SUBBYTES insertion.
  // Two-pass approach: first scan to find insertion points, then apply.
  // This avoids iterator invalidation from modifying the ops vector.

  for (auto &MB : Func.Blocks) {
    RegWriteMap AllWrites;
    RegWriteMap ZextWrites;
    size_t Seq = 0;

    // Seed AllWrites with PHI outputs so sub-register reads in the loop
    // body can resolve to the loop-carried phi variable (e.g. SIL → RSI phi).
    for (const auto &Phi : MB.Phis) {
      if (Phi.Output.Size > 0 && Phi.Output.RegOff != 0) {
        auto Key = std::make_pair(Phi.Output.RegOff, Phi.Output.Size);
        AllWrites[Key] = {Phi.Output.Id, Phi.Output.Size, Phi.Output.RegOff,
                          Seq++};
      }
    }

    struct PendingSubpiece {
      size_t InsertBefore;
      MedOp Op;
    };
    std::vector<PendingSubpiece> Pending;

    for (size_t OI = 0; OI < MB.Ops.size(); ++OI) {
      const auto &MOp = MB.Ops[OI];

      for (uint8_t I = 0; I < MOp.NumInputs; ++I) {
        const auto &Inp = MOp.Inputs[I];
        if (Inp.Kind != MedVar::Reg || Inp.Size == 0)
          continue;

        auto tryRecord = [&](uint64_t WideOff, uint16_t WideSz) -> bool {
          if (WideSz <= Inp.Size)
            return false;
          auto WKey = std::make_pair(WideOff, WideSz);
          const RegWriteInfo *Best = nullptr;
          auto WIt = AllWrites.find(WKey);
          if (WIt != AllWrites.end())
            Best = &WIt->second;
          auto ZIt = ZextWrites.find(WKey);
          if (ZIt != ZextWrites.end() && (!Best || ZIt->second.Ord > Best->Ord))
            Best = &ZIt->second;
          if (!Best)
            return false;
          const auto &WI = *Best;
          if (WI.Id == Inp.Id)
            return false;

          int ByteOff =
              TRI.subRegByteOffset(Inp.RegOff, Inp.Size, WideOff, WideSz);
          if (ByteOff < 0 && Inp.RegOff == WideOff)
            ByteOff = 0;
          if (ByteOff < 0)
            return false;

          auto SKey = std::make_pair(Inp.RegOff, Inp.Size);
          auto SIt = AllWrites.find(SKey);
          if (SIt != AllWrites.end() && SIt->second.Ord > WI.Ord)
            return false;
          auto SZIt = ZextWrites.find(SKey);
          if (SZIt != ZextWrites.end() && SZIt->second.Ord > WI.Ord)
            return false;

          MedOp Sub;
          Sub.Opcode = NdOp::SUBBYTES;
          Sub.Addr = MOp.Addr;
          Sub.Output = Inp;
          MedVar Wide;
          Wide.Kind = MedVar::Reg;
          Wide.Id = WI.Id;
          Wide.Size = WideSz;
          Wide.RegOff = WideOff;
          Wide.TheArch = TargetArch;
          Sub.addInput(Wide);
          Sub.addInput(MedVar::makeConst(static_cast<uint64_t>(ByteOff), 4));
          Pending.push_back({OI, std::move(Sub)});
          return true;
        };

        bool Found = false;
        {
          // Collect EVERY wider register that contains this narrow input and
          // was written, then reconstruct from the MOST RECENTLY written one.
          // Two kinds of containment must be considered together:
          //   (a) same start offset, larger size      (EAX within RAX)
          //   (b) the SubRegs table                    (an ARM S lane lives in
          //       both its D parent AND its Q parent)
          // Picking the newest writer is essential and is why (a) and (b) are
          // unified: a mid-register lane such as ARM S(4N+2) has the SAME start
          // offset as the high D half D(2N+1).  If we only matched start offset
          // (case a) we would capture a stale `vld1` that wrote that D half and
          // miss the later `vcvt` that wrote the whole Q — the high lanes of a
          // per-lane int->float reduction would then read a rodata constant
          // (VectorAlgo8 arm32 fmla/fdiv).
          uint64_t BestOff = 0;
          uint16_t BestSz = 0;
          size_t BestOrd = 0;
          bool HaveCandidate = false;
          auto consider = [&](uint64_t WOff, uint16_t WSz) {
            if (WSz <= Inp.Size)
              return;
            int BO = TRI.subRegByteOffset(Inp.RegOff, Inp.Size, WOff, WSz);
            if (BO < 0 && Inp.RegOff == WOff)
              BO = 0;
            if (BO < 0)
              return; // wide register does not actually contain the narrow
                      // input
            auto WKey = std::make_pair(WOff, WSz);
            size_t Ord = 0;
            bool Written = false;
            auto AIt = AllWrites.find(WKey);
            if (AIt != AllWrites.end()) {
              Ord = AIt->second.Ord;
              Written = true;
            }
            auto ZIt = ZextWrites.find(WKey);
            if (ZIt != ZextWrites.end() && ZIt->second.Ord > Ord) {
              Ord = ZIt->second.Ord;
              Written = true;
            }
            if (Written && (!HaveCandidate || Ord > BestOrd)) {
              BestOff = WOff;
              BestSz = WSz;
              BestOrd = Ord;
              HaveCandidate = true;
            }
          };
          // (a) same start offset, wider size.
          forEachWiderWriteAt(AllWrites, Inp.RegOff, Inp.Size,
                              [&](const auto &Key, const auto &) {
                                consider(Key.first, Key.second);
                              });
          forEachWiderWriteAt(ZextWrites, Inp.RegOff, Inp.Size,
                              [&](const auto &Key, const auto &) {
                                consider(Key.first, Key.second);
                              });
          // (b) SubRegs table (mid-register lanes whose start offset differs).
          forEachSubRegOfNarrowView(
              TRI, Inp.RegOff, Inp.Size, [&](const SubRegEntry &E) {
                consider(E.WideRegOff, static_cast<uint16_t>(E.WideSize));
              });
          if (HaveCandidate && tryRecord(BestOff, BestSz))
            Found = true;
        }

        if (!Found) {
          uint64_t BestOff = 0;
          uint16_t BestSz = 0;
          size_t BestOrd = 0;
          bool HaveCandidate = false;
          forEachWiderWriteAt(ZextWrites, Inp.RegOff, Inp.Size,
                              [&](const auto &Key, const auto &WI) {
                                if (!HaveCandidate || WI.Ord > BestOrd) {
                                  BestOff = Key.first;
                                  BestSz = Key.second;
                                  BestOrd = WI.Ord;
                                  HaveCandidate = true;
                                }
                              });
          if (HaveCandidate) {
            auto WKey = std::make_pair(BestOff, BestSz);
            auto WIt = ZextWrites.find(WKey);
            if (WIt != ZextWrites.end()) {
              const auto &WI = WIt->second;
              auto SKey = std::make_pair(Inp.RegOff, Inp.Size);
              auto SIt = AllWrites.find(SKey);
              if (SIt == AllWrites.end() || SIt->second.Ord < WI.Ord) {
                int ByteOff =
                    TRI.subRegByteOffset(Inp.RegOff, Inp.Size, BestOff, BestSz);
                if (ByteOff < 0 && Inp.RegOff == BestOff)
                  ByteOff = 0;
                if (ByteOff >= 0) {
                  MedOp Sub;
                  Sub.Opcode = NdOp::SUBBYTES;
                  Sub.Addr = MOp.Addr;
                  Sub.Output = Inp;
                  MedVar Wide;
                  Wide.Kind = MedVar::Reg;
                  Wide.Id = WI.Id;
                  Wide.Size = BestSz;
                  Wide.RegOff = BestOff;
                  Wide.TheArch = TargetArch;
                  Sub.addInput(Wide);
                  Sub.addInput(
                      MedVar::makeConst(static_cast<uint64_t>(ByteOff), 4));
                  Pending.push_back({OI, std::move(Sub)});
                  Found = true;
                }
              }
            }
          }
        }

        // ARM: a SUBBYTES of a wide NEON register whose extracted bytes lie
        // entirely within a more recently written D/S sub-register may read
        // that narrower register directly (LowToMedARM.cpp).
        if (!Found && I == 0 &&
            redirectWideSubpieceToNarrowARM(MB, OI, AllWrites))
          Found = true;
      }

      // A later narrow read must not be projected from a wider register view
      // that this call clobbered.  This matters for AAPCS64 v8-v15: D9 is
      // preserved while Q9 as a whole is not.  Keep the exact preserved D
      // write, but forget the Q write before considering post-call inputs.
      if ((MOp.Opcode == NdOp::CALL || MOp.Opcode == NdOp::INDIR_CALL) &&
          !MOp.PreservesCallerSaved) {
        // A full Q write may be the only definition of its low D view. Before
        // discarding the volatile Q carrier, materialize the prefix that the
        // call ABI preserves. Otherwise a post-call D read reconnects to a
        // stale earlier D definition even though the Q write replaced it.
        struct PartialWrite {
          RegWriteInfo Write;
          bool FromZext = false;
        };
        std::map<std::pair<uint64_t, uint16_t>, PartialWrite>
            PartiallyPreserved;
        auto CollectPartial = [&](const RegWriteMap &Writes, bool FromZext) {
          for (const auto &[Key, Write] : Writes) {
            const uint16_t Prefix =
                goCallKeptBytes(MOp, TRI, Key.first, Key.second)
                    .value_or(
                        TRI.callPreservedPrefixSize(Key.first, Key.second));
            if (Prefix == 0 || Prefix >= Key.second)
              continue;
            auto [It, Inserted] =
                PartiallyPreserved.emplace(Key, PartialWrite{Write, FromZext});
            if (!Inserted && It->second.Write.Ord < Write.Ord)
              It->second = {Write, FromZext};
          }
        };
        CollectPartial(AllWrites, false);
        CollectPartial(ZextWrites, true);
        for (const auto &[WideKey, Partial] : PartiallyPreserved) {
          const auto &WideWrite = Partial.Write;
          const uint16_t Prefix =
              goCallKeptBytes(MOp, TRI, WideKey.first, WideKey.second)
                  .value_or(TRI.callPreservedPrefixSize(WideKey.first,
                                                        WideKey.second));
          const auto NarrowKey = std::make_pair(WideKey.first, Prefix);
          auto NarrowId = RegVarMap.find(NarrowKey);
          if (NarrowId == RegVarMap.end())
            continue;
          auto ExistingNarrow = AllWrites.find(NarrowKey);
          if (Partial.FromZext && ExistingNarrow != AllWrites.end() &&
              ExistingNarrow->second.Ord + 1 == WideWrite.Ord)
            continue;
          bool HasNewerNarrow = false;
          for (const RegWriteMap *Writes : {&AllWrites, &ZextWrites}) {
            auto It = Writes->find(NarrowKey);
            if (It != Writes->end() && It->second.Ord > WideWrite.Ord) {
              HasNewerNarrow = true;
            }
          }
          if (HasNewerNarrow)
            continue;

          MedVar Wide;
          Wide.Kind = MedVar::Reg;
          Wide.Id = WideWrite.Id;
          Wide.Size = WideKey.second;
          Wide.RegOff = WideKey.first;
          Wide.TheArch = TargetArch;
          MedVar Narrow = Wide;
          Narrow.Id = NarrowId->second;
          Narrow.Size = Prefix;
          MedOp Extract;
          Extract.Opcode = NdOp::SUBBYTES;
          Extract.Addr = MOp.Addr;
          Extract.Output = Narrow;
          Extract.addInput(Wide);
          Extract.addInput(MedVar::makeConst(0, 4));
          Pending.push_back({OI, std::move(Extract)});
          AllWrites[NarrowKey] = {Narrow.Id, Narrow.Size, Narrow.RegOff, Seq++};
          ZextWrites.erase(NarrowKey);
        }

        // A register the callee's summary shows it never writes keeps the
        // caller's value across the call as a callee-saved one does.
        auto SummaryPreserved = [&](uint64_t RegOff, uint16_t Size) {
          return callPreservesRegisterBytes(TargetArch, MOp.CallPreservedGPRs,
                                            RegOff, Size);
        };
        auto DiscardClobbered = [&](RegWriteMap &Writes) {
          for (auto It = Writes.begin(); It != Writes.end();) {
            uint64_t RegOff = It->first.first;
            uint16_t Size = It->first.second;
            const std::optional<uint16_t> GoKept =
                goCallKeptBytes(MOp, TRI, RegOff, Size);
            if (!TRI.isFrameOrLinkReg(RegOff) &&
                !(GoKept ? *GoKept >= Size
                         : TRI.isCallPreserved(RegOff, Size, TargetFormat)) &&
                !SummaryPreserved(RegOff, Size))
              It = Writes.erase(It);
            else
              ++It;
          }
        };
        DiscardClobbered(AllWrites);
        DiscardClobbered(ZextWrites);
      }

      if (MOp.Output.Kind == MedVar::Reg && MOp.Output.Size > 0) {
        bool IsZextFromNarrow = MOp.Opcode == NdOp::INT_ZEXT &&
                                MOp.NumInputs >= 1 &&
                                MOp.Inputs[0].Kind == MedVar::Reg &&
                                MOp.Inputs[0].RegOff == MOp.Output.RegOff &&
                                MOp.Inputs[0].Size < MOp.Output.Size;
        auto Key = std::make_pair(MOp.Output.RegOff, MOp.Output.Size);
        if (!IsZextFromNarrow) {
          AllWrites[Key] = {MOp.Output.Id, MOp.Output.Size, MOp.Output.RegOff,
                            Seq++};
        } else {
          ZextWrites[Key] = {MOp.Output.Id, MOp.Output.Size, MOp.Output.RegOff,
                             Seq++};
        }
      }
    }

    insertPending(MB.Ops, Pending, &PendingSubpiece::InsertBefore);
  }

  // Phase B2 (x86/x64): merge a more-recent narrow partial write (AL/AH/AX)
  // into a subsequent wider read of the parent register.  See LowToMedX86.cpp.
  fixupPartialWritesX86(Func);

  // Phase B2x (x86/x64): the in-block merge above cannot reach a parent read in
  // another block (the value flows through a phi).  Define the wide parent
  // right after such a partial write so buildSsa carries the merged value
  // across the block boundary.  See LowToMedX86.cpp.
  mergePartialWritesCrossBlockX86(Func);

  // Phase B3 (ARM/AArch64): reconstruct a full-width NEON Q read from its two
  // more-recent 8-byte D halves within a block.  See LowToMedARM.cpp.
  mergeWideVectorReadsARM(Func);

  // Phase C: Wide GP register write → narrow sub-register SUBBYTES.
  // Only insert when: (1) a wide register is the LAST write at that offset
  // in a block, (2) no narrower write follows, and (3) the narrow sub-reg
  // is read in a DIFFERENT block. This targets the case where a loop body
  // mixes 64-bit and 32-bit operations on the same register (e.g. c_gcd).
  // Self-extract SUBBYTES (EAX := low32(RAX)) are projections, not new
  // wide definitions.  SUBBYTES from an unrelated temp (one-operand MUL/
  // DIV writing RDX:RAX) ARE wide definitions and must project EDX/EAX
  // to successors; otherwise a branch between `mulq` and `imul %edx`
  // keeps a stale 32-bit overlay (x64o55_tokenize).
  {
    using RegKey = std::pair<uint64_t, uint16_t>;
    std::map<RegKey, std::set<int>> UpwardReadBlocks;
    for (size_t BI = 0; BI < Func.Blocks.size(); ++BI) {
      auto &B = Func.Blocks[BI];
      std::set<int> LocalDefs;
      for (auto &Op : B.Ops) {
        for (uint8_t I2 = 0; I2 < Op.NumInputs; ++I2) {
          auto &Inp = Op.Inputs[I2];
          if (Inp.Kind == MedVar::Reg && Inp.Size > 0 &&
              !LocalDefs.count(Inp.Id))
            UpwardReadBlocks[{Inp.RegOff, Inp.Size}].insert(B.Id);
        }
        if (Op.Output.Kind == MedVar::Reg && Op.Output.Size > 0)
          LocalDefs.insert(Op.Output.Id);
      }
    }

    // A wide write only needs a projected narrow value when that value can
    // flow across at least one real CFG edge to an upward narrow read.  A
    // function-wide read inventory alone is too broad: an exit block may
    // write X0 after an earlier loop read W0, and projecting W0 at the exit
    // would turn a signed 64-bit return into a zero-extended 32-bit return.
    std::map<int, const MedBlock *> BlockById;
    for (const auto &B : Func.Blocks)
      BlockById.emplace(B.Id, &B);
    std::map<RegKey, std::set<int>> BlocksReachingUpwardRead;
    for (const auto &[Key, ReadBlocks] : UpwardReadBlocks) {
      auto &Reachable = BlocksReachingUpwardRead[Key];
      Reachable = ReadBlocks;
      std::vector<int> Worklist(ReadBlocks.begin(), ReadBlocks.end());
      while (!Worklist.empty()) {
        const int BlockId = Worklist.back();
        Worklist.pop_back();
        auto BlockIt = BlockById.find(BlockId);
        if (BlockIt == BlockById.end())
          continue;
        for (int Pred : BlockIt->second->Preds)
          if (Reachable.insert(Pred).second)
            Worklist.push_back(Pred);
      }
    }

    for (auto &MB : Func.Blocks) {
      // Find the LAST write index for each (RegOff, Size) pair.  Skip only
      // self-extract SUBBYTES (a sub-register projection of the same
      // register).  An INT_ZEXT normally leaves its input alias as the real
      // narrow definition, but it is also the only definition of any
      // intermediate alias wider than that input (for example YMM after a
      // VEX.128 XMM -> ZMM zero-extension), so retain it for the per-read
      // filter below.
      std::map<std::pair<uint64_t, uint16_t>, size_t> LastWide;
      for (size_t OI = 0; OI < MB.Ops.size(); ++OI) {
        auto &MOp = MB.Ops[OI];
        if (MOp.Output.Kind == MedVar::Reg && MOp.Output.Size > 0 &&
            !isSelfLowSliceSubbytes(MOp))
          LastWide[{MOp.Output.RegOff, MOp.Output.Size}] = OI;
      }

      struct NarrowInsert {
        size_t InsertAfter;
        MedOp Op;
      };
      std::vector<NarrowInsert> NPending;

      // For every narrow sub-register that is read in another block (this
      // includes a loop back-edge when the block is its own predecessor),
      // find the latest wide write at the SAME register offset that strictly
      // contains it, and materialize a SUBBYTES right after that write.  This
      // makes the SSA loop-carried value of the narrow register reflect the
      // wide write's low bits instead of a stale earlier narrow definition.
      //
      // x86 crc8 motivating case: the inner loop's final `cmov %eax`/`mov`
      // updates EAX (and implicitly RAX), but the loop-carried CRC lives in
      // AL.  Without this, AL's phi latch keeps the next-to-last iteration's
      // value, silently dropping one loop iteration.  Iterating over the read
      // narrow registers (rather than over `Size/2` of each wide write) lets
      // us cross more than one sub-register level (EAX→AL), and also still
      // covers the RAX→EAX case used by c_gcd (bug #157e / #154).
      for (const auto &[NKey, Reachable] : BlocksReachingUpwardRead) {
        const bool NeededAcrossEdge =
            std::any_of(MB.Succs.begin(), MB.Succs.end(),
                        [&](int Succ) { return Reachable.count(Succ) != 0; });
        if (!NeededAcrossEdge)
          continue;
        uint64_t NarOff = NKey.first;
        uint16_t NarSz = NKey.second;
        auto NIt = RegVarMap.find(NKey);
        if (NIt == RegVarMap.end())
          continue;

        // The narrow register must be contained in the wide write.  Match
        // either (a) the same starting offset & wider (GP families AL/AX/EAX/
        // RAX, W/X), or (b) a non-zero byte offset recorded in the sub-register
        // table.  Case (b) is essential for NEON: the high D-half D(2N+1) of a
        // Q register lives at WideOff+8, so a wide vcvt/vmul write to Q in one
        // block must materialize *both* D halves for cross-block `vst1
        // {dN,dN+1}` reads (low half shared the same offset and worked; the
        // high half was silently left stale).  Mirrors Phase B's table use.
        size_t BestOI = MB.Ops.size();
        int BestByteOff = 0;
        for (const auto &[WKey, OI] : LastWide) {
          uint64_t WideOff = WKey.first;
          uint16_t WideSz = WKey.second;
          if (WideSz <= NarSz)
            continue;
          const MedOp &WideOp = MB.Ops[OI];
          if (WideOp.Opcode == NdOp::INT_ZEXT) {
            if (WideOp.NumInputs < 1)
              continue;
            // The zero-extension of a register's own low slice leaves that
            // slice as the definition of every view no wider than it.  One of
            // anything else, as the byte a load gave (AArch64 `ldrb w7, ...`
            // is a temporary's), defines the whole register it writes, and
            // its low bytes with it.
            const MedVar &From = WideOp.Inputs[0];
            int InputByteOff = -1;
            if (From.Kind == MedVar::Reg) {
              InputByteOff =
                  TRI.subRegByteOffset(From.RegOff, From.Size, WideOff, WideSz);
              if (InputByteOff < 0 && From.RegOff == WideOff)
                InputByteOff = 0;
            }
            const bool OwnSlice = From.Kind == MedVar::Reg && InputByteOff >= 0;
            if (OwnSlice && (From.Size >= NarSz || InputByteOff != 0))
              continue;
          }
          int ByteOff;
          if (WideOff == NarOff)
            ByteOff = 0;
          else {
            ByteOff = TRI.subRegByteOffset(NarOff, NarSz, WideOff, WideSz);
            if (ByteOff < 0)
              continue;
          }
          if (BestOI == MB.Ops.size() || OI > BestOI) {
            BestOI = OI;
            BestByteOff = ByteOff;
          }
        }
        if (BestOI == MB.Ops.size())
          continue;
        auto &MOp = MB.Ops[BestOI];
        if (MOp.Output.Id == NIt->second)
          continue;

        // Only skip if the narrow sub-register is written *after* the wide
        // write.  A narrow def that precedes the wide write does not represent
        // the current value at block end — the wide write's low bits do (e.g.
        // AArch64 `sbfx x8,x8,...` overwrites x8 with mid after earlier w8
        // computations, so cross-block w8 reads must see mid).  bug #157e
        bool NarrowDefinedAfter = false;
        for (size_t K = BestOI + 1; K < MB.Ops.size(); ++K) {
          auto &BOp = MB.Ops[K];
          if (BOp.Output.Kind == MedVar::Reg && BOp.Output.RegOff == NarOff &&
              BOp.Output.Size == NarSz) {
            NarrowDefinedAfter = true;
            break;
          }
        }
        if (NarrowDefinedAfter)
          continue;

        MedOp Sub;
        Sub.Opcode = NdOp::SUBBYTES;
        Sub.Addr = MOp.Addr;
        Sub.Output.Kind = MedVar::Reg;
        Sub.Output.RegOff = NarOff;
        Sub.Output.Size = NarSz;
        Sub.Output.Id = NIt->second;
        Sub.Output.TheArch = TargetArch;
        MedVar Wide;
        Wide.Kind = MedVar::Reg;
        Wide.Id = MOp.Output.Id;
        Wide.Size = MOp.Output.Size;
        Wide.RegOff = MOp.Output.RegOff;
        Wide.TheArch = TargetArch;
        Sub.addInput(Wide);
        Sub.addInput(MedVar::makeConst(static_cast<uint64_t>(BestByteOff), 4));
        NPending.push_back({BestOI + 1, std::move(Sub)});
      }

      std::sort(NPending.begin(), NPending.end(),
                [](const NarrowInsert &A, const NarrowInsert &B) {
                  return A.InsertAfter < B.InsertAfter;
                });
      insertPending(MB.Ops, NPending, &NarrowInsert::InsertAfter);
    }
  }

  // Phase C2 (ARM/AArch64): synthesize Q = CONCAT(D_high, D_low) after the last
  // D-half write so cross-block wide reads observe the D values.  Runs after
  // the generic Phase C above.  See LowToMedARM.cpp.
  synthesizeWideVectorWritesARM(Func);
}

} // namespace neverd
