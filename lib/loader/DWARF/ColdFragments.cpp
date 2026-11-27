//===- ColdFragments.cpp - Frames split from their function ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// GCC's hot/cold partitioning (`-freorder-blocks-and-partition`, on at -O2)
/// moves a function's unlikely code to `.text.unlikely` as `foo.cold`, with
/// its own FDE.  Nothing in DWARF links that FDE back to `foo`: it only
/// describes a frame that begins inside an established one, entered by a
/// jump from `foo` with `foo`'s frame rather than by a call.  As a function
/// of its own, the fragment reads its parent's registers and frame as
/// unknown values.
///
/// A fragment is bound to its parent the way an x64 chained unwind record is
/// (COFFUnwind.cpp): the parent owns its range, its start continues the
/// parent rather than beginning a function, and no symbol makes it one.  The
/// parent is the one function whose direct branches enter the fragment with
/// the frame the fragment's rules describe there.
///
/// A frameless function's part has no frame to begin inside: its FDE starts
/// with the entry rule, like a function's.  GCC moves such a function's
/// isolated trap path there (a null dereference it proved, ending in `ud2`).
/// That part is bound only when nothing else makes it a function: no name of
/// its own, no direct call, no RIP-relative address and no relocated code
/// pointer.  An entry-like region that returns is a function tail-called
/// once, and stays one.
///
//===----------------------------------------------------------------------===//

#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/CodePointerSources.h"
#include "neverd/loader/DWARF/ItaniumEH.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringRef.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <optional>
#include <set>
#include <vector>

namespace neverd::dwarf_eh {
namespace {

/// The unwind rules in effect at one address of a frame: where the CFA is,
/// and the CFA offset of each register saved in the frame.
struct CFIRow {
  uint64_t CFARegister = 0;
  int64_t CFAOffset = 0;
  std::map<uint64_t, int64_t> Saved;

  bool operator==(const CFIRow &Other) const {
    return CFARegister == Other.CFARegister && CFAOffset == Other.CFAOffset &&
           Saved == Other.Saved;
  }
};

/// Apply \p Program's rules that take effect at or before \p Offset to
/// \p Row; a CIE's initial program applies whole.  Fails on a rule a row
/// cannot hold, such as a CFA expression.
bool applyCFI(const std::vector<CFIInstruction> &Program,
              std::optional<uint64_t> Offset, const CFIRow &Initial,
              CFIRow &Row) {
  std::vector<CFIRow> Remembered;
  for (const CFIInstruction &I : Program) {
    if (Offset && I.CodeOffset > *Offset)
      break;
    switch (I.Kind) {
    case CFIOpKind::DefCFA:
      Row.CFARegister = I.Register;
      Row.CFAOffset = I.Offset;
      break;
    case CFIOpKind::DefCFARegister:
      Row.CFARegister = I.Register;
      break;
    case CFIOpKind::DefCFAOffset:
      Row.CFAOffset = I.Offset;
      break;
    case CFIOpKind::Offset:
      Row.Saved[I.Register] = I.Offset;
      break;
    case CFIOpKind::Restore:
      if (const auto It = Initial.Saved.find(I.Register);
          It != Initial.Saved.end())
        Row.Saved[I.Register] = It->second;
      else
        Row.Saved.erase(I.Register);
      break;
    case CFIOpKind::SameValue:
    case CFIOpKind::Undefined:
      Row.Saved.erase(I.Register);
      break;
    case CFIOpKind::RememberState:
      Remembered.push_back(Row);
      break;
    case CFIOpKind::RestoreState:
      if (Remembered.empty())
        return false;
      Row = Remembered.back();
      Remembered.pop_back();
      break;
    case CFIOpKind::Nop:
    case CFIOpKind::SetLoc:
    case CFIOpKind::AdvanceLoc:
    case CFIOpKind::GnuArgsSize:
      break;
    default:
      return false;
    }
  }
  return true;
}

/// A DWARF frame and the rules its CIE starts every row from.
struct Frame {
  size_t Index = 0;
  const ExceptionFunction *Function = nullptr;
  const DwarfCIE *CIE = nullptr;
  CFIRow Initial;

