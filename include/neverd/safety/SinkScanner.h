//===- SinkScanner.h - Locate catalog call sites in lifted MedIR *- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Walks the recovered call sites of every lifted function, matches each callee
/// against the sink catalog, and records the match together with the identity
/// origin of the callee name.  The scanner is format-neutral: a direct call to
/// an ELF PLT stub, a Mach-O dyld stub, or a PE import thunk all reduce to the
/// same catalog name and the same SinkSite record.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SAFETY_SINKSCANNER_H
#define NEVERD_SAFETY_SINKSCANNER_H

#include "neverd/safety/SafetyContext.h"
#include "neverd/safety/SafetyTypes.h"
#include "neverd/safety/SinkCatalog.h"

#include "llvm/ADT/StringRef.h"

#include <string>
#include <vector>

namespace neverd {
struct MedCallInfo;
}

namespace neverd::safety {

/// Return the semantic callee identity used by every safety analysis.  An
/// exact import address or slot takes precedence over the recovered label;
/// name-only import matching is reserved for unresolved indirect calls.
std::string resolveCallName(const AnalysisInput &In, const MedCallInfo &Call);

/// Decide where the name of the routine at \p CalleeAddr came from, honouring
/// the per-format identity precedence: a rename, then an import, then a debug
/// symbol (PDB / DWARF / MAP), then an export or image symbol, then a signature
/// match, and finally a synthesized placeholder.  \p IsIndirect selects the
/// import-slot lookups used for PE IAT / Mach-O dyld indirect calls.
NameSource classifyNameSource(const AnalysisInput &In, va_t CalleeAddr,
                              llvm::StringRef StatedName, bool IsIndirect);

/// Scan every lifted function and return one record per catalog match.
std::vector<SinkSite> scanSinks(const AnalysisInput &In,
                                const SinkCatalog &Cat);

} // namespace neverd::safety

#endif // NEVERD_SAFETY_SINKSCANNER_H
