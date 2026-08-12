//===- LanguageEH.h - Non-Windows exception models ------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Defines the normalized records for exception machinery that does not use
/// the Windows table model: the Itanium C++ ABI (DWARF `.eh_frame` call frame
/// information plus a `.gcc_except_table` language-specific data area), the
/// Darwin compact-unwind encoding, the x86-32 registration chain rooted at
/// `FS:[0]`, and the Go runtime's frame-metadata driven defer/panic/recover.
///
/// These records share the checked-range, provenance, and parse-status
/// discipline of \ref ExceptionInfo.h: a decoder never exposes a raw file
/// pointer, never guesses a schema it did not prove, and marks anything it
/// could not fully validate rather than silently producing a plausible record.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LOADER_LANGUAGEEH_H
#define NEVERD_LOADER_LANGUAGEEH_H

#include "neverd/loader/ExceptionCommon.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace neverd {

/// Which dispatch machinery a record describes.  The model determines how a
/// handler is found at run time, and therefore which normalized fields carry
/// meaning: a Windows table model resolves handlers by address range, an
/// Itanium model resolves them by call-site region plus an action chain, a
/// registration model resolves them by walking a linked list the prologue
/// built on the stack, and Go resolves them from its own frame metadata.
enum class ExceptionModel : uint8_t {
  None,
  /// PE `.pdata`/`.xdata` on x64, ARM32, and ARM64.
  WindowsTable,
  /// x86-32 `EXCEPTION_REGISTRATION_RECORD` chain rooted at `FS:[0]`.
  WindowsRegistration,
  /// DWARF call frame information plus an Itanium LSDA.
  Itanium,
  /// Darwin `__unwind_info` compact unwind, optionally with an Itanium LSDA.
  CompactUnwind,
  /// Go runtime frame metadata (`pclntab` funcdata/pcdata).
  GoRuntime,
};

inline const char *getExceptionModelName(ExceptionModel Model) {
  switch (Model) {
  case ExceptionModel::None:
    return "none";
  case ExceptionModel::WindowsTable:
    return "windows-table";
  case ExceptionModel::WindowsRegistration:
    return "windows-registration";
  case ExceptionModel::Itanium:
    return "itanium";
  case ExceptionModel::CompactUnwind:
    return "compact-unwind";
  case ExceptionModel::GoRuntime:
    return "go-runtime";
  }
  return "unknown";
}

//===----------------------------------------------------------------------===//
// DWARF call frame information
//===----------------------------------------------------------------------===//

/// A canonicalized call-frame instruction.  The primary and extended opcode
/// forms of one operation normalize to the same kind so consumers reason about
/// unwind semantics instead of DWARF encoding trivia; `Opaque` retains the
/// exact bytes of an operation this decoder does not model.
enum class CFIOpKind : uint8_t {
  Nop,
  SetLoc,
  AdvanceLoc,
  DefCFA,
  DefCFARegister,
  DefCFAOffset,
  DefCFAExpression,
  Offset,
  ValOffset,
  Register,
  Expression,
  ValExpression,
  Restore,
  Undefined,
  SameValue,
  RememberState,
  RestoreState,
  /// `DW_CFA_GNU_args_size`: bytes of stack arguments the unwinder must pop.
  GnuArgsSize,
  /// AArch64 pointer-authentication return-address signing state toggle.
  NegateRAState,
  /// AArch64 return-address signing with the PC as a diversifier.
  NegateRAStateWithPC,
  Opaque,
};

const char *getCFIOpKindName(CFIOpKind Kind);

/// One decoded call-frame instruction.  `CodeOffset` is the offset from the
/// owning FDE's initial location at which the rule takes effect, already
/// scaled by the CIE's code alignment factor.  `Offset` is already scaled by
/// the data alignment factor for the rules that are defined to use it, so a
/// consumer never has to re-apply a factor it did not read.
struct CFIInstruction {
  CFIOpKind Kind = CFIOpKind::Opaque;
  uint64_t CodeOffset = 0;
  uint64_t Register = 0;
  uint64_t Register2 = 0;
  int64_t Offset = 0;
  std::vector<uint8_t> Expression;
  std::vector<uint8_t> OperandBytes;
};

