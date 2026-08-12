//===- ObjCEH.cpp - Objective-C exception machinery recovery --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/loader/ObjC/ObjCEH.h"

#include "neverd/Support/BinaryEncoding.h"
#include "neverd/loader/DirectBranch.h"
#include "neverd/loader/LanguageRuntime.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>

#define DEBUG_TYPE "neverd-objc-eh"

namespace neverd::objc_eh {
namespace {

/// Upper bound on the bytes of one function body the runtime-call scan reads.
constexpr size_t MaxScannedFunctionBytes = 1u << 20;

/// Upper bound on a class name.  Objective-C class names are identifiers, and
/// a longer run than this is evidence the address was not one.
constexpr size_t MaxClassNameBytes = 512;

//===----------------------------------------------------------------------===//
// Reading the image
//===----------------------------------------------------------------------===//

/// The NUL-terminated string at \p Address, or empty when there is not one.
///
/// An unterminated run is refused rather than truncated: it is what a pointer
/// into arbitrary data looks like, and returning the bytes anyway is how a
/// mis-resolved slot turns into a confident wrong class name.
std::string readCString(const BinaryImage &Img, va_t Address, size_t Limit) {
  if (Address == 0)
    return {};
  const uint8_t *Bytes = nullptr;
  size_t Available = 0;
  // The mapped run ending short of `Limit` is ordinary -- a string at the end
  // of its section -- so the largest readable window is found by halving
  // rather than by insisting on the whole of it.
  for (size_t Length = Limit; Length > 0; Length /= 2)
    if ((Bytes = Img.readVA(Address, Length)) != nullptr) {
      Available = Length;
      break;
    }
  if (!Bytes)
    return {};
  const auto *Chars = reinterpret_cast<const char *>(Bytes);
  const size_t Size = strnlen(Chars, Available);
  if (Size == 0 || Size == Available)
    return {};
  return std::string(Chars, Size);
}

/// Read a pointer-sized field at \p Address.
va_t readPointerField(const BinaryImage &Img, va_t Address) {
  const unsigned PtrSize = Img.getPointerSize();
  if (Address == 0 || (PtrSize != 4 && PtrSize != 8))
    return 0;
  const uint8_t *Slot = Img.readVA(Address, PtrSize);
  if (!Slot)
    return 0;
  return PtrSize == 4 ? va_t(readLE<uint32_t>(Slot)) : va_t(readLE<uint64_t>(Slot));
}

/// Strip the leading underscores a container adds to a C symbol.  Mach-O adds
/// one to every one of them, so `OBJC_EHTYPE_id` is `_OBJC_EHTYPE_id` there
/// and the mangling marker moves with the format rather than with the name.
llvm::StringRef stripUnderscores(llvm::StringRef Name) {
  while (Name.consume_front("_"))
    ;
  return Name;
}

//===----------------------------------------------------------------------===//
// Type table
//===----------------------------------------------------------------------===//

/// The class an Apple `objc_typeinfo` names.
///
/// The record is `{ const void **vtable; const char *name; Class cls; }`, laid
/// out so that its first two fields match `std::type_info`'s -- which is what
/// lets one type table hold both an Objective-C class and a C++ type, and what
/// makes the generic Itanium reader produce the right name here already.  The
/// third field is Objective-C's own, and reading it is what turns a name into
/// the class object the image actually carries.
va_t readAppleEHTypeClass(const BinaryImage &Img, va_t TypeInfoVA) {
  if (TypeInfoVA == 0)
    return 0;
  const unsigned PtrSize = Img.getPointerSize();
  if ((PtrSize != 4 && PtrSize != 8) || TypeInfoVA > InvalidVA - 3 * PtrSize)
    return 0;
  const va_t Class = readPointerField(Img, TypeInfoVA + 2 * PtrSize);
  // A class the loader binds holds no usable pointer in the file image, and a
  // word that addresses nothing names nothing.
  return Img.readVA(Class, 1) ? Class : 0;
}

/// Whether the descriptor \p Entry names is a C++ `std::type_info` rather than
/// an Objective-C `objc_typeinfo`.
///
/// The two are interchangeable by design for the fields a personality reads,
/// which is the whole reason one table can hold both, so nothing in the record
/// proper separates them.  The vtable in the first field does: an Objective-C
/// descriptor always points into `objc_ehtype_vtable` and a C++ one into one
/// of `__cxxabiv1`'s.  A vtable pointer addresses the second slot past the
/// start of its table, so the symbol is two words below what the field holds.
///
/// Where the descriptor comes from another library -- which is the usual case,
/// since the Foundation classes and the standard exceptions both do -- there
/// is no vtable in this image to read, and the symbol naming the descriptor
/// answers instead.
bool namesACxxType(const BinaryImage &Img, const ItaniumTypeEntry &Entry) {
  const std::string Symbol =
      resolveRoutineName(Img, Entry.TypeInfoVA, Entry.TypeInfoSlotVA);
  if (stripUnderscores(Symbol).starts_with("ZTI"))
    return true;

  const unsigned PtrSize = Img.getPointerSize();
  const va_t VTable = readPointerField(Img, Entry.TypeInfoVA);
  if (VTable < 2 * PtrSize)
    return false;
  const std::string Table = resolveRoutineName(Img, VTable - 2 * PtrSize);
  return stripUnderscores(Table).starts_with("ZTVN10__cxxabiv1");
}

/// What a type-table slot means to \p Runtime, given what the Itanium reader
/// already made of it.
///
/// `Entry.TypeName` arrives holding whatever the generic reader could prove,
/// which for Apple and GNUstep is the class name and for the GNU runtime is
/// nothing usable: there the slot is the name string, so the generic reader
/// followed a pointer it built out of the string's own characters.
ObjCCatchClause classifySlot(const BinaryImage &Img, ObjCRuntimeKind Runtime,
                             const ItaniumTypeEntry &Entry) {
  ObjCCatchClause Clause;
  Clause.TypeIndex = Entry.Index;
  Clause.TypeInfoVA = Entry.TypeInfoVA;
  Clause.TypeInfoSlotVA = Entry.TypeInfoSlotVA;

  // A null slot is `@catch(...)` under every runtime: the personality is
  // reached with nothing to match against and accepts whatever arrived,
  // including an exception no Objective-C runtime raised.
  if (Entry.IsCatchAll) {
    Clause.Kind = ObjCCatchKind::CatchAll;
    return Clause;
  }

  switch (Runtime) {
  case ObjCRuntimeKind::AppleNonFragile:
    Clause.ClassName = Entry.TypeName;
    Clause.ClassVA = readAppleEHTypeClass(Img, Entry.TypeInfoVA);
    break;
  case ObjCRuntimeKind::GNUstepObjCXX:
    // `__objc_eh_typeinfo_<Class>` is a `std::type_info` subclass, so the
    // generic reader's answer is already the right one.  There is no class
    // field: GNUstep names a class by string and lets the runtime look it up.
    Clause.ClassName = Entry.TypeName;
    break;
  case ObjCRuntimeKind::GNU:
    // The slot is the string.  Anything the generic reader produced here came
    // from treating the string's bytes as an address, so it is discarded
    // rather than preferred to what is actually there.
    Clause.ClassName =
        readCString(Img, Entry.TypeInfoVA, MaxClassNameBytes);
    break;
  }

  // Every runtime spells `@catch(id)` as a type of its own rather than as a
  // catch-all, because the two differ: an `id` clause takes any Objective-C
  // object and lets a foreign exception continue past it.  Which symbol says
  // so depends on the runtime, and the GNU runtime says it with a string.
  const llvm::StringRef Name = stripUnderscores(Clause.ClassName);
  if (Name == "OBJC_EHTYPE_id" || Name == "__objc_id_type_info" ||
      Name == "objc_id_type_info" || Name == "@id") {
    Clause.Kind = ObjCCatchKind::AnyObject;
    Clause.ClassName.clear();
    return Clause;
  }

  Clause.Kind = ObjCCatchKind::Class;
  // A slot naming a C++ type is ordinary in Objective-C++: one table holds
  // both, because Apple's descriptor and GNUstep's are both `std::type_info`
  // shaped precisely so that it can.  The GNU runtime is the exception -- its
  // slots are strings and can name nothing but a class -- so it is not asked.
  if (Runtime != ObjCRuntimeKind::GNU && namesACxxType(Img, Entry)) {
    Clause.IsCxxType = true;
    // A C++ type name is self-describing and is left as the mangling has it.
    Clause.ClassName = Name.str();
    return Clause;
  }

  // What the clause names is a class, but what could be read may have been the
  // descriptor's symbol rather than the class name inside it -- which is the
  // usual case, because a clause naming a framework class reaches a descriptor
  // that lives in that framework and holds nothing in this image.  Both
  // spellings are wrappers around the same identifier, so both are unwrapped.
  llvm::StringRef Bare = Name;
  for (llvm::StringRef Wrapper :
       {"OBJC_EHTYPE_$_", "OBJC_EHTYPE_", "objc_eh_typeinfo_"})
    if (Bare.consume_front(Wrapper))
      break;
  Clause.ClassName = Bare.str();
  // Apple's slot is a descriptor whose name the reader may have failed to
  // reach; the descriptor is still the identity, so the clause keeps it.  A
  // clause with no name is a fact about what could be read, not a catch-all.
  return Clause;
}

const ItaniumTypeEntry *findTypeEntry(const ItaniumEHInfo &Info,
                                      uint64_t Index) {
  for (const ItaniumTypeEntry &Entry : Info.TypeTable)
    if (Entry.Index == Index)
      return &Entry;
  return nullptr;
}

const ItaniumAction *findAction(const ItaniumEHInfo &Info, uint64_t Offset) {
  for (const ItaniumAction &Action : Info.Actions)
    if (Action.TableOffset == Offset)
      return &Action;
  return nullptr;
}

/// Walk one call site's action chain and collect the clauses it names, in the
/// order the personality tests them.
std::vector<ObjCCatchClause> collectClauses(const BinaryImage &Img,
                                            ObjCRuntimeKind Runtime,
                                            const ItaniumEHInfo &Info,
                                            const ItaniumCallSite &Site) {
  std::vector<ObjCCatchClause> Clauses;
  std::optional<uint64_t> Offset = Site.FirstActionOffset;
  // The chain is a linked list inside a table this decoder already bounded, so
  // the only way to loop is a table that points back at itself.  Bounding the
  // walk by the action count makes that terminate without a visited set.
  for (size_t Step = 0; Offset && Step <= Info.Actions.size(); ++Step) {
    const ItaniumAction *Action = findAction(Info, *Offset);
    if (!Action)
      break;
    if (Action->isCatch()) {
      const uint64_t Index = static_cast<uint64_t>(Action->TypeFilter);
      if (const ItaniumTypeEntry *Entry = findTypeEntry(Info, Index))
        Clauses.push_back(classifySlot(Img, Runtime, *Entry));
    }
    Offset = Action->NextActionOffset;
  }
  return Clauses;
}

//===----------------------------------------------------------------------===//
// Runtime entry points
//===----------------------------------------------------------------------===//

/// Classify a runtime entry point by name.
///
/// The names are shared across the runtimes: GCC libobjc, GNUstep, ObjFW and
/// Apple all spell the throw `objc_exception_throw` and the catch pair
/// `objc_begin_catch`/`objc_end_catch`, because those are the names the
/// Itanium-based Objective-C EH ABI fixed.  What differs between runtimes is
/// the type table, which is read elsewhere.
std::optional<ObjCRuntimeCallKind> classifyRuntimeName(llvm::StringRef Raw) {
  const llvm::StringRef Name = stripUnderscores(Raw);

  if (Name == "objc_exception_throw")
    return ObjCRuntimeCallKind::Throw;
  if (Name == "objc_exception_rethrow" || Name == "objc_rethrow_exception")
    return ObjCRuntimeCallKind::Rethrow;
  if (Name == "objc_begin_catch")
    return ObjCRuntimeCallKind::BeginCatch;
  if (Name == "objc_end_catch")
    return ObjCRuntimeCallKind::EndCatch;
  if (Name == "objc_sync_enter")
    return ObjCRuntimeCallKind::SyncEnter;
  if (Name == "objc_sync_exit")
    return ObjCRuntimeCallKind::SyncExit;
  if (Name == "objc_terminate")
    return ObjCRuntimeCallKind::Terminate;

  // The fragile runtime's `@try` is a setjmp buffer and a chain of matches
  // rather than a table.  `objc_exception_extract` is its `objc_begin_catch`
  // and `objc_exception_match` its type table, so a frame using them handles
  // exceptions even though it carries no landing pad at all.
  if (Name == "objc_exception_try_enter" || Name == "objc_exception_try_exit" ||
      Name == "objc_exception_extract" || Name == "objc_exception_match")
    return ObjCRuntimeCallKind::FragileTry;

  // ARC's cleanups.  These are what a landing pad in ARC code is mostly made
  // of, and recognizing them is what separates cleanup the compiler inserted
  // from a destructor the program wrote.  `objc_release` is matched exactly
  // rather than by prefix so that `objc_releaseReturnValue` -- an optimization
  // of a return sequence, not a cleanup -- is not swept in with it.
  if (Name == "objc_release" || Name == "objc_storeStrong" ||
      Name == "objc_destroyWeak" || Name == "objc_autoreleasePoolPop")
    return ObjCRuntimeCallKind::ARCCleanup;
  return std::nullopt;
}

/// Every address in the image an Objective-C exception edge can target.
llvm::DenseMap<va_t, std::pair<ObjCRuntimeCallKind, std::string>>
collectRuntimeTargets(const BinaryImage &Img) {
  llvm::DenseMap<va_t, std::pair<ObjCRuntimeCallKind, std::string>> Targets;
  auto consider = [&](va_t RawAddress, llvm::StringRef Name) {
    // An ARM symbol table sets bit 0 of a Thumb function's address to record
    // its instruction set.  A decoded branch target never carries that bit, so
    // the two would never match unless it is dropped here.
    const va_t Address = normalizeCodeAddress(RawAddress, Img.Arch, Img.Mode);
    if (Address == 0 || Name.empty())
      return;
    if (std::optional<ObjCRuntimeCallKind> Kind = classifyRuntimeName(Name))
      Targets.try_emplace(Address, std::make_pair(*Kind, Name.str()));
  };

  for (const Symbol &S : Img.Symbols)
    consider(S.Addr, S.Name);
  // A runtime entry reached through the PLT or an import thunk is named by the
  // stub, not by the symbol, so the stub address is the one a call site holds.
  for (const auto &[StubVA, ImportIndex] : Img.ImportStubIndices)
    if (ImportIndex < Img.Imports.size())
      consider(StubVA, Img.Imports[ImportIndex].Name);
  return Targets;
}

/// Record every direct branch from \p Range into the Objective-C runtime.
void collectRuntimeCalls(
    const BinaryImage &Img, const ExceptionAddressRange &Range,
    const llvm::DenseMap<va_t, std::pair<ObjCRuntimeCallKind, std::string>>
        &Targets,
    ObjCFunctionEH &EH) {
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
                        ObjCRuntimeCall Call;
                        Call.CallVA = SiteVA;
                        Call.TargetVA = TargetVA;
                        Call.Kind = It->second.first;
                        Call.TargetName = It->second.second;
                        EH.RuntimeCalls.push_back(std::move(Call));
                      });
}

