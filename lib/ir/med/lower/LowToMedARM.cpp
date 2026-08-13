//===- LowToMedARM.cpp - ARM/AArch64 sub-register fixups --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// ARM/AArch64-specific sub-register repair passes for the LowIR→MedIR
/// conversion (NEON D/Q half-register reconstruction).  The architecture-
/// generic framework lives in LowToMedSubReg.cpp and dispatches to the
/// functions here by target arch (LLVM target-dispatch pattern).
///
//===----------------------------------------------------------------------===//

#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/med/LowToMed.h"

#include <algorithm>
#include <functional>
#include <map>
#include <set>
#include <vector>

namespace neverd {

// ARM/AArch64 counterpart to mergeLoopCarriedPartialReads.  On NEON, a 128-bit
// Q register (Q9 = D18:D19) loaded inside a loop via `vld1 {d18,d19}` writes
// its two 64-bit halves (D18/D19) as *separate* register Ids; buildSsa
// therefore creates loop phis for D18 and D19 but NOT for the wide Q register
// (a distinct Id that is only ever *read* in the loop).  A full-Q read at the
// loop top — e.g. the previous window in the adjacent-difference idiom
//
//   loop: vld1.32 {d20,d21},[r2]!   ; q10 = next window
//         vext.32 q11, q9, q10, #3  ; reads q9 = {d18,d19} (loop-carried)
//         vld1.32 {d18,d19},[r2]    ; q9 = next-next window (back-edge value)
//
// then resolves to the loop-INVARIANT preamble value of q9 instead of a phi, so
// every iteration's boundary element is the entry value.  Reconstruct the wide
// read from the loop-carried half phis: q = CONCAT(d_high_phi, d_low_phi).
//
// Guards keep this surgical: only fires when (a) the wide register itself is
// never written/phi'd in the block (so the read is genuinely the stale preamble
// value), and (b) the loop-carried sub-register phis *exactly tile* the wide
// read (no gaps/overlap) — i.e. all halves are loop-carried.
void LowToMedConverter::mergeLoopCarriedVectorReads(MedFunc &Func) {
  if (TargetArch != Arch::ARM && TargetArch != Arch::AArch64)
    return;

  const auto &TRI = getTargetRegInfo(TargetArch);

  for (auto &MB : Func.Blocks) {
    struct SubPhiInfo {
      int Id;
      int SSAVer;
      bool Current;
    };
    // Sub-register phis keyed by (RegOff, Size), plus all phi register Ids.
    std::map<std::pair<uint64_t, uint16_t>, SubPhiInfo> SubPhis;
    std::set<int> PhiIds;
    for (const auto &Phi : MB.Phis)
      if (Phi.Output.Kind == MedVar::Reg && Phi.Output.Size > 0) {
        PhiIds.insert(Phi.Output.Id);
        SubPhis[{Phi.Output.RegOff, Phi.Output.Size}] = {
            Phi.Output.Id, Phi.Output.SSAVer, true};
      }
    if (SubPhis.empty())
      continue;

    // Register Ids written as an op output in this block: a wide register that
    // is assigned in-block already reads its correct SSA value and must not be
    // reconstructed.
    std::set<int> WrittenIds;
    for (const auto &Op : MB.Ops)
      if (Op.Output.Kind == MedVar::Reg && Op.Output.Size > 0)
        WrittenIds.insert(Op.Output.Id);

    struct MergeInsert {
      size_t InsertBefore;
      std::vector<MedOp> Seq;
    };
    std::vector<MergeInsert> Pending;

    for (size_t OI = 0; OI < MB.Ops.size(); ++OI) {
      auto &MOp = MB.Ops[OI];
      std::map<std::pair<uint64_t, uint16_t>, MedVar> Recon;

      for (uint8_t I = 0; I < MOp.NumInputs; ++I) {
        MedVar &Inp = MOp.Inputs[I];
        // Only wide vector reads (> 8 bytes, i.e. Q/V) need half
        // reconstruction.
        if (Inp.Kind != MedVar::Reg || Inp.Size <= 8)
          continue;
        // The wide register must be neither phi'd nor written in this block —
        // otherwise its SSA value is authoritative.
        if (PhiIds.count(Inp.Id) || WrittenIds.count(Inp.Id))
          continue;

        uint64_t WideOff = Inp.RegOff;
        uint16_t WideSz = Inp.Size;

        struct Part {
          uint16_t Sz;
          int Id;
          int SSAVer;
          uint64_t Off;
          int ByteOff;
        };
        std::vector<Part> Parts;
        for (const auto &[Key, PI] : SubPhis) {
          if (!PI.Current || Key.second >= WideSz)
            continue;
          int ByteOff =
              TRI.subRegByteOffset(Key.first, Key.second, WideOff, WideSz);
          if (ByteOff < 0)
            continue;
          Parts.push_back({Key.second, PI.Id, PI.SSAVer, Key.first, ByteOff});
        }
        if (Parts.empty())
          continue;
        // Require the loop-carried halves to tile [0, WideSz) exactly.
        std::sort(Parts.begin(), Parts.end(), [](const Part &A, const Part &B) {
          return A.ByteOff < B.ByteOff;
        });
        int Cover = 0;
        bool Tiles = true;
        for (const auto &P : Parts) {
          if (P.ByteOff != Cover) {
            Tiles = false;
            break;
          }
          Cover += P.Sz;
        }
        if (!Tiles || Cover != static_cast<int>(WideSz))
          continue;

        auto CacheIt = Recon.find({WideOff, WideSz});
        if (CacheIt != Recon.end()) {
          MOp.Inputs[I] = CacheIt->second;
          continue;
        }

        // Reconstruct low->high via CONCAT(out, HIGH, LOW).
        auto mkPart = [&](const Part &P) {
          MedVar V;
          V.Kind = MedVar::Reg;
          V.Id = P.Id;
          V.SSAVer = P.SSAVer;
          V.Size = P.Sz;
          V.RegOff = P.Off;
          V.TheArch = TargetArch;
          return V;
        };
        std::vector<MedOp> Ops;
        MedVar Acc = mkPart(Parts[0]);
        uint16_t AccSz = Parts[0].Sz;
        for (size_t K = 1; K < Parts.size(); ++K) {
          MedVar Hi = mkPart(Parts[K]);
          MedVar Out;
          Out.Kind = MedVar::Temp;
          Out.Id = allocVarId();
          Out.Size = static_cast<uint16_t>(AccSz + Parts[K].Sz);
          Out.TheArch = TargetArch;
          MedOp Pc;
          Pc.Opcode = NdOp::CONCAT;
          Pc.Addr = MOp.Addr;
          Pc.Output = Out;
          Pc.addInput(Hi);  // high half
          Pc.addInput(Acc); // low half
          Ops.push_back(Pc);
          Acc = Out;
          AccSz = static_cast<uint16_t>(AccSz + Parts[K].Sz);
        }

        Recon[{WideOff, WideSz}] = Acc;
        MOp.Inputs[I] = Acc;
        Pending.push_back({OI, std::move(Ops)});
      }

      // A write overlapping a sub-register phi's region ends its reign.
      if (MOp.Output.Kind == MedVar::Reg && MOp.Output.Size > 0) {
        uint64_t WLo = MOp.Output.RegOff;
        uint64_t WHi = WLo + MOp.Output.Size;
        for (auto &[Key, PI] : SubPhis) {
          uint64_t NLo = Key.first;
          uint64_t NHi = NLo + Key.second;
          if (NLo < WHi && WLo < NHi)
            PI.Current = false;
        }
      }
    }

    for (auto It = Pending.rbegin(); It != Pending.rend(); ++It)
      MB.Ops.insert(MB.Ops.begin() + static_cast<long>(It->InsertBefore),
                    It->Seq.begin(), It->Seq.end());
  }
}

// Phase B3: wide (Q) read ← newer narrow (D) sub-register writes.
// A 16-byte NEON Q register is frequently written only through its two
// 8-byte D halves (`vld1 {dN,dN+1}`, `vmovn dN`, `vorr dN+1,dN,dN`, ...).  A
// later *full-width* read of the Q register (e.g. `veor q,q,q`) must
// reconstruct from the most-recent write to each half.  fixupPartialWritesX86
// cannot express this (its 64-bit masks overflow at 16 bytes), so rebuild with
// SUBBYTES/CONCAT.  Only fires when a narrow half was written *more recently*
// than the wide register itself.
void LowToMedConverter::mergeWideVectorReadsARM(MedFunc &Func) {
  if (TargetArch != Arch::ARM && TargetArch != Arch::AArch64)
    return;

  const auto &TRI = getTargetRegInfo(TargetArch);

  for (auto &MB : Func.Blocks) {
    struct WInfo {
      int Id = -1;
      size_t Ord = 0;
      bool Derived = false;
    };
    std::map<std::pair<uint64_t, uint16_t>, WInfo> LastW;
    size_t Seq = 1;
    for (const auto &Phi : MB.Phis)
      if (Phi.Output.Kind == MedVar::Reg && Phi.Output.Size > 0)
        LastW[{Phi.Output.RegOff, Phi.Output.Size}] = {Phi.Output.Id, Seq++,
                                                       false};

    struct Ins {
      size_t Before;
      std::vector<MedOp> Ops;
    };
    std::vector<Ins> Pending3;

    for (size_t OI = 0; OI < MB.Ops.size(); ++OI) {
      auto &MOp = MB.Ops[OI];
      std::map<std::pair<uint64_t, uint16_t>, MedVar> Merged;
      for (uint8_t I = 0; I < MOp.NumInputs; ++I) {
        const MedVar &Inp = MOp.Inputs[I];
        if (Inp.Kind != MedVar::Reg || Inp.Size <= 8)
          continue;
        uint64_t WideOff = Inp.RegOff;
        uint16_t WideSz = Inp.Size;
        size_t WideOrd = 0;
        {
          auto WIt = LastW.find({WideOff, WideSz});
          if (WIt != LastW.end())
            WideOrd = WIt->second.Ord;
        }
        // True when any strictly-finer sub-register of [Off,Sz) holds a write
        // newer than BaseOrd.  Recurses Q->D->S so a wide read rebuilt from
        // S-lane writes (a vector built from scalars: `vmov.f32 sN,...`, not
        // just from two D halves) is detected, not only the two-D-half case.
        std::function<bool(uint64_t, uint16_t, size_t)> anyDescNewer =
            [&](uint64_t Off, uint16_t Sz, size_t BaseOrd) -> bool {
          for (const auto &E : TRI.SubRegs) {
            if (E.WideRegOff != Off || E.WideSize != Sz || E.NarrowSize >= Sz)
              continue;
            auto NIt = LastW.find({E.NarrowRegOff, E.NarrowSize});
            if (NIt != LastW.end() && !NIt->second.Derived &&
                NIt->second.Ord > BaseOrd)
              return true;
            if (anyDescNewer(E.NarrowRegOff, E.NarrowSize, BaseOrd))
              return true;
          }
          return false;
        };

        if (!anyDescNewer(WideOff, WideSz, WideOrd))
          continue;

        auto CacheIt = Merged.find({WideOff, WideSz});
        if (CacheIt != Merged.end()) {
          MOp.Inputs[I] = CacheIt->second;
          continue;
        }

        // Reconstruct region [Off,Sz): split into the two table halves whenever
        // a finer write is newer than the best whole-region cover
        // (CoverSrc@Ord), otherwise take the region's own direct write or
        // SUBBYTES it out of the covering value.  CoverSrc is the coarsest
        // register that supplies bytes we are not overriding (the wide read at
        // the top, an intermediate D once chosen).  The split test runs
        // *before* the "no cover" bail so a Q with no whole-Q write (built only
        // from S lanes/D halves) still rebuilds.
        bool Ok = true;
        std::vector<MedOp> Ops;
        std::function<MedVar(uint64_t, uint16_t, size_t, MedVar)> build =
            [&](uint64_t Off, uint16_t Sz, size_t CoverOrd,
                MedVar CoverSrc) -> MedVar {
          size_t LocalOrd = 0;
          int LocalId = -1;
          bool LocalDerived = false;
          if (auto It = LastW.find({Off, Sz}); It != LastW.end()) {
            LocalOrd = It->second.Ord;
            LocalId = It->second.Id;
            LocalDerived = It->second.Derived;
          }
          size_t BestOrd = CoverOrd;
          MedVar BestSrc = CoverSrc;
          if (LocalId >= 0 && !LocalDerived && LocalOrd > CoverOrd) {
            BestOrd = LocalOrd;
            BestSrc.Kind = MedVar::Reg;
            BestSrc.Id = LocalId;
            BestSrc.Size = Sz;
            BestSrc.RegOff = Off;
            BestSrc.TheArch = TargetArch;
          }

          std::vector<std::pair<uint64_t, uint16_t>> Halves; // (off, byteOff)
          if (Sz / 2 >= 4)
            for (const auto &E : TRI.SubRegs)
              if (E.WideRegOff == Off && E.WideSize == Sz &&
                  E.NarrowSize == Sz / 2)
                Halves.push_back({E.NarrowRegOff, E.ByteOffset});

          if (Halves.size() == 2 && anyDescNewer(Off, Sz, BestOrd)) {
            std::sort(Halves.begin(), Halves.end(),
                      [](const auto &A, const auto &B) {
                        return A.second < B.second;
                      });
            MedVar Lo = build(Halves[0].first, Sz / 2, BestOrd, BestSrc);
            MedVar Hi = build(Halves[1].first, Sz / 2, BestOrd, BestSrc);
            MedVar Comb;
            Comb.Kind = MedVar::Temp;
            Comb.Id = allocVarId();
            Comb.Size = Sz;
            Comb.TheArch = TargetArch;
            MedOp PieceOp;
            PieceOp.Opcode = NdOp::CONCAT;
            PieceOp.Addr = MOp.Addr;
            PieceOp.Output = Comb;
            PieceOp.addInput(Hi); // high
            PieceOp.addInput(Lo); // low
            Ops.push_back(PieceOp);
            return Comb;
          }

          if (BestOrd == 0) {
            // No write covers this leaf directly: its bytes retain the covering
            // register's prior value, so SUBBYTES them out of CoverSrc instead
            // of abandoning the whole reconstruction.  This rebuilds a Q read
            // whose written half (e.g. `vorr d18,d17,d17`) must be merged with
            // an unwritten half carried by the incoming Q value — losing it
            // dropped a dependent `veor`/`vqdmull` chain (returned a stale
            // lane).
            if (BestSrc.Size == 0 || BestSrc.Size < Sz ||
                BestSrc.RegOff > Off ||
                Off + Sz > BestSrc.RegOff + BestSrc.Size) {
              Ok = false; // cover cannot supply these bytes: cannot rebuild
              return Inp;
            }
          }
          if (BestSrc.RegOff == Off && BestSrc.Size == Sz)
            return BestSrc;
          MedVar Sub;
          Sub.Kind = MedVar::Temp;
          Sub.Id = allocVarId();
          Sub.Size = Sz;
          Sub.TheArch = TargetArch;
          MedOp S;
          S.Opcode = NdOp::SUBBYTES;
          S.Addr = MOp.Addr;
          S.Output = Sub;
          S.addInput(BestSrc);
          S.addInput(MedVar::makeConst(Off - BestSrc.RegOff, 4));
          Ops.push_back(S);
          return Sub;
        };

        MedVar Acc = build(WideOff, WideSz, WideOrd, Inp);
        if (!Ok)
          continue;
        Merged[{WideOff, WideSz}] = Acc;
        MOp.Inputs[I] = Acc;
        Pending3.push_back({OI, std::move(Ops)});
      }

      if (MOp.Output.Kind == MedVar::Reg && MOp.Output.Size > 0) {
        bool IsSub = MOp.Opcode == NdOp::SUBBYTES;
        LastW[{MOp.Output.RegOff, MOp.Output.Size}] = {MOp.Output.Id, Seq++,
                                                       IsSub};
      }
    }

    for (auto It = Pending3.rbegin(); It != Pending3.rend(); ++It)
      MB.Ops.insert(MB.Ops.begin() + static_cast<long>(It->Before),
                    It->Ops.begin(), It->Ops.end());
  }
}

// Phase C2: Narrow sub-register writes → wide register CONCAT write
// (cross-block wide read).  The complement of the generic Phase C.
//
// A NEON Q register is frequently updated only through its two 8-byte D
// halves — e.g. `vld1 {d0,d1}` writes D0 and D1 as *separate* register Ids
// and never the parent Q0.  buildSsa keys reaching definitions by Id, so such
// D-half writes do NOT update Q0's reaching def: a later *cross-block*
// full-width read of Q0 (e.g. `vmul q0,q8,q0` after an unconditional branch)
// then resolves to a STALE earlier full-Q write (an `vcvt q0,q0`) instead of
// the D-half loads.  mergeWideVectorReadsARM (Phase B3) repairs this within a
// single block; here we cover the cross-block case by synthesizing
// `Q0 = CONCAT(D_high, D_low)` after the last D-half write, so the Q register
// carries the D values forward through its own SSA def (VectorAlgo8 arm32
// fmla/fdiv).
//
// ARM32 only in practice: its Q has two D halves at *different* register
// offsets (D(2N) @0, D(2N+1) @8).  AArch64 maps Dn/Sn/Hn to the *same* offset
// as Vn, so only the low half exists in the table and this never fires.
void LowToMedConverter::synthesizeWideVectorWritesARM(MedFunc &Func) {
  if (TargetArch != Arch::ARM && TargetArch != Arch::AArch64)
    return;

  const auto &TRI = getTargetRegInfo(TargetArch);

  // Wide (>8 byte) registers read in a block where they are not locally
  // defined first — i.e. read across a block boundary.
  std::set<std::pair<uint64_t, uint16_t>> WideReadCrossBlock;
  for (auto &B : Func.Blocks) {
    std::set<int> LocalDefs;
    for (auto &Op : B.Ops) {
      for (uint8_t I2 = 0; I2 < Op.NumInputs; ++I2) {
        const auto &Inp = Op.Inputs[I2];
        if (Inp.Kind == MedVar::Reg && Inp.Size > 8 && !LocalDefs.count(Inp.Id))
          WideReadCrossBlock.insert({Inp.RegOff, Inp.Size});
      }
      if (Op.Output.Kind == MedVar::Reg && Op.Output.Size > 0)
        LocalDefs.insert(Op.Output.Id);
    }
  }

  for (auto &MB : Func.Blocks) {
    struct WideSynth {
      size_t InsertAfter;
      std::vector<MedOp> Ops;
    };
    std::vector<WideSynth> WPending;

    for (const auto &WKey : WideReadCrossBlock) {
      uint64_t WideOff = WKey.first;
      uint16_t WideSz = WKey.second;
      auto WideIt = RegVarMap.find(WKey);
      if (WideIt == RegVarMap.end())
        continue;
      int WideId = WideIt->second;

      // The two equal-size D halves that exactly tile this Q (offsets 0/8).
      struct HalfDef {
        uint64_t Off;
        uint16_t Sz;
        uint16_t ByteOff;
      };
      std::vector<HalfDef> Halves;
      for (const auto &E : TRI.SubRegs) {
        if (E.WideRegOff != WideOff || E.WideSize != WideSz)
          continue;
        if (E.NarrowSize != WideSz / 2) // only the two halves
          continue;
        Halves.push_back({E.NarrowRegOff, E.NarrowSize, E.ByteOffset});
      }
      if (Halves.size() != 2)
        continue;
      std::sort(Halves.begin(), Halves.end(),
                [](const HalfDef &A, const HalfDef &B) {
                  return A.ByteOff < B.ByteOff;
                });

      // Scan: last full-Q write index, and last write index + Id per half.
      size_t LastWide = MB.Ops.size();
      size_t LastHalf[2] = {MB.Ops.size(), MB.Ops.size()};
      int HalfWId[2] = {-1, -1};
      for (size_t OI = 0; OI < MB.Ops.size(); ++OI) {
        const auto &O = MB.Ops[OI];
        if (O.Output.Kind != MedVar::Reg || O.Output.Size == 0)
          continue;
        if (O.Output.RegOff == WideOff && O.Output.Size == WideSz)
          LastWide = OI;
        for (int H = 0; H < 2; ++H)
          if (O.Output.RegOff == Halves[H].Off &&
              O.Output.Size == Halves[H].Sz) {
            LastHalf[H] = OI;
            HalfWId[H] = O.Output.Id;
          }
      }

      bool Has[2] = {LastHalf[0] != MB.Ops.size(),
                     LastHalf[1] != MB.Ops.size()};
      if (!Has[0] && !Has[1])
        continue;

      auto mkHalfReg = [&](int H) {
        MedVar V;
        V.Kind = MedVar::Reg;
        V.Id = HalfWId[H];
        V.Size = Halves[H].Sz;
        V.RegOff = Halves[H].Off;
        V.TheArch = TargetArch;
        return V;
      };

      // Resolve each D half independently against the last full-Q write.  A
      // half is "fresh" when its own write is newer than that full-Q write (or
      // there is none): it is the authoritative value for that half.  A stale
      // half (older than the full-Q write, or never written) keeps the full-Q
      // bits, taken via SUBBYTES of that wide value.  Treating the halves
      // separately covers the mixed case where one half is written after a
      // full-Q write and the other before it — e.g. `vand q2,q7,q2` then
      // `vshr.u16 d5`: d5 must override the wide value while d4 is carried from
      // it (a later cross-block `vmovn q2` otherwise reads a stale high half).
      // The all-fresh case is the pure two-D-half write (`vld1 {d0,d1}`); the
      // single-fresh case is the partial update (`vdup q,#0; vmov.32
      // d[lane],r`, armm14_rle seed).
      bool Fresh[2];
      for (int H = 0; H < 2; ++H)
        Fresh[H] =
            Has[H] && (LastWide == MB.Ops.size() || LastHalf[H] > LastWide);

      // No fresher D write than the full-Q value: it already supplies both
      // halves.
      if (!Fresh[0] && !Fresh[1])
        continue;
      // A stale half must be sourced from a full-Q value; require one to exist.
      if ((!Fresh[0] || !Fresh[1]) && LastWide == MB.Ops.size())
        continue;

      std::vector<MedOp> Seq;
      size_t InsertAfter = (LastWide == MB.Ops.size()) ? 0 : LastWide;
      for (int H = 0; H < 2; ++H)
        if (Fresh[H])
          InsertAfter = std::max(InsertAfter, LastHalf[H]);

      MedVar WideSrc;
      WideSrc.Kind = MedVar::Reg;
      WideSrc.Id = WideId;
      WideSrc.Size = WideSz;
      WideSrc.RegOff = WideOff;
      WideSrc.TheArch = TargetArch;

      MedVar Half[2];
      for (int H = 0; H < 2; ++H) {
        if (Fresh[H]) {
          Half[H] = mkHalfReg(H);
          continue;
        }
        MedVar Carried;
        Carried.Kind = MedVar::Temp;
        Carried.Id = allocVarId();
        Carried.Size = Halves[H].Sz;
        Carried.TheArch = TargetArch;
        MedOp Sub;
        Sub.Opcode = NdOp::SUBBYTES;
        Sub.Addr = MB.Ops[InsertAfter].Addr;
        Sub.Output = Carried;
        Sub.addInput(WideSrc);
        Sub.addInput(MedVar::makeConst(Halves[H].ByteOff, 4));
        Seq.push_back(std::move(Sub));
        Half[H] = Carried;
      }
      MedVar Lo = Half[0]; // CONCAT(high, low)
      MedVar Hi = Half[1];

      MedOp Pc;
      Pc.Opcode = NdOp::CONCAT;
      Pc.Addr = MB.Ops[InsertAfter].Addr;
      Pc.Output.Kind = MedVar::Reg;
      Pc.Output.Id = WideId;
      Pc.Output.Size = WideSz;
      Pc.Output.RegOff = WideOff;
      Pc.Output.TheArch = TargetArch;
      Pc.addInput(Hi); // high half
      Pc.addInput(Lo); // low half
      Seq.push_back(std::move(Pc));
      WPending.push_back({InsertAfter, std::move(Seq)});
    }

    std::sort(WPending.begin(), WPending.end(),
              [](const WideSynth &A, const WideSynth &B) {
                return A.InsertAfter < B.InsertAfter;
              });
    for (auto It = WPending.rbegin(); It != WPending.rend(); ++It)
      MB.Ops.insert(MB.Ops.begin() + static_cast<long>(It->InsertAfter + 1),
                    It->Ops.begin(), It->Ops.end());
  }
}

// SUBBYTES redirect (called from the generic Phase B loop in
// LowToMedSubReg.cpp):
// a `SUBBYTES(Q, off)` whose extracted bytes fall entirely within a narrower
// D/S sub-register that was written *more recently* than Q reads that narrower
// register directly.  E.g. SUBBYTES(Q8, 0, sz=2) → SUBBYTES(D16, 0, sz=2);
// SUBBYTES(D3, 0, sz=4) → S6 (the `vdup.32 d3,d3[0]` lane read of a just
// written `vcvt s6` copysign source; armfa_sign).  Returns true when the op
// was rewritten.
bool LowToMedConverter::redirectWideSubpieceToNarrowARM(
    MedBlock &MB, size_t OI, const RegWriteMap &Writes) {
  if (TargetArch != Arch::ARM)
    return false;

  MedOp &MOp = MB.Ops[OI];
  if (MOp.Opcode != NdOp::SUBBYTES || MOp.NumInputs < 2)
    return false;
  const MedVar &Inp = MOp.Inputs[0];
  if (Inp.Kind != MedVar::Reg || Inp.Size < 8 || !MOp.Inputs[1].isConst())
    return false;

  const auto &TRI = getTargetRegInfo(TargetArch);

  uint64_t ExtractOff = MOp.Inputs[1].ConstVal;
  uint16_t ExtractSz = MOp.Output.Size;
  // A 16-byte Q read redirects only to an 8-byte D half (never an S lane —
  // see below); an 8-byte D read redirects to a 4-byte S lane, its only
  // sub-register.
  uint16_t WantNarrow = Inp.Size > 8 ? 8 : 4;
  // Recency of the wide register's own most-recent write.  We must only
  // redirect to a narrow sub-register when that sub-register was written
  // *more recently* than the wide register; otherwise a newer wide write
  // (e.g. a Q-register vmul/veor result) would be shadowed by a stale
  // D-register value (e.g. an earlier vld1 of a constant).
  size_t WideOrd = 0;
  bool HaveWide = false;
  {
    auto WIt = Writes.find(std::make_pair(Inp.RegOff, Inp.Size));
    if (WIt != Writes.end()) {
      WideOrd = WIt->second.Ord;
      HaveWide = true;
    }
  }
  for (const auto &E : TRI.SubRegs) {
    if (E.WideRegOff != Inp.RegOff || E.WideSize != Inp.Size)
      continue;
    if (E.NarrowSize >= Inp.Size)
      continue;
    // For a Q read, redirect only to a D HALF (8 bytes), never an S lane
    // (4 bytes): an S sub-register can alias a stale vcvt float result of the
    // SAME reused Q, whereas the D half carries the fresh vld1 coefficient
    // (VectorAlgo8 arm32 fmla/fdiv).  A D read has no such ambiguity — its
    // only sub-registers are the two S lanes — so the recency guard alone
    // keeps the redirect correct.
    if (E.NarrowSize != WantNarrow)
      continue;
    unsigned SubStart = static_cast<unsigned>(E.ByteOffset);
    unsigned SubEnd = SubStart + E.NarrowSize;
    if (ExtractOff >= SubStart && ExtractOff + ExtractSz <= SubEnd) {
      auto NKey = std::make_pair(E.NarrowRegOff, E.NarrowSize);
      auto NIt = Writes.find(NKey);
      if (NIt != Writes.end() && (!HaveWide || NIt->second.Ord > WideOrd)) {
        // A Q→D-half redirect must not hide a sibling S lane written *after*
        // the D half: `vmov.32 dN[0],r` is a whole-D CONCAT write whose high
        // half a following `vcvt sM` supersedes.  Leave it as SUBBYTES(Q) so
        // Phase B3 rebuilds the Q from each lane's newest write.  (Reverse
        // order — a stale vcvt S then a fresh vld1 whole-D — keeps S older,
        // so this stays off and the D-half redirect proceeds: VectorAlgo8.)
        if (Inp.Size > 8) {
          bool NewerLane = false;
          for (const auto &SE : TRI.SubRegs) {
            if (SE.WideRegOff != E.NarrowRegOff ||
                SE.WideSize != E.NarrowSize || SE.NarrowSize >= E.NarrowSize)
              continue;
            unsigned LaneStart =
                SubStart + static_cast<unsigned>(SE.ByteOffset);
            if (ExtractOff < LaneStart ||
                ExtractOff + ExtractSz > LaneStart + SE.NarrowSize)
              continue;
            auto LIt =
                Writes.find(std::make_pair(SE.NarrowRegOff, SE.NarrowSize));
            if (LIt != Writes.end() && LIt->second.Ord > NIt->second.Ord) {
              NewerLane = true;
              break;
            }
          }
          if (NewerLane)
            return false;
        }
        MOp.Inputs[0].RegOff = E.NarrowRegOff;
        MOp.Inputs[0].Size = E.NarrowSize;
        MOp.Inputs[0].Id = NIt->second.Id;
        MOp.Inputs[1] = MedVar::makeConst(ExtractOff - SubStart, 4);
        return true;
      }
    }
  }
  return false;
}

} // namespace neverd
