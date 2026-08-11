//===- LSDA.h - Itanium language-specific data area decoding --*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Declares the bounded decoder for the Itanium C++ ABI language-specific
/// data area, emitted into `.gcc_except_table` / `__gcc_except_tab`.  The LSDA
/// is what turns DWARF unwind information into exception *handling*: it names
/// the protected regions, their landing pads, and the action chain that
/// decides whether a landing pad may stop the exception.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LOADER_DWARF_LSDA_H
#define NEVERD_LOADER_DWARF_LSDA_H

#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/DWARF/EHFrame.h"
#include "neverd/loader/ExceptionInfo.h"

#include <optional>
#include <string>
#include <vector>

namespace neverd::dwarf_eh {

struct LSDAParseRequest {
  /// Address of the LSDA record, as named by the FDE augmentation data.
  va_t LSDAVA = 0;
  /// Start of the function the record describes.  Used as the default landing
  /// pad base and to validate that decoded ranges stay inside the function.
  va_t FunctionStart = 0;
  /// End of that function, when the FDE proved one.  Zero disables the
  /// containment check rather than inventing a bound.
  va_t FunctionEnd = 0;
  /// True for the setjmp/longjmp call-site form, whose table holds call-site
  /// indices instead of addresses.
  bool IsSJLJ = false;
  /// Upper bound on call sites, actions, and type-table entries.
  size_t MaxRecords = 1u << 16;
};

struct LSDAParseResult {
  std::optional<ItaniumEHInfo> Info;
  ExceptionParseStatus ParseStatus = ExceptionParseStatus::Complete;
  std::vector<std::string> Diagnostics;
};

/// Decode one LSDA record from the loaded image.
LSDAParseResult parseLSDA(const BinaryImage &Img, const LSDAParseRequest &Req,
                          const PointerBases &Bases);

/// Follow an Itanium `std::type_info *` to its mangled name.
///
/// The ABI fixes the layout: a vtable pointer followed by a `const char *`
/// naming the type.  Returns an empty string when the chain could not be
/// followed inside mapped, readable data, because a guessed name would be
/// indistinguishable from a proven one to every consumer downstream.
std::string readItaniumTypeName(const BinaryImage &Img, va_t TypeInfoVA);

} // namespace neverd::dwarf_eh

#endif // NEVERD_LOADER_DWARF_LSDA_H