/// A decoded Common Information Entry.  The pointer encodings are retained
/// exactly because they are part of the contract a rewriter must reproduce.
struct DwarfCIE {
  /// Offset of this CIE from the start of its frame section.
  uint64_t SectionOffset = 0;
  uint8_t Version = 0;
  std::string Augmentation;
  uint64_t CodeAlignmentFactor = 0;
  int64_t DataAlignmentFactor = 0;
  uint64_t ReturnAddressRegister = 0;
  /// Address size and segment selector size, present from CIE version 4.
  uint8_t AddressSize = 0;
  uint8_t SegmentSelectorSize = 0;
  /// `DW_EH_PE_omit` when the augmentation did not declare the encoding.
  uint8_t FDEPointerEncoding = 0xFF;
  uint8_t LSDAPointerEncoding = 0xFF;
  uint8_t PersonalityEncoding = 0xFF;
  va_t PersonalityVA = 0;
  /// Address of the slot an indirect personality encoding loads through.
  /// Zero for a direct encoding.
  va_t PersonalitySlotVA = 0;
  /// True when the augmentation string begins with 'z' and therefore carries a
  /// self-describing length; a CIE without it cannot be skipped safely by a
  /// consumer that does not understand every augmentation character.
  bool HasAugmentationData = false;
  /// 'S': frames described by this CIE are signal frames, so the return
  /// address is the precise interrupted PC rather than a post-call address.
  bool IsSignalFrame = false;
  /// 'B'/'G': AArch64 pointer authentication and MTE tagged frames.
  bool HasPointerAuth = false;
  bool HasMTETaggedFrame = false;
  std::vector<CFIInstruction> InitialInstructions;
};

/// A decoded Frame Description Entry paired with its resolved CIE.
struct DwarfFDE {
  uint64_t SectionOffset = 0;
  uint64_t CIESectionOffset = 0;
  va_t InitialLocation = 0;
  /// Section offset of the `initial_location` field itself.  In a relocatable
  /// object that field is the target of a relocation and holds no address
  /// until one is applied, so a reader has to be able to ask whether it was.
  uint64_t InitialLocationOffset = 0;
  uint64_t AddressRange = 0;
  /// Zero when the CIE declared no LSDA encoding or the FDE omitted it.
  va_t LSDAVA = 0;
  std::vector<CFIInstruction> Instructions;
};

//===----------------------------------------------------------------------===//
// Itanium language-specific data area
//===----------------------------------------------------------------------===//

/// One entry of the LSDA action chain.  A positive filter selects the
/// 1-based type-table entry a catch matches, zero is a cleanup that always
/// runs, and a negative filter selects a 1-based exception-specification list.
struct ItaniumAction {
  /// Byte offset of this action record inside the action table, which is how
  /// call sites and chained actions name it.
  uint64_t TableOffset = 0;
  int64_t TypeFilter = 0;
  /// Table offset of the next action, or nullopt for the end of the chain.
  std::optional<uint64_t> NextActionOffset;

  bool isCleanup() const { return TypeFilter == 0; }
  bool isCatch() const { return TypeFilter > 0; }
  bool isExceptionSpecification() const { return TypeFilter < 0; }
};

/// One protected region.  A zero landing pad means the region has no local
/// handler: an exception propagates out of the frame directly, which for the
/// `call-site` model is how a `noexcept` boundary or a plain call is spelled.
struct ItaniumCallSite {
  ExceptionAddressRange GuardedRange;
  va_t LandingPadVA = 0;
  /// Byte offset into the action table of the first action, or nullopt when
  /// the call site declared no action (an unconditional cleanup landing pad).
  std::optional<uint64_t> FirstActionOffset;
  /// Native encoded fields, retained for provenance and regeneration.
  uint64_t NativeStart = 0;
  uint64_t NativeLength = 0;
  uint64_t NativeLandingPad = 0;
  uint64_t NativeActionRecord = 0;
};

