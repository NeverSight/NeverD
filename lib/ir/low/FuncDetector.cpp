//===- FuncDetector.cpp - Function entry-point detection
//-------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Implements function entry-point detection via symbol tables, exports,
/// call-target scanning across executable segments, and heuristic
/// validation of candidate addresses.
///
//===----------------------------------------------------------------------===//

#include "neverd/ir/low/FuncDetector.h"

#include "neverd/Limits.h"
#include "neverd/ir/low/LowIR.h"
#include "neverd/libc/LibCNames.h"
#include "neverd/lift/AArch64Lifter.h"
#include "neverd/support/Parallel.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/BinaryFormat/MachO.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <queue>
#include <thread>

#define DEBUG_TYPE "neverd-func-detector"

namespace neverd {

namespace {

bool isUnconditionalNoReturnCall(const BinaryImage &Img,
                                 const std::vector<LowOp> &Ops) {
  va_t Target = InvalidVA;
  bool IsConditional = false;
  for (const LowOp &Op : Ops) {
    if (Op.Opcode == NdOp::COND_BR)
      IsConditional = true;
    else if (Op.Opcode == NdOp::CALL && Op.NumInputs > 0 &&
             Op.Inputs[0].isConst())
      Target = Op.Inputs[0].Offset;
  }
  return !IsConditional && libc::isNoReturnTarget(Img, Target);
}

bool hasBoundedSemanticTerminator(const BinaryImage &Img, Decoder &Dec,
                                  va_t Addr) {
  constexpr int kMaxInsns = limits::kMaxVerifyInsns;
  const Segment *Seg = Img.getSegmentFor(Addr);
  if (!Seg || !Img.hasExecutableCodeOwnerAt(Addr))
    return false;

  const bool PreviousDetail = Dec.detailEnabled();
  Dec.setDetail(true);
  Dec.resetX86FpuState();
  const bool Found = [&]() {
    va_t Cur = Addr;
    for (int I = 0; I < kMaxInsns; ++I) {
      if (Cur < Seg->VA)
        return false;
      const size_t Off = static_cast<size_t>(Cur - Seg->VA);
      if (Off >= Seg->Data.size())
        return false;

      DecodedInsn DI;
      const int Sz = Dec.decodeOneForLift(Seg->Data.data() + Off,
                                          Seg->Data.size() - Off, Cur, DI);
      if (Sz <= 0 || !DI.Raw)
        return false;
      if (!Img.hasExecutableCodeOwnerRange(Cur, static_cast<uint64_t>(Sz)))
        return false;
      if (Dec.isFunctionTerminator(DI))
        return true;

      const va_t CallTarget = Dec.directCallTarget(DI);
      if (CallTarget != InvalidVA && libc::isNoReturnTarget(Img, CallTarget)) {
        std::vector<LowOp> Ops;
        try {
          Dec.liftToLow(DI, Ops);
        } catch (const UnliftedInstruction &) {
          return false;
        }
        if (isUnconditionalNoReturnCall(Img, Ops))
          return true;
      }
      Cur += static_cast<va_t>(Sz);
    }
    return false;
  }();
  Dec.resetX86FpuState();
  Dec.setDetail(PreviousDetail);
  return Found;
}

} // namespace

//===----------------------------------------------------------------------===//
// FuncDetector::detect
//===----------------------------------------------------------------------===//

std::vector<std::pair<va_t, std::string>>
FuncDetector::detect(const BinaryImage &Img, Decoder &Dec) {
  Entries.clear();
  std::vector<std::pair<va_t, std::string>> Results;
  std::set<va_t> VerifiedCandidates;

  if (Img.Entry != 0) {
    Entries.insert(Img.Entry);
    Results.push_back({Img.Entry, Img.getFunctionNameAt(Img.Entry)});
  }

  std::set<va_t> SkipAddrs;
  std::set<va_t> UntypedCOFFExports;
  if (Img.Format == BinaryFormat::MachO && !Img.IsRelocatable) {
    for (const auto &Seg : Img.Segments)
      SkipAddrs.insert(Seg.VA);
  }

  for (const auto &Exp : Img.Exports) {
    if (Entries.count(Exp.Addr))
      continue;
    // Skip the image base marker for linked ELFs; relocatable .o files use
    // Base==0 with real functions at VA 0.
    if (Exp.Addr == Img.Base && Img.Base != 0)
      continue;
    if (SkipAddrs.count(Exp.Addr))
      continue;
    // PE exports are untyped, so they are only candidates here and are
    // decoded below before publication.  Keep the sectionless/packed-image
    // executable-owner fallback for that verification path; requiring an
    // exact section would discard legitimate stripped COFF exports before the
    // decoder can distinguish them from executable-section data.  ELF and
    // Mach-O exports, by contrast, are accepted from the export table itself
    // only when exact instruction-section metadata owns the address.
    const bool IsExportCandidate =
        Img.Format == BinaryFormat::COFF
            ? Img.hasExecutableCodeOwnerAt(Exp.Addr)
            : Img.getSectionFor(Exp.Addr) && Img.isCodeAddress(Exp.Addr);
    if (IsExportCandidate) {
      Entries.insert(Exp.Addr);
      Results.push_back({Exp.Addr, Exp.Name});
      if (Img.Format == BinaryFormat::COFF)
        UntypedCOFFExports.insert(Exp.Addr);
    }
  }

  for (const auto &Sym : Img.Symbols) {
    if (!Sym.IsFunc)
      continue;
    if (Sym.Addr == 0) {
      if (!Img.hasExecutableCodeOwnerAt(0))
        continue;
    }
    if (Entries.count(Sym.Addr))
      continue;
    if (SkipAddrs.count(Sym.Addr))
      continue;
    if (Img.hasExecutableCodeOwnerAt(Sym.Addr)) {
      Entries.insert(Sym.Addr);
      Results.push_back({Sym.Addr, Sym.Name});
    }
  }

  std::set<va_t> DirectCallTargets;
  if (Img.Entry != 0) {
    scanCallTargets(Img, Dec, DirectCallTargets);
    Entries.insert(DirectCallTargets.begin(), DirectCallTargets.end());
  }

  auto IsCoveredMachODirectCallTarget = [&](va_t Addr) {
    return Img.Format == BinaryFormat::MachO &&
           DirectCallTargets.count(Addr) != 0;
  };

  std::set<va_t> Already;
  for (auto &[A, _] : Results)
    Already.insert(A);
  for (va_t Addr : Entries) {
    if (Already.insert(Addr).second)
      Results.push_back(
          {Addr, (kAutoFuncPrefix + llvm::utohexstr(Addr)).str()});
  }

  if (Img.Entry != 0) {
    std::set<va_t> Trusted{Img.Entry};
    // PE exports are untyped: executable-section data can legally appear in
    // the export directory alongside functions.  A COFF export is therefore
    // trusted only when unwind/function-symbol metadata below also identifies
    // it as code; otherwise it must pass the same decode validation as a scan
    // hit. ELF and Mach-O exports can also name data in a coarse RX mapping, so
    // only an exact format-aware code owner is authoritative there.
    if (Img.Format != BinaryFormat::COFF)
      for (const auto &Exp : Img.Exports)
        if (Img.getSectionFor(Exp.Addr) && Img.isCodeAddress(Exp.Addr))
          Trusted.insert(Exp.Addr);
    std::set<va_t> ExplicitFunctionStarts;
    for (const auto &Sym : Img.Symbols)
      if (Sym.IsFunc)
        ExplicitFunctionStarts.insert(
            normalizeCodeAddress(Sym.Addr, Img.Arch, Img.Mode));
    for (const auto &Sym : Img.Symbols)
      if (Sym.IsFunc && Sym.Size > 0)
        Trusted.insert(Sym.Addr);

    const auto &Known = Img.KnownCodeRanges;
    auto InsideKnownButNotStart = [&](va_t A) -> bool {
      auto It = std::upper_bound(Known.begin(), Known.end(),
                                 std::make_pair(A, va_t(~va_t(0))));
      if (It == Known.begin())
        return false;
      --It;
      return A > It->first && A < It->second;
    };

    // Per-candidate keep decision.  A trusted entry (image entry, typed export,
    // sized function symbol) is kept without decoding; an ordinary scan hit
    // inside a known code range but not at its start is dropped.  Mach-O
    // direct-call targets are instead verified: compact-unwind ranges can
    // cover unsymbolized leaf callees on every supported architecture.
    // Untyped COFF exports are always verified because they can be either
    // callable aliases or data.  Only the remaining candidates need the
    // The trial decode dominates only when the scan produced many untrusted
    // candidates; each check is independent and reads only the immutable image,
    // so spread that subset across worker threads with per-thread decoders. A
    // symbol-rich binary (almost everything trusted) leaves NeedVerify small
    // and stays single-threaded, avoiding pointless thread-spawn overhead.
    const size_t N = Results.size();
    std::vector<char> Keep(N, 0);
    std::vector<size_t> NeedVerify;
    std::set<va_t> ExplicitFunctionStarts;
    for (const auto &Sym : Img.Symbols)
      if (Sym.IsFunc)
        ExplicitFunctionStarts.insert(
            normalizeCodeAddress(Sym.Addr, Img.Arch, Img.Mode));
    for (size_t I = 0; I < N; ++I) {
      va_t Addr = Results[I].first;
      if (Trusted.count(Addr))
        Keep[I] = 1;
      else if (UntypedCOFFExports.count(Addr) ||
               IsCoveredMachODirectCallTarget(Addr) ||
               ExplicitFunctionStarts.count(
                   normalizeCodeAddress(Addr, Img.Arch, Img.Mode)) != 0 ||
               !InsideKnownButNotStart(Addr))
        NeedVerify.push_back(I);
    }

    // The trial decode dominates only when the scan produced many untrusted
    // candidates; each check is independent and reads only the immutable image,
    // so spread that subset across worker threads with per-thread decoders.  A
    // symbol-rich binary (almost everything trusted) leaves NeedVerify small
    // and stays single-threaded, avoiding pointless thread-spawn overhead.
    auto verifyIdx = [&](Decoder &LocalDec, size_t I) {
      const va_t Addr = Results[I].first;
      Keep[I] = verifyFunctionDecode(Img, LocalDec, Addr,
                                     UntypedCOFFExports.count(Addr) != 0)
                    ? 1
                    : 0;
    };
    if (NeedVerify.size() < limits::kMinParallelVerify) {
      // Most targets classify the initial linear probe from the instruction id
      // alone.  ARM PC-writing loads, register lists, and data-processing
      // instructions require operand detail to distinguish a real terminator
      // from (for example) `pop {r4}`.
      const bool PrevDetail = Dec.detailEnabled();
      Dec.setDetail(Img.Arch == Arch::ARM);
      for (size_t I : NeedVerify)
        verifyIdx(Dec, I);
      Dec.setDetail(PrevDetail);
    } else {
      parallelForEach(NeedVerify.size(), [&](auto Claim, size_t Count) {
        Decoder LocalDec;
        if (!LocalDec.init(Img.Arch, Img.Mode))
          return;
        LocalDec.setDetail(Img.Arch == Arch::ARM);
        for (size_t P; (P = Claim()) < Count;)
          verifyIdx(LocalDec, NeedVerify[P]);
      });
    }

    for (size_t I : NeedVerify)
      if (Keep[I])
        VerifiedCandidates.insert(
            normalizeCodeAddress(Results[I].first, Img.Arch, Img.Mode));

    std::vector<std::pair<va_t, std::string>> Kept;
    Kept.reserve(N);
    for (size_t I = 0; I < N; ++I)
      if (Keep[I])
        Kept.push_back(std::move(Results[I]));
    Results = std::move(Kept);
  }

  // Reject auto-detected entries that fall strictly *inside* a sized function
  // symbol's [Addr, Addr+Size) range.  Such a symbol claims its whole extent as
  // one function, so any non-symbol entry inside it is spurious — most often an
  // ARM embedded constant pool ($d region) decoded as code, which would be
  // lifted as a garbage `sub_XXXX` full of undecodable instructions (e.g. a
  // bare `msr`/`svc`) and break recompilation of the whole object.  Runs
  // unconditionally (not only when Img.Entry != 0) so relocatable .o objects
  // are covered too.
  {
    std::vector<std::pair<va_t, va_t>> SizedRanges;
    std::set<va_t> FunctionSymbolStarts;
    for (const auto &Sym : Img.Symbols) {
      if (Sym.IsFunc)
        FunctionSymbolStarts.insert(Sym.Addr);
      if (!Sym.IsFunc || Sym.Size == 0 || Sym.Size > InvalidVA - Sym.Addr)
        continue;
      SizedRanges.push_back({Sym.Addr, Sym.Addr + Sym.Size});
    }
    if (!SizedRanges.empty()) {
      auto InsideSized = [&](va_t A) -> bool {
        for (auto &[S, E] : SizedRanges)
          if (A > S && A < E)
            return true;
        return false;
      };
      // A sized function symbol ordinarily claims its whole [Addr, Addr+Size)
      // extent.  An explicit function symbol at an interior address is stronger
      // evidence, however, as is a Mach-O direct-call target that survived the
      // verification pass above: compact-unwind coverage ranges may span leaf
      // functions that have no unwind row of their own.  Preserve those starts
      // while still dropping scan/export-only candidates such as an ARM
      // embedded constant pool ($d) decoded as a bogus `sub_XXXX`.
      std::vector<std::pair<va_t, std::string>> Filtered;
      Filtered.reserve(Results.size());
      for (auto &R : Results) {
        if (InsideSized(R.first) && !FunctionSymbolStarts.count(R.first) &&
            !IsCoveredMachODirectCallTarget(R.first))
          continue;
        Filtered.push_back(R);
      }
      Results = std::move(Filtered);
    }
  }

  // AArch64 instructions are unconditionally 4-byte aligned, so a function
  // entry whose address is not 4-aligned is provably spurious.  These appear
  // when the call-target scan resynchronises after an undecodable byte (or
  // begins a worker chunk) mid-instruction and then decodes a `bl`/`b` at an
  // unaligned PC, yielding an unaligned branch target that happens to land
  // inside real code.  Without symbol sizes (e.g. Mach-O) neither the
  // sized-range nor the known-code-range filter above can reject it, so it
  // survives as a garbage `sub_<addr>` that is lifted and — in the in-place
  // patcher — trampolined at its unaligned VA.  That trampoline lands inside
  // the enclosing real function and overwrites live instructions (e.g. an
  // ADRP+ADD literal load), crashing the otherwise-fit function (section mode
  // escapes only because it relocates the whole function, leaving the clobbered
  // original bytes dead).  Drop them. x86 is variable-length, and ARM/Thumb
  // encode the instruction set in entry bit 0, so this guard is gated strictly
  // to AArch64.
  if (Img.Arch == Arch::AArch64) {
    std::vector<std::pair<va_t, std::string>> Aligned;
    Aligned.reserve(Results.size());
    for (auto &R : Results) {
      if ((R.first & 0x3) != 0) {
        LLVM_DEBUG(llvm::dbgs()
                   << "func-detector: dropping misaligned AArch64 entry 0x"
                   << llvm::utohexstr(R.first) << "\n");
        continue;
      }
      Aligned.push_back(std::move(R));
    }
    Results = std::move(Aligned);
  }

  // Publish only verified candidates that survived every later overlap and
  // alignment filter. A transient decoder hit must never become patch-time
  // function identity after this routine has rejected it.
  for (const auto &[Addr, Name] : Results) {
    (void)Name;
    const va_t Normalized = normalizeCodeAddress(Addr, Img.Arch, Img.Mode);
    if (VerifiedCandidates.count(Normalized))
      Img.VerifiedFunctionEntries.insert(Normalized);
  }

  std::sort(Results.begin(), Results.end());
  return Results;
}

//===----------------------------------------------------------------------===//
// verifyFunctionDecode
//===----------------------------------------------------------------------===//

bool FuncDetector::verifyFunctionDecode(const BinaryImage &Img, Decoder &Dec,
                                        va_t Addr, bool KeepInconclusive) {
  constexpr int kMaxInsns = limits::kMaxVerifyInsns;
  const auto *Seg = Img.getSegmentFor(Addr);
  if (!Seg || !Img.hasExecutableCodeOwnerAt(Addr))
    return false;

  va_t Cur = Addr;
  bool SawTerminator = false;
  for (int I = 0; I < kMaxInsns; ++I) {
    size_t Off = static_cast<size_t>(Cur - Seg->VA);
    if (Off >= Seg->Data.size())
      return false;
    size_t Remain = Seg->Data.size() - Off;

    // Classification only: this walk needs the instruction size and whether it
    // is a function terminator.  The lightweight decode avoids lift-path
    // fixups; ARM callers nevertheless leave detail enabled because writes to
    // PC and POP/LDM register lists are operand-dependent.
    DecodedInsn DI;
    int Sz = Dec.decodeOneLight(Seg->Data.data() + Off, Remain, Cur, DI);
    if (Sz <= 0)
      return false;
    if (!DI.Raw)
      return false;
    if (!Img.hasExecutableCodeOwnerRange(Cur, static_cast<uint64_t>(Sz)))
      return false;

    if (Dec.isFunctionTerminator(DI)) {
      SawTerminator = true;
      break;
    }
    Cur += Sz;
  }
  if (!SawTerminator && !KeepInconclusive &&
      !hasBoundedSemanticTerminator(Img, Dec, Addr))
    return false;

  // A linear walk can encounter RET on one arm while a conditional branch on
  // another arm lands in embedded executable data.  That pattern is common in
  // stripped images whose executable sections contain strings or tables: a
  // byte sequence in the data may also spell CALL, creating a bogus candidate
  // that the straight-line probe above would accept.  Validate direct reachable
  // arms before promoting an untrusted scan hit to a function.
  //
  // This deliberately remains a bounded, decode-focused probe rather than a
  // second full CFGBuilder run.  Exhausting the budget or encountering an
  // unsupported lift is inconclusive, so the candidate is kept for the formal
  // pipeline audit instead of hiding a real coverage gap.
  const bool PreviousDetail = Dec.detailEnabled();
  Dec.setDetail(true);
  Dec.resetX86FpuState();
  const bool ReachablePathsDecode = [&]() {
    constexpr size_t kCFGProbeBudget =
        static_cast<size_t>(limits::kMaxVerifyInsns) * 4;
    std::queue<va_t> Worklist;
    std::set<va_t> Explored;
    Worklist.push(Addr);

    while (!Worklist.empty()) {
      va_t Cur = Worklist.front();
      Worklist.pop();

      while (true) {
        if (Explored.count(Cur))
          break;
        if (Explored.size() >= kCFGProbeBudget)
          return true;

        const auto *PathSeg = Img.getSegmentFor(Cur);
        if (!PathSeg || !Img.hasExecutableCodeOwnerAt(Cur) || Cur < PathSeg->VA)
          return false;
        size_t Off = static_cast<size_t>(Cur - PathSeg->VA);
        if (Off >= PathSeg->Data.size())
          return false;

        DecodedInsn DI;
        int Sz = Dec.decodeOneForLift(PathSeg->Data.data() + Off,
                                      PathSeg->Data.size() - Off, Cur, DI);
        if (Sz <= 0)
          return false;
        if (!Img.hasExecutableCodeOwnerRange(Cur, static_cast<uint64_t>(Sz)))
          return false;
        Explored.insert(Cur);

        std::vector<LowOp> Ops;
        try {
          Dec.liftToLow(DI, Ops);
        } catch (const UnliftedInstruction &) {
          return true;
        }
        const bool IsNoReturnCall = isUnconditionalNoReturnCall(Img, Ops);

        bool IsBranch = false;
        bool IsCond = false;
        bool IsRet = false;
        bool IsIndirect = false;
        va_t BranchTarget = InvalidVA;
        for (const LowOp &Op : Ops) {
          switch (Op.Opcode) {
          case NdOp::BRANCH:
            IsBranch = true;
            if (Op.NumInputs > 0 && Op.Inputs[0].isConst())
              BranchTarget = Op.Inputs[0].Offset;
            break;
          case NdOp::COND_BR:
            IsBranch = true;
            IsCond = true;
            if (Op.NumInputs > 0 && Op.Inputs[0].isConst())
              BranchTarget = Op.Inputs[0].Offset;
            break;
          case NdOp::INDIR_BR:
            IsBranch = true;
            IsIndirect = true;
            break;
          case NdOp::RETURN:
            IsRet = true;
            break;
          default:
            break;
          }
        }
        if (!IsBranch && !IsRet && Dec.isFunctionTerminator(DI))
          IsRet = true;

        if (IsNoReturnCall)
          break;
        if (IsRet && !(IsCond && IsBranch))
          break;
        if (IsRet && IsCond && IsBranch) {
          if (BranchTarget != InvalidVA)
            Worklist.push(BranchTarget);
          break;
        }
        if (!IsBranch) {
          Cur += static_cast<va_t>(Sz);
          continue;
        }
        if (IsIndirect)
          break;
        if (BranchTarget == InvalidVA)
          return true;
        if (BranchTarget != Addr && Entries.count(BranchTarget) > 0) {
          if (IsCond) {
            Cur += static_cast<va_t>(Sz);
            continue;
          }
          break;
        }

        Worklist.push(BranchTarget);
        if (!IsCond)
          break;
        Cur += static_cast<va_t>(Sz);
      }
    }
    return true;
  }();
  Dec.resetX86FpuState();
  Dec.setDetail(PreviousDetail);
  return ReachablePathsDecode;
}

//===----------------------------------------------------------------------===//
// Call-target scanning
//===----------------------------------------------------------------------===//

// AArch64 is fixed 4-byte width and 4-byte aligned, so direct-call (BL)
// discovery does not need a full capstone decode of every position: read each
// aligned 32-bit word and classify it with a single mask+shift.  This visits
// exactly the aligned instruction addresses the capstone-based sweep would
// (real code is 4-aligned), at a fraction of the cost, and shares the BL
// encoding knowledge with the lifter's directCallTarget.
static void scanSegmentCallsAArch64(const BinaryImage &Img, const Segment *Seg,
                                    va_t Start, va_t End, std::set<va_t> &Out) {
  va_t Cur = (Start + 3) & ~static_cast<va_t>(3);
  const uint8_t *Data = Seg->Data.data();
  const size_t DataSize = Seg->Data.size();
  while (Cur <= End && End - Cur >= 4) {
    size_t Off = static_cast<size_t>(Cur - Seg->VA);
    if (Off + 4 > DataSize)
      break;
    if (!Img.hasExecutableCodeOwnerRange(Cur, 4)) {
      Cur += 4;
      continue;
    }
    const uint8_t *P = Data + Off;
    uint32_t Word = static_cast<uint32_t>(P[0]) |
                    (static_cast<uint32_t>(P[1]) << 8) |
                    (static_cast<uint32_t>(P[2]) << 16) |
                    (static_cast<uint32_t>(P[3]) << 24);
    va_t Tgt = AArch64Lifter::decodeBranchLinkTarget(Word, Cur);
    if (Tgt != InvalidVA) {
      if (Img.hasExecutableCodeOwnerAt(Tgt))
        Out.insert(Tgt);
    }
    Cur += 4;
  }
}

static void scanSegmentCalls(const BinaryImage &Img, Decoder &Dec,
                             const Segment *Seg, va_t Start, va_t End,
                             std::set<va_t> &Out) {
  if (Img.Arch == Arch::AArch64) {
    scanSegmentCallsAArch64(Img, Seg, Start, End, Out);
    return;
  }

  va_t Cur = Start;
  while (Cur < End) {
    size_t Off = static_cast<size_t>(Cur - Seg->VA);
    if (Off >= Seg->Data.size())
      break;
    DecodedInsn DI;
    const size_t Remain =
        static_cast<size_t>(std::min<va_t>(Seg->Data.size() - Off, End - Cur));
    int Sz = Dec.decodeOne(Seg->Data.data() + Off, Remain, Cur, DI);
    if (Sz == 0) {
      Cur++;
      continue;
    }
    if (!Img.hasExecutableCodeOwnerRange(Cur, static_cast<uint64_t>(Sz))) {
      Cur += Sz;
      continue;
    }
    va_t Tgt = Dec.directCallTarget(DI);
    if (Tgt != InvalidVA) {
      if (Img.hasExecutableCodeOwnerAt(Tgt))
        Out.insert(Tgt);
    }
    Cur += Sz;
  }
}

void FuncDetector::scanCallTargets(const BinaryImage &Img, Decoder &Dec,
                                   std::set<va_t> &Out) {
  struct ScanChunk {
    const Segment *Seg;
    va_t Start;
    va_t End;
  };
  std::vector<ScanChunk> Chunks;

  const unsigned ThreadsN = workerThreadCount();
  constexpr size_t MinChunk = limits::kMinFuncScanChunk;

  auto AddRange = [&](const Segment *Seg, va_t Start, uint64_t RequestedLen) {
    if (!Seg || !Seg->isExecutable() || Seg->Data.empty() || Start < Seg->VA)
      return;
    const uint64_t StartOff = Start - Seg->VA;
    if (StartOff >= Seg->Data.size())
      return;
    const size_t ScanLen = static_cast<size_t>(std::min<uint64_t>(
        RequestedLen, Seg->Data.size() - static_cast<size_t>(StartOff)));
    if (ScanLen == 0)
      return;
    if (Img.Arch != Arch::AArch64) {
      Chunks.push_back({Seg, Start, Start + ScanLen});
      return;
    }
    const va_t End = Start + ScanLen;
    const va_t AlignedStart = (Start + 3) & ~static_cast<va_t>(3);
    if (AlignedStart >= End)
      return;
    const size_t AlignedLen = static_cast<size_t>(End - AlignedStart);
    size_t ChunkSz = std::max(MinChunk, AlignedLen / ThreadsN);
    ChunkSz = (ChunkSz + 3) & ~static_cast<size_t>(3);
    for (size_t Off = 0; Off < AlignedLen; Off += ChunkSz) {
      const size_t CEnd = std::min(Off + ChunkSz, AlignedLen);
      Chunks.push_back({Seg, AlignedStart + Off, AlignedStart + CEnd});
    }
  };

  // Scan exact instruction sections when a mapping has section metadata.
  // Packed/sectionless executable mappings retain the historical segment-wide
  // fallback, independently for each segment.
  for (const Section &Sec : Img.Sections)
    if (Sec.Size != 0 && Sec.isReadable() && Img.isCodeAddress(Sec.VA))
      AddRange(Img.getSegmentFor(Sec.VA), Sec.VA, Sec.Size);
  for (const auto &[Start, End] : Img.KnownCodeRanges)
    if (End > Start)
      AddRange(Img.getSegmentFor(Start), Start, End - Start);
  for (const Symbol &Sym : Img.Symbols)
    if (Sym.IsFunc && Sym.Size != 0) {
      const va_t Start = normalizeCodeAddress(Sym.Addr, Img.Arch, Img.Mode);
      AddRange(Img.getSegmentFor(Start), Start, Sym.Size);
    }
  for (const auto &[Start, End] : Img.ImportStubRanges)
    if (End > Start)
      AddRange(Img.getSegmentFor(Start), Start, End - Start);
  for (const Segment &Seg : Img.Segments)
    if (!Img.segmentHasReadableSectionMetadata(Seg))
      AddRange(&Seg, Seg.VA, Seg.Data.size());

  if (Chunks.size() <= 1) {
    for (auto &[Seg, Start, End] : Chunks)
      scanSegmentCalls(Img, Dec, Seg, Start, End, Out);
    return;
  }

  std::mutex Mtx;
  std::atomic<size_t> NextChunk{0};

  auto Worker = [&]() {
    Decoder LocalDec;
    if (!LocalDec.init(Img.Arch, Img.Mode))
      return;
    std::set<va_t> LocalEntries;

    while (true) {
      size_t CI = NextChunk.fetch_add(1, std::memory_order_relaxed);
      if (CI >= Chunks.size())
        break;
      auto &[Seg, Start, End] = Chunks[CI];
      scanSegmentCalls(Img, LocalDec, Seg, Start, End, LocalEntries);
    }

    std::lock_guard<std::mutex> Lk(Mtx);
    Out.insert(LocalEntries.begin(), LocalEntries.end());
  };

  std::vector<std::thread> Ts;
  Ts.reserve(ThreadsN);
  for (unsigned T = 0; T < ThreadsN; ++T)
    Ts.emplace_back(Worker);
  for (auto &T : Ts)
    T.join();
}

} // namespace neverd
