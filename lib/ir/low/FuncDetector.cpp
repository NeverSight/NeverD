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
#include "neverd/libc/NoReturnLoadedTarget.h"
#include "neverd/loader/COFF/COFFRegistrationEH.h"
#include "neverd/loader/COFF/ControlFlowEntries.h"
#include "neverd/support/BinaryEncoding.h"
#include "neverd/support/Parallel.h"
#include "neverd/support/ProloguePatterns.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/BinaryFormat/MachO.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <atomic>
#include <iterator>
#include <map>
#include <mutex>
#include <optional>
#include <queue>
#include <thread>
#include <vector>

#define DEBUG_TYPE "neverd-func-detector"

namespace neverd {

namespace {

bool isUnconditionalNoReturnCall(const BinaryImage &Img,
                                 const std::vector<LowOp> &Ops) {
  va_t Target = InvalidVA;
  bool IsConditional = false, IsIndirect = false;
  for (const LowOp &Op : Ops) {
    if (Op.Opcode == NdOp::COND_BR)
      IsConditional = true;
    // x86 lifts `call [slot]` as an INDIR_CALL of the slot address, which
    // names the import the loader binds there, as the CFG builder reads it.
    else if ((Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL) &&
             Op.NumInputs > 0 && Op.Inputs[0].isConst()) {
      Target = Op.Inputs[0].Offset;
      IsIndirect = Op.Opcode == NdOp::INDIR_CALL;
    }
  }
  return !IsConditional &&
         (IsIndirect ? libc::isNoReturnLoadedTarget(Img, Target)
                     : libc::isNoReturnTarget(Img, Target));
}

bool hasBoundedSemanticTerminator(
    const BinaryImage &Img, Decoder &Dec, va_t Addr,
    const func_detect_detail::Cancellation *Control) {
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
      func_detect_detail::checkCancellation(Control);
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
      if (CallTarget != InvalidVA &&
          libc::isNoReturnLoadedTarget(Img, CallTarget)) {
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

/// The target of the direct unconditional jump that is the whole body of the
/// x86 function at \p Entry, after an optional indirect-branch-tracking
/// marker, or InvalidVA when its first transfer is anything else.
va_t forwardingJumpTarget(const BinaryImage &Img, Decoder &Dec, va_t Entry) {
  const Segment *Seg = Img.getSegmentFor(Entry);
  if (!Seg || Entry < Seg->VA)
    return InvalidVA;
  const size_t Off = static_cast<size_t>(Entry - Seg->VA);
  if (Off >= Seg->Data.size())
    return InvalidVA;
  const uint8_t *Bytes = Seg->Data.data() + Off;
  const size_t Size = Seg->Data.size() - Off;
  const size_t Marker = x86BranchTargetMarkerLength(Bytes, Size);
  const va_t Jump = Entry + static_cast<va_t>(Marker);
  if (!Img.hasExecutableCodeOwnerAt(Jump) ||
      !Dec.selectMode(Img, Jump, Img.instructionModeAt(Jump)))
    return InvalidVA;
  DecodedInsn DI;
  const int Sz = Dec.decodeOneForLift(Bytes + Marker, Size - Marker, Jump, DI);
  if (Sz <= 0 || !DI.Raw ||
      !Img.hasExecutableCodeOwnerRange(Jump, static_cast<uint64_t>(Sz)))
    return InvalidVA;
  std::vector<LowOp> Ops;
  try {
    Dec.liftToLow(DI, Ops);
  } catch (const UnliftedInstruction &) {
    return InvalidVA;
  }
  va_t Target = InvalidVA;
  for (const LowOp &Op : Ops)
    switch (Op.Opcode) {
    case NdOp::BRANCH:
      if (Target != InvalidVA || Op.NumInputs == 0 || !Op.Inputs[0].isConst())
        return InvalidVA;
      Target = Op.Inputs[0].Offset;
      break;
    case NdOp::COND_BR:
    case NdOp::INDIR_BR:
    case NdOp::CALL:
    case NdOp::RETURN:
      return InvalidVA;
    default:
      break;
    }
  return Target;
}

} // namespace

//===----------------------------------------------------------------------===//
// FuncDetector::detect
//===----------------------------------------------------------------------===//

std::vector<std::pair<va_t, std::string>>
FuncDetector::detect(const BinaryImage &Img, Decoder &Dec) {
  return detectImpl(Img, Dec, false);
}

std::optional<std::vector<std::pair<va_t, std::string>>>
FuncDetector::detect(const BinaryImage &Img, Decoder &Dec,
                     const std::function<bool()> &Cancelled) {
  func_detect_detail::Cancellation Control{Cancelled};
  const auto *PreviousControl = Cancellation;
  const bool PreviousDetail = Dec.detailEnabled(),
             PreviousText = Dec.textEnabled();
  Cancellation = &Control;
  auto Restore = llvm::scope_exit([&] {
    Cancellation = PreviousControl;
    Dec.setDetail(PreviousDetail);
    Dec.setText(PreviousText);
    Dec.resetX86FpuState();
  });
  try {
    return detectImpl(Img, Dec, false);
  } catch (const func_detect_detail::DiscoveryCancelled &) {
    return std::nullopt;
  }
}

bool RelocatedFunctionEntryVerifier::accepts(va_t Address) {
  Address = normalizeCodeAddress(Address, Img.Arch, Img.Mode);
  // Chained unwind and cold fragments continue another function's frame.
  // Neither a relocated pointer nor CFG metadata makes them callable entries.
  if (!Img.isFunctionEntryCandidateAt(Address))
    return false;
  if (!MetadataPrepared) {
    MetadataPrepared = true;
    auto Entries = readPEControlFlowEntries(Img);
    if (Entries)
      MetadataEntries = std::move(*Entries);
    else
      llvm::logAllUnhandledErrors(Entries.takeError(), llvm::errs(),
                                  "function entry metadata: ");
  }
  if (std::binary_search(MetadataEntries.begin(), MetadataEntries.end(),
                         Address))
    return true;
  if (Img.CodePtrRelocSlots.empty())
    return false;
  if (!Prepared) {
    Prepared = true;
    if (!Dec.init(Img))
      return false;
    (void)Detector.detectImpl(Img, Dec, true);
  }
  const auto [It, Inserted] = Answers.try_emplace(Address, false);
  if (Inserted && Detector.RelocatedCandidates.count(Address))
    It->second = Detector.verifyFunctionDecode(Img, Dec, Address, true, true);
  return It->second;
}

std::vector<std::pair<va_t, std::string>>
FuncDetector::detectImpl(const BinaryImage &Img, Decoder &Dec,
                         bool RelocationsOnly) {
  func_detect_detail::checkCancellation(Cancellation);
  Entries.clear();
  UnsymbolizedX86Entries.clear();
  RelocatedCandidates.clear();
  std::vector<std::pair<va_t, std::string>> Results;
  std::set<va_t> VerifiedCandidates;

  if (Img.Entry != 0 && Img.isFunctionEntryCandidateAt(Img.Entry)) {
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
    func_detect_detail::checkCancellation(Cancellation);
    if (!Img.isFunctionEntryCandidateAt(Exp.Addr))
      continue;
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
  for (va_t EHEntry : Img.ExceptionMetadata.PrimaryFunctionEntries)
    if (EHEntry != 0 && Img.isFunctionEntryCandidateAt(EHEntry))
      ExceptionEntries.insert(EHEntry);
  const std::set<va_t> ExceptionThunks =
      Img.ExceptionMetadata.registrationScopeThunks();
  const auto RegistrationPointerRoles =
      coff_loader::getCheckedX86RegistrationPointerRoles(Img);
  std::set<va_t> IndependentEntries = Entries;
  IndependentEntries.insert(ExceptionEntries.begin(), ExceptionEntries.end());
  // A fragment that continues another function (Img.ContinuationCodeStarts)
  // begins none, though a symbol names it (`foo.cold`).
  for (const auto &Sym : Img.Symbols)
    if (Sym.IsFunc && Sym.Origin != NameOrigin::Synthesized &&
        Img.isFunctionEntryCandidateAt(Sym.Addr))
      IndependentEntries.insert(Sym.Addr);

  for (const auto &Sym : Img.Symbols) {
    func_detect_detail::checkCancellation(Cancellation);
    if (!Sym.IsFunc || !Img.isFunctionEntryCandidateAt(Sym.Addr))
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
    if (Sym.Origin == NameOrigin::Synthesized && RegistrationPointerRoles &&
        RegistrationPointerRoles->RuntimeOnlyPointerTargets.count(Sym.Addr))
      continue;
    if (Img.hasExecutableCodeOwnerAt(Sym.Addr)) {
      Entries.insert(Sym.Addr);
      // As the function is listed: a name lower functions take is numbered.
      Results.push_back({Sym.Addr, Img.functionListName(Sym.Addr, Sym.Name)});
    }
  }

  for (va_t Addr : ExceptionEntries) {
    func_detect_detail::checkCancellation(Cancellation);
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
      if (Sym.IsFunc && Img.isFunctionEntryCandidateAt(Sym.Addr))
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
  const bool IsX64LinkedCOFF = Img.Arch == Arch::X64 &&
                               Img.Format == BinaryFormat::COFF &&
                               !Img.IsRelocatable;
  std::set<va_t> RelocationCodeTargets;
  // What code names, as (naming address, code named): decided below, once the
  // entries that bound each naming function are known.
  std::vector<std::pair<va_t, va_t>> CodeNamedByCode;
  const uint32_t PointerSize = Img.getPointerSize();
  if (PointerSize != 0)
    for (va_t Slot : Img.CodePtrRelocSlots) {
      func_detect_detail::checkCancellation(Cancellation);
      const uint8_t *Bytes = Img.readVA(Slot, PointerSize);
      if (!Bytes)
        continue;
      const va_t Target = normalizeCodeAddress(readPtr(Bytes, Img.is64Bit()),
                                               Img.Arch, Img.Mode);
      if (RegistrationPointerRoles)
        if (const auto Source = RegistrationPointerRoles->Sources.find(Slot);
            Source != RegistrationPointerRoles->Sources.end() &&
            Source->second == Target)
          continue;
      if (Img.DataInCode.count(Target) ||
          !Img.isFunctionEntryCandidateAt(Target) || IsMachOLocalLabel(Target))
        continue;
      if (IsX86LinkedCOFF && Img.hasExecutableCodeOwnerAt(Slot)) {
        CodeNamedByCode.emplace_back(Slot, Target);
        continue;
      }
      Entries.insert(Target);
      RelocationCodeTargets.insert(Target);
    }

  // A table among the code names its dispatcher's cases.  What names them is
  // the instruction reading the table, whose operand the loader records as
  // naming data, rather than the table's own slots: an entry between the
  // cases and the table (a prologue match in dead code before it) does not
  // bound the dispatcher.  A table no instruction is known to read keeps its
  // slots as the namers.
  if (IsX86LinkedCOFF && PointerSize != 0 && !CodeNamedByCode.empty()) {
    auto runOf = [&](va_t Addr) -> std::optional<va_t> {
      auto It = Img.DataInCode.upper_bound(Addr);
      if (It == Img.DataInCode.begin())
        return std::nullopt;
      const va_t Start = *std::prev(It);
      va_t End = Start;
      while (End <= Addr && (Img.CodePtrRelocSlots.count(End) ||
                             Img.DataPtrRelocSlots.count(End)))
        End += PointerSize;
      if (Addr >= End)
        return std::nullopt;
      return Start;
    };
    std::map<va_t, std::vector<va_t>> Readers;
    for (const auto &[Field, Named] : Img.DataAddressRelocOperands)
      if (Img.hasExecutableCodeOwnerAt(Field))
        if (const auto Run = runOf(Named.TargetVA))
          Readers[*Run].push_back(Field);
    if (!Readers.empty()) {
      std::vector<std::pair<va_t, va_t>> ByReader;
      ByReader.reserve(CodeNamedByCode.size());
      for (const auto &[Slot, Target] : CodeNamedByCode) {
        func_detect_detail::checkCancellation(Cancellation);
        const auto Run = runOf(Slot);
        const auto It = Run ? Readers.find(*Run) : Readers.end();
        if (It == Readers.end()) {
          ByReader.emplace_back(Slot, Target);
          continue;
        }
        for (va_t Reader : It->second)
          ByReader.emplace_back(Reader, Target);
      }
      CodeNamedByCode = std::move(ByReader);
    }
  }

  std::set<va_t> DirectCallTargets;
  // Functions a binary file's code takes the address of, whose prologue
  // already vouches for them.
  std::set<va_t> CodePointerTargets;
  // The entries other evidence found before the prologue scan adds its own.
  std::set<va_t> EvidencedX86Entries;
  if (Img.Entry != 0 && !RelocationsOnly) {
    scanCallTargets(Img, Dec, DirectCallTargets, CodeNamedByCode);
    std::erase_if(DirectCallTargets, [&](va_t Addr) {
      return !Img.isFunctionEntryCandidateAt(Addr);
    });
    Entries.insert(DirectCallTargets.begin(), DirectCallTargets.end());
    // A binary file has no symbols, unwind tables or relocations to name
    // the functions its code only takes the address of.  A target starts
    // with a prologue, so its decode below need only not fail: a function
    // such as main runs far past the verifier's window before it returns.
    if (Img.Format == BinaryFormat::Raw && Img.Arch == Arch::X64) {
      func_detect_detail::scanCodePointersX64(Img, Dec, CodePointerTargets,
                                              Cancellation);
      Entries.insert(CodePointerTargets.begin(), CodePointerTargets.end());
    }
    if (Img.Arch == Arch::ARM)
      Entries.insert(Img.ARMVeneerTargets.begin(), Img.ARMVeneerTargets.end());
    if (IsX86LinkedCOFF) {
      EvidencedX86Entries = Entries;
      scanX86UnsymbolizedEntries(Img, Dec, Entries);
    }
  }

  // Reject structurally ineligible roots before they can delimit another
  // function's body. Every evidence source uses the image's same policy.
  std::erase_if(Entries, [&](va_t Addr) {
    return !Img.isFunctionEntryCandidateAt(Addr);
  });
  llvm::erase_if(Results, [&](const auto &R) {
    return !Img.isFunctionEntryCandidateAt(R.first);
  });

  if (RegistrationPointerRoles)
    for (va_t Target : RegistrationPointerRoles->RuntimeOnlyPointerTargets)
      if (!IndependentEntries.count(Target) && !DirectCallTargets.count(Target))
        Entries.erase(Target);

  for (va_t Thunk : ExceptionThunks)
    Entries.erase(Thunk);

  // A prologue match that a function with other evidence reaches as its own
  // code is that function's body, and so is a start the loader only guessed
  // from the padding before it: an int3 byte inside an instruction (a
  // `jne` whose displacement is 0xcc) reads as padding too.  Drop both
  // before the entries bound the functions that name their own labels below.
  if (IsX86LinkedCOFF && Img.Entry != 0) {
    std::set<va_t> Stated{Img.Entry};
    for (const auto &Exp : Img.Exports)
      Stated.insert(Exp.Addr);
    for (const auto &Sym : Img.Symbols)
      if (Sym.IsFunc && !Sym.IsBoundaryGuess)
        Stated.insert(Sym.Addr);
    std::set<va_t> Guessed;
    for (const auto &Sym : Img.Symbols)
      if (Sym.IsFunc && Sym.IsBoundaryGuess && Entries.count(Sym.Addr) &&
          !Stated.count(Sym.Addr) && !ExceptionEntries.count(Sym.Addr) &&
          !DirectCallTargets.count(Sym.Addr) &&
          !RelocationCodeTargets.count(Sym.Addr))
        Guessed.insert(Sym.Addr);
    std::set<va_t> Evidenced;
    for (va_t Entry : EvidencedX86Entries)
      if (Entries.count(Entry) && !Guessed.count(Entry))
        Evidenced.insert(Entry);
    const std::set<va_t> Interior =
        rejectX86InteriorEntries(Img, Dec, Evidenced, Guessed, CodeNamedByCode);
    if (!Interior.empty())
      llvm::erase_if(Results, [&](const std::pair<va_t, std::string> &R) {
        return Interior.count(R.first) != 0;
      });
  }

  // Code that other code names is a function of its own, except inside the
  // function naming it, where it is that function's label: a pointer table
  // among the code names its dispatcher's cases, which the jump-table
  // resolver reads, and a relocated immediate (`mov [obj+off], offset cb`, a
  // callback stored into an object; `push offset filter`, a handler) names a
  // function the code only takes the address of.  The bounded decoder below
  // still decides whether a target is an entry.
  std::set<va_t> CodeNamedTargets;
  if ((IsX86LinkedCOFF || IsX64LinkedCOFF) && Img.Entry != 0) {
    const size_t TableNames = IsX86LinkedCOFF ? CodeNamedByCode.size() : 0;
    if (IsX86LinkedCOFF && !RelocationsOnly)
      func_detect_detail::scanCodeAddressImmediatesX86(
          Img, Dec, CodeNamedByCode, Cancellation);
    const std::vector<va_t> Starts(Entries.begin(), Entries.end());
    // The function an address lies in: the entry before it, and the next.
    auto functionOf = [&](va_t Addr) -> std::optional<std::pair<va_t, va_t>> {
      const auto Next = std::upper_bound(Starts.begin(), Starts.end(), Addr);
      if (Next == Starts.begin())
        return std::nullopt;
      return std::make_pair(*std::prev(Next),
                            Next == Starts.end() ? InvalidVA : *Next);
    };
    auto NamesOwnLabel = [&](va_t Namer, va_t Target) {
      const auto Function = functionOf(Namer);
      return Function && Function->first < Target && Target < Function->second;
    };
    // Code whose address an instruction pushes or stores is a label only
    // where its function's code reaches it.  Code past the function's own
    // returns, which nothing in it reaches (a constructor pushed for an
    // array iterator, a callback for atexit), is a function the code takes
    // the address of.  A function with an indirect jump the walk cannot
    // follow may reach any label, as a computed goto does, and an address
    // loaded into a register, such as a catch block's continuation, keeps
    // the label it names.
    std::map<va_t, std::vector<va_t>> FunctionCases;
    for (size_t I = 0; I < TableNames; ++I)
      if (const auto Function = functionOf(CodeNamedByCode[I].first);
          Function &&
          NamesOwnLabel(CodeNamedByCode[I].first, CodeNamedByCode[I].second))
        FunctionCases[Function->first].push_back(CodeNamedByCode[I].second);
    std::map<va_t, std::optional<std::set<va_t>>> Reached;
    auto Reaches = [&](va_t Namer, va_t Target) {
      const auto Function = functionOf(Namer);
      auto [It, Inserted] = Reached.try_emplace(Function->first);
      if (Inserted) {
        const auto Cases = FunctionCases.find(Function->first);
        bool Computed = false;
        std::set<va_t> Code = x86OwnCodeReached(
            Img, Dec, Function->first, Function->second,
            Cases == FunctionCases.end() ? llvm::ArrayRef<va_t>()
                                         : llvm::ArrayRef<va_t>(Cases->second),
            Computed);
        if (!Computed)
          It->second = std::move(Code);
      }
      return !It->second || It->second->count(Target) != 0;
    };
    std::set<va_t> Labels;
    for (size_t I = 0; I < CodeNamedByCode.size(); ++I) {
      func_detect_detail::checkCancellation(Cancellation);
      const auto &[Namer, Target] = CodeNamedByCode[I];
      if (NamesOwnLabel(Namer, Target) &&
          (IsX64LinkedCOFF || I < TableNames ||
           !func_detect_detail::isX86EscapingCodeImmediate(Img, Namer) ||
           Reaches(Namer, Target)))
        Labels.insert(Target);
    }
    for (const auto &Named : CodeNamedByCode) {
      func_detect_detail::checkCancellation(Cancellation);
      const va_t Target = Named.second;
      // An instruction starts with its opcode, never with a relocated field.
      if (Entries.count(Target) || Labels.count(Target) ||
          ExceptionThunks.count(Target) ||
          Img.CodePtrRelocSlots.count(Target) ||
          Img.DataPtrRelocSlots.count(Target) ||
          Img.CodeAddressRelocOperands.count(Target) ||
          Img.DataAddressRelocOperands.count(Target) ||
          Img.DataInCode.count(Target) ||
          !Img.isFunctionEntryCandidateAt(Target))
        continue;
      if (RegistrationPointerRoles &&
          RegistrationPointerRoles->RuntimeOnlyPointerTargets.count(Target))
        continue;
      CodeNamedTargets.insert(Target);
    }
    Entries.insert(CodeNamedTargets.begin(), CodeNamedTargets.end());
  }

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
    func_detect_detail::checkCancellation(Cancellation);
    if (ExceptionThunks.count(Addr))
      continue;
    if (Already.insert(Addr).second)
      Results.push_back(
          {Addr, (kAutoFuncPrefix + llvm::utohexstr(Addr)).str()});
  }

  if (Img.Entry != 0 || RelocationsOnly) {
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
      func_detect_detail::checkCancellation(Cancellation);
      va_t Addr = Results[I].first;
      if ((RelocationsOnly && !RelocationCodeTargets.count(Addr)) ||
          Img.VerifiedFunctionEntries.count(
              normalizeCodeAddress(Addr, Img.Arch, Img.Mode)) ||
          Trusted.count(Addr) || UnsymbolizedX86Entries.count(Addr) ||
          (!RelocationsOnly && IsX86LinkedCallOrRelocTarget(Addr)))
        Keep[I] = 1;
      else if ((RelocationsOnly && IsX86LinkedCallOrRelocTarget(Addr)) ||
               UntypedCOFFExports.count(Addr) ||
               IsCoveredMachODirectCallTarget(Addr) ||
               IsCoveredImportCallTarget(Addr) ||
               ExplicitFunctionStarts.count(
                   normalizeCodeAddress(Addr, Img.Arch, Img.Mode)) != 0 ||
               !InsideKnownButNotStart(Addr)) {
        if (RelocationsOnly)
          Keep[I] = 1;
        else
          NeedVerify.push_back(I);
      }
    }

    // The trial decode dominates only when the scan produced many untrusted
    // candidates; each check is independent and reads only the immutable image,
    // so spread that subset across worker threads with per-thread decoders.  A
    // symbol-rich binary (almost everything trusted) leaves NeedVerify small
    // and stays single-threaded, avoiding pointless thread-spawn overhead.
    auto verifyIdx = [&](Decoder &LocalDec, size_t I) {
      const va_t Addr = Results[I].first;
      Keep[I] = verifyFunctionDecode(
                    Img, LocalDec, Addr,
                    UntypedCOFFExports.count(Addr) != 0 ||
                        UnsymbolizedX86Entries.count(Addr) != 0 ||
                        IsX86LinkedCallOrRelocTarget(Addr) ||
                        CodePointerTargets.count(Addr) != 0 ||
                        (!IsX64LinkedCOFF && CodeNamedTargets.count(Addr) != 0),
                    IsX64LinkedCOFF && CodeNamedTargets.count(Addr) != 0)
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

    // A function whose whole body is one direct unconditional jump, after an
    // optional indirect-branch-tracking marker, passes its caller's return
    // address, stack and arguments on untouched, so the jump target is
    // callable on the same evidence.  GCC's frame_dummy (`endbr64; jmp
    // register_tm_clones`) is the usual case: in a stripped image nothing else
    // names the target, and the forwarder would otherwise absorb its body.  A
    // target between the forwarder and the next entry is the forwarder's own
    // code, such as a loop entered at its test, and stays inside it.
    if (!RelocationsOnly && (Img.Arch == Arch::X86 || Img.Arch == Arch::X64)) {
      std::set<va_t> Starts;
      for (const auto &R : Results)
        Starts.insert(R.first);
      const bool PreviousDetail = Dec.detailEnabled();
      // Every target is a new start, so a chain of forwarders ends.
      std::vector<va_t> Forwarders(Starts.begin(), Starts.end());
      while (!Forwarders.empty()) {
        func_detect_detail::checkCancellation(Cancellation);
        std::vector<va_t> Targets;
        for (va_t Forwarder : Forwarders) {
          func_detect_detail::checkCancellation(Cancellation);
          Dec.setDetail(true);
          Dec.resetX86FpuState();
          const va_t Target = forwardingJumpTarget(Img, Dec, Forwarder);
          if (Target == InvalidVA || Starts.count(Target) ||
              Img.findImportAt(Target) || !Img.hasExecutableCodeOwnerAt(Target))
            continue;
          const auto Next = Starts.upper_bound(Forwarder);
          if (Target > Forwarder && (Next == Starts.end() || Target < *Next))
            continue;
          if (!verifyFunctionDecode(Img, Dec, Target))
            continue;
          Starts.insert(Target);
          Entries.insert(Target);
          VerifiedCandidates.insert(
              normalizeCodeAddress(Target, Img.Arch, Img.Mode));
          Results.push_back(
              {Target, (kAutoFuncPrefix + llvm::utohexstr(Target)).str()});
          Targets.push_back(Target);
        }
        Forwarders = std::move(Targets);
      }
      Dec.resetX86FpuState();
      Dec.setDetail(PreviousDetail);
    }
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
      func_detect_detail::checkCancellation(Cancellation);
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
        func_detect_detail::checkCancellation(Cancellation);
        if (InsideSized(R.first) && !FunctionSymbolStarts.count(R.first) &&
            !IsCoveredMachODirectCallTarget(R.first) &&
            !IsCoveredImportCallTarget(R.first))
          continue;
        Filtered.push_back(R);
      }
      Results = std::move(Filtered);
    }
  }

  std::sort(Results.begin(), Results.end());
  func_detect_detail::checkCancellation(Cancellation);

  // Publish only verified candidates that survived every candidate and
  // overlap filter. A transient decoder hit must never become patch-time
  // function identity after this routine has rejected it.
  for (const auto &[Addr, Name] : Results) {
    (void)Name;
    const va_t Normalized = normalizeCodeAddress(Addr, Img.Arch, Img.Mode);
    if (RelocationsOnly && RelocationCodeTargets.count(Normalized))
      RelocatedCandidates.insert(Normalized);
    else if (VerifiedCandidates.count(Normalized))
      Img.VerifiedFunctionEntries.insert(Normalized);
  }

  return Results;
}

//===----------------------------------------------------------------------===//
// verifyFunctionDecode
//===----------------------------------------------------------------------===//

bool FuncDetector::verifyFunctionDecode(const BinaryImage &Img, Decoder &Dec,
                                        va_t Addr, bool KeepInconclusive,
                                        bool RequireCallableExit) {
  if (!Img.isFunctionEntryCandidateAt(Addr))
    return false;
  constexpr int kMaxInsns = limits::kMaxVerifyInsns;
  const auto *Seg = Img.getSegmentFor(Addr);
  if (!Seg || !Img.hasExecutableCodeOwnerAt(Addr))
    return false;

  va_t Cur = Addr;
  std::optional<InstructionMode> PathMode = Img.instructionModeAt(Addr);
  bool SawTerminator = false;
  for (int I = 0; I < kMaxInsns; ++I) {
    func_detect_detail::checkCancellation(Cancellation);
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
  // A long body can return just beyond this linear prefix, or through a
  // conditional arm. Let the existing reachable probe establish a callable
  // exit before retaining that candidate. An exhausted or unsupported probe
  // cannot stand in for the missing evidence.
  const bool NeedsCallableExit =
      RequireCallableExit ||
      (!SawTerminator && !KeepInconclusive &&
       !hasBoundedSemanticTerminator(Img, Dec, Addr, Cancellation));

  // A linear walk can encounter RET on one arm while a conditional branch on
  // another arm lands in embedded executable data.  That pattern is common in
  // stripped images whose executable sections contain strings or tables: a
  // byte sequence in the data may also spell CALL, creating a bogus candidate
  // that the straight-line probe above would accept.  Validate direct reachable
  // arms before promoting an untrusted scan hit to a function.
  //
  // This deliberately remains a bounded, decode-focused probe rather than a
  // second full CFGBuilder run. When existing entry evidence permits an
  // inconclusive probe, keep the candidate for the formal pipeline audit.
  // Otherwise a missing callable exit must be established within this bound.
  // One observed exit establishes candidate identity, not whole-path coverage;
  // other indirect frontiers still belong to the formal pipeline audit.
  const bool PreviousDetail = Dec.detailEnabled();
  Dec.setDetail(true);
  Dec.resetX86FpuState();
  const bool ReachablePathsDecode = [&]() {
    bool SawCallableExit = false;
    constexpr size_t kCFGProbeBudget =
        static_cast<size_t>(limits::kMaxVerifyInsns) * 4;
    std::queue<std::pair<va_t, std::optional<InstructionMode>>> Worklist;
    std::map<va_t, InstructionMode> Explored;
    Worklist.push({Addr, Img.instructionModeAt(Addr)});

    while (!Worklist.empty()) {
      func_detect_detail::checkCancellation(Cancellation);
      auto [Cur, PathMode] = Worklist.front();
      Worklist.pop();

      while (true) {
        func_detect_detail::checkCancellation(Cancellation);
        if (const auto Existing = Explored.find(Cur);
            Existing != Explored.end()) {
          if (Img.Arch == Arch::ARM &&
              Img.instructionModeAt(Cur, PathMode) != Existing->second)
            return false;
          break;
        }
        if (Explored.size() >= kCFGProbeBudget)
          return !NeedsCallableExit;

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
          return !NeedsCallableExit;
        }
        const bool IsNoReturnCall = isUnconditionalNoReturnCall(Img, Ops);

        bool IsBranch = false;
        bool IsCond = false;
        bool IsRet = false;
        bool IsIndirect = false;
        va_t BranchTarget = InvalidVA;
        for (const LowOp &Op : Ops) {
          func_detect_detail::checkCancellation(Cancellation);
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
            SawCallableExit = true;
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

        if (IsNoReturnCall) {
          SawCallableExit = true;
          break;
        }
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
          return !NeedsCallableExit;
        if (BranchTarget != Addr && Entries.count(BranchTarget) > 0) {
          if (NeedsCallableExit &&
              !Img.hasAuthenticatedFunctionEntryAt(BranchTarget)) {
            Worklist.push({BranchTarget, BranchMode});
            if (!IsCond)
              break;
            Cur += static_cast<va_t>(Sz);
            continue;
          }
          if (IsCond) {
            Cur += static_cast<va_t>(Sz);
            continue;
          }
          SawCallableExit = true;
          break;
        }

        Worklist.push({BranchTarget, BranchMode});
        if (!IsCond)
          break;
        Cur += static_cast<va_t>(Sz);
      }
    }
    return !NeedsCallableExit || SawCallableExit;
  }();
  Dec.resetX86FpuState();
  Dec.setDetail(PreviousDetail);
  return ReachablePathsDecode;
}

//===----------------------------------------------------------------------===//
// Call-target scanning
//===----------------------------------------------------------------------===//

void FuncDetector::scanCallTargets(
    const BinaryImage &Img, Decoder &Dec, std::set<va_t> &Out,
    std::vector<std::pair<va_t, va_t>> &CodeAddresses) {
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
      func_detect_detail::checkCancellation(Cancellation);
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
  // A sectionless mapping can start with what is no code, as an executable
  // ELF's one PT_LOAD maps its headers: a sweep through them may come out of
  // step and decode across the first instructions.  The entry point is an
  // instruction, so the sweep starts again there.
  for (const Segment &Seg : Img.Segments)
    if (!Img.segmentHasReadableSectionMetadata(Seg)) {
      const va_t End = Seg.VA + Seg.Data.size();
      const va_t Entry = normalizeCodeAddress(Img.Entry, Img.Arch, Img.Mode);
      if (Img.Entry != 0 && Entry > Seg.VA && Entry < End) {
        AddRange(&Seg, Seg.VA, Entry - Seg.VA);
        AddRange(&Seg, Entry, End - Entry);
      } else
        AddRange(&Seg, Seg.VA, Seg.Data.size());
    }

  // An x86 sweep reads sizes and ids, call targets and linked PE code
  // addresses.
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
      func_detect_detail::scanSegmentCalls(Img, Dec, Seg, Start, End, Out,
                                           CodeAddresses, Cancellation);
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
      func_detect_detail::checkCancellation(Cancellation);
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
  // A step's start is the preceding step's end (or the piece start). Keep
  // only those ends and the sparse calls, rather than three VAs per decoded
  // instruction. Deferred subranges need boundaries, not a second call log.
  struct PieceScan {
    std::vector<va_t> Ends;
    std::vector<std::pair<va_t, va_t>> Calls;
    std::vector<std::pair<va_t, va_t>> CodeAddresses;
  };
  std::vector<PieceScan> PieceSteps(PieceJobs.size());

  auto Worker = [&]() {
    Decoder LocalDec;
    if (!LocalDec.init(Img))
      return;
    if (SweepWithoutDetail) {
      LocalDec.setDetail(false);
      LocalDec.setText(false);
    }
    std::set<va_t> LocalEntries;
    std::vector<std::pair<va_t, va_t>> LocalCodeAddresses;

    while (true) {
      func_detect_detail::checkCancellation(Cancellation);
      const size_t Job = NextJob.fetch_add(1, std::memory_order_relaxed);
      if (Job < PieceJobs.size()) {
        const PieceJob &Piece = PieceJobs[Job];
        func_detect_detail::CodeInterval Known =
            func_detect_detail::codeIntervalAround(Img, Piece.Start);
        for (va_t Cur = Piece.Start; Cur < Piece.End;) {
          func_detect_detail::checkCancellation(Cancellation);
          const auto Step = func_detect_detail::stepCallsX86(
              Img, LocalDec, Piece.Seg, Cur, Piece.RangeEnd, Known);
          if (!Step)
            break;
          PieceSteps[Job].Ends.push_back(Step->Next);
          if (Step->Target != InvalidVA)
            PieceSteps[Job].Calls.emplace_back(Step->Addr, Step->Target);
          if (Step->CodeTarget != InvalidVA)
            PieceSteps[Job].CodeAddresses.emplace_back(Step->Addr,
                                                       Step->CodeTarget);
          Cur = Step->Next;
        }
        continue;
      }
      if (Job >= PieceJobs.size() + Chunks.size())
        break;
      auto &[Seg, Start, End] = Chunks[Job - PieceJobs.size()];
      func_detect_detail::scanSegmentCalls(Img, LocalDec, Seg, Start, End,
                                           LocalEntries, LocalCodeAddresses,
                                           Cancellation);
    }

    std::lock_guard<std::mutex> Lk(Mtx);
    Out.insert(LocalEntries.begin(), LocalEntries.end());
    CodeAddresses.insert(CodeAddresses.end(), LocalCodeAddresses.begin(),
                         LocalCodeAddresses.end());
  };

  std::vector<std::thread> Ts;
  Ts.reserve(ThreadsN);
  for (unsigned T = 0; T < ThreadsN; ++T)
    Ts.emplace_back([&] {
      try {
        Worker();
      } catch (const func_detect_detail::DiscoveryCancelled &) {
      }
    });
  for (auto &T : Ts)
    T.join();
  func_detect_detail::checkCancellation(Cancellation);

  // The targets the joined sweeps find, added to \p Out in order at the end.
  std::vector<va_t> Found;
  std::vector<std::vector<va_t>> RangeSteps(SplitRanges.size());
  for (size_t RangeIndex = 0; RangeIndex < SplitRanges.size(); ++RangeIndex) {
    func_detect_detail::checkCancellation(Cancellation);
    const SplitRange &Range = SplitRanges[RangeIndex];
    auto &Taken = RangeSteps[RangeIndex];
    size_t PieceStepCount = 0;
    for (size_t Piece = Range.FirstPiece;
         Piece < Range.FirstPiece + Range.Pieces; ++Piece)
      PieceStepCount += PieceSteps[Piece].Ends.size();
    Taken.reserve(PieceStepCount);
    va_t Pos = Range.Start;
    func_detect_detail::CodeInterval Known =
        func_detect_detail::codeIntervalAround(Img, Pos);
    for (size_t Piece = Range.FirstPiece;
         Piece < Range.FirstPiece + Range.Pieces; ++Piece) {
      func_detect_detail::checkCancellation(Cancellation);
      auto &Scan = PieceSteps[Piece];
      const auto &Steps = Scan.Ends;
      auto It = Steps.begin();
      if (Pos > PieceJobs[Piece].Start) {
        It = std::lower_bound(Steps.begin(), Steps.end(), Pos);
        if (It != Steps.end())
          ++It;
      }
      const auto AddressOf = [&] {
        return It == Steps.begin() ? PieceJobs[Piece].Start : *std::prev(It);
      };
      // Sweep on until this piece's sweep is reached.
      while (Pos < PieceJobs[Piece].End &&
             (It == Steps.end() || AddressOf() != Pos)) {
        func_detect_detail::checkCancellation(Cancellation);
        const auto Step = func_detect_detail::stepCallsX86(
            Img, Dec, Range.Seg, Pos, Range.End, Known);
        if (!Step) {
          Pos = Range.End;
          break;
        }
        if (Step->Target != InvalidVA)
          Found.push_back(Step->Target);
        if (Step->CodeTarget != InvalidVA)
          CodeAddresses.emplace_back(Step->Addr, Step->CodeTarget);
        Taken.push_back(Step->Next);
        Pos = Step->Next;
        while (It != Steps.end() && AddressOf() < Pos)
          ++It;
      }
      if (It != Steps.end() && AddressOf() == Pos) {
        func_detect_detail::checkCancellation(Cancellation);
        const auto Call = std::lower_bound(
            Scan.Calls.begin(), Scan.Calls.end(), Pos,
            [](const auto &C, va_t Address) { return C.first < Address; });
        for (auto C = Call; C != Scan.Calls.end(); ++C) {
          func_detect_detail::checkCancellation(Cancellation);
          Found.push_back(C->second);
        }
        const auto Address = std::lower_bound(
            Scan.CodeAddresses.begin(), Scan.CodeAddresses.end(), Pos,
            [](const auto &C, va_t Address) { return C.first < Address; });
        CodeAddresses.insert(CodeAddresses.end(), Address,
                             Scan.CodeAddresses.end());
        Taken.insert(Taken.end(), It, Steps.end());
        Pos = Steps.back();
      }
      // The canonical boundaries are now owned by Taken. Release each piece
      // before joining the next range rather than retaining both full logs.
      Scan = {};
    }
  }
  llvm::sort(Found);
  auto Hint = Out.begin();
  for (va_t Target : Found)
    Hint = std::next(Out.insert(Hint, Target));

  for (const Deferred &Item : DeferredChunks) {
    func_detect_detail::checkCancellation(Cancellation);
    const auto &Taken = RangeSteps[Item.Range];
    const ScanChunk &Chunk = Item.Chunk;
    va_t From = Chunk.Start;
    const auto Start =
        std::lower_bound(Taken.begin(), Taken.end(), Chunk.Start);
    if (Chunk.Start == SplitRanges[Item.Range].Start ||
        (Start != Taken.end() && *Start == Chunk.Start)) {
      // The last step the long sweep takes before the chunk's end.
      const auto Last = std::lower_bound(Start, Taken.end(), Chunk.End);
      if (Last == Taken.end() || *Last <= Chunk.End)
        continue;
      From = Last == Taken.begin() ? SplitRanges[Item.Range].Start
                                   : *std::prev(Last);
    }
    func_detect_detail::scanSegmentCalls(Img, Dec, Chunk.Seg, From, Chunk.End,
                                         Out, CodeAddresses, Cancellation);
  }
  Dec.setDetail(PreviousDetail);
  Dec.setText(PreviousText);
}

} // namespace neverd