/// One type-table slot.  `TypeInfoVA` is zero for the catch-all slot, which
/// the ABI spells as a null `std::type_info*`.
struct ItaniumTypeEntry {
  /// 1-based index as named by a positive action filter.
  uint64_t Index = 0;
  va_t TypeInfoVA = 0;
  /// For an indirect encoding, the address of the cell the `std::type_info*`
  /// was loaded through.  A cell bound at load time holds a placeholder in the
  /// file image, so this is what lets the RTTI be named from the binding.
  va_t TypeInfoSlotVA = 0;
  /// Mangled RTTI symbol (`_ZTI...`) or the `std::type_info::__type_name`
  /// string, whichever could be proven; empty when neither could be.
  std::string TypeName;
  bool IsCatchAll = false;
};

/// One exception-specification list, named by a negative action filter.
struct ItaniumExceptionSpec {
  /// 1-based index as named by `-Index`.
  uint64_t Index = 0;
  /// Type-table indices the specification permits; an empty list is
  /// `throw()`/`noexcept`.
  std::vector<uint64_t> TypeIndices;
};

/// A fully decoded `.gcc_except_table` record.
struct ItaniumEHInfo {
  va_t LSDAVA = 0;
  /// Encoding and resolved base for landing-pad addresses.
  uint8_t LandingPadBaseEncoding = 0xFF;
  va_t LandingPadBase = 0;
  /// Encoding and resolved base of the type table.  `TypeTableVA` addresses
  /// the slot *past* the last entry because the table grows downward.
  uint8_t TypeTableEncoding = 0xFF;
  va_t TypeTableVA = 0;
  uint8_t CallSiteEncoding = 0xFF;
  uint64_t CallSiteTableLength = 0;
  std::vector<ItaniumCallSite> CallSites;
  std::vector<ItaniumAction> Actions;
  std::vector<ItaniumTypeEntry> TypeTable;
  std::vector<ItaniumExceptionSpec> ExceptionSpecs;
  /// True when the record uses the `.gcc_except_table` call-site table; false
  /// when it uses the SJLJ call-site form, whose "ranges" are call-site
  /// indices rather than addresses.
  bool IsCallSiteAddressForm = true;

  /// True when no call site names a catch or exception-specification action,
  /// so every landing pad is destructor cleanup.  A Rust frame compiled with
  /// `panic=unwind` and no `catch_unwind` has exactly this shape.
  bool isCleanupOnly() const {
    for (const ItaniumAction &A : Actions)
      if (!A.isCleanup())
        return false;
    return true;
  }
};

//===----------------------------------------------------------------------===//
// Darwin compact unwind
//===----------------------------------------------------------------------===//

/// \ref UnwindRegisterClass is declared beside the Windows unwind operations
/// in `ExceptionInfo.h`, which includes this header, so from here it can only
/// be named opaquely.  That is still worth more than forking a second register
/// file vocabulary for Darwin: a consumer that already knows how to read one
/// architecture's saved registers should not have to learn a second spelling
/// of "this number belongs to the floating-point file".
enum class UnwindRegisterClass : uint8_t;

/// The frame shape a compact-unwind entry encodes.  The concrete meaning of
/// the mode bits is architecture specific, so the normalized kind is what
/// consumers use and `NativeEncoding` retains the exact word.
enum class CompactUnwindKind : uint8_t {
  /// No unwind information: the entry exists only to terminate a range.
  None,
  /// Frame-pointer based; saved registers are at negative offsets from the
  /// frame pointer.
  FramePointer,
  /// Frameless with an immediate stack size.
  FramelessImmediate,
  /// Frameless whose stack size is read from the function's `sub` immediate.
  FramelessIndirect,
  /// Defers to a DWARF FDE for this range.
  DwarfFDE,
  Unknown,
};

const char *getCompactUnwindKindName(CompactUnwindKind Kind);

/// One slot in the run of saved-register slots a compact encoding describes.
struct CompactUnwindRegisterSlot {
  /// Value-initializes to `UnwindRegisterClass::None`, which marks a slot the
  /// encoding reserved and left empty.
  UnwindRegisterClass RegisterClass{};
  /// Register number in the target's own numbering rather than the compact
  /// encoding's private one-to-six table, so `rbx` is 3, `r12` is 12, `x19` is
  /// 19 and `d8` is 8.  A number therefore means the same physical register
  /// here as it does in a Windows unwind operation for the same machine.
  uint16_t Register = 0;
};

