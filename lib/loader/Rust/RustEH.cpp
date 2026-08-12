//===- RustEH.cpp - Rust panic machinery recovery -------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/loader/Rust/RustEH.h"

#include "neverd/Support/BinaryEncoding.h"
#include "neverd/loader/DirectBranch.h"
#include "neverd/loader/LanguageRuntime.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>

#define DEBUG_TYPE "neverd-rust-eh"

namespace neverd::rust_eh {
namespace {

/// The `name` field of an MSVC `TypeDescriptor` that Rust throws through.
/// `library/panic_unwind/src/seh.rs` deliberately does not mangle it, so that
/// C++ cannot catch a Rust panic by declaring `struct rust_panic`.  That makes
/// the literal string the identity: the CRT matches type descriptors by
/// comparing these names rather than by pointer.
constexpr llvm::StringLiteral RustPanicTypeName("rust_panic");

/// Upper bound on the bytes of one function body the panic-site scan reads.
/// A Rust binary statically links its whole dependency graph, so the scan runs
/// over every function in the image and needs a bound that a pathological
/// record cannot escape.
constexpr size_t MaxScannedFunctionBytes = 1u << 20;

//===----------------------------------------------------------------------===//
// Panic runtime entry points
//===----------------------------------------------------------------------===//

/// Classify a runtime entry point from its demangled path.
///
/// The match is on the path's last component, which is the function's own
/// name.  Matching the whole path would classify every member of
/// `std::panicking` as a panic origin, and most of them only inspect a panic
/// that is already in flight -- `panic_count::increase` and
/// `catch_unwind::cleanup` among them.
///
/// Within the leaf the match is by shape rather than by an exact table, because
/// the set of `core::panicking` entries changes with nearly every Rust release
/// and an exact list would quietly stop recognizing sites the next one renames.
std::optional<RustPanicKind> classifyPanicName(llvm::StringRef Path) {
  // `_Unwind_Resume` continues a panic that is already in flight.  It is the
  // tail of every cleanup pad, so treating it as an origin would put a raise
  // edge on every frame that merely runs `Drop` glue.
  if (Path == "_Unwind_Resume" || Path == "__Unwind_Resume")
    return RustPanicKind::Resume;

  llvm::StringRef Leaf = Path;
  if (size_t Separator = Leaf.rfind("::");
      Separator != llvm::StringRef::npos)
    Leaf = Leaf.substr(Separator + 2);
  Leaf.consume_front("__");
  Leaf.consume_front("_");

  // A nounwind check aborts rather than unwinding, so it ends the program at
  // its call site instead of transferring to a landing pad.  These are tested
  // first because they also match the broader patterns below.
  if (Leaf.starts_with("panic_nounwind") || Leaf == "panic_cannot_unwind" ||
      Leaf == "panic_in_cleanup")
    return RustPanicKind::NoUnwind;

  if (Leaf == "panic_bounds_check" || Leaf.ends_with("_index_len_fail") ||
      Leaf.ends_with("_index_order_fail") ||
      Leaf.ends_with("_index_overflow_fail") || Leaf == "slice_index_fail")
    return RustPanicKind::BoundsCheck;

  // `core::panicking::panic_const::panic_const_*` covers every arithmetic
  // check since Rust 1.79; older releases spelled the same checks as plain
  // `panic` calls carrying a static message, which fall through to Explicit.
  if (Leaf.starts_with("panic_const_"))
    return RustPanicKind::Arithmetic;

  // A helper that only tidies up after a panic is not where one begins.
  if (Leaf.ends_with("_cleanup") || Leaf == "cleanup")
    return std::nullopt;

  if (Leaf.starts_with("panic") || Leaf.contains("rust_panic") ||
      Leaf == "rust_begin_unwind" || Leaf == "begin_panic" ||
      Leaf.ends_with("unwrap_failed") || Leaf.ends_with("expect_failed") ||
      Leaf.ends_with("assert_failed") || Leaf == "unreachable_display")
    return RustPanicKind::Explicit;
  return std::nullopt;
}

/// Best available spelling of a symbol: its demangled Rust path when it has
/// one, and the raw name otherwise.  Both are worth trying against the
/// classifier, because the shims `panic_unwind` exports (`rust_begin_unwind`,
/// `__rust_start_panic`) are plain C symbols in some builds and v0-mangled
/// members of the synthetic `__rustc` crate in others.
std::string readablePath(llvm::StringRef Name) {
  std::string Demangled = demangleRustName(Name);
  return Demangled.empty() ? Name.str() : Demangled;
}

/// Every address in the image that a panic edge can target, and what reaching
/// it means.
llvm::DenseMap<va_t, std::pair<RustPanicKind, std::string>>
collectPanicTargets(const BinaryImage &Img) {
  llvm::DenseMap<va_t, std::pair<RustPanicKind, std::string>> Targets;
  auto consider = [&](va_t Rawaddress, llvm::StringRef Name) {
    // An ARM symbol table sets bit 0 of a Thumb function's address to record
    // its instruction set.  A decoded branch target never carries that bit, so
    // the two would never match unless it is dropped here.
    const va_t Address = normalizeCodeAddress(Rawaddress, Img.Arch, Img.Mode);
    if (Address == 0 || Name.empty())
      return;
    std::string Path = readablePath(Name);
    std::optional<RustPanicKind> Kind = classifyPanicName(Path);
    if (!Kind)
      Kind = classifyPanicName(Name);
    if (Kind)
      Targets.try_emplace(Address, std::make_pair(*Kind, std::move(Path)));
  };

  for (const Symbol &S : Img.Symbols)
    consider(S.Addr, S.Name);
  // A panic helper reached through the PLT or an import thunk is named by the
  // stub, not by the symbol, so the stub address is the one a call site holds.
  for (const auto &[StubVA, ImportIndex] : Img.ImportStubIndices)
    if (ImportIndex < Img.Imports.size())
      consider(StubVA, Img.Imports[ImportIndex].Name);
  return Targets;
}

//===----------------------------------------------------------------------===//
// Landing pad classification
//===----------------------------------------------------------------------===//

/// True when \p Index names the type-table slot Rust uses for a catch-all.
/// The Itanium ABI spells `catch (...)` as a null `std::type_info *`, and Rust
/// emits no other kind of catch, so a null slot here is `catch_unwind`.
bool isCatchAllIndex(const ItaniumEHInfo &Info, int64_t Filter) {
  if (Filter <= 0)
    return false;
  for (const ItaniumTypeEntry &Entry : Info.TypeTable)
    if (Entry.Index == static_cast<uint64_t>(Filter))
      return Entry.IsCatchAll;
  // A filter that names no decoded slot cannot be proven to be a catch-all.
  return false;
}

/// True when \p Filter names an exception specification whose type list is
/// empty.  Rust emits `filter []` to mean "this frame must not unwind"; the
/// personality routine responds by aborting instead of searching further.
bool isEmptySpecificationIndex(const ItaniumEHInfo &Info, int64_t Filter) {
  if (Filter >= 0)
    return false;
  const uint64_t Index = static_cast<uint64_t>(-Filter);
  for (const ItaniumExceptionSpec &Spec : Info.ExceptionSpecs)
    if (Spec.Index == Index)
      return Spec.TypeIndices.empty();
  return false;
}

const ItaniumAction *findAction(const ItaniumEHInfo &Info, uint64_t Offset) {
  for (const ItaniumAction &Action : Info.Actions)
    if (Action.TableOffset == Offset)
      return &Action;
  return nullptr;
}

/// Classify one call site's landing pad by walking its action chain.
///
/// A chain can name more than one thing -- a catch preceded by a cleanup is
/// ordinary -- so the classification reports the strongest outcome the chain
/// can reach.  Stopping the panic outranks resuming it, because that is the
/// difference a caller has to reason about; a guard outranks plain cleanup for
/// the same reason.
RustLandingPadKind classifyLandingPad(const ItaniumEHInfo &Info,
                                      const ItaniumCallSite &Site) {
  if (!Site.FirstActionOffset)
    return RustLandingPadKind::DropGlue;

  bool SawGuard = false;
  std::optional<uint64_t> Offset = Site.FirstActionOffset;
  // The chain is a linked list inside a table this decoder already bounded, so
  // the only way to loop is a table that points back at itself.  Bounding the
  // walk by the action count makes that terminate without a visited set.
  for (size_t Step = 0; Offset && Step <= Info.Actions.size(); ++Step) {
    const ItaniumAction *Action = findAction(Info, *Offset);
    if (!Action)
      break;
    if (isCatchAllIndex(Info, Action->TypeFilter))
      return RustLandingPadKind::CatchUnwind;
    SawGuard = SawGuard || isEmptySpecificationIndex(Info, Action->TypeFilter);
    Offset = Action->NextActionOffset;
  }
  return SawGuard ? RustLandingPadKind::NoUnwindGuard
                  : RustLandingPadKind::DropGlue;
}

/// Read the `name` field of an MSVC `TypeDescriptor`.  The record is
/// `{ void *pVFTable; void *spare; char name[]; }`, so the string starts two
/// pointers in.
std::string readTypeDescriptorName(const BinaryImage &Img, va_t DescriptorVA) {
  if (DescriptorVA == 0)
    return {};
  const size_t PointerSize = Img.is64Bit() ? 8 : 4;
  const va_t NameVA = DescriptorVA + 2 * PointerSize;
  // A type descriptor name is a short identifier; a longer read would only
  // walk off into whatever follows the record.
  constexpr size_t MaxNameBytes = 256;
  const uint8_t *Bytes = nullptr;
  for (size_t Length = MaxNameBytes; Length > 0; Length /= 2)
    if ((Bytes = Img.readVA(NameVA, Length)) != nullptr)
      return std::string(reinterpret_cast<const char *>(Bytes),
                         strnlen(reinterpret_cast<const char *>(Bytes), Length));
  return {};
}

//===----------------------------------------------------------------------===//
// Per-function annotation
//===----------------------------------------------------------------------===//

/// Classify the Itanium record on \p F, if it belongs to Rust.
bool annotateItanium(ExceptionFunction &F, RustFunctionEH &EH) {
  if (!F.Itanium || F.Personality != ExceptionPersonality::RustEhPersonality)
    return false;
  for (const ItaniumCallSite &Site : F.Itanium->CallSites) {
    if (Site.LandingPadVA == 0)
      continue;
    RustLandingPad Pad;
    Pad.GuardedRange = Site.GuardedRange;
    Pad.PadVA = Site.LandingPadVA;
    Pad.Kind = classifyLandingPad(*F.Itanium, Site);
    EH.LandingPads.push_back(Pad);
  }
  return true;
}

/// Classify the MSVC record on \p F, if it dispatches Rust panics.
///
/// On `*-pc-windows-msvc` a Rust frame is indistinguishable from a C++ frame
/// by personality alone -- both use `__CxxFrameHandler3`, because LLVM picks
/// the unwind table format from the personality's name.  What separates them
/// is the type a catch names: Rust's `try` intrinsic emits a catch on the
/// unmangled `rust_panic` descriptor, which no C++ catch can name.
bool annotateMSVC(ExceptionFunction &F, const BinaryImage &Img,
                  RustFunctionEH &EH, va_t &PanicTypeDescriptorVA) {
  if (!F.Cxx)
    return false;
  bool IsRust = false;
  for (const CxxTryBlock &Try : F.Cxx->TryBlocks) {
    for (const CxxCatchHandler &Catch : Try.Handlers) {
      if (readTypeDescriptorName(Img, Catch.TypeDescriptorVA) !=
          RustPanicTypeName)
        continue;
      IsRust = true;
      PanicTypeDescriptorVA = Catch.TypeDescriptorVA;
      RustLandingPad Pad;
      Pad.PadVA = Catch.HandlerVA;
      Pad.Kind = RustLandingPadKind::CatchUnwind;
      Pad.GuardedRange = F.CodeRange;
      EH.LandingPads.push_back(Pad);
    }
  }
  if (!IsRust)
    return false;
  EH.UsesMSVCTables = true;
  // A destructor in the unwind map is `Drop` glue: the MSVC tables spell a
  // cleanup as an unwind action rather than as a landing pad, so the pads
  // above would otherwise miss it entirely.
  for (const CxxUnwindAction &Action : F.Cxx->UnwindMap) {
    if (Action.ActionVA == 0)
      continue;
    RustLandingPad Pad;
    Pad.PadVA = Action.ActionVA;
    Pad.Kind = RustLandingPadKind::DropGlue;
    Pad.GuardedRange = F.CodeRange;
    EH.LandingPads.push_back(Pad);
  }
  return true;
}

/// Record every direct branch from \p F's body into the panic runtime.
void collectPanicSites(
    const BinaryImage &Img, const ExceptionAddressRange &Range,
    const llvm::DenseMap<va_t, std::pair<RustPanicKind, std::string>> &Targets,
    RustFunctionEH &EH) {
  if (Targets.empty() || !Range.isValid() ||
      Range.size() > MaxScannedFunctionBytes)
    return;
  const size_t Size = static_cast<size_t>(Range.size());
  const uint8_t *Code = Img.readVA(Range.Begin, Size);
  if (!Code)
    return;
  forEachDirectBranch(Img.Arch, Img.Mode, Code, Size, Range.Begin,
                      [&](va_t SiteVA, va_t TargetVA) {
                        auto It = Targets.find(TargetVA);
                        if (It == Targets.end())
                          return;
                        RustPanicSite Site;
                        Site.CallVA = SiteVA;
                        Site.TargetVA = TargetVA;
                        Site.Kind = It->second.first;
                        Site.TargetName = It->second.second;
                        EH.Panics.push_back(std::move(Site));
                      });
}

/// True when the image links machinery that can raise or continue an unwind.
/// Its absence in an image that is otherwise plainly Rust is what distinguishes
/// `panic=abort` from a build whose panics simply never made it into a table.
bool linksUnwinder(const BinaryImage &Img) {
  auto names = [](llvm::StringRef Name) {
    return Name.contains("_Unwind_RaiseException") ||
           Name.contains("_Unwind_Resume") ||
           Name.contains("rust_eh_personality");
  };
  for (const Symbol &S : Img.Symbols)
    if (names(S.Name))
      return true;
  for (const Import &I : Img.Imports)
    if (names(I.Name))
      return true;
  return false;
}

} // namespace

bool hasRustRuntime(const BinaryImage &Img) {
  // What the image-wide detection already concluded, which is the only thing
  // that works on a PE executable: its names live in a PDB the file does not
  // carry, so the symbol evidence below finds nothing and the string evidence
  // -- a standard library source path in a panic message -- is all there is.
  // That is also the target where Rust needs recognizing most, because there
  // its frames are spelled in the same tables as C++.
  if (Img.ExceptionMetadata.Runtime.is(SourceLanguageRuntime::Rust))
    return true;
  for (const ExceptionFunction &F : Img.ExceptionMetadata.Functions)
    if (F.Personality == ExceptionPersonality::RustEhPersonality)
      return true;
  for (const Symbol &S : Img.Symbols) {
    llvm::StringRef Name(S.Name);
    if (Name.contains("rust_eh_personality") || Name.contains("rust_begin_unwind"))
      return true;
    if (isRustMangledName(Name)) {
      llvm::StringRef Bare = Name;
      Bare.consume_front("_");
      Bare.consume_front("_");
      // Only the standard library proves a Rust *runtime*; a stray mangled
      // symbol could have been linked in from a staticlib whose panics were
      // compiled away.
      if (Bare.starts_with("ZN4core") || Bare.starts_with("ZN3std") ||
          Bare.starts_with("R"))
        return true;
    }
  }
  return false;
}

void parseRustExceptions(BinaryImage &Img) {
  if (!hasRustRuntime(Img))
    return;

  ExceptionInfo &Info = Img.ExceptionMetadata;
  const auto Targets = collectPanicTargets(Img);

  RustRuntimeInfo Runtime;
  for (ExceptionFunction &F : Info.Functions) {
    // A record whose table could not be read cannot be classified from what it
    // does contain: the action chain a classification walks is exactly the
    // part that may be missing.
    if (F.ParseStatus == ExceptionParseStatus::Malformed)
      continue;
    RustFunctionEH EH;
    const bool FromTables =
        annotateItanium(F, EH) |
        static_cast<int>(
            annotateMSVC(F, Img, EH, Runtime.PanicTypeDescriptorVA));
    collectPanicSites(Img, F.CodeRange, Targets, EH);
    // A frame that names no personality has declared no language, and calling
    // the panic runtime is then evidence enough that it is Rust's.  Under
    // `-C panic=abort` it is the only evidence there is: nothing the producer
    // compiled carries a handler, so requiring one leaves every frame it wrote
    // unclassified while the prebuilt standard library beside it -- compiled
    // to unwind, tables and all -- is the only thing that gets read.  A frame
    // that does name one has already said whose it is, and a `throw()`
    // specification under the C++ personality is not a Rust nounwind guard
    // however much the two look alike.
    if (!FromTables && (F.Personality != ExceptionPersonality::None ||
                        !EH.raisesAPanic()))
      continue;
    Runtime.UsesMSVCUnwinding |= EH.UsesMSVCTables;
    Runtime.CleanupFrames += EH.runsDropGlue();
    Runtime.CatchUnwindFrames += EH.catchesUnwind();
    Runtime.NoUnwindGuardFrames += EH.guardsAgainstUnwind();
    Runtime.PanicSites += EH.Panics.size();
    if (EH.hasExceptionalControlFlow())
      F.Rust = std::move(EH);
  }

  // A frame that carries a landing pad settles the question outright.  With no
  // such frame the strategy is only decidable negatively: an image that links
  // nothing able to raise or continue an unwind cannot have been built to
  // unwind, whatever its tables look like.
  const bool AnyPad = Runtime.CleanupFrames != 0 ||
                      Runtime.CatchUnwindFrames != 0 ||
                      Runtime.NoUnwindGuardFrames != 0;
  if (AnyPad || linksUnwinder(Img))
    Runtime.Strategy = RustPanicStrategy::Unwind;
  else if (!Targets.empty())
    Runtime.Strategy = RustPanicStrategy::Abort;

  Info.RustRuntime = Runtime;
  LLVM_DEBUG(llvm::dbgs()
             << "rust-eh: strategy "
             << getRustPanicStrategyName(Runtime.Strategy) << ", "
             << Runtime.CleanupFrames << " cleanup, "
             << Runtime.CatchUnwindFrames << " catch_unwind, "
             << Runtime.NoUnwindGuardFrames << " nounwind, "
             << Runtime.PanicSites << " panic sites\n");
}

} // namespace neverd::rust_eh
