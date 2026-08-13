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

#include "neverd/ir/med/LowToMed.h"

#include "neverd/ir/TargetRegInfo.h"

#include <algorithm>
#include <map>
#include <set>

namespace neverd {

void LowToMedConverter::fixupSubRegisters(MedFunc &Func) {
  const auto &TRI = getTargetRegInfo(TargetArch);

  // Phase A: Implicit zero-extension insertion.
  // On x86-64 and AArch64, writing a 32-bit register implicitly zero-extends
  // to 64 bits.  Insert explicit INT_ZEXT so later passes see the dependency.
  for (auto &MB : Func.Blocks) {
    for (size_t OI = 0; OI < MB.Ops.size(); ++OI) {
      auto &MOp = MB.Ops[OI];
      if (MOp.Output.Kind != MedVar::Reg || MOp.Output.Size == 0)
        continue;
      if (MOp.Opcode == NdOp::SUBBYTES) {
        // A SUBBYTES re-viewing the low bytes of the SAME wide register it
        // writes (EAX := low32(RAX), offset 0) is a no-op extract; zero-
        // extending it would wrongly clobber that register's genuine upper
        // bits.  But a SUBBYTES that narrows an UNRELATED value (a temp, or a
        // different register) into a 32-bit GP register is a real zero-
        // extending write — e.g. the low word of a 64-bit MUL product stored
        // to EAX must clear RAX[63:32].  Only skip the self low-slice form.
        bool SelfLowSlice = MOp.NumInputs >= 1 &&
                            MOp.Inputs[0].Kind == MedVar::Reg &&
                            MOp.Inputs[0].RegOff == MOp.Output.RegOff &&
                            MOp.Inputs[0].Size >= MOp.Output.Size &&
                            (MOp.NumInputs < 2 || !MOp.Inputs[1].isConst() ||
                             MOp.Inputs[1].ConstVal == 0);
        if (SelfLowSlice)
          continue;
      }
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
      MB.Ops.insert(MB.Ops.begin() + static_cast<long>(OI + 1), Zext);
      ++OI;
    }
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
          for (const auto &[Key, WI] : AllWrites)
            if (Key.first == Inp.RegOff && Key.second > Inp.Size)
              consider(Key.first, Key.second);
          for (const auto &[Key, WI] : ZextWrites)
            if (Key.first == Inp.RegOff && Key.second > Inp.Size)
              consider(Key.first, Key.second);
          // (b) SubRegs table (mid-register lanes whose start offset differs).
          for (const auto &E : TRI.SubRegs)
            if (E.NarrowRegOff == Inp.RegOff && E.NarrowSize == Inp.Size)
              consider(E.WideRegOff, static_cast<uint16_t>(E.WideSize));
          if (HaveCandidate && tryRecord(BestOff, BestSz))
            Found = true;
        }

        if (!Found) {
          uint64_t BestOff = 0;
          uint16_t BestSz = 0;
          size_t BestOrd = 0;
          bool HaveCandidate = false;
          for (const auto &[Key, WI] : ZextWrites) {
            if (Key.first == Inp.RegOff && Key.second > Inp.Size) {
              if (!HaveCandidate || WI.Ord > BestOrd) {
                BestOff = Key.first;
                BestSz = Key.second;
                BestOrd = WI.Ord;
                HaveCandidate = true;
              }
            }
          }
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
        auto DiscardClobbered = [&](RegWriteMap &Writes) {
          for (auto It = Writes.begin(); It != Writes.end();) {
            uint64_t RegOff = It->first.first;
            uint16_t Size = It->first.second;
            if (!TRI.isFrameOrLinkReg(RegOff) &&
                !TRI.isCallPreserved(RegOff, Size))
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

    // Apply pending insertions in reverse order to preserve indices.
    for (auto It = Pending.rbegin(); It != Pending.rend(); ++It)
      MB.Ops.insert(MB.Ops.begin() + static_cast<long>(It->InsertBefore),
                    std::move(It->Op));
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
  {
    std::set<std::pair<uint64_t, uint16_t>> ReadInOtherBlock;
    for (size_t BI = 0; BI < Func.Blocks.size(); ++BI) {
      auto &B = Func.Blocks[BI];
      std::set<int> LocalDefs;
      for (auto &Op : B.Ops) {
        for (uint8_t I2 = 0; I2 < Op.NumInputs; ++I2) {
          auto &Inp = Op.Inputs[I2];
          if (Inp.Kind == MedVar::Reg && Inp.Size > 0 &&
              !LocalDefs.count(Inp.Id))
            ReadInOtherBlock.insert({Inp.RegOff, Inp.Size});
        }
        if (Op.Output.Kind == MedVar::Reg && Op.Output.Size > 0)
          LocalDefs.insert(Op.Output.Id);
      }
    }

    for (auto &MB : Func.Blocks) {
      // Find the LAST write index for each (RegOff, Size) pair, excluding
      // SUBBYTES (a sub-register extract) and INT_ZEXT (an implicit
      // zero-extension synced by Phase A; its real value lives in the
      // narrower write it extends).
      std::map<std::pair<uint64_t, uint16_t>, size_t> LastWide;
      for (size_t OI = 0; OI < MB.Ops.size(); ++OI) {
        auto &MOp = MB.Ops[OI];
        if (MOp.Output.Kind == MedVar::Reg && MOp.Output.Size > 0 &&
            MOp.Opcode != NdOp::SUBBYTES && MOp.Opcode != NdOp::INT_ZEXT)
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
      for (const auto &NKey : ReadInOtherBlock) {
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
      for (auto It = NPending.rbegin(); It != NPending.rend(); ++It)
        MB.Ops.insert(MB.Ops.begin() + static_cast<long>(It->InsertAfter),
                      std::move(It->Op));
    }
  }

  // Phase C2 (ARM/AArch64): synthesize Q = CONCAT(D_high, D_low) after the last
  // D-half write so cross-block wide reads observe the D values.  Runs after
  // the generic Phase C above.  See LowToMedARM.cpp.
  synthesizeWideVectorWritesARM(Func);
}

} // namespace neverd
