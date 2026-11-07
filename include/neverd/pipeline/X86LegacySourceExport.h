//===- X86LegacySourceExport.h - Protected source selection ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Export an explicit original-byte protected-mode dispatcher and manifest.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_PIPELINE_X86LEGACYSOURCEEXPORT_H
#define NEVERD_PIPELINE_X86LEGACYSOURCEEXPORT_H

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"

#include <string>

namespace neverd {
struct BinaryImage;

struct X86LegacySourceExport {
  std::string Source;
  std::string Manifest;
};

/// Requires profile protected-legacy-v1 and a nonempty instructions array.
/// Each entry declares address (linear uint32), code_bits (16/32) and size
/// (1..15); the shared execution grammar authenticates its complete original
/// bytes. Selection is mode-sensitive and independent of the live CS base.
/// Generated C calls the matching X86LegacySourceExecution bridge. It supplies
/// no ordinary ABI, runtime profiles, CPU capture or architectural state bank.
/// The instruction budget borrows separate host storage, disjoint from the
/// context/state/RAM and not modified by a device or debug callback.
/// All selected completed transfers, including CALL/RET/far/system returns,
/// can continue; every delivered event and partial event entry stops. The host
/// inspects/repairs/resumes the retained runtime, then invokes the driver
/// again. This is one runtime-backed C route, not HighC/LLVMC instruction
/// projection.
llvm::Expected<X86LegacySourceExport>
exportX86LegacySource(const BinaryImage &Image, llvm::StringRef RequestJSON);

} // namespace neverd

#endif // NEVERD_PIPELINE_X86LEGACYSOURCEEXPORT_H
