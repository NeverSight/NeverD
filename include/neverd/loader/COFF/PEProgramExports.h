//===- PEProgramExports.h - Original PE export identities -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_LOADER_COFF_PEPROGRAMEXPORTS_H
#define NEVERD_LOADER_COFF_PEPROGRAMEXPORTS_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/Error.h"

#include <cstdint>
#include <string>
#include <vector>

namespace neverd {
namespace pe_export_defaults {
#define NEVERD_PE_EXPORT_LIMIT(Name, Value)                                    \
  inline constexpr uint64_t Name = Value;
#include "neverd/loader/COFF/PEProgramExports.def"
#undef NEVERD_PE_EXPORT_LIMIT
} // namespace pe_export_defaults
struct PEExportLimits {
  /// Repeated reads consume work again, including shared strings and tables.
  uint64_t Bytes = pe_export_defaults::Bytes;
  uint64_t Records = pe_export_defaults::Records;
  uint64_t StringBytes = pe_export_defaults::StringBytes;
};
enum class PEExportKind { Address, Forwarder, Hole };
struct PEProgramExport {
  uint32_t Ordinal;
  uint32_t RVA;
  PEExportKind Kind;
  /// Multiple distinct names may identify one address-table ordinal.
  std::vector<std::string> Names;
  std::string Forwarder;
};
struct PEMetadataRange {
  uint64_t RVA, Size;
};
struct PEProgramExports {
  std::string Module;
  /// Complete address-table order, including unnamed exports and zero holes.
  std::vector<PEProgramExport> Entries;
  /// File-backed metadata reads, coalesced in encounter order.
  std::vector<PEMetadataRange> Metadata;
  uint64_t BytesRead = 0, RecordsRead = 0;
};

/// Decode bounded PE32/PE32+ exports from original file bytes. Never consult
/// analysis-patched segments or infer a callable address for an ordinal hole.
/// Direct RVAs lie within SizeOfImage; mapping permissions and forwarder
/// resolution remain the guest loader's responsibility. Malformed metadata,
/// ambiguous file mappings and excessive work return Error, never partial data.
llvm::Expected<PEProgramExports>
readPEProgramExports(llvm::ArrayRef<uint8_t> File,
                     const PEExportLimits &Limits = {});
} // namespace neverd
#endif
