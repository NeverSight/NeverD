//===- LanguageEHObjC.h - Objective-C exception machinery -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Normalized Objective-C exception records.  Objective-C has no table format
/// of its own, so what these carry is the reading of a shared Itanium or MSVC
/// table: which runtime an image links, how its type-table slots name a class,
/// and what each landing pad and runtime call is for.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LOADER_LANGUAGEEHOBJC_H
#define NEVERD_LOADER_LANGUAGEEHOBJC_H

#include "neverd/loader/ExceptionCommon.h"

#include <cstdint>
#include <string>
#include <vector>

namespace neverd {

/// Which Objective-C runtime an image's exception machinery belongs to.
///
/// Objective-C has no table format of its own: every runtime below emits an
/// Itanium LSDA and differs only in what it puts in the type table.  That one
/// difference is total, though, because the three conventions are not merely
/// different encodings of a pointer -- one of them is not a pointer at all --
/// so a slot cannot be read until the runtime is known.
enum class ObjCRuntimeKind : uint8_t {
  /// Apple's non-fragile runtime (`macosx`, `ios`, `watchos`), reached through
  /// `__objc_personality_v0`.  A slot addresses an `objc_typeinfo`, whose
  /// first two fields are deliberately laid out as `std::type_info`'s are so
  /// that one table can name both an Objective-C class and a C++ type.
  AppleNonFragile,
  /// GCC libobjc, GNUstep's Objective-C routine, and ObjFW.  A slot holds the
  /// class name string itself; there is no descriptor to follow.
  GNU,
  /// GNUstep's Objective-C++ routine.  A slot addresses a
  /// `gnustep::libobjc::__objc_class_type_info`, which really is a
  /// `std::type_info` subclass, so the slot is read as an Itanium one.
  GNUstepObjCXX,
};

const char *getObjCRuntimeKindName(ObjCRuntimeKind Kind);

/// What one `@catch` clause accepts.
///
/// The distinction between the last three is not pedantry: `@catch(id)` takes
/// any Objective-C object and lets a C++ exception pass through, `@catch(...)`
/// takes anything at all including a foreign exception, and only a named class
/// tests the thrown object's identity.  Apple spells the first two as two
/// different slots -- a reference to `OBJC_EHTYPE_id` and a null -- so an
/// image says which it meant, and collapsing them would put a handler on
/// exceptions that would in fact have flown past it.
enum class ObjCCatchKind : uint8_t {
  /// `@catch(SomeClass *e)`: matched by the runtime against the thrown
  /// object's class and its superclasses.
  Class,
  /// `@catch(id e)`: any Objective-C object, but not a foreign exception.
  AnyObject,
  /// `@catch(...)`: anything that reaches the frame.
  CatchAll,
};

const char *getObjCCatchKindName(ObjCCatchKind Kind);

/// One `@catch` clause recovered from a call site's action chain.
struct ObjCCatchClause {
  ObjCCatchKind Kind = ObjCCatchKind::Class;
  /// 1-based type-table index the action's filter named.
  uint64_t TypeIndex = 0;
  /// Address of the `objc_typeinfo` (Apple), the `__objc_class_type_info`
  /// (GNUstep C++), or the class-name string (GNU) the slot named.
  va_t TypeInfoVA = 0;
  /// For an indirect encoding, the cell the descriptor was loaded through.  A
  /// descriptor that comes from another library -- which every clause naming a
  /// framework class does -- is bound at load time and holds nothing in the
  /// file image, so the cell is the only thing that names it.
  va_t TypeInfoSlotVA = 0;
  /// The class the clause tests for, read from the descriptor's third field.
  /// Only Apple's descriptor carries one; the other runtimes name a class by
  /// string and leave the runtime to look it up, so this stays zero there.
  va_t ClassVA = 0;
  /// Bare class name, with whatever wrapper this runtime's spelling put around
  /// it removed.  Empty when the slot could not be resolved -- which is a fact
  /// about the image and not a catch-all.
  std::string ClassName;
  /// True when the slot was *proven* to name a C++ type rather than an
  /// Objective-C class.  One table holds both in Objective-C++, and a
  /// `catch (std::exception &)` there is not a clause any Objective-C object
  /// can satisfy.  The two descriptors are interchangeable for every field a
  /// personality reads, so what proves it is the vtable they carry or the
  /// `_ZTI` symbol that names them; where neither could be read this stays
  /// false, which is "not shown to be C++" rather than "shown to be a class".
  bool IsCxxType = false;
};

/// What a landing pad does in an Objective-C frame.
///
/// `@finally` has no enumerator because it has no spelling in the tables: the
/// compiler duplicates the body onto the normal path and leaves an ordinary
/// cleanup pad behind, so nothing in the record distinguishes it from the ARC
/// releases beside it.  Claiming one would be a guess, and a consumer is
/// better served by a cleanup pad it can trust than by a `@finally` it cannot.
enum class ObjCPadKind : uint8_t {
  /// The pad only runs cleanup: ARC releases, C++ destructors, or a
  /// `@finally` body, and then resumes the unwind.
  Cleanup,
  /// The pad enters one or more `@catch` clauses.
  Catch,
  /// The cleanup that closes a `@synchronized` body.  Unlike `@finally` this
  /// one is provable: the pad calls `objc_sync_exit`, which nothing else does.
  SynchronizedExit,
};

const char *getObjCPadKindName(ObjCPadKind Kind);

/// One classified landing pad and the region it serves.
struct ObjCLandingPad {
  ExceptionAddressRange GuardedRange;
  va_t PadVA = 0;
  ObjCPadKind Kind = ObjCPadKind::Cleanup;
  /// The clauses the pad's action chain named, in the order the runtime tests
  /// them.  Empty for a pure cleanup pad.
  std::vector<ObjCCatchClause> Catches;
};

/// What a call into the Objective-C runtime does to an exception.
enum class ObjCRuntimeCallKind : uint8_t {
  /// `objc_exception_throw`: raises a new Objective-C exception.
  Throw,
  /// `objc_exception_rethrow`: continues one already caught.
  Rethrow,
  /// `objc_begin_catch`: opens a catch and yields the thrown object.
  BeginCatch,
  /// `objc_end_catch`: closes it.  Reached on both the normal and the
  /// exceptional path out of a `@catch` body.
  EndCatch,
  /// `objc_sync_enter`: the head of a `@synchronized` body, which is what
  /// gives the body a cleanup pad it would not otherwise need.
  SyncEnter,
  /// `objc_sync_exit`: the tail, run on both paths out of the body.
  SyncExit,
  /// `objc_terminate`: what the personality calls when an exception escapes a
  /// frame that may not let one out.
  Terminate,
  /// An ARC release the compiler placed in a cleanup pad: `objc_release`,
  /// `objc_storeStrong`, `objc_destroyWeak`, `objc_autoreleasePoolPop`.  These
  /// are what a pad in ARC code is mostly made of, and telling them from a C++
  /// destructor is what says the cleanup is the compiler's rather than the
  /// program's.
  ARCCleanup,
  /// The fragile runtime's `@try`, which is not table driven at all:
  /// `objc_exception_try_enter` plus `_setjmp`, with `objc_exception_match`
  /// standing in for a type table and `objc_exception_extract` for
  /// `objc_begin_catch`.  A frame built this way carries no landing pad, so
  /// these sites are the only evidence it handles anything.
  FragileTry,
};

const char *getObjCRuntimeCallKindName(ObjCRuntimeCallKind Kind);

/// One call site that enters the Objective-C runtime.
struct ObjCRuntimeCall {
  va_t CallVA = 0;
  va_t TargetVA = 0;
  std::string TargetName;
  ObjCRuntimeCallKind Kind = ObjCRuntimeCallKind::Throw;
};

/// Normalized Objective-C exception behaviour for one function.
struct ObjCFunctionEH {
  ObjCRuntimeKind Runtime = ObjCRuntimeKind::AppleNonFragile;
  std::vector<ObjCLandingPad> LandingPads;
  std::vector<ObjCRuntimeCall> RuntimeCalls;
  /// True when the frame's `@try` is the fragile runtime's setjmp form, which
  /// has no table behind it.
  bool UsesFragileSetjmp = false;
  /// True when dispatch is spelled with the MSVC C++ tables, which is what
  /// clang emits for every Objective-C target on `*-windows-msvc` whatever
  /// runtime was asked for.
  bool UsesMSVCTables = false;