//===----------------------------------------------------------------------===//
// Landing pads
//===----------------------------------------------------------------------===//

/// Mark the pads that close a `@synchronized` body.
///
/// A pad's extent is not in any table, so it is taken to run from its own
/// address to the next pad -- which is how the compiler lays them out, at the
/// end of the function one after another.  That is an over-approximation, and
/// it is used only to attribute a call that has already been proven to exist:
/// the worst a wrong boundary can do is credit `objc_sync_exit` to the pad
/// before the one that makes it, and no pad is given a kind on the strength of
/// a call the function does not contain.
void markSynchronizedPads(const ExceptionAddressRange &FunctionRange,
                          const std::vector<ObjCRuntimeCall> &Calls,
                          std::vector<ObjCLandingPad> &Pads) {
  std::vector<va_t> Starts;
  for (const ObjCLandingPad &Pad : Pads)
    Starts.push_back(Pad.PadVA);
  std::sort(Starts.begin(), Starts.end());

  for (ObjCLandingPad &Pad : Pads) {
    if (Pad.Kind != ObjCPadKind::Cleanup)
      continue;
    const auto Next = std::upper_bound(Starts.begin(), Starts.end(), Pad.PadVA);
    const va_t End = Next == Starts.end() ? FunctionRange.End : *Next;
    for (const ObjCRuntimeCall &Call : Calls)
      if (Call.Kind == ObjCRuntimeCallKind::SyncExit &&
          Call.CallVA >= Pad.PadVA && Call.CallVA < End) {
        Pad.Kind = ObjCPadKind::SynchronizedExit;
        break;
      }
  }
}

