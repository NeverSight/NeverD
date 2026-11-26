//===- SafetyJSON.h - Shared finding serialization --------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Serialize one analyzer observation in the legacy finding schema.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_SAFETY_SAFETYJSON_H
#define NEVERD_LIB_SAFETY_SAFETYJSON_H

#include "neverd/safety/SafetyTypes.h"

#include "llvm/Support/JSON.h"

namespace neverd::safety {
llvm::json::Object findingJson(const Finding &F);
}

#endif // NEVERD_LIB_SAFETY_SAFETYJSON_H
