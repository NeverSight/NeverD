//===- EHFrameDetail.h - Private DWARF frame decoding helpers ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Implementation detail of the DWARF frame decoder in `lib/loader/DWARF`.
/// The declarations here are shared between the translation units that make up
/// that decoder and must not be included from outside this directory.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_LOADER_DWARF_EHFRAMEDETAIL_H
#define NEVERD_LIB_LOADER_DWARF_EHFRAMEDETAIL_H

#include "neverd/loader/DWARF/EHFrame.h"

#include "llvm/Support/Compiler.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace neverd::dwarf_eh {
namespace LLVM_LIBRARY_VISIBILITY_NAMESPACE detail {

/// Decode the call-frame instruction program of one entry.
///
/// Returns false only when the program could not be decoded at all; a
/// truncated tail is reported through \p Status so the instructions that were
/// proven remain available.
bool decodeCFIProgram(const uint8_t *Buf, size_t Size, size_t Cursor,
                      size_t End, const DwarfCIE &CIE, uint8_t FDEEncoding,
                      va_t BufVA, const PointerBases &Bases, unsigned PtrSize,
                      const BinaryImage *Img, const ParseLimits &Limits,
                      std::vector<CFIInstruction> &Out,
                      ExceptionParseStatus &Status,
                      std::vector<std::string> &Diagnostics);

} // namespace LLVM_LIBRARY_VISIBILITY_NAMESPACE detail
} // namespace neverd::dwarf_eh

#endif // NEVERD_LIB_LOADER_DWARF_EHFRAMEDETAIL_H