/// Classify the Itanium record on \p F as Objective-C.
bool annotateItanium(const BinaryImage &Img, ExceptionFunction &F,
                     ObjCRuntimeKind Runtime, ObjCFunctionEH &EH) {
  // An SJLJ table's call-site "ranges" are indices the compiler handed out
  // rather than addresses, so nothing in it names a pad that could be placed.
  if (!F.Itanium || !F.Itanium->IsCallSiteAddressForm)
    return false;
  if (!getObjCRuntimeForPersonality(F.Personality))
    return false;

  for (const ItaniumCallSite &Site : F.Itanium->CallSites) {
    if (Site.LandingPadVA == 0)
      continue;
    ObjCLandingPad Pad;
    Pad.GuardedRange = Site.GuardedRange;
    Pad.PadVA = Site.LandingPadVA;
    Pad.Catches = collectClauses(Img, Runtime, *F.Itanium, Site);
    Pad.Kind = Pad.Catches.empty() ? ObjCPadKind::Cleanup : ObjCPadKind::Catch;
    EH.LandingPads.push_back(std::move(Pad));
  }
  return true;
}

/// Classify the MSVC record on \p F, if it dispatches Objective-C exceptions.
///
/// On `*-pc-windows-msvc` clang emits `__CxxFrameHandler3` for Objective-C
/// whatever runtime was requested, so the personality says C++ and the tables
/// are C++'s.  What is Objective-C about such a frame is only what it calls,
/// which is why this claims nothing from the tables alone.
bool annotateMSVC(ExceptionFunction &F, ObjCFunctionEH &EH) {
  if (!F.Cxx || !isCxxPersonality(F.Personality))
    return false;
  if (!EH.hasCall(ObjCRuntimeCallKind::Throw) &&
      !EH.hasCall(ObjCRuntimeCallKind::BeginCatch) &&
      !EH.hasCall(ObjCRuntimeCallKind::SyncEnter))
    return false;
  EH.UsesMSVCTables = true;
  for (const CxxTryBlock &Try : F.Cxx->TryBlocks)
    for (const CxxCatchHandler &Catch : Try.Handlers) {
      ObjCLandingPad Pad;
      Pad.PadVA = Catch.HandlerVA;
      Pad.GuardedRange = F.CodeRange;
      Pad.Kind = ObjCPadKind::Catch;
      EH.LandingPads.push_back(std::move(Pad));
    }
  return true;
}