struct CompactUnwindEntry {
  ExceptionAddressRange CodeRange;
  uint32_t NativeEncoding = 0;
  CompactUnwindKind Kind = CompactUnwindKind::Unknown;
  /// Byte size of the frame for the frameless forms.
  uint32_t StackSize = 0;
  /// True when \ref StackSize is a size the entry actually established.  The
  /// frameless-indirect form keeps its size in the function's own prologue, so
  /// an entry whose prologue could not be read leaves this false instead of
  /// letting a zero pass for a frame that allocates nothing.
  bool HasStackSize = false;
  /// Section offset of the DWARF FDE for `DwarfFDE` entries.
  uint32_t DwarfFDEOffset = 0;
  va_t PersonalityVA = 0;
  va_t LSDAVA = 0;
  bool HasLSDA = false;

  /// The saved-register slots in the order the encoding lists them, which is
  /// the order each architecture's unwinder restores them in: the x86 forms
  /// run from the lowest-addressed slot upward, the ARM64 forms in register
  /// number order, which runs downward in memory.
  std::vector<CompactUnwindRegisterSlot> SavedRegisterSlots;
  /// Every general-purpose register the slots name, as a bitmask over the same
  /// numbering \ref CompactUnwindRegisterSlot::Register uses.  The frame
  /// pointer and return address that a frame form saves are absent: no bit of
  /// the encoding names them, \ref Kind already implies them, and a mask that
  /// invented them could not be encoded back into a word.
  uint32_t SavedGPRMask = 0;
  /// Every floating-point register the slots name.  Only the ARM64 encodings
  /// can name one.
  uint32_t SavedFPRMask = 0;
  /// Distance in bytes from the frame pointer down to slot zero, for the x86
  /// frame forms: slot *i* sits at `fp - FrameOffset + i * <pointer size>`.
  /// The ARM64 frame form fixes that distance at one pointer and spends no
  /// field on it, so it leaves this zero.
  uint32_t FrameOffset = 0;
};

//===----------------------------------------------------------------------===//
// x86-32 registration chain
//===----------------------------------------------------------------------===//

/// One `_except_handler3`/`_except_handler4` scope-table entry.  The scope
/// table is a flat array indexed by "try level"; nesting is expressed by each
/// entry naming its enclosing level rather than by containment of ranges,
/// which is why this model keeps the level graph instead of address ranges.
struct RegistrationScopeRecord {
  /// Enclosing try level, or -1 for a scope directly under the frame.
  int32_t EnclosingLevel = -1;
  /// Filter expression address; zero marks a `__finally` (termination) scope.
  va_t FilterVA = 0;
  /// `__except` body, or `__finally` body when `FilterVA` is zero.
  va_t HandlerVA = 0;
  bool IsFinally = false;
};

/// The prologue-established registration record for one x86-32 function.
struct RegistrationChainInfo {
  /// Address of the handler the prologue installed (`_except_handler3`,
  /// `_except_handler4`, `__CxxFrameHandler`, or a language runtime's own).
  va_t HandlerVA = 0;
  /// Address of the scope table or `FuncInfo` the prologue pushed.
  va_t ScopeTableVA = 0;
  /// Frame offset of the current-try-level slot, relative to the established
  /// frame register, when the prologue's stores proved it.
  std::optional<int32_t> TryLevelOffset;
  /// Address at which the prologue stored the new registration record, i.e.
  /// the value written to `FS:[0]`, expressed as a frame offset.
  std::optional<int32_t> RegistrationOffset;
  /// `_except_handler4` cookie fields.  Present only for the EH4 scope-table
  /// header, which precedes the entry array at a negative displacement.
  bool HasSecurityCookies = false;
  int32_t GSCookieOffset = 0;
  int32_t GSCookieXOROffset = 0;
  int32_t EHCookieOffset = 0;
  int32_t EHCookieXOROffset = 0;
  /// Native scope-table magic for `_except_handler4` (`0xFFFFFFFE` and the
  /// obfuscated variants), retained because it selects the header layout.
  uint32_t ScopeTableMagic = 0;
  std::vector<RegistrationScopeRecord> Scopes;
  /// Addresses at which the prologue/epilogue manipulate the chain.  These
  /// bound the region in which the registration record is live.
  va_t ChainInstallVA = 0;
  va_t ChainRemoveVA = 0;
};

