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
#include "neverd/ir/high/HighIR.h"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace neverd {

/// Optional load-time progress.  Image parse and PDB publics must not look idle.
struct LoadProgress {
  using Fn = void (*)(void *User, const char *Phase, unsigned long long Done,
                      unsigned long long Total, const char *Detail);
  Fn Callback = nullptr;
  void *User = nullptr;
  void report(const char *Phase, unsigned long long Done,
              unsigned long long Total, const char *Detail = "") const {
    if (Callback)
      Callback(User, Phase, Done, Total, Detail ? Detail : "");
  }
};

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
  DebugCallConv CallConv = DebugCallConv::Unknown;

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

class DebugContext {
public:
  virtual ~DebugContext() = default;

  virtual std::optional<FunctionSym> resolveFunction(va_t Addr) const = 0;
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
