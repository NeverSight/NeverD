//===- ProcessReport.h - Process options and result JSON -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_PROCESSREPORT_H
#define NEVERD_EMULATION_PROCESSREPORT_H
#include "neverd/emulation/ProcessSession.h"

namespace llvm {
class raw_ostream;
}

namespace neverd::emulation {
llvm::Expected<ProcessOptions> processOptionsFromJSON(llvm::StringRef Text);
/// Write compact JSON with object keys in lexical order.
/// Binary streams use lowercase hexadecimal. Addresses, service numbers,
/// argument registers and raw results use hexadecimal strings to preserve all
/// 64 bits even in consumers whose JSON number type is a double.
void writeProcessResultJSON(llvm::raw_ostream &OS, const ProcessResult &Result);
/// Return the same compact JSON for callers that need an owning string.
std::string processResultJSON(const ProcessResult &Result);
} // namespace neverd::emulation
#endif
