//===- X86LegacyDescriptorAddress.h - IA-32e descriptor operands -*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Check descriptor canonicality before an implicit table read. A canonical
/// start with a noncanonical tail uses the bound processor's error code.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_TRANSLATE_X86_X86LEGACYDESCRIPTORADDRESS_H
#define NEVERD_LIB_TRANSLATE_X86_X86LEGACYDESCRIPTORADDRESS_H

#include "neverd/translate/X86/X86LegacySegmentExecution.h"

#include "llvm/Support/Errc.h"

namespace neverd::translate::detail {

inline llvm::Expected<std::optional<X86MemoryException>>
checkLegacyDescriptorAddress(
    const X86PagingState &C, const X86LinearAccess &A, unsigned Size,
    uint16_t Selector,
    std::optional<X86DescriptorCanonicalityProfile> Profile) {
  auto Extent = checkX86LinearAccess(C, A, Size);
  if (!Extent || !*Extent)
    return Extent;
  auto Start = checkX86LinearAccess(C, A, 1);
  if (!Start)
    return Start.takeError();
  if (!*Start && !Profile)
    return llvm::createStringError(
        llvm::errc::invalid_argument,
        "a noncanonical descriptor tail requires its processor profile");
  if (*Start || Profile == X86DescriptorCanonicalityProfile::SelectorForRange)
    (*Extent)->ErrorCode = Selector & 0xfffc;
  return Extent;
}

} // namespace neverd::translate::detail

#endif // NEVERD_LIB_TRANSLATE_X86_X86LEGACYDESCRIPTORADDRESS_H