//===----------------------------------------------------------------------===//
// Delphi registration frames
//===----------------------------------------------------------------------===//

/// Which RTL routine a Delphi `TExcDesc` forwards to, and therefore what the
/// bytes after its jump mean.  Delphi puts no table in a data section: the
/// descriptor *is* code, and the dispatch data — when there is any — follows
/// the jump inline, so the routine identity is what says whether the next
/// bytes are an arm table or the handler body itself.
enum class DelphiHandlerKind : uint8_t {
  Unknown,
  /// `@HandleFinally`: a second jump follows, naming the cleanup body.
  Finally,
  /// `@HandleAnyException`: the `except` body follows inline.
  AnyException,
  /// `@HandleOnException`: a count and an array of class/handler arms follow.
  OnException,
  /// `@HandleAutoException`: the `safecall` wrapper, which converts an
  /// escaping exception into an HRESULT instead of running a handler.
  AutoException,
};

const char *getDelphiHandlerKindName(DelphiHandlerKind Kind);

/// One `except on <class> do` arm.
struct DelphiOnExceptionEntry {
  /// Address of the slot the arm names, which holds the class reference.  The
  /// RTL loads the class through it, so the slot is the identity that appears
  /// in the image and the class address is what it currently points at.
  va_t ClassSlotVA = 0;
  /// Class reference (VMT address) the arm matches.  Zero for an `else` arm,
  /// which the RTL treats as matching anything.
  va_t ClassVA = 0;
  /// Class name read from the VMT, when the VMT proved to be one.
  std::string ClassName;
  va_t HandlerVA = 0;
  bool IsCatchAll = false;
};

/// A Delphi `TExcFrame` the prologue linked onto the `FS:[0]` chain.
///
/// Delphi shares the registration mechanism with Windows SEH but nothing else:
/// there is no scope table, no try level, and no filter expression.  A frame
/// pushes one `TExcDesc`, and which of four RTL routines that descriptor jumps
/// to is the whole of its dispatch semantics.
struct DelphiFrameInfo {
  /// The `TExcDesc` the prologue pushed as the handler.
  va_t DescriptorVA = 0;
  /// RTL routine the descriptor's leading jump reaches.
  va_t RuntimeHandlerVA = 0;
  std::string RuntimeHandlerName;
  DelphiHandlerKind Kind = DelphiHandlerKind::Unknown;
  /// `Finally`: the cleanup body named by the descriptor's second jump.
  va_t FinallyBodyVA = 0;
  /// `AnyException`/`AutoException`: the handler body, which begins at the
  /// first byte after the descriptor's jump.
  va_t ExceptBodyVA = 0;
  /// `OnException`: the arms, in the order the RTL tests them.
  std::vector<DelphiOnExceptionEntry> OnExceptions;
  /// Where the prologue wrote the new record to `FS:[0]`.
  va_t ChainInstallVA = 0;
  /// True when the routine identity came from a symbol rather than from the
  /// shape of the descriptors that target it.
  bool RuntimeHandlerNamed = false;
};

/// What a Delphi x86-64 `TExcScope.TableOffset` selects.  The field is a
/// discriminant for its three smallest values and an RVA for everything else,
/// which is how one 16-byte record spells four dispatch shapes.
enum class DelphiScopeKind : uint8_t {
  /// `TableOffset == 0`: `TargetOffset` is a `try..finally` cleanup body.
  Finally,
  /// `TableOffset == 1`: `TargetOffset` is the catch a `safecall` wrapper
  /// enters to turn an escaping exception into an HRESULT.
  SafecallCatch,
  /// `TableOffset == 2`: `TargetOffset` is a `try..except` body that catches
  /// anything.
  CatchAll,
  /// `TableOffset > 2`: it is the RVA of a `TExcDesc` arm table, and
  /// `TargetOffset` carries nothing.
  OnException,
};

const char *getDelphiScopeKindName(DelphiScopeKind Kind);

/// One `TExcScope`: four RVAs in sixteen bytes.
struct DelphiScopeRecord {
  ExceptionAddressRange GuardedRange;
  DelphiScopeKind Kind = DelphiScopeKind::Finally;
  /// Cleanup or catch body, for every kind but \ref DelphiScopeKind::OnException.
  va_t TargetVA = 0;
  /// The `TExcDesc` arm table, for \ref DelphiScopeKind::OnException.
  va_t DescriptorVA = 0;
  std::vector<DelphiOnExceptionEntry> OnExceptions;
};

