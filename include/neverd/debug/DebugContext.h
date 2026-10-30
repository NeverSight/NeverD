//===- DebugContext.h - Debug information context ------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Defines the DebugContext abstract interface for querying debug
/// information (DWARF, PDB) — function symbols, variable locations,
/// types, and source locations.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_DEBUG_DEBUGCONTEXT_H
#define NEVERD_DEBUG_DEBUGCONTEXT_H

#include "neverd/Common.h"
#include "neverd/ir/SourceTypeHint.h"
#include "neverd/ir/high/HighIR.h"
#include "neverd/support/LoadProgress.h"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace neverd {

struct SourceLoc {
  std::string File;
  uint32_t Line = 0;
  uint32_t Col = 0;
};

/// Source calling convention recovered from authenticated debug information.
/// This is independent of MedIR::CallingConv, which remains an ABI-recovery
/// result and must not be guessed from a pretty-print attribute.
enum class DebugCallConv : uint8_t {
  Unknown,
  Cdecl,
  Stdcall,
  Thiscall,
  Fastcall,
};

struct FunctionSym {
  std::string Name;
  va_t Addr = 0;
  uint64_t Size = 0;
  SourceLoc DeclLoc;
  TypeRef ReturnType;
  std::vector<std::pair<std::string, TypeRef>> Params;
  /// Params names only the fixed prefix of a variadic declaration.
  bool IsVariadic = false;
  DebugCallConv CallConv = DebugCallConv::Unknown;
  /// The function follows the platform's C calling convention, as C, C++,
  /// Objective-C and Rust code does.  False for a language with conventions
  /// of its own, such as Go's register ABI, or one the debug information
  /// does not name: no C placement rules locate its parameters.
  bool PlatformConvention = true;
  /// Where each parameter enters the function, for one the debug information
  /// places in a register at the entry where the platform's convention would
  /// not: an i386 function GCC passes EAX, EDX and ECX (regparm), or clang
  /// ECX and EDX (fastcc).  Parallel to Params: an IntegerRegister location
  /// names its register, a Stack one says only that the value is in the
  /// caller's slots, and None that the information does not say.  Empty for
  /// every other function.
  std::vector<SourceABIValueLocation> ParamEntryLocations;
  /// Where a known routine's return and parameters come from when neither
  /// the debug information nor the user gives them: the type library whose
  /// prototype of the function's name gives them (windows-kernel-x64), and
  /// that prototype as the library declares it, in C.  Empty otherwise.
  std::string PrototypeLibrary;
  std::string PrototypeDeclaration;
  /// The same declaration with portable scalar spellings, retaining source
  /// qualifiers for C output that does not include the target platform SDK.
  /// Its outer cdecl/stdcall is carried by CallConv, not the spelling.
  std::string PortablePrototypeDeclaration;
  /// Parallel source cast types from that same declaration graph. These
  /// preserve nested callback pointees/qualifiers that machine types omit;
  /// they carry no additional argument-placement or register-state evidence.
  std::vector<std::string> PortableParameterTypes;
  /// The prototype comes from a selected platform header library, including
  /// Windows SDK/WDK headers that are rendered without a host header include.
  bool PrototypeFromHeader = false;
  /// The library is a C library's headers (glibc's, bionic's, musl's,
  /// libSystem's), which a unit's C includes: a definition of the name in
  /// the image is a copy of the routine they declare, not that routine.
  bool PrototypeFromCLibrary = false;
  /// The function never returns, as its declaration says (`_Noreturn`).
  bool NoReturn = false;

  bool contains(va_t Address) const {
    return Address >= Addr && Address - Addr < Size;
  }
};

/// TPI LF_PROCEDURE / LF_MFUNCTION argument types own display arity.
/// Extra S_LOCAL IsParameter names (RAII lock homes, this-relative REGREL)
/// are locals, not call arguments.
inline bool debugParamTypeFits(const TypeRef &Have, const TypeRef &Want) {
  if (!Want || !Have)
    return true;
  return Have->Kind == Want->Kind;
}

inline std::vector<std::pair<std::string, TypeRef>>
bindDebugParamsToTpi(std::vector<std::pair<std::string, TypeRef>> Locals,
                     const std::vector<TypeRef> &TpiParams) {
  if (TpiParams.empty())
    return Locals;
  std::vector<std::pair<std::string, TypeRef>> Kept;
  size_t I = 0;
  if (!Locals.empty() && Locals[0].first == "this") {
    Kept.push_back(Locals[0]);
    I = 1;
  }
  for (const TypeRef &Want : TpiParams) {
    while (I < Locals.size() && !debugParamTypeFits(Locals[I].second, Want))
      ++I;
    if (I < Locals.size()) {
      auto Named = Locals[I++];
      if (!Named.second)
        Named.second = Want;
      Kept.push_back(std::move(Named));
    } else
      Kept.emplace_back(std::string{}, Want);
  }
  return Kept;
}