  bool catchesAnything() const {
    for (const ObjCLandingPad &Pad : LandingPads)
      if (!Pad.Catches.empty())
        return true;
    return false;
  }
  bool hasCall(ObjCRuntimeCallKind Kind) const {
    for (const ObjCRuntimeCall &Call : RuntimeCalls)
      if (Call.Kind == Kind)
        return true;
    return false;
  }
  /// True when the frame raises an exception rather than only handling or
  /// cleaning up after one.  A rethrow continues an exception that is already
  /// in flight, so like `_Unwind_Resume` it says nothing about whose frame
  /// this is.
  bool throwsAnException() const { return hasCall(ObjCRuntimeCallKind::Throw); }
  bool guardsASynchronizedBody() const {
    return hasCall(ObjCRuntimeCallKind::SyncEnter);
  }
  bool hasExceptionalControlFlow() const {
    return !LandingPads.empty() || !RuntimeCalls.empty();
  }
};

/// Image-wide facts about an Objective-C image's exception machinery.
struct ObjCRuntimeInfo {
  ObjCRuntimeKind Runtime = ObjCRuntimeKind::AppleNonFragile;
  /// True when some frame proved the runtime by installing one of its
  /// personalities, as opposed to the runtime having been inferred from the
  /// image's sections alone.
  bool RuntimeProvenByPersonality = false;
  /// True when the image links ARC rather than managing retain counts by hand,
  /// which decides whether a cleanup pad is the compiler's or the program's.
  bool UsesARC = false;
  /// Count of frames in each classification, so an image's shape can be
  /// reported without walking every record again.
  uint64_t CatchFrames = 0;
  uint64_t CleanupFrames = 0;
  uint64_t SynchronizedFrames = 0;
  uint64_t FragileTryFrames = 0;
  uint64_t ThrowSites = 0;
};

} // namespace neverd

#endif // NEVERD_LOADER_LANGUAGEEHOBJC_H