/// The `TExcData` a Delphi x86-64 frame keeps in its unwind handler data.
///
/// This shares nothing with \ref DelphiFrameInfo but the vendor.  Delphi on
/// x86-64 abandoned the registration chain for the ordinary table mechanism,
/// so the dispatch data is a count-prefixed array of pure data in `.xdata`
/// rather than a descriptor that is itself code, and every address in it is a
/// 32-bit RVA rather than an absolute pointer.
struct DelphiScopeTable {
  /// Address of the `TExcData`, which is the first byte after the unwind
  /// info's handler RVA.
  va_t TableVA = 0;
  std::vector<DelphiScopeRecord> Scopes;
};

//===----------------------------------------------------------------------===//
// Rust panic machinery
//===----------------------------------------------------------------------===//

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

//===----------------------------------------------------------------------===//
// Go runtime frame metadata
//===----------------------------------------------------------------------===//

/// How a deferred call is recorded.  Go has changed this three times, and the
/// shape decides where the deferred closure is found at run time.
enum class GoDeferKind : uint8_t {
  /// `runtime.deferproc`: heap-allocated `_defer` record.
  Heap,
  /// `runtime.deferprocStack`: caller-allocated `_defer` record.
  Stack,
  /// Open-coded: no record; the frame's defer bits and `FUNCDATA_
  /// OpenCodedDeferInfo` describe the closures directly.
  OpenCoded,
};

const char *getGoDeferKindName(GoDeferKind Kind);

/// One deferred-call site.
struct GoDeferSite {
  va_t CallVA = 0;
  GoDeferKind Kind = GoDeferKind::Heap;
  /// Deferred function when a constant closure could be proven at the site.
  va_t TargetVA = 0;
  std::string TargetName;
};

/// One open-coded defer slot described by `FUNCDATA_OpenCodedDeferInfo`.
struct GoOpenCodedDefer {
  /// Frame offset of the closure pointer, relative to varp.
  int32_t ClosureOffset = 0;
};

/// The whole of `FUNCDATA_OpenCodedDeferInfo`: two unsigned varints giving the
/// distance *below* varp of the defer bitmask byte and of the first closure
/// slot.  The remaining slots follow the first consecutively, one pointer
/// apart.  The record deliberately does not store how many slots are live —
/// the runtime learns that from the bitmask at unwind time — so a decoder that
/// reports a slot count has invented it.
struct GoOpenCodedDeferInfo {
  uint32_t DeferBitsOffset = 0;
  uint32_t SlotsOffset = 0;
  /// Upper bound on live slots, fixed by the one-byte bitmask.
  static constexpr unsigned MaxSlots = 8;
};

/// What `PCDATA_UnsafePoint` says about a stretch of instructions.
///
/// Go preempts a goroutine asynchronously by delivering a signal and rewriting
/// the interrupted frame so that it calls `asyncPreempt`, which spills every
/// register into the frame for the collector to see.  That is only sound where
/// the compiler says it is, and this table is where it says so.  Read as a
/// statement about the code rather than about the scheduler, an unsafe stretch
/// is one the compiler built assuming nothing can observe the frame part way
/// through it: the pointer maps need not describe the frame there, and a
/// deferred call or a panic taken from inside one is unwinding a frame whose
/// shape is momentarily not the declared one.
enum class GoUnsafePointKind : uint8_t {
  /// `UnsafePointSafe` (-1), which is also the value in effect before the
  /// table's first entry and after its last.
  Safe,
  /// `UnsafePointUnsafe` (-2).
  Unsafe,
  /// `UnsafePointRestart1` (-3) and `UnsafePointRestart2` (-4): a restartable
  /// sequence, which an interrupt resumes at the start of rather than where it
  /// landed.  Two spellings exist only so that two abutting sequences can be
  /// told apart; they carry the same meaning, and which one a range used is in
  /// `GoUnsafePointRange::NativeValue`.
  RestartSequence,
  /// `UnsafePointRestartAtEntry` (-5): an interrupt restarts the function.
  RestartAtEntry,
  /// A value the table is not defined to hold.  Kept rather than dropped so
  /// that a range decoded from a table this decoder does not fully understand
  /// is visible as such instead of silently reading as safe.
  Unknown,
};

