//===- EmbeddedObjects.h - Session embedded-file evidence -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Shares the loader-owned embedded ELF inventory between public references
/// and the object catalogue, without creating nested analysis sessions.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_SDK_CAPI_EMBEDDEDOBJECTS_H
#define NEVERD_LIB_SDK_CAPI_EMBEDDEDOBJECTS_H

#include "neverd/loader/ELF/EmbeddedELF.h"

#include "llvm/Support/Error.h"

namespace neverd::sdk {
struct Session;

llvm::Expected<llvm::ArrayRef<EmbeddedELFObject>> embeddedObjects(Session &S);
std::string embeddedReferenceText(const EmbeddedELFObject &Object,
                                  const EmbeddedELFReference &Reference);
} // namespace neverd::sdk

#endif // NEVERD_LIB_SDK_CAPI_EMBEDDEDOBJECTS_H