//===----------------------------------------------------------------------===//
// Image-wide state
//===----------------------------------------------------------------------===//

/// The runtime the image's own frames named, if any did.
std::optional<ObjCRuntimeKind> runtimeFromPersonalities(const ExceptionInfo &I) {
  for (const ExceptionFunction &F : I.Functions)
    if (std::optional<ObjCRuntimeKind> Kind =
            getObjCRuntimeForPersonality(F.Personality))
      return Kind;
  return std::nullopt;
}

/// The runtime the image's sections and symbols imply, for a target whose
/// frames name a personality that is not Objective-C's.  That is every
/// `*-windows-msvc` image and every frame whose only Objective-C content is a
/// message send, so the question is worth answering without a personality.
ObjCRuntimeKind runtimeFromImage(const BinaryImage &Img) {
  // Apple's runtime is the one that publishes class lists under these names;
  // the GNU runtimes publish nothing of the sort.
  if (Img.getSectionByName("__objc_classlist") ||
      Img.getSectionByName("__objc_catlist"))
    return ObjCRuntimeKind::AppleNonFragile;
  for (const Symbol &S : Img.Symbols) {
    const llvm::StringRef Name = stripUnderscores(S.Name);
    if (Name == "objc_msg_lookup" || Name == "objc_msg_lookup_sender")
      return ObjCRuntimeKind::GNU;
  }
  return ObjCRuntimeKind::AppleNonFragile;
}