const char *getGoUnsafePointKindName(GoUnsafePointKind Kind);

/// One stretch of a function over which `PCDATA_UnsafePoint` holds one value.
struct GoUnsafePointRange {
  ExceptionAddressRange Range;
  GoUnsafePointKind Kind = GoUnsafePointKind::Safe;
  /// The value as the table spells it, which is what keeps the two restart
  /// spellings distinguishable after the kind has normalized them together.
  int32_t NativeValue = -1;
};

/// One bitmap of a `runtime.stackmap`: which pointer-sized slots of a frame
/// region hold a live pointer at the program points the bitmap covers.
struct GoStackMapBitmap {
  /// Index within the owning map, i.e. the `PCDATA_StackMapIndex` value that
  /// selects this bitmap.
  uint32_t Index = 0;
  /// `stackmap.nbit`: how many slots the bitmap describes.  Slot \p I lies at
  /// `I * PtrSize` from the base of the region the map covers -- the argument
  /// area for `FUNCDATA_ArgsPointerMaps`, the locals area below varp for
  /// `FUNCDATA_LocalsPointerMaps`.
  uint32_t BitCount = 0;
  /// The bitmap itself, least significant bit of the first byte first.
  std::vector<uint8_t> Bits;

  bool isPointerSlot(uint32_t Slot) const {
    if (Slot >= BitCount || Slot / 8 >= Bits.size())
      return false;
    return ((Bits[Slot / 8] >> (Slot % 8)) & 1) != 0;
  }
};

/// A decoded `runtime.stackmap`: `n` bitmaps of `nbit` bits each, laid out
/// consecutively with each starting on a byte boundary.
struct GoStackMap {
  va_t RecordVA = 0;
  /// `stackmap.nbit`, repeated here because it is a property of the map rather
  /// than of any one bitmap and a map with no bitmaps still declares it.
  uint32_t BitCount = 0;
  std::vector<GoStackMapBitmap> Bitmaps;

  /// Caps on what a record is allowed to claim before it is treated as not
  /// being a `stackmap` at all.  All three are far above what a Go frame
  /// reaches and far below what would let a mis-resolved funcdata pointer turn
  /// into a large allocation; the last is needed because the first two
  /// multiply, and their product is thirty megabytes for one map.
  static constexpr uint32_t MaxBitmaps = 1u << 12;
  static constexpr uint32_t MaxBits = 1u << 16;
  static constexpr uint32_t MaxTotalBytes = 1u << 20;
};

/// One stretch of a function over which `PCDATA_StackMapIndex` selects one
/// bitmap out of both of the function's pointer maps.
struct GoStackMapRange {
  ExceptionAddressRange Range;
  /// Bitmap index the range selects.  Negative where the table leaves the
  /// value unset, which the runtime reads as "this is the prologue" and
  /// resolves by using index 0; that fallback is the runtime's policy rather
  /// than something the table said, so it is not applied here.
  int32_t Index = -1;
};

/// A `runtime.gorecover` call site: the only place a panicking goroutine can
/// be brought back under program control.
struct GoRecoverSite {
  va_t CallVA = 0;
  /// True when the recover call is lexically inside a deferred closure, which
  /// is the only position where `recover()` is defined to do anything.
  bool InDeferredFrame = false;
};

/// A `runtime.gopanic` call site, including the compiler-inserted panics for
/// bounds, nil, and division checks.
struct GoPanicSite {
  va_t CallVA = 0;
  /// Runtime entry point the site calls, e.g. `runtime.gopanic`,
  /// `runtime.goPanicIndex`, `runtime.panicdivide`.
  std::string RuntimeName;
  /// True for a compiler-inserted check rather than a user `panic(...)`.
  bool IsImplicitCheck = false;
};

