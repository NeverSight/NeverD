//===- LowToMedX86.cpp - x86 sub-register fixups for LowIR→MedIR -------===//
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

#include <algorithm>
#include <map>
#include <set>
#include <vector>

namespace neverd {

// Post-SSA: reconstruct wide reads that overlap a *loop-carried* narrow
// sub-register phi (x86/x64 only).
//
// fixupSubRegisters' Phase B2 already merges a narrow partial write into a
// subsequent wider read within straight-line code, and it tries to seed PHI
// outputs so the same works across loop back-edges.  But fixupSubRegisters runs
// *before* buildSsa, so at that point no phis exist and the seeding is dead.
// The classic victim is a byte loop such as the per-byte popcount idiom:
//
//   loop:  movl %edi, %ecx     ; reads the 32-bit parent (b & 1)
//          andb $1, %cl
//          shrb %dil           ; partial 8-bit write — loop-carried via a phi
//          jne loop
//
// Here `b` lives in DIL and is shifted each iteration; the wide read of EDI at
// the loop top must observe {stale-upper | zext(DIL_phi)}, not the loop-
// invariant entry value.  Pre-SSA Phase B2 cannot see the cross-iteration DIL
// write, so the read wrongly resolves to the entry register (e.g. `arg0 & 1`),
// counting only the original low bit forever.
//
// Run after buildSsa, when the DIL phi exists.  Straight-line partial reads
// were already rewritten to temps by pre-SSA Phase B2, so they are skipped here
// (Inp.Kind != Reg) and never double-merged.
void LowToMedConverter::mergeLoopCarriedPartialReads(MedFunc &Func) {
  if (TargetArch != Arch::X64 && TargetArch != Arch::X86)
    return;

  const auto &TRI = getTargetRegInfo(TargetArch);

  for (auto &MB : Func.Blocks) {
    // Narrow (<8 byte) sub-register phi outputs carry partial values across the
    // loop back-edge.  `Current` drops once a later op overwrites the region,
    // so reads after an in-block redefinition keep their own (SSA) value.
    struct PhiInfo {
      int Id;
      int SSAVer;
      bool Current;
    };
    std::map<std::pair<uint64_t, uint16_t>, PhiInfo> NarrowPhis;
    // All phi register regions, used to detect when a *wider* register is
    // itself loop-carried (has its own phi).  In that case its phi is the
    // authoritative value and a narrow sub-register phi must NOT be merged into
    // reads of it — doing so corrupts accumulators whose low byte happens to
    // also get an independent SSA phi (interleave_bits/crc8: `result` is a
    // 64-bit phi while its AL low byte gets a separate i8 phi).
    std::vector<std::pair<uint64_t, uint16_t>> AllPhiRegions;
    for (const auto &Phi : MB.Phis)
      if (Phi.Output.Kind == MedVar::Reg && Phi.Output.Size > 0)
        AllPhiRegions.push_back({Phi.Output.RegOff, Phi.Output.Size});
    for (const auto &Phi : MB.Phis)
      // Only 8-bit and 16-bit sub-register writes are *partial* on x86-64
      // (they preserve the parent's upper bits).  A 32-bit write zero-extends
      // the full 64-bit register, so a 32-bit phi must NOT be merged as a
      // partial value into a 64-bit read — that case is a plain zext handled by
      // the wide-register phi / Phase A.  Merging it here corrupted loops whose
      // counter lives in a 32-bit sub-register (reverse_bits/interleave_bits…).
      if (Phi.Output.Kind == MedVar::Reg &&
          (Phi.Output.Size == 1 || Phi.Output.Size == 2))
        NarrowPhis[{Phi.Output.RegOff, Phi.Output.Size}] = {
            Phi.Output.Id, Phi.Output.SSAVer, true};
    if (NarrowPhis.empty())
      continue;

    struct MergeInsert {
      size_t InsertBefore;
      std::vector<MedOp> Seq;
    };
    std::vector<MergeInsert> Pending;

    for (size_t OI = 0; OI < MB.Ops.size(); ++OI) {
      auto &MOp = MB.Ops[OI];
      std::map<std::pair<uint64_t, uint16_t>, MedVar> Merged;

      for (uint8_t I = 0; I < MOp.NumInputs; ++I) {
        const MedVar &Inp = MOp.Inputs[I];
        if (Inp.Kind != MedVar::Reg || Inp.Size == 0 || Inp.Size > 8)
          continue;
        uint64_t WideOff = Inp.RegOff;
        uint16_t WideSz = Inp.Size;

        // If a phi covers this wide read at >= its width, the wide register is
        // itself loop-carried; its phi is authoritative.  Don't splice a narrow
        // sub-register phi into it.
        bool WideIsLoopCarried = false;
        for (const auto &PR : AllPhiRegions)
          if (PR.second >= WideSz && PR.first <= WideOff &&
              PR.first + PR.second >= WideOff + WideSz) {
            WideIsLoopCarried = true;
            break;
          }
        if (WideIsLoopCarried)
          continue;

        struct NarrowW {
          uint64_t Off;
          uint16_t Sz;
          int Id;
          int SSAVer;
          int ByteOff;
        };
        std::vector<NarrowW> Narrows;
        for (const auto &[Key, PI] : NarrowPhis) {
          if (!PI.Current || Key.second >= WideSz)
            continue;
          int ByteOff =
              TRI.subRegByteOffset(Key.first, Key.second, WideOff, WideSz);
          if (ByteOff < 0)
            continue;
          Narrows.push_back({Key.first, Key.second, PI.Id, PI.SSAVer, ByteOff});
        }
        if (Narrows.empty())
          continue;

        auto CacheIt = Merged.find({WideOff, WideSz});
        if (CacheIt != Merged.end()) {
          MOp.Inputs[I] = CacheIt->second;
          continue;
        }

        std::vector<MedOp> Ops;
        MedVar Cur = Inp;
        for (const auto &NW : Narrows) {
          uint64_t Mask = ((NW.Sz >= 8) ? ~0ULL : ((1ULL << (NW.Sz * 8)) - 1))
                          << (NW.ByteOff * 8);
          uint64_t ClearMask = ~Mask;
          if (WideSz < 8)
            ClearMask &= ((1ULL << (WideSz * 8)) - 1);

          MedVar Narrow;
          Narrow.Kind = MedVar::Reg;
          Narrow.Id = NW.Id;
          Narrow.SSAVer = NW.SSAVer;
          Narrow.Size = NW.Sz;
          Narrow.RegOff = NW.Off;
          Narrow.TheArch = TargetArch;

          MedVar Zext;
          Zext.Kind = MedVar::Temp;
          Zext.Id = allocVarId();
          Zext.Size = WideSz;
          Zext.TheArch = TargetArch;
          MedOp ZextOp;
          ZextOp.Opcode = NdOp::INT_ZEXT;
          ZextOp.Addr = MOp.Addr;
          ZextOp.Output = Zext;
          ZextOp.addInput(Narrow);
          Ops.push_back(ZextOp);

          MedVar Placed = Zext;
          if (NW.ByteOff > 0) {
            MedVar Shifted;
            Shifted.Kind = MedVar::Temp;
            Shifted.Id = allocVarId();
            Shifted.Size = WideSz;
            Shifted.TheArch = TargetArch;
            MedOp ShOp;
            ShOp.Opcode = NdOp::INT_LEFT;
            ShOp.Addr = MOp.Addr;
            ShOp.Output = Shifted;
            ShOp.addInput(Zext);
            ShOp.addInput(MedVar::makeConst(
                static_cast<uint64_t>(NW.ByteOff) * 8, WideSz));
            Ops.push_back(ShOp);
            Placed = Shifted;
          }

          MedVar Cleared;
          Cleared.Kind = MedVar::Temp;
          Cleared.Id = allocVarId();
          Cleared.Size = WideSz;
          Cleared.TheArch = TargetArch;
          MedOp AndOp;
          AndOp.Opcode = NdOp::INT_AND;
          AndOp.Addr = MOp.Addr;
          AndOp.Output = Cleared;
          AndOp.addInput(Cur);
          AndOp.addInput(MedVar::makeConst(ClearMask, WideSz));
          Ops.push_back(AndOp);

          MedVar Combined;
          Combined.Kind = MedVar::Temp;
          Combined.Id = allocVarId();
          Combined.Size = WideSz;
          Combined.TheArch = TargetArch;
          MedOp OrOp;
          OrOp.Opcode = NdOp::INT_OR;
          OrOp.Addr = MOp.Addr;
          OrOp.Output = Combined;
          OrOp.addInput(Cleared);
          OrOp.addInput(Placed);
          Ops.push_back(OrOp);

          Cur = Combined;
        }

        Merged[{WideOff, WideSz}] = Cur;
        MOp.Inputs[I] = Cur;
        Pending.push_back({OI, std::move(Ops)});
      }

      // A write overlapping a narrow phi's region ends that phi's reign: later
      // reads observe the freshly written (SSA) value instead.
      if (MOp.Output.Kind == MedVar::Reg && MOp.Output.Size > 0) {
        uint64_t WLo = MOp.Output.RegOff;
        uint64_t WHi = WLo + MOp.Output.Size;
        for (auto &[Key, PI] : NarrowPhis) {
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

// Phase B2: Narrow partial write → wider read merge (x86/x64 only).
//
// On x86/x64, writing an 8-bit or 16-bit sub-register (DL/AL/DX/AX, or the
// high-byte AH/DH/...) is a PARTIAL write: the upper bits of the parent
// register are preserved.  A subsequent read of the wider register must
// therefore see the merged value.  Example (clang -O2 crc8):
//
//   sarq %cl, %rdx          ; RDX = a >> cl                (wide write)
//   xorb %al, %dl           ; DL  = DL ^ AL                (partial write)
//   leal (%rdx,%rdx), %eax  ; EAX = 2*EDX  — must see new DL low byte!
//
// fixupSubRegisters' Phase B only handles wide-write→narrow-read.  Here we
// handle the reverse: when a wide register is read and a more-recent narrow
// sub-register write exists, reconstruct
//   merged = (wide & ~mask) | (zext(narrow) << shift)
// and rewrite the read to use `merged`.  AArch64/ARM sub-register writes
// zero-extend (table WriteZeroExtends=true / handled by Phase A), so they
// never need this and the pass is restricted to x86/x64.
void LowToMedConverter::fixupPartialWritesX86(MedFunc &Func) {
  if (TargetArch != Arch::X64 && TargetArch != Arch::X86)
    return;

  const auto &TRI = getTargetRegInfo(TargetArch);

  for (auto &MB : Func.Blocks) {
    struct WInfo {
      int Id = -1;
      size_t Ord = 0;
      bool Derived =
          false; // true for SUBBYTES outputs (not real partial writes)
    };
    std::map<std::pair<uint64_t, uint16_t>, WInfo> LastW;
    size_t Seq = 1; // 0 reserved for "before any write in block"

    // Seed PHI outputs as wide writes so loop-carried values are visible.
    for (const auto &Phi : MB.Phis)
      if (Phi.Output.Kind == MedVar::Reg && Phi.Output.Size > 0)
        LastW[{Phi.Output.RegOff, Phi.Output.Size}] = {Phi.Output.Id, Seq++,
                                                       false};

    struct MergeInsert {
      size_t InsertBefore;
      std::vector<MedOp> Seq;
    };
    std::vector<MergeInsert> Pending2;

    for (size_t OI = 0; OI < MB.Ops.size(); ++OI) {
      auto &MOp = MB.Ops[OI];

      // Per-op cache so `add rdx,rdx` (both inputs RDX) shares one merge.
      std::map<std::pair<uint64_t, uint16_t>, MedVar> Merged;

      for (uint8_t I = 0; I < MOp.NumInputs; ++I) {
        const MedVar &Inp = MOp.Inputs[I];
        if (Inp.Kind != MedVar::Reg || Inp.Size == 0)
          continue;
        uint64_t WideOff = Inp.RegOff;
        uint16_t WideSz = Inp.Size;

        // The value the read currently resolves to is the most recent write
        // at exactly (WideOff, WideSz); narrow writes after it must merge.
        size_t WideOrd = 0;
        auto WIt = LastW.find({WideOff, WideSz});
        if (WIt != LastW.end())
          WideOrd = WIt->second.Ord;

        // Collect more-recent genuine partial writes that are sub-registers.
        struct NarrowW {
          uint64_t Off;
          uint16_t Sz;
          int Id;
          int ByteOff;
          size_t Ord;
        };
        std::vector<NarrowW> Narrows;
        for (const auto &[Key, WI] : LastW) {
          if (WI.Derived || WI.Ord <= WideOrd)
            continue;
          if (Key.second >= WideSz)
            continue;
          int ByteOff =
              TRI.subRegByteOffset(Key.first, Key.second, WideOff, WideSz);
          if (ByteOff < 0)
            continue;
          Narrows.push_back({Key.first, Key.second, WI.Id, ByteOff, WI.Ord});
        }
        if (Narrows.empty())
          continue;
        // Apply oldest-first so later writes win their byte range.
        std::sort(
            Narrows.begin(), Narrows.end(),
            [](const NarrowW &A, const NarrowW &B) { return A.Ord < B.Ord; });

        auto CacheIt = Merged.find({WideOff, WideSz});
        if (CacheIt != Merged.end()) {
          MOp.Inputs[I] = CacheIt->second;
          continue;
        }

        // Build the merge sequence.
        std::vector<MedOp> Ops;
        MedVar Cur = Inp; // start from the wide value as-is
        for (const auto &NW : Narrows) {
          uint64_t Mask = ((NW.Sz >= 8) ? ~0ULL : ((1ULL << (NW.Sz * 8)) - 1))
                          << (NW.ByteOff * 8);
          uint64_t ClearMask = ~Mask;
          if (WideSz < 8)
            ClearMask &= (WideSz >= 8) ? ~0ULL : ((1ULL << (WideSz * 8)) - 1);

          MedVar Narrow;
          Narrow.Kind = MedVar::Reg;
          Narrow.Id = NW.Id;
          Narrow.Size = NW.Sz;
          Narrow.RegOff = NW.Off;
          Narrow.TheArch = TargetArch;

          // zext narrow → wide width
          MedVar Zext;
          Zext.Kind = MedVar::Temp;
          Zext.Id = allocVarId();
          Zext.Size = WideSz;
          Zext.TheArch = TargetArch;
          MedOp ZextOp;
          ZextOp.Opcode = NdOp::INT_ZEXT;
          ZextOp.Addr = MOp.Addr;
          ZextOp.Output = Zext;
          ZextOp.addInput(Narrow);
          Ops.push_back(ZextOp);

          MedVar Placed = Zext;
          if (NW.ByteOff > 0) {
            MedVar Shifted;
            Shifted.Kind = MedVar::Temp;
            Shifted.Id = allocVarId();
            Shifted.Size = WideSz;
            Shifted.TheArch = TargetArch;
            MedOp ShOp;
            ShOp.Opcode = NdOp::INT_LEFT;
            ShOp.Addr = MOp.Addr;
            ShOp.Output = Shifted;
            ShOp.addInput(Zext);
            ShOp.addInput(MedVar::makeConst(
                static_cast<uint64_t>(NW.ByteOff) * 8, WideSz));
            Ops.push_back(ShOp);
            Placed = Shifted;
          }

          // clear the target bytes of the running value
          MedVar Cleared;
          Cleared.Kind = MedVar::Temp;
          Cleared.Id = allocVarId();
          Cleared.Size = WideSz;
          Cleared.TheArch = TargetArch;
          MedOp AndOp;
          AndOp.Opcode = NdOp::INT_AND;
          AndOp.Addr = MOp.Addr;
          AndOp.Output = Cleared;
          AndOp.addInput(Cur);
          AndOp.addInput(MedVar::makeConst(ClearMask, WideSz));
          Ops.push_back(AndOp);

          // or in the placed narrow value
          MedVar Combined;
          Combined.Kind = MedVar::Temp;
          Combined.Id = allocVarId();
          Combined.Size = WideSz;
          Combined.TheArch = TargetArch;
          MedOp OrOp;
          OrOp.Opcode = NdOp::INT_OR;
          OrOp.Addr = MOp.Addr;
          OrOp.Output = Combined;
          OrOp.addInput(Cleared);
          OrOp.addInput(Placed);
          Ops.push_back(OrOp);

          Cur = Combined;
        }

        Merged[{WideOff, WideSz}] = Cur;
        MOp.Inputs[I] = Cur;
        Pending2.push_back({OI, std::move(Ops)});
      }

      // Record this op's output write (after rewriting its inputs).
      if (MOp.Output.Kind == MedVar::Reg && MOp.Output.Size > 0) {
        bool IsSub = MOp.Opcode == NdOp::SUBBYTES;
        // A SUBBYTES output is a "derived self-extract" (which must NOT merge
        // back into a wider read) only when it slices the SAME register region
        // it writes.  A SUBBYTES that narrows an UNRELATED value into a small
        // GP sub-register (size 1/2) is a genuine partial write that must merge
        // — e.g. byte DIV writes AL=quotient and AH=remainder from temporaries,
        // and a later AX read must observe both.
        bool DerivedExtract = IsSub;
        if (IsSub && MOp.Output.Size <= 2 && MOp.NumInputs >= 1) {
          const MedVar &In0 = MOp.Inputs[0];
          bool SelfSlice =
              In0.Kind == MedVar::Reg && In0.RegOff <= MOp.Output.RegOff &&
              In0.RegOff + In0.Size >= MOp.Output.RegOff + MOp.Output.Size;
          DerivedExtract = SelfSlice;
        }
        LastW[{MOp.Output.RegOff, MOp.Output.Size}] = {MOp.Output.Id, Seq++,
                                                       DerivedExtract};
      }
    }

    // Insert merge sequences in reverse op order to preserve indices.
    for (auto It = Pending2.rbegin(); It != Pending2.rend(); ++It)
      MB.Ops.insert(MB.Ops.begin() + static_cast<long>(It->InsertBefore),
                    It->Seq.begin(), It->Seq.end());
  }
}

} // namespace neverd