/// Authenticated, per-function return contract.  This is intentionally richer
/// than a has-value bit: heap ownership can be carried only by a pointer-
/// compatible integer ABI result, while floating-point and aggregate results
/// use different carriers and must never inherit a stale integer return value.
enum class AuthenticatedReturnKind : uint8_t {
  Unknown,
  NoValue,
  Pointer,
  Integer,
  FloatingPoint,
  Aggregate,
};

struct AuthenticatedReturnValueState {
  AuthenticatedReturnKind Kind = AuthenticatedReturnKind::Unknown;
  uint16_t Size = 0;

  bool operator==(const AuthenticatedReturnValueState &) const = default;
};

struct VariableSym {
  std::string Name;
  TypeRef Type;
  int64_t StackOffset = 0;
  bool IsParam = false;
  /// Provider type-graph index (PDB TPI TypeIndex). 0 means none.
  /// `resolveVariable` materializes Type from this; do not treat it as ABI.
  uint32_t TypeId = 0;
};

/// A fixed-address, typed data object recovered from authenticated debug
/// information.  IsBuffer is true only for an array object whose full byte
/// extent is known; non-buffer objects are retained so an equal/overlapping
/// address cannot silently borrow array capacity.
struct DataObjectSym {
  std::string Name;
  va_t Addr = InvalidVA;
  uint64_t Size = 0;
  bool IsBuffer = false;
};

enum class VariableExtentLookupStatus : uint8_t {
  NotFound,
  Unique,
  Ambiguous,
};

/// Result of an occurrence-sensitive local-object lookup.  Ambiguous is an
/// authoritative negative result: consumers must not turn it into an exact
/// extent by falling through to another coordinate system or weaker source.
struct VariableExtentLookup {
  VariableExtentLookupStatus Status = VariableExtentLookupStatus::NotFound;
  VariableSym Variable;

  static VariableExtentLookup notFound() { return {}; }
  static VariableExtentLookup unique(VariableSym V) {
    VariableExtentLookup R;
    R.Status = VariableExtentLookupStatus::Unique;
    R.Variable = std::move(V);
    return R;
  }
  static VariableExtentLookup ambiguous() {
    VariableExtentLookup R;
    R.Status = VariableExtentLookupStatus::Ambiguous;
    return R;
  }
};

struct TypeSym {
  std::string Name;
  TypeRef Type;
};

/// A pointer-to-record parameter located in a physical register at the exact
/// function entry. This is identity evidence only, never a callable ABI or a
/// license to access/replace the record. Providers must authenticate the image,
/// complete qualified record identity, byte size and concrete entry location.
struct AuthenticatedRecordParameter {
  uint64_t Register = 0;
  std::string QualifiedType;
  uint32_t ObjectBytes = 0;
};

class DebugContext {
public:
  virtual ~DebugContext() = default;

  virtual std::optional<FunctionSym> resolveFunction(va_t Addr) const = 0;
  /// The prototype of the routine named \p Name, by its C name, as the
  /// program's types declare it: a header library's for a routine another
  /// module exports, as the Windows kernel's headers declare what drivers
  /// import.  None where none declares one the decompiler can describe.
  virtual std::optional<FunctionSym>
  resolvePrototype(llvm::StringRef Name) const {
    (void)Name;
    return std::nullopt;
  }
  /// Public name only.  PDB `--func` must not walk the owning module's
  /// S_LOCAL stream just to replace `sub_<va>`.
  virtual std::optional<std::string> functionName(va_t Addr) const {
    if (auto FS = resolveFunction(Addr); FS && !FS->Name.empty())
      return FS->Name;
    return std::nullopt;
  }
  virtual std::optional<VariableSym> resolveVariable(va_t FuncAddr,
                                                     int64_t Offset) const = 0;
  /// True only when function declarations (including return kinds) are tied
  /// to the exact loaded image and were decoded by a validated type reader.
  /// Names-only PDB/MAP providers and unauthenticated split companions remain
  /// useful identity sources but must not answer semantic return questions.
  virtual bool hasAuthenticatedFunctionSignatures() const { return false; }
  /// Return a declaration-quality contract for exactly one function entry.
  /// Providers must return Unknown for malformed type references, ambiguous
  /// subprogram identities, or declarations not authenticated to the loaded
  /// image.  In particular, absence is NoValue only when the source format
  /// explicitly defines a missing return-type attribute as void.
  virtual AuthenticatedReturnValueState
  resolveAuthenticatedReturnValueState(va_t) const {
    return {};
  }
  /// True only when local/global object boundaries are tied to the exact
  /// loaded image.  PDB, MAP, mismatched companions, and legacy providers are
  /// untrusted by default even when they remain useful for names and lines.
  virtual bool hasAuthenticatedObjectExtents() const { return false; }

  virtual std::vector<AuthenticatedRecordParameter>
  resolveAuthenticatedRecordParameters(va_t) const {
    return {};
  }

  /// An authenticated pointer-to-record return declaration. Its register is
  /// the ABI result carrier, not an inferred receiver or parameter location.
  virtual std::optional<AuthenticatedRecordParameter>
  resolveAuthenticatedRecordReturn(va_t) const {
    return std::nullopt;
  }

