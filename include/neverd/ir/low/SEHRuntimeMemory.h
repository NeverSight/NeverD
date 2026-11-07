//===- SEHRuntimeMemory.h - Windows CRT frame effects -----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Retains authenticated Windows CRT bindings and source memory evidence.

#ifndef NEVERD_IR_LOW_SEHRUNTIMEMEMORY_H
#define NEVERD_IR_LOW_SEHRUNTIMEMEMORY_H

#include "neverd/ir/Limits.h"
#include "neverd/loader/ExceptionCommon.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"

#include <cstddef>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace neverd {
struct BinaryImage;
struct LowFunc;

/// Check the bounded scalar Windows printf grammar, without a terminating NUL.
/// Source argument type hints do not supply this runtime-memory contract.
bool isWindowsSEHScalarFormat(llvm::StringRef Format);

/// An exact PE import binding, supplied by the image owner. Neither a display
/// name nor an ABI declaration supplies this identity. Call arguments and the
/// invocation's live return slots remain obligations of the frame analysis.
struct LowSEHRuntimeCall {
  enum class Kind : uint8_t { CRTStream, CRTVfprintf, CRTControlFP } Routine;
  va_t IATAddress = InvalidVA;
  std::string Module;
  std::string Name;
  bool operator==(const LowSEHRuntimeCall &) const = default;
};

/// The file-backed writable ranges of an x64 COFF image's data that every
/// catalog of the image holds, and the steps the catalog's scan of them
/// takes; not Complete where the scan passes the catalog's allowance or
/// range limit.
struct LowSEHWritableImageRanges {
  const BinaryImage *Image = nullptr;
  std::vector<ExceptionAddressRange> Ranges;
  size_t Steps = 0;
  bool Complete = false;
};

class LowSEHRuntimeMemory {
public:
  const auto &calls() const { return Calls; }
  const auto &scalarFormats() const { return ScalarFormats; }
  const auto &writableImageRanges() const { return WritableImageRanges; }

private:
  friend LowSEHRuntimeMemory
  collectLowSEHRuntimeMemory(const BinaryImage &, llvm::ArrayRef<LowFunc>,
                             const LowSEHWritableImageRanges *);
  /// The Boolean distinguishes a direct executable veneer from an IAT cell.
  std::map<std::pair<va_t, bool>, LowSEHRuntimeCall> Calls;
  /// Complete immutable narrow formats, including their terminating NUL.
  /// Their checked conversions take only scalars and cannot write through a
  /// variadic pointer. Dynamic formats and count-output conversions refuse.
  std::map<va_t, std::string> ScalarFormats;
  /// Exact file-backed image storage, disjoint from the physical stack.
  std::vector<ExceptionAddressRange> WritableImageRanges;
};

/// Binds the immutable import, format and image-storage facts consulted by a
/// frame proof. An oversized or malformed catalog produces no digest.
std::string lowSEHRuntimeMemoryDigest(const LowSEHRuntimeMemory &Memory);

} // namespace neverd

#endif