/// True when the image links ARC rather than managing retain counts by hand.
///
/// The marker is `objc_storeStrong`: the compiler emits it for every strong
/// assignment and nothing else calls it, whereas `objc_release` also appears
/// in code that manages its own counts.
bool linksARC(const BinaryImage &Img) {
  auto names = [](llvm::StringRef Raw) {
    const llvm::StringRef Name = stripUnderscores(Raw);
    return Name == "objc_storeStrong" || Name == "objc_retainAutoreleaseReturnValue" ||
           Name == "objc_autoreleaseReturnValue";
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

bool hasObjCRuntime(const BinaryImage &Img) {
  if (Img.ExceptionMetadata.Runtime.is(SourceLanguageRuntime::ObjectiveC))
    return true;
  for (const ExceptionFunction &F : Img.ExceptionMetadata.Functions)
    if (getObjCRuntimeForPersonality(F.Personality))
      return true;
  if (Img.getSectionByName("__objc_classlist") ||
      Img.getSectionByName("__objc_catlist") ||
      Img.getSectionByName(".objc_class_refs"))
    return true;
  auto names = [](llvm::StringRef Raw) {
    const llvm::StringRef Name = stripUnderscores(Raw);
    return Name == "objc_msgSend" || Name == "objc_msg_lookup" ||
           Name == "objc_exception_throw";
  };
  for (const Symbol &S : Img.Symbols)
    if (names(S.Name))
      return true;
  for (const Import &I : Img.Imports)
    if (names(I.Name))
      return true;
  return false;
}

void parseObjCExceptions(BinaryImage &Img) {
  if (!hasObjCRuntime(Img))
    return;

  ExceptionInfo &Info = Img.ExceptionMetadata;
  const auto Targets = collectRuntimeTargets(Img);

  ObjCRuntimeInfo Runtime;
  Runtime.UsesARC = linksARC(Img);
  // A personality is a statement the producer made about how this frame is
  // dispatched, so it outranks anything inferred from the image's shape.
  if (std::optional<ObjCRuntimeKind> Named = runtimeFromPersonalities(Info)) {
    Runtime.Runtime = *Named;
    Runtime.RuntimeProvenByPersonality = true;
  } else {
    Runtime.Runtime = runtimeFromImage(Img);
  }

  for (ExceptionFunction &F : Info.Functions) {
    // A record whose table could not be read cannot be classified from what it
    // does contain: the action chain a classification walks is exactly the
    // part that may be missing.
    if (F.ParseStatus == ExceptionParseStatus::Malformed)
      continue;

    ObjCFunctionEH EH;
    // A frame that named a personality has said which runtime dispatches it,
    // and that may differ from the image's majority: an Objective-C++ object
    // linked into a GNUstep program installs the Objective-C++ routine while
    // its plain Objective-C neighbours install the other one.
    EH.Runtime =
        getObjCRuntimeForPersonality(F.Personality).value_or(Runtime.Runtime);

    // The runtime calls are collected first because the MSVC reading depends
    // on them: there the tables say C++ and only the calls say Objective-C.
    collectRuntimeCalls(Img, F.CodeRange, Targets, EH);
    const bool FromTables = annotateItanium(Img, F, EH.Runtime, EH) |
                            static_cast<int>(annotateMSVC(F, EH));
    EH.UsesFragileSetjmp = EH.hasCall(ObjCRuntimeCallKind::FragileTry);
    markSynchronizedPads(F.CodeRange, EH.RuntimeCalls, EH.LandingPads);

    // A frame that named an Objective-C personality is Objective-C's whatever
    // else it contains.  One that did not is claimed only when it calls the
    // runtime in a way that no other language's code does -- a bare
    // `objc_release` is emitted into C++ and Swift too, so it is not enough on
    // its own to make the frame Objective-C's.
    const bool FromCalls = EH.throwsAnException() || EH.UsesFragileSetjmp ||
                           EH.hasCall(ObjCRuntimeCallKind::BeginCatch) ||
                           EH.hasCall(ObjCRuntimeCallKind::SyncEnter);
    if (!FromTables && !FromCalls)
      continue;

    Runtime.CatchFrames += EH.catchesAnything();
    Runtime.SynchronizedFrames += EH.guardsASynchronizedBody();
    Runtime.FragileTryFrames += EH.UsesFragileSetjmp;
    for (const ObjCRuntimeCall &Call : EH.RuntimeCalls)
      Runtime.ThrowSites += Call.Kind == ObjCRuntimeCallKind::Throw;
    for (const ObjCLandingPad &Pad : EH.LandingPads)
      if (Pad.Kind != ObjCPadKind::Catch) {
        ++Runtime.CleanupFrames;
        break;
      }

    if (EH.hasExceptionalControlFlow())
      F.ObjC = std::move(EH);
  }

  Info.ObjCRuntime = Runtime;
  LLVM_DEBUG(llvm::dbgs()
             << "objc-eh: runtime " << getObjCRuntimeKindName(Runtime.Runtime)
             << (Runtime.RuntimeProvenByPersonality ? " (from personality)"
                                                    : " (inferred)")
             << ", " << Runtime.CatchFrames << " catch, "
             << Runtime.CleanupFrames << " cleanup, "
             << Runtime.SynchronizedFrames << " synchronized, "
             << Runtime.FragileTryFrames << " fragile-try, "
             << Runtime.ThrowSites << " throw sites\n");
}

} // namespace neverd::objc_eh
