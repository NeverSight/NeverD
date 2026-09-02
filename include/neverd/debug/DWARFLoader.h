//===- DWARFLoader.h - DWARF debug info loader --------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Declares DWARFDebugContext which loads DWARF debug information from
/// ELF and Mach-O binaries via LLVM's DWARFContext.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_DEBUG_DWARFLOADER_H
#define NEVERD_DEBUG_DWARFLOADER_H

#include "neverd/debug/DebugContext.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace neverd {

enum class DWARFLoadTrust {
  InImage,
  UnauthenticatedCompanion,
};

namespace dwarf_loader_detail {

using SubprogramRange = std::pair<va_t, va_t>;

/// Closed set of location-expression shapes that the exact-object resolver
/// understands.  Malformed also covers otherwise valid DWARF expressions
/// whose complete semantics are outside this resolver's deliberately narrow
/// stack-coordinate model.
enum class LocationExpressionBase {
  CFA,
  StackPointer,
  FramePointer,
  FrameBase,
  NonStack,
  Malformed,
};

struct DecodedLocationExpression {
  LocationExpressionBase Base = LocationExpressionBase::Malformed;
  int64_t Offset = 0;
};

/// Pure merge policy used while following DW_AT_specification and
/// DW_AT_abstract_origin.  Unspecified means the current DIE contributed no
/// declaration; NoValue is a followed declaration that omits DW_AT_type.
enum class ReturnTypeProvenanceStatus : uint8_t {
  Unspecified,
  NoValue,
  Value,
  Malformed,
};

ReturnTypeProvenanceStatus
mergeReturnTypeProvenance(ReturnTypeProvenanceStatus Current,
                          ReturnTypeProvenanceStatus Candidate,
                          bool SameConcreteType = true);

DecodedLocationExpression decodeLocationExpressionShape(
    uint8_t Opcode, std::span<const uint64_t> Operands, bool Complete,
    bool IsFrameBase, std::optional<unsigned> StackPointerRegister,
    std::optional<unsigned> FramePointerRegister);

/// Order-independent ambiguity index for subprogram DIE extents.  Duplicate
/// or overlapping DIEs may still contribute names and lines, but can never let
/// whichever compile unit happened to be visited last publish object bounds.
class SubprogramExtentRegistry {
public:
  void insert(va_t Entry, std::span<const SubprogramRange> Ranges);
  bool isAmbiguous(va_t Entry) const;

private:
  struct Record {
    va_t Entry = InvalidVA;
    std::vector<SubprogramRange> Ranges;
    bool Ambiguous = false;
  };
  std::vector<Record> Records;
};

} // namespace dwarf_loader_detail

class DWARFDebugContext : public DebugContext {
public:
  ~DWARFDebugContext() override;

  static std::unique_ptr<DWARFDebugContext>
  load(const std::filesystem::path &BinaryPath, BinaryFormat Fmt,
       DWARFLoadTrust Trust, std::span<const uint8_t> ExpectedImageBytes = {});

  std::optional<FunctionSym> resolveFunction(va_t Addr) const override;
  std::optional<VariableSym> resolveVariable(va_t FuncAddr,
                                             int64_t Offset) const override;
  VariableExtentLookup resolveVariableAt(va_t FuncAddr, va_t UsePC,
                                         int64_t Offset) const override;
  std::optional<VariableSym>
  resolveStackPointerVariable(va_t FuncAddr, int64_t Offset) const override;
  VariableExtentLookup
  resolveStackPointerVariableAt(va_t FuncAddr, va_t UsePC,
                                int64_t Offset) const override;
  VariableExtentLookup
  resolveFramePointerVariableAt(va_t FuncAddr, va_t UsePC,
                                int64_t Offset) const override;
  std::optional<TypeSym> resolveType(uint64_t TypeId) const override;
  std::optional<SourceLoc> sourceLocation(va_t Addr) const override;
  std::vector<FunctionSym> allFunctions() const override;
  std::vector<DataObjectSym> allDataObjects() const override;
  bool hasAuthenticatedFunctionSignatures() const override;
  AuthenticatedReturnValueState
  resolveAuthenticatedReturnValueState(va_t Entry) const override;
  bool hasAuthenticatedObjectExtents() const override;
  bool hasInfo() const override;

private:
  DWARFDebugContext() = default;

  struct Impl;
  std::unique_ptr<Impl> PImpl;
};

} // namespace neverd

#endif // NEVERD_DEBUG_DWARFLOADER_H
