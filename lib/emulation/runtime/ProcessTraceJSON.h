//===- ProcessTraceJSON.h - Streaming guest trace fields -------*- C++ -*-===//
//
// NeverD Decompiler
/// \file Write the hexadecimal register arrays and native calls shared by
/// process report profiles.
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_RUNTIME_PROCESSTRACEJSON_H
#define NEVERD_EMULATION_RUNTIME_PROCESSTRACEJSON_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"

#include <cstdint>

namespace llvm::json {
class OStream;
}

namespace neverd::emulation {
struct ProcessResult;
void writeHexArray(llvm::json::OStream &J, llvm::StringRef Key,
                   llvm::ArrayRef<uint64_t> Values);
void writeNativeCalls(llvm::json::OStream &J, const ProcessResult &Result);
} // namespace neverd::emulation
#endif // NEVERD_EMULATION_RUNTIME_PROCESSTRACEJSON_H
