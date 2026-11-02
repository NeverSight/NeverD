//===- ExportTable.h - CLI export table formatting --------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Human-readable formatting of a complete SDK export-table response.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TOOLS_NEVERD_EXPORTTABLE_H
#define NEVERD_TOOLS_NEVERD_EXPORTTABLE_H

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"

namespace llvm {
class raw_ostream;
}

namespace neverd::cli {

/// Validate the SDK response before printing any rows. There is no transport
/// frame-size limit on this in-process, complete export table.
llvm::Error printExportTable(llvm::StringRef Text, llvm::raw_ostream &Output);

} // namespace neverd::cli

#endif // NEVERD_TOOLS_NEVERD_EXPORTTABLE_H