  /// Resolve the unique object containing \p Offset at one concrete machine
  /// instruction.  The returned variable retains its declared base in
  /// VariableSym::StackOffset so consumers can compute an interior pointer's
  /// remaining extent.  This legacy adapter is reachable only after a
  /// provider explicitly opts into authenticated object extents.
  virtual VariableExtentLookup resolveVariableAt(va_t FuncAddr, va_t UsePC,
                                                 int64_t Offset) const {
    (void)UsePC;
    std::optional<VariableSym> V = resolveVariable(FuncAddr, Offset);
    return V ? VariableExtentLookup::unique(std::move(*V))
             : VariableExtentLookup::notFound();
  }
  /// Resolve a variable described relative to the function's adjusted stack
  /// pointer. Most consumers use CFA-relative offsets through resolveVariable;
  /// this separate query keeps the two coordinate systems unambiguous.
  virtual std::optional<VariableSym>
  resolveStackPointerVariable(va_t, int64_t) const {
    return std::nullopt;
  }
  virtual VariableExtentLookup
  resolveStackPointerVariableAt(va_t FuncAddr, va_t UsePC,
                                int64_t Offset) const {
    (void)UsePC;
    std::optional<VariableSym> V =
        resolveStackPointerVariable(FuncAddr, Offset);
    return V ? VariableExtentLookup::unique(std::move(*V))
             : VariableExtentLookup::notFound();
  }
  /// Resolve a variable relative to the architecture's hardware frame-pointer
  /// register.  Providers must expose DW_OP_fbreg here only after proving the
  /// subprogram's frame-base expression names that register at UsePC.
  virtual VariableExtentLookup resolveFramePointerVariableAt(va_t, va_t,
                                                             int64_t) const {
    return VariableExtentLookup::notFound();
  }
  /// Attach display fields on a named TPI/DWARF record. Pointers peel to
  /// the pointee. No-op when the provider already filled the record or the
  /// type is not a named struct/enum. Callers that print `this->m` must
  /// invoke this before walking FieldDisplayNames; PDB pointer fields stay
  /// name-only until the pointee is completed.
  virtual void completeType(const TypeRef &) const {}
  virtual std::optional<TypeSym> resolveType(uint64_t TypeId) const = 0;
  virtual std::optional<SourceLoc> sourceLocation(va_t Addr) const = 0;

  virtual std::vector<FunctionSym> allFunctions() const = 0;
  virtual std::vector<DataObjectSym> allDataObjects() const { return {}; }
  /// Name a data public at exactly \p Addr.  The default walks
  /// allDataObjects(); providers that keep an uningested publics index
  /// override this so `--func` does not have to materialize every S_PUB32.
  virtual std::optional<DataObjectSym> resolveDataObject(va_t Addr) const {
    for (const DataObjectSym &Object : allDataObjects()) {
      if (Object.Addr == Addr && !Object.Name.empty())
        return Object;
    }
    return std::nullopt;
  }
  /// Where the data symbols nearest \p Addr begin: the last at or before it
  /// and the first after it.  A symbol begins an object even when unsized,
  /// so the two bound one the image does not size.  The default walks
  /// allDataObjects(); providers with an address-ordered index override it.
  virtual std::pair<std::optional<va_t>, std::optional<va_t>>
  dataSymbolsAround(va_t Addr) const {
    std::optional<va_t> Before, After;
    for (const DataObjectSym &Object : allDataObjects()) {
      if (Object.Addr <= Addr && (!Before || Object.Addr > *Before))
        Before = Object.Addr;
      else if (Object.Addr > Addr && (!After || Object.Addr < *After))
        After = Object.Addr;
    }
    return {Before, After};
  }
  /// Where the data symbol that \p Addr follows begins: the last symbol of
  /// \p Addr's section at or before it, when that symbol names data and no
  /// function.  No named function begins between the two, so the address
  /// lies in data even where nothing sizes the object, as an unnamed table
  /// after a named string does.  Providers with an address-ordered index
  /// answer it; the default knows none.
  virtual std::optional<va_t> precedingDataSymbol(va_t Addr) const {
    (void)Addr;
    return std::nullopt;
  }

  virtual bool hasInfo() const = 0;
};

class NullDebugContext : public DebugContext {
public:
  std::optional<FunctionSym> resolveFunction(va_t) const override {
    return std::nullopt;
  }
  std::optional<VariableSym> resolveVariable(va_t, int64_t) const override {
    return std::nullopt;
  }
  std::optional<TypeSym> resolveType(uint64_t) const override {
    return std::nullopt;
  }
  std::optional<SourceLoc> sourceLocation(va_t) const override {
    return std::nullopt;
  }
  std::vector<FunctionSym> allFunctions() const override { return {}; }
  bool hasInfo() const override { return false; }
};

} // namespace neverd

#endif // NEVERD_DEBUG_DEBUGCONTEXT_H
