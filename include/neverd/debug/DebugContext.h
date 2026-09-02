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

struct FunctionSym {
  std::string Name;
  va_t Addr = 0;
  uint64_t Size = 0;
  SourceLoc DeclLoc;
  TypeRef ReturnType;
  std::vector<std::pair<std::string, TypeRef>> Params;

  bool contains(va_t Address) const {
    return Address >= Addr && Address - Addr < Size;
  }
};

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
  virtual std::optional<TypeSym> resolveType(uint64_t TypeId) const = 0;
  virtual std::optional<SourceLoc> sourceLocation(va_t Addr) const = 0;

  virtual std::vector<FunctionSym> allFunctions() const = 0;
  virtual std::vector<DataObjectSym> allDataObjects() const { return {}; }

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
