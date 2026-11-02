//===- PortableX87Function.h - Prepared portable function ABI --*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Immutable prepared functions for explicit portable x87 generated execution.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_MED_X86_PORTABLEX87FUNCTION_H
#define NEVERD_IR_MED_X86_PORTABLEX87FUNCTION_H

#include "neverd/ir/med/MedIR.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace neverd {

class MedLLVMEmitter;
class HighCEmitter;

/// Immutable prepared input for the explicit portable emission entry points.
/// The source machine ABI remains distinct: source parameters retain their
/// indices as integer guest values, followed by an opaque execution pointer
/// and a uint64_t source-return pointer. The function itself returns status.
/// No public mutable MedIR or flag can manufacture a prepared contract.
class PreparedPortableX87Function final {
public:
  llvm::StringRef name() const { return Function.Name; }
  llvm::ArrayRef<uint16_t> sourceParameterBytes() const {
    return ParameterBytes;
  }
  unsigned executionParameterIndex() const { return ParameterBytes.size(); }
  unsigned sourceReturnParameterIndex() const {
    return ParameterBytes.size() + 1;
  }
  BinaryFormat sourceFormat() const { return SourceFormat; }
  va_t sourceEntry() const { return SourceEntry; }

  /// Revalidate the private body, exact ABI roles and helper success edges
  /// immediately before emission. It never relies on a capability boolean.
  llvm::Error validate() const;

private:
  friend class MedLLVMEmitter;
  friend class HighCEmitter;
  friend llvm::Expected<PreparedPortableX87Function>
  preparePortableX87Function(const MedFunc &, Arch, BinaryFormat);

  PreparedPortableX87Function(MedFunc Function,
                              std::vector<uint16_t> ParameterBytes,
                              BinaryFormat Format, va_t SourceEntry,
                              MedVar Execution, MedVar SourceReturn)
      : Function(std::move(Function)),
        ParameterBytes(std::move(ParameterBytes)), SourceFormat(Format),
        SourceEntry(SourceEntry), Execution(Execution),
        SourceReturn(SourceReturn) {}

  MedFunc Function;
  std::vector<uint16_t> ParameterBytes;
  BinaryFormat SourceFormat;
  va_t SourceEntry;
  MedVar Execution;
  MedVar SourceReturn;
};

/// Copy an ABI-recovered leaf function and build the alternate execution ABI.
/// The whole function must belong to the admitted x64 owned-context slice.
/// Calls, EH, external synchronization, MMX, typed floating ABI and ordinary
/// guest-memory operations are refused before any output can be published.
llvm::Expected<PreparedPortableX87Function>
preparePortableX87Function(const MedFunc &Source, Arch SourceArch,
                           BinaryFormat SourceFormat);

} // namespace neverd

#endif // NEVERD_IR_MED_X86_PORTABLEX87FUNCTION_H