  /// The rules in effect at \p Address of this frame.
  std::optional<CFIRow> rowAt(va_t Address) const {
    CFIRow Row = Initial;
    if (!applyCFI(Function->Dwarf->Instructions,
                  Address - Function->CodeRange.Begin, Initial, Row))
      return std::nullopt;
    return Row;
  }
};

/// The frame of \p Sorted, ordered by start, whose code holds \p Address.
const Frame *frameContaining(const std::vector<const Frame *> &Sorted,
                             va_t Address) {
  const auto It = std::upper_bound(
      Sorted.begin(), Sorted.end(), Address,
      [](va_t A, const Frame *F) { return A < F->Function->CodeRange.Begin; });
  if (It == Sorted.begin())
    return nullptr;
  const Frame *F = *std::prev(It);
  return F->Function->CodeRange.contains(Address) ? F : nullptr;
}

/// Whether \p F begins inside an established frame: its first row puts the
/// CFA elsewhere than its CIE's entry rules do, or saves a register they do
/// not (other than the return address).  `_start`'s rule that the return
/// address is undefined is no frame.
bool beginsMidFrame(const Frame &F) {
  if (F.CIE->IsSignalFrame)
    return false;
  const std::optional<CFIRow> First = F.rowAt(F.Function->CodeRange.Begin);
  if (!First)
    return false;
  if (First->CFARegister != F.Initial.CFARegister ||
      First->CFAOffset != F.Initial.CFAOffset)
    return true;
  return llvm::any_of(First->Saved, [&](const auto &Rule) {
    return Rule.first != F.CIE->ReturnAddressRegister &&
           !F.Initial.Saved.count(Rule.first);
  });
}

/// Whether a landing pad of \p F's call sites would make it a function of
/// its own exceptional flow: binding moves its code into the parent, whose
/// call-site table does not hold its pads.
bool hasLandingPads(const ExceptionFunction &F) {
  return F.Itanium &&
         llvm::any_of(F.Itanium->CallSites, [](const ItaniumCallSite &Site) {
           return Site.LandingPadVA != 0;
         });
}

/// Whether \p F's code ends in x86 `ud2`, the trap that ends a path GCC
/// isolated.
bool endsInTrap(const BinaryImage &Img, const Frame &F) {
  const ExceptionAddressRange &Code = F.Function->CodeRange;
  if (Code.End - Code.Begin < 2)
    return false;
  const uint8_t *Bytes = Img.readVA(Code.End - 2, 2);
  return Bytes && Bytes[0] == 0x0F && Bytes[1] == 0x0B;
}

/// Whether \p Name is GCC's name for a part of a function: `foo.cold`, or
/// `foo.cold.N` when one function has several.
bool isColdPartName(llvm::StringRef Name) {
  return Name.ends_with(".cold") || Name.contains(".cold.");
}

/// Visit each code address \p Img's executable code names other than as a
/// jump target: x86 `call rel32` and a RIP-relative `lea`, matched at every
/// offset.  A misaligned match can only add an address, and an address keeps
/// its region a function.
template <typename VisitT>
void forEachNamedCodeAddress(const BinaryImage &Img, VisitT Visit) {
  for (const Segment &Seg : Img.Segments) {
    if (!Seg.isExecutable())
      continue;
    const uint8_t *D = Seg.Data.data();
    const size_t N = Seg.Data.size();
    for (size_t I = 0; I + 5 <= N; ++I) {
      int32_t Rel;
      if (D[I] == 0xE8) {
        std::memcpy(&Rel, D + I + 1, sizeof(Rel));
        Visit(static_cast<va_t>(Seg.VA + I + 5 + static_cast<int64_t>(Rel)));
      } else if (D[I] == 0x8D && I + 6 <= N && (D[I + 1] & 0xC7) == 0x05) {
        // `lea disp32(%rip), r` ends after its displacement, with or without
        // the REX prefix before the opcode.
        std::memcpy(&Rel, D + I + 2, sizeof(Rel));
        Visit(static_cast<va_t>(Seg.VA + I + 6 + static_cast<int64_t>(Rel)));
      }
    }
  }
}

/// Visit each direct branch encoded in \p Img's executable code as
/// (source, target): x86 `jmp rel32` and `jcc rel32`, matched at every
/// offset; the caller's frame check rejects what is not an instruction.
template <typename VisitT>
void forEachDirectBranch(const BinaryImage &Img, VisitT Visit) {
  for (const Segment &Seg : Img.Segments) {
    if (!Seg.isExecutable())
      continue;
    const uint8_t *D = Seg.Data.data();
    const size_t N = Seg.Data.size();
    for (size_t I = 0; I + 5 <= N; ++I) {
      size_t Length = 0;
      if (D[I] == 0xE9)
        Length = 5;
      else if (D[I] == 0x0F && I + 6 <= N && (D[I + 1] & 0xF0) == 0x80)
        Length = 6;
      if (!Length)
        continue;
      int32_t Rel;
      std::memcpy(&Rel, D + I + Length - 4, sizeof(Rel));
      const va_t Source = Seg.VA + I;
      Visit(Source,
            static_cast<va_t>(Source + Length + static_cast<int64_t>(Rel)));
    }
  }
}

} // namespace

void bindColdFragments(BinaryImage &Img) {
  // The x86 targets are the ones GCC partitions by default; an AArch64 or
  // ARM image's fragments stay functions of their own until one shows them.
  if (Img.IsRelocatable || (Img.Arch != Arch::X86 && Img.Arch != Arch::X64))
    return;
  ExceptionInfo &Info = Img.ExceptionMetadata;
  std::vector<Frame> Frames;
  for (size_t Index = 0; Index < Info.Functions.size(); ++Index) {
    const ExceptionFunction &F = Info.Functions[Index];
    if (F.Encoding != ExceptionEncoding::DwarfFDE ||
        F.Kind != RuntimeFunctionKind::Primary || !F.Dwarf ||
        !F.CodeRange.isValid())
      continue;
    const DwarfCIE *CIE = Info.findCIE(F.Dwarf->CIESectionOffset);
    if (!CIE)
      continue;
    Frame Entry{Index, &F, CIE, {}};
    if (!applyCFI(CIE->InitialInstructions, std::nullopt, Entry.Initial,
                  Entry.Initial))
      continue;
    Frames.push_back(std::move(Entry));
  }
  llvm::sort(Frames, [](const Frame &A, const Frame &B) {
    return A.Function->CodeRange.Begin < B.Function->CodeRange.Begin;
  });
  // A part that begins with the entry rule is a candidate only when it ends
  // in a trap and nothing names it as a function: a symbol of its own, a
  // direct call, a RIP-relative address or a relocated code pointer.
  std::set<va_t> TrapParts;
  for (const Frame &F : Frames)
    if (!beginsMidFrame(F) && !hasLandingPads(*F.Function) &&
        endsInTrap(Img, F))
      TrapParts.insert(F.Function->CodeRange.Begin);
  if (!TrapParts.empty()) {
    for (const Symbol &Sym : Img.Symbols)
      if (Sym.IsFunc && TrapParts.count(Sym.Addr) &&
          Sym.Name != Symbol::makeFunc(Sym.Addr).Name &&
          !isColdPartName(Sym.Name))
        TrapParts.erase(Sym.Addr);
    if (!TrapParts.empty())
      forEachNamedCodeAddress(Img,
                              [&](va_t Target) { TrapParts.erase(Target); });
    for (va_t Slot : Img.CodePtrRelocSlots)
      if (const std::optional<va_t> Target =
              normalizedRelocatedCodeTarget(Img, Slot))
        TrapParts.erase(*Target);
  }
  // Fragments whose code a parent can take as its own.
  std::vector<const Frame *> Fragments;
  for (const Frame &F : Frames)
    if (!hasLandingPads(*F.Function) &&
        (beginsMidFrame(F) || TrapParts.count(F.Function->CodeRange.Begin)))
      Fragments.push_back(&F);
  if (Fragments.empty())
    return;

  std::vector<const Frame *> AllFrames;
  AllFrames.reserve(Frames.size());
  for (const Frame &F : Frames)
    AllFrames.push_back(&F);
  const std::set<const Frame *> FragmentSet(Fragments.begin(), Fragments.end());
  // Each fragment's parents: the frames a direct branch enters it from with
  // the frame the fragment's rules describe at the target.
  std::map<const Frame *, std::set<const Frame *>> Parents;
  forEachDirectBranch(Img, [&](va_t Source, va_t Target) {
    const Frame *Fragment = frameContaining(Fragments, Target);
    if (!Fragment)
      return;
    const Frame *From = frameContaining(AllFrames, Source);
    if (!From || From == Fragment || FragmentSet.count(From))
      return;
    const std::optional<CFIRow> AtBranch = From->rowAt(Source);
    const std::optional<CFIRow> AtTarget = Fragment->rowAt(Target);
    if (AtBranch && AtTarget && *AtBranch == *AtTarget)
      Parents[Fragment].insert(From);
  });

  std::set<va_t> Bound;
  for (const auto &[Fragment, From] : Parents) {
    if (From.size() != 1)
      continue;
    const Frame &Parent = **From.begin();
    ExceptionFunction &Child = Info.Functions[Fragment->Index];
    Child.Kind = RuntimeFunctionKind::Fragment;
    Child.PrimaryFunctionIndex = Parent.Index;
    Info.Functions[Parent.Index].FragmentRanges.push_back(Child.CodeRange);
    Img.ContinuationCodeStarts.insert(Child.CodeRange.Begin);
    Bound.insert(Child.CodeRange.Begin);
  }
  for (ExceptionFunction &F : Info.Functions)
    llvm::sort(F.FragmentRanges, [](const ExceptionAddressRange &A,
                                    const ExceptionAddressRange &B) {
      return A.Begin < B.Begin;
    });
  // A fragment begins no function: the automatic names the frame index and
  // the FDEs gave its start go.  A name the image gives it (`foo.cold`)
  // stays, for the code it labels.
  llvm::erase_if(Img.Symbols, [&](const Symbol &Sym) {
    return Sym.IsFunc && Bound.count(Sym.Addr) &&
           Sym.Name == Symbol::makeFunc(Sym.Addr).Name;
  });
}

} // namespace neverd::dwarf_eh