/// Normalized Go frame metadata for one function.
struct GoFunctionEH {
  va_t EntryVA = 0;
  std::string Name;
  /// `runtime._func.deferreturn`: offset of the `deferreturn` call from the
  /// function entry, or nullopt when the function defers nothing.
  std::optional<uint32_t> DeferReturnOffset;
  /// Raw `runtime._func.flag` bits.
  uint8_t FuncFlags = 0;
  /// Raw `runtime._func.funcID`.  A nonzero ID names a function the runtime
  /// itself treats specially, which is how `runtime.gopanic` and
  /// `runtime.sigpanic` are identified without trusting a symbol name.
  uint8_t FuncID = 0;
  /// Frame size from the function's `pcsp` table at entry, when available.
  std::optional<int32_t> FrameSize;
  bool UsesOpenCodedDefers = false;
  /// Present when `FUNCDATA_OpenCodedDeferInfo` was decoded.
  std::optional<GoOpenCodedDeferInfo> OpenCodedDeferInfo;
  std::vector<GoOpenCodedDefer> OpenCodedDefers;
  std::vector<GoDeferSite> Defers;
  std::vector<GoRecoverSite> Recovers;
  std::vector<GoPanicSite> Panics;
  /// `PCDATA_UnsafePoint` as a partition of the body, safe stretches included,
  /// so that a stretch the table never covered is distinguishable from one it
  /// covered and called safe.  Empty for a function that declares no such
  /// table, and for every function of a pre-Go 1.16 image, where the table did
  /// not exist.
  std::vector<GoUnsafePointRange> UnsafePointRanges;
  /// `FUNCDATA_ArgsPointerMaps`, describing the incoming argument area.
  std::optional<GoStackMap> ArgsPointerMap;
  /// `FUNCDATA_LocalsPointerMaps`, describing the locals area below varp.
  std::optional<GoStackMap> LocalsPointerMap;
  /// `PCDATA_StackMapIndex`: which bitmap of both maps above is live where.
  std::vector<GoStackMapRange> StackMapRanges;

  bool hasExceptionalControlFlow() const {
    return DeferReturnOffset.has_value() || !Defers.empty() ||
           !Recovers.empty() || !Panics.empty();
  }
};

/// Image-wide state recovered from the Go runtime's `pclntab` and the
/// `moduledata` that anchors it.  A Go image needs both: the `pclntab` holds
/// the per-function records, but the offsets inside them are relative to bases
/// that only `moduledata` names.
struct GoModuleInfo {
  /// Version implied by the `pcHeader` magic, e.g. "go1.20".
  std::string PclnTabVersion;
  uint32_t PclnTabMagic = 0;
  va_t PcHeaderVA = 0;
  va_t ModuleDataVA = 0;
  /// Base that `functab.entryoff` and `_func.entryOff` are relative to.
  va_t TextBase = 0;
  /// Base that `_func` funcdata offsets are relative to (`go:func.*`).  Zero
  /// when `moduledata` could not be located, in which case funcdata — and so
  /// open-coded defer info — is unreadable while everything else still is.
  va_t GoFuncBase = 0;
  va_t FuncNameTabVA = 0;
  va_t PcTabVA = 0;
  va_t FuncTabVA = 0;
  uint64_t FunctionCount = 0;
  uint8_t MinLC = 0;
  uint8_t PtrSize = 0;
  /// True when the image splits text across several sections, so an entry
  /// offset must be mapped through `moduledata.textsectmap` rather than simply
  /// added to `TextBase`.
  bool HasMultipleTextSections = false;
  /// True when the `_func` records lack `deferreturn`, `funcID`, and `flag`,
  /// and spell `nfuncdata` as a full word instead of the record's last byte.
  /// Only the Go 1.2 magic can set this: it spans Go 1.2 through Go 1.15 and
  /// the record grew those fields in Go 1.12 without the magic changing, so
  /// the shape has to be inferred from the records rather than read off the
  /// header.  Every later magic implies the fields are present.
  bool UsesPreGo112FuncLayout = false;
  /// Position of `PCDATA_StackMapIndex` in each `_func`'s pcdata array.  Go
  /// 1.13 moved it from 0 to 1 without changing the magic, so on the Go 1.2
  /// layout this records which position the pointer maps proved; nullopt when
  /// nothing proved one, in which case no function carries stack map ranges.
  std::optional<uint32_t> StackMapPCDataIndex;
};

} // namespace neverd

#endif // NEVERD_LOADER_LANGUAGEEH_H
