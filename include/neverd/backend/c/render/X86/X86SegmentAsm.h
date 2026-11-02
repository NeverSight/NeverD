//===- X86SegmentAsm.h - C text for x86 segment selector moves --*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// HighC and LLVMC print a segment selector move the same way: C has no
// spelling for a segment register, so both use a GNU asm statement.
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_C_RENDER_X86_X86SEGMENTASM_H
#define NEVERD_BACKEND_C_RENDER_X86_X86SEGMENTASM_H

#include "llvm/ADT/StringRef.h"

#include <string>

namespace neverd {

/// A statement expression whose value is segment register \p Name (AT&T,
/// without the `%` sigil) as a `uint16_t`.
inline std::string x86SegmentReadText(llvm::StringRef Name) {
  return "({ uint16_t neverd_selector; __asm__ volatile(\"mov %%" + Name.str() +
         ", %0\" : \"=r\"(neverd_selector)); neverd_selector; })";
}

/// An asm statement loading the low 16 bits of \p Value into segment
/// register \p Name.
inline std::string x86SegmentWriteText(llvm::StringRef Name,
                                       llvm::StringRef Value) {
  return "__asm__ volatile(\"mov %w0, %%" + Name.str() +
         "\" : : \"r\"((uint32_t)(" + Value.str() + ")) : \"memory\")";
}

} // namespace neverd

#endif // NEVERD_BACKEND_C_RENDER_X86_X86SEGMENTASM_H
