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

#include "FuncDetectorDetail.h"

#include "neverd/Limits.h"
#include "neverd/ir/low/LowIR.h"
#include "neverd/libc/LibCNames.h"
#include "neverd/loader/COFF/COFFRegistrationEH.h"
#include "neverd/support/BinaryEncoding.h"
#include "neverd/support/Parallel.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/BinaryFormat/MachO.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <atomic>
#include <iterator>
#include <map>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

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
    std::optional<InstructionMode> PathMode = Img.instructionModeAt(Addr);
    for (int I = 0; I < kMaxInsns; ++I) {
      if (Cur < Seg->VA)
        return false;
      const size_t Off = static_cast<size_t>(Cur - Seg->VA);
      if (Off >= Seg->Data.size())
        return false;

      if (!Dec.selectMode(Img, Cur, PathMode))
        return false;
      PathMode = Dec.currentMode();
      DecodedInsn DI;
      const int Sz = Dec.decodeOneForLift(Seg->Data.data() + Off,
                                          Seg->Data.size() - Off, Cur, DI);
      if (Sz <= 0 || !DI.Raw)
        return false;
      if (!Img.hasExecutableCodeOwnerRange(Cur, static_cast<uint64_t>(Sz)))
        return false;
      if (Img.Arch == Arch::ARM &&
          Img.instructionModeAt(Cur + Sz - 1, Dec.currentMode()) !=
              Dec.currentMode())
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
  UnsymbolizedX86Entries.clear();
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

  std::set<va_t> ExceptionEntries;
  for (const ExceptionFunction &EH : Img.ExceptionMetadata.Functions) {
    // A frame can begin after its function's first instruction.
    const va_t EHEntry =
        EH.FunctionEntry != 0 ? EH.FunctionEntry : EH.CodeRange.Begin;
    if (EH.Kind == RuntimeFunctionKind::Primary && EH.CodeRange.isValid() &&
        EHEntry != 0 && Img.hasExecutableCodeOwnerAt(EHEntry))
      ExceptionEntries.insert(EHEntry);
  }
  const std::set<va_t> ExceptionThunks =
      Img.ExceptionMetadata.registrationScopeThunks();
  const auto CxxPointerRoles =
      coff_loader::getCheckedX86CxxCallbackPointerRoles(Img);
  std::set<va_t> IndependentEntries = Entries;
  IndependentEntries.insert(ExceptionEntries.begin(), ExceptionEntries.end());
  for (const auto &Sym : Img.Symbols)
    if (Sym.IsFunc && Sym.Origin != NameOrigin::Synthesized)
      IndependentEntries.insert(Sym.Addr);

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
    if (ExceptionThunks.count(Sym.Addr))
      continue;
    if (Sym.Origin == NameOrigin::Synthesized && CxxPointerRoles &&
        CxxPointerRoles->RuntimeOnlyPointerTargets.count(Sym.Addr))
      continue;
    if (Img.hasExecutableCodeOwnerAt(Sym.Addr)) {
      Entries.insert(Sym.Addr);
      Results.push_back({Sym.Addr, Sym.Name});
    }
  }

  for (va_t Addr : ExceptionEntries) {
    if (Entries.count(Addr) || SkipAddrs.count(Addr))
      continue;
    Entries.insert(Addr);
    Results.push_back({Addr, Img.getFunctionNameAt(Addr)});
  }

  // A linked Mach-O nlist symbol has no size field, but adjacent function
  // symbols still delimit the compiler-emitted body between them.  Full-width
  // rebases for GNU local-label tables point into that interval; treating each
  // label as a standalone function candidate makes the interval end at its
  // first case and prevents the owning CFG from recovering the table.  Keep an
  // exact function-symbol target, but leave strict interior rebases to the
  // owning function.  Stripped images and the unbounded final symbol retain
  // the conservative relocation-candidate behavior below.
  std::vector<va_t> MachOFunctionStarts;
  if (Img.Format == BinaryFormat::MachO && !Img.IsRelocatable) {
    for (const Symbol &Sym : Img.Symbols)
      if (Sym.IsFunc && Img.hasExecutableCodeOwnerAt(Sym.Addr))
        MachOFunctionStarts.push_back(
            normalizeCodeAddress(Sym.Addr, Img.Arch, Img.Mode));
    std::sort(MachOFunctionStarts.begin(), MachOFunctionStarts.end());
    MachOFunctionStarts.erase(
        std::unique(MachOFunctionStarts.begin(), MachOFunctionStarts.end()),
        MachOFunctionStarts.end());
  }
  auto IsMachOLocalLabel = [&](va_t Target) {
    if (MachOFunctionStarts.empty())
      return false;
    const auto Next = std::upper_bound(MachOFunctionStarts.begin(),
                                       MachOFunctionStarts.end(), Target);
    if (Next == MachOFunctionStarts.begin() ||
        Next == MachOFunctionStarts.end())
      return false;
    return *std::prev(Next) < Target && Target < *Next;
  };

  // A full-width relocation from data to executable bytes is structural
  // evidence for a callable candidate even when the target is a tiny leaf
  // with neither unwind metadata nor a conventional prologue.  Do not trust
  // the address here: the bounded decoder and the overlap filters below still
  // decide whether it is a complete function entry.  In particular, a
  // relocation-backed jump-table target inside a known function remains
  // rejected as an interior block rather than being promoted to a function.
  const bool IsX86LinkedCOFF = Img.Arch == Arch::X86 &&
                               Img.Format == BinaryFormat::COFF &&
                               !Img.IsRelocatable;
  std::set<va_t> RelocationCodeTargets;
  const uint32_t PointerSize = Img.getPointerSize();
  if (PointerSize != 0)
    for (va_t Slot : Img.CodePtrRelocSlots) {
      const uint8_t *Bytes = Img.readVA(Slot, PointerSize);
      if (!Bytes)
        continue;
      const va_t Target = normalizeCodeAddress(readPtr(Bytes, Img.is64Bit()),
                                               Img.Arch, Img.Mode);
      if (CxxPointerRoles)
        if (const auto Source = CxxPointerRoles->Sources.find(Slot);
            Source != CxxPointerRoles->Sources.end() &&
            Source->second == Target)
          continue;
      if (Img.hasExecutableCodeOwnerAt(Target) && !IsMachOLocalLabel(Target)) {
        Entries.insert(Target);
        RelocationCodeTargets.insert(Target);
      }
    }

  std::set<va_t> DirectCallTargets;
  // Functions a binary file's code takes the address of, whose prologue
  // already vouches for them.
  std::set<va_t> CodePointerTargets;
  if (Img.Entry != 0) {
    scanCallTargets(Img, Dec, DirectCallTargets);
    Entries.insert(DirectCallTargets.begin(), DirectCallTargets.end());
    // A binary file has no symbols, unwind tables or relocations to name
    // the functions its code only takes the address of.  A target starts
    // with a prologue, so its decode below need only not fail: a function
    // such as main runs far past the verifier's window before it returns.
    if (Img.Format == BinaryFormat::Raw && Img.Arch == Arch::X64) {
      func_detect_detail::scanCodePointersX64(Img, Dec, CodePointerTargets);
      Entries.insert(CodePointerTargets.begin(), CodePointerTargets.end());
    }
    if (Img.Arch == Arch::ARM)
      Entries.insert(Img.ARMVeneerTargets.begin(), Img.ARMVeneerTargets.end());
    if (IsX86LinkedCOFF)
      scanX86UnsymbolizedEntries(Img, Dec, Entries);
  }

  if (CxxPointerRoles)
    for (va_t Target : CxxPointerRoles->RuntimeOnlyPointerTargets)
      if (!IndependentEntries.count(Target) && !DirectCallTargets.count(Target))
        Entries.erase(Target);

  for (va_t Thunk : ExceptionThunks)
    Entries.erase(Thunk);

  auto IsCoveredMachODirectCallTarget = [&](va_t Addr) {
    return Img.Format == BinaryFormat::MachO &&
           DirectCallTargets.count(Addr) != 0;
  };
  auto IsCoveredImportCallTarget = [&](va_t Addr) {
    // One linker-generated unwind row can cover the whole PLT. A direct
    // call to an exact import veneer still names a distinct callable entry;
    // the coarse stub range alone is not enough to establish that identity.
    return DirectCallTargets.count(Addr) != 0 && Img.isImportStubAt(Addr) &&
           Img.findImportStubAt(Addr) != nullptr;
  };
  auto IsX86LinkedCallOrRelocTarget = [&](va_t Addr) {
    return IsX86LinkedCOFF && (DirectCallTargets.count(Addr) != 0 ||
                               RelocationCodeTargets.count(Addr) != 0);
  };

  std::set<va_t> Already;
  for (auto &[A, _] : Results)
    Already.insert(A);
  for (va_t Addr : Entries) {
    if (ExceptionThunks.count(Addr))
      continue;
    if (Already.insert(Addr).second)
      Results.push_back(
          {Addr, (kAutoFuncPrefix + llvm::utohexstr(Addr)).str()});
  }

  // Every AArch64 entry must be four-byte aligned, regardless of whether it
  // came from the image entry, a symbol, an export, a relocation or a scan.
  // Reject misaligned candidates before either trusting metadata or trying
  // to decode and lift their bytes: a mid-instruction probe can otherwise
  // reach a lifter with operands that do not describe the real instruction.
  // Apply this to images without an entry point too. Other architectures
  // have different alignment and instruction-set address conventions.
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

  if (Img.Entry != 0) {
    std::set<va_t> Trusted{Img.Entry};
    Trusted.insert(ExceptionEntries.begin(), ExceptionEntries.end());
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
    // Linked x86 PE call and reloc targets are verified the same way: a VC6
    // .text KnownCodeRange (or a coarse unwind span) is not one function.
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
    for (size_t I = 0; I < N; ++I) {
      va_t Addr = Results[I].first;
      if (Trusted.count(Addr) || UnsymbolizedX86Entries.count(Addr) ||
          IsX86LinkedCallOrRelocTarget(Addr))
        Keep[I] = 1;
      else if (UntypedCOFFExports.count(Addr) ||
               IsCoveredMachODirectCallTarget(Addr) ||
               IsCoveredImportCallTarget(Addr) ||
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
      Keep[I] =
          verifyFunctionDecode(Img, LocalDec, Addr,
                               UntypedCOFFExports.count(Addr) != 0 ||
                                   UnsymbolizedX86Entries.count(Addr) != 0 ||
                                   IsX86LinkedCallOrRelocTarget(Addr) ||
                                   CodePointerTargets.count(Addr) != 0)
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
        if (!LocalDec.init(Img))
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
    // A decoded FDE supplies an exact extent even when a pre-existing COFF
    // function symbol has no size. KnownCodeRanges can also contain coarse
    // coverage, so only the format-authenticated FDE contributes here.
    for (const ExceptionFunction &EH : Img.ExceptionMetadata.Functions)
      if (EH.Kind == RuntimeFunctionKind::Primary &&
          EH.Encoding == ExceptionEncoding::DwarfFDE && EH.Dwarf &&
          EH.ParseStatus != ExceptionParseStatus::Malformed &&
          EH.CodeRange.isValid())
        SizedRanges.emplace_back(EH.FunctionEntry ? EH.FunctionEntry
                                                  : EH.CodeRange.Begin,
                                 EH.CodeRange.End);
    if (!SizedRanges.empty()) {
      // Sorted by start, with the furthest end any range up to each one
      // reaches: A is strictly inside some range exactly when a range that
      // starts below A reaches past it.
      std::sort(SizedRanges.begin(), SizedRanges.end());
      std::vector<va_t> FurthestEnd(SizedRanges.size());
      for (size_t I = 0; I < SizedRanges.size(); ++I)
        FurthestEnd[I] =
            std::max(I ? FurthestEnd[I - 1] : va_t(0), SizedRanges[I].second);
      auto InsideSized = [&](va_t A) -> bool {
        const auto Below = std::lower_bound(
            SizedRanges.begin(), SizedRanges.end(), A,
            [](const std::pair<va_t, va_t> &Range, va_t Address) {
              return Range.first < Address;
            });
        if (Below == SizedRanges.begin())
          return false;
        return FurthestEnd[static_cast<size_t>(Below - SizedRanges.begin()) -
                           1] > A;
      };
      // A sized function symbol ordinarily claims its whole [Addr, Addr+Size)
      // extent.  An explicit function symbol at an interior address is stronger
      // evidence, as is a Mach-O direct-call target that survived the
      // verification pass above: compact-unwind coverage ranges may span leaf
      // functions that have no unwind row of their own.  x86 linked
      // CALL/HIGHLOW targets inside that extent are still interior blocks —
      // typically the immediate of `push imm32` / a 5-byte jmp landing — and
      // must not split the owning function into an empty named stub plus
      // `sub_XXXX`.
      std::vector<std::pair<va_t, std::string>> Filtered;
      Filtered.reserve(Results.size());
      for (auto &R : Results) {
        if (InsideSized(R.first) && !FunctionSymbolStarts.count(R.first) &&
            !IsCoveredMachODirectCallTarget(R.first) &&
            !IsCoveredImportCallTarget(R.first))
          continue;
        Filtered.push_back(R);
      }
      Results = std::move(Filtered);
    }
  }

  // Publish only verified candidates that survived every candidate and
  // overlap filter. A transient decoder hit must never become patch-time
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
  std::optional<InstructionMode> PathMode = Img.instructionModeAt(Addr);
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
    if (!Dec.selectMode(Img, Cur, PathMode))
      return false;
    PathMode = Dec.currentMode();
    DecodedInsn DI;
    int Sz = Dec.decodeOneLight(Seg->Data.data() + Off, Remain, Cur, DI);
    if (Sz <= 0)
      return false;
    if (!DI.Raw)
      return false;
    if (!Img.hasExecutableCodeOwnerRange(Cur, static_cast<uint64_t>(Sz)))
      return false;
    if (Img.Arch == Arch::ARM &&
        Img.instructionModeAt(Cur + Sz - 1, Dec.currentMode()) !=
            Dec.currentMode())
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
    std::queue<std::pair<va_t, std::optional<InstructionMode>>> Worklist;
    std::map<va_t, InstructionMode> Explored;
    Worklist.push({Addr, Img.instructionModeAt(Addr)});

    while (!Worklist.empty()) {
      auto [Cur, PathMode] = Worklist.front();
      Worklist.pop();

      while (true) {
        if (const auto Existing = Explored.find(Cur);
            Existing != Explored.end()) {
          if (Img.Arch == Arch::ARM &&
              Img.instructionModeAt(Cur, PathMode) != Existing->second)
            return false;
          break;
        }
        if (Explored.size() >= kCFGProbeBudget)
          return true;

        const auto *PathSeg = Img.getSegmentFor(Cur);
        if (!PathSeg || !Img.hasExecutableCodeOwnerAt(Cur) || Cur < PathSeg->VA)
          return false;
        size_t Off = static_cast<size_t>(Cur - PathSeg->VA);
        if (Off >= PathSeg->Data.size())
          return false;

        if (!Dec.selectMode(Img, Cur, PathMode))
          return false;
        PathMode = Dec.currentMode();
        DecodedInsn DI;
        int Sz = Dec.decodeOneForLift(PathSeg->Data.data() + Off,
                                      PathSeg->Data.size() - Off, Cur, DI);
        if (Sz <= 0)
          return false;
        if (!Img.hasExecutableCodeOwnerRange(Cur, static_cast<uint64_t>(Sz)))
          return false;
        if (Img.Arch == Arch::ARM &&
            Img.instructionModeAt(Cur + Sz - 1, Dec.currentMode()) !=
                Dec.currentMode())
          return false;
        Explored.emplace(Cur, Dec.currentMode());

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

        std::optional<InstructionMode> BranchMode = PathMode;
        switch (Dec.controlTargetMode(DI, Dec.currentMode())) {
        case LowInstructionTargetMode::ARM:
          BranchMode = InstructionMode::ARM;
          break;
        case LowInstructionTargetMode::Thumb:
          BranchMode = InstructionMode::Thumb;
          break;
        case LowInstructionTargetMode::FromTargetBit0:
          BranchMode.reset();
          break;
        case LowInstructionTargetMode::Preserve:
          break;
        }

        if (IsNoReturnCall)
          break;
        if (IsRet && !(IsCond && IsBranch))
          break;
        if (IsRet && IsCond && IsBranch) {
          if (BranchTarget != InvalidVA)
            Worklist.push({BranchTarget, BranchMode});
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

        Worklist.push({BranchTarget, BranchMode});
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

void FuncDetector::scanCallTargets(const BinaryImage &Img, Decoder &Dec,
                                   std::set<va_t> &Out) {
  struct ScanChunk {
    const Segment *Seg;
    va_t Start;
    va_t End;
  };
  std::vector<ScanChunk> Chunks;
  // An x86 sweep cannot start just anywhere: from the middle of an
  // instruction it decodes other instructions.  So each piece of a long range
  // is swept on its own, from its nominal start and with the range's end as
  // the decode bound, and its steps are kept.  Joining the pieces in order,
  // the sweep of each continues from where the previous one left off, and
  // one piece's steps are taken as soon as that sweep reaches an address the
  // piece also reached: from there on, the two are the same sweep.
  struct PieceJob {
    const Segment *Seg;
    va_t Start;
    va_t End;
    va_t RangeEnd;
  };
  struct SplitRange {
    const Segment *Seg;
    va_t Start;
    va_t End;
    size_t FirstPiece;
    size_t Pieces;
  };
  std::vector<PieceJob> PieceJobs;
  std::vector<SplitRange> SplitRanges;

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
    if (Img.Arch == Arch::X86 || Img.Arch == Arch::X64) {
      // A long range is swept in pieces on the worker threads and the pieces
      // joined afterwards exactly as one sweep would have gone; see below.
      const size_t Pieces =
          std::min<size_t>(ThreadsN * limits::kFuncScanPiecesPerWorker,
                           ScanLen / limits::kMinFuncScanPiece);
      if (Pieces < 2) {
        Chunks.push_back({Seg, Start, Start + ScanLen});
        return;
      }
      SplitRange Range{Seg, Start, Start + ScanLen, PieceJobs.size(), Pieces};
      for (size_t Piece = 0; Piece < Pieces; ++Piece)
        PieceJobs.push_back({Seg, Start + ScanLen * Piece / Pieces,
                             Start + ScanLen * (Piece + 1) / Pieces,
                             Start + ScanLen});
      SplitRanges.push_back(Range);
      return;
    }
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

  // An x86 sweep reads only sizes and ids, and the targets of calls.
  const bool SweepWithoutDetail =
      Img.Arch == Arch::X86 || Img.Arch == Arch::X64;
  const bool PreviousDetail = Dec.detailEnabled();
  const bool PreviousText = Dec.textEnabled();
  if (SweepWithoutDetail) {
    Dec.setDetail(false);
    Dec.setText(false);
  }

  if (Chunks.size() <= 1 && PieceJobs.empty()) {
    for (auto &[Seg, Start, End] : Chunks)
      func_detect_detail::scanSegmentCalls(Img, Dec, Seg, Start, End, Out);
    Dec.setDetail(PreviousDetail);
    Dec.setText(PreviousText);
    return;
  }

  // A range inside a split one -- a sized function symbol inside `.text`,
  // say -- is swept again only where its own sweep can differ: from its
  // start when the long sweep does not pass through it, and otherwise only
  // across the instruction the long sweep decodes over its end.  Everything
  // before that the long sweep has already done, and found.
  struct Deferred {
    size_t Range;
    ScanChunk Chunk;
  };
  std::vector<Deferred> DeferredChunks;
  if (!SplitRanges.empty()) {
    std::vector<ScanChunk> Kept;
    Kept.reserve(Chunks.size());
    for (const ScanChunk &Chunk : Chunks) {
      const auto Range = std::find_if(
          SplitRanges.begin(), SplitRanges.end(), [&](const SplitRange &R) {
            return R.Seg == Chunk.Seg && R.Start <= Chunk.Start &&
                   Chunk.End <= R.End;
          });
      if (Range == SplitRanges.end())
        Kept.push_back(Chunk);
      else
        DeferredChunks.push_back(
            {static_cast<size_t>(Range - SplitRanges.begin()), Chunk});
    }
    Chunks = std::move(Kept);
  }

  std::mutex Mtx;
  std::atomic<size_t> NextJob{0};
  std::vector<std::vector<func_detect_detail::CallScanStep>> PieceSteps(
      PieceJobs.size());

  auto Worker = [&]() {
    Decoder LocalDec;
    if (!LocalDec.init(Img))
      return;
    if (SweepWithoutDetail) {
      LocalDec.setDetail(false);
      LocalDec.setText(false);
    }
    std::set<va_t> LocalEntries;

    while (true) {
      const size_t Job = NextJob.fetch_add(1, std::memory_order_relaxed);
      if (Job < PieceJobs.size()) {
        const PieceJob &Piece = PieceJobs[Job];
        func_detect_detail::CodeInterval Known =
            func_detect_detail::codeIntervalAround(Img, Piece.Start);
        for (va_t Cur = Piece.Start; Cur < Piece.End;) {
          const auto Step = func_detect_detail::stepCallsX86(
              Img, LocalDec, Piece.Seg, Cur, Piece.RangeEnd, Known);
          if (!Step)
            break;
          PieceSteps[Job].push_back(*Step);
          Cur = Step->Next;
        }
        continue;
      }
      if (Job >= PieceJobs.size() + Chunks.size())
        break;
      auto &[Seg, Start, End] = Chunks[Job - PieceJobs.size()];
      func_detect_detail::scanSegmentCalls(Img, LocalDec, Seg, Start, End,
                                           LocalEntries);
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

  // The targets the joined sweeps find, added to \p Out in order at the end.
  std::vector<va_t> Found;
  std::vector<std::vector<func_detect_detail::CallScanStep>> RangeSteps(
      SplitRanges.size());
  for (size_t RangeIndex = 0; RangeIndex < SplitRanges.size(); ++RangeIndex) {
    const SplitRange &Range = SplitRanges[RangeIndex];
    std::vector<func_detect_detail::CallScanStep> &Taken =
        RangeSteps[RangeIndex];
    size_t PieceStepCount = 0;
    for (size_t Piece = Range.FirstPiece;
         Piece < Range.FirstPiece + Range.Pieces; ++Piece)
      PieceStepCount += PieceSteps[Piece].size();
    Taken.reserve(PieceStepCount);
    va_t Pos = Range.Start;
    func_detect_detail::CodeInterval Known =
        func_detect_detail::codeIntervalAround(Img, Pos);
    for (size_t Piece = Range.FirstPiece;
         Piece < Range.FirstPiece + Range.Pieces; ++Piece) {
      const auto &Steps = PieceSteps[Piece];
      auto It =
          std::lower_bound(Steps.begin(), Steps.end(), Pos,
                           [](const func_detect_detail::CallScanStep &Step,
                              va_t Address) { return Step.Addr < Address; });
      // Sweep on until this piece's sweep is reached.
      while (Pos < PieceJobs[Piece].End &&
             (It == Steps.end() || It->Addr != Pos)) {
        const auto Step = func_detect_detail::stepCallsX86(
            Img, Dec, Range.Seg, Pos, Range.End, Known);
        if (!Step) {
          Pos = Range.End;
          break;
        }
        if (Step->Target != InvalidVA)
          Found.push_back(Step->Target);
        Taken.push_back(*Step);
        Pos = Step->Next;
        while (It != Steps.end() && It->Addr < Pos)
          ++It;
      }
      for (; It != Steps.end() && It->Addr == Pos; ++It) {
        if (It->Target != InvalidVA)
          Found.push_back(It->Target);
        Taken.push_back(*It);
        Pos = It->Next;
      }
    }
  }
  llvm::sort(Found);
  auto Hint = Out.begin();
  for (va_t Target : Found)
    Hint = std::next(Out.insert(Hint, Target));

  const auto ByAddress = [](const func_detect_detail::CallScanStep &Step,
                            va_t Address) { return Step.Addr < Address; };
  for (const Deferred &Item : DeferredChunks) {
    const auto &Taken = RangeSteps[Item.Range];
    const ScanChunk &Chunk = Item.Chunk;
    va_t From = Chunk.Start;
    const auto Start =
        std::lower_bound(Taken.begin(), Taken.end(), Chunk.Start, ByAddress);
    if (Start != Taken.end() && Start->Addr == Chunk.Start) {
      // The last step the long sweep takes before the chunk's end.
      const auto Past =
          std::lower_bound(Start, Taken.end(), Chunk.End, ByAddress);
      const auto Last = std::prev(Past);
      if (Last->Next <= Chunk.End)
        continue;
      From = Last->Addr;
    }
    func_detect_detail::scanSegmentCalls(Img, Dec, Chunk.Seg, From, Chunk.End,
                                         Out);
  }
  Dec.setDetail(PreviousDetail);
  Dec.setText(PreviousText);
}

} // namespace neverd
