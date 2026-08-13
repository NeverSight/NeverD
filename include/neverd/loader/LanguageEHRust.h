//===- LanguageEHRust.h - Rust panic machinery ----------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Normalized Rust panic records.  Rust borrows the Itanium LSDA and, on the
/// MSVC targets, the C++ `FuncInfo`, so what these carry is the reading of a
/// shared table: what each landing pad is for, what each call into the panic
/// runtime does, and how an image responds to a panic at all.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LOADER_LANGUAGEEHRUST_H
#define NEVERD_LOADER_LANGUAGEEHRUST_H

#include "neverd/loader/ExceptionCommon.h"

#include <cstdint>
#include <string>
#include <vector>

namespace neverd {

/// What a Rust landing pad is for.
///
/// Rust has no `catch` in the C++ sense.  A panic unwinds, running `Drop` glue
/// on the way out, and is stopped in exactly one place: the pad
/// `std::panic::catch_unwind` compiles to.  Everything else is either that
/// cleanup or a boundary that is not permitted to unwind at all.  The three
/// cases are told apart by the action a call site names, which is why this
/// classification is a reading of the LSDA rather than a guess about the code.
enum class RustLandingPadKind : uint8_t {
  /// The call site names only cleanup actions: the pad runs `Drop` glue and
  /// resumes the panic.
  DropGlue,
  /// The call site names a catch whose type-table slot is null.  Rust never
  /// emits a typed catch, so a catch-all is a `catch_unwind` boundary and the
  /// only place a panic stops.
  CatchUnwind,
  /// The call site names an exception specification whose type list is empty.
  /// The Itanium ABI resolves that by calling the unexpected handler instead
  /// of unwinding, and Rust uses it to spell "this frame must not unwind" --
  /// the handler aborts.  An `extern "C"` boundary compiles to exactly this.
  NoUnwindGuard,
};

const char *getRustLandingPadKindName(RustLandingPadKind Kind);

/// One classified landing pad and the region it serves.
struct RustLandingPad {
  ExceptionAddressRange GuardedRange;
  va_t PadVA = 0;
  RustLandingPadKind Kind = RustLandingPadKind::DropGlue;
};

/// What a call into the panic runtime does.
enum class RustPanicKind : uint8_t {
  /// `panic!`, `.unwrap()`, `.expect()`: reaches the `#[panic_handler]`.
  Explicit,
  /// A compiler-inserted bounds or slice-index check.
  BoundsCheck,
  /// A compiler-inserted arithmetic check: overflow, divide by zero, or a
  /// shift past the width of the type.
  Arithmetic,
  /// `panic_nounwind`/`panic_cannot_unwind`: aborts rather than unwinding, so
  /// the site ends the program instead of starting a panic.
  NoUnwind,
  /// `_Unwind_Resume`: the tail of a cleanup pad.  It continues a panic that
  /// is already in flight rather than raising one.
  Resume,
};

const char *getRustPanicKindName(RustPanicKind Kind);

/// One call site that enters the panic runtime.
struct RustPanicSite {
  va_t CallVA = 0;
  va_t TargetVA = 0;
  /// Demangled runtime entry name where the mangling could be read, and the
  /// raw symbol otherwise.
  std::string TargetName;
  RustPanicKind Kind = RustPanicKind::Explicit;
};

/// Normalized Rust panic behaviour for one function.
struct RustFunctionEH {
  std::vector<RustLandingPad> LandingPads;
  std::vector<RustPanicSite> Panics;
  /// True when dispatch is spelled with the MSVC C++ tables and the
  /// `rust_panic` type descriptor rather than with an Itanium LSDA, which is
  /// what every `*-pc-windows-msvc` target does.
  bool UsesMSVCTables = false;

  bool catchesUnwind() const { return hasPad(RustLandingPadKind::CatchUnwind); }
  bool runsDropGlue() const { return hasPad(RustLandingPadKind::DropGlue); }
  bool guardsAgainstUnwind() const {
    return hasPad(RustLandingPadKind::NoUnwindGuard);
  }
  bool hasExceptionalControlFlow() const {
    return !LandingPads.empty() || !Panics.empty();
  }
  /// True when some site here begins a panic rather than continuing one.
  /// `_Unwind_Resume` is the tail of a cleanup pad in every language that
  /// unwinds, so it is the one panic site that says nothing about whose
  /// frame this is.
  bool raisesAPanic() const {
    for (const RustPanicSite &Site : Panics)
      if (Site.Kind != RustPanicKind::Resume)
        return true;
    return false;
  }

private:
  bool hasPad(RustLandingPadKind Kind) const {
    for (const RustLandingPad &Pad : LandingPads)
      if (Pad.Kind == Kind)
        return true;
    return false;
  }
};

/// How an image's Rust code responds to a panic.
enum class RustPanicStrategy : uint8_t {
  /// Not enough evidence either way.
  Unknown,
  /// Panics unwind, running `Drop` glue, and can be stopped by
  /// `catch_unwind`.
  Unwind,
  /// Panics abort immediately.  Nothing unwinds, so no frame carries a
  /// landing pad and `catch_unwind` can never return an error.
  Abort,
};

const char *getRustPanicStrategyName(RustPanicStrategy Strategy);

/// Image-wide facts about a Rust image's panic machinery.
struct RustRuntimeInfo {
  RustPanicStrategy Strategy = RustPanicStrategy::Unknown;
  /// True when Rust panics travel through the MSVC C++ tables.
  bool UsesMSVCUnwinding = false;
  /// Address of the `rust_panic` type descriptor, for an MSVC image where one
  /// was found.  It is what tells a Rust panic apart from a C++ exception in
  /// tables the two share.
  va_t PanicTypeDescriptorVA = 0;
  /// Count of frames in each classification, so the shape of an image can be
  /// reported without walking every record again.
  uint64_t CleanupFrames = 0;
  uint64_t CatchUnwindFrames = 0;
  uint64_t NoUnwindGuardFrames = 0;
  uint64_t PanicSites = 0;
};

} // namespace neverd

#endif // NEVERD_LOADER_LANGUAGEEHRUST_H
